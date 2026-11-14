// предыдущий вариант разрывов объёмной ткани
// ProjectDamage fabric tears for solidified cloth — GPL-3.0-or-later; see LICENSE.
//
// Recipe fabric-solid-v1 (FabricTearParams::solid). For cloth with real
// thickness - a garment with Solidify: a CLOSED shell, two walls a few mm
// apart per panel, the other panel some cm away. The tears are Boolean cuts
// (so the torn side wall of the hole comes out closed and clean), with cutters
// that FOLLOW THE CLOTH and are exactly as deep as the panel:
//   1. tear sites on the outside of a panel: a point is used when the ray
//      inward leaves the shell within a plausible wall thickness and the ray
//      outward finds open space (not the panel next to it);
//   2. a ragged tear outline (the Classic shapes: lens, lobes, jagged rim,
//      meander, split into holes with bridges) in the tangent plane,
//      triangulated and subdivided;
//   3. every outline vertex is dropped onto the panel along the tear normal
//      (the outside hit nearest to the tear plane); there the local wall
//      thickness t is measured (to the exit hit) and the gap g to the next
//      surface (the entry of the next wall); the cutter runs from 0.3 t above
//      the cloth to t + min(0.5 t, 0.45 g) below it - through this panel,
//      never into the other one, on curved cloth too;
//   4. top, bottom and side walls make one closed cutter per tear;
//   5. threads: closed tubes anchored on the outer rim, bridging the hole
//      and sagging into it, broken ends and fray stubs - unioned to the cloth.
//   Edge Bevel (solidBevel > 0, 2026-10-26): the cutter's side wall flares
//   out in quarter circles at the outer and the inner face of the panel, so
//   the torn rim keeps a rounded lip instead of two sharp edges. Radius =
//   solidBevel x 0.48 x the local wall thickness; solidBevelSegments rings
//   per quarter circle.
//   Tear Shape Realistic (tearStyle 1, 2026-10-28): the outlines come from
//   fabric_tear_shapes.h (rips along the grain, corner tears, worn holes,
//   punctures; stair-stepped yarn-end rims), one cutter per connected hole;
//   the threads are its yarns; the flap of a corner tear is rebuilt as a slab
//   of the cloth (80 % of its thickness, rooted inside the panel past the
//   hinge) and folded back out of the hole.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "manifold/polygon.h"
#include "fabric_solid.h"
#include "fabric_tear_shapes_legacy.h"
#include "project_damage_bvh.h"
#include "project_damage_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace m = manifold;
using namespace project_damage;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
using V2 = m::vec2;

double R01(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Noise1(double x, uint64_t seed, uint64_t ch)
{
	const double f = std::floor(x), t = x - f;
	const double a = R01(seed, uint64_t(int64_t(f) + (1 << 20)), ch) * 2 - 1;
	const double b = R01(seed, uint64_t(int64_t(f) + 1 + (1 << 20)), ch) * 2 - 1;
	const double s = t * t * (3 - 2 * t);
	return a + (b - a) * s;
}
V Unit(const V& v, const V& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; }

struct Hit { double s; V nrm; };

                                                              
std::vector<Hit> HitsAlong(const ProjectDamageBVH& bvh, const MeshView& mv, const std::vector<V>& fn,
	const V& q, const V& d, double s0, double s1, double eps)
{
	std::vector<Hit> out;
	V o = q + d * s0;
	double done = s0;
	for (int guard = 0; guard < 64 && done < s1; ++guard)
	{
		double dist = 0.0;
		int32_t poly = -1;
		if (!bvh.Raycast(o, d, s1 - done, &dist, &poly)) break;
		done += dist;
		out.push_back({ done, poly >= 0 && (size_t)poly < fn.size() ? fn[(size_t)poly] : V(0.0) });
		o = q + d * (done + eps);
		done += eps;
	}
	(void)mv;
	return out;
}

m::Manifold Tube(const std::vector<V>& path, double r0, double r1, int sides)
{
	const size_t ns = path.size();
	if (ns < 2) return m::Manifold();
	m::MeshGL64 mg;
	mg.numProp = 3;
	V prevN = m::la::cross(Unit(path[1] - path[0], V(1, 0, 0)), V(0, 0, 1));
	if (m::la::length(prevN) < 1e-6) prevN = V(1, 0, 0);
	for (size_t i = 0; i < ns; ++i)
	{
		const V t = Unit(path[std::min(ns - 1, i + 1)] - path[i > 0 ? i - 1 : 0], V(1, 0, 0));
		const V nx = Unit(prevN - t * m::la::dot(prevN, t), m::la::cross(t, V(0, 0, 1)));
		const V ny = m::la::cross(t, nx);
		prevN = nx;
		const double r = Lerp(r0, r1, double(i) / (ns - 1));
		for (int s = 0; s < sides; ++s)
		{
			const double a = 2.0 * m::kPi * s / sides;
			const V p = path[i] + (nx * std::cos(a) + ny * std::sin(a)) * r;
			mg.vertProperties.insert(mg.vertProperties.end(), { p.x, p.y, p.z });
		}
	}
	const uint64_t ca = ns * (size_t)sides, cb = ca + 1;
	mg.vertProperties.insert(mg.vertProperties.end(), { path.front().x, path.front().y, path.front().z, path.back().x, path.back().y, path.back().z });
	for (size_t i = 0; i + 1 < ns; ++i)
		for (int s = 0; s < sides; ++s)
		{
			const uint64_t a = i * sides + s, b = i * sides + (s + 1) % sides, c = (i + 1) * sides + (s + 1) % sides, d = (i + 1) * sides + s;
			mg.triVerts.insert(mg.triVerts.end(), { a, b, c, a, c, d });
		}
	for (int s = 0; s < sides; ++s)
	{
		mg.triVerts.insert(mg.triVerts.end(), { ca, (uint64_t)((s + 1) % sides), (uint64_t)s });
		mg.triVerts.insert(mg.triVerts.end(), { cb, (uint64_t)((ns - 1) * sides + s), (uint64_t)((ns - 1) * sides + (s + 1) % sides) });
	}
	m::Manifold t(mg);
	if (t.Status() != m::Manifold::Error::NoError) return m::Manifold();
	if (t.Volume() < 0) return m::Manifold();
	return t;
}

}               

void PlaceFabricSolidTearsLegacy(const CoreInput& in, const FabricTearParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& threads, const ProjectDamageHost& host)
{
	const MeshView& mv = in.view;
	if (mv.npolys <= 0 || !p.holesEnable || p.holeAmount <= 0) return;
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x534F4C4944464142ULL) ^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	                          
	struct STri { Int32 v[3]; Int32 poly; double area; V n; };
	std::vector<STri> tris;
	std::vector<V> fn((size_t)mv.npolys, V(0.0));
	double total = 0.0;
	V lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < mv.npts; ++i) { lo = m::la::min(lo, V(mv.pts[i])); hi = m::la::max(hi, V(mv.pts[i])); }
	const double diag = m::la::length(hi - lo);
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		const int n = q.c == q.d ? 3 : 4;
		V nsum(0.0);
		for (int k = 1; k + 1 < n; ++k)
		{
			STri t{ { ids[0], ids[k], ids[k + 1] }, i, 0.0, V(0.0) };
			const V cr = m::la::cross(V(mv.pts[t.v[1]]) - V(mv.pts[t.v[0]]), V(mv.pts[t.v[2]]) - V(mv.pts[t.v[0]]));
			t.area = 0.5 * m::la::length(cr);
			t.n = Unit(cr, V(0, 0, 1));
			nsum += cr;
			total += t.area;
			tris.push_back(t);
		}
		fn[(size_t)i] = GuidedNormal(mv, i, Unit(nsum, V(0, 0, 1)));
	}
	if (tris.empty() || !(total > 0.0)) return;
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	const double eps = 1e-6 * diag + 1e-9;
	auto avoided = [&](Int32 poly) {
		return p.avoid && in.avoidPoly != nullptr && (size_t)poly < in.avoidPoly->size() && (*in.avoidPoly)[(size_t)poly];
	};
	std::vector<double> cdf;
	double acc = 0.0;
	for (const STri& t : tris) { acc += avoided(t.poly) ? 0.0 : t.area; cdf.push_back(acc); }
	                                                                        
	                                                           
	auto shellAt = [&](const V& q, const V& n, double& t, double& gap) {
		const auto hits = HitsAlong(bvh, mv, fn, q, n * -1.0, eps, 0.25 * diag, eps);
		t = -1.0; gap = 1e300;
		for (size_t i = 0; i < hits.size(); ++i)
			if (m::la::dot(hits[i].nrm, n) < -0.1)                     
			{
				t = hits[i].s;
				if (i + 1 < hits.size()) gap = hits[i + 1].s - t;
				break;
			}
		return t > 0.0;
	};
	const V gravW = Unit(V(p.gravityX, p.gravityY, p.gravityZ), V(0, 0, -1));
	const double tr = std::max(1e-5, p.threadRadius);
	const int sides = std::clamp(p.threadSides, 3, 12);
	int made = 0;
	for (int h = 0; h < p.holeAmount; ++h)
	{
		                                      
		V pos(0.0), n(0.0);
		double t0 = 0.0, g0 = 0.0;
		bool ok = false;
		for (int attempt = 0; attempt < 40 && !ok; ++attempt)
		{
			const uint64_t k = 1000 + uint64_t(h) * 97 + uint64_t(attempt) * 100003;
			const double r = R01(seed, k, 1) * acc;
			const size_t ti = (size_t)(std::upper_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
			if (ti >= tris.size()) continue;
			double u = R01(seed, k, 2), v = R01(seed, k, 3);
			if (u + v > 1) { u = 1 - u; v = 1 - v; }
			const STri& t = tris[ti];
			pos = V(mv.pts[t.v[0]]) * (1 - u - v) + V(mv.pts[t.v[1]]) * u + V(mv.pts[t.v[2]]) * v;
			n = GuidedNormal(mv,t.poly,t.n);
            if(R01(seed,k,81)>fabric_noise::Weight(p,pos,seed))continue;
			if (!shellAt(pos, n, t0, g0) || t0 > 0.08 * diag) continue;
			                                                                
			const auto front = HitsAlong(bvh, mv, fn, pos, n, eps * 10, 3.0 * t0 + 0.002 * diag, eps);
			if (!front.empty()) continue;
			ok = true;
		}
		if (!ok) continue;
		const uint64_t hk = 77000 + uint64_t(h) * 131;
		                                  
		V dir = p.holeDirMode == 1 ? V(p.holeDirX, p.holeDirY, p.holeDirZ)
			: V(R01(seed, hk, 4) - 0.5, R01(seed, hk, 5) - 0.5, R01(seed, hk, 6) - 0.5);
		dir = dir - n * m::la::dot(dir, n);
		if (m::la::length(dir) < 1e-6) dir = m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0));
		dir = Unit(dir, V(1, 0, 0));
		const double jit = (R01(seed, hk, 7) * 2 - 1) * p.holeDirJitter * m::kPi / 180.0;
		const V b0 = m::la::cross(n, dir);
		dir = Unit(dir * std::cos(jit) + b0 * std::sin(jit), dir);
		const V T = dir, B = m::la::cross(n, dir);
		const double L = Lerp(p.holeLengthMin, p.holeLengthMax, R01(seed, hk, 8));
		const double W = Lerp(p.holeWidthMin, p.holeWidthMax, R01(seed, hk, 9));
		const int nSplit = 1 + int(R01(seed, hk, 10) * std::max(1, p.holeSplitMax));
		std::vector<std::pair<double, double>> spans;
		{
			std::vector<double> cuts = { -0.5 * L, 0.5 * L };
			for (int s = 1; s < nSplit; ++s) cuts.insert(cuts.end() - 1, -0.5 * L + L * (s + (R01(seed, hk, 20 + s) - 0.5) * 0.4) / nSplit);
			for (size_t s = 0; s + 1 < cuts.size(); ++s)
			{
				const double a0 = cuts[s] + (s > 0 ? 0.5 * p.holeBridge : 0.0);
				const double a1 = cuts[s + 1] - (s + 2 < cuts.size() ? 0.5 * p.holeBridge : 0.0);
				if (a1 - a0 > 0.15 * W) spans.push_back({ a0, a1 });
			}
		}
		const int nU = std::max(24, int(L / std::max(0.05, 0.12 * W / std::max(0.05, p.detail))));
		auto centreV = [&](double u) { return p.holeMeander * W * 1.2 * Noise1(u / std::max(1e-6, 0.4 * L) * 2.0, seed ^ hk, 31); };
		auto halfW = [&](double u, double u0, double u1, int side) {
			const double s = (u - u0) / std::max(1e-9, u1 - u0) * 2.0 - 1.0;
			const double lens = std::pow(std::max(0.0, 1.0 - s * s), 0.65);
			const double lobes = 1.0 + p.holeLobes * 0.6 * Noise1(u / std::max(1e-6, 0.25 * L), seed ^ hk, 41 + side);
			const double jag = 1.0 + p.holeJagged * 0.45 * (Noise1(u / std::max(1e-6, 0.035 * L), seed ^ hk, 51 + side)
				+ 0.5 * Noise1(u / std::max(1e-6, 0.012 * L), seed ^ hk, 61 + side));
			return 0.5 * W * lens * std::max(0.05, lobes) * std::max(0.1, jag);
		};
		                                                                 
		const bool real = p.tearStyle >= 1;
		fabric_shapes_legacy::Shape sh;
		std::vector<m::Polygons> comps;
		if (real)
		{
			sh = fabric_shapes_legacy::MakeTearShape(p, fabric_shapes_legacy::PickKind(p, R01(seed, hk, 11)), L, W, seed, hk);
			for (const m::CrossSection& part : m::CrossSection(sh.holes, m::CrossSection::FillRule::Positive).Decompose())
			{
				m::Polygons rings = fabric_shapes_legacy::OuterRings(part, 0.0);
				if (!rings.empty()) comps.push_back(std::move(rings));
			}
			if (comps.empty()) continue;
		}
		const double reach = real ? std::max(0.6 * L + W, sh.reach) : 0.6 * L + W;
		                                                                           
		auto drop = [&](const V2& s2, V& top, V& bot, V& surf, double* thick = nullptr) {
			const V q = pos + T * s2.x + B * s2.y;
			const double H = 0.5 * reach;
			const auto hits = HitsAlong(bvh, mv, fn, q + n * H, n * -1.0, 0.0, 2.0 * H, eps);
			double best = 1e300; double sBest = 0.0; bool found = false;
			for (const Hit& hh : hits)
				if (m::la::dot(hh.nrm, n) > 0.1 && std::abs(hh.s - H) < best) { best = std::abs(hh.s - H); sBest = hh.s; found = true; }
			if (!found) return false;
			surf = q + n * (H - sBest);
			double t = 0.0, gap = 1e300;
			if (!shellAt(surf, n, t, gap)) { t = t0; gap = g0; }
			const double margin = std::min(0.5 * t, gap < 1e299 ? 0.45 * gap : 0.5 * t);
			top = surf + n * (0.3 * t + 1e-4 * diag);
			bot = surf - n * (t + margin);
			if (thick) *thick = t;
			return true;
		};
		const size_t nComp = real ? comps.size() : spans.size();
		for (size_t si = 0; si < nComp; ++si)
		{
			std::vector<V2> top2, bot2;
			m::Polygons polys;
			if (real) polys = comps[si];
			else
			{
				const auto& sp = spans[si];
				for (int i = 0; i <= nU; ++i)
				{
					const double u = Lerp(sp.first, sp.second, double(i) / nU);
					const double c = centreV(u);
					top2.push_back(V2(u, c + halfW(u, sp.first, sp.second, 0)));
					bot2.push_back(V2(u, c - halfW(u, sp.first, sp.second, 1)));
				}
				m::SimplePolygon poly;
				for (const V2& q : bot2) poly.push_back(q);
				for (int i = (int)top2.size() - 1; i >= 0; --i) poly.push_back(top2[(size_t)i]);
				polys = { poly };
			}
			                                                        
			std::vector<m::ivec3> tri;
			try { tri = m::Triangulate(polys, 1e-9 * diag); } catch (...) { continue; }
			std::vector<V2> v2;
			for (const auto& r : polys) v2.insert(v2.end(), r.begin(), r.end());
			std::vector<std::array<int, 3>> t2;
			for (const auto& tt : tri) t2.push_back({ tt[0], tt[1], tt[2] });
			const double target = std::max(0.25 * W, 2.0 * t0);
			for (int level = 0; level < 5; ++level)
			{
				double longest = 0.0;
				for (const auto& tt : t2) for (int k = 0; k < 3; ++k) longest = std::max(longest, (double)m::la::length(v2[(size_t)tt[(k + 1) % 3]] - v2[(size_t)tt[k]]));
				if (longest <= target) break;
				std::map<std::pair<int, int>, int> mid;
				auto midOf = [&](int a, int b) {
					const auto key = std::make_pair(std::min(a, b), std::max(a, b));
					auto it = mid.find(key);
					if (it != mid.end()) return it->second;
					v2.push_back((v2[(size_t)a] + v2[(size_t)b]) * 0.5);
					return mid[key] = (int)v2.size() - 1;
				};
				std::vector<std::array<int, 3>> nx;
				for (const auto& tt : t2)
				{
					const int a = tt[0], b = tt[1], c = tt[2];
					const int ab = midOf(a, b), bc = midOf(b, c), ca = midOf(c, a);
					nx.push_back({ a, ab, ca }); nx.push_back({ ab, b, bc }); nx.push_back({ ca, bc, c }); nx.push_back({ ab, bc, ca });
				}
				t2.swap(nx);
			}
			                                                        
			                    
			const size_t nv = v2.size();
			std::vector<V> topP(nv), botP(nv), surfP(nv);
			std::vector<double> thickP(nv, t0);
			std::vector<char> okv(nv, 0);
			int good = 0;
			for (size_t i = 0; i < nv; ++i) { okv[i] = drop(v2[i], topP[i], botP[i], surfP[i], &thickP[i]); good += okv[i]; }
			if (good == 0) continue;
			for (size_t i = 0; i < nv; ++i)
				if (!okv[i])
				{
					                                                       
					size_t bj = 0; double bd = 1e300;
					for (size_t j = 0; j < nv; ++j) if (okv[j]) { const double d = m::la::length(v2[j] - v2[i]); if (d < bd) { bd = d; bj = j; } }
					const V shift = T * (v2[i].x - v2[bj].x) + B * (v2[i].y - v2[bj].y);
					topP[i] = topP[bj] + shift; botP[i] = botP[bj] + shift; surfP[i] = surfP[bj] + shift;
					thickP[i] = thickP[bj];
				}
			std::map<std::pair<int, int>, int> edgeUse;
			for (const auto& tt : t2)
				for (int k = 0; k < 3; ++k) edgeUse[{ tt[k], tt[(k + 1) % 3] }]++;
			std::vector<std::pair<int, int>> rim;                                      
			for (const auto& e : edgeUse)
				if (!edgeUse.count({ e.first.second, e.first.first })) rim.push_back(e.first);
			                                                                   
			const int segs = std::clamp(p.solidBevelSegments, 1, 12);
			std::vector<V2> outN(nv, V2(0.0));
			std::vector<int> near(nv, -1);                                                         
			std::vector<double> fade(nv, 0.0);
			std::vector<int> ring(nv, -1);                                          
			int nRim = 0;
			double bevel = std::clamp(p.solidBevel, 0.0, 1.0);
			if (bevel > 0.0)
			{
				for (const auto& e : rim)
				{
					const V2 d = v2[(size_t)e.second] - v2[(size_t)e.first];
					const double l = m::la::length(d);
					if (l < 1e-12) continue;
					const V2 o(d.y / l, -d.x / l);                                               
					outN[(size_t)e.first] += o; outN[(size_t)e.second] += o;
				}
				double area2 = 0.0;
				for (const auto& e : rim) area2 += v2[(size_t)e.first].x * v2[(size_t)e.second].y - v2[(size_t)e.second].x * v2[(size_t)e.first].y;
				const double flip = area2 < 0.0 ? -1.0 : 1.0;
				for (const auto& e : rim)
					for (int v : { e.first, e.second })
						if (ring[(size_t)v] < 0)
						{
							ring[(size_t)v] = nRim++;
							const double l = m::la::length(outN[(size_t)v]);
							outN[(size_t)v] = l > 1e-12 ? outN[(size_t)v] * (flip / l) : V2(0.0);
						}
				                                                               
				                                                             
				{
					std::vector<int> nxt(nv, -1), prv(nv, -1);
					double el = 0.0;
					for (const auto& e : rim) { nxt[(size_t)e.first] = e.second; prv[(size_t)e.second] = e.first; el += m::la::length(v2[(size_t)e.second] - v2[(size_t)e.first]); }
					el /= std::max<size_t>(1, rim.size());
					double rmax = 0.0;
					for (size_t v = 0; v < nv; ++v) if (ring[v] >= 0) rmax = std::max(rmax, bevel * 0.48 * thickP[v]);
					const double spread = 1.5 * rmax / std::max(1e-12, el);
					const int passes = std::clamp(int(spread * spread), 0, 200);
					std::vector<V2> tmp(outN);
					for (int pass = 0; pass < passes; ++pass)
					{
						for (size_t v = 0; v < nv; ++v)
						{
							if (ring[v] < 0) continue;
							V2 sum = outN[v] * 2.0;
							if (prv[v] >= 0) sum += outN[(size_t)prv[v]];
							if (nxt[v] >= 0) sum += outN[(size_t)nxt[v]];
							const double l = m::la::length(sum);
							tmp[v] = l > 1e-12 ? sum / l : outN[v];
						}
						outN.swap(tmp);
					}
				}
				                                                           
				                                                     
				for (size_t v = 0; v < nv; ++v)
				{
					if (ring[v] >= 0) { near[v] = (int)v; fade[v] = 1.0; continue; }
					double bd = 1e300; int bi = -1;
					for (const auto& e : rim)
					{
						const double d = m::la::length2(v2[(size_t)e.first] - v2[v]);
						if (d < bd) { bd = d; bi = e.first; }
					}
					if (bi < 0) continue;
					near[v] = bi;
					fade[v] = std::max(0.0, 1.0 - std::sqrt(bd) / std::max(1e-12, 3.0 * 0.48 * thickP[(size_t)bi]));
				}
				                                                                     
				for (int tries = 0; tries < 5 && bevel > 0.0; ++tries)
				{
					bool folded = false;
					for (const auto& tt : t2)
					{
						V2 a[3], b[3];
						for (int k = 0; k < 3; ++k)
						{
							const size_t v = (size_t)tt[k];
							a[k] = v2[v];
							b[k] = v2[v] + (near[v] >= 0 ? outN[(size_t)near[v]] * (fade[v] * bevel * 0.48 * thickP[(size_t)near[v]]) : V2(0.0));
						}
						const double sa = (a[1].x - a[0].x) * (a[2].y - a[0].y) - (a[1].y - a[0].y) * (a[2].x - a[0].x);
						const double sb = (b[1].x - b[0].x) * (b[2].y - b[0].y) - (b[1].y - b[0].y) * (b[2].x - b[0].x);
						if (std::abs(sa) < 1e-12 * W * W) continue;                                
						if (sa * sb <= 0.0) { folded = true; break; }
					}
					if (!folded) break;
					bevel = tries < 4 ? bevel * 0.5 : 0.0;
				}
			}
			const bool round = bevel > 0.0 && nRim > 0;
			if (!round) near.assign(nv, -1);
			const int nRings = round ? 2 * (segs + 1) : 0;
			m::MeshGL64 mg;
			mg.numProp = 3;
			auto offsetOf = [&](size_t v, double off) { return T * (outN[v].x * off) + B * (outN[v].y * off); };
			auto capShift = [&](size_t i) {
				const int r = near[i];
				return r < 0 ? V(0.0) : offsetOf((size_t)r, fade[i] * bevel * 0.48 * thickP[(size_t)r]);
			};
			for (size_t i = 0; i < nv; ++i)
			{
				const V q = topP[i] + capShift(i);
				mg.vertProperties.insert(mg.vertProperties.end(), { q.x, q.y, q.z });
			}
			for (size_t i = 0; i < nv; ++i)
			{
				const V q = botP[i] + capShift(i);
				mg.vertProperties.insert(mg.vertProperties.end(), { q.x, q.y, q.z });
			}
			                                                                 
			                                                                   
			std::vector<size_t> rimVert((size_t)nRim);
			for (size_t i = 0; i < nv; ++i) if (ring[i] >= 0) rimVert[(size_t)ring[i]] = i;
			for (int r = 0; r < nRings; ++r)
			{
				const bool lower = r > segs;
				const int k = lower ? r - (segs + 1) : r;
				const double phi = 0.5 * m::kPi * (lower ? double(segs - k) : double(k)) / segs;
				for (int j = 0; j < nRim; ++j)
				{
					const size_t i = rimVert[(size_t)j];
					const double rad = bevel * 0.48 * thickP[i];
					const double dep = rad - rad * std::cos(phi), off = rad - rad * std::sin(phi);
					const V q = surfP[i] + n * -(lower ? thickP[i] - dep : dep) + offsetOf(i, off);
					mg.vertProperties.insert(mg.vertProperties.end(), { q.x, q.y, q.z });
				}
			}
			auto level = [&](int L, int v) -> uint64_t {                                                
				if (L == 0) return (uint64_t)v;
				if (L == nRings + 1) return (uint64_t)(nv + v);
				return (uint64_t)(2 * nv + (size_t)(L - 1) * nRim + (size_t)ring[(size_t)v]);
			};
			for (const auto& tt : t2)
			{
				mg.triVerts.insert(mg.triVerts.end(), { (uint64_t)tt[0], (uint64_t)tt[1], (uint64_t)tt[2] });                          
				mg.triVerts.insert(mg.triVerts.end(), { (uint64_t)(nv + tt[0]), (uint64_t)(nv + tt[2]), (uint64_t)(nv + tt[1]) });            
			}
			for (const auto& e : rim)
			{
				const int a = e.first, b = e.second;
				                                                                            
				for (int L = 0; L <= nRings; ++L)
					mg.triVerts.insert(mg.triVerts.end(), { level(L, b), level(L, a), level(L + 1, a), level(L, b), level(L + 1, a), level(L + 1, b) });
			}
			double vol = 0.0;
			auto P = [&](uint64_t i) { return V(mg.vertProperties[3 * i], mg.vertProperties[3 * i + 1], mg.vertProperties[3 * i + 2]); };
			for (size_t q = 0; q < mg.triVerts.size(); q += 3) vol += m::la::dot(P(mg.triVerts[q]), m::la::cross(P(mg.triVerts[q + 1]), P(mg.triVerts[q + 2])));
			if (vol < 0) for (size_t q = 0; q < mg.triVerts.size(); q += 3) std::swap(mg.triVerts[q + 1], mg.triVerts[q + 2]);
			m::Manifold cut(mg);
			if (cut.Status() != m::Manifold::Error::NoError || cut.IsEmpty()) continue;
			cutters.push_back(std::move(cut));
			++made;
			                                                              
			if (!p.threadsEnable || real) continue;
			const V inward = n * -1.0;
			const V gT = Unit(gravW - n * m::la::dot(gravW, n), T);
			double since = 0.0;
			for (int i = 1; i < nU && (int)threads.size() < p.maxThreads; ++i)
			{
				since += m::la::length(bot2[(size_t)i] - bot2[(size_t)i - 1]);
				if (since < p.threadSpacing) continue;
				since = 0.0;
				V ta, ba, sa, tb2, bb, sb;
				if (!drop(bot2[(size_t)i], ta, ba, sa) || !drop(top2[(size_t)i], tb2, bb, sb)) continue;
				                                                                            
				const V a = sa + inward * (0.5 * t0), b = sb + inward * (0.5 * t0);
				const double span = m::la::length(b - a);
				if (span < 4.0 * tr) continue;
				const V across = (b - a) / span;
				const uint64_t tk = 900000 + uint64_t(h) * 7919 + si * 1013 + (uint64_t)i;
				const double pick = R01(seed, tk, 1);
				if (p.bridgeEnable && pick < p.bridgeShare)
				{
					std::vector<V> path;
					const int ns = 10;
					for (int k = 0; k <= ns; ++k)
					{
						const double f = double(k) / ns, bell = 4.0 * f * (1 - f);
						path.push_back(a + (b - a) * f + inward * (p.threadSag * span * bell) + T * ((R01(seed, tk, 2) - 0.5) * p.threadSkew * span * bell));
					}
					m::Manifold tube = Tube(path, tr, tr, sides);
					if (!tube.IsEmpty()) threads.push_back(std::move(tube));
				}
				else if (p.brokenEnable && pick < p.bridgeShare + p.brokenShare)
					for (int side = 0; side < 2; ++side)
					{
						const V root = side == 0 ? a : b;
						V d = side == 0 ? across : across * -1.0;
						const double len = span * Lerp(0.2, 0.45, R01(seed, tk, 10 + side));
						std::vector<V> path;
						V cur = root - d * tr;
						for (int k = 0; k <= 8; ++k)
						{
							path.push_back(cur);
							d = Unit(d + T * ((R01(seed, tk, 20 + side * 10 + k) - 0.5) * p.threadCurl * 0.5) + (inward * 0.6 + gT * 0.4) * (p.threadDroop * 0.15), d);
							cur = cur + d * (len / 8);
						}
						m::Manifold tube = Tube(path, tr, tr * p.threadTaper, sides);
						if (!tube.IsEmpty()) threads.push_back(std::move(tube));
					}
				else if (p.frayEnable)
					for (int side = 0; side < 2; ++side)
					{
						const V root = side == 0 ? a : b;
						const V d = side == 0 ? across : across * -1.0;
						const double len = std::min(0.45 * span, Lerp(p.frayLengthMin, p.frayLengthMax, R01(seed, tk, 40 + side)));
						m::Manifold tube = Tube({ root - d * tr, root + d * (0.55 * len) + inward * (0.1 * len), root + d * len + inward * (0.3 * len) }, tr * 0.8, tr * 0.35, sides);
						if (!tube.IsEmpty()) threads.push_back(std::move(tube));
					}
			}
		}
		if (!real) continue;
		                                                       
		if (p.threadsEnable)
		{
			const V gT = Unit(gravW - n * m::la::dot(gravW, n), T);
			auto liftYarn = [&](const V2& s2, V& pos, int& tag) {
				V tq, bq, sq; double th = t0;
				if (!drop(s2, tq, bq, sq, &th)) return false;
				pos = sq - n * (0.5 * th);
				tag = 0;
				return true;
			};
			std::vector<fabric_shapes_legacy::YarnPath> paths;
			const size_t room = p.maxThreads > (int)threads.size() ? size_t(p.maxThreads) - threads.size() : 0;
			fabric_shapes_legacy::YarnPaths(sh, p, seed ^ (0x5941524EULL + uint64_t(h)), liftYarn, T, n * -1.0, gT, room, paths);
			for (const auto& yp : paths)
			{
				m::Manifold tube = Tube(yp.path, yp.r0, yp.r1, sides);
				if (!tube.IsEmpty()) threads.push_back(std::move(tube));
			}
		}
		                                                                       
		for (const fabric_shapes_legacy::Flap& fl : sh.flaps)
		{
			const V2 hd = fl.h1 - fl.h0;
			const double hl = m::la::length(hd);
			if (hl < 1e-9) continue;
			const V2 ha = hd / hl;
			V2 e2(-ha.y, ha.x);
			if (m::la::dot(fl.tip - fl.h0, e2) < 0) e2 = e2 * -1.0;
			                                                                
			const double ms = std::max(3.0 * t0, 2.0 * sh.pitch);
			m::SimplePolygon strip = { fl.h0 - e2 * ms, fl.h1 - e2 * ms, fl.h1 + e2 * (0.5 * sh.pitch), fl.h0 + e2 * (0.5 * sh.pitch) };
			if (fabric_shapes_legacy::RingArea(strip) < 0) std::reverse(strip.begin(), strip.end());
			const m::Polygons fp = fabric_shapes_legacy::OuterRings(m::CrossSection(fl.poly, m::CrossSection::FillRule::Positive) + m::CrossSection(strip), 0.0);
			if (fp.empty()) continue;
			std::vector<m::ivec3> ftri;
			try { ftri = m::Triangulate(fp, 1e-9 * diag); } catch (...) { continue; }
			std::vector<V2> fv;
			for (const auto& r : fp) fv.insert(fv.end(), r.begin(), r.end());
			std::vector<std::array<int, 3>> ft;
			for (const auto& tt : ftri) ft.push_back({ tt[0], tt[1], tt[2] });
			const double rollR = std::max(0.6 * t0, 0.08 * hl);                               
			const double target = std::max(0.35 * rollR, 0.5 * t0);                                          
			for (int level = 0; level < 6; ++level)
			{
				double longest = 0.0;
				for (const auto& tt : ft) for (int k = 0; k < 3; ++k) longest = std::max(longest, (double)m::la::length(fv[(size_t)tt[(k + 1) % 3]] - fv[(size_t)tt[k]]));
				if (longest <= target || ft.size() * 4 > 40000) break;
				std::map<std::pair<int, int>, int> mid;
				auto midOf = [&](int a, int b) {
					const auto key = std::make_pair(std::min(a, b), std::max(a, b));
					auto it = mid.find(key);
					if (it != mid.end()) return it->second;
					fv.push_back((fv[(size_t)a] + fv[(size_t)b]) * 0.5);
					return mid[key] = (int)fv.size() - 1;
				};
				std::vector<std::array<int, 3>> nx;
				for (const auto& tt : ft)
				{
					const int a = tt[0], b = tt[1], c = tt[2];
					const int ab = midOf(a, b), bc = midOf(b, c), ca = midOf(c, a);
					nx.push_back({ a, ab, ca }); nx.push_back({ ab, b, bc }); nx.push_back({ ca, bc, c }); nx.push_back({ ab, bc, ca });
				}
				ft.swap(nx);
			}
			V tq, bq, HA, HB, HC; double th = t0;
			if (!drop(fl.h0, tq, bq, HA) || !drop(fl.h1, tq, bq, HB) || !drop(fl.tip, tq, bq, HC)) continue;
			fabric_shapes_legacy::Fold fd;
			fd.H = HA;
			fd.a = Unit(HB - HA, T);
			fd.e = Unit((HC - HA) - fd.a * m::la::dot(HC - HA, fd.a), B);
			fd.n = n;
			fd.R = rollR;
			fd.theta = fl.angle;
			const size_t nf = fv.size();
			std::vector<V> topF(nf), botF(nf);
			bool okF = true;
			for (size_t i = 0; i < nf && okF; ++i)
			{
				V sq;
				th = t0;
				if (!drop(fv[i], tq, bq, sq, &th)) { sq = HA + T * (fv[i].x - fl.h0.x) + B * (fv[i].y - fl.h0.y); th = t0; }
				topF[i] = fabric_shapes_legacy::FoldPoint(fd, sq - n * (0.1 * th));
				botF[i] = fabric_shapes_legacy::FoldPoint(fd, sq - n * (0.9 * th));
			}
			m::MeshGL64 fm;
			fm.numProp = 3;
			for (const V& q : topF) fm.vertProperties.insert(fm.vertProperties.end(), { q.x, q.y, q.z });
			for (const V& q : botF) fm.vertProperties.insert(fm.vertProperties.end(), { q.x, q.y, q.z });
			std::map<std::pair<int, int>, int> eu;
			for (const auto& tt : ft)
			{
				fm.triVerts.insert(fm.triVerts.end(), { (uint64_t)tt[0], (uint64_t)tt[1], (uint64_t)tt[2] });
				fm.triVerts.insert(fm.triVerts.end(), { (uint64_t)(nf + tt[0]), (uint64_t)(nf + tt[2]), (uint64_t)(nf + tt[1]) });
				for (int k = 0; k < 3; ++k) eu[{ tt[k], tt[(k + 1) % 3] }]++;
			}
			for (const auto& e : eu)
			{
				if (eu.count({ e.first.second, e.first.first })) continue;
				const uint64_t a = (uint64_t)e.first.first, b = (uint64_t)e.first.second;
				fm.triVerts.insert(fm.triVerts.end(), { b, a, nf + a, b, nf + a, nf + b });
			}
			double vol = 0.0;
			auto P = [&](uint64_t i) { return V(fm.vertProperties[3 * i], fm.vertProperties[3 * i + 1], fm.vertProperties[3 * i + 2]); };
			for (size_t q = 0; q < fm.triVerts.size(); q += 3) vol += m::la::dot(P(fm.triVerts[q]), m::la::cross(P(fm.triVerts[q + 1]), P(fm.triVerts[q + 2])));
			if (vol < 0) for (size_t q = 0; q < fm.triVerts.size(); q += 3) std::swap(fm.triVerts[q + 1], fm.triVerts[q + 2]);
			m::Manifold flap(fm);
			if (flap.Status() != m::Manifold::Error::NoError || flap.IsEmpty()) continue;
			threads.push_back(std::move(flap));
		}
	}
	HostLog(host, "[ProjectDamage] fabric (solidify): " + std::to_string(made) + " tear cutters, " + std::to_string(threads.size()) + " threads");
}

PROJECT_DAMAGE_NS_MACRO_END
