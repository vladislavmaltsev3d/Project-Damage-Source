// старый способ построения отслоившегося покрытия
// ProjectDamage coating peeling — GPL-3.0-or-later; see LICENSE.
//
// Recipe: coating-peeling-v6 (inline, one mesh: hole + curvy strips). A
// procedural appearance model of paint/coating failure, not a delamination
// simulation. It builds NO coating object: the coating is the outer
// Thickness of the source itself, and every feature is cut into (or fused
// onto) the source mesh by the main Boolean.
//   1. coating band: the source offset outward by Thickness/2 minus the source
//      offset inward by Thickness (mitred vertex normals, so faces move by
//      exactly that at edges and corners). Every peel cutter is intersected
//      with the band, so a peel is a hole exactly Thickness deep in the
//      original surface, also where Edge Wrap folds it over a convex edge;
//   2. peeled patches: jagged, surface-conforming cutters that remove the
//      coating down to the substrate, with satellite flake-offs, optional
//      stretch along a local axis, an optional facing filter (e.g. top faces
//      only), and a fold over convex edges (Edge Wrap);
//   3. cracks (v6): thin, noisy random walks from the rims outward that
//      branch now and then, cut into the coating (no chips);
//   4. strips (v6, replaces the v5 petals and chips): the torn coating
//      extruded from the rim into the hole, curving, with a z offset that
//      grows towards the free end; the longest are merged in pairs at their
//      free ends. Each strip (or merged pair) is a signed distance field
//      meshed by Manifold::LevelSet: one smooth surface, anchored in the
//      remaining coating, fused into the source by the main Boolean.
//      Validated against ALL patches;
//   5. optional painted zone: closed Grease Pencil contours (paint_regions.h,
//      the Paint Damage stroke input) smoothed on a fine copy of the surface
//      and cut at an iso-line Inset inside, so a painted area peels as a
//      rounded, ragged island; strips along its rim;
//   6. Flake Side: strips only on the upper or lower rim of each island;
//   7. one mesh (one_mesh.h): coating crumbs cut free are peeled too, lifted
//      paint that misses the source is dropped, pockets are filled.
// Nothing is offset or added as a separate layer: untouched source faces keep
// their original id and with it their materials, UVs and attributes.
// Candidates use SplitMix64 counter channels like surface deposits, keyed by
// seed (main seed and Coating Seed), candidate ID and channel, so changing
// the amount does not reshuffle earlier patches.
#include "manifold/manifold.h"
#include "coating_peeling.h"
#include "paint_regions.h"
#include "one_mesh.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <stdexcept>
#include <functional>
#include <unordered_map>

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
                double dist = 0.0;
                const bool hit = bvh.Raycast(pos + dir * (step * i) + n * probe, -n, 2.0 * probe, &dist);
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

    m::vec3 World(double u, double v, double z) const { return pos + t * u + b * v + n * z; }
};

                                                                           
                                                                             
                                         
struct Outline {
    double cu = 0, cv = 0, A = 1, C = 1, axisU = 1, axisV = 0;
    double lobe = 0, jag = 0;
    m::vec3 noise;

    double Radius(double th) const
    {
        const double ct = std::cos(th), st = std::sin(th);
        const double low = Fbm(m::vec3(ct * 1.3, st * 1.3, 0.0) + noise, 3);
        const double mid = std::abs(Fbm(m::vec3(ct * 4.0, st * 4.0, 0.5) + noise * 1.7, 2));
        const double fine = std::abs(Fbm(m::vec3(ct * 14.0, st * 14.0, 0.9) + noise * 2.3, 2));
        return std::max(0.3, 1.0 + lobe * low - jag * (0.7 * mid + 0.45 * fine));
    }

                                                                             
                                                                       
    double Rho(double u, double v) const
    {
        const double du = u - cu, dv = v - cv;
        const double ex = (du * axisU + dv * axisV) / A;
        const double ey = (-du * axisV + dv * axisU) / C;
        return std::hypot(ex, ey) / Radius(std::atan2(ey, ex));
    }

    void Point(double rho, double th, double& u, double& v) const
    {
        const double re = Radius(th);
        const double lx = std::cos(th) * A * re * rho, ly = std::sin(th) * C * re * rho;
        u = cu + lx * axisU - ly * axisV;
        v = cv + lx * axisV + ly * axisU;
    }
};

                                                                          
                                                                               
                                                                             
                                                                         
                                                                             
                                                                          
                                                                          
m::Manifold BuildSlab(const m::MeshGL& unitMesh, const SurfaceGrid& g, const Outline& o,
                      double up, double down, double wrap, double maxFold)
{
    m::MeshGL mesh = unitMesh;
    for (size_t j = 0; j < mesh.NumVert(); ++j) {
        const size_t idx = j * mesh.numProp;
        const double qx = mesh.vertProperties[idx], qy = mesh.vertProperties[idx + 1],
                     qz = mesh.vertProperties[idx + 2];
        const double rho = std::sqrt(std::max(0.0, 1.0 - std::abs(qz)));
        const double th = (qx * qx + qy * qy) > 1e-24 ? std::atan2(qy, qx) : 0.0;
        double u, v;
        o.Point(rho, th, u, v);
        const double surface = g.Height(u, v);
        const double over = std::hypot(u, v) - g.Limit(u, v);
        const double fold = over > 0.0 ? std::min(maxFold, wrap * over) : 0.0;
        const double floor = down + fold;
        const double prof = std::pow(std::max(0.0, 1.0 - rho * rho), 0.08);
        const double z = qz >= 0.0 ? surface - floor + (up + floor) * prof
                                   : surface - floor - 0.25 * down * std::sqrt(std::max(0.0, 1.0 - rho * rho));
        const m::vec3 w = g.World(u, v, z);
        mesh.vertProperties[idx] = float(w.x);
        mesh.vertProperties[idx + 1] = float(w.y);
        mesh.vertProperties[idx + 2] = float(w.z);
    }
    return m::Manifold(mesh);
}

                                                                                
                                                                             
                                                                     
                                                                          
                                                                           
                                                                           
                                                                        
                                                                          
                                                                         
                                                                             
                                                                        
struct StripSpec {
    m::vec3 base, d, n;                                                                                
    double length = 1, width = 1, anchor = 1;
    double rise = 0;                                                          
    double power = 2;                                                   
    double bend = 0;                                                                 
    double jag = 0;
    m::vec3 noise;
    std::vector<m::vec3> keepCoated;                                   
    std::vector<m::vec3> mustBePeeled;                               
    bool merged = false;
    m::vec3 target;                                                 
};

                                               
struct Ribbon {
    std::vector<m::vec3> c, T, E, U;                                
    std::vector<m::vec3> vT, vE;                                     
    std::vector<double> hw, ht, hd, len, rad;                                          
    std::vector<double> flare;                                                               
    m::vec3 lo, hi;
};

double Smoothstep(double a, double b, double x)
{
    const double t = std::max(0.0, std::min(1.0, (x - a) / (b - a)));
    return t * t * (3.0 - 2.0 * t);
}

m::vec3 StripPoint(const StripSpec& S, double s, double t)
{
    const m::vec3 e = m::la::cross(S.n, S.d);
    const double sig = std::max(0.0, s) / S.length;
    const double lift = sig;
                                                                     
                                                                           
                                                                          
                                                                          
                                      
    const double z = Lerp(-0.65 * t, -0.25 * t, Smoothstep(-2.0 * t, 0.0, s)) + S.rise * std::pow(lift, S.power);
    double wander = 0.25 * S.width * Fbm(m::vec3(sig * 2.2, 0.3, 0.7) + S.noise, 2) * sig;
    if (S.merged) wander *= 1.0 - Smoothstep(0.3, 1.0, lift);
    m::vec3 q = S.base + S.d * s + e * (S.bend * sig * sig + wander) + S.n * z;
    if (S.merged) {
        const m::vec3 raw = S.base + S.d * S.length + e * S.bend + S.n * (-0.25 * t + S.rise);
        q += (S.target - raw) * Smoothstep(0.3, 1.0, lift);
    }
    return q;
}

Ribbon BuildRibbon(const StripSpec& S, double t)
{
    constexpr int NS = 30;
    Ribbon R;
    const m::vec3 e = m::la::cross(S.n, S.d);
    for (int i = 0; i <= NS; ++i) {
        const double s = -S.anchor + (S.length + S.anchor) * double(i) / NS;
        R.c.push_back(StripPoint(S, s, t));
        const double sig = std::max(0.0, s) / S.length;
                                                                      
        const double rag = 1.0 + S.jag * 0.22 * Fbm(m::vec3(sig * 5.0, 1.7, 0.2) + S.noise * 1.3, 2);
        const double round = std::sqrt(std::max(0.0, 1.0 - std::pow(sig, 6.0)));
        R.hw.push_back(std::max(0.6 * t, 0.5 * S.width * (1.0 - 0.6 * std::pow(sig, 1.5)) * rag * round));
        R.ht.push_back(0.5 * t * (1.0 - 0.15 * sig));
    }
                                                                            
                                     
                                                                           
                                                                            
                                                                          
    for (int i = 0; i <= NS; ++i) {
        const double s = -S.anchor + (S.length + S.anchor) * double(i) / NS;
        R.hd.push_back(R.ht[size_t(i)] + 0.6 * t * (1.0 - Smoothstep(-1.4 * t, -0.2 * t, s)));
        R.flare.push_back(Lerp(1.0, 0.3, Smoothstep(0.5 * t, 3.0 * t, s)));
    }
    R.lo = R.hi = R.c[0];
    for (int i = 0; i < NS; ++i) {
        const m::vec3 a = R.c[size_t(i)], b = R.c[size_t(i) + 1];
        const m::vec3 T = Unit(b - a, S.d);
        const m::vec3 E = Unit(e - T * m::la::dot(e, T), e);
        R.T.push_back(T); R.E.push_back(E); R.U.push_back(m::la::cross(T, E));
        R.len.push_back(m::la::length(b - a));
        const double w = std::max(R.hw[size_t(i)], R.hw[size_t(i) + 1])
            + 2.0 * std::max(R.flare[size_t(i)], R.flare[size_t(i) + 1]) * std::max(R.hd[size_t(i)], R.hd[size_t(i) + 1]);
        const double h = std::max({R.ht[size_t(i)], R.hd[size_t(i)], R.ht[size_t(i) + 1], R.hd[size_t(i) + 1]});
        R.rad.push_back(std::sqrt(0.25 * R.len.back() * R.len.back() + w * w + h * h) + 0.05 * t);
    }
    for (int i = 0; i <= NS; ++i) {
        const size_t a = size_t(std::max(0, i - 1)), b = size_t(std::min(NS - 1, i));
        const m::vec3 T = Unit(R.T[a] + R.T[b], R.T[b]);
        const m::vec3 E0 = R.E[a] + R.E[b];
        R.vT.push_back(T);
        R.vE.push_back(Unit(E0 - T * m::la::dot(E0, T), R.E[b]));
    }
    double pad = 0.0;
    for (size_t i = 0; i < R.c.size(); ++i) {
        R.lo = m::la::min(R.lo, R.c[i]); R.hi = m::la::max(R.hi, R.c[i]);
        pad = std::max(pad, std::max(R.hw[i] + 2.0 * R.flare[i] * R.hd[i], R.hd[i]));
    }
    R.lo -= m::vec3(pad); R.hi += m::vec3(pad);
    return R;
}

                                                                           
                                                                           
                                                                          
                                                                          
                                                                      
double RibbonDistance(const Ribbon& R, const m::vec3& p, double t)
{
    size_t bi = 0;
    double bt = 0.0, bd = 1e300;
    for (size_t i = 0; i + 1 < R.c.size(); ++i) {
        const m::vec3 q = p - R.c[i], seg = R.c[i + 1] - R.c[i];
        const m::vec3 qq = q - R.E[i] * m::la::dot(q, R.E[i]);
        const double tt = std::max(0.0, std::min(1.0, m::la::dot(qq, seg) / std::max(1e-24, m::la::dot(seg, seg))));
        const double d = m::la::length(qq - seg * tt);
        if (d < bd) { bd = d; bi = i; bt = tt; }
    }
    const size_t j = bi + 1;
    const m::vec3 c = R.c[bi] + (R.c[j] - R.c[bi]) * bt;
    const m::vec3 T = Unit(R.vT[bi] * (1.0 - bt) + R.vT[j] * bt, R.T[bi]);
    const m::vec3 E = Unit(R.vE[bi] * (1.0 - bt) + R.vE[j] * bt - T * m::la::dot(R.vE[bi] * (1.0 - bt) + R.vE[j] * bt, T), R.E[bi]);
    const m::vec3 U = m::la::cross(T, E), q = p - c;
    const double hw = Lerp(R.hw[bi], R.hw[j], bt), ht = Lerp(R.ht[bi], R.ht[j], bt);
    const double hd = Lerp(R.hd[bi], R.hd[j], bt), fl = Lerp(R.flare[bi], R.flare[j], bt);
    const double r = 0.3 * t;
    const double zl = m::la::dot(q, U) - 0.5 * (ht - hd), H = 0.5 * (ht + hd);
    const double wz = hw + fl * std::max(0.0, std::min(2.0 * H, H - zl));                        
    const double qy = (std::abs(m::la::dot(q, E)) - wz) / std::sqrt(1.0 + fl * fl) + r;
    const double qz = std::abs(zl) - H + r;
    double d = std::hypot(std::max(qy, 0.0), std::max(qz, 0.0)) + std::min(std::max(qy, qz), 0.0) - r;
    if (bi == 0) d = std::max(d, m::la::dot(p - R.c.front(), -R.vT.front()));
    if (j + 1 == R.c.size()) d = std::max(d, m::la::dot(p - R.c.back(), R.vT.back()));
    return d;
}

double SmoothMin(double a, double b, double k)
{
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

                                                                           
                                                     
m::Manifold BuildStripGroup(const std::vector<Ribbon>& rib, const std::vector<size_t>& members,
                            double t, const m::vec3& grain, const m::vec3& base, const m::vec3& up)
{
    const double k = 0.9 * t;
    m::vec3 lo = rib[members[0]].lo, hi = rib[members[0]].hi;
    for (size_t i : members) { lo = m::la::min(lo, rib[i].lo); hi = m::la::max(hi, rib[i].hi); }
    lo -= m::vec3(1.5 * t); hi += m::vec3(1.5 * t);
    auto sdf = [&](m::vec3 p) {
                                                                         
        double boxD = 1e300;
        for (size_t i : members) {
            const m::vec3 out = m::la::max(rib[i].lo - p, m::la::max(p - rib[i].hi, m::vec3(0.0)));
            boxD = std::min(boxD, m::la::length(out));
        }
        if (boxD > 0.5 * t) return -boxD;
        double d = 1e300;
        for (size_t i : members) {
            const Ribbon& R = rib[i];
            const m::vec3 out = m::la::max(R.lo - p, m::la::max(p - R.hi, m::vec3(0.0)));
            if (m::la::length(out) > d + k) continue;                             
            const double di = RibbonDistance(R, p, t);
            d = d >= 1e299 ? di : SmoothMin(d, di, k);
        }
                                                                           
                                                                      
        if (std::abs(d) < 0.3 * t) {
            const double h = m::la::dot(p - base, up);
            const double w = Smoothstep(0.4 * t, 1.5 * t, h);
            if (w > 0.0) d += 0.05 * t * w * Fbm(p / (5.0 * t) + grain, 2);
        }
        return -d;                               
    };
    return m::Manifold::LevelSet(sdf, m::Box(lo, hi), 0.45 * t).Simplify(0.03 * t);
}

                                                                        
                                                                        
                                                                 
void MergeLongest(std::vector<StripSpec>& S, double t)
{
    if (S.size() < 2) return;
    std::vector<size_t> order(S.size());
    for (size_t i = 0; i < S.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return S[a].length > S[b].length; });
    const size_t nLong = std::max<size_t>(2, (S.size() * 2 + 2) / 5);
    std::vector<char> isLong(S.size(), 0);
    for (size_t k = 0; k < nLong && k < order.size(); ++k) isLong[order[k]] = 1;
    auto rawTip = [&](const StripSpec& A) {
        return A.base + A.d * A.length + m::la::cross(A.n, A.d) * A.bend + A.n * (-0.25 * t + A.rise);
    };
    for (size_t oi : order) {
        if (!isLong[oi] || S[oi].merged) continue;
        StripSpec& A = S[oi];
        const m::vec3 ta = rawTip(A);
        double bestD = 1e300;
        size_t bj = S.size();
        for (size_t j = 0; j < S.size(); ++j) {
            if (j == oi || !isLong[j] || S[j].merged) continue;
            if (m::la::dot(A.n, S[j].n) < 0.7) continue;                           
            const double dd = m::la::length(rawTip(S[j]) - ta);
            if (dd < bestD && dd < 0.9 * 0.5 * (A.length + S[j].length)) { bestD = dd; bj = j; }
        }
        if (bj == S.size()) continue;
        StripSpec& B = S[bj];
        const m::vec3 nn = Unit(A.n + B.n, A.n);
        const m::vec3 target = (ta + rawTip(B)) * 0.5 + nn * (0.2 * 0.5 * (A.rise + B.rise));
        A.merged = B.merged = true;
        A.target = B.target = target;
    }
}

                                                                                
                                                                              
                                                                              
                                                                           
void CrackWalk(double u, double v, double dir, double len, double w, double t,
               uint64_t seed, uint64_t id, int depth, std::vector<Outline>& out)
{
    const double step = std::max(1.2 * t, len / 16.0);
    const int n = std::max(2, int(len / step));
    uint64_t ch = 0;
    auto rnd = [&]() { return Random01(seed, id, ch++); };
    double drift = (rnd() - 0.5) * 0.3;
    for (int k = 0; k < n && out.size() < 600; ++k) {
        drift = 0.8 * drift + (rnd() - 0.5) * 0.35;
        dir += drift + (rnd() - 0.5) * 0.9;
        const double sl = step * Lerp(0.7, 1.2, rnd());
        const double u1 = u + std::cos(dir) * sl, v1 = v + std::sin(dir) * sl;
        Outline o;
        o.cu = 0.5 * (u + u1); o.cv = 0.5 * (v + v1);
        o.A = 0.5 * sl * 1.2; o.C = 0.5 * w;
        o.axisU = std::cos(dir); o.axisV = std::sin(dir);
        out.push_back(o);
        if (depth < 2 && k > 0 && rnd() < 0.14) {
            const double side = rnd() < 0.5 ? -1.0 : 1.0;
            CrackWalk(u1, v1, dir + side * Lerp(0.5, 1.2, rnd()), double(n - k) * step * Lerp(0.3, 0.6, rnd()),
                      w * 0.8, t, seed, id * 31 + 7 + uint64_t(k), depth + 1, out);
        }
        u = u1; v = v1;
        w = std::max(0.25 * t, w * 0.96);
    }
}

                                                                              
                                                                               
                                                                               
                                                                             
                                                                    
m::Manifold BuildOffset(const CoreInput& in, const std::vector<EVec>& normals, double d,
                        const ProjectDamageBVH* bvh, double eps)
{
    const MeshView& v = in.view;
    std::vector<m::vec3> vn(size_t(v.npts), m::vec3(0.0));
    auto corner = [&](Int32 p, Int32 prev, Int32 next, const m::vec3& fn) {
        const m::vec3 a = Unit(v.pts[prev] - v.pts[p], m::vec3(0.0));
        const m::vec3 b = Unit(v.pts[next] - v.pts[p], m::vec3(0.0));
        const double ang = std::acos(std::max(-1.0, std::min(1.0, m::la::dot(a, b))));
        vn[size_t(p)] += fn * ang;
    };
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        const m::vec3 fn = Unit(normals[f], m::vec3(0.0));
        if (q.c == q.d) {
            corner(q.a, q.c, q.b, fn); corner(q.b, q.a, q.c, fn); corner(q.c, q.b, q.a, fn);
        } else {
            corner(q.a, q.d, q.b, fn); corner(q.b, q.a, q.c, fn);
            corner(q.c, q.b, q.d, fn); corner(q.d, q.c, q.a, fn);
        }
    }
    for (auto& x : vn) x = Unit(x, m::vec3(0, 0, 1));
    std::vector<double> minCos(size_t(v.npts), 1.0);
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        const m::vec3 fn = Unit(normals[f], m::vec3(0.0));
        const Int32 ids[4] = {q.a, q.b, q.c, q.d};
        for (int k = 0; k < (q.c == q.d ? 3 : 4); ++k)
            minCos[size_t(ids[k])] = std::min(minCos[size_t(ids[k])], m::la::dot(vn[size_t(ids[k])], fn));
    }
    std::vector<m::vec3> moved(size_t(v.npts));
    ProjectDamageParallelFor(0, int64_t(v.npts), [&](int64_t i) {
        double s = std::abs(d) / std::max(0.35, minCos[size_t(i)]);
        if (d < 0.0 && bvh != nullptr) {
            double dist = 0.0;
            if (bvh->Raycast(v.pts[i] - vn[size_t(i)] * eps, -vn[size_t(i)], 1e9, &dist))
                s = std::min(s, 0.45 * dist);
        }
        moved[size_t(i)] = v.pts[i] + vn[size_t(i)] * (d < 0.0 ? -s : s);
    });
    m::MeshGL mesh;
    mesh.numProp = 3;
    for (const m::vec3& p : moved) {
        mesh.vertProperties.push_back(float(p.x));
        mesh.vertProperties.push_back(float(p.y));
        mesh.vertProperties.push_back(float(p.z));
    }
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        mesh.triVerts.insert(mesh.triVerts.end(), {uint32_t(q.a), uint32_t(q.b), uint32_t(q.c)});
        if (q.c != q.d)
            mesh.triVerts.insert(mesh.triVerts.end(), {uint32_t(q.a), uint32_t(q.c), uint32_t(q.d)});
    }
    mesh.Merge();
    return m::Manifold(mesh);
}
}

namespace {

struct Patch {
    bool valid = false;
    SurfaceGrid g;
    std::vector<Outline> outlines;                                         
    std::vector<Outline> cracks;
    double up = 0, down = 0, reach = 0, wrap = 0, maxFold = 0;
    std::vector<m::Manifold> cutters;
    std::vector<StripSpec> strips;
};

                                                                         
                                                                           
                                                                          
                                                                              
bool InCutter(const Patch& P, const m::vec3& p, double margin, bool withCracks,
              bool skipMain = false)
{
    const m::vec3 rel = p - P.g.pos;
    if (m::la::length(rel) > P.reach + P.up) return false;
    const double u = m::la::dot(rel, P.g.t), v = m::la::dot(rel, P.g.b), z = m::la::dot(rel, P.g.n);
    const double s = P.g.Height(u, v);
    const double over = std::hypot(u, v) - P.g.Limit(u, v);
    const double fold = over > 0.0 ? std::min(P.maxFold, P.wrap * over) : 0.0;                  
    if (z < s - P.down - fold - 1e-9 || z > s + P.up) return false;
    for (size_t k = skipMain ? 1 : 0; k < P.outlines.size(); ++k)
        if (P.outlines[k].Rho(u, v) <= margin) return true;
    if (withCracks) for (const auto& o : P.cracks) if (o.Rho(u, v) <= margin) return true;
    return false;
}

                                                                                
                                                                             
                                                                         
                                                                          
                                                           
                                                                       
                                                                            
                                                                              
                                                                        
                                                                             
                                                                             
                                                                            
                                                                
struct FineMesh {
    std::vector<m::vec3> pos, nrm;
    std::vector<double> thick, w;
    std::vector<std::array<int32_t, 3>> tris;
};

                                                                              
                                                                       
struct ZoneSegment { m::vec3 a, b, n, inward, island; };

struct Zone {
    bool valid = false;
    FineMesh fine;
    std::vector<double> u;                                        
    double theta = 0.5;
    m::Manifold cutter;
    std::vector<ZoneSegment> contour;
                                       
    m::vec3 lo;
    double h = 1.0;
    int dim[3] = {1, 1, 1};
    std::vector<int32_t> start, items;

    int Cell(double x, int k) const { return std::max(0, std::min(dim[k] - 1, int((x - lo[k]) / h))); }
    size_t Index(int x, int y, int z) const { return (size_t(z) * dim[1] + y) * dim[0] + x; }

    void BuildLocator()
    {
        m::vec3 hi = fine.pos[0];
        lo = fine.pos[0];
        for (const auto& q : fine.pos) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
        const m::vec3 ext = hi - lo;
        h = std::max(1e-9, m::la::length(ext)) / 64.0;
        for (int k = 0; k < 3; ++k) dim[k] = std::max(1, std::min(96, int(std::ceil(ext[k] / h)) + 1));
        lo -= m::vec3(1e-9);
        std::vector<int32_t> count(size_t(dim[0]) * dim[1] * dim[2] + 1, 0);
        auto each = [&](const std::array<int32_t, 3>& t, auto&& fn) {
            const m::vec3 a = fine.pos[size_t(t[0])], b = fine.pos[size_t(t[1])], c = fine.pos[size_t(t[2])];
            const m::vec3 bl = m::la::min(a, m::la::min(b, c)), bh = m::la::max(a, m::la::max(b, c));
            int c0[3], c1[3];
            for (int k = 0; k < 3; ++k) { c0[k] = Cell(bl[k], k); c1[k] = Cell(bh[k], k); }
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x) fn(Index(x, y, z));
        };
        for (const auto& t : fine.tris) each(t, [&](size_t id) { ++count[id + 1]; });
        for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
        start.assign(count.begin(), count.end());
        items.assign(size_t(count.back()), 0);
        std::vector<int32_t> fill(start.begin(), start.end() - 1);
        for (size_t ti = 0; ti < fine.tris.size(); ++ti)
            each(fine.tris[ti], [&](size_t id) { items[size_t(fill[id]++)] = int32_t(ti); });
    }

                                                                           
    double FieldAt(const m::vec3& p, double maxDist) const
    {
        int c[3];
        for (int k = 0; k < 3; ++k) c[k] = Cell(p[k], k);
        double best = 1e300, val = 0.0;
        const int maxR = std::max(1, int(std::ceil(maxDist / h)) + 1);
        for (int r = 0; r <= maxR; ++r) {
            for (int z = c[2] - r; z <= c[2] + r; ++z)
                for (int y = c[1] - r; y <= c[1] + r; ++y)
                    for (int x = c[0] - r; x <= c[0] + r; ++x) {
                        if (std::max(std::abs(x - c[0]), std::max(std::abs(y - c[1]), std::abs(z - c[2]))) != r) continue;
                        if (x < 0 || y < 0 || z < 0 || x >= dim[0] || y >= dim[1] || z >= dim[2]) continue;
                        const size_t id = Index(x, y, z);
                        for (int32_t k = start[id]; k < start[id + 1]; ++k) {
                            const auto& t = fine.tris[size_t(items[size_t(k)])];
                            const m::vec3 a = fine.pos[size_t(t[0])], b = fine.pos[size_t(t[1])], cc = fine.pos[size_t(t[2])];
                                                                                 
                            const m::vec3 ab = b - a, ac = cc - a, ap = p - a;
                            const double d00 = m::la::dot(ab, ab), d01 = m::la::dot(ab, ac), d11 = m::la::dot(ac, ac);
                            const double d20 = m::la::dot(ap, ab), d21 = m::la::dot(ap, ac);
                            const double den = d00 * d11 - d01 * d01;
                            if (std::abs(den) < 1e-30) continue;
                            double bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
                            bv = std::max(0.0, bv); bw = std::max(0.0, bw);
                            const double sum = bv + bw;
                            if (sum > 1.0) { bv /= sum; bw /= sum; }
                            const double bu = 1.0 - bv - bw;
                            const m::vec3 q = a * bu + b * bv + cc * bw;
                            const double d = m::la::dot(p - q, p - q);
                            if (d < best) {
                                best = d;
                                val = u[size_t(t[0])] * bu + u[size_t(t[1])] * bv + u[size_t(t[2])] * bw;
                            }
                        }
                    }
            if (best < 1e299 && std::sqrt(best) <= r * h) break;
        }
        return std::sqrt(best) <= maxDist ? val : 0.0;
    }

    bool Peeled(const m::vec3& p, double tol) const { return valid && FieldAt(p, tol) >= theta; }
};

                                                               
class PointSet {
public:
    PointSet(const std::vector<m::vec3>& pts, double cell) : pts_(pts), h_(std::max(1e-9, cell))
    {
        for (size_t i = 0; i < pts.size(); ++i) map_[Key(pts[i])].push_back(int32_t(i));
    }
                                                      
    double Nearest(const m::vec3& p, double cap) const
    {
        if (pts_.empty()) return cap;
        double best = cap * cap;
        const int64_t cx = int64_t(std::floor(p.x / h_)), cy = int64_t(std::floor(p.y / h_)), cz = int64_t(std::floor(p.z / h_));
        const int maxR = int(std::ceil(cap / h_)) + 1;
        for (int r = 0; r <= maxR; ++r) {
            for (int64_t z = cz - r; z <= cz + r; ++z)
                for (int64_t y = cy - r; y <= cy + r; ++y)
                    for (int64_t x = cx - r; x <= cx + r; ++x) {
                        if (std::max(std::llabs(x - cx), std::max(std::llabs(y - cy), std::llabs(z - cz))) != r) continue;
                        auto it = map_.find(Pack(x, y, z));
                        if (it == map_.end()) continue;
                        for (int32_t i : it->second) {
                            const m::vec3 d = pts_[size_t(i)] - p;
                            best = std::min(best, m::la::dot(d, d));
                        }
                    }
            if (std::sqrt(best) <= r * h_) break;
        }
        return std::min(cap, std::sqrt(best));
    }
private:
    static uint64_t Pack(int64_t x, int64_t y, int64_t z)
    {
        return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
    }
    uint64_t Key(const m::vec3& p) const
    {
        return Pack(int64_t(std::floor(p.x / h_)), int64_t(std::floor(p.y / h_)), int64_t(std::floor(p.z / h_)));
    }
    const std::vector<m::vec3>& pts_;
    double h_;
    std::unordered_map<uint64_t, std::vector<int32_t>> map_;
};

                                                                          
                                                                           
                                                                  
// основной вход: собираем геометрию по переданным настройкам
void BuildZone(const CoreInput& in, const ProjectDamageParams& p, const std::vector<EVec>& normals,
               const ProjectDamageBVH& bvh, const PaintRegions& paint, double t, double downT,
               double eps, Zone& Z)
{
    const MeshView& v = in.view;
    struct Tri3 { int32_t a, b, c; };
    std::vector<Tri3> tris;
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        tris.push_back({q.a, q.b, q.c});
        if (q.c != q.d) tris.push_back({q.a, q.c, q.d});
    }
                                                                          
                                                                            
                                                                   
    const double margin = 2.0 * p.coatingZoneSmooth + 4.0 * p.coatingZoneDetail;
    std::vector<char> ring(size_t(v.npts), 0);
    std::vector<size_t> region;
    double maxEdge = 0.0;
    for (size_t i = 0; i < tris.size(); ++i) {
        const auto& tr = tris[i];
        const m::vec3 lo = m::la::min(v.pts[tr.a], m::la::min(v.pts[tr.b], v.pts[tr.c]));
        const m::vec3 hi = m::la::max(v.pts[tr.a], m::la::max(v.pts[tr.b], v.pts[tr.c]));
        if (!paint.Near(lo, hi, margin)) continue;
        ring[size_t(tr.a)] = ring[size_t(tr.b)] = ring[size_t(tr.c)] = 1;
        region.push_back(i);
        maxEdge = std::max(maxEdge, std::max(m::la::length(v.pts[tr.b] - v.pts[tr.a]),
            std::max(m::la::length(v.pts[tr.c] - v.pts[tr.b]), m::la::length(v.pts[tr.a] - v.pts[tr.c]))));
    }
    if (region.empty()) return;
    int sub = std::max(1, int(std::ceil(maxEdge / std::max(1e-6, p.coatingZoneDetail))));
    sub = std::min(sub, std::max(1, int(std::sqrt(400000.0 / double(region.size())))));
    sub = std::min(sub, 64);

                                                                       
    std::vector<m::vec3> vn(size_t(v.npts), m::vec3(0.0));
    for (const auto& tr : tris) {
        const int32_t id[3] = {tr.a, tr.b, tr.c};
        const m::vec3 fn = Unit(m::la::cross(v.pts[tr.b] - v.pts[tr.a], v.pts[tr.c] - v.pts[tr.a]), m::vec3(0.0));
        for (int k = 0; k < 3; ++k) {
            const m::vec3 a = Unit(v.pts[id[(k + 1) % 3]] - v.pts[id[k]], m::vec3(0.0));
            const m::vec3 b = Unit(v.pts[id[(k + 2) % 3]] - v.pts[id[k]], m::vec3(0.0));
            vn[size_t(id[k])] += fn * std::acos(std::max(-1.0, std::min(1.0, m::la::dot(a, b))));
        }
    }
    for (auto& x : vn) x = Unit(x, m::vec3(0, 0, 1));
    std::vector<double> thick(size_t(v.npts), 1e30);
    for (Int32 i = 0; i < v.npts; ++i) {
        if (!ring[size_t(i)]) continue;
        double dist = 0.0;
        if (bvh.Raycast(v.pts[i] - vn[size_t(i)] * eps, -vn[size_t(i)], 1e6, &dist)) thick[size_t(i)] = dist;
    }

                                                  
    FineMesh& F = Z.fine;
    std::unordered_map<uint64_t, int32_t> keyed;
    auto cornerKey = [](int32_t a) { return (uint64_t(1) << 62) | uint64_t(uint32_t(a)); };
    auto edgeKey = [&](int32_t a, int32_t b, int k) {
        if (a > b) { std::swap(a, b); k = sub - k; }
        return (uint64_t(1) << 63) | (uint64_t(uint32_t(a)) << 37) | (uint64_t(uint32_t(b)) << 7) | uint64_t(k);
    };
    auto make = [&](const Tri3& tr, double w0, double w1, double w2) {
        F.pos.push_back(v.pts[tr.a] * w0 + v.pts[tr.b] * w1 + v.pts[tr.c] * w2);
        F.nrm.push_back(Unit(vn[size_t(tr.a)] * w0 + vn[size_t(tr.b)] * w1 + vn[size_t(tr.c)] * w2, m::vec3(0, 0, 1)));
        F.thick.push_back(std::min({thick[size_t(tr.a)], thick[size_t(tr.b)], thick[size_t(tr.c)]}));
                                                                          
                                                                     
        F.w.push_back(paint.Inside(F.pos.back(), F.nrm.back()) ? 1.0 : 0.0);
        return int32_t(F.pos.size() - 1);
    };
    auto at = [&](const Tri3& tr, int i, int j) {
        const int k = sub - i - j;
        uint64_t key = 0;
        bool shared = true;
        if (i == 0 && j == 0) key = cornerKey(tr.a);
        else if (i == sub) key = cornerKey(tr.b);
        else if (j == sub) key = cornerKey(tr.c);
        else if (j == 0) key = edgeKey(tr.a, tr.b, i);
        else if (i == 0) key = edgeKey(tr.a, tr.c, j);
        else if (k == 0) key = edgeKey(tr.b, tr.c, j);
        else shared = false;
        const double w1 = double(i) / sub, w2 = double(j) / sub;
        if (!shared) return make(tr, 1.0 - w1 - w2, w1, w2);
        auto it = keyed.find(key);
        if (it != keyed.end()) return it->second;
        const int32_t id = make(tr, 1.0 - w1 - w2, w1, w2);
        keyed.emplace(key, id);
        return id;
    };
    for (size_t ri : region) {
        const Tri3& tr = tris[ri];
        std::vector<int32_t> grid((size_t(sub) + 1) * (size_t(sub) + 1), -1);
        auto g = [&](int i, int j) -> int32_t& { return grid[size_t(i) * (sub + 1) + j]; };
        for (int i = 0; i <= sub; ++i)
            for (int j = 0; i + j <= sub; ++j) g(i, j) = at(tr, i, j);
        for (int i = 0; i < sub; ++i)
            for (int j = 0; i + j < sub; ++j) {
                F.tris.push_back({g(i, j), g(i + 1, j), g(i, j + 1)});
                if (i + j + 1 < sub) F.tris.push_back({g(i + 1, j), g(i + 1, j + 1), g(i, j + 1)});
            }
    }

                                                                            
    double r = std::max(0.0, p.coatingZoneSmooth);
    double inset = std::max(0.0, p.coatingZoneInset);
    const double detail = p.coatingZoneDetail;
    const size_t nf = F.pos.size();
    auto edgeSamples = [&](const std::vector<double>& f, double level) {
        std::vector<m::vec3> out;
        for (const auto& tri : F.tris)
            for (int k = 0; k < 3; ++k) {
                const int32_t a = tri[size_t(k)], b = tri[size_t((k + 1) % 3)];
                if (a > b) continue;                                                      
                const double fa = f[size_t(a)] - level, fb = f[size_t(b)] - level;
                if ((fa >= 0.0) == (fb >= 0.0)) continue;
                const double tpos = fa / (fa - fb);
                out.push_back(F.pos[size_t(a)] * (1.0 - tpos) + F.pos[size_t(b)] * tpos);
            }
        return out;
    };
    const double cap = inset + 2.0 * r + 4.0 * detail + 1e-6;
                                                             
    std::vector<double> dIn(nf, 0.0);
    {
        const std::vector<m::vec3> border = edgeSamples(F.w, 0.5);
        if (border.empty()) {
            for (size_t i = 0; i < nf; ++i) dIn[i] = F.w[i] > 0.5 ? cap : -cap;
        } else {
            const PointSet ps(border, std::max({detail, 1e-3, 0.25 * cap}));                                          
            ProjectDamageParallelFor(0, int64_t(nf), [&](int64_t i) {
                const double d = ps.Nearest(F.pos[size_t(i)], cap);
                dIn[size_t(i)] = F.w[size_t(i)] > 0.5 ? d : -d;
            });
        }
    }
                                                                            
                                                                        
    {
        double inradius = 0.0;
        for (double d : dIn) inradius = std::max(inradius, d);
        const double avail = std::max(0.0, inradius - 0.5 * detail);
        inset = std::min(inset, 0.6 * avail);
        r = std::min(r, std::max(0.0, avail - inset));
    }
                                                                     
    std::vector<double> g(nf);
    if (r <= 0.0) {
        for (size_t i = 0; i < nf; ++i) g[i] = dIn[i] - inset;
    } else {
        const double er = inset + r;
        const std::vector<m::vec3> eroded = edgeSamples(dIn, er);
        const PointSet ps(eroded, std::max({detail, 1e-3, 0.25 * cap}));
        const bool anyInside = std::any_of(dIn.begin(), dIn.end(), [&](double d) { return d >= er; });
        ProjectDamageParallelFor(0, int64_t(nf), [&](int64_t i) {
            const double d = ps.Nearest(F.pos[size_t(i)], cap);
            const double f = (dIn[size_t(i)] >= er) ? -d : (anyInside ? d : cap);
            g[size_t(i)] = r - f;
        });
    }
    const std::vector<double> smooth = g;
    Z.u = g;
    Z.theta = 0.0;
    const double jag = Clamp01(p.coatingJagged);
    const double slope = std::max(0.5 * r, 3.0 * detail);                           
    const double scale = std::max(r, 4.0 * detail);
    const uint64_t zseed = uint64_t(uint32_t(p.seed)) ^ (uint64_t(uint32_t(p.coatingSeed)) * 0x9e3779b97f4a7c15ULL);
    const m::vec3 off(Random01(zseed, 7, 900) * 1000, Random01(zseed, 7, 901) * 1000, Random01(zseed, 7, 902) * 1000);
    for (size_t i = 0; i < Z.u.size(); ++i) {
        const m::vec3 q = F.pos[i];
        const double low = Fbm(q / (1.2 * scale) + off, 3);
        const double fine = std::abs(Fbm(q / (0.3 * scale) + off * 1.7, 2));
        Z.u[i] += jag * slope * (0.7 * low - 0.45 * fine);
    }

                                          
    std::vector<m::vec3> P, N;
    std::vector<double> TH;
    std::unordered_map<uint64_t, int32_t> idOf;
    auto vertexId = [&](int32_t fv) {
        const uint64_t key = uint64_t(uint32_t(fv));
        auto it = idOf.find(key);
        if (it != idOf.end()) return it->second;
        P.push_back(F.pos[size_t(fv)]); N.push_back(F.nrm[size_t(fv)]); TH.push_back(F.thick[size_t(fv)]);
        idOf.emplace(key, int32_t(P.size() - 1));
        return int32_t(P.size() - 1);
    };
    auto isoId = [&](int32_t a, int32_t b) {
        if (a > b) std::swap(a, b);
        const uint64_t key = (uint64_t(1) << 63) | (uint64_t(uint32_t(a)) << 31) | uint64_t(uint32_t(b));
        auto it = idOf.find(key);
        if (it != idOf.end()) return it->second;
        const double ua = Z.u[size_t(a)], ub = Z.u[size_t(b)];
        const double f = std::max(0.0, std::min(1.0, (Z.theta - ua) / (ub - ua)));
        P.push_back(F.pos[size_t(a)] * (1 - f) + F.pos[size_t(b)] * f);
        N.push_back(Unit(F.nrm[size_t(a)] * (1 - f) + F.nrm[size_t(b)] * f, F.nrm[size_t(a)]));
        TH.push_back(std::min(F.thick[size_t(a)], F.thick[size_t(b)]));
        idOf.emplace(key, int32_t(P.size() - 1));
        return int32_t(P.size() - 1);
    };
    std::vector<std::array<int32_t, 3>> T;
    for (const auto& tri : F.tris) {
        bool inside[3];
        int count = 0;
        for (int k = 0; k < 3; ++k) { inside[k] = Z.u[size_t(tri[k])] >= Z.theta; count += inside[k]; }
        if (count == 0) continue;
        if (count == 3) { T.push_back({vertexId(tri[0]), vertexId(tri[1]), vertexId(tri[2])}); continue; }
                                                          
        int r = 0;
        if (count == 1) { while (!inside[r]) ++r; } else { while (inside[r]) ++r; r = (r + 1) % 3; }
        const int32_t A = tri[r], B = tri[(r + 1) % 3], C = tri[(r + 2) % 3];
        m::vec3 e0, e1;
        if (count == 1) {              
            const int32_t ab = isoId(A, B), ca = isoId(C, A);
            T.push_back({vertexId(A), ab, ca});
            e0 = P[size_t(ab)]; e1 = P[size_t(ca)];
        } else {                                     
            const int32_t bc = isoId(B, C), ca = isoId(C, A);
            T.push_back({vertexId(A), vertexId(B), bc});
            T.push_back({vertexId(A), bc, ca});
            e0 = P[size_t(bc)]; e1 = P[size_t(ca)];
        }
                                                         
        const m::vec3 a = F.pos[size_t(tri[0])], b = F.pos[size_t(tri[1])], c = F.pos[size_t(tri[2])];
        const m::vec3 nf = Unit(m::la::cross(b - a, c - a), F.nrm[size_t(tri[0])]);
        const double area2 = std::max(1e-30, m::la::length(m::la::cross(b - a, c - a)));
        const m::vec3 grad = (m::la::cross(nf, c - b) * Z.u[size_t(tri[0])] + m::la::cross(nf, a - c) * Z.u[size_t(tri[1])]
            + m::la::cross(nf, b - a) * Z.u[size_t(tri[2])]) / area2;
        const m::vec3 nAvg = Unit(F.nrm[size_t(tri[0])] + F.nrm[size_t(tri[1])] + F.nrm[size_t(tri[2])], nf);
                                                                             
                                                                              
                                                        
        const m::vec3 inward = Unit(grad - nf * m::la::dot(grad, nf), m::vec3(0.0));
        const m::vec3 gs = (m::la::cross(nf, c - b) * smooth[size_t(tri[0])] + m::la::cross(nf, a - c) * smooth[size_t(tri[1])]
            + m::la::cross(nf, b - a) * smooth[size_t(tri[2])]) / area2;
        const m::vec3 island = Unit(gs - nf * m::la::dot(gs, nf), inward);
        if (m::la::length(e1 - e0) > 1e-12 && m::la::length(inward) > 0.5)
            Z.contour.push_back({e0, e1, nAvg, inward, island});
    }
    if (T.empty()) return;

                                          
    m::MeshGL mesh;
    mesh.numProp = 3;
    const double up = 2.5 * t;
    for (size_t i = 0; i < P.size(); ++i) {
        const double down = std::max(20.0 * eps, std::min(downT, 0.45 * TH[i]));
        for (const m::vec3& q : {P[i] + N[i] * up, P[i] - N[i] * down})
            for (int c = 0; c < 3; ++c) mesh.vertProperties.push_back(float(q[c]));
    }
    auto O = [](int32_t i) { return uint32_t(2 * i); };
    auto I = [](int32_t i) { return uint32_t(2 * i + 1); };
    std::unordered_map<uint64_t, int> use;
    auto ek = [](int32_t a, int32_t b) {
        if (a > b) std::swap(a, b);
        return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
    };
    for (const auto& tr : T) {
        mesh.triVerts.insert(mesh.triVerts.end(), {O(tr[0]), O(tr[1]), O(tr[2]), I(tr[0]), I(tr[2]), I(tr[1])});
        ++use[ek(tr[0], tr[1])]; ++use[ek(tr[1], tr[2])]; ++use[ek(tr[2], tr[0])];
    }
    for (const auto& tr : T)
        for (int e = 0; e < 3; ++e) {
            const int32_t a = tr[size_t(e)], b = tr[size_t((e + 1) % 3)];
            if (use[ek(a, b)] != 1) continue;
            mesh.triVerts.insert(mesh.triVerts.end(), {O(a), I(a), I(b), O(a), I(b), O(b)});
        }
    m::Manifold cut(mesh);
    if (cut.Status() != m::Manifold::Error::NoError || cut.NumTri() == 0) return;
    Z.cutter = std::move(cut);
    Z.BuildLocator();
    Z.valid = true;
}

                                                                            
                                                                              
bool SideOk(const ProjectDamageParams& p, const m::vec3& inward)
{
    if (p.coatingFlakeSide == 0) return true;
    const m::vec3 axis = Unit(m::vec3(p.coatingFlakeSideX, p.coatingFlakeSideY, p.coatingFlakeSideZ), m::vec3(0, 0, 1));
    const double outward = -m::la::dot(inward, axis);
    return p.coatingFlakeSide == 1 ? outward > 0.3 : outward < -0.3;
}
}

void PlaceCoatingPeeling(const CoreInput& in, const ProjectDamageParams& p,
                         const std::vector<EVec>& normals,
                         std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& buildup)
{
    if (!p.coatingEnable) return;
    if (p.coatingAmount < 0 || p.coatingAmount > 1000)
        throw std::invalid_argument("coating: amount 0..1000 required");
    const double vals[] = {p.coatingThickness, p.coatingPatchMin, p.coatingPatchMax,
        p.coatingJagged, p.coatingSatellites, p.coatingStretch, p.coatingDirX, p.coatingDirY,
        p.coatingDirZ, p.coatingEdgeWrap, p.coatingFacingX, p.coatingFacingY, p.coatingFacingZ,
        p.coatingFacingAngle, p.coatingFlakes, p.coatingFlakeSize, p.coatingCurl,
        p.coatingCracks, p.coatingCrackLength, p.coatingZoneSmooth, p.coatingZoneInset,
        p.coatingZoneDetail, p.coatingFlakeSideX, p.coatingFlakeSideY, p.coatingFlakeSideZ,
        p.surfPaintSnap};
    for (double x : vals) if (!std::isfinite(x))
        throw std::invalid_argument("coating: parameters must be finite");
    if (p.coatingThickness <= 0 || p.coatingPatchMin <= 0 || p.coatingPatchMax < p.coatingPatchMin
        || p.coatingFlakeSize <= 0 || p.coatingCrackLength <= 0)
        throw std::invalid_argument("coating: positive thickness, sizes and ordered patch radii required");
    if (p.coatingZoneDetail <= 0 || p.coatingZoneSmooth < 0 || p.coatingZoneInset < 0
        || p.coatingFlakeSide < 0 || p.coatingFlakeSide > 2)
        throw std::invalid_argument("coating: zone detail > 0, smooth/inset >= 0, flake side 0..2 required");
                                                                           
                                                               
    PaintRegions paint;
    if (p.coatingZoneEnable) paint.Build(in.coatingPaint, p.surfPaintSnap);
    const bool zoneOn = p.coatingZoneEnable && !paint.Empty();

    const double t = p.coatingThickness;
    if (p.coatingAmount == 0 && !zoneOn) return;

                                                                              
                                                                    
    const MeshView& v = in.view;
    const m::vec3 facing = Unit(m::vec3(p.coatingFacingX, p.coatingFacingY, p.coatingFacingZ), m::vec3(0, 0, 1));
    const double facingCos = std::cos(std::max(0.0, std::min(180.0, p.coatingFacingAngle)) * m::kPi / 180.0);
    std::vector<double> cdf, firstShare;
    double area = 0.0;
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        bool skip = p.avoidBuild && in.avoidPoly && (*in.avoidPoly)[f];
        if (p.coatingFacingOnly && m::la::dot(Unit(normals[f], m::vec3(0.0)), facing) < facingCos - 1e-12)
            skip = true;
        auto triArea = [&](Int32 a, Int32 b, Int32 c) {
            return 0.5 * m::la::length(m::la::cross(v.pts[b]-v.pts[a], v.pts[c]-v.pts[a]));
        };
        const double a1 = skip ? 0.0 : triArea(q.a, q.b, q.c);
        const double a2 = skip || q.c == q.d ? 0.0 : triArea(q.a, q.c, q.d);
        area += a1 + a2;
        cdf.push_back(area);
        firstShare.push_back(a1 + a2 > 0 ? a1 / (a1 + a2) : 1.0);
    }
    if (area <= 0 && !zoneOn) return;
    const int nPatches = area > 0 ? p.coatingAmount : 0;

    ProjectDamageBVH bvh;
    bvh.Build(v);
    const double eps = std::max(1e-5, 1e-7 * m::la::length(in.bbRad));
                                                                         
                                                                        
                                                                         
                                                                             
                                                                       
                                                
    m::Manifold band, inner;
    {
        const m::Manifold outer = BuildOffset(in, normals, 0.5 * t, nullptr, eps);
        inner = BuildOffset(in, normals, -t, &bvh, eps);
        if (outer.Status() == m::Manifold::Error::NoError && inner.Status() == m::Manifold::Error::NoError)
            band = outer - inner;
    }
    const bool banded = band.Status() == m::Manifold::Error::NoError && band.NumTri() > 0;
    const double downT = banded ? 1.5 * t : t;
    const m::MeshGL patchMesh = m::Manifold::Sphere(1.0, 24).GetMeshGL();
    const m::MeshGL crackMesh = m::Manifold::Sphere(1.0, 8).GetMeshGL();
                                                                               
                                                             
    const uint64_t seed = (uint64_t(uint32_t(p.seed)) ^ 0x434f4154494e4750ULL)                
        ^ (uint64_t(uint32_t(p.coatingSeed)) * 0x9e3779b97f4a7c15ULL);
    const double jag = Clamp01(p.coatingJagged);
    const double sats = Clamp01(p.coatingSatellites);
    const double stretch = Clamp01(p.coatingStretch);
    const double wrap = banded ? std::max(0.0, std::min(2.0, p.coatingEdgeWrap)) : 0.0;
    const m::vec3 stretchDir = Unit(m::vec3(p.coatingDirX, p.coatingDirY, p.coatingDirZ), m::vec3(0.0));
    const double sizeMid = std::sqrt(p.coatingPatchMin * p.coatingPatchMax);

                                                                  
    std::vector<Patch> patches(static_cast<size_t>(nPatches));
    ProjectDamageParallelFor(0, int64_t(nPatches), [&](int64_t ci) {
        const int i = int(ci);
        Patch& P = patches[size_t(i)];
        auto rnd = [&](uint64_t ch) { return Random01(seed, uint64_t(i), ch); };
        const size_t f = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
        if (f >= cdf.size()) return;
        const ProjectDamagePoly& q = v.polys[f];
        const bool first = rnd(1) < firstShare[f] || q.c == q.d;
        const double r1 = std::sqrt(rnd(2)), r2 = rnd(3);
        const m::vec3 pos = v.pts[q.a]*(1-r1)
            + v.pts[first?q.b:q.c]*(r1*(1-r2)) + v.pts[first?q.c:q.d]*(r1*r2);
        const m::vec3 n = Unit(normals[f], m::vec3(0, 0, 1));
        const m::vec3 axis = std::abs(n.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(0, 1, 0);
        const m::vec3 t0 = Unit(m::la::cross(axis, n), m::vec3(1, 0, 0));
        const m::vec3 b0 = m::la::cross(n, t0);
        const double spin = rnd(4) * 2 * m::kPi;
        const m::vec3 tt = t0 * std::cos(spin) + b0 * std::sin(spin);
        const m::vec3 bb = m::la::cross(n, tt);

        double sizeScale = 1.0;
        if (p.mask) {
            const double w = p.mask->WeightIfUsed(pos);
            if (!p.mask->KeepW(w, i, p.seed + 19000)) return;
            sizeScale = p.mask->SizeScaleW(w);
            if (sizeScale <= 1e-3) return;
        }
        const double R = p.coatingPatchMin * std::pow(p.coatingPatchMax / p.coatingPatchMin,
                                                      std::pow(rnd(5), 1.6)) * sizeScale;

                                                                            
                                        
        double au = 1.0, av = 0.0, k = 0.0;
        {
            const m::vec3 dp = stretchDir - n * m::la::dot(stretchDir, n);
            k = stretch * m::la::length(dp);
            if (k > 1e-6) {
                const double du = m::la::dot(dp, tt), dv = m::la::dot(dp, bb);
                const double dl = std::hypot(du, dv);
                const double jit = (rnd(13) - 0.5) * 0.5 * (1.0 - stretch);
                const double tu = du / dl * std::cos(jit) - dv / dl * std::sin(jit);
                const double tv = du / dl * std::sin(jit) + dv / dl * std::cos(jit);
                au = Lerp(1.0, tu, std::min(1.0, 2.0 * k));
                av = Lerp(0.0, tv, std::min(1.0, 2.0 * k));
                const double l = std::max(1e-9, std::hypot(au, av));
                au /= l; av /= l;
            }
        }
        Outline main;
        main.A = R * (1.0 + 1.6 * k);
        main.C = R * (0.55 + 0.45 * rnd(6)) / (1.0 + 0.4 * k);
        main.axisU = au; main.axisV = av;
        main.lobe = 0.2 + 0.45 * jag;
        main.jag = 0.05 + 0.45 * jag;
        main.noise = m::vec3(rnd(7) * 1000, rnd(8) * 1000, rnd(9) * 1000);
        P.outlines.push_back(main);

        const int nSat = int(rnd(10) * (0.5 + 5.0 * sats));
        for (int s = 0; s < nSat; ++s) {
            const uint64_t ch = 100 + uint64_t(s) * 5;
            double ang = 2 * m::kPi * rnd(ch);
                                                                         
            const double along = std::atan2(av, au) + (rnd(ch + 4) < 0.5 ? 0.0 : m::kPi);
            ang = Lerp(ang, along + (rnd(ch) - 0.5) * 0.8, std::min(1.0, k));
            double cu, cv;
            main.Point(Lerp(1.05, 1.45, rnd(ch + 1)), ang, cu, cv);
            Outline o = main;
            o.cu = cu; o.cv = cv;
            o.A = R * Lerp(0.10, 0.28, rnd(ch + 2));
            o.C = o.A * Lerp(0.6, 1.0, rnd(ch + 3));
            o.noise = main.noise + m::vec3(41.0 * (s + 1), 7.0, -13.0);
            P.outlines.push_back(o);
        }

                                                                          
                                                         
        const int nCracks = int(std::lround(Clamp01(p.coatingCracks) * (3.0 + 7.0 * rnd(11))));
        for (int c = 0; c < nCracks; ++c) {
            const uint64_t ch = 200 + uint64_t(c) * 16;
            const double ang = 2 * m::kPi * rnd(ch);
            double u, vv;
            main.Point(0.97, ang, u, vv);
            double dir = std::atan2(vv - main.cv, u - main.cu) + (rnd(ch + 1) - 0.5) * 0.8;
            const double axisAng = std::atan2(av, au) + (std::cos(dir - std::atan2(av, au)) < 0 ? m::kPi : 0.0);
            dir = Lerp(dir, axisAng, 0.6 * std::min(1.0, k));
            const double len = R * Lerp(0.6, 1.4, rnd(ch + 3)) * p.coatingCrackLength;
            CrackWalk(u, vv, dir, len, t * Lerp(0.4, 0.65, rnd(ch + 4)), t, seed ^ 0x435257ULL,
                      uint64_t(i) * 4096 + uint64_t(c) * 97, 0, P.cracks);
        }

        double reach = 0.0;
        for (const auto& o : P.outlines)
            reach = std::max(reach, std::hypot(o.cu, o.cv) + std::max(o.A, o.C) * (1.0 + o.lobe));
        for (const auto& o : P.cracks)
            reach = std::max(reach, std::hypot(o.cu, o.cv) + o.A);
        P.reach = reach * 1.05;
        P.g.pos = pos; P.g.t = tt; P.g.b = bb; P.g.n = n;
        P.g.Build(bvh, P.reach, 1.5);

        double thickness = 1e30;
        {
            double dist = 0.0;
            if (bvh.Raycast(pos - n * eps, -n, 20.0 * t, &dist)) thickness = dist;
        }
        P.up = 2.5 * t;
        P.down = std::max(20.0 * eps, std::min(downT, 0.45 * thickness));
                                                                              
                                                                              
                                          
        const double maxFold = std::max(0.0, std::min(0.8 * R, 0.6 * thickness - P.down));
        P.wrap = wrap;
        P.maxFold = maxFold;
        for (const auto& o : P.outlines)
            P.cutters.push_back(BuildSlab(patchMesh, P.g, o, P.up, P.down, wrap, maxFold));
        for (const auto& o : P.cracks)
            P.cutters.push_back(BuildSlab(crackMesh, P.g, o, P.up, P.down, wrap, maxFold));

                                                                          
                                                                   
        const double curl = Clamp01(p.coatingCurl);
        const double perim = 2.0 * m::kPi * std::sqrt(0.5 * (main.A * main.A + main.C * main.C));
        const int nStrips = std::min(48, int(std::lround(Clamp01(p.coatingFlakes) * perim
            / std::max(1e-9, 0.4 * R * p.coatingFlakeSize) * Lerp(0.7, 1.1, rnd(12)))));
        for (int kf = 0; kf < nStrips; ++kf) {
            const uint64_t ch = 400 + uint64_t(kf) * 16;
            const double ang = 2 * m::kPi * (double(kf) + 0.6 * rnd(ch)) / std::max(1, nStrips);
            double bu, bv;
            main.Point(1.0, ang, bu, bv);
            const double ru = bu - main.cu, rv = bv - main.cv;
            const double rim = std::hypot(ru, rv);
            if (rim < 1e-9) continue;
            const double skew = (rnd(ch + 1) - 0.5) * 0.5;
            const double iu = -ru / rim, iv = -rv / rim;
            const double du = iu * std::cos(skew) - iv * std::sin(skew);
            const double dv = iu * std::sin(skew) + iv * std::cos(skew);
            const double eu = -dv, ev = du;                                      
            StripSpec S;
            S.width = std::min(R * Lerp(0.18, 0.4, rnd(ch + 3)) * p.coatingFlakeSize, 1.3 * perim / std::max(1, nStrips));
            S.length = std::min(0.75 * rim, R * Lerp(0.25, 0.8, std::pow(rnd(ch + 2), 1.2)) * p.coatingFlakeSize);
            if (S.length < 6.0 * t || S.width < 2.5 * t) continue;
            if (!SideOk(p, P.g.t * iu + P.g.b * iv)) continue;                              
            double rise = curl * S.length * Lerp(0.3, 0.95, rnd(ch + 4));
            if (rnd(ch + 6) < 0.2) rise *= 0.3;                        
            S.rise = std::max(1.5 * t, rise);
            S.power = Lerp(1.7, 2.6, rnd(ch + 5));
            S.bend = (rnd(ch + 7) - 0.5) * 0.6 * S.length * (0.3 + 0.7 * curl);
            S.jag = 0.3 + 0.7 * jag;
            S.anchor = std::max(3.0 * t, 0.3 * S.width);
            S.d = P.g.t * du + P.g.b * dv;
            S.n = n;
            S.base = P.g.World(bu, bv, P.g.Height(bu, bv));
            S.noise = m::vec3(rnd(ch + 9) * 1000, rnd(ch + 10) * 1000, rnd(ch + 11) * 1000);
                                                                               
                                                                       
            const double a = 0.6 * S.anchor, hw = 0.45 * S.width;
            const double pts[][2] = {{-a, 0}, {-a, hw}, {-a, -hw}, {0, hw}, {0, -hw},
                                     {0.6 * S.length, 0}, {0.25 * S.length, 0}};
            bool ok = true;
            for (const auto& q2 : pts) {
                const double u = bu + du * q2[0] + eu * q2[1], vv = bv + dv * q2[0] + ev * q2[1];
                if (!P.g.Inside(u, vv, 0.95)) { ok = false; break; }
            }
            if (!ok) continue;
            for (double x : {0.0, hw, -hw}) {
                const double u = bu - du * a + eu * x, vv = bv - dv * a + ev * x;
                S.keepCoated.push_back(P.g.World(u, vv, P.g.Height(u, vv) - 0.5 * t));
            }
            for (double f : {0.25, 0.6}) {
                const double u = bu + du * f * S.length, vv = bv + dv * f * S.length;
                S.mustBePeeled.push_back(P.g.World(u, vv, P.g.Height(u, vv) - 0.5 * t));
            }
            P.strips.push_back(std::move(S));
        }
        P.valid = true;
    });

                                       
    Zone zone;
    std::vector<StripSpec> zoneStrips;
    if (zoneOn) {
        BuildZone(in, p, normals, bvh, paint, t, downT, eps, zone);
        if (zone.valid && p.coatingFlakes > 0.0) {
            const double wTyp = sizeMid * 0.35 * p.coatingFlakeSize;
            const double spacing = std::max(2.5 * t, 0.85 * wTyp / std::max(0.05, Clamp01(p.coatingFlakes)));
            const double curl = Clamp01(p.coatingCurl);
            const uint64_t zs = seed ^ 0x5a4f4e45ULL;            
            double carry = 0.5 * spacing;
            int kf = 0;
            for (const auto& Sg : zone.contour) {
                const double len = m::la::length(Sg.b - Sg.a);
                double at = carry;
                for (; at < len && kf < 4000; at += spacing, ++kf) {
                    auto rnd = [&](uint64_t ch) { return Random01(zs, uint64_t(kf), ch); };
                    const m::vec3 n = Sg.n;
                    const double skew = (rnd(1) - 0.5) * 0.6;
                    const m::vec3 side = m::la::cross(n, Sg.inward);
                    const m::vec3 d = Unit(Sg.inward * std::cos(skew) + side * std::sin(skew), Sg.inward);
                    if (!SideOk(p, Sg.island)) continue;                                            
                    const m::vec3 e = m::la::cross(n, d);
                    StripSpec S;
                    S.length = sizeMid * Lerp(0.35, 1.1, std::pow(rnd(2), 1.2)) * p.coatingFlakeSize;
                    S.width = std::min(wTyp * Lerp(0.7, 1.3, rnd(3)), 1.3 * spacing);
                    if (S.length < 6.0 * t || S.width < 2.5 * t) continue;
                    double rise = curl * S.length * Lerp(0.3, 0.95, rnd(4));
                    if (rnd(9) < 0.2) rise *= 0.3;
                    S.rise = std::max(1.5 * t, rise);
                    S.power = Lerp(1.7, 2.6, rnd(5));
                    S.bend = (rnd(10) - 0.5) * 0.6 * S.length * (0.3 + 0.7 * curl);
                    S.jag = 0.3 + 0.7 * Clamp01(p.coatingJagged);
                    S.anchor = std::max(3.0 * t, 0.3 * S.width);
                            S.base = Sg.a + (Sg.b - Sg.a) * (at / len);
                    S.d = d; S.n = n;
                    S.noise = m::vec3(rnd(6) * 1000, rnd(7) * 1000, rnd(8) * 1000);
                    const double a = 0.6 * S.anchor, hw = 0.45 * S.width;
                    S.keepCoated = {S.base - d * a - n * (0.5 * t), S.base - d * a + e * hw - n * (0.5 * t),
                                    S.base - d * a - e * hw - n * (0.5 * t)};
                    S.mustBePeeled = {S.base + d * std::max(0.25 * S.length, 2.0 * t) - n * (0.5 * t),
                                      S.base + d * (0.6 * S.length) - n * (0.5 * t)};
                    zoneStrips.push_back(std::move(S));
                }
                carry = at - len;
            }
        }
    }
    const double zoneTol = 3.0 * t + 4.0 * p.coatingZoneDetail;
                                                                          
                                               
    std::vector<m::Manifold> zoneCracks;
    if (zone.valid && p.coatingCracks > 0.0 && !zone.contour.empty()) {
        const double every = 0.9 * sizeMid / Clamp01(p.coatingCracks);
        const uint64_t cs = seed ^ 0x435241434bULL;             
        std::vector<std::pair<const ZoneSegment*, double>> starts;
        double carry = 0.5 * every;
        for (const auto& Sg : zone.contour) {
            const double len = m::la::length(Sg.b - Sg.a);
            for (double at = carry; at < len; at += every) starts.push_back({&Sg, at / len});
            carry -= len;
            while (carry < 0) carry += every;
        }
        zoneCracks.resize(starts.size());
        ProjectDamageParallelFor(0, int64_t(starts.size()), [&](int64_t ci) {
            auto rnd = [&](uint64_t ch) { return Random01(cs, uint64_t(ci), ch); };
            const ZoneSegment& Sg = *starts[size_t(ci)].first;
            const m::vec3 pos = Sg.a + (Sg.b - Sg.a) * starts[size_t(ci)].second;
            const m::vec3 n = Sg.n, tt = -Sg.inward, bb = m::la::cross(n, tt);
            const double len = sizeMid * Lerp(0.6, 1.4, rnd(2)) * p.coatingCrackLength;
            std::vector<Outline> segs;
            CrackWalk(0.0, 0.0, (rnd(3) - 0.5) * 0.8, len, t * Lerp(0.4, 0.65, rnd(4)), t, cs, uint64_t(ci) * 4096 + 1, 0, segs);
            double reach = 0.0;
            for (const auto& o : segs) reach = std::max(reach, std::hypot(o.cu, o.cv) + o.A);
            SurfaceGrid g;
            g.pos = pos; g.t = tt; g.b = bb; g.n = n;
            g.Build(bvh, 1.05 * reach + t, 1.5);
            double thickness = 1e30, dist = 0.0;
            if (bvh.Raycast(pos - n * eps, -n, 20.0 * t, &dist)) thickness = dist;
            const double down = std::max(20.0 * eps, std::min(downT, 0.45 * thickness));
            std::vector<m::Manifold> parts;
            for (const auto& o : segs) parts.push_back(BuildSlab(crackMesh, g, o, 2.5 * t, down, 0.0, 0.0));
            m::Manifold crack = m::Manifold::BatchBoolean(parts, m::OpType::Add);
            if (crack.Status() == m::Manifold::Error::NoError) zoneCracks[size_t(ci)] = std::move(crack);
        });
    }

                                                                           
                                                                          
                                                                       
                                                                           
    auto peeledAt = [&](const m::vec3& q) {
        if (zone.Peeled(q, zoneTol)) return true;
        for (const auto& O : patches) if (O.valid && InCutter(O, q, 0.97, false)) return true;
        return false;
    };
    auto anchorOk = [&](const StripSpec& S, int own) {
        for (size_t c = 0; c < S.keepCoated.size(); ++c) {
            if (zone.Peeled(S.keepCoated[c], zoneTol)) return false;
            for (size_t o = 0; o < patches.size(); ++o)
                if (patches[o].valid && InCutter(patches[o], S.keepCoated[c], 1.0, true, int(o) == own && c > 0))
                    return false;
        }
        for (const auto& q : S.mustBePeeled) if (!peeledAt(q)) return false;
        return true;
    };
                                                                                
    std::vector<std::vector<StripSpec>> rims(patches.size() + 1);
    ProjectDamageParallelFor(0, int64_t(patches.size()), [&](int64_t pi) {
        const Patch& P = patches[size_t(pi)];
        if (!P.valid) return;
        for (const auto& S : P.strips) if (anchorOk(S, int(pi))) rims[size_t(pi)].push_back(S);
        MergeLongest(rims[size_t(pi)], t);
    });
    {
        std::vector<char> ok(zoneStrips.size(), 0);
        ProjectDamageParallelFor(0, int64_t(zoneStrips.size()), [&](int64_t fi) { ok[size_t(fi)] = anchorOk(zoneStrips[size_t(fi)], -1); });
        for (size_t i = 0; i < zoneStrips.size(); ++i) if (ok[i]) rims.back().push_back(std::move(zoneStrips[i]));
        MergeLongest(rims.back(), t);
    }
    std::vector<StripSpec> strips;
    for (auto& l : rims) for (auto& S : l) strips.push_back(std::move(S));

                                                                         
                                                               
    std::vector<m::Manifold> cuts;
    if (zone.valid) cuts.push_back(zone.cutter);
    for (auto& c : zoneCracks) if (c.NumTri() > 0) cuts.push_back(std::move(c));
    for (auto& P : patches) for (auto& c : P.cutters) cuts.push_back(std::move(c));

                                                                            
                                                                          
    std::vector<Ribbon> rib(strips.size());
    ProjectDamageParallelFor(0, int64_t(strips.size()), [&](int64_t i) { rib[size_t(i)] = BuildRibbon(strips[size_t(i)], t); });
                                                                           
                                                                         
                                                 
    std::vector<size_t> parent(strips.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (size_t i = 0; i < rib.size(); ++i)
        for (size_t j = i + 1; j < rib.size(); ++j)
            if (strips[i].merged && strips[j].merged && m::la::length(strips[j].target - strips[i].target) < 1e-9)
                parent[root(i)] = root(j);
    std::unordered_map<size_t, std::vector<size_t>> byRoot;
    for (size_t i = 0; i < rib.size(); ++i) byRoot[root(i)].push_back(i);
    std::vector<std::vector<size_t>> groups;
    for (auto& kv : byRoot) groups.push_back(std::move(kv.second));
    std::sort(groups.begin(), groups.end());
    std::vector<m::Manifold> lifted(groups.size());
    ProjectDamageParallelFor(0, int64_t(groups.size()), [&](int64_t gi) {
        const auto& G = groups[size_t(gi)];
        m::Manifold s = BuildStripGroup(rib, G, t, strips[G[0]].noise * 0.37, strips[G[0]].base, strips[G[0]].n);
        if (s.Status() == m::Manifold::Error::NoError && s.NumTri() > 0) lifted[size_t(gi)] = std::move(s);
    });
                                                                             
                                                                          
                                                                       
                                                   
    std::vector<m::Manifold> adds;
    for (auto& L : lifted) if (L.NumTri() > 0) adds.push_back(std::move(L));
    KeepOneMesh(in, cuts, adds, 0.05 * t, banded ? &band : nullptr, banded ? &inner : nullptr);
    for (auto& c : cuts) {
        if (c.Status() != m::Manifold::Error::NoError)
            throw std::runtime_error("coating: Boolean result is invalid");
        if (c.NumTri() > 0) cutters.push_back(std::move(c));
    }
    for (auto& a : adds) buildup.push_back(std::move(a));
}
PROJECT_DAMAGE_NS_MACRO_END
