// строим следы пуль с формой, зависящей от материала
// ProjectDamage bullet impacts — GPL-3.0-or-later; see LICENSE.
//
// Recipe: bullet-impacts-v1. Procedural bullet hits on metal plate, wood and
// concrete / stone:
//   1. placement: automatic shots are area-weighted surface samples, most of
//      them grouped into bursts (Poisson-spaced around burst centres), the
//      rest scattered. Paint mode reads grease pencil strokes: closed strokes
//      are areas filled with shots, open strokes are burst lines, dots and
//      short strokes are single shots, a small drawn circle is one shot of
//      that radius. Every shot gets a frame at its surface point, a slant
//      (random, or from a fixed shot direction) that stretches its outline,
//      a hole radius, and the wall thickness under it from a ray. A wall
//      thinner than the shot's penetration is shot through, and the back
//      face gets an exit frame;
//   2. metal: a ragged prism punched through (or a blind dent), the plate
//      dished in around it, a burr crown ADDED on the entry side (a ring
//      solid with torn petals curling over the hole), flared petals ADDED on
//      the exit side, a paper-thin rust halo, and spatter dings;
//   3. wood: a bore prism, a crushed funnel whose fibre strips splinter out
//      along the grain with pointed ends, a torn-out exit (larger, deeper,
//      splintered along the grain) with loose slivers ADDED, and a split
//      along the grain through the hole;
//   4. concrete: a conical spall crater (height-field slab with a lobed,
//      ragged rim, a shallow skin terrace, a cone and a pitted floor) around
//      a deep bore, or just a bore pit, pock marks around it, hairline
//      cracks out of the spall, and a wider exit spall on the back;
//   5. every added solid has the holes of all nearby shots cut out of it
//      before it is returned, so merged holes stay open.
// Craters and halos use the height-field slab of concrete damage (one ray
// per grid column, misses filled from neighbours, outside columns floating
// above the surface). Holes, bores and pits are stacked-ring prisms, so their
// outlines are crisp, not grid-stepped.
//
// Channel map, per shot key: 0-9 shape keys, 10-12 noise offset, 20-29 metal,
// 30-39 wood, 40-49 concrete, 100 + k*8 pits, 200 + k*16 slivers, 400 + k*32
// cracks. Placement: candidate k on (seed ^ "AUTO") channels 0-7, burst c on
// channels 0-3 of key 0x10000 + c.
#include "manifold/manifold.h"
#include "bullet_impacts.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
struct V2 { double x = 0.0, y = 0.0; };

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

                                                  
double Gauss(double u1, double u2)
{
	return std::sqrt(-2.0 * std::log(std::max(1e-12, u1))) * std::cos(2.0 * m::kPi * u2);
}

V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Smooth(double e0, double e1, double x)
{
	if (e1 <= e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double Fbm(V q, int octaves)
{
	double sum = 0.0, amp = 1.0, norm = 0.0;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += amp;
		amp *= 0.5;
		q = q * 2.03 + V(17.13, -9.31, 5.77);
	}
	return sum / norm;
}

                                                                              
double RingNoise(double ang, double freq, const V& off, int octaves)
{
	return Fbm(V(std::cos(ang) * freq, std::sin(ang) * freq, 0.0) + off, octaves);
}

V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

                                                                       
m::Manifold Orient(m::MeshGL& mesh)
{
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	if (solid.Status() != m::Manifold::Error::NoError) return m::Manifold();
	return solid;
}

void PushV(m::MeshGL& mesh, const V& q)
{
	mesh.vertProperties.push_back(float(q.x));
	mesh.vertProperties.push_back(float(q.y));
	mesh.vertProperties.push_back(float(q.z));
}

void Quad(m::MeshGL& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
}

                                                                        
                                                                         
                                                         
m::Manifold SlabSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(size_t(ns + 1) * size_t(nw + 1) * 6);
	for (int i = 0; i <= ns; ++i)
		for (int j = 0; j <= nw; ++j) {
			PushV(mesh, bot[size_t(i * (nw + 1) + j)]);
			PushV(mesh, top[size_t(i * (nw + 1) + j)]);
		}
	auto B = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j)); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j) + 1); };
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			Quad(mesh, T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			Quad(mesh, B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
		}
	for (int i = 0; i < ns; ++i) {
		Quad(mesh, B(i, 0), B(i + 1, 0), T(i + 1, 0), T(i, 0));
		Quad(mesh, B(i, nw), T(i, nw), T(i + 1, nw), B(i + 1, nw));
	}
	for (int j = 0; j < nw; ++j) {
		Quad(mesh, B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
		Quad(mesh, B(ns, j), B(ns, j + 1), T(ns, j + 1), T(ns, j));
	}
	return Orient(mesh);
}

                                                                           
                                                                           
                                                                       
m::Manifold RingSolid(const std::vector<V>& top, const std::vector<V>& bot, int nr, int na)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (int i = 0; i <= nr; ++i)
		for (int j = 0; j < na; ++j) {
			PushV(mesh, bot[size_t(i * na + j)]);
			PushV(mesh, top[size_t(i * na + j)]);
		}
	auto B = [&](int i, int j) { return uint32_t(2 * (i * na + (j % na))); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * na + (j % na)) + 1); };
	for (int i = 0; i < nr; ++i)
		for (int j = 0; j < na; ++j) {
			Quad(mesh, T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			Quad(mesh, B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
		}
	for (int j = 0; j < na; ++j) {
		Quad(mesh, B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
		Quad(mesh, B(nr, j), B(nr, j + 1), T(nr, j + 1), T(nr, j));
	}
	return Orient(mesh);
}

                                                                            
                                                                           
                                                                        
                                                                            
                   
m::Manifold StackSolid(const V& c, const V& axisDown, const std::vector<V>& outline,
	const std::vector<std::pair<double, double>>& rings, double apex)
{
	const size_t n = outline.size();
	if (n < 3 || rings.size() < 2) return m::Manifold();
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (const auto& rg : rings)
		for (size_t i = 0; i < n; ++i) PushV(mesh, c + (outline[i] - c) * rg.first + axisDown * rg.second);
	const uint32_t topC = uint32_t(rings.size() * n);
	PushV(mesh, c + axisDown * rings.front().second);
	const uint32_t botC = topC + 1;
	PushV(mesh, c + axisDown * (rings.back().second + std::max(0.0, apex)));
	auto Id = [&](size_t k, size_t i) { return uint32_t(k * n + (i % n)); };
	for (size_t i = 0; i < n; ++i) {
		mesh.triVerts.insert(mesh.triVerts.end(), {topC, Id(0, i + 1), Id(0, i)});
		mesh.triVerts.insert(mesh.triVerts.end(), {botC, Id(rings.size() - 1, i), Id(rings.size() - 1, i + 1)});
		for (size_t k = 0; k + 1 < rings.size(); ++k) Quad(mesh, Id(k, i), Id(k, i + 1), Id(k + 1, i + 1), Id(k + 1, i));
	}
	return Orient(mesh);
}

                                                                           
                                                                           
m::Manifold SweepSolid(const std::vector<V>& c, const std::vector<V>& side, const std::vector<V>& up,
	const std::vector<double>& hw, const std::vector<double>& ht)
{
	const size_t n = c.size();
	if (n < 2) return m::Manifold();
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (size_t k = 0; k < n; ++k) {
		const V s = side[k] * hw[k], u = up[k] * ht[k];
		PushV(mesh, c[k] - s - u);
		PushV(mesh, c[k] + s - u);
		PushV(mesh, c[k] + s + u);
		PushV(mesh, c[k] - s + u);
	}
	auto Id = [&](size_t k, int q) { return uint32_t(k * 4 + size_t(q & 3)); };
	for (size_t k = 0; k + 1 < n; ++k)
		for (int q = 0; q < 4; ++q) Quad(mesh, Id(k, q), Id(k, q + 1), Id(k + 1, q + 1), Id(k + 1, q));
	Quad(mesh, Id(0, 3), Id(0, 2), Id(0, 1), Id(0, 0));
	Quad(mesh, Id(n - 1, 0), Id(n - 1, 1), Id(n - 1, 2), Id(n - 1, 3));
	return Orient(mesh);
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> cumArea;                                           
	ProjectDamageBVH bvh;
	double diag = 1.0;
	V lo, hi;
	const std::vector<bool>* avoid = nullptr;

	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}
	bool Ray(const V& P, const V& dir, double maxDist, double& d, int32_t& poly) const
	{
		poly = -1;
		return bvh.Raycast(P, dir, maxDist, &d, &poly) && poly >= 0;
	}
	                                                                        
	                                                                        
	bool NormalNear(const V& P, double snap, V& N, V& S) const
	{
		static const V dirs[13] = {
			V(1, 0, 0), V(0, 1, 0), V(0, 0, 1),
			V(1, 1, 0), V(1, -1, 0), V(1, 0, 1), V(1, 0, -1), V(0, 1, 1), V(0, 1, -1),
			V(1, 1, 1), V(1, 1, -1), V(1, -1, 1), V(-1, 1, 1)};
		double best = 1e300;
		bool ok = false;
		for (const V& d0 : dirs) {
			const V d = Unit(d0, V(0, 0, 1));
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				const V dir = d * double(sgn);
				double t = 0.0;
				int32_t poly = -1;
				if (!Ray(P - dir * snap, dir, 2.0 * snap, t, poly)) continue;
				const V& pn = polyN[size_t(poly)];
				const double graze = std::abs(m::la::dot(pn, dir));
				if (graze < 0.15) continue;
				const double score = std::abs(t - snap) / graze;
				if (score < best) { best = score; N = pn; S = P - dir * snap + dir * t; ok = true; }
			}
		}
		return ok && best <= 1.5 * snap;
	}
	                                                          
	bool Sample(double r0, double r1, double r2, V& P, int32_t& poly) const
	{
		if (cumArea.empty() || !(cumArea.back() > 0.0)) return false;
		const double target = r0 * cumArea.back();
		const size_t i = size_t(std::upper_bound(cumArea.begin(), cumArea.end(), target) - cumArea.begin());
		poly = int32_t(std::min(i, cumArea.size() - 1));
		const ProjectDamagePoly& f = v->polys[poly];
		V a = v->pts[f.a], b = v->pts[f.b], c = v->pts[f.c];
		if (f.c != f.d) {
			const double A1 = m::la::length(m::la::cross(b - a, c - a));
			const double A2 = m::la::length(m::la::cross(c - a, v->pts[f.d] - a));
			const double lo0 = poly > 0 ? cumArea[size_t(poly) - 1] : 0.0;
			const double fr = (target - lo0) / std::max(1e-300, cumArea[size_t(poly)] - lo0);
			if (fr * (A1 + A2) > A1) { b = c; c = v->pts[f.d]; }
		}
		const double s = std::sqrt(r1);
		P = a * (1.0 - s) + b * (s * (1.0 - r2)) + c * (s * r2);
		return true;
	}
};

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, bool useAvoid, Surf& s)
{
	const MeshView& v = in.view;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	s.cumArea.assign(size_t(v.npolys), 0.0);
	double acc = 0.0;
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		double area = m::la::length(cr);
		if (q.c != q.d) {
			const V c2 = m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
			area += m::la::length(c2);
			cr = cr + c2;
		}
		s.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
		if (useAvoid && in.avoidPoly && size_t(i) < in.avoidPoly->size() && (*in.avoidPoly)[size_t(i)]) area = 0.0;
		acc += 0.5 * area;
		s.cumArea[size_t(i)] = acc;
	}
	s.bvh.Build(v);
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.lo = lo;
	s.hi = hi;
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                                      

                                                                              
                                                                            
                                                                        
                                                                        
struct Site {
	V o, n, t, b;
	double r = 1.0;
	double ca = 1.0, sa = 0.0, e = 1.0;
	double gx = 1.0, gy = 0.0;                                            
	double Hs = 1.0, drop = 2.0;                                                  
};

V World(const Site& F, double u, double v, double h) { return F.o + F.t * u + F.b * v + F.n * h; }

                                                             
V2 ToF(const Site& F, double x, double y, double scale)
{
	double a = F.ca * x + F.sa * y;
	const double b = -F.sa * x + F.ca * y;
	a *= F.e;
	const double k = scale * F.r;
	return {k * (F.ca * a - F.sa * b), k * (F.sa * a + F.ca * b)};
}

                                                       
V2 ToN(const Site& F, double u, double v, double scale)
{
	const double k = 1.0 / (scale * F.r);
	double a = (F.ca * u + F.sa * v) * k;
	const double b = (-F.sa * u + F.ca * v) * k;
	a /= F.e;
	return {F.ca * a - F.sa * b, F.sa * a + F.ca * b};
}

                                                                              
bool Height(const Site& F, const Surf& s, double u, double v, double& h, int32_t& poly)
{
	double dist = 0.0;
	const V org = World(F, u, v, F.Hs);
	if (!s.Ray(org, -F.n, F.Hs + F.drop, dist, poly)) return false;
	if (m::la::dot(s.polyN[size_t(poly)], F.n) < 0.05) return false;
	h = F.Hs - dist;
	return true;
}

                                                            
double HeightOr0(const Site& F, const Surf& s, double u, double v, bool* hit = nullptr)
{
	double h = 0.0;
	int32_t poly = -1;
	const bool ok = Height(F, s, u, v, h, poly);
	if (hit) *hit = ok;
	return ok ? h : 0.0;
}

                                                                                      

struct Out { std::vector<m::Manifold> cutters, cracks, halos, solids, holes; };

                                                                             
                                                                           
                                                                          
                                                                      
                                                                   
using DepthFn = std::function<double(double u, double v, const V& W, double thick, double& sd)>;

                                                                     
                                                                           
                                                                       
                                                                         
                                                                         
                                                           
m::Manifold HeightCut(const Surf& s, const Site& F, double u0, double u1, double v0, double v1, double cell,
	bool wantThick, const DepthFn& depth)
{
	if (!(u1 > u0) || !(v1 > v0) || !(cell > 0.0)) return m::Manifold();
	const double ext = std::max(u1 - u0, v1 - v0);
	cell = std::max(cell, ext / 260.0);
	const int nu = std::max(6, int(std::ceil((u1 - u0) / cell)));
	const int nv = std::max(6, int(std::ceil((v1 - v0) / cell)));
	const double cu = (u1 - u0) / nu, cv = (v1 - v0) / nv;
	auto I = [&](int i, int j) { return size_t(i) * size_t(nv + 1) + size_t(j); };
	const size_t N = size_t(nu + 1) * size_t(nv + 1);
	std::vector<double> h(N, 0.0), thick(N, -1.0), d(N, 0.0), sd(N, std::nan("")), raw(N, 0.0);
	std::vector<char> valid(N, 0), forbid(N, 0), outside(N, 0);
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			int32_t poly = -1;
			if (!Height(F, s, u0 + i * cu, v0 + j * cv, h[k], poly)) continue;
			valid[k] = 1;
			forbid[k] = s.Avoided(poly) ? 1 : 0;
			if (!wantThick) continue;
			double th = 0.0;
			int32_t tp = -1;
			if (s.Ray(World(F, u0 + i * cu, v0 + j * cv, h[k]) - F.n * (1e-5 * s.diag), -F.n, 2.0 * s.diag, th, tp)) thick[k] = th;
		}
	{
		std::vector<char> known(valid);
		bool any = false;
		for (char c : known) any |= c != 0;
		if (!any) return m::Manifold();
		for (int pass = 0; pass < nu + nv + 2; ++pass) {
			std::vector<char> nk(known);
			bool changed = false;
			for (int i = 0; i <= nu; ++i)
				for (int j = 0; j <= nv; ++j) {
					const size_t k = I(i, j);
					if (known[k]) continue;
					double sh = 0.0, st = 0.0;
					int c = 0, ct = 0;
					for (int di = -1; di <= 1; ++di)
						for (int dj = -1; dj <= 1; ++dj) {
							const int a = i + di, b = j + dj;
							if (a < 0 || b < 0 || a > nu || b > nv) continue;
							const size_t q = I(a, b);
							if (!known[q]) continue;
							sh += h[q]; ++c;
							if (thick[q] > 0.0) { st += thick[q]; ++ct; }
						}
					if (c == 0) continue;
					h[k] = sh / c;
					if (ct) thick[k] = st / ct;
					nk[k] = 1;
					changed = true;
				}
			known.swap(nk);
			if (!changed) break;
		}
	}
	double dmax = 0.0;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			if (forbid[k]) { outside[k] = 1; continue; }
			const double u = u0 + i * cu, v = v0 + j * cv;
			double sk = std::nan("");
			const double dd = depth(u, v, World(F, u, v, h[k]), thick[k], sk);
			sd[k] = sk;
			if (!(dd > 0.0)) { outside[k] = 1; continue; }
			d[k] = raw[k] = dd;
			dmax = std::max(dmax, dd);
		}
	if (!(dmax > 0.0)) return m::Manifold();
	const double margin = std::max(std::min(0.5 * dmax, 0.5 * ext), 2.0 * std::max(cu, cv));
	const double lift = 0.4 * margin;
	const double ramp = std::max(cu, cv);
	for (size_t k = 0; k < N; ++k)
		if (!outside[k] && std::isfinite(sd[k]) && sd[k] < ramp) d[k] *= std::max(sd[k], 0.05 * ramp) / ramp;
	std::vector<V> top(N), bot(N);
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			double hmax = -1e300;
			for (int di = -1; di <= 1; ++di)
				for (int dj = -1; dj <= 1; ++dj) {
					const int a = i + di, b = j + dj;
					if (a < 0 || b < 0 || a > nu || b > nv) continue;
					hmax = std::max(hmax, h[I(a, b)]);
				}
			const double u = u0 + i * cu, v = v0 + j * cv;
			top[k] = World(F, u, v, hmax + margin);
			double b = outside[k] ? hmax + lift : h[k] - d[k];
			if (outside[k] && !forbid[k] && std::isfinite(sd[k])) {
				double dn = 0.0;
				for (int di = -1; di <= 1; ++di)
					for (int dj = -1; dj <= 1; ++dj) {
						const int a = i + di, c = j + dj;
						if (a < 0 || c < 0 || a > nu || c > nv) continue;
						if (!outside[I(a, c)]) dn = std::max(dn, raw[I(a, c)]);
					}
				if (dn > 0.0) b = h[k] + ClampD(dn * std::max(0.0, -sd[k]) / ramp, 0.02 * dn, lift);
			}
			bot[k] = World(F, u, v, b);
		}
	return SlabSolid(top, bot, nu, nv);
}

                                                                              
void BoxOf(const Site& F, double rad, double& u0, double& u1, double& v0, double& v1)
{
	u0 = v0 = 1e300; u1 = v1 = -1e300;
	for (int k = 0; k < 32; ++k) {
		const double a = 2.0 * m::kPi * k / 32.0;
		const V2 q = ToF(F, std::cos(a), std::sin(a), rad * 1.02);
		u0 = std::min(u0, q.x); u1 = std::max(u1, q.x); v0 = std::min(v0, q.y); v1 = std::max(v1, q.y);
	}
}

                                                                         
                                                                         
                                                                             
                                                                            
                             
m::Manifold Bore(const Surf& s, const Site& F, const std::function<double(double)>& rho, int na, double depth,
	double taper, double apexDepth, double scale = 1.0)
{
	std::vector<V> outline(static_cast<size_t>(na));
	double hmax = -1e300, hmin = 1e300;
	bool any = false;
	for (int j = 0; j < na; ++j) {
		const double a = 2.0 * m::kPi * j / na;
		const double rr = rho(a);
		const V2 q = ToF(F, rr * std::cos(a), rr * std::sin(a), scale);
		bool hit = false;
		const double h = HeightOr0(F, s, q.x, q.y, &hit);
		if (hit) { hmax = std::max(hmax, h); hmin = std::min(hmin, h); any = true; }
		outline[size_t(j)] = World(F, q.x, q.y, 0.0);
	}
	if (!any) { hmax = 0.0; hmin = 0.0; }
	hmax = std::max(hmax, 0.0);
	const double lid = hmax + std::max(0.25 * F.r * scale, 0.1 * depth) + 1e-4;
	for (V& p : outline) p = p + F.n * lid;
	const double total = lid - hmin + depth;
	std::vector<std::pair<double, double>> rings;
	rings.push_back({1.0, 0.0});
	rings.push_back({1.0, lid - hmin});                                                      
	if (taper < 0.999) {
		rings.push_back({Lerp(1.0, taper, 0.55), lid - hmin + 0.5 * depth});
		rings.push_back({taper, total});
	} else {
		rings.push_back({1.0, total});
	}
	return StackSolid(World(F, 0.0, 0.0, 0.0) + F.n * lid, -F.n, outline, rings, apexDepth);
}

                                                                          
                                                                       
                                                   
double BoreRoom(const Surf& s, const Site& F, double rad, double want)
{
	const int steps = std::max(1, std::min(24, int(std::ceil(want / (0.5 * rad)))));
	for (int k = 1; k <= steps; ++k) {
		const double d = want * k / steps;
		const V A = F.o - F.n * d;
		for (int q = 0; q < 4; ++q) {
			const double a = 0.5 * m::kPi * q + 0.4;
			const V dir = F.t * std::cos(a) + F.b * std::sin(a);
			double t = 0.0;
			int32_t poly = -1;
			if (s.Ray(A, dir, 1.4 * rad, t, poly)) return want * (k - 1) / steps;
		}
	}
	return want;
}

                                                                          
                                                                          
                                                                             
                               
m::Manifold Widen(const m::Manifold& c, const V& o, const V& n, double k)
{
	m::mat3x4 T;
	for (int col = 0; col < 3; ++col)
		for (int row = 0; row < 3; ++row) {
			const double e = (row == col ? 1.0 : 0.0);
			T[col][row] = e + (k - 1.0) * (e - n[row] * n[col]);
		}
	const V t = o - V(T[0][0] * o.x + T[1][0] * o.y + T[2][0] * o.z, T[0][1] * o.x + T[1][1] * o.y + T[2][1] * o.z,
		T[0][2] * o.x + T[1][2] * o.y + T[2][2] * o.z);
	T[3] = t;
	return c.Transform(T);
}

                                                                
void CrackStrip(const Site& F, const Surf& s, const std::vector<V2>& pts, const std::vector<double>& w,
	const std::vector<double>& dep, std::vector<m::Manifold>& out)
{
	std::vector<V> topv, botv;
	int rows = 0;
	auto flush = [&]() {
		if (rows >= 3) {
			m::Manifold c = SlabSolid(topv, botv, rows - 1, 2);
			if (!c.IsEmpty()) out.push_back(std::move(c));
		}
		topv.clear(); botv.clear(); rows = 0;
	};
	for (size_t i = 0; i < pts.size(); ++i) {
		double hgt = 0.0;
		int32_t poly = -1;
		if (!Height(F, s, pts[i].x, pts[i].y, hgt, poly) || m::la::dot(s.polyN[size_t(poly)], F.n) < 0.3 || s.Avoided(poly)) {
			flush();
			continue;
		}
		const V2 a = pts[i > 0 ? i - 1 : i], b = pts[i + 1 < pts.size() ? i + 1 : i];
		const V dirW = Unit(F.t * (b.x - a.x) + F.b * (b.y - a.y), F.t);
		const V side = Unit(m::la::cross(F.n, dirW), F.b);
		const V S = World(F, pts[i].x, pts[i].y, hgt);
		const double hw = 0.5 * w[i] + 1e-4;
		const double mc = 0.25 * dep[i] + 2.0 * hw;
		topv.push_back(S - side * hw + F.n * mc); topv.push_back(S + F.n * mc); topv.push_back(S + side * hw + F.n * mc);
		botv.push_back(S - side * hw + F.n * (0.3 * mc)); botv.push_back(S - F.n * dep[i]); botv.push_back(S + side * hw + F.n * (0.3 * mc));
		++rows;
	}
	flush();
}

                                                                      
void CrackWalk(const Site& F, const Surf& s, V2 x0, double ang, double len, double width, double depth,
	double meander, double detail, uint64_t key, uint64_t ch, std::vector<m::Manifold>& out)
{
	if (!(len > 4.0 * width)) return;
	const double step = std::max(1.5 * width, len / 40.0) / std::sqrt(detail);
	const int ns = std::max(3, int(std::ceil(len / step)));
	const V noff(Random01(key, ch, 0) * 1000.0, Random01(key, ch, 1) * 1000.0, 0.0);
	std::vector<V2> pts;
	std::vector<double> w, dep;
	V2 x = x0;
	for (int i = 0; i <= ns; ++i) {
		const double f = double(i) / ns;
		if (i > 0) {
			ang += meander * 2.0 * Fbm(V(f * 4.0, 0.5, 0.0) + noff, 3) * (step / (0.2 * len + 1e-9));
			x.x += std::cos(ang) * step;
			x.y += std::sin(ang) * step;
		}
		pts.push_back(x);
		w.push_back(width * (1.0 - 0.85 * std::pow(f, 1.4)) + 1e-4);
		dep.push_back(depth * (1.0 - 0.7 * std::pow(f, 1.4)));
	}
	CrackStrip(F, s, pts, w, dep, out);
}

                                                                                        

struct Shot {
	uint64_t key = 0;
	Site F;                        
	Site B;                                            
	bool hasBack = false;
	double thick = -1.0;                                                    
	bool through = false;
	bool dent = false;                          
	bool spall = false;                              
	V off;                                
	double reach = 1.0;                                                               
};

struct Ctx {
	const BulletImpactParams* p;
	const Surf* s;
};

                                                                             
                            
double HoleRho(const BulletImpactParams& p, const Shot& S, double a)
{
	const double jag = p.material == 0 ? ClampD(p.metalHoleJagged, 0.0, 2.0) : 0.25;
	const double petals = std::max(1, p.metalPetals);
	const double tear = p.material == 0 ? 0.5 + 0.5 * std::cos(petals * a + Random01(S.key, 1, 2) * 6.283) : 1.0;
	return 1.0 + jag * (0.14 * RingNoise(a, 1.6, S.off, 3) + 0.06 * RingNoise(a, 5.0, S.off + V(3.3, 1.1, 0.0), 2))
		- jag * 0.08 * (1.0 - std::pow(tear, 0.3));
}

void Halo(const Ctx& cx, const Shot& S, Out& out)
{
	const BulletImpactParams& p = *cx.p;
	if (!p.haloEnable || p.haloScale <= 1.0 || p.haloDepth <= 0.0) return;
	const Site& F = S.F;
	const double R = p.haloScale;
	const double jag = ClampD(p.haloJagged, 0.0, 2.0);
	double u0, u1, v0, v1;
	BoxOf(F, R * (1.0 + 0.45 * jag), u0, u1, v0, v1);
	const double cell = F.r * R * 0.08 / std::sqrt(p.detail);
	const double depth = p.haloDepth;
	const V off = S.off + V(7.1, -3.9, 2.4);
	m::Manifold c = HeightCut(*cx.s, F, u0, u1, v0, v1, cell, false, [&](double u, double v, const V&, double, double& sd) {
		const V2 q = ToN(F, u, v, 1.0);
		const double rr = std::sqrt(q.x * q.x + q.y * q.y);
		const double a = std::atan2(q.y, q.x);
		const double edge = R * (1.0 + jag * (0.3 * RingNoise(a, 1.4, off, 3) + 0.08 * Fbm(V(q.x * 1.5, q.y * 1.5, 0.0) + off, 2)));
		sd = (edge - rr) * F.r;
		return rr < edge ? depth : -1.0;
	});
	if (!c.IsEmpty()) out.halos.push_back(std::move(c));
}

                                                         
void Pits(const Ctx& cx, const Shot& S, Out& out)
{
	const BulletImpactParams& p = *cx.p;
	const Surf& s = *cx.s;
	if (p.pitAmount <= 0 || p.pitSize <= 0.0) return;
	const Site& F = S.F;
	for (int k = 0; k < p.pitAmount; ++k) {
		auto rnd = [&](uint64_t ch) { return Random01(S.key, 100 + uint64_t(k) * 8, ch); };
		const double a = rnd(0) * 2.0 * m::kPi;
		const double dist = Lerp(1.6, std::max(1.7, p.pitSpread), std::sqrt(rnd(1)));
		const V2 q = ToF(F, std::cos(a) * dist, std::sin(a) * dist, 1.0);
		double h = 0.0;
		int32_t poly = -1;
		if (!Height(F, s, q.x, q.y, h, poly) || s.Avoided(poly)) continue;
		Site P = F;
		P.o = World(F, q.x, q.y, h);
		P.r = F.r * p.pitSize * Lerp(0.4, 1.3, rnd(2));
		P.e = 1.0;
		P.Hs = 2.0 * P.r + 1e-3;
		P.drop = 4.0 * P.r + 1e-3;
		const V po = S.off + V(rnd(3) * 100.0, rnd(4) * 100.0, 0.0);
		const double dep = P.r * Lerp(0.35, 0.8, rnd(5));
		m::Manifold c = Bore(s, P, [&](double t) { return 1.0 + 0.25 * RingNoise(t, 1.5, po, 2); }, 14, 0.3 * dep, 0.45, 0.7 * dep);
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	}
}

                                                          
                                                                            
                                                                        
                                                
                                                                            
                                                              
                                                                       
                                                                            
                                       
m::Manifold Crown(const Ctx& cx, const Shot& S, const Site& F, const std::function<double(double)>& rIn, double width,
	double height, double tearAmt, double curl, bool petal, const std::function<double(double)>& dish, uint64_t ch)
{
	const BulletImpactParams& p = *cx.p;
	const Surf& s = *cx.s;
	if (!(height > 0.0) || !(width > 0.0)) return m::Manifold();
	const int petals = petal ? std::max(3, std::min(p.metalPetals - 1 + int(Random01(S.key, ch, 2) * 3.0), 24))
		: std::max(1, std::min(p.metalPetals, 24));
	const int na = std::max(32, std::min(240, int(std::ceil(std::max(48.0, 12.0 * petals) * std::sqrt(p.detail)))));
	const int nr = petal ? 8 : 6;
	const double tear = ClampD(tearAmt, 0.0, 1.0);
	const double embed = std::max(0.25 * height, 0.08 * F.r);
	const double sheet = std::max(0.07 * F.r, 0.1 * height);                                 
	const double phase = Random01(S.key, ch, 0) * 2.0 * m::kPi;
	const V off = S.off + V(Random01(S.key, ch, 1) * 100.0, 5.5, -2.5);
	std::vector<double> petalW(static_cast<size_t>(petals));
	double sumW = 0.0;
	for (int k = 0; k < petals; ++k) { petalW[size_t(k)] = Lerp(0.6, 1.4, Random01(S.key, ch, 10 + uint64_t(k))); sumW += petalW[size_t(k)]; }
	std::vector<V> top(size_t((nr + 1) * na)), bot(size_t((nr + 1) * na));
	for (int j = 0; j < na; ++j) {
		const double a = 2.0 * m::kPi * j / na;
		                                                                  
		double t = std::fmod(a - phase + 4.0 * m::kPi, 2.0 * m::kPi) / (2.0 * m::kPi) * sumW;
		int k = 0;
		while (k + 1 < petals && t > petalW[size_t(k)]) { t -= petalW[size_t(k)]; ++k; }
		const double f = ClampD(t / petalW[size_t(k)], 0.0, 1.0);
		const double prof = std::sin(m::kPi * f);
		const double gate = Lerp(1.0, Smooth(0.0, 0.45, prof), tear);
		const double var = Lerp(0.7, 1.25, Random01(S.key, ch, 50 + uint64_t(k)));
		const double nz = 0.85 + 0.3 * RingNoise(a, 2.0, off, 2);
		const double r0 = rIn(a);
		const double ca = std::cos(a), sa = std::sin(a);
		                                                                        
		const double W = petal
			? width * var * (0.12 + 0.88 * std::pow(prof, 0.7)) * (0.85 + 0.3 * RingNoise(a, 6.0, off + V(1.0, 2.0, 3.0), 2))
			: width * (0.7 + 0.45 * std::pow(prof, 0.5)) * var;
		const double H = height * var * gate * nz;
		const double inTop = petal ? r0 : r0 * (1.0 - 0.18 * ClampD(curl, 0.0, 1.0) * gate);
		const double inBot = petal ? r0 : r0 * 1.03 + 0.02;
		for (int i = 0; i <= nr; ++i) {
			const double fr = double(i) / nr;
			const double rt = Lerp(inTop, r0 + W, fr), rb = Lerp(inBot, r0 + W, fr);
			const V2 qt = ToF(F, rt * ca, rt * sa, 1.0), qb = ToF(F, rb * ca, rb * sa, 1.0);
			const double ht = HeightOr0(F, s, qt.x, qt.y) - dish(rt);
			const double hb = HeightOr0(F, s, qb.x, qb.y) - dish(rb);
			double topH, botH;
			if (petal) {
				                                                              
				const double rise = H * std::pow(fr, 1.4);
				const double thk = sheet * (1.0 - 0.75 * fr) * (0.4 + 0.6 * gate);
				botH = Lerp(hb - embed, hb + rise - thk, Smooth(0.0, 0.3, fr));
				topH = std::max(hb + rise, botH + 0.25 * thk) + 0.15 * thk;
				topH = std::max(topH, ht - 0.35 * embed);
			} else {
				                                                        
				const double shape = std::pow(1.0 - fr, 1.3) * (1.0 + 0.6 * curl * (1.0 - fr)) / (1.0 + 0.6 * curl);
				botH = hb - embed;
				topH = std::max(ht + H * shape - 0.35 * embed, botH + 0.05 * embed);
			}
			top[size_t(i * na + j)] = World(F, qt.x, qt.y, topH);
			bot[size_t(i * na + j)] = World(F, qb.x, qb.y, botH);
		}
	}
	return RingSolid(top, bot, nr, na);
}

                                                                                        

void Metal(const Ctx& cx, const Shot& S, Out& out)
{
	const BulletImpactParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Site& F = S.F;
	const double dishW = std::max(1.05, p.metalDishWidth);
	const double dishD = std::max(0.0, p.metalDish) * F.r * (S.dent ? 1.6 : 1.0);
	auto dish = [=](double rho) {
		if (rho >= dishW || dishD <= 0.0) return 0.0;
		const double t = 1.0 - (rho / dishW) * (rho / dishW);
		return dishD * t * t;
	};
	auto rho = [&](double a) { return HoleRho(p, S, a); };
	const int na = std::max(24, int(std::ceil(48.0 * std::sqrt(p.detail))));
	                       
	if (dishD > 0.0) {
		double u0, u1, v0, v1;
		BoxOf(F, dishW, u0, u1, v0, v1);
		const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
		m::Manifold c = HeightCut(s, F, u0, u1, v0, v1, F.r * 0.1 / std::sqrt(p.detail), true,
			[&](double u, double v, const V&, double th, double&) {
				const V2 q = ToN(F, u, v, 1.0);
				double d = dish(std::sqrt(q.x * q.x + q.y * q.y));
				if (th > 0.0) d = std::min(d, cap * th);
				return d > 1e-4 * F.r ? d : -1.0;
			});
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	}
	if (S.through) {
		const double depth = S.thick + dishD + 0.5 * F.r;
		m::Manifold c = Bore(s, F, rho, na, depth, 1.0, 0.0);
		if (!c.IsEmpty()) { out.holes.push_back(Widen(c, F.o, F.n, 1.02)); out.cutters.push_back(std::move(c)); }
	} else {
		                                                       
		const double cap = (p.thicknessCap > 0.0 && S.thick > 0.0) ? p.thicknessCap * S.thick : 1e9;
		const double depth = std::min(0.45 * F.r, std::max(0.0, cap - dishD));
		if (depth > 1e-4) {
			m::Manifold c = Bore(s, F, [&](double a) { return 0.8 * rho(a); }, na, 0.2 * depth, 0.55, 0.8 * depth);
			if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
		}
	}
	              
	const double lipH = std::max(0.0, p.metalLipHeight) * F.r * (S.dent ? 0.5 : 1.0);
	{
		m::Manifold c = Crown(cx, S, F, [&](double a) { return S.through ? rho(a) : 0.85 * rho(a); }, std::max(0.0, p.metalLipWidth),
			lipH, p.metalPetalTear, p.metalCurl, false, dish, 20);
		if (!c.IsEmpty()) out.solids.push_back(std::move(c));
	}
	                                                                        
	                                      
	if (S.through && S.hasBack && p.metalExitBurr > 0.0 && !out.holes.empty()) {
		const Site& B = S.B;
		const double inner = 0.85 * F.e;
		const double len = p.metalExitBurr;
		m::Manifold c = Crown(cx, S, B, [=](double) { return inner; }, len + 0.15 * F.e, 0.6 * len * F.r,
			std::min(1.0, p.metalPetalTear + 0.4), 0.0, true, [](double) { return 0.0; }, 21);
		if (!c.IsEmpty()) {
			m::Manifold cut = c - out.holes.back();
			if (cut.Status() == m::Manifold::Error::NoError && !cut.IsEmpty()) out.solids.push_back(std::move(cut));
		}
	}
	Halo(cx, S, out);
	Pits(cx, S, out);
}

                                                                                        

                                                                         
                                                                            
                                              
double SplinterField(const BulletImpactParams& p, uint64_t key, uint64_t ch, double x, double y,
	double core, double reach, double coreDepth, double fibreDepth, const V& off, double& sd)
{
	const double w = std::max(0.05, p.woodFiberWidth) * std::max(1.0, core / 1.25);
	const double ky = std::floor(y / w);
	const double fr = y / w - ky;
	const int64_t strip = int64_t(ky);
	const double rr = std::sqrt(x * x + y * y);
	                                  
	const double a = std::atan2(y, x);
	const double edge = core * (1.0 + 0.12 * RingNoise(a, 2.0, off, 2));
	double d = -1.0;
	sd = edge - rr;
	if (rr < edge) {
		const double t = rr / edge;
		d = fibreDepth + coreDepth * std::pow(1.0 - t, 0.8);
	}
	                                                              
	const double yc = (ky + 0.5) * w;
	if (std::abs(yc) < core) {
		const double c0 = std::sqrt(std::max(0.0, core * core - yc * yc));
		const double side = x >= 0.0 ? 1.0 : 0.0;
		const double rs = Random01(key, ch + uint64_t(strip + 4096) * 2 + uint64_t(side), 0);
		const double L = reach * std::pow(rs, 0.8) * (1.0 - 0.5 * std::pow(std::abs(yc) / core, 2.0));
		const double tip = std::pow(1.0 - std::abs(2.0 * fr - 1.0), 0.8);
		const double end = c0 + L * tip;
		sd = std::max(sd, std::min(end - std::abs(x), w * std::min(fr, 1.0 - fr)));
		if (std::abs(x) < end) {
			const double t = ClampD((std::abs(x) - c0) / std::max(1e-6, L), 0.0, 1.0);
			const double fd = fibreDepth * (1.0 - 0.75 * t) * Lerp(0.6, 1.2, Random01(key, ch + uint64_t(strip + 4096) * 2 + uint64_t(side), 1));
			d = std::max(d, fd);
		}
	}
	return d;
}

void Wood(const Ctx& cx, const Shot& S, Out& out)
{
	const BulletImpactParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Site& F = S.F;
	const int na = std::max(24, int(std::ceil(40.0 * std::sqrt(p.detail))));
	auto rnd = [&](uint64_t ch) { return Random01(S.key, 30, ch); };
	const V off = S.off;
	                                        
	{
		const double reach = std::max(0.0, p.woodSplinter);
		const double core = 1.3;
		const double ext = core + reach + 0.2;
		double u0, u1, v0, v1;
		BoxOf(F, ext, u0, u1, v0, v1);
		const double cd = std::max(0.0, p.woodCrush) * F.r, fd = 0.1 * F.r;
		const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
		m::Manifold c = HeightCut(s, F, u0, u1, v0, v1, F.r * 0.07 / std::sqrt(p.detail), true,
			[&](double u, double v, const V&, double th, double& sd) {
				const V2 q = ToN(F, u, v, 1.0);
				const double x = q.x * F.gx + q.y * F.gy, y = -q.x * F.gy + q.y * F.gx;
				double d = SplinterField(p, S.key, 3000, x, y, core, reach, cd / F.r, fd / F.r, off, sd) * F.r;
				sd *= F.r;
				if (d > 0.0 && th > 0.0) d = std::min(d, cap * th);
				return d;
			});
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	}
	           
	{
		auto rho = [&](double a) { return 0.85 * HoleRho(p, S, a); };
		m::Manifold c;
		if (S.through) c = Bore(s, F, rho, na, S.thick + 0.5 * F.r, 1.0, 0.0);
		else {
			const double cap = (p.thicknessCap > 0.0 && S.thick > 0.0) ? p.thicknessCap * S.thick : 1e9;
			                                                                        
			double depth = std::min({cap, Lerp(0.7, 1.0, rnd(0)) * std::max(p.penetration, 4.0 * F.r), 5.0 * F.r});
			depth = std::max(0.5 * F.r, BoreRoom(s, F, 0.9 * F.r, depth));
			c = Bore(s, F, rho, na, 0.8 * depth, 0.6, 0.2 * depth);
		}
		if (!c.IsEmpty()) { out.holes.push_back(Widen(c, F.o, F.n, 1.02)); out.cutters.push_back(std::move(c)); }
	}
	                                         
	if (rnd(1) < ClampD(p.woodSplitChance, 0.0, 1.0) && p.woodSplitLength > 0.0) {
		const double ang = std::atan2(F.gy, F.gx);
		for (int sd = 0; sd < 2; ++sd) {
			const double len = p.woodSplitLength * F.r * Lerp(0.3, 1.0, rnd(2 + uint64_t(sd)));
			const double a = ang + (sd ? m::kPi : 0.0);
			const V2 x0{std::cos(a) * 0.6 * F.r, std::sin(a) * 0.6 * F.r};
			CrackWalk(F, s, x0, a, len, p.crackWidth * 1.2, std::max(p.crackDepth, 0.4 * F.r), 0.08, p.detail,
				S.key, 400 + uint64_t(sd) * 32, out.cracks);
		}
	}
	                                     
	if (S.through && S.hasBack) {
		const Site& B = S.B;
		const double sc = std::max(1.0, p.woodExitScale);
		const double reach = std::max(0.0, p.woodSplinter) * sc * 0.8;
		const double core = 0.75 * sc;
		const double depthT = ClampD(p.woodExitDepth, 0.0, 0.95) * S.thick;
		double u0, u1, v0, v1;
		BoxOf(B, core + reach + 0.3, u0, u1, v0, v1);
		const V boff = off + V(11.0, 4.0, -6.0);
		m::Manifold c = HeightCut(s, B, u0, u1, v0, v1, B.r * 0.12 / std::sqrt(p.detail), false,
			[&](double u, double v, const V& W, double, double& sd) {
				const V2 q = ToN(B, u, v, 1.0);
				const double x = q.x * B.gx + q.y * B.gy, y = -q.x * B.gy + q.y * B.gx;
				                            
				const double d = SplinterField(p, S.key, 5000, x / 1.5, y, core, reach / 1.5, depthT / B.r, 0.25 * depthT / B.r, boff, sd);
				sd *= B.r;
				if (d <= 0.0) return -1.0;
				return d * B.r * (0.85 + 0.3 * Fbm(W / (0.6 * B.r) + boff, 2));
			});
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
		                                                                    
		                                                
		for (int k = 0; k < p.woodSlivers; ++k) {
			auto sr = [&](uint64_t ch) { return Random01(S.key, 200 + uint64_t(k) * 16, ch); };
			const double side = sr(0) < 0.5 ? -1.0 : 1.0;
			const double y = (sr(1) * 2.0 - 1.0) * 0.7 * core;
			const double edgeX = 1.5 * std::sqrt(std::max(0.0, core * core - y * y)) + 1.05 * reach;
			const double len = std::max(0.3, p.woodSliverLength) * Lerp(0.6, 1.2, sr(2));
			const double embedL = 0.35 * len;
			const double lift = Lerp(0.15, 0.45, sr(3)) * len;
			const int K = 10;
			std::vector<V> cpts, side_, up;
			std::vector<double> hw, ht;
			const double w0 = Lerp(0.18, 0.32, sr(4)), t0 = Lerp(0.06, 0.11, sr(5));
			                                                       
			auto at = [&](double x) { return ToF(B, x * B.gx - y * B.gy, x * B.gy + y * B.gx, 1.0); };
			const V2 rim = at(side * edgeX);
			const double rimH = HeightOr0(B, s, rim.x, rim.y);
			for (int i = 0; i <= K; ++i) {
				const double f = double(i) / K;
				const double run = f * (embedL + len);                                       
				const V2 q = at(side * (edgeX + embedL - run));
				const double fo = ClampD((run - embedL) / len, 0.0, 1.0);                       
				                                                                                 
				const double z = run < embedL ? HeightOr0(B, s, q.x, q.y) - 0.8 * t0 * B.r : rimH + lift * B.r * fo * fo;
				cpts.push_back(World(B, q.x, q.y, z));
				hw.push_back(B.r * w0 * (1.0 - 0.9 * std::pow(f, 1.2)) + 0.01 * B.r);
				ht.push_back(B.r * t0 * (1.0 - 0.6 * f) + 0.01 * B.r);
			}
			for (int i = 0; i <= K; ++i) {
				const V dir = Unit(cpts[size_t(std::min(i + 1, K))] - cpts[size_t(std::max(i - 1, 0))], B.t);
				const V sd = Unit(m::la::cross(B.n, dir), B.b);
				side_.push_back(sd);
				up.push_back(Unit(m::la::cross(dir, sd), B.n));
			}
			m::Manifold sl = SweepSolid(cpts, side_, up, hw, ht);
			if (!sl.IsEmpty()) out.solids.push_back(std::move(sl));
		}
	}
	Halo(cx, S, out);
	Pits(cx, S, out);
}

                                                                                            

                                                                           
                                                                      
                                                                           
                                          
double SpallDepth(const BulletImpactParams& p, const Site& F, double R, double skin, double cone, const V& off,
	double shift, double sAng, double u, double v, const V& W, double& sd)
{
	const V2 q = ToN(F, u, v, 1.0);
	const double rr = std::sqrt(q.x * q.x + q.y * q.y);
	const double a = std::atan2(q.y, q.x);
	const double jag = ClampD(p.concJagged, 0.0, 2.0);
	const double edge = R * (1.0 + shift * std::cos(a - sAng)
		+ jag * (0.38 * RingNoise(a, 1.1, off, 3) + 0.14 * RingNoise(a, 3.2, off + V(2.0, 9.0, 0.0), 2)
			+ 0.05 * RingNoise(a, 9.0, off + V(-6.0, 3.0, 1.0), 2)));
	sd = (edge - rr) * F.r;
	if (rr >= edge) return -1.0;
	const double t = rr / edge;
	const double terr = 0.8 + 0.1 * RingNoise(a, 2.2, off + V(-4.0, 1.0, 3.0), 2);
	double d = skin * (0.75 + 0.5 * std::max(0.0, Fbm(W / (0.5 * R * F.r) + off, 2)));
	if (t < terr) {
		                                                          
		const double c = (terr - t) / terr;
		const double w = c * 3.0 + 1.3 * Fbm(W / (0.3 * R * F.r) + off + V(1.7, -2.9, 4.4), 3);
		const double fl = std::floor(w);
		const double stepped = ClampD((fl + Smooth(0.55, 1.0, w - fl)) / 3.0, 0.0, 1.0);
		d += cone * std::pow(Lerp(c, stepped, 0.3), 1.1);
	}
	if (p.concRoughness > 0.0) {
		const double agg = std::max(0.05, p.concAggregate) * F.r;
		const double pit = 1.0 - 2.0 * std::abs(Fbm(W / agg + off + V(-3.3, 8.1, 2.2), 3));
		const double conch = Fbm(W / (0.35 * R * F.r) + off + V(5.5, 1.5, 9.5), 3);
		d += ClampD(p.concRoughness, 0.0, 2.0) * (0.12 * cone * pit + 0.18 * cone * conch) * Smooth(0.0, 0.3, 1.0 - t);
		d = std::max(d, 0.4 * skin);
	}
	return d;
}

void Concrete(const Ctx& cx, const Shot& S, Out& out)
{
	const BulletImpactParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Site& F = S.F;
	const int na = std::max(20, int(std::ceil(36.0 * std::sqrt(p.detail))));
	auto rnd = [&](uint64_t ch) { return Random01(S.key, 40, ch); };
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double R = S.spall ? std::max(1.2, p.concSpallScale) * Lerp(0.75, 1.2, rnd(0)) : 1.5;
	const double skin = std::max(0.0, p.concSkinDepth) * F.r;
	const double cone = (S.spall ? std::max(0.0, p.concSpallDepth) * Lerp(0.7, 1.2, rnd(1)) : 0.35) * F.r;
	const double shift = S.spall ? 0.3 * rnd(3) : 0.0, sAng = rnd(4) * 2.0 * m::kPi;
	{
		double u0, u1, v0, v1;
		BoxOf(F, R * (1.0 + shift + 0.6 * ClampD(p.concJagged, 0.0, 2.0)), u0, u1, v0, v1);
		const double cell = F.r * std::max(0.06, 0.05 * R) / std::sqrt(p.detail);
		m::Manifold c = HeightCut(s, F, u0, u1, v0, v1, cell, true, [&](double u, double v, const V& W, double th, double& sd) {
			double d = SpallDepth(p, F, R, S.spall ? skin : 0.3 * skin, cone, S.off, shift, sAng, u, v, W, sd);
			if (d > 0.0 && th > 0.0) d = std::min(d, cap * th);
			return d;
		});
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	}
	       
	{
		auto rho = [&](double a) { return 0.7 * HoleRho(p, S, a); };
		m::Manifold c;
		if (S.through) c = Bore(s, F, rho, na, S.thick + 0.5 * F.r, 1.0, 0.0);
		else {
			const double lim = S.thick > 0.0 ? cap * S.thick : 1e9;
			double depth = std::min(lim, cone + std::max(0.0, p.concBoreDepth) * F.r * Lerp(0.6, 1.1, rnd(2)));
			depth = std::max(0.5 * F.r, BoreRoom(s, F, 0.75 * F.r, depth));
			c = Bore(s, F, rho, na, 0.75 * depth, 0.55, 0.25 * depth);
		}
		if (!c.IsEmpty()) { out.holes.push_back(Widen(c, F.o, F.n, 1.02)); out.cutters.push_back(std::move(c)); }
	}
	                                   
	if (S.spall && p.concCracks > 0 && p.crackWidth > 0.0) {
		for (int k = 0; k < p.concCracks; ++k) {
			auto cr = [&](uint64_t ch) { return Random01(S.key, 400 + uint64_t(k) * 32, ch); };
			const double a = 2.0 * m::kPi * (k + 0.8 * cr(0)) / p.concCracks;
			const V2 x0 = ToF(F, std::cos(a) * 0.85 * R, std::sin(a) * 0.85 * R, 1.0);
			const double len = std::max(0.0, p.concCrackLength) * R * F.r * Lerp(0.35, 1.0, cr(1));
			CrackWalk(F, s, x0, std::atan2(x0.y, x0.x) + (cr(2) - 0.5) * 0.6, len, p.crackWidth,
				std::max(p.crackDepth, 0.2 * cone), 0.5, p.detail, S.key, 400 + uint64_t(k) * 32 + 8, out.cracks);
		}
	}
	             
	if (S.through && S.hasBack) {
		const Site& B = S.B;
		const double Rb = std::max(1.2, R * std::max(1.0, p.concExitScale));
		const double coneB = std::min(0.45 * S.thick, cone * 1.3 + 0.2 * F.r);
		double u0, u1, v0, v1;
		BoxOf(B, Rb * (1.0 + 0.6 * ClampD(p.concJagged, 0.0, 2.0)), u0, u1, v0, v1);
		const V boff = S.off + V(13.0, -7.0, 4.0);
		m::Manifold c = HeightCut(s, B, u0, u1, v0, v1, B.r * std::max(0.06, 0.05 * Rb) / std::sqrt(p.detail), false,
			[&](double u, double v, const V& W, double, double& sd) { return SpallDepth(p, B, Rb, skin, coneB, boff, 0.0, 0.0, u, v, W, sd); });
		if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	}
	Halo(cx, S, out);
	Pits(cx, S, out);
}

void BuildShot(const Ctx& cx, const Shot& S, Out& out)
{
	switch (cx.p->material) {
	case 1: Wood(cx, S, out); break;
	case 2: Concrete(cx, S, out); break;
	default: Metal(cx, S, out); break;
	}
}

                                                                             
V GrainAxis(const BulletImpactParams& p, const Surf& s)
{
	if (p.grainMode == 1) return Unit(V(p.grainX, p.grainY, p.grainZ), V(0, 0, 1));
	const V e = s.hi - s.lo;
	if (e.x >= e.y && e.x >= e.z) return V(1, 0, 0);
	if (e.y >= e.z) return V(0, 1, 0);
	return V(0, 0, 1);
}

                                                                           
void MakeSite(Site& F, const V& o, const V& n, double r, const V& grain)
{
	F.o = o;
	F.n = n;
	F.t = AnyPerp(n);
	F.b = m::la::cross(F.n, F.t);
	F.r = r;
	V g = grain - n * m::la::dot(grain, n);
	if (m::la::length(g) < 0.2) g = F.t;                              
	g = Unit(g, F.t);
	F.gx = m::la::dot(g, F.t);
	F.gy = m::la::dot(g, F.b);
}

}               

void PlaceBulletImpacts(const CoreInput& in, const BulletImpactParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& halos, std::vector<m::Manifold>& solids)
{
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	BulletImpactParams p = pIn;
	p.material = std::max(0, std::min(p.material, 2));
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.sizeMin = std::max(1e-3, p.sizeMin);
	p.sizeMax = std::max(1e-3, p.sizeMax);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	p.amount = std::max(0, std::min(p.amount, 5000));
	p.clusters = std::max(0, std::min(p.clusters, 256));
	p.pitAmount = std::max(0, std::min(p.pitAmount, 64));
	p.woodSlivers = std::max(0, std::min(p.woodSlivers, 32));
	p.concCracks = std::max(0, std::min(p.concCracks, 16));
	p.metalPetals = std::max(1, std::min(p.metalPetals, 24));
	p.paintSpacing = std::max(1e-3, p.paintSpacing);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	p.crackWidth = std::max(1e-4, p.crackWidth);
	const bool paint = p.mode == 1;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && p.amount <= 0) return;

	Surf s;
	BuildSurf(in, p.avoid, s);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x42554c4c45545321ULL)                 
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4249;                             
	const V grain = GrainAxis(p, s);
	const bool fixedShot = p.shotMode == 1;
	const V shotDir = Unit(V(p.shotX, p.shotY, p.shotZ), V(0, 1, 0));
	const double sizeExp = std::pow(4.0, 2.0 * ClampD(p.sizeBias, 0.0, 1.0) - 1.0);

	                                                                          
	                                                                           
	                                                    
	auto makeShot = [&](const V& P, const V& N, uint64_t key, int32_t maskIdx, double rIn, Shot& S) -> bool {
		auto rnd = [&](uint64_t ch) { return Random01(key, 0, ch); };
		if (mask && !mask->Keep(P, maskIdx, kStream)) return false;
		const double sc = mask ? mask->SizeScale(P) : 1.0;
		if (!(sc > 0.0)) return false;
		S.key = key;
		const double r = (rIn > 0.0 ? rIn : Lerp(p.sizeMin, p.sizeMax, std::pow(rnd(0), sizeExp))) * sc;
		MakeSite(S.F, P, N, r, grain);
		        
		double sl = 0.0, sx = 1.0, sy = 0.0;
		if (fixedShot) {
			const double c = -m::la::dot(shotDir, N);
			if (c < 0.08) return false;
			const V tp = shotDir - N * m::la::dot(shotDir, N);
			sl = std::sqrt(std::max(0.0, 1.0 - c * c));
			const double tl = m::la::length(tp);
			if (tl > 1e-9) { sx = m::la::dot(tp, S.F.t) / tl; sy = m::la::dot(tp, S.F.b) / tl; }
		} else {
			sl = std::sin(ClampD(p.oblique, 0.0, 1.0) * 1.22 * std::pow(rnd(1), 1.5));
			const double a = rnd(2) * 2.0 * m::kPi;
			sx = std::cos(a); sy = std::sin(a);
		}
		S.F.ca = sx; S.F.sa = sy;
		S.F.e = 1.0 + std::max(0.0, p.elongate) * 0.9 * sl * sl / std::max(0.35, std::sqrt(1.0 - sl * sl));
		S.F.e = std::min(S.F.e, 3.5);
		S.off = V(rnd(10) * 1000.0, rnd(11) * 1000.0, rnd(12) * 1000.0);
		                          
		double foot = 1.6;
		if (p.material == 0) foot = std::max({p.metalDishWidth, 1.0 + p.metalLipWidth * 1.5, p.haloEnable ? p.haloScale * 1.5 : 0.0});
		if (p.material == 1) foot = 1.3 + p.woodSplinter + 0.3;
		if (p.material == 2) foot = std::max(1.5, p.concSpallScale * 1.2 * (1.3 + 0.6 * ClampD(p.concJagged, 0.0, 2.0)));
		if (p.pitAmount > 0) foot = std::max(foot, p.pitSpread + 0.5);
		const double E = foot * r * S.F.e;
		S.reach = E;
		S.F.Hs = 0.6 * E + 2.0 * r + 1e-3;
		S.F.drop = 1.2 * E + 6.0 * r + 1e-3;
		                                 
		double th = 0.0;
		int32_t bp = -1;
		S.thick = -1.0;
		if (s.Ray(P - N * (1e-5 * s.diag), -N, 2.0 * s.diag, th, bp)) S.thick = th;
		const double pen = std::max(0.0, p.penetration) * Lerp(1.0 - ClampD(p.penetrationVar, 0.0, 1.0), 1.0 + 0.5 * ClampD(p.penetrationVar, 0.0, 1.0), rnd(3));
		S.through = S.thick > 0.0 && S.thick <= pen;
		S.dent = p.material == 0 && rnd(4) < ClampD(p.metalDentChance, 0.0, 1.0);
		if (S.dent) S.through = false;
		S.spall = p.material == 2 && rnd(5) < ClampD(p.concSpallChance, 0.0, 1.0);
		S.hasBack = false;
		if (S.through && bp >= 0) {
			const V bn = s.polyN[size_t(bp)];
			if (m::la::dot(bn, N) < -0.5) {
				S.hasBack = true;
				Site& B = S.B;
				MakeSite(B, P - N * th, bn, r, grain);
				B.e = 1.0;
				B.ca = 1.0; B.sa = 0.0;
				double footB = foot;
				if (p.material == 1) footB = std::max(foot, (0.75 + p.woodSplinter * 0.8) * std::max(1.0, p.woodExitScale) * 1.5 + 0.5);
				if (p.material == 2) footB = foot * std::max(1.0, p.concExitScale);
				if (p.material == 0) footB = std::max(foot, 2.5 + 2.0 * p.metalExitBurr);
				B.Hs = 0.6 * footB * r + 2.0 * r + 1e-3;
				B.drop = 1.2 * footB * r + 6.0 * r + 1e-3;
				S.reach = std::max(S.reach, footB * r);
			}
		}
		return true;
	};

	std::vector<Shot> shots;
	auto spaced = [&](const V& P, double r) {
		const double ms = std::max(0.0, p.minSpacing);
		for (const Shot& q : shots) {
			const double lim = ms * (q.F.r + r);
			if (m::la::dot(q.F.o - P, q.F.o - P) < lim * lim) return false;
		}
		return true;
	};
	                                                                
	auto drop = [&](const V& P, const V& N, double reach, V& S, V& Ns, int32_t& poly) {
		double d = 0.0;
		if (!s.Ray(P + N * reach, -N, 2.0 * reach, d, poly)) return false;
		S = P + N * reach - N * d;
		Ns = s.polyN[size_t(poly)];
		return m::la::dot(Ns, N) > 0.3 && !s.Avoided(poly);
	};

	if (!paint) {
		const uint64_t as = seed ^ 0x4155544fULL;            
		struct Burst { V o, n, t, b; bool ok = false; };
		std::vector<Burst> bursts(size_t(p.clusters));
		for (int c = 0; c < p.clusters; ++c) {
			Burst& Bu = bursts[size_t(c)];
			int32_t poly = -1;
			for (int tries = 0; tries < 16 && !Bu.ok; ++tries) {
				V P;
				if (!s.Sample(Random01(as, 0x10000000ULL + uint64_t(c) * 64 + uint64_t(tries), 0),
					Random01(as, 0x10000000ULL + uint64_t(c) * 64 + uint64_t(tries), 1),
					Random01(as, 0x10000000ULL + uint64_t(c) * 64 + uint64_t(tries), 2), P, poly)) break;
				const V n = s.polyN[size_t(poly)];
				if (fixedShot && -m::la::dot(shotDir, n) < 0.08) continue;
				Bu.o = P; Bu.n = n; Bu.t = AnyPerp(n); Bu.b = m::la::cross(n, Bu.t); Bu.ok = true;
			}
		}
		int okBursts = 0;
		for (const Burst& b : bursts) okBursts += b.ok ? 1 : 0;
		const int maxCand = p.amount * 40 + 100;
		int accepted = 0;
		for (int k = 0; k < maxCand && accepted < p.amount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(as, uint64_t(k), ch); };
			V P, N;
			int32_t poly = -1;
			if (okBursts > 0 && rnd(0) < ClampD(p.clusterShare, 0.0, 1.0)) {
				int c = int(rnd(1) * p.clusters) % p.clusters;
				while (!bursts[size_t(c)].ok) c = (c + 1) % p.clusters;
				const Burst& Bu = bursts[size_t(c)];
				const double sp = std::max(1e-3, p.clusterSpread);
				const double dx = 0.5 * sp * Gauss(rnd(2), rnd(3)), dy = 0.5 * sp * Gauss(rnd(4), rnd(5));
				if (dx * dx + dy * dy > 4.0 * sp * sp) continue;
				if (!drop(Bu.o + Bu.t * dx + Bu.b * dy, Bu.n, sp + 1.0, P, N, poly)) continue;
			} else {
				if (!s.Sample(rnd(2), rnd(3), rnd(4), P, poly)) break;
				N = s.polyN[size_t(poly)];
				if (s.Avoided(poly)) continue;
			}
			const uint64_t key = seed ^ ((uint64_t(k) + 1) * 0xa24baed4963ee407ULL);
			Shot S;
			if (!makeShot(P, N, key, int32_t(k), 0.0, S)) continue;
			if (!spaced(P, S.F.r)) continue;
			shots.push_back(std::move(S));
			++accepted;
		}
	} else {
		const uint64_t ps = seed ^ 0x5041494eULL;            
		for (size_t si = 0; si < paintStrokes->size(); ++si) {
			std::vector<V> pts;
			for (const EVec& q : (*paintStrokes)[si])
				if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
					&& (pts.empty() || m::la::length(q - pts.back()) > 1e-9)) pts.push_back(q);
			if (pts.empty()) continue;
			double len = 0.0;
			for (size_t i = 0; i + 1 < pts.size(); ++i) len += m::la::length(pts[i + 1] - pts[i]);
			auto rnd = [&](uint64_t sub, uint64_t ch) { return Random01(ps, (uint64_t(si) << 20) + sub, ch); };
			auto keyOf = [&](uint64_t sub) { return ps ^ (((uint64_t(si) << 20) + sub + 1) * 0xa24baed4963ee407ULL); };
			auto addAt = [&](const V& P0, uint64_t sub, double rForce) {
				V N, Sp;
				if (!s.NormalNear(P0, p.paintSnap, N, Sp)) return;
				V P, Ns;
				int32_t poly = -1;
				if (!drop(Sp, N, std::max(p.paintSnap, 1e-3), P, Ns, poly)) return;
				Shot S;
				if (!makeShot(P, Ns, keyOf(sub), int32_t((si << 12) + sub), rForce, S)) return;
				if (rForce <= 0.0 && !spaced(P, S.F.r)) return;
				shots.push_back(std::move(S));
			};
			V cen(0.0);
			for (const V& q : pts) cen = cen + q;
			cen = cen / double(pts.size());
			                                
			if (pts.size() < 2 || len < p.paintSingle) { addAt(cen, 0, 0.0); continue; }
			const double gapEnd = m::la::length(pts.back() - pts.front());
			const bool closed = pts.size() >= 4 && (gapEnd < 1e-6 * len + 1e-9 || gapEnd < ClampD(p.paintClose, 0.0, 1.0) * len);
			if (closed) {
				if (gapEnd < 1e-6 * len + 1e-9) pts.pop_back();
				V nsum(0.0);
				const size_t stride = std::max<size_t>(1, pts.size() / 48);
				for (size_t i = 0; i < pts.size(); i += stride) {
					V N, Sp;
					if (s.NormalNear(pts[i], p.paintSnap, N, Sp)) nsum = nsum + N;
				}
				const V n = Unit(nsum, V(0, 0, 1));
				const V t = AnyPerp(n), b = m::la::cross(n, t);
				std::vector<V2> poly2;
				double area = 0.0, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
				for (const V& q : pts) {
					const V d = q - cen;
					poly2.push_back({m::la::dot(d, t), m::la::dot(d, b)});
					x0 = std::min(x0, poly2.back().x); x1 = std::max(x1, poly2.back().x);
					y0 = std::min(y0, poly2.back().y); y1 = std::max(y1, poly2.back().y);
				}
				for (size_t i = 0, j = poly2.size() - 1; i < poly2.size(); j = i++)
					area += poly2[j].x * poly2[i].y - poly2[i].x * poly2[j].y;
				area = 0.5 * std::abs(area);
				const double rc = std::sqrt(area / m::kPi);
				if (rc < p.paintSingle) {                                           
					addAt(cen, 0, std::max(1e-3, rc));
					continue;
				}
				auto inside = [&](double x, double y) {
					bool in = false;
					for (size_t i = 0, j = poly2.size() - 1; i < poly2.size(); j = i++) {
						const V2 &A = poly2[i], &Bq = poly2[j];
						if ((A.y > y) != (Bq.y > y) && x < (Bq.x - A.x) * (y - A.y) / (Bq.y - A.y) + A.x) in = !in;
					}
					return in;
				};
				const int target = std::max(1, int(std::round(area / (p.paintSpacing * p.paintSpacing))));
				const int cand = target * 40;
				int got = 0;
				for (int k = 0; k < cand && got < target; ++k) {
					const double x = Lerp(x0, x1, rnd(uint64_t(k) + 1, 0)), y = Lerp(y0, y1, rnd(uint64_t(k) + 1, 1));
					if (!inside(x, y)) continue;
					const size_t before = shots.size();
					                                                                 
					bool clash = false;
					const V P0 = cen + t * x + b * y;
					for (size_t q = 0; q < shots.size() && !clash; ++q)
						clash = m::la::length(shots[q].F.o - P0) < 0.7 * p.paintSpacing;
					if (clash) continue;
					addAt(P0, uint64_t(k) + 1, 0.0);
					if (shots.size() > before) ++got;
				}
				continue;
			}
			                            
			double acc = 0.0, next = 0.5 * p.paintSpacing * rnd(0, 5);
			uint64_t sub = 1;
			for (size_t i = 0; i + 1 < pts.size(); ++i) {
				const double sl = m::la::length(pts[i + 1] - pts[i]);
				while (next <= acc + sl) {
					const V P0 = pts[i] + (pts[i + 1] - pts[i]) * ((next - acc) / sl);
					const V dir = Unit(pts[i + 1] - pts[i], V(1, 0, 0));
					V N, Sp;
					if (s.NormalNear(P0, p.paintSnap, N, Sp)) {
						const V side = Unit(m::la::cross(N, dir), AnyPerp(N));
						const double off = p.paintSpread * 0.6 * Gauss(rnd(sub, 0), rnd(sub, 1));
						const double along = p.paintSpacing * 0.25 * (rnd(sub, 2) - 0.5);
						addAt(Sp + side * off + dir * along, sub, 0.0);
					}
					++sub;
					next += p.paintSpacing * Lerp(0.6, 1.4, rnd(sub, 3));
				}
				acc += sl;
			}
		}
	}
	if (shots.empty()) return;

	                                 
	Ctx cx{&p, &s};
	std::vector<Out> outs(shots.size());
	ProjectDamageParallelFor(0, int64_t(shots.size()), [&](int64_t i) { BuildShot(cx, shots[size_t(i)], outs[size_t(i)]); });

	                                                                          
	                                                                       
	                                                                          
	                              
	ProjectDamageParallelFor(0, int64_t(shots.size()), [&](int64_t i) {
		Out& o = outs[size_t(i)];
		if (o.solids.empty()) return;
		const Shot& A = shots[size_t(i)];
		std::vector<m::Manifold> nearHoles, nearCuts;
		for (size_t j = 0; j < shots.size(); ++j) {
			const Shot& Bs = shots[j];
			const double lim = A.reach + Bs.reach;
			const double dF = m::la::length(Bs.F.o - A.F.o);
			const double dB = A.hasBack ? m::la::length(Bs.F.o - A.B.o) : 1e300;
			const double dBB = A.hasBack && Bs.hasBack ? m::la::length(Bs.B.o - A.B.o) : 1e300;
			if (std::min({dF, dB, dBB}) > lim + std::max(0.0, Bs.thick)) continue;
			if (j != size_t(i)) for (const m::Manifold& h : outs[j].holes) nearHoles.push_back(h);
			for (const m::Manifold& c : outs[j].cutters) nearCuts.push_back(c);
			for (const m::Manifold& c : outs[j].halos) nearCuts.push_back(c);
			for (const m::Manifold& c : outs[j].cracks) nearCuts.push_back(c);
		}
		if (!nearHoles.empty()) {
			const m::Manifold holes = m::Manifold::BatchBoolean(nearHoles, m::OpType::Add);
			for (m::Manifold& sld : o.solids) {
				m::Manifold cut = sld - holes;
				if (cut.Status() == m::Manifold::Error::NoError) sld = std::move(cut);
			}
		}
		if (in.box) {
			for (m::Manifold& sld : o.solids) {
				if (sld.IsEmpty()) continue;
				const m::Box bb = sld.BoundingBox();
				std::vector<m::Manifold> hit;
				for (const m::Manifold& c : nearCuts) {
					const m::Box cb = c.BoundingBox();
					if (cb.min.x <= bb.max.x && cb.max.x >= bb.min.x && cb.min.y <= bb.max.y && cb.max.y >= bb.min.y
						&& cb.min.z <= bb.max.z && cb.max.z >= bb.min.z) hit.push_back(c);
				}
				const m::Manifold cuts = hit.empty() ? m::Manifold() : m::Manifold::BatchBoolean(hit, m::OpType::Add);
				                                                               
				                                           
				std::vector<m::Manifold> keep;
				for (m::Manifold& part : sld.Decompose()) {
					const double vol = part.Volume();
					if (!(vol > 0.0)) continue;
					m::Manifold root = part ^ *in.box;
					if (!hit.empty()) root = root - cuts;
					if (root.Status() == m::Manifold::Error::NoError && root.Volume() > 0.02 * vol) keep.push_back(std::move(part));
				}
				sld = keep.empty() ? m::Manifold() : m::Manifold::BatchBoolean(keep, m::OpType::Add);
			}
		}
	});

	for (Out& o : outs) {
		for (auto& c : o.cutters) cutters.push_back(std::move(c));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
		for (auto& c : o.halos) halos.push_back(std::move(c));
		for (auto& c : o.solids) if (!c.IsEmpty()) solids.push_back(std::move(c));
	}
}
PROJECT_DAMAGE_NS_MACRO_END
