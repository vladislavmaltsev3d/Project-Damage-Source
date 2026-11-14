// создаём ржавые отверстия и неровные края вокруг них
// ProjectDamage rusty holes — GPL-3.0-or-later; see LICENSE.
//
// Recipe: rusty-holes-v1. Corroded-through holes in sheet metal.
//   1. placement: automatic clusters are area-weighted surface samples kept
//      apart by minSpacing. Paint mode reads grease pencil strokes: a dot is
//      a cluster, a small circle a cluster of that radius, a closed stroke a
//      drawn hole eaten along its rim, an open stroke a trail of holes joined
//      by a crack (split into windows, each with its own frame);
//   2. every cluster is a planar frame with a 2D corrosion field: a union of
//      discs (main lobes, rim bites, satellite pits, pits along cracks, trail
//      holes) and drawn regions, smoothly merged, on a domain-warped plane
//      with fine edge noise. Discs that coalesce give the scalloped rims with
//      sharp cusps of real perforation; pits thin out away from the holes;
//   3. the field is contoured (marching squares), unioned with the crack
//      slits (tapered meandering ribbons with forks), metal strips narrower
//      than minBridge are closed over, metal islands inside a hole are filled
//      (they would fall out), and the outlines are simplified to the grid;
//   4. each outline is extruded ONCE along the frame normal from above the
//      surface down to `thickness` below it (or through the wall, when the
//      wall is thinner), measured by rays over the outline, so the opposite
//      wall of a pipe or box is left alone. RustyOpenFloors then deletes the
//      floors of those cuts from the result: the hole is a rim `thickness`
//      deep that opens into the inside of the mesh.
// Channel map, per cluster key: 0-9 shape, 10-15 noise offsets, 100 + k*8
// lobes, 1000 + k*8 rim bites, 3000 + k*8 pits, 6000 + k*64 cracks,
// 20000 + k*8 trail holes. Placement: candidate k on (seed ^ "AUTO").
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "rusty_holes.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
using V2 = m::vec2;

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

V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

                                                                       
double SMax(double a, double b, double k)
{
	if (!(k > 0.0)) return std::max(a, b);
	const double h = std::max(k - std::abs(a - b), 0.0) / k;
	return std::max(a, b) + 0.25 * h * h * k;
}

double Area2(const m::SimplePolygon& p)
{
	double a = 0.0;
	for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += p[j].x * p[i].y - p[i].x * p[j].y;
	return 0.5 * a;
}

bool Inside2(const std::vector<V2>& poly, const V2& q)
{
	bool in = false;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		const V2 &A = poly[i], &B = poly[j];
		if ((A.y > q.y) != (B.y > q.y) && q.x < (B.x - A.x) * (q.y - A.y) / (B.y - A.y) + A.x) in = !in;
	}
	return in;
}

double SegDist(const V2& q, const V2& a, const V2& b)
{
	const V2 ab = b - a;
	const double l2 = m::la::dot(ab, ab);
	const double t = l2 > 0.0 ? ClampD(m::la::dot(q - a, ab) / l2, 0.0, 1.0) : 0.0;
	return m::la::length(q - (a + ab * t));
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> cumArea;
	ProjectDamageBVH bvh;
	double diag = 1.0;
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
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                                    

struct Disc { V2 c; double r = 0.0; };
struct Crack { std::vector<V2> pts; std::vector<double> w; };

                                                                        
                                                              
struct Cluster {
	V o, n, t, b;
	uint64_t key = 0;
	double R = 1.0;                                                               
	V2 flow{0.0, 0.0};                                                                 
	std::vector<Disc> discs;
	std::vector<std::vector<V2>> regions;                           
	std::vector<Crack> cracks;
	double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};

	void Grow(const V2& q, double pad)
	{
		lo[0] = std::min(lo[0], q.x - pad); lo[1] = std::min(lo[1], q.y - pad);
		hi[0] = std::max(hi[0], q.x + pad); hi[1] = std::max(hi[1], q.y + pad);
	}
	V World(const V2& q, double h) const { return o + t * q.x + b * q.y + n * h; }
	V2 Local(const V& P) const { const V d = P - o; return {m::la::dot(d, t), m::la::dot(d, b)}; }
};

struct Ctx {
	const RustyHoleParams* p = nullptr;
	const Surf* s = nullptr;
};

                                                                     
void MakeFrame(const RustyHoleParams& p, Cluster& C, const V& o, const V& n)
{
	C.o = o;
	C.n = n;
	const V down = Unit(V(p.downX, p.downY, p.downZ), V(0, 0, -1));
	V g = down - n * m::la::dot(down, n);
	const double gl = m::la::length(g);
	C.t = gl > 0.2 ? g / gl : AnyPerp(n);                                                
	C.b = m::la::cross(C.n, C.t);
	const double fs = ClampD(p.flow, 0.0, 1.0) * ClampD((gl - 0.2) / 0.5, 0.0, 1.0);
	C.flow = gl > 0.2 ? V2(fs, 0.0) : V2(0.0, 0.0);
}

                                                                     
                                                                         
                                                    
bool Probe(const Surf& s, const Cluster& C, const V2& q, double Hs, double reach,
	double& h, double& thick, double& room, int32_t& poly)
{
	double d = 0.0;
	const V org = C.World(q, Hs);
	if (!s.Ray(org, -C.n, Hs + reach, d, poly)) return false;
	if (m::la::dot(s.polyN[size_t(poly)], C.n) < 0.1) return false;
	h = Hs - d;
	thick = -1.0;
	room = -1.0;
	const double eps = 1e-5 * s.diag + 1e-6;
	const V hit = org - C.n * d;
	double d2 = 0.0;
	int32_t p2 = -1;
	if (s.Ray(hit - C.n * eps, -C.n, 4.0 * s.diag, d2, p2)) {
		thick = d2 + eps;
		double d3 = 0.0;
		int32_t p3 = -1;
		const V back = hit - C.n * thick;
		if (s.Ray(back - C.n * eps, -C.n, 4.0 * s.diag, d3, p3)) room = d3 + eps;
	}
	return true;
}

                                                                      
               
bool AvoidedAt(const Surf& s, const Cluster& C, const V2& q, double Hs, double reach)
{
	if (!s.avoid) return false;
	double h, th, rm;
	int32_t poly = -1;
	if (!Probe(s, C, q, Hs, reach, h, th, rm, poly)) return false;
	return s.Avoided(poly);
}

                                                                                           

                                                                            
                                                                  
void GrowCrack(const RustyHoleParams& p, Cluster& C, uint64_t key, uint64_t ch, V2 from, double ang,
	double len, double w0, double cell, int depth)
{
	auto rnd = [&](uint64_t c) { return Random01(key, ch, c); };
	Crack k;
	const double step = std::max(cell * 1.2, C.R * 0.07);
	const int n = std::max(2, int(std::ceil(len / step)));
	V2 x = from;
	const double meander = ClampD(p.crackMeander, 0.0, 2.0);
	const V off(rnd(0) * 500.0, rnd(1) * 500.0, rnd(2) * 500.0);
	for (int i = 0; i <= n; ++i) {
		const double s = double(i) / double(n);
		k.pts.push_back(x);
		const double wn = 0.75 + 0.5 * ProjectDamageNoise(off + V(s * 6.0, 0.0, 0.0));
		k.w.push_back(std::max(0.0, w0 * std::pow(1.0 - s, 0.7) * wn));
		C.Grow(x, w0 + cell);
		                                                
		ang += meander * 0.9 * (2.0 * ProjectDamageNoise(off + V(s * 3.0 * len / C.R, 7.0, 0.0)) - 1.0) * (step / C.R) * 3.0;
		ang += (rnd(10 + uint64_t(i)) - 0.5) * meander * 0.5;
		const double fl = m::la::length(C.flow);
		if (fl > 0.0) {
			const double fa = std::atan2(C.flow.y, C.flow.x);
			double da = std::remainder(fa - ang, 2.0 * m::kPi);
			ang += da * 0.12 * fl;
		}
		if (i < n) {
			                     
			if (rnd(200 + uint64_t(i)) < ClampD(p.crackPits, 0.0, 1.0) * 0.12 && s > 0.1 && s < 0.85) {
				const double r = std::max(1.4 * cell, w0 * Lerp(1.5, 4.5, std::pow(rnd(300 + uint64_t(i)), 2.0)));
				C.discs.push_back({x + V2((rnd(400 + uint64_t(i)) - 0.5) * r, (rnd(500 + uint64_t(i)) - 0.5) * r), r});
				C.Grow(C.discs.back().c, r);
			}
			         
			if (depth < 2 && s > 0.15 && s < 0.7 && rnd(600 + uint64_t(i)) < ClampD(p.crackFork, 0.0, 1.0) * 0.08) {
				const double side = rnd(700 + uint64_t(i)) < 0.5 ? -1.0 : 1.0;
				GrowCrack(p, C, key, ch * 31 + 7 + uint64_t(i), x, ang + side * Lerp(0.4, 0.9, rnd(800 + uint64_t(i))),
					len * (1.0 - s) * Lerp(0.35, 0.7, rnd(900 + uint64_t(i))), k.w.back() * 0.8, cell, depth + 1);
			}
			x = x + V2(std::cos(ang), std::sin(ang)) * step;
		}
	}
	C.cracks.push_back(std::move(k));
}

                                                                     
double RimAlong(const Cluster& C, size_t ndisc, const V2& c, double a)
{
	const V2 d(std::cos(a), std::sin(a));
	double best = 0.0;
	for (size_t i = 0; i < ndisc; ++i) {
		const Disc& D = C.discs[i];
		const V2 w = c - D.c;
		const double bq = m::la::dot(w, d), cq = m::la::dot(w, w) - D.r * D.r;
		const double disc = bq * bq - cq;
		if (disc < 0.0) continue;
		best = std::max(best, -bq + std::sqrt(disc));
	}
	return best;
}

                                                                            
                                      
void AddPit(const RustyHoleParams& p, Cluster& C, const V2& at, double rel, double u, double cell)
{
	const double r = std::max(1.3 * cell, ClampD(p.pitSize, 0.0, 2.0) * C.R * (0.3 + 1.7 * u * u * u) * (1.0 - 0.55 * rel));
	C.discs.push_back({at, r});
	C.Grow(at, r);
}

                                                               
void ShapeAuto(const RustyHoleParams& p, Cluster& C, double cell)
{
	const uint64_t key = C.key;
	auto rnd = [&](uint64_t ch) { return Random01(key, 0, ch); };
	const double R = C.R;
	const double st = 1.0 + ClampD(p.stretch, 0.0, 3.0) * m::la::length(C.flow) / std::max(1e-9, ClampD(p.flow, 0.0, 1.0) + 1e-9);
	const V2 fd = m::la::length(C.flow) > 0.0 ? m::la::normalize(C.flow) : V2(1.0, 0.0);
	const V2 sd(-fd.y, fd.x);
	auto ell = [&](double x, double y) { return fd * (x * st) + sd * y; };
	             
	const int lobes = std::max(1, std::min(p.lobes, 24));
	C.discs.push_back({V2(0.0, 0.0), R * Lerp(0.45, 0.62, rnd(0))});
	for (int k = 1; k < lobes; ++k) {
		auto r = [&](uint64_t c) { return rnd(100 + uint64_t(k) * 8 + c); };
		const double a = r(0) * 2.0 * m::kPi;
		const double d = R * ClampD(p.lobeSpread, 0.0, 2.0) * std::sqrt(r(1)) * Lerp(0.6, 1.1, r(2));
		V2 c = ell(std::cos(a) * d, std::sin(a) * d) + fd * (m::la::length(C.flow) * R * 0.35 * r(3));
		C.discs.push_back({c, R * Lerp(0.22, 0.5, r(4))});
	}
	const size_t core = C.discs.size();
	for (const Disc& D : C.discs) C.Grow(D.c, D.r);
	                                                                           
	const int bites = int(std::round(ClampD(p.scallop, 0.0, 5.0) * (10.0 + 3.0 * lobes)));
	for (int k = 0; k < bites; ++k) {
		auto r = [&](uint64_t c) { return rnd(1000 + uint64_t(k) * 8 + c); };
		const Disc& L = C.discs[size_t(r(0) * double(core)) % core];
		const double a = r(1) * 2.0 * m::kPi;
		const V2 c = L.c + V2(std::cos(a), std::sin(a)) * (L.r * Lerp(0.85, 1.08, r(2)));
		const double rr = std::max(1.3 * cell, R * Lerp(0.07, 0.2, r(3) * r(3)));
		C.discs.push_back({c, rr});
		C.Grow(c, rr);
	}
	                                                          
	const int pits = std::max(0, std::min(p.pitAmount, 2000));
	const double spread = std::max(1.0, p.pitSpread);
	for (int k = 0; k < pits; ++k) {
		auto r = [&](uint64_t c) { return rnd(3000 + uint64_t(k) * 8 + c); };
		const double rel = std::pow(r(0), 2.2);
		const double d = R * Lerp(0.7, spread, rel);
		const double a = r(1) * 2.0 * m::kPi;
		V2 c = ell(std::cos(a) * d, std::sin(a) * d);
		c = c + fd * (m::la::length(C.flow) * d * 0.5 * r(2));
		AddPit(p, C, c, rel, r(3), cell);
	}
	                        
	const int cracks = std::max(0, std::min(p.crackAmount, 32));
	for (int k = 0; k < cracks; ++k) {
		auto r = [&](uint64_t c) { return rnd(6000 + uint64_t(k) * 64 + c); };
		double a = r(0) * 2.0 * m::kPi;
		const double fl = m::la::length(C.flow);
		if (r(1) < fl) a = std::atan2(fd.y, fd.x) + (r(2) - 0.5) * 1.2;
		const double rim = RimAlong(C, core, V2(0.0, 0.0), a);
		const V2 from = V2(std::cos(a), std::sin(a)) * (rim * 0.92);
		GrowCrack(p, C, key, 6000 + uint64_t(k) * 64 + 8, from, a, R * std::max(0.0, p.crackLength) * Lerp(0.5, 1.2, r(3)),
			std::max(1e-4, p.crackWidth) * Lerp(0.8, 1.4, r(4)), cell, 0);
	}
}

                                                                
std::vector<V2> Resample(const std::vector<V2>& in, double step, bool closed)
{
	std::vector<V2> out;
	if (in.size() < 2) return in;
	const size_t n = in.size();
	const size_t segs = closed ? n : n - 1;
	double carry = 0.0;
	out.push_back(in[0]);
	for (size_t i = 0; i < segs; ++i) {
		const V2 a = in[i], b = in[(i + 1) % n];
		const double l = m::la::length(b - a);
		double t = step - carry;
		while (t < l) { out.push_back(a + (b - a) * (t / l)); t += step; }
		carry = l - (t - step);
	}
	if (closed && out.size() > 1 && m::la::length(out.back() - out.front()) < 0.3 * step) out.pop_back();
	if (!closed && m::la::length(out.back() - in.back()) > 1e-9) out.push_back(in.back());
	return out;
}

                                                                          
                    
void ShapeRegion(const RustyHoleParams& p, Cluster& C, const std::vector<V2>& poly, double cell)
{
	const uint64_t key = C.key;
	auto rnd = [&](uint64_t ch) { return Random01(key, 0, ch); };
	std::vector<V2> ring = Resample(poly, std::max(cell * 1.5, 1e-4), true);
	if (ring.size() < 3) return;
	m::SimplePolygon sp(ring.begin(), ring.end());
	if (Area2(sp) < 0.0) std::reverse(ring.begin(), ring.end());
	double per = 0.0;
	for (size_t i = 0; i < ring.size(); ++i) per += m::la::length(ring[(i + 1) % ring.size()] - ring[i]);
	for (const V2& q : ring) C.Grow(q, cell);
	C.regions.push_back(ring);
	const double R = C.R;
	auto rimPoint = [&](double u, V2& at, V2& out) {
		double target = u * per, acc = 0.0;
		for (size_t i = 0; i < ring.size(); ++i) {
			const V2 a = ring[i], b = ring[(i + 1) % ring.size()];
			const double l = m::la::length(b - a);
			if (acc + l >= target || i + 1 == ring.size()) {
				const double f = l > 0.0 ? ClampD((target - acc) / l, 0.0, 1.0) : 0.0;
				at = a + (b - a) * f;
				const V2 tg = l > 0.0 ? (b - a) / l : V2(1.0, 0.0);
				out = V2(tg.y, -tg.x);                                       
				return;
			}
			acc += l;
		}
	};
	const double loops = per / (2.0 * m::kPi * R);
	const int bites = int(std::round(ClampD(p.scallop, 0.0, 5.0) * 22.0 * loops));
	for (int k = 0; k < std::min(bites, 4000); ++k) {
		auto r = [&](uint64_t c) { return rnd(1000 + uint64_t(k) * 8 + c); };
		V2 at, out;
		rimPoint(r(0), at, out);
		const double rr = std::max(1.3 * cell, R * Lerp(0.08, 0.26, r(1) * r(1)));
		const V2 c = at + out * (rr * Lerp(-0.4, 0.35, r(2)));
		C.discs.push_back({c, rr});
		C.Grow(c, rr);
	}
	const int pits = std::min(4000, int(std::round(std::max(0, p.pitAmount) * std::max(1.0, loops))));
	const double spread = std::max(1.0, p.pitSpread);
	for (int k = 0; k < pits; ++k) {
		auto r = [&](uint64_t c) { return rnd(3000 + uint64_t(k) * 8 + c); };
		V2 at, out;
		rimPoint(r(0), at, out);
		const double rel = std::pow(r(1), 1.5);
		V2 c = at + out * (R * (spread - 1.0) * rel + R * 0.1);
		c = c + C.flow * (R * 0.6 * rel * r(2));
		AddPit(p, C, c, rel, r(3), cell);
	}
	const int cracks = std::min(64, int(std::round(std::max(0, p.crackAmount) * std::max(1.0, loops))));
	for (int k = 0; k < cracks; ++k) {
		auto r = [&](uint64_t c) { return rnd(6000 + uint64_t(k) * 64 + c); };
		V2 at, out;
		rimPoint(r(0), at, out);
		double a = std::atan2(out.y, out.x);
		const double fl = m::la::length(C.flow);
		if (fl > 0.0 && m::la::dot(out, C.flow) > 0.0 && r(1) < fl) a = std::atan2(C.flow.y, C.flow.x) + (r(2) - 0.5) * 1.0;
		GrowCrack(p, C, key, 6000 + uint64_t(k) * 64 + 8, at - out * cell, a, R * std::max(0.0, p.crackLength) * Lerp(0.5, 1.2, r(3)),
			std::max(1e-4, p.crackWidth) * Lerp(0.8, 1.4, r(4)), cell, 0);
	}
}

                                                                          
                                     
void ShapeTrail(const RustyHoleParams& p, Cluster& C, const std::vector<V2>& line, double s0, double s1, double cell)
{
	const uint64_t key = C.key;
	auto rnd = [&](uint64_t ch) { return Random01(key, 0, ch); };
	const double size = std::max(1e-3, p.paintSize);
	const double spacing = std::max(0.2, p.paintSpacing) * size;
	                         
	std::vector<double> acc(line.size(), 0.0);
	for (size_t i = 1; i < line.size(); ++i) acc[i] = acc[i - 1] + m::la::length(line[i] - line[i - 1]);
	auto at = [&](double s, V2& q, V2& tg) {
		s = ClampD(s, 0.0, acc.back());
		size_t i = size_t(std::upper_bound(acc.begin(), acc.end(), s) - acc.begin());
		i = std::max<size_t>(1, std::min(i, line.size() - 1));
		const double l = acc[i] - acc[i - 1];
		const double f = l > 0.0 ? (s - acc[i - 1]) / l : 0.0;
		q = line[i - 1] + (line[i] - line[i - 1]) * f;
		tg = l > 0.0 ? (line[i] - line[i - 1]) / l : V2(1.0, 0.0);
	};
	int k = 0;
	for (double s = s0 + spacing * 0.5 * rnd(1); s < s1; s += spacing * Lerp(0.6, 1.4, rnd(20000 + uint64_t(k) * 8 + 7)), ++k) {
		auto r = [&](uint64_t c) { return rnd(20000 + uint64_t(k) * 8 + c); };
		if (r(0) < ClampD(p.paintGaps, 0.0, 1.0)) continue;
		V2 q, tg;
		at(s, q, tg);
		const V2 side(-tg.y, tg.x);
		const double rr = std::max(1.3 * cell, size * Lerp(0.35, 1.25, std::pow(r(1), 1.6)));
		const V2 c = q + side * (0.25 * size * Gauss(std::max(1e-9, r(2)), r(3)));
		C.discs.push_back({c, rr});
		C.Grow(c, rr);
		                                          
		if (rr > 0.8 * size && r(4) < 0.6) {
			const double a = r(5) * 2.0 * m::kPi;
			const V2 c2 = c + V2(std::cos(a), std::sin(a)) * (rr * 0.8);
			C.discs.push_back({c2, rr * 0.55});
			C.Grow(c2, rr * 0.55);
		}
	}
	                                                                      
	if (p.paintCrack) {
		Crack kc;
		const double step = std::max(cell * 1.5, size * 0.25);
		const double w0 = std::max(1e-4, p.crackWidth);
		const V off(rnd(10) * 500.0, rnd(11) * 500.0, 0.0);
		for (double s = std::max(0.0, s0 - step); s <= std::min(acc.back(), s1 + step) + 1e-9; s += step) {
			V2 q, tg;
			at(s, q, tg);
			const V2 side(-tg.y, tg.x);
			const double wob = (2.0 * ProjectDamageNoise(off + V(s / size * 0.8, 0.0, 0.0)) - 1.0) * size * 0.25 * ClampD(p.crackMeander, 0.0, 2.0);
			const double tip = std::min(1.0, std::min(s, acc.back() - s) / (3.0 * size) + 0.15);
			kc.pts.push_back(q + side * wob);
			kc.w.push_back(w0 * tip * (0.7 + 0.6 * ProjectDamageNoise(off + V(s / size * 2.0, 3.0, 0.0))));
			C.Grow(kc.pts.back(), w0 + cell);
		}
		if (kc.pts.size() >= 2) C.cracks.push_back(std::move(kc));
	}
	                        
	const double wl = std::max(0.0, s1 - s0);
	const int pits = std::min(2000, int(std::round(std::max(0, p.pitAmount) * wl / (2.0 * m::kPi * size * 2.0))));
	const double spread = std::max(1.0, p.pitSpread);
	for (int j = 0; j < pits; ++j) {
		auto r = [&](uint64_t c) { return rnd(3000 + uint64_t(j) * 8 + c); };
		V2 q, tg;
		at(Lerp(s0, s1, r(0)), q, tg);
		const V2 side(-tg.y, tg.x);
		const double rel = std::pow(r(1), 1.5);
		const V2 c = q + side * ((r(2) < 0.5 ? -1.0 : 1.0) * size * Lerp(0.5, spread, rel)) + C.flow * (size * rel * r(4));
		AddPit(p, C, c, rel, r(3), cell);
	}
	                    
	const int cracks = std::min(16, int(std::round(std::max(0, p.crackAmount) * 0.5 * wl / (4.0 * size))));
	for (int j = 0; j < cracks; ++j) {
		auto r = [&](uint64_t c) { return rnd(6000 + uint64_t(j) * 64 + c); };
		V2 q, tg;
		at(Lerp(s0, s1, r(0)), q, tg);
		const V2 side(-tg.y, tg.x);
		const double sg = r(1) < 0.5 ? -1.0 : 1.0;
		const double a = std::atan2(side.y * sg, side.x * sg) + (r(2) - 0.5) * 1.2;
		GrowCrack(p, C, key, 6000 + uint64_t(j) * 64 + 8, q, a, size * std::max(0.0, p.crackLength) * Lerp(1.0, 2.5, r(3)),
			std::max(1e-4, p.crackWidth) * Lerp(0.7, 1.2, r(4)), cell, 1);
	}
}

                                                                                          

struct Field {
	const Cluster* C = nullptr;
	double warpAmp = 0.0, warpFreq = 1.0, roughAmp = 0.0, roughFreq = 1.0, merge = 0.0;
	double laceBand = 0.0, laceFreq = 1.0;
	V offW1, offW2, offR, offL;
	                           
	double bx0 = 0.0, by0 = 0.0, bs = 1.0;
	int bnx = 1, bny = 1;
	std::vector<std::vector<int32_t>> buckets;
	std::vector<double> regionLen;

	void Build(double cell)
	{
		const Cluster& K = *C;
		double rmax = cell;
		for (const Disc& D : K.discs) rmax = std::max(rmax, D.r);
		bs = std::max(rmax * 2.0, cell * 8.0);
		bx0 = K.lo[0]; by0 = K.lo[1];
		bnx = std::max(1, std::min(512, int(std::ceil((K.hi[0] - K.lo[0]) / bs)) + 1));
		bny = std::max(1, std::min(512, int(std::ceil((K.hi[1] - K.lo[1]) / bs)) + 1));
		bs = std::max(bs, std::max((K.hi[0] - K.lo[0]) / double(bnx - 1 > 0 ? bnx - 1 : 1), (K.hi[1] - K.lo[1]) / double(bny - 1 > 0 ? bny - 1 : 1)));
		buckets.assign(size_t(bnx) * size_t(bny), {});
		const double reach = warpAmp * 1.5 + merge + laceBand * 3.0;
		for (size_t i = 0; i < K.discs.size(); ++i) {
			const Disc& D = K.discs[i];
			const double e = D.r + reach + D.r * merge;
			const int x0 = std::max(0, int(std::floor((D.c.x - e - bx0) / bs))), x1 = std::min(bnx - 1, int(std::floor((D.c.x + e - bx0) / bs)));
			const int y0 = std::max(0, int(std::floor((D.c.y - e - by0) / bs))), y1 = std::min(bny - 1, int(std::floor((D.c.y + e - by0) / bs)));
			for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) buckets[size_t(y) * size_t(bnx) + size_t(x)].push_back(int32_t(i));
		}
	}

	double operator()(const V2& p) const
	{
		const Cluster& K = *C;
		V2 q = p;
		if (warpAmp > 0.0) {
			const V w(p.x * warpFreq, p.y * warpFreq, 0.0);
			q = p + V2(Fbm(w + offW1, 3), Fbm(w + offW2, 3)) * warpAmp;
		}
		double f = -1e30;
		const int bx = std::max(0, std::min(bnx - 1, int(std::floor((q.x - bx0) / bs))));
		const int by = std::max(0, std::min(bny - 1, int(std::floor((q.y - by0) / bs))));
		for (int32_t i : buckets[size_t(by) * size_t(bnx) + size_t(bx)]) {
			const Disc& D = K.discs[size_t(i)];
			const double v = D.r - m::la::length(q - D.c);
			f = SMax(f, v, merge * D.r);
		}
		for (const std::vector<V2>& reg : K.regions) {
			double d = 1e300;
			for (size_t i = 0, j = reg.size() - 1; i < reg.size(); j = i++) d = std::min(d, SegDist(q, reg[j], reg[i]));
			const double v = Inside2(reg, q) ? d : -d;
			f = SMax(f, v, merge * K.R * 0.3);
		}
		                                                                   
		if (laceBand > 0.0 && f > -laceBand * 3.0) {
			const double d = std::max(0.0, -f);
			const double L = 0.5 + 0.5 * Fbm(V(q.x * laceFreq, q.y * laceFreq, 0.0) + offL, 3);
			const double fl = (L - 0.56 - 0.25 * d / laceBand) / (1.6 * laceFreq);
			f = SMax(f, fl, merge * 0.5 / laceFreq);
		}
		if (roughAmp > 0.0) f += roughAmp * Fbm(V(p.x * roughFreq, p.y * roughFreq, 0.0) + offR, 3);
		return f;
	}
};

                                                                      
                                              
m::Polygons Contour(const Field& F, double x0, double y0, double cell, int nx, int ny)
{
	std::vector<double> g(size_t(nx) * size_t(ny));
	for (int j = 0; j < ny; ++j)
		for (int i = 0; i < nx; ++i) {
			double v = F(V2(x0 + i * cell, y0 + j * cell));
			if (i == 0 || j == 0 || i == nx - 1 || j == ny - 1) v = std::min(v, -1e-6);
			if (v == 0.0) v = -1e-12;
			g[size_t(j) * size_t(nx) + size_t(i)] = v;
		}
	auto G = [&](int i, int j) { return g[size_t(j) * size_t(nx) + size_t(i)]; };
	                                                                                              
	auto hid = [&](int i, int j) { return int64_t(2) * (int64_t(j) * nx + i); };
	auto vid = [&](int i, int j) { return int64_t(2) * (int64_t(j) * nx + i) + 1; };
	auto point = [&](int64_t id) {
		const int64_t base = id / 2;
		const int i = int(base % nx), j = int(base / nx);
		const double a = G(i, j);
		if ((id & 1) == 0) { const double b = G(i + 1, j); const double t = a / (a - b); return V2(x0 + (i + t) * cell, y0 + j * cell); }
		const double b = G(i, j + 1); const double t = a / (a - b); return V2(x0 + i * cell, y0 + (j + t) * cell);
	};
	std::unordered_map<int64_t, int64_t> next;                                    
	next.reserve(size_t(nx) * 4);
	for (int j = 0; j + 1 < ny; ++j)
		for (int i = 0; i + 1 < nx; ++i) {
			const double a = G(i, j), b = G(i + 1, j), c = G(i + 1, j + 1), d = G(i, j + 1);
			const int code = (a > 0 ? 1 : 0) | (b > 0 ? 2 : 0) | (c > 0 ? 4 : 0) | (d > 0 ? 8 : 0);
			if (code == 0 || code == 15) continue;
			const int64_t eB = hid(i, j), eR = vid(i + 1, j), eT = hid(i, j + 1), eL = vid(i, j);
			                                                       
			auto seg = [&](int64_t from, int64_t to) { next[from] = to; };
			switch (code) {
			case 1: seg(eL, eB); break;
			case 2: seg(eB, eR); break;
			case 3: seg(eL, eR); break;
			case 4: seg(eR, eT); break;
			case 6: seg(eB, eT); break;
			case 7: seg(eL, eT); break;
			case 8: seg(eT, eL); break;
			case 9: seg(eT, eB); break;
			case 11: seg(eT, eR); break;
			case 12: seg(eR, eL); break;
			case 13: seg(eR, eB); break;
			case 14: seg(eB, eL); break;
			case 5: {
				const bool joined = (a + b + c + d) * 0.25 > 0.0;
				if (joined) { seg(eL, eT); seg(eR, eB); } else { seg(eL, eB); seg(eR, eT); }
				break;
			}
			case 10: {
				const bool joined = (a + b + c + d) * 0.25 > 0.0;
				if (joined) { seg(eB, eL); seg(eT, eR); } else { seg(eB, eR); seg(eT, eL); }
				break;
			}
			default: break;
			}
		}
	m::Polygons loops;
	std::unordered_map<int64_t, bool> used;
	used.reserve(next.size());
	for (const auto& kv : next) {
		if (used.count(kv.first)) continue;
		m::SimplePolygon loop;
		int64_t e = kv.first;
		size_t guard = 0;
		while (!used.count(e) && guard++ < next.size() + 2) {
			used[e] = true;
			loop.push_back(point(e));
			auto it = next.find(e);
			if (it == next.end()) break;
			e = it->second;
		}
		                                                                                          
		std::reverse(loop.begin(), loop.end());
		if (loop.size() >= 3) loops.push_back(std::move(loop));
	}
	return loops;
}

m::CrossSection CrackSection(const Crack& k)
{
	const size_t n = k.pts.size();
	if (n < 2) return m::CrossSection();
	std::vector<V2> left, right;
	for (size_t i = 0; i + 1 < n; ++i) {
		const V2 a = k.pts[i > 0 ? i - 1 : 0], b = k.pts[i + 1];
		V2 tg = b - a;
		const double l = m::la::length(tg);
		tg = l > 0.0 ? tg / l : V2(1.0, 0.0);
		const V2 nr(-tg.y, tg.x);
		const double w = 0.5 * std::max(0.0, k.w[i]);
		left.push_back(k.pts[i] + nr * w);
		right.push_back(k.pts[i] - nr * w);
	}
	m::SimplePolygon poly;
	for (const V2& q : right) poly.push_back(q);
	poly.push_back(k.pts[n - 1]);
	for (size_t i = left.size(); i-- > 0;) poly.push_back(left[i]);
	if (poly.size() < 3) return m::CrossSection();
	if (Area2(poly) < 0.0) std::reverse(poly.begin(), poly.end());
	return m::CrossSection(m::Polygons{poly}, m::CrossSection::FillRule::NonZero);
}

                                                                                          

void BuildCluster(const Ctx& cx, const Cluster& C, double cell, std::vector<m::Manifold>& out)
{
	const RustyHoleParams& p = *cx.p;
	const Surf& s = *cx.s;
	if (C.discs.empty() && C.regions.empty() && C.cracks.empty()) return;
	auto rnd = [&](uint64_t ch) { return Random01(C.key, 0, ch); };

	Field F;
	F.C = &C;
	F.warpAmp = ClampD(p.jagged, 0.0, 3.0) * 0.3 * C.R;
	F.warpFreq = 1.0 / std::max(1e-6, 0.65 * C.R);
	F.roughAmp = ClampD(p.roughness, 0.0, 4.0) * 0.07 * C.R;
	F.roughFreq = 1.0 / std::max(1e-6, std::max(0.09 * C.R, 2.5 * cell));
	F.merge = ClampD(p.merge, 0.0, 2.0);
	F.offW1 = V(rnd(10) * 900.0, rnd(11) * 900.0, rnd(12) * 900.0);
	F.offW2 = V(rnd(13) * 900.0, rnd(14) * 900.0, rnd(15) * 900.0);
	F.offR = V(rnd(16) * 900.0, rnd(17) * 900.0, rnd(18) * 900.0);
	F.offL = V(rnd(19) * 900.0, rnd(20) * 900.0, rnd(21) * 900.0);
	F.laceBand = ClampD(p.lace, 0.0, 3.0) * 0.45 * C.R;
	F.laceFreq = 1.0 / std::max(1e-6, std::max(0.07 * C.R, 2.0 * cell));
	F.Build(cell);

	const double pad = F.warpAmp * 1.6 + 2.0 * cell + F.roughAmp + F.laceBand * 1.5;
	const double x0 = C.lo[0] - pad, y0 = C.lo[1] - pad;
	const int nx = int(std::ceil((C.hi[0] + pad - x0) / cell)) + 1;
	const int ny = int(std::ceil((C.hi[1] + pad - y0) / cell)) + 1;
	if (nx < 3 || ny < 3 || double(nx) * double(ny) > 4.0e6) return;

	m::Polygons loops = Contour(F, x0, y0, cell, nx, ny);
	std::vector<m::CrossSection> parts;
	if (!loops.empty()) parts.push_back(m::CrossSection(loops, m::CrossSection::FillRule::Positive));
	for (const Crack& k : C.cracks) {
		m::CrossSection cs = CrackSection(k);
		if (!cs.IsEmpty()) parts.push_back(cs);
	}
	if (parts.empty()) return;
	m::CrossSection holes = m::CrossSection::BatchBoolean(parts, m::OpType::Add);
	                                                                  
	const double bridge = std::max(0.0, p.minBridge) * 0.5;
	if (bridge > 0.0)
		holes = holes.Offset(bridge, m::CrossSection::JoinType::Miter, 2.0).Offset(-bridge, m::CrossSection::JoinType::Miter, 2.0);
	holes = holes.Simplify(0.45 * cell);

	const double T = std::max(1e-4, p.thickness);
	const double Hs = 0.5 * C.R + 2.0 * T + 4.0 * cell + 1e-3;
	const double reach = 1.5 * C.R + 4.0 * T + 1e-3;
	                                                                      
	double up = 1e300;
	{
		double d = 0.0;
		int32_t poly = -1;
		if (s.Ray(C.o + C.n * (1e-5 * s.diag + 1e-6), C.n, 4.0 * s.diag, d, poly)) up = d;
	}

	for (const m::CrossSection& comp : holes.Decompose()) {
		                                                                          
		m::Polygons polys = comp.ToPolygons();
		size_t best = 0;
		double bestA = 0.0;
		for (size_t i = 0; i < polys.size(); ++i) {
			const double a = Area2(polys[i]);
			if (a > bestA) { bestA = a; best = i; }
		}
		if (!(bestA > 3.0 * cell * cell)) continue;
		m::SimplePolygon outer = polys[best];
		if (outer.size() < 3) continue;
		                                                   
		std::vector<V2> samples;
		const size_t stride = std::max<size_t>(1, outer.size() / 400);
		for (size_t i = 0; i < outer.size(); i += stride) samples.push_back(outer[i]);
		const m::Rect bb = comp.Bounds();
		std::vector<V2> outerV(outer.begin(), outer.end());
		const int gn = 6;
		for (int j = 0; j < gn; ++j)
			for (int i = 0; i < gn; ++i) {
				const V2 q(Lerp(bb.min.x, bb.max.x, (i + 0.5) / gn), Lerp(bb.min.y, bb.max.y, (j + 0.5) / gn));
				if (Inside2(outerV, q)) samples.push_back(q);
			}
		                                                                      
		                                                                
		double top = -1e300, low = 1e300, back = 1e300, floorLim = -1e300;
		int hits = 0;
		bool through = true;
		for (const V2& q : samples) {
			double h, th, rm;
			int32_t poly = -1;
			if (!Probe(s, C, q, Hs, reach, h, th, rm, poly)) continue;
			++hits;
			top = std::max(top, h);
			low = std::min(low, h);
			if (th < 0.0 || th > T) { through = false; continue; }
			back = std::min(back, h - th);
			if (rm > 0.0) floorLim = std::max(floorLim, h - th - 0.5 * rm);
		}
		if (hits == 0) continue;
		const double margin = std::max(0.25 * T, 0.5 * cell) + 1e-4;
		top += margin;
		if (up < 1e299) top = std::min(top, 0.5 * up);
		double bot = through ? back - margin : low - T;
		bot = std::max(bot, floorLim);
		if (!(top - bot > 1e-5)) continue;
		m::Manifold prism = m::Manifold::Extrude(m::Polygons{outer}, top - bot);
		if (prism.Status() != m::Manifold::Error::NoError || prism.IsEmpty()) continue;
		const m::mat3x4 X(C.t, C.b, C.n, C.o + C.n * bot);
		prism = prism.Transform(X);
		if (prism.Status() == m::Manifold::Error::NoError && !prism.IsEmpty()) out.push_back(std::move(prism));
	}
}

double CellFor(const RustyHoleParams& p, double R)
{
	return std::max(1e-4, R * 0.035 / ClampD(p.detail, 0.05, 4.0));
}

}               

void PlaceRustyHoles(const CoreInput& in, const RustyHoleParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters)
{
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	RustyHoleParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.sizeMin = std::max(1e-3, p.sizeMin);
	p.sizeMax = std::max(1e-3, p.sizeMax);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	p.amount = std::max(0, std::min(p.amount, 2000));
	p.paintSnap = std::max(1e-4, p.paintSnap);
	p.paintSize = std::max(1e-3, p.paintSize);
	const bool paint = p.mode == 1;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && p.amount <= 0) return;

	Surf s;
	BuildSurf(in, p.avoid, s);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x5255535459484f4cULL)                 
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x5248;                             
	const double sizeExp = std::pow(4.0, 2.0 * ClampD(p.sizeBias, 0.0, 1.0) - 1.0);

	struct Job { Cluster C; double cell = 1.0; };
	std::vector<Job> jobs;

	auto drop = [&](const V& P, const V& N, double reach, V& S, V& Ns, int32_t& poly) {
		double d = 0.0;
		if (!s.Ray(P + N * reach, -N, 2.0 * reach, d, poly)) return false;
		S = P + N * reach - N * d;
		Ns = s.polyN[size_t(poly)];
		return m::la::dot(Ns, N) > 0.3 && !s.Avoided(poly);
	};
	                                     
	auto addCluster = [&](const V& P, const V& N, uint64_t key, int32_t maskIdx, double rForce) {
		if (mask && !mask->Keep(P, maskIdx, kStream)) return;
		const double sc = mask ? mask->SizeScale(P) : 1.0;
		if (!(sc > 0.0)) return;
		const double R = (rForce > 0.0 ? rForce : Lerp(p.sizeMin, p.sizeMax, std::pow(Random01(key, 0, 1), sizeExp))) * sc;
		Job J;
		J.C.key = key;
		J.C.R = R;
		MakeFrame(p, J.C, P, N);
		J.cell = CellFor(p, R);
		ShapeAuto(p, J.C, J.cell);
		jobs.push_back(std::move(J));
	};

	if (!paint) {
		const uint64_t as = seed ^ 0x4155544fULL;            
		struct Placed { V o; double r; };
		std::vector<Placed> placed;
		const int maxCand = p.amount * 40 + 100;
		int accepted = 0;
		for (int k = 0; k < maxCand && accepted < p.amount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(as, uint64_t(k), ch); };
			V P;
			int32_t poly = -1;
			if (!s.Sample(rnd(0), rnd(1), rnd(2), P, poly)) break;
			if (s.Avoided(poly)) continue;
			const V N = s.polyN[size_t(poly)];
			                                                                 
			                                                    
			{
				const V T0 = AnyPerp(N), T1 = m::la::cross(N, T0);
				const double w = 0.5 * p.sizeMin;
				int ok = 0;
				for (int j = 0; j < 4; ++j) {
					const V d = (j & 1 ? -1.0 : 1.0) * (j < 2 ? T0 : T1);
					V S, Ns;
					int32_t pp = -1;
					if (drop(P + d * w, N, w, S, Ns, pp) && m::la::dot(Ns, N) > 0.7) ++ok;
				}
				if (ok < 4) continue;
			}
			const uint64_t key = seed ^ ((uint64_t(k) + 1) * 0xa24baed4963ee407ULL);
			const double R = Lerp(p.sizeMin, p.sizeMax, std::pow(Random01(key, 0, 1), sizeExp)) * (mask ? mask->SizeScale(P) : 1.0);
			bool clash = false;
			for (const Placed& q : placed) {
				const double lim = std::max(0.0, p.minSpacing) * (q.r + R);
				if (m::la::dot(q.o - P, q.o - P) < lim * lim) { clash = true; break; }
			}
			if (clash) continue;
			const size_t before = jobs.size();
			addCluster(P, N, key, int32_t(k), 0.0);
			if (jobs.size() > before) { placed.push_back({P, R}); ++accepted; }
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
			auto keyOf = [&](uint64_t sub) { return ps ^ (((uint64_t(si) << 20) + sub + 1) * 0xa24baed4963ee407ULL); };
			auto snapTo = [&](const V& P0, V& P, V& N) {
				V Nn, Sp;
				if (!s.NormalNear(P0, p.paintSnap, Nn, Sp)) return false;
				int32_t poly = -1;
				return drop(Sp, Nn, std::max(p.paintSnap, 1e-3), P, N, poly);
			};
			V cen(0.0);
			for (const V& q : pts) cen = cen + q;
			cen = cen / double(pts.size());
			if (pts.size() < 2 || len < p.paintSingle) {
				V P, N;
				if (snapTo(cen, P, N)) addCluster(P, N, keyOf(0), int32_t(si << 12), 0.0);
				continue;
			}
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
				if (!(m::la::length(nsum) > 0.0)) continue;
				const V n = Unit(nsum, V(0, 0, 1));
				V P, N;
				int32_t poly = -1;
				if (!drop(cen, n, std::max(p.paintSnap, 1e-3) + len, P, N, poly)) {
					                                                      
					P = cen;
				}
				Cluster probe;
				MakeFrame(p, probe, P, n);
				std::vector<V2> poly2;
				for (const V& q : pts) poly2.push_back(probe.Local(q));
				m::SimplePolygon sp(poly2.begin(), poly2.end());
				const double area = std::abs(Area2(sp));
				const double rc = std::sqrt(area / m::kPi);
				if (rc < p.paintSingle) {
					V Ps, Ns;
					if (snapTo(cen, Ps, Ns)) addCluster(Ps, Ns, keyOf(0), int32_t(si << 12), std::max(1e-3, rc));
					continue;
				}
				const uint64_t key = keyOf(0);
				if (mask && !mask->Keep(P, int32_t(si << 12), kStream)) continue;
				Job J;
				J.C = probe;
				J.C.key = key;
				J.C.R = std::min(rc, p.sizeMax);
				J.cell = std::max(CellFor(p, J.C.R), std::sqrt(area) / 700.0);
				ShapeRegion(p, J.C, poly2, J.cell);
				jobs.push_back(std::move(J));
				continue;
			}
			                                                               
			const double win = std::max(10.0 * p.paintSize, 4.0);
			const int nw = std::max(1, int(std::ceil(len / win)));
			std::vector<double> acc(pts.size(), 0.0);
			for (size_t i = 1; i < pts.size(); ++i) acc[i] = acc[i - 1] + m::la::length(pts[i] - pts[i - 1]);
			for (int w = 0; w < nw; ++w) {
				const double s0 = len * double(w) / nw, s1 = len * double(w + 1) / nw;
				const double sm = 0.5 * (s0 + s1);
				size_t i = size_t(std::upper_bound(acc.begin(), acc.end(), sm) - acc.begin());
				i = std::max<size_t>(1, std::min(i, pts.size() - 1));
				const double l = acc[i] - acc[i - 1];
				const V Pm = pts[i - 1] + (pts[i] - pts[i - 1]) * (l > 0.0 ? (sm - acc[i - 1]) / l : 0.0);
				V P, N;
				if (!snapTo(Pm, P, N)) continue;
				if (mask && !mask->Keep(P, int32_t((si << 12) + uint64_t(w) + 1), kStream)) continue;
				Job J;
				J.C.key = keyOf(uint64_t(w) + 1);
				J.C.R = p.paintSize * 1.5;
				MakeFrame(p, J.C, P, N);
				J.cell = CellFor(p, J.C.R);
				                                                                  
				std::vector<V2> line;
				for (const V& q : pts) line.push_back(J.C.Local(q));
				                                                                              
				std::vector<V2> fixed;
				fixed.push_back(line[0]);
				for (size_t k = 1; k < line.size(); ++k) {
					const V2 d = line[k] - line[k - 1];
					const double l2 = m::la::length(d), l3 = acc[k] - acc[k - 1];
					fixed.push_back(fixed.back() + (l2 > 1e-12 ? d * (l3 / l2) : V2(0.0, 0.0)));
				}
				                                                         
				const V2 shift = J.C.Local(Pm) - [&]() {
					const double ll = acc[i] - acc[i - 1];
					return fixed[i - 1] + (fixed[i] - fixed[i - 1]) * (ll > 0.0 ? (sm - acc[i - 1]) / ll : 0.0);
				}();
				for (V2& q : fixed) q = q + shift;
				ShapeTrail(p, J.C, fixed, s0, s1, J.cell);
				jobs.push_back(std::move(J));
			}
		}
	}
	if (jobs.empty()) return;

	                             
	Ctx cx{&p, &s};
	std::vector<std::vector<m::Manifold>> outs(jobs.size());
	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t i) {
		Job& J = jobs[size_t(i)];
		if (s.avoid) {
			const double Hs = 0.5 * J.C.R + 2.0 * std::max(0.0, p.thickness) + 1e-3;
			const double reach = 1.5 * J.C.R + 4.0 * std::max(0.0, p.thickness) + 1e-3;
			std::vector<Disc> keep;
			for (const Disc& D : J.C.discs) if (!AvoidedAt(s, J.C, D.c, Hs, reach)) keep.push_back(D);
			J.C.discs.swap(keep);
		}
		BuildCluster(cx, J.C, J.cell, outs[size_t(i)]);
	});
	for (auto& o : outs) for (auto& c : o) cutters.push_back(std::move(c));
}

void RustyCutAxes(const std::vector<m::Manifold>& cutters, const std::vector<Int32>& ids,
	const std::unordered_map<Int32, Int32>& owners, Int32 effect, std::unordered_map<Int32, m::vec3>& axes)
{
	for (size_t c = 0; c < cutters.size() && c < ids.size(); ++c) {
		auto it = owners.find(ids[c]);
		if (it == owners.end() || it->second != effect) continue;
		                                                                       
		                                                                    
		                                        
		const m::MeshGL g = cutters[c].GetMeshGL();
		const size_t nt = g.triVerts.size() / 3;
		std::vector<V> dirs, tn;
		std::vector<double> dirA;
		std::vector<std::vector<uint32_t>> verts;
		for (size_t t = 0; t < nt; ++t) {
			V p[3];
			for (int k = 0; k < 3; ++k) {
				const size_t o = size_t(g.triVerts[3 * t + k]) * g.numProp;
				p[k] = V(g.vertProperties[o], g.vertProperties[o + 1], g.vertProperties[o + 2]);
			}
			V n = m::la::cross(p[1] - p[0], p[2] - p[0]);
			const double l = m::la::length(n);
			if (!(l > 0.0)) continue;
			n = n / l;
			tn.push_back(n);
			size_t k = 0;
			while (k < dirs.size() && std::abs(m::la::dot(dirs[k], n)) < 0.9995) ++k;
			if (k == dirs.size()) { dirs.push_back(n); dirA.push_back(0.0); verts.emplace_back(); }
			dirA[k] += 0.5 * l;
			for (int j = 0; j < 3; ++j) verts[k].push_back(g.triVerts[3 * t + j]);
		}
		size_t best = 0, bestN = 0;
		double bestA = 0.0;
		for (size_t k = 0; k < dirs.size(); ++k) {
			bool square = true;
			for (const V& n : tn) {
				const double d = std::abs(m::la::dot(dirs[k], n));
				if (d < 0.9995 && d > 0.03) { square = false; break; }
			}
			if (!square) continue;
			std::sort(verts[k].begin(), verts[k].end());
			const size_t n = size_t(std::unique(verts[k].begin(), verts[k].end()) - verts[k].begin());
			if (n > bestN || (n == bestN && dirA[k] > bestA)) { bestN = n; bestA = dirA[k]; best = k; }
		}
		if (bestN > 0) axes[ids[c]] = dirs[best];
	}
}

void RustyOpenFloors(m::MeshGL& mesh, const std::unordered_map<Int32, m::vec3>& axes)
{
	const size_t nRun = mesh.runOriginalID.size();
	if (axes.empty() || nRun == 0 || mesh.runIndex.size() < nRun + 1 || mesh.numProp < 3) return;
	const size_t nTri = mesh.triVerts.size() / 3;
	std::vector<uint8_t> drop(nTri, 0);
	size_t dropped = 0;
	auto pos = [&](uint32_t v) {
		const size_t o = size_t(v) * mesh.numProp;
		return V(mesh.vertProperties[o], mesh.vertProperties[o + 1], mesh.vertProperties[o + 2]);
	};
	for (size_t r = 0; r < nRun; ++r) {
		auto it = axes.find(Int32(mesh.runOriginalID[r]));
		if (it == axes.end()) continue;
		                                                                 
		V ax = it->second;
		if (mesh.runTransform.size() >= 12 * (r + 1)) {
			const float* M = &mesh.runTransform[12 * r];                      
			ax = V(M[0] * ax.x + M[3] * ax.y + M[6] * ax.z, M[1] * ax.x + M[4] * ax.y + M[7] * ax.z,
				M[2] * ax.x + M[5] * ax.y + M[8] * ax.z);
			const double l = m::la::length(ax);
			if (!(l > 0.0)) continue;
			ax = ax / l;
		}
		                                                                       
		                                                                       
		                                       
		const size_t t0 = mesh.runIndex[r] / 3, t1 = std::min(nTri, size_t(mesh.runIndex[r + 1] / 3));
		for (size_t t = t0; t < t1; ++t) {
			const V a = pos(mesh.triVerts[3 * t]), b = pos(mesh.triVerts[3 * t + 1]), c = pos(mesh.triVerts[3 * t + 2]);
			const V x = m::la::cross(b - a, c - a);
			const double l = m::la::length(x);
			if (l > 0.0 && std::abs(m::la::dot(x / l, ax)) > 0.99) { drop[t] = 1; ++dropped; }
		}
	}
	if (dropped == 0) return;
	std::vector<uint32_t> tv;
	tv.reserve(mesh.triVerts.size() - 3 * dropped);
	const bool hasFace = mesh.faceID.size() == nTri;
	const bool hasTan = mesh.halfedgeTangent.size() == 12 * nTri;
	std::vector<uint32_t> fid;
	std::vector<float> tan;
	std::vector<uint32_t> runIdx(nRun + 1, 0);
	for (size_t r = 0; r < nRun; ++r) {
		runIdx[r] = uint32_t(tv.size());
		const size_t t0 = mesh.runIndex[r] / 3, t1 = std::min(nTri, size_t(mesh.runIndex[r + 1] / 3));
		for (size_t t = t0; t < t1; ++t) {
			if (drop[t]) continue;
			tv.insert(tv.end(), {mesh.triVerts[3 * t], mesh.triVerts[3 * t + 1], mesh.triVerts[3 * t + 2]});
			if (hasFace) fid.push_back(mesh.faceID[t]);
			if (hasTan) tan.insert(tan.end(), mesh.halfedgeTangent.begin() + 12 * t, mesh.halfedgeTangent.begin() + 12 * t + 12);
		}
	}
	runIdx[nRun] = uint32_t(tv.size());
	mesh.triVerts.swap(tv);
	mesh.runIndex.swap(runIdx);
	if (hasFace) mesh.faceID.swap(fid);
	if (hasTan) mesh.halfedgeTangent.swap(tan);
}
PROJECT_DAMAGE_NS_MACRO_END
