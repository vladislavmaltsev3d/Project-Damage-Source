// строим отложения, наплывы и заполнение углов
// ProjectDamage surface deposits — GPL-3.0-or-later; see LICENSE.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#include "manifold/manifold.h"
#include "project_damage_deposits.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {
double Random01(uint64_t seed, uint64_t candidate, uint64_t channel)
{
    uint64_t z = seed ^ (candidate * 0x9e3779b97f4a7c15ULL)
        ^ ((channel + 1) * 0xd1b54a32d192ed03ULL);
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    return double(z >> 11) * (1.0 / 9007199254740992.0);
}

m::vec3 Unit(const m::vec3& v, const m::vec3& fallback)
{
    const double len = m::la::length(v);
    return len > 1e-12 ? v / len : fallback;
}

double Smoothstep(double e0, double e1, double x)
{
    const double t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }

double Fbm(m::vec3 q, int octaves)
{
    double sum = 0.0, amp = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
        norm += amp;
        amp *= 0.5;
        q = q * 2.03 + m::vec3(17.13, -9.31, 5.77);
    }
    return sum / norm;
}

double Shelter(const ProjectDamageBVH& bvh, const m::vec3& p, const m::vec3& n,
               const m::vec3& t, const m::vec3& b, double radius, double eps)
{
    int hits = 0;
    constexpr int rays = 12;
    for (int k = 0; k < rays; ++k) {
        const double z = (k + 0.5) / rays;
        const double r = std::sqrt(1.0 - z * z);
        const double phi = k * 2.399963229728653;
        const m::vec3 d = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z;
        double dist = 0.0;
        if (bvh.Raycast(p + n * eps, d, radius, &dist) && dist > eps * 0.1) ++hits;
    }
    return double(hits) / rays;
}

struct StyleSpec {
    double reposeDeg;
    double profileK;
    double heightMul, widthMul;
    double edgeNoise;
    double bump, bumpFreq;
    double sag;
    int satMin, satMax;
    double satDist0, satDist1;
    double satSize0, satSize1;
    double satHeight;
    double satK;
    double sizeSkew;
    int extraPores;
};

const StyleSpec kStyles[4] = {
    {48, 0.70, 1.00, 1.00, 0.30, 0.30, 2.6, 0.35, 2, 5, 0.55, 1.45, 0.12, 0.30, 0.75, 0.60, 1.8, 2},
    {34, 2.20, 0.50, 1.35, 0.12, 0.05, 1.3, 0.55, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.3, 0},
    {72, 0.28, 0.55, 1.25, 0.32, 0.14, 3.6, 0.15, 0, 2, 0.90, 1.25, 0.10, 0.22, 0.45, 0.30, 1.2, 6},
    {66, 0.65, 1.30, 0.85, 0.16, 0.22, 2.4, 0.25, 2, 5, 0.40, 0.80, 0.36, 0.58, 0.85, 0.75, 1.5, 3},
};

struct SurfaceGrid {
    static constexpr int J = 24, I = 10;
    m::vec3 pos, t, b, n;
    double rmax = 0.0;
    double s[J][I + 1];
    double lim[J];

    void Build(const ProjectDamageBVH& bvh, double radius, double maxSlope)
    {
        rmax = radius;
        const double probe = 0.9 * rmax;
        const double step = rmax / I;
        for (int j = 0; j < J; ++j) {
            const double phi = 2.0 * m::kPi * j / J;
            const m::vec3 dir = t * std::cos(phi) + b * std::sin(phi);
            s[j][0] = 0.0;
            lim[j] = rmax;
            double prev = 0.0;
            for (int i = 1; i <= I; ++i) {
                const m::vec3 x = pos + dir * (step * i) + n * probe;
                double dist = 0.0;
                const bool hit = bvh.Raycast(x, -n, 2.0 * probe, &dist);
                const double sv = probe - dist;
                if (!hit || std::abs(sv - prev) > maxSlope * step) {
                    lim[j] = step * (i - 1);
                    for (int k = i; k <= I; ++k) s[j][k] = prev;
                    break;
                }
                s[j][i] = prev = sv;
            }
        }
    }

    void Angle(double u, double v, int& j0, int& j1, double& w) const
    {
        double a = std::atan2(v, u);
        if (a < 0) a += 2.0 * m::kPi;
        const double f = a / (2.0 * m::kPi) * J;
        j0 = int(std::floor(f)) % J;
        j1 = (j0 + 1) % J;
        w = f - std::floor(f);
    }

    double Limit(double u, double v) const
    {
        int j0, j1; double w;
        Angle(u, v, j0, j1, w);
        const double a0 = 2.0 * m::kPi * j0 / J, a1 = 2.0 * m::kPi * (j0 + 1) / J;
        const double p0x = lim[j0] * std::cos(a0), p0y = lim[j0] * std::sin(a0);
        const double p1x = lim[j1] * std::cos(a1), p1y = lim[j1] * std::sin(a1);
        const double ex = p1x - p0x, ey = p1y - p0y;
        const double len = std::hypot(u, v);
        if (len < 1e-12) return std::min(lim[j0], lim[j1]);
        const double dx = u / len, dy = v / len;
        const double den = dx * ey - dy * ex;
        if (std::abs(den) < 1e-12) return std::min(lim[j0], lim[j1]);
        return std::max(0.0, (p0x * ey - p0y * ex) / den);
    }

    bool Inside(double u, double v, double margin) const
    {
        return std::hypot(u, v) <= margin * Limit(u, v);
    }

    double Height(double u, double v) const
    {
        int j0, j1; double w;
        Angle(u, v, j0, j1, w);
        const double f = std::min(double(I) - 1e-9, std::hypot(u, v) / rmax * I);
        const int i0 = int(f);
        const double wi = f - i0;
        const double h0 = Lerp(s[j0][i0], s[j0][i0 + 1], wi);
        const double h1 = Lerp(s[j1][i0], s[j1][i0 + 1], wi);
        return Lerp(h0, h1, w);
    }

    void Coverage(double A, double C, double au, double av,
                  double& mean, double& worst) const
    {
        double sum = 0.0;
        worst = 1.0;
        for (int j = 0; j < J; ++j) {
            const double phi = 2.0 * m::kPi * j / J - std::atan2(av, au);
            const double need = (A * C) / std::hypot(C * std::cos(phi), A * std::sin(phi));
            const double r = std::min(1.0, lim[j] / std::max(1e-12, need));
            sum += r;
            worst = std::min(worst, r);
        }
        mean = sum / J;
    }

    m::vec3 World(double u, double v, double z) const { return pos + t * u + b * v + n * z; }
};

struct BlobSpec {
    double cu = 0, cv = 0;
    double A = 1, C = 1;
    double axisU = 1, axisV = 0;
    double height = 1;
    double root = 0;
    double sink = 0;
    double k = 1;
    double edgeAmp = 0, bumpAmp = 0, bumpFreq = 1;
    double sagU = 0, sagV = 0, sag = 0;
    m::vec3 noise;
};

struct Mapped { double u, v, surface, top; };

Mapped MapPoint(const SurfaceGrid& g, const BlobSpec& s, double rho, double th, bool topSide)
{
    rho = std::min(1.0, std::max(0.0, rho));
    const double ct = std::cos(th), st = std::sin(th);
    const double re = std::max(0.55, 1.0 + s.edgeAmp * Fbm(m::vec3(ct * 1.7, st * 1.7, 0.0) + s.noise, 3));
    const double lx = ct * s.A * re, ly = st * s.C * re;
    const double ou = lx * s.axisU - ly * s.axisV;
    const double ov = lx * s.axisV + ly * s.axisU;
    double kappa = 1.0;
    if (!g.Inside(s.cu + ou, s.cv + ov, 0.97)) {
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 12; ++it) {
            const double mid = 0.5 * (lo + hi);
            if (g.Inside(s.cu + ou * mid, s.cv + ov * mid, 0.97)) lo = mid; else hi = mid;
        }
        kappa = lo;
    }
    Mapped r;
    r.u = s.cu + ou * kappa * rho;
    r.v = s.cv + ov * kappa * rho;
    r.surface = g.Height(r.u, r.v);
    const double fall = Lerp(1.0, 0.35 + 0.65 * kappa, rho);
    const double prof = std::pow(std::max(0.0, 1.0 - rho * rho), s.k) * fall;
    const double along = (ou * s.sagU + ov * s.sagV) / std::max(1e-12, std::hypot(ou, ov));
    const double skew = std::max(0.4, 1.0 + s.sag * rho * along);
    const double bump = std::max(0.3, 1.0 + s.bumpAmp * Fbm(m::vec3(
        r.u / std::max(1e-9, s.A) * s.bumpFreq, r.v / std::max(1e-9, s.A) * s.bumpFreq, 0.37)
        + s.noise * 1.31, 3));
    r.top = r.surface - s.sink + (s.height + s.sink) * prof * skew * bump;
    if (!topSide)
        r.top = r.surface - s.sink - s.root * std::sqrt(std::max(0.0, 1.0 - rho * rho));
    return r;
}

m::Manifold BuildBlob(const m::MeshGL& unitMesh, const SurfaceGrid& g, const BlobSpec& s)
{
    m::MeshGL mesh = unitMesh;
    for (size_t j = 0; j < mesh.NumVert(); ++j) {
        const size_t idx = j * mesh.numProp;
        const double qx = mesh.vertProperties[idx], qy = mesh.vertProperties[idx + 1],
                     qz = mesh.vertProperties[idx + 2];
        const double rho = std::sqrt(std::max(0.0, 1.0 - std::abs(qz)));
        const double th = (qx * qx + qy * qy) > 1e-24 ? std::atan2(qy, qx) : 0.0;
        const Mapped r = MapPoint(g, s, rho, th, qz >= 0.0);
        const m::vec3 w = g.World(r.u, r.v, r.top);
        mesh.vertProperties[idx] = float(w.x);
        mesh.vertProperties[idx + 1] = float(w.y);
        mesh.vertProperties[idx + 2] = float(w.z);
    }
    return m::Manifold(mesh);
}
}

std::vector<m::Manifold> PlaceSurfaceDeposits(const CoreInput& in,
        const ProjectDamageParams& pIn, const std::vector<EVec>& normals)
{
    std::vector<m::Manifold> out;
    if (pIn.depositAmount <= 0) return out;
                                                                          
                                                                              
    const double vals[] = {pIn.depositSizeMin,pIn.depositSizeMax,pIn.depositHeight,
        pIn.depositGravity,pIn.depositGravityX,pIn.depositGravityY,pIn.depositGravityZ,
        pIn.depositShelter,pIn.depositShelterRadius,pIn.depositEmbed,pIn.depositPorosity,
        pIn.buildDetail,pIn.depositDetail};
    for (double v : vals) if (!std::isfinite(v)) return out;
    ProjectDamageParams p = pIn;
    p.depositAmount = std::min<Int32>(p.depositAmount, 2000);
    p.depositStyle = std::max<Int32>(0, std::min<Int32>(3, p.depositStyle));
    p.depositLayers = std::max<Int32>(1, std::min<Int32>(4, p.depositLayers));
    p.depositSizeMin = std::max(1e-3, p.depositSizeMin);
    p.depositSizeMax = std::max(p.depositSizeMin, p.depositSizeMax);
    p.depositHeight = std::max(1e-3, p.depositHeight);
    p.depositShelterRadius = std::max(1e-3, p.depositShelterRadius);
    p.buildDetail = p.depositDetail > 0.0 ? p.depositDetail : p.buildDetail;
    if (!(p.buildDetail > 0.0)) p.buildDetail = 1.0;
    if (m::la::length(m::vec3(p.depositGravityX, p.depositGravityY, p.depositGravityZ)) < 1e-12)
    { p.depositGravityX = 0.0; p.depositGravityY = 0.0; p.depositGravityZ = -1.0; }

    std::vector<double> cdf;
    std::vector<double> firstShare;
    double area = 0.0;
    const MeshView& v = in.view;
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        const bool skip = p.avoidBuild && in.avoidPoly && (*in.avoidPoly)[f];
        auto triArea = [&](Int32 a, Int32 b, Int32 c) {
            return 0.5 * m::la::length(m::la::cross(v.pts[b]-v.pts[a],v.pts[c]-v.pts[a]));
        };
        const double a1 = skip ? 0.0 : triArea(q.a,q.b,q.c);
        const double a2 = skip || q.c == q.d ? 0.0 : triArea(q.a,q.c,q.d);
        area += a1+a2;
        cdf.push_back(area);
        firstShare.push_back(a1+a2 > 0 ? a1/(a1+a2) : 1.0);
    }
    if (area <= 0) return out;

    const StyleSpec& st = kStyles[p.depositStyle];
    ProjectDamageBVH bvh;
    bvh.Build(v);
    const m::vec3 up = -Unit(m::vec3(p.depositGravityX,p.depositGravityY,p.depositGravityZ),m::vec3(0,0,-1));
    const int seg = std::max(8,std::min(48,int(std::lround(16*p.buildDetail))));
    const m::MeshGL unitMesh = m::Manifold::Sphere(1.0,std::min(48,seg*3/2)).GetMeshGL();
    const m::MeshGL smallMesh = m::Manifold::Sphere(1.0,std::max(8,seg/2)).GetMeshGL();
    const m::Manifold poreUnit = m::Manifold::Sphere(1.0,std::max(8,seg/2));
    const uint64_t seed = uint64_t(uint32_t(p.seed)) ^ 0x4445504f53495453ULL;
    const double eps = std::max(1e-5,1e-7*m::la::length(in.bbRad));
    const double gravity = Clamp01(p.depositGravity);
    const double embed = std::max(0.05,std::min(0.9,p.depositEmbed));
    const double sizeMid = std::sqrt(p.depositSizeMin * p.depositSizeMax);
    const double repose = st.reposeDeg * m::kPi / 180.0;
    const double band = 12.0 * m::kPi / 180.0;
    const double cosLo = std::cos(std::min(89.5 * m::kPi / 180.0, repose + band));
    const double cosHi = std::cos(std::max(0.0, repose - band));

    std::vector<m::Manifold> slots(size_t(p.depositAmount));
    ProjectDamageParallelFor(0, int64_t(p.depositAmount), [&](int64_t ci) {
        const int i = int(ci);
        auto rnd = [&](uint64_t ch) { return Random01(seed,uint64_t(i),ch); };
        const double pick = rnd(0)*area;
        const size_t f = size_t(std::upper_bound(cdf.begin(),cdf.end(),pick)-cdf.begin());
        if (f >= cdf.size()) return;
        const ProjectDamagePoly& q = v.polys[f];
        const bool first = rnd(1) < firstShare[f] || q.c == q.d;
        const double r1 = std::sqrt(rnd(2)), r2 = rnd(3);
        const m::vec3 pos = v.pts[q.a]*(1-r1)
            + v.pts[first?q.b:q.c]*(r1*(1-r2)) + v.pts[first?q.c:q.d]*(r1*r2);
        const m::vec3 n = Unit(normals[f],m::vec3(0,0,1));
        const m::vec3 axis = std::abs(n.z)<0.9 ? m::vec3(0,0,1) : m::vec3(0,1,0);
        const m::vec3 t0 = Unit(m::la::cross(axis,n),m::vec3(1,0,0));
        const m::vec3 b0 = m::la::cross(n,t0);
        const double spin = rnd(4)*2*m::kPi;
        const m::vec3 t = t0*std::cos(spin)+b0*std::sin(spin);
        const m::vec3 b = m::la::cross(n,t);

        const double cosSlope = m::la::dot(n, up);
        const double orientation = (1 - gravity) + gravity * Smoothstep(cosLo, cosHi, cosSlope);
        const double sheltered = p.depositShelter > 0
            ? Shelter(bvh,pos,n,t,b,p.depositShelterRadius,eps) : 0.0;
        const double probability = orientation*((1-Clamp01(p.depositShelter))
            + Clamp01(p.depositShelter)*sheltered);
        if (rnd(5) >= probability) return;
        double sizeScale = 1.0;
        if (p.mask) {
            const double w = p.mask->WeightIfUsed(pos);
            if (!p.mask->KeepW(w,i,p.seed+18000)) return;
            sizeScale = p.mask->SizeScaleW(w);
            if (sizeScale <= 1e-3) return;
        }
        if (p.depositShelter > 0) sizeScale *= 1.0 + 0.35 * Clamp01(p.depositShelter) * sheltered;

        const m::vec3 down = -up;
        const m::vec3 gt = down - n * m::la::dot(down, n);
        const double slope = m::la::length(gt);
        double gu = 1.0, gv = 0.0;
        if (slope > 1e-6) { gu = m::la::dot(gt, t) / slope; gv = m::la::dot(gt, b) / slope; }
        const double slopeAngle = std::acos(std::max(-1.0, std::min(1.0, cosSlope)));
        const double onSlope = gravity * Smoothstep(0.08, 0.5, slope) * (cosSlope > 0 ? 1.0 : 0.0);

        const double a = p.depositSizeMin * std::pow(p.depositSizeMax / p.depositSizeMin,
                                                    std::pow(rnd(6), st.sizeSkew)) * sizeScale;
        const double sizeHeight = std::max(0.5, std::min(1.6, std::sqrt(a / (sizeMid * sizeScale))));
        const double hBase = p.depositHeight * (0.75 + 0.5 * rnd(8)) * sizeScale * sizeHeight;
        const double thin = 1.0 - 0.45 * gravity * Clamp01(slopeAngle / repose);

        BlobSpec main;
        main.A = a * st.widthMul * (1.0 + 0.35 * onSlope * slope);
        main.C = a * st.widthMul * (0.65 + 0.35 * rnd(7)) * (1.0 - 0.08 * onSlope * slope);
        {
            const double jitter = (rnd(20) - 0.5) * 1.2;
            const double ru = std::cos(jitter), rv = std::sin(jitter);
            double au = Lerp(ru, gu * ru - gv * rv, 0.55 * onSlope);
            double av = Lerp(rv, gu * rv + gv * ru, 0.55 * onSlope);
            const double al = std::max(1e-9, std::hypot(au, av));
            main.axisU = au / al; main.axisV = av / al;
        }
        main.height = hBase * (2.0 - embed) * st.heightMul * thin;
        main.root = embed * hBase;
        main.k = st.profileK;
        main.edgeAmp = st.edgeNoise;
        main.bumpAmp = st.bump;
        main.bumpFreq = st.bumpFreq;
        main.sag = st.sag * onSlope;
        main.sagU = gu; main.sagV = gv;
        main.noise = m::vec3(rnd(9)*1000,rnd(10)*1000,rnd(11)*1000);

        std::vector<BlobSpec> parts{main};
        for (int layer = 1; layer < p.depositLayers; ++layer) {
            const uint64_t ch = 300 + uint64_t(layer) * 8;
            const double fs = std::pow(0.78, layer);
            BlobSpec l = main;
            l.A = main.A * fs; l.C = main.C * fs;
            l.height = main.height * (1.0 + 0.28 * layer);
            l.root = main.root * fs;
            l.cu = (gu * 0.12 * layer * onSlope + (rnd(ch) - 0.5) * 0.18 * fs) * main.A;
            l.cv = (gv * 0.12 * layer * onSlope + (rnd(ch + 1) - 0.5) * 0.18 * fs) * main.A;
            l.noise = m::vec3(rnd(ch+2)*1000, rnd(ch+3)*1000, rnd(ch+4)*1000);
            l.k = std::max(0.25, st.profileK * (1.0 - 0.12 * layer));
            parts.push_back(l);
        }
        const int nSat = st.satMax > 0
            ? st.satMin + int(rnd(21) * (st.satMax - st.satMin + 1) * 0.999999) : 0;
        const double rMain = std::sqrt(main.A * main.C);
        for (int sIdx = 0; sIdx < nSat; ++sIdx) {
            const uint64_t ch = 200 + uint64_t(sIdx) * 6;
            double ang = 2 * m::kPi * rnd(ch);
            const double downAng = std::atan2(gv, gu);
            ang = Lerp(ang, downAng + (rnd(ch + 5) - 0.5) * 1.2, 0.6 * onSlope);
            const double dist = Lerp(st.satDist0, st.satDist1, rnd(ch + 1)) * rMain;
            const double size = Lerp(st.satSize0, st.satSize1, rnd(ch + 2)) * rMain;
            BlobSpec s = main;
            s.cu = std::cos(ang) * dist; s.cv = std::sin(ang) * dist;
            s.A = size; s.C = size * (0.7 + 0.3 * rnd(ch + 3));
            s.height = size * st.satHeight * (0.8 + 0.4 * rnd(ch + 4));
            if (p.depositStyle == 3)
                s.height = std::min(main.height * Lerp(0.6, 1.0, rnd(ch + 4)), size * 1.1);
            s.root = std::min(main.root, embed * s.height);
            s.k = st.satK;
            s.sag = 0.0;
            s.edgeAmp = st.edgeNoise * 0.7;
            s.noise = main.noise + m::vec3(37.0 * (sIdx + 1), 11.0, -23.0);
            parts.push_back(s);
        }

        double rmax = 0.0;
        for (const auto& s : parts)
            rmax = std::max(rmax, std::hypot(s.cu, s.cv) + std::max(s.A, s.C) * (1.0 + s.edgeAmp));
        SurfaceGrid g;
        g.pos = pos; g.t = t; g.b = b; g.n = n;
        g.Build(bvh, rmax * 1.05, 1.5);
        {
            double mean = 0.0, worst = 0.0;
            g.Coverage(main.A, main.C, main.axisU, main.axisV, mean, worst);
            if (mean < 0.45 || worst < 0.22) return;
        }

        double thickness = 1e30;
        {
            double dist = 0.0;
            if (bvh.Raycast(pos - n * eps, -n, 4.0 * main.root + 4.0 * main.height, &dist))
                thickness = dist;
        }
        const double sink = std::max(20.0 * eps, std::min(0.12 * main.height, 0.2 * thickness));
        for (auto& s : parts) {
            s.root = std::min(s.root, 0.4 * thickness);
            s.sink = std::min(sink, std::max(s.root, 20.0 * eps));
            if (s.root < s.sink) s.root = s.sink;
        }

        std::vector<m::Manifold> solids;
        solids.push_back(BuildBlob(unitMesh, g, parts[0]));
        for (size_t k = 1; k < parts.size(); ++k) {
            const auto& s = parts[k];
            if (!g.Inside(s.cu, s.cv, 0.85)) continue;
            const bool fine = k < size_t(p.depositLayers) || p.depositStyle == 3;
            solids.push_back(BuildBlob(fine ? unitMesh : smallMesh, g, s));
        }
        m::Manifold deposit = solids.size() == 1 ? solids[0]
            : m::Manifold::BatchBoolean(solids, m::OpType::Add);

        if (p.depositPorosity > 0) {
            std::vector<m::Manifold> pores;
            const int candidates = 3 + 3 * p.depositLayers + st.extraPores;
            const int nBodies = std::min(int(parts.size()), p.depositLayers);
            for (int pi = 0; pi < candidates; ++pi) {
                const uint64_t ch = 400 + uint64_t(pi) * 6;
                if (rnd(ch) >= Clamp01(p.depositPorosity)) continue;
                const BlobSpec& s = parts[size_t(std::min(nBodies - 1, int(rnd(ch + 1) * nBodies)))];
                const double ang = 2 * m::kPi * rnd(ch + 2);
                const double rr = 0.85 * std::sqrt(rnd(ch + 3));
                const Mapped mp = MapPoint(g, s, rr, ang, true);
                const double local = mp.top - mp.surface;
                const double rMin = 0.05 * std::min(s.A, s.C), rMax = 0.2 * std::min(s.A, s.C);
                double pr = rMin * std::pow(rMax / rMin, rnd(ch + 4) * rnd(ch + 4));
                pr = std::min(pr, 0.6 * local);
                if (pr < 50.0 * eps) continue;
                const double depth = pr * (0.2 + 0.6 * rnd(ch + 5));
                const m::vec3 c = g.World(mp.u, mp.v, mp.top) - n * depth;
                const double aspect = 0.7 + 0.3 * rnd(ch + 1);
                pores.push_back(poreUnit.Transform(m::mat3x4(t * pr, b * (pr * aspect), n * (pr * 1.5), c)));
            }
            if (!pores.empty())
                deposit = deposit - m::Manifold::BatchBoolean(pores, m::OpType::Add);
        }
        if (deposit.Status()!=m::Manifold::Error::NoError)
            return;                                                     
        if (deposit.NumTri()>0) slots[size_t(i)] = std::move(deposit);
    });
    for (auto& s : slots) if (s.NumTri() > 0) out.push_back(std::move(s));
    return out;
}
PROJECT_DAMAGE_NS_MACRO_END
