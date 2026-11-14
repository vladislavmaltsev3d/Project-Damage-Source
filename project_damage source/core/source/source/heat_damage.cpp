// изменяем поверхность для следов нагрева
// ProjectDamage heat damage and charring — GPL-3.0-or-later; see LICENSE.
//
// Recipe: heat-damage-v1. A procedural appearance model, not combustion or
// pyrolysis physics.
//
// Exposure field
//   q_i  = intensity * w_i, w = artist-painted vertex group (1 when absent).
//   Without diffusion: E = clamp(q, 0, 1).
//   With diffusion, the surface heat equation with source and linear loss,
//       M_A du/dt = -k L u - h M_A u + M_A q,      k = spread^2, h = 1,
//   where M_A is the lumped (barycentric) area mass matrix and L the cotangent
//   stiffness matrix (symmetric positive semidefinite), integrated with N
//   backward-Euler steps over duration T (dt = T/N), from u_0 = 0:
//       (M_A + dt (k L + h M_A)) u_{n+1} = M_A (u_n + dt q).
//   The system matrix is SPD (M_A > 0, L >= 0), solved by Jacobi-PCG.
//   E = clamp(u_N / (1 - (1 + dt)^-N), 0, 1): the normaliser is the exact
//   discrete response of a uniformly heated region, so a large painted area
//   reaches E = q while heat spreads ~spread around it. T -> infinity gives
//   the steady screened-Poisson field (k L + M_A) u = M_A q.
//   (If a different source/loss form is intended, only SolveHeat changes.)
//
// Damage, all driven by the char amount g(E) = smoothstep(char start, 1, E):
//   1. shrinkage: one closed cutter over the heated faces, subdivided to Heat
//      Detail, whose floor sinks by depth * g * blotch noise, deeper toward
//      char-cell borders (rounded char blocks); capped by local thickness;
//   2. char checking cracks: Poisson-disc cell seeds (cell size shrinks with
//      g), local Voronoi edges clipped in the tangent plane, cut as
//      surface-conforming V grooves whose width/depth follow g along the crack;
//   3. blisters: hollow domes in a mid-exposure band, some burst open;
//   4. attributes: exposure, char amount, char layer depth per result point.
#include "manifold/manifold.h"
#include "heat_damage.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
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

                                                                               

struct Csr {
    std::vector<int64_t> start;
    std::vector<int32_t> col;
    std::vector<double> val;
    std::vector<int64_t> diag;                                             

    void Mul(const std::vector<double>& x, std::vector<double>& y) const
    {
        const int64_t n = int64_t(start.size()) - 1;
        y.assign(size_t(n), 0.0);
        for (int64_t i = 0; i < n; ++i) {
            double s = 0.0;
            for (int64_t k = start[size_t(i)]; k < start[size_t(i) + 1]; ++k)
                s += val[size_t(k)] * x[size_t(col[size_t(k)])];
            y[size_t(i)] = s;
        }
    }
};

                                                                            
                                                                          
// основной вход: собираем геометрию по переданным настройкам
void BuildLaplacian(const MeshView& v, const std::vector<Tri>& tris, Csr& L, std::vector<double>& mass)
{
    const size_t n = size_t(v.npts);
    mass.assign(n, 0.0);
    struct Trip { int32_t r, c; double w; };
    std::vector<Trip> trips;
    trips.reserve(tris.size() * 12 + n);
    for (size_t i = 0; i < n; ++i) trips.push_back({int32_t(i), int32_t(i), 0.0});
    for (const Tri& t : tris) {
        const int32_t id[3] = {t.a, t.b, t.c};
        const double area = TriArea(v, t);
        for (int k = 0; k < 3; ++k) mass[size_t(id[k])] += area / 3.0;
        if (area <= 1e-18) continue;
        for (int k = 0; k < 3; ++k) {
            const int32_t o = id[k], i = id[(k + 1) % 3], j = id[(k + 2) % 3];
            const m::vec3 e1 = v.pts[i] - v.pts[o], e2 = v.pts[j] - v.pts[o];
            const double cot = m::la::dot(e1, e2) / (2.0 * area);                        
            const double w = 0.5 * cot;
            trips.push_back({i, j, -w}); trips.push_back({j, i, -w});
            trips.push_back({i, i, w});  trips.push_back({j, j, w});
        }
    }
    std::sort(trips.begin(), trips.end(), [](const Trip& a, const Trip& b) {
        return a.r != b.r ? a.r < b.r : a.c < b.c;
    });
    L.start.assign(n + 1, 0);
    L.col.clear(); L.val.clear(); L.diag.assign(n, -1);
    size_t k = 0;
    for (size_t r = 0; r < n; ++r) {
        L.start[r] = int64_t(L.col.size());
        while (k < trips.size() && size_t(trips[k].r) == r) {
            const int32_t c = trips[k].c;
            double w = 0.0;
            while (k < trips.size() && size_t(trips[k].r) == r && trips[k].c == c) w += trips[k++].w;
            if (size_t(c) == r) L.diag[r] = int64_t(L.col.size());
            L.col.push_back(c);
            L.val.push_back(w);
        }
    }
    L.start[n] = int64_t(L.col.size());
}

bool SolvePcg(const Csr& A, const std::vector<double>& b, std::vector<double>& x)
{
    const size_t n = b.size();
    std::vector<double> r(n), z(n), p(n), Ap;
    A.Mul(x, Ap);
    double bn = 0.0;
    for (size_t i = 0; i < n; ++i) { r[i] = b[i] - Ap[i]; bn += b[i] * b[i]; }
    bn = std::sqrt(bn);
    if (bn <= 1e-300) { std::fill(x.begin(), x.end(), 0.0); return true; }
    auto precond = [&](const std::vector<double>& in, std::vector<double>& out) {
        for (size_t i = 0; i < n; ++i) {
            const double d = A.val[size_t(A.diag[i])];
            out[i] = d > 1e-300 ? in[i] / d : in[i];
        }
    };
    precond(r, z);
    p = z;
    double rz = 0.0;
    for (size_t i = 0; i < n; ++i) rz += r[i] * z[i];
    for (int it = 0; it < 4000; ++it) {
        A.Mul(p, Ap);
        double pAp = 0.0;
        for (size_t i = 0; i < n; ++i) pAp += p[i] * Ap[i];
        if (pAp <= 0.0) return false;
        const double alpha = rz / pAp;
        double rn = 0.0;
        for (size_t i = 0; i < n; ++i) { x[i] += alpha * p[i]; r[i] -= alpha * Ap[i]; rn += r[i] * r[i]; }
        if (std::sqrt(rn) <= 1e-10 * bn) return true;
        precond(r, z);
        double rz2 = 0.0;
        for (size_t i = 0; i < n; ++i) rz2 += r[i] * z[i];
        const double beta = rz2 / rz;
        rz = rz2;
        for (size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    }
    return true;                                         
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

                                 
std::vector<m::vec3> VertexNormals(const MeshView& v, const std::vector<Tri>& tris)
{
    std::vector<m::vec3> vn(size_t(v.npts), m::vec3(0.0));
    for (const Tri& t : tris) {
        const int32_t id[3] = {t.a, t.b, t.c};
        const m::vec3 fn = Unit(m::la::cross(v.pts[t.b] - v.pts[t.a], v.pts[t.c] - v.pts[t.a]), m::vec3(0.0));
        for (int k = 0; k < 3; ++k) {
            const m::vec3 a = Unit(v.pts[id[(k + 1) % 3]] - v.pts[id[k]], m::vec3(0.0));
            const m::vec3 b = Unit(v.pts[id[(k + 2) % 3]] - v.pts[id[k]], m::vec3(0.0));
            vn[size_t(id[k])] += fn * std::acos(std::max(-1.0, std::min(1.0, m::la::dot(a, b))));
        }
    }
    for (auto& n : vn) n = Unit(n, m::vec3(0, 0, 1));
    return vn;
}

                                   
struct Seed { m::vec3 pos, n; double cell, g; };

class SeedHash {
public:
    explicit SeedHash(double h) : h_(std::max(1e-9, h)) {}
    void Add(int32_t id, const m::vec3& p) { map_[Key(p)].push_back(id); }
    template <class F> void Near(const m::vec3& p, double radius, F&& fn) const
    {
        const int r = int(std::ceil(radius / h_));
        const int64_t cx = int64_t(std::floor(p.x / h_)), cy = int64_t(std::floor(p.y / h_)), cz = int64_t(std::floor(p.z / h_));
        for (int64_t z = cz - r; z <= cz + r; ++z)
            for (int64_t y = cy - r; y <= cy + r; ++y)
                for (int64_t x = cx - r; x <= cx + r; ++x) {
                    auto it = map_.find(Pack(x, y, z));
                    if (it == map_.end()) continue;
                    for (int32_t id : it->second) fn(id);
                }
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
    double h_;
    std::unordered_map<uint64_t, std::vector<int32_t>> map_;
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

struct Context {
    const CoreInput* in = nullptr;
    const ProjectDamageParams* p = nullptr;
    const HeatField* heat = nullptr;
    std::vector<Tri> tris;
    Locator loc;
    ProjectDamageBVH bvh;
    double eps = 1e-5;

    double ExposureAt(const m::vec3& x, int32_t* triOut = nullptr) const
    {
        int32_t t; double u, v, w;
        if (!loc.Query(x, t, u, v, w)) return 0.0;
        if (triOut) *triOut = t;
        const Tri& tr = tris[size_t(t)];
        return heat->E[size_t(tr.a)] * u + heat->E[size_t(tr.b)] * v + heat->E[size_t(tr.c)] * w;
    }

                                                                               
    double Height(const m::vec3& x, const m::vec3& n, double probe, bool& ok) const
    {
        double dist = 0.0;
        ok = bvh.Raycast(x + n * probe, -n, 2.0 * probe, &dist);
        return ok ? probe - dist : 0.0;
    }
};

                                                                        
                                                                        
m::Manifold BuildGroove(const Context& ctx, const m::vec3& a, const m::vec3& b, const m::vec3& nIn,
                        double width, double depth, double up)
{
    const ProjectDamageParams& p = *ctx.p;
    const m::vec3 n = Unit(nIn, m::vec3(0, 0, 1));
    const m::vec3 seg = (b - a) - n * m::la::dot(b - a, n);
    const double len = m::la::length(seg);
    if (len < 1e-9) return m::Manifold();
    const m::vec3 t = seg / len, e = m::la::cross(n, t);
    const double ext = 0.5 * width;                                                 
    const double total = len + 2.0 * ext;
    const int ns = std::max(4, std::min(28, int(std::ceil(total / std::max(1e-6, 0.5 * width)))));
    constexpr int nw = 4;
    const double probe = std::max(2.0 * width, depth) + up;
    std::vector<m::vec3> top, bot;
    top.reserve(size_t((ns + 1) * (nw + 1))); bot.reserve(top.capacity());
    bool any = false;
    for (int i = 0; i <= ns; ++i) {
        const double s = -ext + total * i / ns;
        const m::vec3 c = a + t * s;
        const double g = HeatCharAmount(p, ctx.ExposureAt(c));
                                                                   
        const double endT = std::min(1.0, std::min((s + ext) / ext, (len + ext - s) / ext));
        double localDepth = depth * g * Smoothstep(0.0, 1.0, endT);
        const double localWidth = width * (0.3 + 0.9 * g);                                           
        if (localDepth > 1e-6) any = true;
                                                                          
        {
            bool ok;
            const double h0 = ctx.Height(c, n, probe, ok);
            double dist = 0.0;
            if (ok && ctx.bvh.Raycast(c + n * (h0 - ctx.eps), -n, 1e6, &dist))
                localDepth = std::min(localDepth, 0.4 * dist);
        }
        for (int j = 0; j <= nw; ++j) {
            const double y = (double(j) / nw - 0.5) * localWidth;
            const m::vec3 x = c + e * y;
            bool ok;
            const double h = ctx.Height(x, n, probe, ok);
            const double lateral = 1.0 - std::pow(2.0 * y / std::max(1e-9, localWidth), 2.0);
                                                                               
                                                                         
                                                                            
            const double rim = 0.05 * std::max(1e-6, depth) + 10.0 * ctx.eps;
            const double on = 2.0 * Smoothstep(0.0, 0.1 * std::max(1e-9, depth), localDepth) - 1.0;
            const double dz = localDepth * std::sqrt(std::max(0.0, lateral)) + rim * on;
            top.push_back(x + n * (h + up));
            bot.push_back(x + n * (h - dz));
        }
    }
    if (!any) return m::Manifold();
    return StripSolid(top, bot, ns, nw);
}

                                                                    
                                                                          
m::Manifold BuildDome(const Context& ctx, const m::MeshGL& unit, const m::vec3& pos, const m::vec3& n,
                      const m::vec3& t, const m::vec3& b, double r, double h, double k, double root, double lift)
{
    m::MeshGL mesh = unit;
    const double probe = 2.0 * r + h;
    for (size_t j = 0; j < mesh.NumVert(); ++j) {
        const size_t idx = j * mesh.numProp;
        const double qx = mesh.vertProperties[idx], qy = mesh.vertProperties[idx + 1], qz = mesh.vertProperties[idx + 2];
        const double rho = std::sqrt(std::max(0.0, 1.0 - std::abs(qz)));
        const double th = (qx * qx + qy * qy) > 1e-24 ? std::atan2(qy, qx) : 0.0;
        const m::vec3 x = pos + t * (rho * std::cos(th) * r) + b * (rho * std::sin(th) * r);
        bool ok;
        const double s = ctx.Height(x, n, probe, ok);
        const double prof = std::pow(std::max(0.0, 1.0 - rho * rho), k);
        const double sink = 0.08 * h;
        const double z = qz >= 0.0 ? s + lift - sink + (h + sink) * prof
                                   : s + lift - sink - root * std::sqrt(std::max(0.0, 1.0 - rho * rho));
        const m::vec3 w = x + n * z;
        mesh.vertProperties[idx] = float(w.x);
        mesh.vertProperties[idx + 1] = float(w.y);
        mesh.vertProperties[idx + 2] = float(w.z);
    }
    return m::Manifold(mesh);
}
}

double HeatCharAmount(const ProjectDamageParams& p, double e)
{
    return Smoothstep(Clamp01(p.heatCharStart), 1.0, e);
}

HeatField ComputeHeatField(const CoreInput& in, const ProjectDamageParams& p)
{
    HeatField H;
    if (!p.heatEnable) return H;
    const double vals[] = {p.heatIntensity, p.heatSpread, p.heatDuration, p.heatCharStart,
        p.heatShrinkDepth, p.heatDetail, p.heatBlockRound, p.heatCellMin, p.heatCellMax,
        p.heatCrackWidth, p.heatCrackDepth, p.heatBlisterMin, p.heatBlisterMax,
        p.heatBlisterLow, p.heatBlisterHigh, p.heatBlisterBurst, p.heatCharLayer};
    for (double x : vals) if (!std::isfinite(x))
        throw std::invalid_argument("heat: parameters must be finite");
    if (p.heatSpread < 0 || p.heatDuration <= 0 || p.heatSteps < 1 || p.heatSteps > 64
        || p.heatDetail <= 0 || p.heatCellMin <= 0 || p.heatCellMax < p.heatCellMin
        || p.heatCrackWidth < 0 || p.heatCrackDepth < 0 || p.heatShrinkDepth < 0
        || p.heatBlisterMin <= 0 || p.heatBlisterMax < p.heatBlisterMin
        || p.heatCrackCells < 0 || p.heatCrackCells > 5000 || p.heatBlisters < 0 || p.heatBlisters > 2000)
        throw std::invalid_argument("heat: invalid ranges (sizes > 0, ordered min/max, steps 1..64, cells <= 5000, blisters <= 2000)");
    const MeshView& v = in.view;
    const size_t n = size_t(v.npts);
    if (in.heatWeights && in.heatWeights->size() != n)
        throw std::invalid_argument("heat: weights must have one entry per point");
    std::vector<double> q(n);
    for (size_t i = 0; i < n; ++i)
        q[i] = std::max(0.0, p.heatIntensity) * (in.heatWeights ? Clamp01((*in.heatWeights)[i]) : 1.0);
    H.E.assign(n, 0.0);
    if (!p.heatDiffuse || p.heatSpread <= 0.0) {
        for (size_t i = 0; i < n; ++i) H.E[i] = Clamp01(q[i]);
        H.valid = true;
        return H;
    }
    const std::vector<Tri> tris = Triangulate(v);
    Csr A;
    std::vector<double> mass;
    BuildLaplacian(v, tris, A, mass);
    double avgMass = 0.0;
    for (double x : mass) avgMass += x;
    avgMass = std::max(1e-300, avgMass / double(std::max<size_t>(1, n)));
    for (double& x : mass) x = std::max(x, 1e-9 * avgMass);
    const double k = p.heatSpread * p.heatSpread, dt = p.heatDuration / p.heatSteps;
    for (double& x : A.val) x *= dt * k;                                
    for (size_t i = 0; i < n; ++i) A.val[size_t(A.diag[i])] += (1.0 + dt) * mass[i];                    
    std::vector<double> u(n, 0.0), rhs(n);
    for (int s = 0; s < p.heatSteps; ++s) {
        for (size_t i = 0; i < n; ++i) rhs[i] = mass[i] * (u[i] + dt * q[i]);
        SolvePcg(A, rhs, u);                         
    }
    const double norm = 1.0 - std::pow(1.0 + dt, -double(p.heatSteps));
    for (size_t i = 0; i < n; ++i) H.E[i] = Clamp01(u[i] / std::max(1e-12, norm));
    H.valid = true;
    return H;
}

void PlaceHeatDamage(const CoreInput& in, const ProjectDamageParams& p, const std::vector<EVec>& normals,
                     const HeatField& heat, std::vector<m::Manifold>& cutters,
                     std::vector<m::Manifold>& blisters)
{
    if (!heat.valid) return;
    const MeshView& v = in.view;
    Context ctx;
    ctx.in = &in; ctx.p = &p; ctx.heat = &heat;
    ctx.tris = Triangulate(v);
    if (ctx.tris.empty()) return;
    ctx.loc.Build(v, ctx.tris);
    ctx.bvh.Build(v);
    ctx.eps = std::max(1e-5, 1e-7 * m::la::length(in.bbRad));
    const uint64_t seed = (uint64_t(uint32_t(p.seed)) ^ 0x484541544441474dULL)                 
        ^ (uint64_t(uint32_t(p.heatSeed)) * 0x9e3779b97f4a7c15ULL);
    const auto avoided = [&](int32_t poly) { return p.avoidSurf && in.avoidPoly && (*in.avoidPoly)[size_t(poly)]; };

    std::vector<double> g(size_t(v.npts));
    for (size_t i = 0; i < g.size(); ++i) g[i] = HeatCharAmount(p, heat.E[i]);
    const std::vector<m::vec3> vn = VertexNormals(v, ctx.tris);
    auto triNormal = [&](const Tri& t) {
        return Unit(m::la::cross(v.pts[t.b] - v.pts[t.a], v.pts[t.c] - v.pts[t.a]), m::vec3(0, 0, 1));
    };

                                                                                 
    std::vector<Seed> seeds;
    const double cellMax = p.heatCellMax, cellMin = p.heatCellMin;
    SeedHash hash(cellMax);
    if (p.heatCrackCells > 0) {
        std::vector<double> cdf;
        double area = 0.0;
        for (const Tri& t : ctx.tris) {
            const double gm = std::max(g[size_t(t.a)], std::max(g[size_t(t.b)], g[size_t(t.c)]));
            area += (gm > 0.0 && !avoided(t.poly)) ? TriArea(v, t) : 0.0;
            cdf.push_back(area);
        }
        const int tries = p.heatCrackCells * 8;
        for (int i = 0; i < tries && area > 0.0 && int(seeds.size()) < p.heatCrackCells; ++i) {
            auto rnd = [&](uint64_t ch) { return Random01(seed, uint64_t(i), ch); };
            const size_t ti = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
            if (ti >= ctx.tris.size()) continue;
            const Tri& t = ctx.tris[ti];
            double r1 = rnd(1), r2 = rnd(2);
            if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
            const double w0 = 1.0 - r1 - r2;
            const m::vec3 pos = v.pts[t.a] * w0 + v.pts[t.b] * r1 + v.pts[t.c] * r2;
            const double gs = g[size_t(t.a)] * w0 + g[size_t(t.b)] * r1 + g[size_t(t.c)] * r2;
            if (gs <= 0.0 || rnd(3) >= 0.35 + 0.65 * gs) continue;
            const double cell = Lerp(cellMax, cellMin, gs) * (0.85 + 0.3 * rnd(4));
            bool clash = false;
            hash.Near(pos, cellMax, [&](int32_t k) {
                const Seed& o = seeds[size_t(k)];
                if (m::la::length(o.pos - pos) < 0.75 * std::min(cell, o.cell)) clash = true;
            });
            if (clash) continue;
            hash.Add(int32_t(seeds.size()), pos);
            seeds.push_back({pos, triNormal(t), cell, gs});
        }
    }

                                                                                 
    const double crackDepthTotal = p.heatCrackDepth + p.heatShrinkDepth;
    if (p.heatShrinkDepth > 0.0) {
        std::vector<char> inRegion(ctx.tris.size(), 0);
        double maxEdge = 0.0;
        for (size_t ti = 0; ti < ctx.tris.size(); ++ti) {
            const Tri& t = ctx.tris[ti];
            if (avoided(t.poly)) continue;
            if (std::max(g[size_t(t.a)], std::max(g[size_t(t.b)], g[size_t(t.c)])) <= 0.0) continue;
            inRegion[ti] = 1;
            maxEdge = std::max(maxEdge, std::max(m::la::length(v.pts[t.b] - v.pts[t.a]),
                std::max(m::la::length(v.pts[t.c] - v.pts[t.b]), m::la::length(v.pts[t.a] - v.pts[t.c]))));
        }
        const int sub = std::max(1, std::min(16, int(std::ceil(maxEdge / p.heatDetail))));
                                                                          
        std::vector<double> thick(size_t(v.npts), 1e30);
        ProjectDamageParallelFor(0, int64_t(v.npts), [&](int64_t i) {
            double dist = 0.0;
            if (ctx.bvh.Raycast(v.pts[i] - vn[size_t(i)] * ctx.eps, -vn[size_t(i)], 1e6, &dist))
                thick[size_t(i)] = dist;
        });
        const double delta = std::max(10.0 * ctx.eps, 0.03 * p.heatShrinkDepth);
        const double outerLift = 2.5 * delta;
        const m::vec3 noiseOff(Random01(seed, 0, 200) * 1000, Random01(seed, 0, 201) * 1000, Random01(seed, 0, 202) * 1000);

        std::unordered_map<uint64_t, int32_t> keyToVert;
        std::vector<m::vec3> outer, inner;
        auto edgeKey = [&](int32_t a, int32_t b, int k) {
                                                                      
            if (a > b) { std::swap(a, b); k = sub - k; }
            return (uint64_t(1) << 63) | (uint64_t(uint32_t(a)) << 37) | (uint64_t(uint32_t(b)) << 6) | uint64_t(k);
        };
        auto cornerKey = [](int32_t a) { return (uint64_t(1) << 62) | uint64_t(uint32_t(a)); };
        auto makeVert = [&](const Tri& t, double w0, double w1, double w2) {
            const m::vec3 pos = v.pts[t.a] * w0 + v.pts[t.b] * w1 + v.pts[t.c] * w2;
            const m::vec3 n = Unit(vn[size_t(t.a)] * w0 + vn[size_t(t.b)] * w1 + vn[size_t(t.c)] * w2, triNormal(t));
            const double gs = g[size_t(t.a)] * w0 + g[size_t(t.b)] * w1 + g[size_t(t.c)] * w2;
            const double th = thick[size_t(t.a)] * w0 + thick[size_t(t.b)] * w1 + thick[size_t(t.c)] * w2;
            double blot = 0.75 + 0.5 * (0.5 + 0.5 * Fbm(pos / std::max(1e-6, 2.0 * cellMax) + noiseOff, 3));
            if (!seeds.empty() && p.heatBlockRound > 0.0) {
                double d1 = 1e30, d2 = 1e30;
                hash.Near(pos, 2.0 * cellMax, [&](int32_t k) {
                    const double d = m::la::length(seeds[size_t(k)].pos - pos);
                    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
                });
                if (d2 < 1e29) {
                    const double edgeness = 1.0 - (d2 - d1) / (d2 + d1 + 1e-12);                        
                                                                                   
                    blot *= std::max(0.15, 1.0 + Clamp01(p.heatBlockRound) * 2.2 * (edgeness - 0.4));
                }
            }
            double d = p.heatShrinkDepth * gs * blot;
            d = std::min(d, 0.35 * th) - delta;
            outer.push_back(pos + n * outerLift);
            inner.push_back(pos - n * d);
            return int32_t(outer.size() - 1);
        };
        auto vertexAt = [&](const Tri& t, int i, int j) {
                                                                           
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
            const double w1 = double(i) / sub, w2 = double(j) / sub, w0 = 1.0 - w1 - w2;
            if (!shared) return makeVert(t, w0, w1, w2);
            auto it = keyToVert.find(key);
            if (it != keyToVert.end()) return it->second;
            const int32_t id = makeVert(t, w0, w1, w2);
            keyToVert.emplace(key, id);
            return id;
        };
        std::vector<int32_t> faces;                                            
        std::unordered_map<uint64_t, int> edgeUse;
        auto ek = [](int32_t a, int32_t b) {
            if (a > b) std::swap(a, b);
            return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
        };
        std::vector<size_t> regionTris;
        for (size_t ti = 0; ti < ctx.tris.size(); ++ti) {
            if (!inRegion[ti]) continue;
            regionTris.push_back(ti);
            const Tri& t = ctx.tris[ti];
            ++edgeUse[ek(t.a, t.b)]; ++edgeUse[ek(t.b, t.c)]; ++edgeUse[ek(t.c, t.a)];
            std::vector<int32_t> row((size_t(sub) + 1) * (size_t(sub) + 1), -1);
            auto at = [&](int i, int j) -> int32_t& { return row[size_t(i) * (sub + 1) + j]; };
            for (int i = 0; i <= sub; ++i)
                for (int j = 0; i + j <= sub; ++j) at(i, j) = vertexAt(t, i, j);
            for (int i = 0; i < sub; ++i)
                for (int j = 0; i + j < sub; ++j) {
                    faces.insert(faces.end(), {at(i, j), at(i + 1, j), at(i, j + 1)});
                    if (i + j + 1 < sub)
                        faces.insert(faces.end(), {at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
                }
        }
        if (!faces.empty()) {
            m::MeshGL mesh;
            mesh.numProp = 3;
            for (size_t i = 0; i < outer.size(); ++i)
                for (const m::vec3& q : {outer[i], inner[i]})
                    for (int c = 0; c < 3; ++c) mesh.vertProperties.push_back(float(q[c]));
            auto O = [](int32_t i) { return uint32_t(2 * i); };
            auto I = [](int32_t i) { return uint32_t(2 * i + 1); };
            for (size_t f = 0; f < faces.size(); f += 3) {
                mesh.triVerts.insert(mesh.triVerts.end(), {O(faces[f]), O(faces[f + 1]), O(faces[f + 2])});
                mesh.triVerts.insert(mesh.triVerts.end(), {I(faces[f]), I(faces[f + 2]), I(faces[f + 1])});
            }
                                                                              
                                                                                   
            for (size_t ti : regionTris) {
                const Tri& t = ctx.tris[ti];
                const int32_t c[3] = {t.a, t.b, t.c};
                for (int e = 0; e < 3; ++e) {
                    const int32_t a = c[e], b = c[(e + 1) % 3];
                    if (edgeUse[ek(a, b)] != 1) continue;
                    std::vector<int32_t> chain;
                    for (int s = 0; s <= sub; ++s) {
                        uint64_t key;
                        if (s == 0) key = cornerKey(a);
                        else if (s == sub) key = cornerKey(b);
                        else key = edgeKey(a, b, s);
                        chain.push_back(keyToVert.at(key));
                    }
                    for (int s = 0; s < sub; ++s) {
                        const int32_t pa = chain[size_t(s)], pb = chain[size_t(s) + 1];
                        mesh.triVerts.insert(mesh.triVerts.end(), {O(pa), I(pa), I(pb), O(pa), I(pb), O(pb)});
                    }
                }
            }
            m::Manifold shrink(mesh);
            if (shrink.Status() == m::Manifold::Error::NoError && shrink.NumTri() > 0)
                cutters.push_back(std::move(shrink));
        }
    }

                                                                                 
    if (seeds.size() >= 2 && crackDepthTotal > 0.0 && p.heatCrackWidth > 0.0) {
        std::vector<std::vector<m::Manifold>> perSeed(seeds.size());
        ProjectDamageParallelFor(0, int64_t(seeds.size()), [&](int64_t si) {
            const Seed& S = seeds[size_t(si)];
            std::vector<int32_t> nb;
            hash.Near(S.pos, 2.2 * cellMax, [&](int32_t k) { if (k != int32_t(si)) nb.push_back(k); });
            for (int32_t j : nb) {
                if (j < int32_t(si)) continue;
                const Seed& J = seeds[size_t(j)];
                const double dij = m::la::length(J.pos - S.pos);
                if (dij > 1.8 * std::max(S.cell, J.cell) || dij < 1e-9) continue;
                const m::vec3 mid = (S.pos + J.pos) * 0.5;
                const m::vec3 n = Unit(S.n + J.n, S.n);
                const m::vec3 tm = Unit((J.pos - S.pos) - n * m::la::dot(J.pos - S.pos, n), m::vec3(1, 0, 0));
                const m::vec3 bm = m::la::cross(n, tm);
                auto to2 = [&](const m::vec3& x) { return m::vec2(m::la::dot(x - mid, tm), m::la::dot(x - mid, bm)); };
                const m::vec2 i2 = to2(S.pos);
                const double reach = 1.5 * std::max(S.cell, J.cell);
                double s0 = -reach, s1 = reach;
                auto clip = [&](int32_t k) {
                    if (k == int32_t(si) || k == j) return;
                    const m::vec2 k2 = to2(seeds[size_t(k)].pos);
                    const double a = 2.0 * (k2.y - i2.y);
                    const double b = m::la::dot(k2, k2) - m::la::dot(i2, i2);                
                    if (std::abs(a) < 1e-12) { if (b < 0) s1 = s0 - 1; return; }
                    if (a > 0) s1 = std::min(s1, b / a); else s0 = std::max(s0, b / a);
                };
                for (int32_t k : nb) clip(k);
                hash.Near(J.pos, 2.2 * cellMax, [&](int32_t k) { clip(k); });
                if (s1 - s0 < 0.2 * p.heatCrackWidth) continue;
                const double gMid = 0.5 * (S.g + J.g);
                const m::vec3 a = mid + bm * s0, b = mid + bm * s1;
                m::Manifold groove = BuildGroove(ctx, a, b, n, p.heatCrackWidth * (0.6 + 0.4 * gMid),
                                                 crackDepthTotal, 2.0 * p.heatCrackWidth + 0.5 * crackDepthTotal);
                if (groove.Status() == m::Manifold::Error::NoError && groove.NumTri() > 0)
                    perSeed[size_t(si)].push_back(std::move(groove));
            }
        });
        for (auto& list : perSeed) for (auto& c : list) cutters.push_back(std::move(c));
    }

                                                                                 
    if (p.heatBlisters > 0) {
        std::vector<double> cdf;
        double area = 0.0;
        for (const Tri& t : ctx.tris) {
            area += avoided(t.poly) ? 0.0 : TriArea(v, t);
            cdf.push_back(area);
        }
        const m::MeshGL unit = m::Manifold::Sphere(1.0, 20).GetMeshGL();
        const double lo = Clamp01(p.heatBlisterLow), hi = std::max(lo + 1e-3, Clamp01(p.heatBlisterHigh));
        const double midBand = 0.5 * (lo + hi);
        std::vector<m::Manifold> slots(size_t(p.heatBlisters));
        ProjectDamageParallelFor(0, int64_t(p.heatBlisters), [&](int64_t bi) {
            auto rnd = [&](uint64_t ch) { return Random01(seed ^ 0xb115735ULL, uint64_t(bi), 100 + ch); };
            if (area <= 0.0) return;
            const size_t ti = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
            if (ti >= ctx.tris.size()) return;
            const Tri& t = ctx.tris[ti];
            double r1 = rnd(1), r2 = rnd(2);
            if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
            const double w0 = 1.0 - r1 - r2;
            const m::vec3 pos = v.pts[t.a] * w0 + v.pts[t.b] * r1 + v.pts[t.c] * r2;
            const double E = heat.E[size_t(t.a)] * w0 + heat.E[size_t(t.b)] * r1 + heat.E[size_t(t.c)] * r2;
            const double bell = Smoothstep(lo, midBand, E) * (1.0 - Smoothstep(midBand, hi, E));
            if (rnd(3) >= bell) return;
            const m::vec3 n = triNormal(t);
            const m::vec3 axis = std::abs(n.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(0, 1, 0);
            const m::vec3 t0 = Unit(m::la::cross(axis, n), m::vec3(1, 0, 0));
            const m::vec3 b0 = m::la::cross(n, t0);
            const double r = p.heatBlisterMin * std::pow(p.heatBlisterMax / p.heatBlisterMin, std::pow(rnd(4), 1.5));
            const double h = r * Lerp(0.28, 0.42, rnd(5));
            double thickness = 1e30, dist = 0.0;
            if (ctx.bvh.Raycast(pos - n * ctx.eps, -n, 1e6, &dist)) thickness = dist;
            const double root = std::min(0.3 * h, 0.3 * thickness);
            m::Manifold outerDome = BuildDome(ctx, unit, pos, n, t0, b0, r, h, 0.6, root, 0.0);
                                                                           
                                                                  
            const double skin = std::max(0.12 * h, 20.0 * ctx.eps);
            m::Manifold innerDome = BuildDome(ctx, unit, pos, n, t0, b0, 0.8 * r, std::max(0.0, h - skin), 0.6, 2.0 * root + skin, 0.0);
            m::Manifold blister = outerDome - innerDome;
            if (rnd(6) < Clamp01(p.heatBlisterBurst)) {
                bool ok;
                const double s = ctx.Height(pos, n, 2.0 * r + h, ok);
                const double cr = r * Lerp(0.35, 0.6, rnd(7));
                blister = blister - m::Manifold::Sphere(cr, 20).Translate(pos + n * (s + h + 0.35 * cr));
            }
            if (blister.Status() == m::Manifold::Error::NoError && blister.NumTri() > 0)
                slots[size_t(bi)] = std::move(blister);
        });
        for (auto& b : slots) if (b.NumTri() > 0) blisters.push_back(std::move(b));
    }
}

void EvaluateHeatAttributes(const CoreInput& in, const ProjectDamageParams& p, const HeatField& heat,
                            const std::vector<EVec>& points, std::vector<float>& exposure,
                            std::vector<float>& charAmount, std::vector<float>& charDepth)
{
    exposure.assign(points.size(), 0.0f);
    charAmount.assign(points.size(), 0.0f);
    charDepth.assign(points.size(), 0.0f);
    if (!heat.valid || points.empty()) return;
    std::vector<Tri> tris = Triangulate(in.view);
    if (tris.empty()) return;
    Locator loc;
    loc.Build(in.view, tris);
    ProjectDamageParallelFor(0, int64_t(points.size()), [&](int64_t i) {
        int32_t t; double u, v, w;
        if (!loc.Query(points[size_t(i)], t, u, v, w)) return;
        const Tri& tr = tris[size_t(t)];
        const double e = heat.E[size_t(tr.a)] * u + heat.E[size_t(tr.b)] * v + heat.E[size_t(tr.c)] * w;
        const double g = HeatCharAmount(p, e);
        exposure[size_t(i)] = float(e);
        charAmount[size_t(i)] = float(g);
        charDepth[size_t(i)] = float(g * p.heatCharLayer);
    });
}
PROJECT_DAMAGE_NS_MACRO_END
