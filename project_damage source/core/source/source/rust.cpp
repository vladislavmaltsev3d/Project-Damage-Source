// строим старый вариант ржавых повреждений
// ProjectDamage rust - GPL-3.0-or-later; see LICENSE.
//
// Recipe: rust-v2 (inline). A procedural appearance model of corrosion on
// painted metal, after the references: a flame-edged rust front (domain-warped
// noise on the painted mask), paint crazing and blisters ahead of the front,
// paint flakes curling at it; on the metal, pitting ahead of the scale, a
// rough eroded scale surface, perforation where the rust is strongest
// (through thin walls, a deep ragged pit in thick ones) with delaminated
// rust sheets peeling off the hole rim; and per-point attributes for the
// rust amount and the streaks running down along gravity.
//
// v2 is INLINE: no paint shell and no crust layer on top of the source. The
// paint is the outer coating Thickness of the source itself and the scale is
// a rough recess Crust Height deep, so every feature is a cutter or a solid
// fused into the source by the main Boolean, and untouched source faces keep
// their materials, UVs and attributes.
//
// The field: r(x) = clamp(intensity * mask(x) + edgeBias * crease(x) +
// bottomBias * low(x)) + noise(x), noise amplitude Noise/2. mask comes from
// closed Grease Pencil contours (paint_regions.h): 0 on a contour, rising to
// 1 at kPaintRamp x the contour's inradius inside it, so each painted area
// grows nested bands - crazing at its border, bare metal and pits further in,
// scale, and perforation at its heart. Without painted input the mask is 1
// (uniform) unless a bias is set. Every feature is an iso-band of r on a fine
// copy of the surface (Detail), so metal, paint and attributes share one front.
#include "manifold/manifold.h"
#include "rust.h"
#include "paint_regions.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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

double Smoothstep(double e0, double e1, double x)
{
    if (e1 <= e0) return x >= e1 ? 1.0 : 0.0;
    const double t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

                                                                           
                                                                  
constexpr double kPaintRamp = 0.6;

double PaintedBase(const RustField& f, const m::vec3& x, const m::vec3& n)
{
    if (!f.painted || f.paint.Empty()) return 0.0;
    double frac = -1.0;
    if (f.paint.Signed(x, n, 1e-6, &frac) <= 0.0) return 0.0;
    return f.intensity * Smoothstep(0.0, kPaintRamp, frac);
}

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


struct Tri { int32_t a, b, c, poly; };

std::vector<Tri> Triangulate(const MeshView& v)
{
    std::vector<Tri> tris;
    tris.reserve(size_t(v.npolys) * 2);
    for (Int32 f = 0; f < v.npolys; ++f) {
        const ProjectDamagePoly& q = v.polys[f];
        tris.push_back({q.a, q.b, q.c, f});
        if (q.c != q.d) tris.push_back({q.a, q.c, q.d, f});
    }
    return tris;
}

double TriArea(const MeshView& v, const Tri& t)
{
    return 0.5 * m::la::length(m::la::cross(v.pts[t.b] - v.pts[t.a], v.pts[t.c] - v.pts[t.a]));
}


                                                                           
                                                  
double ClosestOnTri(const m::vec3& p, const m::vec3& a, const m::vec3& b, const m::vec3& c,
                    double& u, double& v, double& w)
{
    const m::vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
    auto done = [&](double uu, double vv, double ww) {
        u = uu; v = vv; w = ww;
        const m::vec3 q = a * uu + b * vv + c * ww;
        return m::la::dot(p - q, p - q);
    };
    if (d1 <= 0 && d2 <= 0) return done(1, 0, 0);
    const m::vec3 bp = p - b;
    const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return done(0, 1, 0);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double t = d1 / (d1 - d3); return done(1 - t, t, 0); }
    const m::vec3 cp = p - c;
    const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return done(0, 0, 1);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double t = d2 / (d2 - d6); return done(1 - t, 0, t); }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return done(0, 1 - t, t);
    }
    const double denom = 1.0 / (va + vb + vc);
    const double vv = vb * denom, ww = vc * denom;
    return done(1 - vv - ww, vv, ww);
}

                                                                           
class Locator {
public:
    void Build(const MeshView& v, const std::vector<Tri>& tris)
    {
        v_ = &v; tris_ = &tris;
        lo_ = hi_ = v.pts[0];
        for (Int32 i = 1; i < v.npts; ++i) { lo_ = m::la::min(lo_, v.pts[i]); hi_ = m::la::max(hi_, v.pts[i]); }
        const m::vec3 ext = hi_ - lo_;
        const double diag = std::max(1e-9, m::la::length(ext));
        h_ = diag / 64.0;
        for (int k = 0; k < 3; ++k) dim_[k] = std::max(1, std::min(96, int(std::ceil(ext[k] / h_)) + 1));
        lo_ -= m::vec3(1e-9);
        std::vector<int32_t> count(size_t(dim_[0]) * dim_[1] * dim_[2] + 1, 0);
        auto each = [&](const Tri& t, auto&& fn) {
            const m::vec3 a = v.pts[t.a], b = v.pts[t.b], c = v.pts[t.c];
            const m::vec3 bl = m::la::min(a, m::la::min(b, c)), bh = m::la::max(a, m::la::max(b, c));
            int c0[3], c1[3];
            for (int k = 0; k < 3; ++k) { c0[k] = Cell(bl[k], k); c1[k] = Cell(bh[k], k); }
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x) fn(Index(x, y, z));
        };
        for (const Tri& t : tris) each(t, [&](size_t id) { ++count[id + 1]; });
        for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
        start_.assign(count.begin(), count.end());
        items_.assign(size_t(count.back()), 0);
        std::vector<int32_t> fill(start_.begin(), start_.end() - 1);
        for (size_t ti = 0; ti < tris.size(); ++ti)
            each(tris[ti], [&](size_t id) { items_[size_t(fill[id]++)] = int32_t(ti); });
    }

                                                                             
    bool Query(const m::vec3& p, int32_t& tri, double& u, double& v, double& w) const
    {
        int c[3];
        for (int k = 0; k < 3; ++k) c[k] = Cell(p[k], k);
        double best = 1e300;
        tri = -1;
        const int maxR = std::max(dim_[0], std::max(dim_[1], dim_[2]));
        for (int r = 0; r <= maxR; ++r) {
            for (int z = c[2] - r; z <= c[2] + r; ++z)
                for (int y = c[1] - r; y <= c[1] + r; ++y)
                    for (int x = c[0] - r; x <= c[0] + r; ++x) {
                        if (std::max(std::abs(x - c[0]), std::max(std::abs(y - c[1]), std::abs(z - c[2]))) != r) continue;
                        if (x < 0 || y < 0 || z < 0 || x >= dim_[0] || y >= dim_[1] || z >= dim_[2]) continue;
                        const size_t id = Index(x, y, z);
                        for (int32_t k = start_[id]; k < start_[id + 1]; ++k) {
                            const Tri& t = (*tris_)[size_t(items_[size_t(k)])];
                            double uu, vv, ww;
                            const double d = ClosestOnTri(p, v_->pts[t.a], v_->pts[t.b], v_->pts[t.c], uu, vv, ww);
                            if (d < best) { best = d; tri = items_[size_t(k)]; u = uu; v = vv; w = ww; }
                        }
                    }
            if (tri >= 0 && std::sqrt(best) <= r * h_) break;
        }
        return tri >= 0;
    }

private:
    int Cell(double x, int k) const { return std::max(0, std::min(dim_[k] - 1, int((x - lo_[k]) / h_))); }
    size_t Index(int x, int y, int z) const { return (size_t(z) * dim_[1] + y) * dim_[0] + x; }
    const MeshView* v_ = nullptr;
    const std::vector<Tri>* tris_ = nullptr;
    m::vec3 lo_, hi_;
    double h_ = 1.0;
    int dim_[3] = {1, 1, 1};
    std::vector<int32_t> start_, items_;
};


                                                                           
                                                                           
m::Manifold StripSolid(const std::vector<m::vec3>& top, const std::vector<m::vec3>& bot, int ns, int nw)
{
    m::MeshGL mesh;
    mesh.numProp = 3;
    auto push = [&](const m::vec3& p) {
        mesh.vertProperties.push_back(float(p.x));
        mesh.vertProperties.push_back(float(p.y));
        mesh.vertProperties.push_back(float(p.z));
    };
    for (int i = 0; i <= ns; ++i)
        for (int j = 0; j <= nw; ++j) {
            push(bot[size_t(i * (nw + 1) + j)]);
            push(top[size_t(i * (nw + 1) + j)]);
        }
    auto B = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j)); };
    auto T = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j) + 1); };
    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
    };
    for (int i = 0; i < ns; ++i)
        for (int j = 0; j < nw; ++j) {
            quad(T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
            quad(B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
        }
    for (int i = 0; i < ns; ++i) {
        quad(B(i, 0), B(i + 1, 0), T(i + 1, 0), T(i, 0));
        quad(B(i, nw), T(i, nw), T(i + 1, nw), B(i + 1, nw));
    }
    for (int j = 0; j < nw; ++j) {
        quad(B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
        quad(B(ns, j), B(ns, j + 1), T(ns, j + 1), T(ns, j));
    }
    m::Manifold solid(mesh);
    if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
        for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
        solid = m::Manifold(mesh);
    }
    return solid;
}


                                                                              
                                                                            
                                                                          
                                       
m::Manifold BuildFlake(const m::vec3& base, const m::vec3& d, const m::vec3& n,
                       double anchor, double length, double width, double rc,
                       double tilt, double thick, double lift, const m::vec3& noise)
{
    constexpr int NS = 20, NW = 4;
    const m::vec3 e = m::la::cross(n, d);
    const double total = anchor + length;
    m::MeshGL mesh;
    mesh.numProp = 3;
    auto push = [&](const m::vec3& p) {
        mesh.vertProperties.push_back(float(p.x));
        mesh.vertProperties.push_back(float(p.y));
        mesh.vertProperties.push_back(float(p.z));
    };
    for (int i = 0; i <= NS; ++i) {
        const double s = -anchor + total * i / NS;
        double x = s, y = 0.0, phi = 0.0;
        if (s > 0.0) {
            phi = tilt + s / rc;
            x = rc * (std::sin(phi) - std::sin(tilt));
            y = rc * (std::cos(tilt) - std::cos(phi));
        }
        const m::vec3 nu = n * std::cos(phi) - d * std::sin(phi);
        const double tip = s > 0.5 * length ? (s - 0.5 * length) / (0.5 * length) : 0.0;
        const double taper = std::sqrt(std::max(0.04, 1.0 - tip * tip));
                                                                          
                                                                     
        const double wave = 0.5 * thick * Fbm(m::vec3(s / width * 1.7, 0.3, 0.0) + noise, 2)
            * std::min(1.0, std::max(0.0, s / (0.3 * length)));
        const double left = 0.5 * width * taper * (1.0 + 0.25 * Fbm(m::vec3(s / width * 3.0, 1.1, 0.0) + noise, 2));
        const double right = 0.5 * width * taper * (1.0 + 0.25 * Fbm(m::vec3(s / width * 3.0, -1.7, 0.0) + noise, 2));
        const m::vec3 c = base + d * x + n * (lift + y) + nu * wave;
        for (int j = 0; j <= NW; ++j) {
            const double f = double(j) / NW;
            const m::vec3 p = c + e * Lerp(-left, right, f);
            push(p);                          
            push(p + nu * thick);          
        }
    }
    auto bot = [&](int i, int j) { return uint32_t(2 * (i * (NW + 1) + j)); };
    auto top = [&](int i, int j) { return uint32_t(2 * (i * (NW + 1) + j) + 1); };
    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t dd) {
        mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, dd});
    };
    for (int i = 0; i < NS; ++i)
        for (int j = 0; j < NW; ++j) {
            quad(top(i, j), top(i + 1, j), top(i + 1, j + 1), top(i, j + 1));
            quad(bot(i, j), bot(i, j + 1), bot(i + 1, j + 1), bot(i + 1, j));
        }
    for (int i = 0; i < NS; ++i) {
        quad(bot(i, 0), bot(i + 1, 0), top(i + 1, 0), top(i, 0));
        quad(bot(i, NW), top(i, NW), top(i + 1, NW), bot(i + 1, NW));
    }
    for (int j = 0; j < NW; ++j) {
        quad(bot(0, j), top(0, j), top(0, j + 1), bot(0, j + 1));
        quad(bot(NS, j), bot(NS, j + 1), top(NS, j + 1), top(NS, j));
    }
    m::Manifold flake(mesh);
    if (flake.Status() == m::Manifold::Error::NoError && flake.Volume() < 0.0) {
                                                                        
        for (size_t k = 0; k < mesh.triVerts.size(); k += 3)
            std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
        flake = m::Manifold(mesh);
    }
    return flake;
}


                                                                                

struct RustCtx {
    const CoreInput* in = nullptr;
    const ProjectDamageParams* p = nullptr;
    const RustField* field = nullptr;
    std::vector<Tri> tris;
    Locator loc;
    ProjectDamageBVH bvh;
    double eps = 1e-5, scale = 6.0, amp = 0.25;
    uint64_t seed = 0;
    m::vec3 off[5];
    m::vec3 g{0, 0, -1};

    void Init(const CoreInput& inp, const ProjectDamageParams& prm, const RustField& f)
    {
        in = &inp; p = &prm; field = &f;
        tris = Triangulate(inp.view);
        if (!tris.empty())
            loc.Build(inp.view, tris);
        bvh.Build(inp.view);
        eps = std::max(1e-5, 1e-7 * m::la::length(inp.bbRad));
        scale = std::max(1e-3, prm.rustNoiseScale);
        amp = 0.5 * Clamp01(prm.rustNoise);
        seed = (uint64_t(uint32_t(prm.seed)) ^ 0x5255535452555354ULL)                       
            ^ (uint64_t(uint32_t(prm.rustSeed)) * 0x9e3779b97f4a7c15ULL);
        for (int k = 0; k < 5; k++)
            off[k] = m::vec3(Random01(seed, 0, 10 + 3 * k) * 1000, Random01(seed, 0, 11 + 3 * k) * 1000,
                             Random01(seed, 0, 12 + 3 * k) * 1000);
        g = Unit(m::vec3(prm.rustGravityX, prm.rustGravityY, prm.rustGravityZ), m::vec3(0, 0, -1));
    }

                                                                          
                                               
    double FrontNoise(const m::vec3& x) const
    {
        const m::vec3 q = x / scale;
        const m::vec3 w(Fbm(q + off[0], 3), Fbm(q + off[1], 3), Fbm(q + off[2], 3));
        const m::vec3 q2 = q + w * 0.9;
        const double base = Fbm(q2 + off[3], 4);
        const double ridge = 1.0 - 2.0 * std::abs(Fbm(q2 * 2.7 + off[4], 3));
        return 0.65 * base + 0.35 * ridge;
    }

                                                                           
    double Base(const m::vec3& x, double* dist = nullptr, m::vec3* closest = nullptr) const
    {
        int32_t t; double u, v, w;
        if (tris.empty() || !loc.Query(x, t, u, v, w)) {
            if (dist) *dist = 1e30;
            return 0.0;
        }
        const Tri& tr = tris[size_t(t)];
        const MeshView& mv = in->view;
        const m::vec3 c = mv.pts[tr.a] * u + mv.pts[tr.b] * v + mv.pts[tr.c] * w;
        if (dist) *dist = m::la::length(c - x);
        if (closest) *closest = c;
        const m::vec3 fn = Unit(m::la::cross(mv.pts[tr.b] - mv.pts[tr.a], mv.pts[tr.c] - mv.pts[tr.a]), m::vec3(0, 0, 1));
        return std::min(1.5, field->R[size_t(tr.a)] * u + field->R[size_t(tr.b)] * v + field->R[size_t(tr.c)] * w
                                 + PaintedBase(*field, c, fn));
    }

                                                                             
                                                                               
    double Noisy(double base, const m::vec3& x) const
    {
        const double gate = Smoothstep(0.02, 0.25, base);
        return gate > 0.0 ? base + amp * gate * FrontNoise(x) : base;
    }

                                                                              
                                                                             
                                                                        
    double HoleField(double base, const m::vec3& x) const
    {
        const double gate = Smoothstep(0.02, 0.25, base);
        if (gate <= 0.0) return base;
        double nz = FrontNoise(x);
        if (nz < 0.0) nz *= 1.0 - Smoothstep(p->rustHoleStart - 0.06, p->rustHoleStart + 0.02, base);
        return base + amp * gate * nz;
    }

    double Height(const m::vec3& x, const m::vec3& n, double probe, bool& ok) const
    {
        double dist = 0.0;
        ok = bvh.Raycast(x + n * probe, -n, 2.0 * probe, &dist);
        return ok ? probe - dist : 0.0;
    }

    double Wall(const m::vec3& x, const m::vec3& n) const
    {
        double dist = 0.0;
        return bvh.Raycast(x - n * eps, -n, 1e9, &dist) ? dist : 1e30;
    }
};

                                                                          
                                                        
struct Fine {
    std::vector<m::vec3> pos, nrm;
    std::vector<double> wall, r, rh;                                   
    std::vector<std::array<int32_t, 3>> tris;
};

void BuildFine(const RustCtx& C, double rmin, Fine& F)
{
    const MeshView& v = C.in->view;
    const std::vector<double>& R = C.field->R;
    std::vector<char> hot(size_t(v.npts), 0), ring(size_t(v.npts), 0);
    for (Int32 i = 0; i < v.npts; ++i) hot[size_t(i)] = R[size_t(i)] > 0.02 && R[size_t(i)] + C.amp > rmin;
                                                                            
                                           
    const bool painted = C.field->painted && !C.field->paint.Empty() && C.field->intensity + C.amp > rmin;
    const double margin = 2.0 * C.p->rustDetail;
    for (const Tri& t : C.tris) {
        bool on = hot[size_t(t.a)] || hot[size_t(t.b)] || hot[size_t(t.c)];
        if (!on && painted) {
            const m::vec3 lo = m::la::min(v.pts[t.a], m::la::min(v.pts[t.b], v.pts[t.c]));
            const m::vec3 hi = m::la::max(v.pts[t.a], m::la::max(v.pts[t.b], v.pts[t.c]));
            on = C.field->paint.Near(lo, hi, margin);
        }
        if (on) ring[size_t(t.a)] = ring[size_t(t.b)] = ring[size_t(t.c)] = 1;
    }
    std::vector<size_t> region;
    double maxEdge = 0.0;
    for (size_t i = 0; i < C.tris.size(); ++i) {
        const Tri& t = C.tris[i];
        if (!(ring[size_t(t.a)] || ring[size_t(t.b)] || ring[size_t(t.c)])) continue;
        region.push_back(i);
        maxEdge = std::max(maxEdge, std::max(m::la::length(v.pts[t.b] - v.pts[t.a]),
            std::max(m::la::length(v.pts[t.c] - v.pts[t.b]), m::la::length(v.pts[t.a] - v.pts[t.c]))));
    }
    if (region.empty()) return;
    int sub = std::max(1, int(std::ceil(maxEdge / std::max(1e-4, C.p->rustDetail))));
    sub = std::min(sub, std::max(1, int(std::sqrt(500000.0 / double(region.size())))));
    sub = std::min(sub, 64);
                                                                           
    std::vector<m::vec3> vn(size_t(v.npts), m::vec3(0.0));
    for (const Tri& t : C.tris) {
        const int32_t id[3] = {t.a, t.b, t.c};
        const m::vec3 fn = Unit(m::la::cross(v.pts[t.b] - v.pts[t.a], v.pts[t.c] - v.pts[t.a]), m::vec3(0.0));
        for (int k = 0; k < 3; ++k) {
            const m::vec3 a = Unit(v.pts[id[(k + 1) % 3]] - v.pts[id[k]], m::vec3(0.0));
            const m::vec3 b = Unit(v.pts[id[(k + 2) % 3]] - v.pts[id[k]], m::vec3(0.0));
            vn[size_t(id[k])] += fn * std::acos(std::max(-1.0, std::min(1.0, m::la::dot(a, b))));
        }
    }
    for (auto& x : vn) x = Unit(x, m::vec3(0, 0, 1));
    std::vector<double> wall(size_t(v.npts), 1e30);
    for (Int32 i = 0; i < v.npts; ++i)
        if (ring[size_t(i)]) wall[size_t(i)] = C.Wall(v.pts[i], vn[size_t(i)]);
    std::unordered_map<uint64_t, int32_t> keyed;
    auto cornerKey = [](int32_t a) { return (uint64_t(1) << 62) | uint64_t(uint32_t(a)); };
    auto edgeKey = [&](int32_t a, int32_t b, int k) {
        if (a > b) { std::swap(a, b); k = sub - k; }
        return (uint64_t(1) << 63) | (uint64_t(uint32_t(a)) << 37) | (uint64_t(uint32_t(b)) << 7) | uint64_t(k);
    };
    auto make = [&](const Tri& t, double w0, double w1, double w2) {
        const m::vec3 p = v.pts[t.a] * w0 + v.pts[t.b] * w1 + v.pts[t.c] * w2;
        F.pos.push_back(p);
        F.nrm.push_back(Unit(vn[size_t(t.a)] * w0 + vn[size_t(t.b)] * w1 + vn[size_t(t.c)] * w2, m::vec3(0, 0, 1)));
        F.wall.push_back(std::min({wall[size_t(t.a)], wall[size_t(t.b)], wall[size_t(t.c)]}));
        const double base = std::min(1.5, R[size_t(t.a)] * w0 + R[size_t(t.b)] * w1 + R[size_t(t.c)] * w2
                                              + PaintedBase(*C.field, p, F.nrm.back()));
        F.r.push_back(C.Noisy(base, p));
        F.rh.push_back(C.HoleField(base, p));
        return int32_t(F.pos.size() - 1);
    };
    auto at = [&](const Tri& t, int i, int j) {
        const int k = sub - i - j;
        uint64_t key = 0;
        bool shared = true;
        if (i == 0 && j == 0) key = cornerKey(t.a);
        else if (i == sub) key = cornerKey(t.b);
        else if (j == sub) key = cornerKey(t.c);
        else if (j == 0) key = edgeKey(t.a, t.b, i);
        else if (i == 0) key = edgeKey(t.a, t.c, j);
        else if (k == 0) key = edgeKey(t.b, t.c, j);
        else shared = false;
        const double w1 = double(i) / sub, w2 = double(j) / sub;
        if (!shared) return make(t, 1.0 - w1 - w2, w1, w2);
        auto it = keyed.find(key);
        if (it != keyed.end()) return it->second;
        const int32_t id = make(t, 1.0 - w1 - w2, w1, w2);
        keyed.emplace(key, id);
        return id;
    };
    for (size_t ri : region) {
        const Tri& t = C.tris[ri];
        std::vector<int32_t> grid((size_t(sub) + 1) * (size_t(sub) + 1), -1);
        auto gg = [&](int i, int j) -> int32_t& { return grid[size_t(i) * (sub + 1) + j]; };
        for (int i = 0; i <= sub; ++i)
            for (int j = 0; i + j <= sub; ++j) gg(i, j) = at(t, i, j);
        for (int i = 0; i < sub; ++i)
            for (int j = 0; i + j < sub; ++j) {
                F.tris.push_back({gg(i, j), gg(i + 1, j), gg(i, j + 1)});
                if (i + j + 1 < sub) F.tris.push_back({gg(i + 1, j), gg(i + 1, j + 1), gg(i, j + 1)});
            }
    }
}

                                                                          
                                                                        
                                                                  
struct Patch {
    std::vector<m::vec3> P, N;
    std::vector<double> W, R;
    std::vector<std::array<int32_t, 3>> T;
};

Patch ClipBand(const Fine& F, double lo, double hi, bool loOnHole = false, bool hiOnHole = false)
{
    Patch out;
    const std::vector<double>* fld[2] = {&F.r, &F.rh};
    struct VD { int32_t a, b; double t; double v[2]; int level; };                         
    std::unordered_map<uint64_t, int32_t> ids;
    auto vid = [&](const VD& d) {
        uint64_t key;
        if (d.a == d.b) key = uint64_t(uint32_t(d.a));
        else {
            const int32_t x = std::min(d.a, d.b), y = std::max(d.a, d.b);
            key = (uint64_t(d.level + 1) << 62) | (uint64_t(uint32_t(x)) << 31) | uint64_t(uint32_t(y));
        }
        auto it = ids.find(key);
        if (it != ids.end()) return it->second;
        const size_t a = size_t(d.a), b = size_t(d.b);
        const double t = d.t;
        out.P.push_back(F.pos[a] * (1 - t) + F.pos[b] * t);
        out.N.push_back(Unit(F.nrm[a] * (1 - t) + F.nrm[b] * t, F.nrm[a]));
        out.W.push_back(std::min(F.wall[a], F.wall[b]));
        out.R.push_back(d.v[0]);
        const int32_t id = int32_t(out.P.size() - 1);
        ids.emplace(key, id);
        return id;
    };
                                                              
    auto clip = [&](const std::vector<VD>& poly, int k, double level, bool above, int levelId) {
        std::vector<VD> res;
        const size_t n = poly.size();
        for (size_t i = 0; i < n; ++i) {
            const VD& c = poly[i];
            const VD& nx = poly[(i + 1) % n];
            const bool inC = above ? c.v[k] >= level : c.v[k] <= level;
            const bool inN = above ? nx.v[k] >= level : nx.v[k] <= level;
            if (inC) res.push_back(c);
            if (inC == inN) continue;
                                                                            
                                                                          
            int32_t ea, eb;
            if (c.a == c.b && nx.a == nx.b) { ea = c.a; eb = nx.a; }
            else if (c.a == c.b) { ea = nx.a; eb = nx.b; }
            else { ea = c.a; eb = c.b; }
            const double fa = (*fld[k])[size_t(ea)], fb = (*fld[k])[size_t(eb)];
            const double t = std::max(0.0, std::min(1.0, (level - fa) / (fb - fa)));
            VD x{ea, eb, t, {0, 0}, levelId};
            for (int q = 0; q < 2; ++q)
                x.v[q] = (*fld[q])[size_t(ea)] * (1 - t) + (*fld[q])[size_t(eb)] * t;
            x.v[k] = level;
            res.push_back(x);
        }
        return res;
    };
    for (const auto& tri : F.tris) {
        std::vector<VD> poly;
        for (int k = 0; k < 3; ++k) {
            const size_t id = size_t(tri[size_t(k)]);
            poly.push_back({tri[size_t(k)], tri[size_t(k)], 0.0, {F.r[id], F.rh[id]}, -1});
        }
        poly = clip(poly, loOnHole ? 1 : 0, lo, true, 0);
        if (poly.size() >= 3 && std::isfinite(hi)) poly = clip(poly, hiOnHole ? 1 : 0, hi, false, 1);
        if (poly.size() < 3) continue;
        std::vector<int32_t> pid;
        for (const VD& d : poly) pid.push_back(vid(d));
        for (size_t k = 1; k + 1 < pid.size(); ++k)
            if (pid[0] != pid[k] && pid[k] != pid[k + 1] && pid[0] != pid[k + 1])
                out.T.push_back({pid[0], pid[k], pid[k + 1]});
    }
    return out;
}

                                                                            
                                                                           
                                                                             
                                                                           
                          
std::vector<m::Manifold> PatchSlabs(const Patch& Pt, const std::vector<double>& top, const std::vector<double>& bot)
{
    std::vector<m::Manifold> out;
    if (Pt.T.empty()) return out;
                                      
    std::vector<int32_t> parent(Pt.P.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = int32_t(i);
    std::function<int32_t(int32_t)> find = [&](int32_t x) {
        while (parent[size_t(x)] != x) { parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; x = parent[size_t(x)]; }
        return x;
    };
    for (const auto& t : Pt.T)
        for (int e = 1; e < 3; ++e) {
            const int32_t a = find(t[0]), b = find(t[size_t(e)]);
            if (a != b) parent[size_t(a)] = b;
        }
    std::unordered_map<int32_t, std::vector<size_t>> comps;
    for (size_t k = 0; k < Pt.T.size(); ++k) comps[find(Pt.T[k][0])].push_back(k);
    std::vector<int32_t> roots;
    for (auto& kv : comps) roots.push_back(kv.first);
    std::sort(roots.begin(), roots.end());                                       
    for (int32_t root : roots) {
        const std::vector<size_t>& tris = comps[root];
        std::unordered_map<int32_t, int32_t> local;
        m::MeshGL mesh;
        mesh.numProp = 3;
        auto idx = [&](int32_t v) {
            auto it = local.find(v);
            if (it != local.end()) return it->second;
            const int32_t id = int32_t(local.size());
            local.emplace(v, id);
            for (const m::vec3& q : {Pt.P[size_t(v)] + Pt.N[size_t(v)] * top[size_t(v)],
                                     Pt.P[size_t(v)] - Pt.N[size_t(v)] * bot[size_t(v)]})
                for (int c = 0; c < 3; ++c) mesh.vertProperties.push_back(float(q[c]));
            return id;
        };
        auto O = [&](int32_t v) { return uint32_t(2 * idx(v)); };
        auto I = [&](int32_t v) { return uint32_t(2 * idx(v) + 1); };
        std::unordered_map<uint64_t, int> use;
        auto ek = [](int32_t a, int32_t b) {
            if (a > b) std::swap(a, b);
            return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
        };
        for (size_t k : tris) {
            const auto& t = Pt.T[k];
            mesh.triVerts.insert(mesh.triVerts.end(), {O(t[0]), O(t[1]), O(t[2]), I(t[0]), I(t[2]), I(t[1])});
            ++use[ek(t[0], t[1])]; ++use[ek(t[1], t[2])]; ++use[ek(t[2], t[0])];
        }
        for (size_t k : tris) {
            const auto& t = Pt.T[k];
            for (int e = 0; e < 3; ++e) {
                const int32_t a = t[size_t(e)], b = t[size_t((e + 1) % 3)];
                if (use[ek(a, b)] != 1) continue;
                mesh.triVerts.insert(mesh.triVerts.end(), {O(a), I(a), I(b), O(a), I(b), O(b)});
            }
        }
        m::Manifold s(mesh);
        if (s.Status() == m::Manifold::Error::NoError && s.NumTri() > 0)
            out.push_back(std::move(s));
    }
    return out;
}

                                                                           
                                                               
struct Seg { m::vec3 a, b, n, inward; };

std::vector<Seg> Contour(const Fine& F, double level, bool onHole = false)
{
    std::vector<Seg> out;
    const std::vector<double>& fr = onHole ? F.rh : F.r;
    for (const auto& tri : F.tris) {
        const m::vec3 a = F.pos[size_t(tri[0])], b = F.pos[size_t(tri[1])], c = F.pos[size_t(tri[2])];
        const double ra = fr[size_t(tri[0])], rb = fr[size_t(tri[1])], rc = fr[size_t(tri[2])];
        std::vector<m::vec3> pts;
        const m::vec3 P3[3] = {a, b, c};
        const double R3[3] = {ra, rb, rc};
        for (int k = 0; k < 3; ++k) {
            const double r0 = R3[k], r1 = R3[(k + 1) % 3];
            if ((r0 >= level) != (r1 >= level)) {
                const double t = (level - r0) / (r1 - r0);
                pts.push_back(P3[k] * (1 - t) + P3[(k + 1) % 3] * t);
            }
        }
        if (pts.size() != 2 || m::la::length(pts[1] - pts[0]) < 1e-12) continue;
        const m::vec3 nf = Unit(m::la::cross(b - a, c - a), F.nrm[size_t(tri[0])]);
        const double area2 = std::max(1e-30, m::la::length(m::la::cross(b - a, c - a)));
        const m::vec3 grad = (m::la::cross(nf, c - b) * ra + m::la::cross(nf, a - c) * rb + m::la::cross(nf, b - a) * rc) / area2;
        const m::vec3 inward = Unit(grad - nf * m::la::dot(grad, nf), m::vec3(0.0));
        if (m::la::length(inward) < 0.5) continue;
        out.push_back({pts[0], pts[1], Unit(F.nrm[size_t(tri[0])] + F.nrm[size_t(tri[1])] + F.nrm[size_t(tri[2])], nf), inward});
    }
    return out;
}

                                                                 
struct Sample { m::vec3 p, n; double r, wall; };

std::vector<Sample> SamplePatch(const Patch& Pt, int count, uint64_t seed, uint64_t stream)
{
    std::vector<Sample> out;
    if (Pt.T.empty() || count <= 0) return out;
    std::vector<double> cdf;
    double area = 0.0;
    for (const auto& t : Pt.T) {
        area += 0.5 * m::la::length(m::la::cross(Pt.P[size_t(t[1])] - Pt.P[size_t(t[0])], Pt.P[size_t(t[2])] - Pt.P[size_t(t[0])]));
        cdf.push_back(area);
    }
    for (int i = 0; i < count; ++i) {
        auto rnd = [&](uint64_t ch) { return Random01(seed, uint64_t(i), stream + ch); };
        const size_t k = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
        if (k >= Pt.T.size()) continue;
        const auto& t = Pt.T[k];
        double u = rnd(1), w = rnd(2);
        if (u + w > 1.0) { u = 1.0 - u; w = 1.0 - w; }
        const double v0 = 1.0 - u - w;
        const size_t a = size_t(t[0]), b = size_t(t[1]), c = size_t(t[2]);
        out.push_back({Pt.P[a] * v0 + Pt.P[b] * u + Pt.P[c] * w,
                       Unit(Pt.N[a] * v0 + Pt.N[b] * u + Pt.N[c] * w, Pt.N[a]),
                       Pt.R[a] * v0 + Pt.R[b] * u + Pt.R[c] * w,
                       std::min({Pt.W[a], Pt.W[b], Pt.W[c]})});
    }
    return out;
}

void Frame(const m::vec3& n, m::vec3& t, m::vec3& b)
{
    const m::vec3 axis = std::abs(n.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(0, 1, 0);
    t = Unit(m::la::cross(axis, n), m::vec3(1, 0, 0));
    b = m::la::cross(n, t);
}

                                                                        
m::Manifold Pit(const m::vec3& c, const m::vec3& n, double radius, double depth, double up,
                uint64_t seed, uint64_t idx)
{
    m::vec3 t, b;
    Frame(n, t, b);
    std::vector<m::vec3> hull;
    const int nr = 8;
    for (int j = 0; j < nr; ++j) {
        const double a = 2.0 * m::kPi * j / nr + (Random01(seed, idx, 900 + j) - 0.5) * 0.5;
        const double rj = radius * (0.7 + 0.5 * Random01(seed, idx, 920 + j));
        const m::vec3 o = (t * std::cos(a) + b * std::sin(a)) * rj;
        hull.push_back(c + o - n * (0.15 * depth));
        hull.push_back(c + o + n * up);
    }
    hull.push_back(c - n * depth);
    return m::Manifold::Hull(hull);
}

                                                                     
                                                                   
m::Manifold Dome(const RustCtx& C, const m::MeshGL& unit, const m::vec3& pos, const m::vec3& n,
                 double r, double h, double base, double root)
{
    m::vec3 t, b;
    Frame(n, t, b);
    m::MeshGL mesh = unit;
    const double probe = 2.0 * r + h + base;
    for (size_t j = 0; j < mesh.NumVert(); ++j) {
        const size_t idx = j * mesh.numProp;
        const double qx = mesh.vertProperties[idx], qy = mesh.vertProperties[idx + 1], qz = mesh.vertProperties[idx + 2];
        const double rho = std::sqrt(std::max(0.0, 1.0 - std::abs(qz)));
        const double th = (qx * qx + qy * qy) > 1e-24 ? std::atan2(qy, qx) : 0.0;
        const m::vec3 x = pos + t * (rho * std::cos(th) * r) + b * (rho * std::sin(th) * r);
        bool ok;
        const double s = C.Height(x, n, probe, ok);
        const double prof = std::pow(std::max(0.0, 1.0 - rho * rho), 0.6);
        const double sink = 0.05 * h;
        const double z = qz >= 0.0 ? s + base - sink + (h + sink) * prof
                                   : s + base - sink - root * std::sqrt(std::max(0.0, 1.0 - rho * rho));
        const m::vec3 w = x + n * z;
        mesh.vertProperties[idx] = float(w.x);
        mesh.vertProperties[idx + 1] = float(w.y);
        mesh.vertProperties[idx + 2] = float(w.z);
    }
    return m::Manifold(mesh);
}

                                                                         
                                                       
m::Manifold Groove(const RustCtx& C, const m::vec3& a, const m::vec3& b, const m::vec3& nIn,
                   double width, double below, double above)
{
    const m::vec3 n = Unit(nIn, m::vec3(0, 0, 1));
    const m::vec3 seg = (b - a) - n * m::la::dot(b - a, n);
    const double len = m::la::length(seg);
    if (len < 1e-9) return m::Manifold();
    const m::vec3 t = seg / len, e = m::la::cross(n, t);
    const double ext = 0.5 * width, total = len + 2.0 * ext;
    const int ns = std::max(3, std::min(24, int(std::ceil(total / std::max(1e-6, 2.0 * width)))));
    constexpr int nw = 2;
    std::vector<m::vec3> top, bot;
    const double probe = 4.0 * width + above;
    for (int i = 0; i <= ns; ++i) {
        const m::vec3 c = a + t * (-ext + total * i / ns);
        for (int j = 0; j <= nw; ++j) {
            const m::vec3 x = c + e * ((double(j) / nw - 0.5) * width);
            bool ok;
            const double h = C.Height(x, n, probe, ok);
            top.push_back(x + n * (h + above));
            bot.push_back(x + n * (h - below));
        }
    }
    return StripSolid(top, bot, ns, nw);
}
}

                                                                               

RustField ComputeRustField(const CoreInput& in, const ProjectDamageParams& p)
{
    RustField f;
    if (!p.rustEnable) return f;
    const double vals[] = {p.rustIntensity, p.rustEdgeBias, p.rustBottomBias, p.rustNoise, p.rustNoiseScale,
        p.rustDetail, p.rustCrustStart, p.rustCrustHeight, p.rustCrustRough, p.rustPitSize, p.rustPitDepth,
        p.rustPitStart, p.rustHoleStart, p.rustHoleDepth, p.rustFlakes, p.rustFlakeSize, p.rustPaintStart,
        p.rustPaintFlakes, p.rustBlisterSize, p.rustCraze, p.rustCrazeCell, p.rustBand, p.rustStreak,
        p.rustStreakLength, p.rustGravityX, p.rustGravityY, p.rustGravityZ, p.surfPaintSnap};
    for (double x : vals) if (!std::isfinite(x))
        throw std::invalid_argument("rust: parameters must be finite");
    if (p.rustNoiseScale <= 0 || p.rustDetail <= 0 || p.rustCrustHeight < 0 || p.rustPitSize <= 0
        || p.rustPitDepth < 0 || p.rustHoleDepth <= 0 || p.rustFlakeSize <= 0 || p.rustBlisterSize <= 0
        || p.rustCrazeCell <= 0 || p.rustStreakLength < 0 || p.rustPits < 0 || p.rustPits > 5000
        || p.rustBlisters < 0 || p.rustBlisters > 3000)
        throw std::invalid_argument("rust: invalid ranges (sizes > 0, pits <= 5000, blisters <= 3000)");
    const MeshView& v = in.view;
    const size_t n = size_t(v.npts);
                                                                          
                                                          
    std::vector<double> edgeness(n, 0.0), lowness(n, 0.0);
    if (p.rustEdgeBias > 0.0) {
        std::vector<double> minDot(n, 1.0);
        std::vector<m::vec3> first(n, m::vec3(0.0));
        std::vector<char> has(n, 0);
        for (Int32 f = 0; f < v.npolys; ++f) {
            const ProjectDamagePoly& q = v.polys[f];
            const m::vec3 fn = Unit(m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]), m::vec3(0.0));
            const Int32 ids[4] = {q.a, q.b, q.c, q.d};
            for (int k = 0; k < (q.c == q.d ? 3 : 4); ++k) {
                const size_t id = size_t(ids[k]);
                if (!has[id]) { first[id] = fn; has[id] = 1; }
                else minDot[id] = std::min(minDot[id], m::la::dot(first[id], fn));
            }
        }
        for (size_t i = 0; i < n; ++i)
            edgeness[i] = Smoothstep(0.98, 0.5, minDot[i]);                                    
    }
    if (p.rustBottomBias > 0.0) {
        const m::vec3 g = Unit(m::vec3(p.rustGravityX, p.rustGravityY, p.rustGravityZ), m::vec3(0, 0, -1));
        double lo = 1e300, hi = -1e300;
        for (size_t i = 0; i < n; ++i) { const double h = -m::la::dot(v.pts[i], g); lo = std::min(lo, h); hi = std::max(hi, h); }
        const double span = std::max(1e-9, hi - lo);
        for (size_t i = 0; i < n; ++i) {
            const double u = 1.0 - (-m::la::dot(v.pts[i], g) - lo) / span;                     
            lowness[i] = u * u * u;
        }
    }
    const bool procedural = p.rustEdgeBias > 0.0 || p.rustBottomBias > 0.0;
                                                                                
                                                                
    f.painted = in.rustPaint != nullptr;
    f.intensity = std::max(0.0, p.rustIntensity);
    if (f.painted) f.paint.Build(in.rustPaint, p.surfPaintSnap);
    const double uniform = (f.painted || procedural) ? 0.0 : 1.0;
    f.R.resize(n);
    for (size_t i = 0; i < n; ++i)
        f.R[i] = std::max(0.0, std::min(1.5, p.rustIntensity * uniform
            + p.rustEdgeBias * edgeness[i] + p.rustBottomBias * lowness[i]));
    f.valid = true;
    return f;
}

void PlaceRust(const CoreInput& in, const ProjectDamageParams& p, const RustField& field,
               std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& buildup)
{
    if (!field.valid) return;
    RustCtx C;
    C.Init(in, p, field);
    if (C.tris.empty()) return;
    const bool holes = p.rustHoleStart < 1.4;
    const double rmin = std::min({p.rustPitStart, p.rustCrustStart, holes ? p.rustHoleStart : 9.0});
    Fine F;
    BuildFine(C, rmin, F);
    if (F.tris.empty()) return;
    const double holeHi = holes ? p.rustHoleStart : std::numeric_limits<double>::infinity();
    const double crustH = p.rustCrustHeight;

                                                                       
    if (holes) {
        Patch H = ClipBand(F, p.rustHoleStart, std::numeric_limits<double>::infinity(), true);
        std::vector<double> top(H.P.size()), bot(H.P.size());
        for (size_t i = 0; i < H.P.size(); ++i) {
            top[i] = 1.0 + 20.0 * C.eps;                                         
            bot[i] = H.W[i] <= p.rustHoleDepth ? H.W[i] * 1.05 + 0.3 : p.rustHoleDepth;
        }
        for (auto& cut : PatchSlabs(H, top, bot)) cutters.push_back(std::move(cut));
    }

                                                                                 
    if (p.rustPits > 0 && p.rustPitDepth > 0.0 && p.rustPitStart < p.rustCrustStart) {
        Patch Pp = ClipBand(F, p.rustPitStart, p.rustCrustStart);                                        
        std::vector<Sample> s = SamplePatch(Pp, p.rustPits * 3, C.seed, 1000);
        std::vector<m::vec3> placed;
        int made = 0;
        for (size_t i = 0; i < s.size() && made < p.rustPits; ++i) {
            const double grow = Smoothstep(p.rustPitStart, p.rustCrustStart, s[i].r);
            if (Random01(C.seed, i, 1100) > 0.35 + 0.65 * grow) continue;
            const double radius = p.rustPitSize * std::pow(2.5, Random01(C.seed, i, 1101) - 0.6) * (0.6 + 0.6 * grow);
            bool clash = false;
            for (const m::vec3& q : placed)
                if (m::la::length(q - s[i].p) < 1.1 * radius) { clash = true; break; }
            if (clash) continue;
            const double depth = std::min(p.rustPitDepth * (0.5 + 0.8 * grow) * (0.7 + 0.6 * Random01(C.seed, i, 1102)),
                                          0.4 * s[i].wall);
            if (depth <= 20.0 * C.eps) continue;
            cutters.push_back(Pit(s[i].p, s[i].n, radius, depth, 1.0, C.seed, uint64_t(i)));
            placed.push_back(s[i].p);
            ++made;
        }
    }

                                                                               
                                                                          
                                                                            
                                                                               
    if (crustH > 0.0) {
        Patch Cr = ClipBand(F, p.rustCrustStart, holeHi, false, true);
        std::vector<double> top(Cr.P.size()), bot(Cr.P.size());
        const double delta = 0.03 * crustH + 10.0 * C.eps;
        const double rough = Clamp01(p.rustCrustRough);
        for (size_t i = 0; i < Cr.P.size(); ++i) {
            const m::vec3 q = Cr.P[i] / C.scale;
            const double grow = Smoothstep(p.rustCrustStart, p.rustCrustStart + 0.3, Cr.R[i]);
            const double scales = 0.55 + 0.45 * (0.5 + 0.5 * Fbm(q * 3.0 + C.off[2], 3));
            const double grain = 1.0 + rough * 0.6 * Fbm(q * 11.0 + C.off[3], 2);
            const double h = std::max(0.0, crustH * grow * scales * grain);
            top[i] = 1.0 + 20.0 * C.eps;
            bot[i] = std::max(delta, std::min(h, 0.3 * Cr.W[i]));
        }
        for (auto& erosion : PatchSlabs(Cr, top, bot)) cutters.push_back(std::move(erosion));
    }

                                                                           
    if (holes && p.rustFlakes > 0.0) {
        const std::vector<Seg> rim = Contour(F, p.rustHoleStart, true);
        const double spacing = p.rustFlakeSize * 0.9 / std::max(0.05, Clamp01(p.rustFlakes));
        const double thick = std::max(0.35 * crustH, 0.04);
        double carry = 0.5 * spacing;
        int site = 0;
        for (const Seg& S : rim) {
            const double len = m::la::length(S.b - S.a);
            double at = carry;
            for (; at < len && site < 3000; at += spacing, ++site) {
                auto rnd = [&](uint64_t ch) { return Random01(C.seed, uint64_t(site), 2000 + ch); };
                const m::vec3 base = S.a + (S.b - S.a) * (at / len);
                const double skew = (rnd(0) - 0.5) * 0.7;
                const m::vec3 side = m::la::cross(S.n, S.inward);
                const m::vec3 d = Unit(S.inward * std::cos(skew) + side * std::sin(skew), S.inward);
                                                                        
                bool ok;
                const double anchorLen = std::max(3.0 * thick, 0.25 * p.rustFlakeSize);
                const m::vec3 anchor = base - d * (0.6 * anchorLen);
                const m::vec3 over = base + d * (0.3 * p.rustFlakeSize);
                if (C.HoleField(C.Base(anchor), anchor) >= p.rustHoleStart) continue;
                if (C.HoleField(C.Base(over), over) < p.rustHoleStart) continue;
                const double h0 = C.Height(base, S.n, 2.0, ok);
                const int layers = 1 + int(rnd(1) * 2.999);
                for (int L = 0; L < layers; ++L) {
                    const double length = p.rustFlakeSize * (1.0 - 0.25 * L) * Lerp(0.5, 1.2, rnd(2 + L));
                    const double width = p.rustFlakeSize * Lerp(0.35, 0.8, rnd(5 + L));
                    const double tilt = Lerp(0.05, 0.6, rnd(8 + L)) + 0.12 * L;
                    const double roll = Lerp(0.1, 0.9, rnd(11 + L) * rnd(11 + L)) * m::kPi;
                    const double rc = std::max(2.5 * thick, length / std::max(0.15, roll));
                                                                              
                                                        
                    const double lift = -crustH - 1.02 * thick + L * 1.1 * thick;
                    const m::vec3 noise(rnd(14 + L) * 1000, rnd(17 + L) * 1000, rnd(20 + L) * 1000);
                    m::Manifold fl = BuildFlake(base + S.n * h0, d, S.n, anchorLen, length, width, rc, tilt,
                                                thick, lift, noise);
                    if (fl.Status() == m::Manifold::Error::NoError && fl.NumTri() > 0)
                        buildup.push_back(std::move(fl));
                }
            }
            carry = at - len;
        }
    }
}

// основной вход: собираем геометрию по переданным настройкам
void BuildRustPaint(const CoreInput& in, const ProjectDamageParams& p, const RustField& field, double t,
                    std::vector<m::Manifold>& cuts, std::vector<m::Manifold>& adds)
{
    if (!field.valid || !p.rustPaint) return;
    RustCtx C;
    C.Init(in, p, field);
    if (C.tris.empty()) return;
    const double paintT = p.rustPaintStart;
    const double band = std::max(0.0, p.rustBand);
    Fine F;
    BuildFine(C, paintT - band, F);
    if (F.tris.empty()) return;

                                                                             
                                                                    
    {
        Patch Z = ClipBand(F, paintT, std::numeric_limits<double>::infinity());
        std::vector<double> top(Z.P.size(), 2.0 * t + 20.0 * C.eps), bot(Z.P.size());
        for (size_t i = 0; i < Z.P.size(); ++i) bot[i] = std::max(20.0 * C.eps, std::min(t, 0.3 * Z.W[i]));
        for (auto& z : PatchSlabs(Z, top, bot)) cuts.push_back(std::move(z));
    }
                                                          
    if (p.rustPaintFlakes > 0.0) {
        const std::vector<Seg> front = Contour(F, paintT);
        const double size = std::max(6.0 * t, p.rustFlakeSize * 0.6);
        const double spacing = size * 0.9 / std::max(0.05, Clamp01(p.rustPaintFlakes));
        double carry = 0.5 * spacing;
        int site = 0;
        for (const Seg& S : front) {
            const double len = m::la::length(S.b - S.a);
            double at = carry;
            for (; at < len && site < 4000; at += spacing, ++site) {
                auto rnd = [&](uint64_t ch) { return Random01(C.seed, uint64_t(site), 3000 + ch); };
                const m::vec3 base = S.a + (S.b - S.a) * (at / len);
                const m::vec3 side = m::la::cross(S.n, S.inward);
                const double skew = (rnd(0) - 0.5) * 0.6;
                const m::vec3 d = Unit(S.inward * std::cos(skew) + side * std::sin(skew), S.inward);
                const double length = size * Lerp(0.3, 1.0, rnd(1) * rnd(1));
                const double width = size * Lerp(0.3, 0.7, rnd(2));
                const double anchorLen = std::max(3.0 * t, 0.15 * width);
                const m::vec3 anchor = base - d * (0.6 * anchorLen);
                if (C.Noisy(C.Base(anchor), anchor) >= paintT) continue;
                bool ok;
                const double h0 = C.Height(base, S.n, 4.0 * t + 1.0, ok);
                const double roll = Lerp(0.15, 1.0, rnd(3) * rnd(3)) * m::kPi;
                const double rc = std::max(2.5 * t, length / std::max(0.15, roll));
                const m::vec3 noise(rnd(4) * 1000, rnd(5) * 1000, rnd(6) * 1000);
                                                                               
                                                      
                m::Manifold fl = BuildFlake(base + S.n * h0, d, S.n, anchorLen, length, width, rc,
                                            Lerp(0.05, 0.4, rnd(7)), t, -1.02 * t, noise);
                if (fl.Status() == m::Manifold::Error::NoError && fl.NumTri() > 0)
                    adds.push_back(std::move(fl));
            }
            carry = at - len;
        }
    }
    if (band <= 0.0) return;
    Patch B = ClipBand(F, paintT - band, paintT);
                                                                        
                                                     
    if (p.rustCraze > 0.0 && !B.T.empty()) {
        double bandArea = 0.0;
        for (const auto& tt : B.T)
            bandArea += 0.5 * m::la::length(m::la::cross(B.P[size_t(tt[1])] - B.P[size_t(tt[0])], B.P[size_t(tt[2])] - B.P[size_t(tt[0])]));
        const double cell = p.rustCrazeCell;
        const int want = std::min(4000, int(Clamp01(p.rustCraze) * bandArea / (cell * cell) * 1.3));
        std::vector<Sample> s = SamplePatch(B, want * 4, C.seed, 4000);
        struct SeedP { m::vec3 p, n; double c, k; };
        std::vector<SeedP> seeds;
        std::unordered_map<uint64_t, std::vector<int32_t>> hash;
        const double hcell = cell;
        auto key = [&](const m::vec3& x) {
            return (uint64_t(int64_t(std::floor(x.x / hcell)) & 0x1fffff) << 42)
                 | (uint64_t(int64_t(std::floor(x.y / hcell)) & 0x1fffff) << 21)
                 | uint64_t(int64_t(std::floor(x.z / hcell)) & 0x1fffff);
        };
        auto near = [&](const m::vec3& x, double radius, auto&& fn) {
            const int rr = int(std::ceil(radius / hcell));
            const int64_t cx = int64_t(std::floor(x.x / hcell)), cy = int64_t(std::floor(x.y / hcell)), cz = int64_t(std::floor(x.z / hcell));
            for (int64_t z = cz - rr; z <= cz + rr; ++z)
                for (int64_t y = cy - rr; y <= cy + rr; ++y)
                    for (int64_t xx = cx - rr; xx <= cx + rr; ++xx) {
                        const uint64_t k2 = (uint64_t(xx & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
                        auto it = hash.find(k2);
                        if (it != hash.end()) for (int32_t id : it->second) fn(id);
                    }
        };
        for (const Sample& q : s) {
            if (int(seeds.size()) >= want) break;
            const double k = Smoothstep(paintT - band, paintT, q.r);                        
            const double c = cell * Lerp(1.3, 0.6, k);
            bool clash = false;
            near(q.p, 1.3 * cell, [&](int32_t j) { if (m::la::length(seeds[size_t(j)].p - q.p) < 0.75 * std::min(c, seeds[size_t(j)].c)) clash = true; });
            if (clash) continue;
            hash[key(q.p)].push_back(int32_t(seeds.size()));
            seeds.push_back({q.p, q.n, c, k});
        }
        std::vector<std::vector<m::Manifold>> per(seeds.size());
        ProjectDamageParallelFor(0, int64_t(seeds.size()), [&](int64_t si) {
            const SeedP& Sd = seeds[size_t(si)];
            std::vector<int32_t> nb;
            near(Sd.p, 2.2 * cell, [&](int32_t k) { if (k != int32_t(si)) nb.push_back(k); });
            for (int32_t j : nb) {
                if (j < int32_t(si)) continue;
                const SeedP& J = seeds[size_t(j)];
                const double dij = m::la::length(J.p - Sd.p);
                if (dij > 1.9 * std::max(Sd.c, J.c) || dij < 1e-9) continue;
                const m::vec3 mid = (Sd.p + J.p) * 0.5;
                const m::vec3 n = Unit(Sd.n + J.n, Sd.n);
                const m::vec3 tm = Unit((J.p - Sd.p) - n * m::la::dot(J.p - Sd.p, n), m::vec3(1, 0, 0));
                const m::vec3 bm = m::la::cross(n, tm);
                auto to2 = [&](const m::vec3& x) { return m::vec2(m::la::dot(x - mid, tm), m::la::dot(x - mid, bm)); };
                const m::vec2 i2 = to2(Sd.p);
                double s0 = -1.5 * std::max(Sd.c, J.c), s1 = -s0;
                auto clipk = [&](int32_t k) {
                    if (k == int32_t(si) || k == j) return;
                    const m::vec2 k2 = to2(seeds[size_t(k)].p);
                    const double aa = 2.0 * (k2.y - i2.y), bb = m::la::dot(k2, k2) - m::la::dot(i2, i2);
                    if (std::abs(aa) < 1e-12) { if (bb < 0) s1 = s0 - 1; return; }
                    if (aa > 0) s1 = std::min(s1, bb / aa); else s0 = std::max(s0, bb / aa);
                };
                for (int32_t k : nb) clipk(k);
                near(J.p, 2.2 * cell, [&](int32_t k) { clipk(k); });
                if (s1 - s0 < 0.3 * t) continue;
                const double k = 0.5 * (Sd.k + J.k);
                const double width = t * Lerp(0.5, 1.4, k);
                m::Manifold gr = Groove(C, mid + bm * s0, mid + bm * s1, n, width, t, 2.0 * t);
                if (gr.Status() == m::Manifold::Error::NoError && gr.NumTri() > 0)
                    per[size_t(si)].push_back(std::move(gr));
            }
        });
        for (auto& list : per) for (auto& g : list) cuts.push_back(std::move(g));
    }
                                                                               
    if (p.rustBlisters > 0 && !B.T.empty()) {
        const m::MeshGL unit = m::Manifold::Sphere(1.0, 16).GetMeshGL();
        std::vector<Sample> s = SamplePatch(B, p.rustBlisters, C.seed, 5000);
        std::vector<m::Manifold> slots(s.size());
        ProjectDamageParallelFor(0, int64_t(s.size()), [&](int64_t i) {
            const double k = Smoothstep(paintT - band, paintT, s[size_t(i)].r);
            if (Random01(C.seed, uint64_t(i), 5100) > 0.25 + 0.75 * k) return;
            const double r = p.rustBlisterSize * std::pow(2.2, Random01(C.seed, uint64_t(i), 5101) - 0.5) * (0.6 + 0.6 * k);
            const double h = r * Lerp(0.25, 0.45, Random01(C.seed, uint64_t(i), 5102));
                                                                                
            m::Manifold d = Dome(C, unit, s[size_t(i)].p, s[size_t(i)].n, r, h, 0.0, t);
            if (d.Status() == m::Manifold::Error::NoError && d.NumTri() > 0) slots[size_t(i)] = std::move(d);
        });
        for (auto& d : slots) if (d.NumTri() > 0) adds.push_back(std::move(d));
    }
}

void EvaluateRustAttributes(const CoreInput& in, const ProjectDamageParams& p, const RustField& field,
                            const std::vector<EVec>& points, std::vector<float>& rust,
                            std::vector<float>& streak)
{
    rust.assign(points.size(), 0.0f);
    streak.assign(points.size(), 0.0f);
    if (!field.valid || points.empty()) return;
    RustCtx C;
    C.Init(in, p, field);
    if (C.tris.empty()) return;
    const m::vec3 up = -C.g;
    m::vec3 e1, e2;
    Frame(up, e1, e2);
    const int K = 6;
    const double step = p.rustStreakLength / K;
    const double strength = Clamp01(p.rustStreak);
    const double start = std::min(p.rustPaintStart, p.rustCrustStart);
    ProjectDamageParallelFor(0, int64_t(points.size()), [&](int64_t i) {
        const m::vec3 x = points[size_t(i)];
        const double r = C.Noisy(C.Base(x), x);
        rust[size_t(i)] = float(std::max(0.0, std::min(1.0, r)));
        if (strength <= 0.0 || step <= 0.0) return;
        double best = 0.0;
        for (int k = 1; k <= K; ++k) {
            double dist;
            m::vec3 c;
            const m::vec3 q = x + up * (k * step);
            const double b = C.Base(q, &dist, &c);
            if (dist > 1.5 * step) break;                                    
            const double rq = C.Noisy(b, c);
            best = std::max(best, Smoothstep(start, start + 0.3, rq) * (1.0 - double(k - 1) / K));
        }
                                                       
        const m::vec3 hq(m::la::dot(x, e1) * 1.6 / C.scale, m::la::dot(x, e2) * 1.6 / C.scale,
                         m::la::dot(x, up) * 0.08 / C.scale);
                                                           
        const double stripes = 0.35 + 0.65 * Smoothstep(-0.35, 0.35, Fbm(hq + C.off[1], 3));
        streak[size_t(i)] = float(std::max(0.0, std::min(1.0, best * stripes * strength)));
    });
}
PROJECT_DAMAGE_NS_MACRO_END
