// создаём повреждения дерева вдоль волокон
// ProjectDamage wood damage — GPL-3.0-or-later; see LICENSE.
//
// Recipe: wood-damage-v1. A procedural appearance model of weathered,
// splintered timber (old barn boards, broken planks):
//   1. grain: every polygon gets a grain direction. Automatic mode welds the
//      mesh into parts (planks) and takes each part's principal axis (area-
//      weighted PCA), so a mesh of crossed boards gets crossed grain; near-
//      cubic parts and Fixed mode use the Grain Axis. Every frame below is
//      (t along the grain, b across it, n out of the surface);
//   2. patches: a tear patch is a local grain frame on the surface plus a 2D
//      signed field s0(a,c) > 0 inside the outline. Automatic mode builds it
//      from a few overlapping blobs stretched along the grain (area-weighted
//      seeds, pulled toward plank ends and edges). Paint mode builds it from
//      the drawn strokes, as concrete damage does: a closed stroke is the
//      outline, an open stroke is swept with Paint Radius, split where the
//      surface turns;
//   3. splinters: the patch is cut into fibre strips across the grain (random
//      widths). Where the outline faces along the grain (the tear's ends,
//      found from the gradient of s0), each strip pushes the outline out by
//      its own length with a pointed tip, so the ends break into ragged
//      splinter fingers. Each strip tears to its own depth layer and peels up
//      gradually toward its tip; the floor carries fibrous ridges;
//   4. grooves: weathered growth rings - grooves along the grain anchored in
//      object space, so overlapping patches agree - cut inside the tear and in
//      a fading halo around it;
//   5. break through: in some patches the core (with its splinter fingers)
//      goes through the whole board, which at a plank end reads as a broken-
//      off board end;
//   6. the cutter is a height-field slab over the grid in the patch frame
//      (identical construction to concrete damage: rays per column, misses
//      extrapolated, outside columns float above the surface so the slab is
//      always a closed solid that cuts exactly the damaged region);
//   7. splits: V-groove checks that run straight along the grain out of the
//      tear ends, out of knots, and free over the surface (Global);
//   8. knots: elliptical knot holes with a stepped rim, grain grooves bending
//      around them;
//   9. slivers: thin pointed solids ADDED back, rooted in the intact wood at
//      the tear ends, reaching over the torn area and curling up.
// Candidates use SplitMix64 counter channels keyed by seed (main seed and
// Wood Seed), patch id and channel, like concrete damage and fabric tears.
//
// Channel map (auto patch k, candidate c): c*8 triangle, c*8+1..3 barycentric;
// 3 size (rnd), 4 depth, 5 stretch, 6 angle, 7 lobes, 10+ lobes, 40-42 noise
// offset, 43 break, 44 split count. Strips: (patch id, 1000 + strip * 16 +
// sub-channel). Splits: (patch id, 200 + split * 16 + sub-channel). Knots
// use their own stream, global splits theirs.
#include "manifold/manifold.h"
#include "wood_damage.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

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

                                                                        
                                                                         
                                                         
m::Manifold SlabSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(size_t(ns + 1) * size_t(nw + 1) * 6);
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
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
	return Orient(mesh);
}

                                                                           
                                                                           
                                                                   
                                       
m::Manifold Sliver(const std::vector<V>& raw, const std::vector<double>& rawW,
	const std::vector<double>& rawH, const V& up)
{
	std::vector<V> pts;
	std::vector<double> hw, hh;
	for (size_t i = 0; i < raw.size(); ++i) {
		if (pts.empty() || m::la::length(raw[i] - pts.back()) > 1e-4) { pts.push_back(raw[i]); hw.push_back(rawW[i]); hh.push_back(rawH[i]); }
	}
	if (pts.size() < 2) return m::Manifold();
	const size_t n = pts.size();
	static const double sx[6] = {1.0, 0.45, -0.45, -1.0, -0.45, 0.45};
	static const double sy[6] = {0.0, 1.0, 1.0, 0.0, -1.0, -1.0};
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	for (size_t i = 0; i < n; ++i) {
		V tan;
		if (i == 0) tan = Unit(pts[1] - pts[0], AnyPerp(up));
		else if (i + 1 == n) tan = Unit(pts[i] - pts[i - 1], AnyPerp(up));
		else tan = Unit(Unit(pts[i] - pts[i - 1], up) + Unit(pts[i + 1] - pts[i], up), AnyPerp(up));
		const V side = Unit(m::la::cross(up, tan), AnyPerp(tan));
		const V u2 = Unit(m::la::cross(tan, side), up);
		for (int k = 0; k < 6; ++k) push(pts[i] + side * (sx[k] * hw[i]) + u2 * (sy[k] * hh[i]));
	}
	const uint32_t c0 = uint32_t(n * 6), c1 = c0 + 1;
	push(pts[0]);
	push(pts[n - 1]);
	auto R = [&](size_t i, int k) { return uint32_t(i * 6 + size_t((k % 6 + 6) % 6)); };
	for (size_t i = 0; i + 1 < n; ++i)
		for (int k = 0; k < 6; ++k) {
			const uint32_t a = R(i, k), b = R(i + 1, k), c = R(i + 1, k + 1), d = R(i, k + 1);
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
		}
	for (int k = 0; k < 6; ++k) {
		mesh.triVerts.insert(mesh.triVerts.end(), {c0, R(0, k), R(0, k + 1)});
		mesh.triVerts.insert(mesh.triVerts.end(), {c1, R(n - 1, k + 1), R(n - 1, k)});
	}
	return Orient(mesh);
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> polyA;
	std::vector<V> polyG;                                   
	std::vector<int32_t> polyPart;                                        
	ProjectDamageBVH bvh;
	double diag = 1.0;
	const std::vector<bool>* avoid = nullptr;

	int32_t Part(int32_t poly) const { return poly >= 0 && size_t(poly) < polyPart.size() ? polyPart[size_t(poly)] : -1; }
	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}
	bool Ray(const V& P, const V& dir, double maxDist, double& d, int32_t& poly) const
	{
		poly = -1;
		return bvh.Raycast(P, dir, maxDist, &d, &poly) && poly >= 0;
	}
	                                                                      
	                                                              
	bool NormalNear(const V& P, double snap, V& N, int32_t& polyOut) const
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
				if (score < best) { best = score; N = pn; polyOut = poly; ok = true; }
			}
		}
		return ok && best <= 1.5 * snap;
	}
};

                                                                               
double PrincipalAxis(const double C[3][3], V& axis)
{
	int big = 0;
	for (int i = 1; i < 3; ++i) if (C[i][i] > C[big][big]) big = i;
	V x(0.0);
	x[big] = 1.0;
	x = Unit(x + V(0.013, 0.027, 0.041), V(1, 0, 0));
	double lam = 0.0;
	for (int it = 0; it < 64; ++it) {
		const V y(C[0][0] * x.x + C[0][1] * x.y + C[0][2] * x.z,
			C[1][0] * x.x + C[1][1] * x.y + C[1][2] * x.z,
			C[2][0] * x.x + C[2][1] * x.y + C[2][2] * x.z);
		lam = m::la::length(y);
		if (lam < 1e-300) break;
		x = y / lam;
	}
	axis = x;
	return lam;
}

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, const WoodDamageParams& p, Surf& s)
{
	const MeshView& v = in.view;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	s.polyA.assign(size_t(v.npolys), 0.0);
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
		s.polyA[size_t(i)] = 0.5 * m::la::length(cr);
		s.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
	}
	s.bvh.Build(v);
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = p.avoid ? in.avoidPoly : nullptr;

	const V fixedAxis = Unit(V(p.grainX, p.grainY, p.grainZ), V(0, 0, 1));
	s.polyG.assign(size_t(v.npolys), fixedAxis);

	                                                                      
	                      
	const double q = 1e-6 * s.diag;
	std::unordered_map<uint64_t, int32_t> cellRep;
	std::vector<int32_t> rep(size_t(v.npts));
	for (int32_t i = 0; i < v.npts; ++i) {
		const V& P = v.pts[i];
		const uint64_t key = (uint64_t(int64_t(std::floor((P.x - lo.x) / q)) & 0x1fffff) << 42)
			| (uint64_t(int64_t(std::floor((P.y - lo.y) / q)) & 0x1fffff) << 21)
			| uint64_t(int64_t(std::floor((P.z - lo.z) / q)) & 0x1fffff);
		auto it = cellRep.find(key);
		if (it == cellRep.end()) { cellRep.emplace(key, i); rep[size_t(i)] = i; }
		else rep[size_t(i)] = it->second;
	}
	std::vector<int32_t> parent(size_t(v.npts));
	for (int32_t i = 0; i < v.npts; ++i) parent[size_t(i)] = i;
	std::function<int32_t(int32_t)> find = [&](int32_t x) {
		while (parent[size_t(x)] != x) { parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; x = parent[size_t(x)]; }
		return x;
	};
	auto unite = [&](int32_t a, int32_t b) {
		a = find(rep[size_t(a)]); b = find(rep[size_t(b)]);
		if (a != b) parent[size_t(std::max(a, b))] = std::min(a, b);
	};
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& pq = v.polys[i];
		unite(pq.a, pq.b); unite(pq.a, pq.c); unite(pq.a, pq.d);
	}
	std::unordered_map<int32_t, size_t> partIndex;
	std::vector<int32_t>& polyPart = s.polyPart;
	polyPart.assign(size_t(v.npolys), 0);
	struct Acc { double w = 0.0; V m1 = V(0.0); double m2[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}; };
	std::vector<Acc> acc;
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& pq = v.polys[i];
		const int32_t root = find(rep[size_t(pq.a)]);
		auto it = partIndex.find(root);
		size_t pi;
		if (it == partIndex.end()) { pi = acc.size(); partIndex.emplace(root, pi); acc.emplace_back(); }
		else pi = it->second;
		polyPart[size_t(i)] = int32_t(pi);
		const double w = s.polyA[size_t(i)];
		const int nc = pq.c == pq.d ? 3 : 4;
		const int32_t ids[4] = {pq.a, pq.b, pq.c, pq.d};
		for (int c = 0; c < nc; ++c) {
			const V& P = v.pts[ids[c]];
			Acc& A = acc[pi];
			A.w += w;
			A.m1 = A.m1 + P * w;
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 3; ++k) A.m2[r][k] += w * P[r] * P[k];
		}
	}
	std::vector<V> partAxis(acc.size(), fixedAxis);
	if (p.grainMode != 0) return;                                             
	for (size_t pi = 0; pi < acc.size(); ++pi) {
		const Acc& A = acc[pi];
		if (A.w <= 0.0) continue;
		const V mean = A.m1 / A.w;
		double C[3][3];
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 3; ++k) C[r][k] = A.m2[r][k] / A.w - mean[r] * mean[k];
		V a1;
		const double l1 = PrincipalAxis(C, a1);
		double D[3][3];
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 3; ++k) D[r][k] = C[r][k] - l1 * a1[r] * a1[k];
		V a2;
		const double l2 = PrincipalAxis(D, a2);
		if (!(l1 > 1.4 * l2)) continue;                                                
		int big = 0;
		for (int r = 1; r < 3; ++r) if (std::abs(a1[r]) > std::abs(a1[big])) big = r;
		partAxis[pi] = a1[big] < 0.0 ? -a1 : a1;
	}
	for (int32_t i = 0; i < v.npolys; ++i) s.polyG[size_t(i)] = partAxis[size_t(polyPart[size_t(i)])];
}

                                                                    
void GrainFrame(const V& n, const V& grain, V& t, V& b)
{
	V g = grain - n * m::la::dot(grain, n);
	if (m::la::length(g) < 0.25) {                                         
		const V up = std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0);
		g = up - n * m::la::dot(up, n);
	}
	t = Unit(g, AnyPerp(n));
	b = m::la::cross(n, t);
}

                                                                       
                                                                         
struct EdgeGrid {
	std::vector<std::pair<V, V>> segs;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	double cell = 1.0;
	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	void Build(const Surf& s, double cellSize, bool ends)
	{
		const MeshView& v = *s.v;
		cell = std::max(cellSize, s.diag * 1e-3);
		std::unordered_map<uint64_t, std::pair<int32_t, int32_t>> edges;
		auto add = [&](int32_t a, int32_t b, int32_t poly) {
			const uint64_t k = a < b ? (uint64_t(uint32_t(a)) << 32) | uint32_t(b) : (uint64_t(uint32_t(b)) << 32) | uint32_t(a);
			auto it = edges.find(k);
			if (it == edges.end()) edges.emplace(k, std::make_pair(poly, -1));
			else if (it->second.second < 0) it->second.second = poly;
		};
		for (int32_t i = 0; i < v.npolys; ++i) {
			const ProjectDamagePoly& q = v.polys[i];
			if (q.c == q.d) { add(q.a, q.b, i); add(q.b, q.c, i); add(q.c, q.a, i); }
			else { add(q.a, q.b, i); add(q.b, q.c, i); add(q.c, q.d, i); add(q.d, q.a, i); }
		}
		std::vector<uint64_t> keys;
		keys.reserve(edges.size());
		for (const auto& e : edges) keys.push_back(e.first);
		std::sort(keys.begin(), keys.end());                         
		for (uint64_t k : keys) {
			const auto& f = edges[k];
			if (f.second < 0) continue;
			if (m::la::dot(s.polyN[size_t(f.first)], s.polyN[size_t(f.second)]) > 0.766) continue;                  
			const V a = v.pts[int32_t(k >> 32)], b = v.pts[int32_t(k & 0xffffffffULL)];
			if (ends && std::abs(m::la::dot(Unit(b - a, V(0, 0, 0)), s.polyG[size_t(f.first)])) > 0.4) continue;
			const int32_t id = int32_t(segs.size());
			segs.push_back({a, b});
			const int steps = std::max(1, int(std::ceil(m::la::length(b - a) / cell)));
			uint64_t last = ~0ULL;
			for (int t = 0; t <= steps; ++t) {
				const V q = a + (b - a) * (double(t) / steps);
				const uint64_t key = Key(int64_t(std::floor(q.x / cell)), int64_t(std::floor(q.y / cell)), int64_t(std::floor(q.z / cell)));
				if (key == last) continue;
				last = key;
				auto& bucket = grid[key];
				if (bucket.empty() || bucket.back() != id) bucket.push_back(id);
			}
		}
	}
	double Nearest(const V& p, int rings) const
	{
		if (segs.empty()) return 1e300;
		const int64_t cx = int64_t(std::floor(p.x / cell)), cy = int64_t(std::floor(p.y / cell)), cz = int64_t(std::floor(p.z / cell));
		double best = 1e300;
		for (int64_t dx = -rings; dx <= rings; ++dx)
			for (int64_t dy = -rings; dy <= rings; ++dy)
				for (int64_t dz = -rings; dz <= rings; ++dz) {
					auto it = grid.find(Key(cx + dx, cy + dy, cz + dz));
					if (it == grid.end()) continue;
					for (int32_t id : it->second) {
						const V& a = segs[size_t(id)].first;
						const V ab = segs[size_t(id)].second - a;
						const double l2 = m::la::dot(ab, ab);
						const double t = l2 > 0.0 ? ClampD(m::la::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
						best = std::min(best, m::la::length(p - (a + ab * t)));
					}
				}
		return best;
	}
};

                                                                                    

enum class Shape { Blobs, Polygon, Stroke };
struct Blob { double x, y, r; };

struct Patch {
	Shape shape = Shape::Blobs;
	bool knot = false;
	std::vector<int32_t> parts;                                                    
	uint64_t id = 0;                       
	V o, n, t, b;                                                  
	std::vector<Blob> blobs;                                  
	double ca = 1.0, sa = 0.0, invE = 1.0;
	std::vector<V> poly;                                                           
	std::vector<double> arc;                                           
	double strokeLen = 0.0, brush = 0.0;
	double R = 1.0;                                         
	double Rn = 1.0;                                              
	double D = 1.0;                        
	double sc = 1.0;                            
	bool brk = false;                                    
	int splits = 0;
	V off;                                   
	double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

                                                                            
double RawField(const Patch& P, double x, double y)
{
	switch (P.shape) {
	case Shape::Blobs: {
		const double xs = (x * P.ca + y * P.sa) * P.invE, ys = -x * P.sa + y * P.ca;
		double s = -1e300;
		const double k = 0.25 * P.R;
		for (const Blob& b : P.blobs) {
			const double d = b.r - std::sqrt((xs - b.x) * (xs - b.x) + (ys - b.y) * (ys - b.y));
			if (s <= -1e299) { s = d; continue; }
			const double h = ClampD(0.5 + 0.5 * (d - s) / k, 0.0, 1.0);
			s = Lerp(s, d, h) + k * h * (1.0 - h);                
		}
		return s;
	}
	case Shape::Polygon: {
		bool inside = false;
		double nearest = 1e300;
		const auto& xy = P.poly;
		for (size_t i = 0, j = xy.size() - 1; i < xy.size(); j = i++) {
			const V a = xy[j], b = xy[i];
			if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
			const V q(x - a.x, y - a.y, 0.0), ab = b - a;
			const double den = m::la::dot(ab, ab);
			const double t = den > 0.0 ? ClampD(m::la::dot(q, ab) / den, 0.0, 1.0) : 0.0;
			nearest = std::min(nearest, m::la::length(q - ab * t));
		}
		return inside ? nearest : -nearest;
	}
	case Shape::Stroke: {
		double best = 1e300, bestArc = 0.0;
		const auto& xy = P.poly;
		for (size_t i = 0; i + 1 < xy.size(); ++i) {
			const V ab = xy[i + 1] - xy[i];
			const V q(x - xy[i].x, y - xy[i].y, 0.0);
			const double den = m::la::dot(ab, ab);
			const double t = den > 0.0 ? ClampD(m::la::dot(q, ab) / den, 0.0, 1.0) : 0.0;
			const double d = m::la::length(q - ab * t);
			if (d < best) { best = d; bestArc = Lerp(P.arc[i], P.arc[i + 1], t); }
		}
		const double endDist = std::min(bestArc, P.strokeLen - bestArc);
		const double rr = P.brush * (0.55 + 0.45 * Smooth(0.0, 2.0 * P.brush, endDist));
		return rr - best;
	}
	}
	return -1.0;
}

                                                                                      

struct Ctx {
	const WoodDamageParams* p;
	const Surf* s;
	uint64_t seed;
};

bool InParts(const std::vector<int32_t>& parts, int32_t part)
{
	return parts.empty() || std::find(parts.begin(), parts.end(), part) != parts.end();
}

                                                                          
                                                                         
                                                                           
                                                     
double Groove(const Ctx& cx, const V& W, const V& t, const V& b, const V& n, double bend)
{
	const WoodDamageParams& p = *cx.p;
	const double sp = std::max(1e-4, p.grooveSpacing);
	const double ya = m::la::dot(W, t), zb = m::la::dot(W, n);
	double x = (m::la::dot(W, b) + bend) / sp;
	x += ClampD(p.grooveWave, 0.0, 3.0) * 0.9 * Fbm(V(ya / (30.0 * sp), x * 0.07, zb / (6.0 * sp)), 3);
	const double ring = std::floor(x);
	const double f = x - ring;
	const double tri = 1.0 - std::abs(2.0 * f - 1.0);
	const double ringDepth = Lerp(0.45, 1.15, Random01(cx.seed ^ 0x52494e47ULL, uint64_t(int64_t(ring)), 0));            
	double g = Smooth(0.12, 0.65, tri) * ringDepth;
	const double br = ClampD(p.grooveBreakup, 0.0, 1.0);
	if (br > 0.0) {
		const double e = Fbm(V(ya / (14.0 * sp), x * 0.35, zb / (6.0 * sp)) + V(7.7, 3.1, -2.9), 3);
		g *= Lerp(1.0, Smooth(-0.25, 0.35, e), br);
	}
	return g;
}

                                                                             
                                                                               
void WalkSplit(const Ctx& cx, const V& o, const V& n, const V& t, const V& b,
	double x, double y, double ang, double len, double w, double dep, double reach, uint64_t key,
	const std::vector<int32_t>& parts, const std::function<double(double, double)>& floorAt,
	std::vector<m::Manifold>& out)
{
	const WoodDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	const double step = std::max(1.5 * w, len / 64.0);
	const int ns = std::max(2, int(std::ceil(len / step)));
	const V noff(Random01(cx.seed, key, 8) * 1000.0, Random01(cx.seed, key, 9) * 1000.0, 0.0);
	const double ang0 = ang;
	std::vector<V> topv, botv;
	int rows = 0;
	for (int i = 0; i <= ns; ++i) {
		const double f = double(i) / ns;
		if (i > 0) {
			                                                  
			ang += ClampD(p.checkMeander, 0.0, 2.0) * 1.5 * Fbm(V(f * 4.0, 0.5, 0.0) + noff, 3) * (step / (0.15 * len + 1e-6));
			ang = Lerp(ang, ang0, 0.15);
			x += std::cos(ang) * step;
			y += std::sin(ang) * step;
		}
		const V org = o + t * x + b * y + n * reach;
		double dist = 0.0;
		int32_t poly = -1;
		if (!s.Ray(org, -n, 2.0 * reach + 4.0 * dep, dist, poly) || m::la::dot(s.polyN[size_t(poly)], n) < 0.5 || s.Avoided(poly) || !InParts(parts, s.Part(poly))) break;
		const V S = org - n * dist;
		const V dirW = Unit(t * std::cos(ang) + b * std::sin(ang), t);
		const V side = Unit(m::la::cross(n, dirW), b);
		                                                   
		const double env = Smooth(0.0, 0.12, f) * (1.0 - 0.85 * Smooth(0.45, 1.0, f)) * 0.85 + 0.15;
		const double hw = 0.5 * w * env + 1e-4;
		double d = dep * (0.25 + 0.75 * env);
		const double fl = floorAt(x, y);
		if (fl < 0.0) d = 0.05 * dep;
		else d += fl;
		const double mc = 0.25 * dep + 2.0 * hw;
		topv.push_back(S - side * hw + n * mc); topv.push_back(S + n * mc); topv.push_back(S + side * hw + n * mc);
		botv.push_back(S - side * hw + n * (0.3 * mc)); botv.push_back(S - n * d); botv.push_back(S + side * hw + n * (0.3 * mc));
		++rows;
	}
	if (rows >= 3) {
		m::Manifold c = SlabSolid(topv, botv, rows - 1, 2);
		if (!c.IsEmpty()) out.push_back(std::move(c));
	}
}

                                                                                      

struct Out {
	m::Manifold cutter;
	std::vector<m::Manifold> splits, slivers;
};

struct Strip { double lo, hi, tip; double rEnd[2]; double level, ramp; uint64_t key; };

// строим одну область повреждения и её внутренние детали
void BuildPatch(const Ctx& cx, const Patch& P, Out& out)
{
	const WoodDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	auto rnd = [&](uint64_t ch) { return Random01(cx.seed ^ 0x574f4f44ULL, P.id, ch); };            
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const bool grooves = p.grooveEnable && p.grooveDepth > 0.0;
	const double halo = grooves ? ClampD(p.weatherHalo, 0.0, 3.0) * P.R : 0.0;
	const double Lsp = P.knot ? 0.0 : std::max(0.0, p.splinterLength) * P.sc;
	const double ampOut = jag * 0.3 * P.Rn * (P.knot ? 0.4 : 1.0);
	const double featOut = 0.5 * P.Rn;
	const double padV = halo + 0.6 * ampOut + 0.08 * P.R + 1e-3;
	const double padU = padV + 0.8 * Lsp;
	const double u0 = P.u0 - padU, u1 = P.u1 + padU, v0 = P.v0 - padV, v1 = P.v1 + padV;
	const double extU = u1 - u0, extV = v1 - v0;
	if (!(extU > 0.0) || !(extV > 0.0)) return;
	const double fiberW = std::max(1e-3, p.fiberWidth * P.sc);

	                                                                           
	const double base = std::max(extU, extV) / (110.0 * p.detail);
	double cellV = std::min(base, fiberW / 6.0);
	if (grooves) cellV = std::min(cellV, p.grooveSpacing / 2.6);
	cellV = std::max(cellV, extV / 700.0);
	double cellU = std::min(base, std::max(2.0 * cellV, 0.04 * std::max(Lsp, P.R)));
	cellU = std::max(cellU, extU / 700.0);
	{
		const double cells = (extU / cellU) * (extV / cellV);
		const double cap = 4.0e5 * p.detail * p.detail;
		if (cells > cap) { const double k = std::sqrt(cells / cap); cellU *= k; cellV *= k; }
	}
	const int nu = std::max(4, int(std::ceil(extU / cellU)));
	const int nv = std::max(4, int(std::ceil(extV / cellV)));
	const double cu = extU / nu, cv = extV / nv;
	const double cell = std::max(cu, cv);
	auto I = [&](int i, int j) { return size_t(i) * size_t(nv + 1) + size_t(j); };
	const size_t N = size_t(nu + 1) * size_t(nv + 1);
	std::vector<double> h(N, 0.0), d(N, 0.0), sf(N, -1.0), s0(N, -1.0), thick(N, -1.0), endW(N, 0.0);
	std::vector<char> valid(N, 0), holeF(N, 0), forbid(N, 0), outside(N, 0);
	std::vector<int32_t> stripOf(N, 0);

	const double Hs = 0.3 * std::max(extU, extV) + P.D + 1e-3;
	const double drop = 0.6 * std::max(extU, extV) + 2.0 * P.D;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double u = u0 + i * cu, v = v0 + j * cv;
			const V org = P.o + P.t * u + P.b * v + P.n * Hs;
			double dist = 0.0;
			int32_t poly = -1;
			if (s.Ray(org, -P.n, Hs + drop, dist, poly) && m::la::dot(s.polyN[size_t(poly)], P.n) > 0.05) {
				h[k] = Hs - dist;
				valid[k] = 1;
				forbid[k] = (s.Avoided(poly) || !InParts(P.parts, s.Part(poly))) ? 1 : 0;
				const V S = P.o + P.t * u + P.b * v + P.n * h[k];
				double th = 0.0;
				int32_t tp = -1;
				if (s.Ray(S - P.n * (1e-5 * s.diag), -P.n, 2.0 * s.diag, th, tp)) thick[k] = th;
			}
		}
	                                                                    
	{
		std::vector<char> known(valid);
		bool any = false;
		for (char c : known) any |= c != 0;
		if (!any) return;
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

	                                
	std::vector<Strip> strips;
	{
		const double jit = ClampD(p.fiberJitter, 0.0, 1.0);
		double c = v0;
		for (uint64_t k = 0; c < v1 && k < 20000; ++k) {
			const uint64_t ch = 1000 + k * 16;
			Strip st;
			st.key = ch;
			st.lo = c;
			const double w = fiberW * Lerp(1.0 - 0.75 * jit, 1.0 + 1.2 * jit, std::pow(rnd(ch), 1.6));
			st.hi = c + w;
			st.tip = Lerp(st.lo, st.hi, Lerp(0.2, 0.8, rnd(ch + 1)));
			st.rEnd[0] = std::pow(rnd(ch + 2), 1.3);
			st.rEnd[1] = std::pow(rnd(ch + 3), 1.3);
			st.level = Lerp(1.0 - 0.45 * ClampD(p.fiberDepthVar, 0.0, 1.0), 1.0 + 0.2 * ClampD(p.fiberDepthVar, 0.0, 1.0), rnd(ch + 4));
			st.ramp = Lerp(0.25, 1.0, rnd(ch + 5));
			strips.push_back(st);
			c = st.hi;
		}
	}
	auto stripAt = [&](double c) {
		size_t lo = 0, hi = strips.size();
		while (hi - lo > 1) {
			const size_t mid = (lo + hi) / 2;
			if (strips[mid].lo <= c) lo = mid; else hi = mid;
		}
		return lo;
	};

	                                                                           
	                                                                        
	                                                                       
	                                                             
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) s0[I(i, j)] = RawField(P, u0 + i * cu, v0 + j * cv);
	const double sharp = ClampD(p.splinterSharp, 0.0, 1.0);
	const int L = P.knot ? 1 : std::max(1, std::min(p.layers, 6));
	const double insetEnd = ClampD(p.layerInset, 0.0, 3.0) * Lsp;
	const double insetSide = 0.06 * P.R;
	std::vector<double> sB(N, -1.0), gAbs(N, 0.0);
	std::vector<char> gNeg(N, 0);
	auto layerField = [&](size_t k, double v, int l) {
		double sv = sB[k] - l * (insetSide + insetEnd * gAbs[k] * endW[k]);
		if (Lsp > 0.0 && endW[k] > 0.0) {
			const Strip& st = strips[size_t(stripOf[k])];
			const double half = v < st.tip ? st.tip - st.lo : st.hi - st.tip;
			const double e = ClampD(std::abs(v - st.tip) / std::max(1e-9, half), 0.0, 1.0);
			                                                                
			const double shape = sharp <= 0.5 ? Lerp(1.0, 1.0 - e, 2.0 * sharp) : std::pow(1.0 - e, Lerp(1.0, 2.0, 2.0 * sharp - 1.0));
			const int side = gNeg[k] ? 1 : 0;
			const double rs = l == 0 ? st.rEnd[side] : std::pow(rnd(st.key + 20 + uint64_t(l) * 2 + uint64_t(side)), 1.3);
			                                                     
			const double chunk = ClampD(0.5 + 0.9 * Fbm(V(0.5 * (st.lo + st.hi) / (3.5 * fiberW), 13.1 * side + 7.3 * l, 0.0) + P.off, 2), 0.0, 1.0);
			const double r = Lerp(rs, chunk, ClampD(p.splinterChunk, 0.0, 1.0));
			                                                              
			sv += Lsp * (r * shape - 0.3) * gAbs[k] * endW[k];
		}
		return sv;
	};
	double sMax = -1e300;
	size_t kMax = 0;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double u = u0 + i * cu, v = v0 + j * cv;
			stripOf[k] = int32_t(stripAt(v));
			                                                          
			const int ia = std::max(0, i - 1), ib = std::min(nu, i + 1), ja = std::max(0, j - 1), jb = std::min(nv, j + 1);
			const double ga = (s0[I(ib, j)] - s0[I(ia, j)]) / ((ib - ia) * cu);
			const double gc = (s0[I(i, jb)] - s0[I(i, ja)]) / ((jb - ja) * cv);
			const double kdir = std::abs(ga) / (std::sqrt(ga * ga + gc * gc) + 1e-12);
			endW[k] = Smooth(0.35, 0.85, kdir);
			gAbs[k] = std::min(1.0, std::abs(ga));
			gNeg[k] = ga < 0.0 ? 1 : 0;
			sB[k] = s0[k] + ampOut * Fbm(V(u / (2.5 * featOut), v / (0.6 * featOut), 0.0) + P.off, 4);
			double sv = layerField(k, v, 0);
			if (forbid[k]) sv = std::min(sv, -cell);
			sf[k] = sv;
			if (sv > sMax) { sMax = sv; kMax = k; }
		}
	const double Wall = 1.2 * cell;
	if (sMax < 1.5 * Wall) return;                       

	                
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double margin = std::max(0.3 * P.D, 2.0 * cell);
	const double lift = 0.4 * margin;
	const double breakThr = (1.0 - ClampD(p.breakSize, 0.0, 1.0)) * sMax;
	const double ampB = jag * 0.25 * sMax;
	const double ff = ClampD(p.floorFibers, 0.0, 2.0);
	const double rampL = ClampD(p.splinterRamp, 0.0, 1.0) * Lsp;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double sv = sf[k];
			const double u = u0 + i * cu, v = v0 + j * cv;
			const V W = P.o + P.t * u + P.b * v + P.n * h[k];
			double dd = 0.0;
			if (sv > 0.0) {
				if (P.knot) {
					dd = P.D * (0.35 * Smooth(0.0, Wall, sv) + 0.65 * Smooth(0.25 * sMax, 0.75 * sMax, sv));
					dd += 0.12 * P.D * Fbm(W / (0.3 * P.R) + P.off, 3);
				} else {
					const Strip& st = strips[size_t(stripOf[k])];
					const double ramp = Wall + rampL * st.ramp * endW[k];
					for (int l = 0; l < L; ++l) {
						const double sl = l == 0 ? sv : layerField(k, v, l);
						if (sl <= 0.0) continue;
						dd += P.D / L * Smooth(0.0, l == 0 ? ramp : Wall + 0.5 * (ramp - Wall), sl);
					}
					dd *= st.level;
					if (ff > 0.0) {
						const double fib = Fbm(V(u / (4.0 * fiberW), v / (0.4 * fiberW), 0.0) + P.off + V(3.1, -7.7, 1.3), 3);
						dd += ff * 0.3 * P.D * fib * Smooth(0.0, 2.0 * Wall, sv);
					}
				}
				if (grooves) dd += p.grooveDepth * Groove(cx, W, P.t, P.b, P.n, 0.0);
				dd = std::max(dd, 0.25 * p.grooveDepth * Smooth(0.0, Wall, sv));
				if (thick[k] > 0.0) dd = std::min(dd, cap * thick[k]);
				if (P.brk) {
					                                                          
					const double sd = L > 1 ? layerField(k, v, L - 1) : sv;
					const double sh = sd - breakThr + ampB * Fbm(V(u / (2.0 * featOut), v / (0.5 * featOut), 0.0) + P.off + V(-21.0, 4.0, 13.0), 4);
					if (sh > 0.0) {
						const double f = Smooth(0.0, 0.7 * Wall, sh);
						const double through = (thick[k] > 0.0 ? thick[k] : 3.0 * P.D + extU) + margin;
						dd = Lerp(dd, through, f);
						if (f > 0.5) holeF[k] = 1;
					}
				}
			} else if (halo > 0.0 && sv > -halo && !forbid[k]) {
				double bend = 0.0;
				if (P.knot) {                                   
					const double r2 = (u * u) / (6.0 * P.R * P.R) + (v * v) / (2.0 * P.R * P.R);
					bend = (v < 0.0 ? -1.0 : 1.0) * 0.9 * P.R * std::exp(-r2);
				}
				                                                                  
				                                                        
				const double reachN = Fbm(V(u / (1.5 * P.R), v / (0.25 * P.R), 0.0) + P.off + V(9.0, 2.0, -5.0), 3);
				const double fade = Smooth(-halo, -0.1 * halo, sv + 0.5 * halo * reachN + 0.3 * halo * endW[k]);
				const double g = Groove(cx, W, P.t, P.b, P.n, bend);
				dd = p.grooveDepth * std::max(0.0, g - (1.0 - fade)) / std::max(0.25, fade);
				if (thick[k] > 0.0) dd = std::min(dd, cap * thick[k]);
			}
			if (dd <= 1e-6) { outside[k] = 1; dd = 0.0; }
			d[k] = dd;
		}

	                                                     
	{
		int i0 = nu, i1 = 0, j0 = nv, j1 = 0;
		for (int i = 0; i <= nu; ++i)
			for (int j = 0; j <= nv; ++j)
				if (!outside[I(i, j)]) { i0 = std::min(i0, i); i1 = std::max(i1, i); j0 = std::min(j0, j); j1 = std::max(j1, j); }
		if (i1 < i0) return;
		i0 = std::max(0, i0 - 1); j0 = std::max(0, j0 - 1); i1 = std::min(nu, i1 + 1); j1 = std::min(nv, j1 + 1);
		const int mu = i1 - i0, mv = j1 - j0;
		std::vector<V> top(size_t(mu + 1) * size_t(mv + 1)), bot(top.size());
		for (int i = i0; i <= i1; ++i)
			for (int j = j0; j <= j1; ++j) {
				const size_t k = I(i, j), ks = size_t(i - i0) * size_t(mv + 1) + size_t(j - j0);
				double hmax = -1e300;
				for (int di = -1; di <= 1; ++di)
					for (int dj = -1; dj <= 1; ++dj) {
						const int a = i + di, b = j + dj;
						if (a < 0 || b < 0 || a > nu || b > nv) continue;
						hmax = std::max(hmax, h[I(a, b)]);
					}
				const V base3 = P.o + P.t * (u0 + i * cu) + P.b * (v0 + j * cv);
				top[ks] = base3 + P.n * (hmax + margin);
				bot[ks] = base3 + P.n * (outside[k] ? hmax + lift : h[k] - d[k]);
			}
		out.cutter = SlabSolid(top, bot, mu, mv);
	}

	                               
	auto sample = [&](const std::vector<double>& f, double u, double v, bool& ok) {
		const double fu = (u - u0) / cu, fv = (v - v0) / cv;
		ok = !(fu < 0.0 || fv < 0.0 || fu > nu || fv > nv);
		if (!ok) return 0.0;
		const int i = std::min(nu - 1, int(fu)), j = std::min(nv - 1, int(fv));
		const double a = fu - i, c = fv - j;
		ok = valid[I(i, j)] && valid[I(i + 1, j)] && valid[I(i, j + 1)] && valid[I(i + 1, j + 1)];
		return (f[I(i, j)] * (1 - a) + f[I(i + 1, j)] * a) * (1 - c) + (f[I(i, j + 1)] * (1 - a) + f[I(i + 1, j + 1)] * a) * c;
	};
	auto nearestK = [&](double u, double v) {
		const int i = std::max(0, std::min(nu, int(std::lround((u - u0) / cu))));
		const int j = std::max(0, std::min(nv, int(std::lround((v - v0) / cv))));
		return I(i, j);
	};
	                                                                         
	                                                                         
	auto cutEnd = [&](double uc, double c, int dir, double& uEnd) {
		const int j = std::max(0, std::min(nv, int(std::lround((c - v0) / cv))));
		int i = std::max(0, std::min(nu, int(std::lround((uc - u0) / cu))));
		if (sf[I(i, j)] <= 0.0) return false;
		while (i + dir >= 0 && i + dir <= nu && sf[I(i + dir, j)] > 0.0) i += dir;
		if (i + dir < 0 || i + dir > nu) return false;
		uEnd = u0 + i * cu;
		return true;
	};
	const double ucMax = u0 + double(kMax / size_t(nv + 1)) * cu;
	const double vcMax = v0 + double(kMax % size_t(nv + 1)) * cv;

	                                                                                  
	if (p.checkEnable && P.splits > 0 && p.checkLengthMax > 0.0 && p.checkWidth > 0.0) {
		auto floorAt = [&](double u, double v) {
			bool ok = false;
			const double dd = sample(d, u, v, ok);
			if (!ok) return 0.0;
			return holeF[nearestK(u, v)] ? -1.0 : dd;
		};
		const double halfV = 0.5 * (P.v1 - P.v0);
		for (int c = 0; c < P.splits; ++c) {
			const uint64_t ch = 200 + uint64_t(c) * 16;
			const int dir = rnd(ch) < 0.5 ? -1 : 1;
			const double cc = (P.knot ? 0.0 : vcMax) + (rnd(ch + 1) - 0.5) * (P.knot ? 0.6 : 1.4) * halfV;
			double ue = 0.0;
			if (!cutEnd(P.knot ? 0.0 : ucMax, cc, dir, ue)) continue;
			const double len = Lerp(p.checkLengthMin, p.checkLengthMax, std::pow(rnd(ch + 2), 1.4)) * (P.knot ? 0.5 : 1.0);
			const double w = p.checkWidth * Lerp(0.6, 1.3, rnd(ch + 3)) * (P.knot ? 0.8 : 1.0);
			const double dep = p.checkDepth * Lerp(0.6, 1.3, rnd(ch + 4));
			const double ang = (dir > 0 ? 0.0 : m::kPi) + (rnd(ch + 5) - 0.5) * 0.06;
			WalkSplit(cx, P.o, P.n, P.t, P.b, ue - dir * 2.0 * cell, cc, ang, len, w, dep, Hs,
				(P.id << 8) ^ ch, P.parts, floorAt, out.splits);
		}
	}

	                                                                                    
	if (!P.knot && p.sliverEnable && p.sliverAmount > 0.0 && p.sliverLength > 0.0 && p.sliverThick > 0.0) {
		const double amount = ClampD(p.sliverAmount, 0.0, 1.0);
		int made = 0;
		for (size_t si = 0; si < strips.size() && made < 64; ++si) {
			const Strip& st = strips[si];
			const double cc = 0.5 * (st.lo + st.hi);
			for (int side = 0; side < 2; ++side) {
				const uint64_t ch = st.key + 8 + uint64_t(side) * 4;
				if (rnd(ch) >= amount) continue;
				const int dir = side == 0 ? 1 : -1;
				double ue = 0.0;
				if (!cutEnd(ucMax, cc, dir, ue)) continue;
				const size_t ke = nearestK(ue, cc);
				if (endW[ke] < 0.5 || holeF[ke]) continue;
				const double len = p.sliverLength * P.sc * Lerp(0.5, 1.2, rnd(ch + 1));
				                                    
				bool ok = false;
				const double dMid = sample(d, ue - dir * 0.5 * len, cc, ok);
				if (!ok) continue;
				const double hh0 = 0.5 * p.sliverThick * P.sc;
				if (dMid < 3.0 * hh0) continue;
				const double hw0 = std::min(0.5 * p.sliverWidth * P.sc, 0.4 * (st.hi - st.lo));
				const double z0 = Lerp(1.3 * hh0, std::max(1.3 * hh0, 0.5 * dMid), rnd(ch + 2));
				const double embed = std::max(2.0 * cell, p.sliverEmbed * P.sc);
				const double lift = ClampD(p.sliverLift, 0.0, 2.0) * len * Lerp(0.5, 1.2, rnd(ch + 3));
				const double drift = (rnd(ch + 4) - 0.5) * 0.4 * (st.hi - st.lo);
				std::vector<V> ctr;
				std::vector<double> ws, hs;
				const int nsm = 14;
				bool good = true;
				for (int q = 0; q <= nsm; ++q) {
					const double a = -embed + (embed + len) * double(q) / nsm;                      
					const double f = std::max(0.0, a / len);
					const double u = ue - dir * a, v = cc + drift * f * f;
					bool ok2 = false;
					const double hs0 = sample(h, u, v, ok2);
					if (!ok2) { good = false; break; }
					const double z = z0 - lift * f * f;
					ctr.push_back(P.o + P.t * u + P.b * v + P.n * (hs0 - z));
					ws.push_back(hw0 * std::max(0.08, 1.0 - 0.92 * std::pow(f, 1.3)));
					hs.push_back(hh0 * std::max(0.25, 1.0 - 0.7 * f));
				}
				if (!good) continue;
				m::Manifold sl = Sliver(ctr, ws, hs, P.n);
				if (!sl.IsEmpty()) { out.slivers.push_back(std::move(sl)); ++made; }
			}
		}
	}
}

}               

void PlaceWoodDamage(const CoreInput& in, const WoodDamageParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& splinters)
{
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	WoodDamageParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.amount = std::max(0, std::min(p.amount, 500));
	p.lobes = std::max(1, std::min(p.lobes, 8));
	p.checkAmount = std::max(0, std::min(p.checkAmount, 32));
	p.checkGlobal = std::max(0, std::min(p.checkGlobal, 2000));
	p.knotAmount = std::max(0, std::min(p.knotAmount, 500));
	p.knotChecks = std::max(0, std::min(p.knotChecks, 8));
	p.stretch = ClampD(p.stretch, 1.0, 10.0);
	p.knotStretch = ClampD(p.knotStretch, 1.0, 6.0);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	if (p.depthMax < p.depthMin) std::swap(p.depthMin, p.depthMax);
	if (p.checkLengthMax < p.checkLengthMin) std::swap(p.checkLengthMin, p.checkLengthMax);
	p.sizeMin = std::max(1e-4, p.sizeMin);
	p.depthMin = std::max(1e-4, p.depthMin);
	p.paintRadius = std::max(1e-4, p.paintRadius);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	p.grooveSpacing = std::max(1e-3, p.grooveSpacing);
	p.fiberWidth = std::max(1e-3, p.fiberWidth);
	const bool paint = p.mode == 1;

	Surf s;
	BuildSurf(in, p, s);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x574f4f4444414dULL)                 
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x5744;                             

	Ctx cx;
	cx.p = &p;
	cx.s = &s;
	cx.seed = seed;

	std::vector<double> cdf(size_t(v.npolys));
	double total = 0.0;
	for (int32_t i = 0; i < v.npolys; ++i) {
		total += s.Avoided(i) ? 0.0 : s.polyA[size_t(i)];
		cdf[size_t(i)] = total;
	}
	auto samplePoint = [&](uint64_t stream, uint64_t k, uint64_t c, V& pos) {
		const double pick = Random01(stream, k, c * 8) * total;
		int32_t ti = int32_t(std::upper_bound(cdf.begin(), cdf.end(), pick) - cdf.begin());
		if (ti >= v.npolys) ti = v.npolys - 1;
		const ProjectDamagePoly& q = v.polys[ti];
		double r1 = Random01(stream, k, c * 8 + 1), r2 = Random01(stream, k, c * 8 + 2);
		V A = v.pts[q.a], B = v.pts[q.b], C = v.pts[q.c];
		if (q.c != q.d && Random01(stream, k, c * 8 + 3) < 0.5) { B = v.pts[q.c]; C = v.pts[q.d]; }
		if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
		pos = A * (1.0 - r1 - r2) + B * r1 + C * r2;
		return ti;
	};

	std::vector<Patch> patches;

	if (!paint && p.amount > 0 && total > 0.0) {
		                                                                             
		const double endB = ClampD(p.endBias, 0.0, 1.0), edgeB = ClampD(p.edgeBias, 0.0, 1.0);
		EdgeGrid endGrid, edgeGrid;
		if (endB > 0.0) endGrid.Build(s, p.sizeMax, true);
		if (edgeB > 0.0) edgeGrid.Build(s, p.sizeMax, false);
		const int cands = 1 + int(std::round(std::max(endB, edgeB) * 6.0));
		const double capD = 4.0 * p.sizeMax * p.stretch;
		const uint64_t as = seed ^ 0x4155544fULL;            
		std::vector<std::pair<V, double>> placed;
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.amount) * 30 && accepted < p.amount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(as, k, ch); };
			double best = 1e300;
			V pos(0.0);
			int32_t pi = -1;
			for (int c = 0; c < cands; ++c) {
				V cand;
				const int32_t ti = samplePoint(as, k, uint64_t(c), cand);
				double score = 0.0;
				if (cands > 1) {
					const int rings = 2;
					if (endB > 0.0) score += endB * std::min(capD, endGrid.Nearest(cand, rings));
					if (edgeB > 0.0) score += edgeB * std::min(capD, edgeGrid.Nearest(cand, rings));
				}
				if (pi < 0 || score < best) { best = score; pos = cand; pi = ti; }
			}
			if (pi < 0) continue;
			if (mask && !mask->Keep(pos, int32_t(k), kStream)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			Patch P;
			P.shape = Shape::Blobs;
			P.id = k;
			P.sc = sc;
			P.n = s.polyN[size_t(pi)];
			GrainFrame(P.n, s.polyG[size_t(pi)], P.t, P.b);
			P.parts.push_back(s.Part(pi));
			P.o = pos;
			P.R = Lerp(p.sizeMin, p.sizeMax, rnd(3)) * sc;
			P.Rn = P.R;
			P.D = Lerp(p.depthMin, p.depthMax, rnd(4)) * sc;
			const double e = Lerp(1.0 + 0.5 * (p.stretch - 1.0), p.stretch, rnd(5));
			const double ang = (rnd(6) - 0.5) * 0.2;                        
			P.ca = std::cos(ang); P.sa = std::sin(ang); P.invE = 1.0 / e;
			const int nb = 1 + int(std::floor(rnd(7) * p.lobes));
			P.blobs.push_back({0.0, 0.0, 0.55 * P.R});
			for (int b = 1; b < std::min(nb, p.lobes); ++b) {
				const double a = rnd(10 + uint64_t(b) * 3) * 2.0 * m::kPi;
				const double dist = Lerp(0.35, 0.75, rnd(11 + uint64_t(b) * 3)) * P.R;
				P.blobs.push_back({std::cos(a) * dist, std::sin(a) * dist, Lerp(0.3, 0.55, rnd(12 + uint64_t(b) * 3)) * P.R});
			}
			double ext = 0.0;
			for (const Blob& b : P.blobs) ext = std::max(ext, std::sqrt(b.x * b.x + b.y * b.y) + b.r);
			P.u0 = -ext * e * 1.05; P.u1 = ext * e * 1.05;
			P.v0 = -ext * 1.2; P.v1 = ext * 1.2;
			P.off = V(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			P.brk = p.breakEnable && rnd(43) < ClampD(p.breakChance, 0.0, 1.0);
			P.splits = p.checkEnable ? int(std::round(p.checkAmount * Lerp(0.6, 1.4, rnd(44)))) : 0;
			bool clash = false;
			for (const auto& q : placed)
				if (m::la::length(q.first - pos) < 0.6 * (q.second + P.R)) { clash = true; break; }
			if (clash) continue;
			placed.push_back({pos, P.R});
			patches.push_back(std::move(P));
			++accepted;
		}
	} else if (paint && paintStrokes && !paintStrokes->empty()) {
		                                                                             
		const uint64_t ps = seed ^ 0x5041494eULL;            
		for (size_t si = 0; si < paintStrokes->size(); ++si) {
			std::vector<V> pts;
			for (const EVec& q : (*paintStrokes)[si])
				if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
					&& (pts.empty() || m::la::length(q - pts.back()) > 1e-9)) pts.push_back(q);
			if (pts.size() < 2) continue;
			double len = 0.0;
			for (size_t i = 0; i + 1 < pts.size(); ++i) len += m::la::length(pts[i + 1] - pts[i]);
			if (len <= 1e-6) continue;
			auto rnd = [&](uint64_t ch) { return Random01(ps, si, ch); };
			const double gapEnd = m::la::length(pts.back() - pts.front());
			bool closed = pts.size() >= 4 && (gapEnd < 1e-6 * len + 1e-9 || gapEnd < ClampD(p.paintClose, 0.0, 1.0) * len);
			const V mid = pts[pts.size() / 2];
			if (mask && !mask->Keep(mid, int32_t(si), kStream + 1)) continue;
			const double sc = mask ? mask->SizeScale(mid) : 1.0;
			if (sc <= 0.0) continue;
			const double D = Lerp(p.depthMin, p.depthMax, rnd(3)) * sc;
			const bool brk = p.breakEnable && rnd(43) < ClampD(p.breakChance, 0.0, 1.0);
			const V off(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			const double brush = p.paintRadius * sc;

			if (closed) {
				if (gapEnd < 1e-6 * len + 1e-9) pts.pop_back();
				V nsum(0.0), gsum(0.0), cen(0.0);
				std::vector<int32_t> parts;
				int hits = 0;
				const size_t stride = std::max<size_t>(1, pts.size() / 48);
				for (size_t i = 0; i < pts.size(); i += stride) {
					V N;
					int32_t poly = -1;
					if (s.NormalNear(pts[i], p.paintSnap, N, poly)) {
						nsum = nsum + N;
						const V g = s.polyG[size_t(poly)];
						gsum = gsum + (m::la::dot(g, gsum) < 0.0 ? -g : g);
						if (!InParts(parts, s.Part(poly)) || parts.empty()) parts.push_back(s.Part(poly));
						++hits;
					}
				}
				for (const V& q : pts) cen = cen + q;
				cen = cen / double(pts.size());
				if (hits > 0 && m::la::length(nsum) / hits > 0.5) {
					Patch P;
					P.shape = Shape::Polygon;
					P.id = 0x100000ULL + si * 64;
					P.sc = sc;
					P.parts = parts;
					P.n = Unit(nsum, V(0, 0, 1));
					GrainFrame(P.n, Unit(gsum, V(0, 0, 1)), P.t, P.b);
					P.o = cen;
					double area = 0.0, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
					for (const V& q : pts) {
						const V dd = q - cen;
						const V l(m::la::dot(dd, P.t), m::la::dot(dd, P.b), 0.0);
						P.poly.push_back(l);
						x0 = std::min(x0, l.x); x1 = std::max(x1, l.x); y0 = std::min(y0, l.y); y1 = std::max(y1, l.y);
					}
					for (size_t i = 0, j = P.poly.size() - 1; i < P.poly.size(); j = i++)
						area += P.poly[j].x * P.poly[i].y - P.poly[i].x * P.poly[j].y;
					area = 0.5 * std::abs(area);
					if (area <= 1e-9) continue;
					P.R = std::sqrt(area / m::kPi);
					P.Rn = std::min(P.R, 1.5 * brush);
					P.D = D;
					P.u0 = x0; P.u1 = x1; P.v0 = y0; P.v1 = y1;
					P.off = off;
					P.brk = brk;
					P.splits = p.checkEnable ? p.checkAmount : 0;
					patches.push_back(std::move(P));
					continue;
				}
				pts.push_back(pts.front());                                                  
				closed = false;
			}

			                                                      
			const double step = std::max(std::min(0.5 * brush, len / 12.0), len / 2000.0);
			std::vector<V> rp;
			std::vector<double> ra;
			{
				double acc = 0.0, next = 0.0;
				for (size_t i = 0; i + 1 < pts.size(); ++i) {
					const double sl = m::la::length(pts[i + 1] - pts[i]);
					while (next <= acc + sl) {
						rp.push_back(pts[i] + (pts[i + 1] - pts[i]) * ((next - acc) / sl));
						ra.push_back(next);
						next += step;
					}
					acc += sl;
				}
				if (ra.empty() || ra.back() < len - 1e-9) { rp.push_back(pts.back()); ra.push_back(len); }
			}
			std::vector<V> rn(rp.size(), V(0.0)), rg(rp.size(), V(0.0));
			std::vector<int32_t> rpart(rp.size(), -1);
			std::vector<char> rok(rp.size(), 0);
			for (size_t i = 0; i < rp.size(); ++i) {
				V N;
				int32_t poly = -1;
				if (s.NormalNear(rp[i], p.paintSnap, N, poly)) { rn[i] = N; rg[i] = s.polyG[size_t(poly)]; rpart[i] = s.Part(poly); rok[i] = 1; }
			}
			std::vector<std::pair<size_t, size_t>> segs;
			{
				size_t a = 0;
				V first(0.0);
				bool have = false;
				for (size_t i = 0; i < rp.size(); ++i) {
					if (!rok[i]) continue;
					if (have && m::la::dot(rn[i], first) < 0.866) {
						segs.push_back({a, i - 1});
						a = i;
						have = false;
					}
					if (!have) { first = rn[i]; have = true; }
				}
				segs.push_back({a, rp.size() - 1});
			}
			for (size_t sg = 0; sg < segs.size(); ++sg) {
				const double fa = std::max(0.0, ra[segs[sg].first] - 2.0 * brush);
				const double fb = std::min(len, ra[segs[sg].second] + 2.0 * brush);
				V nsum(0.0), gsum(0.0);
				for (size_t i = segs[sg].first; i <= segs[sg].second; ++i)
					if (rok[i]) { nsum = nsum + rn[i]; gsum = gsum + (m::la::dot(rg[i], gsum) < 0.0 ? -rg[i] : rg[i]); }
				if (m::la::length(nsum) < 1e-9) continue;
				Patch P;
				P.shape = Shape::Stroke;
				P.id = 0x100000ULL + si * 64 + sg + 1;
				P.sc = sc;
				for (size_t i = 0; i < rp.size(); ++i)
					if (rok[i] && ra[i] >= fa - 1e-9 && ra[i] <= fb + 1e-9 && (P.parts.empty() || !InParts(P.parts, rpart[i])))
						P.parts.push_back(rpart[i]);
				P.n = Unit(nsum, V(0, 0, 1));
				GrainFrame(P.n, Unit(gsum, V(0, 0, 1)), P.t, P.b);
				P.o = rp[(segs[sg].first + segs[sg].second) / 2];
				double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
				for (size_t i = 0; i < rp.size(); ++i) {
					if (ra[i] < fa - 1e-9 || ra[i] > fb + 1e-9) continue;
					const V dd = rp[i] - P.o;
					const V l(m::la::dot(dd, P.t), m::la::dot(dd, P.b), 0.0);
					P.poly.push_back(l);
					P.arc.push_back(ra[i]);
					x0 = std::min(x0, l.x); x1 = std::max(x1, l.x); y0 = std::min(y0, l.y); y1 = std::max(y1, l.y);
				}
				if (P.poly.size() < 2) continue;
				P.strokeLen = len;
				P.brush = brush;
				P.R = brush;
				P.Rn = brush;
				P.D = D;
				P.u0 = x0 - brush; P.u1 = x1 + brush; P.v0 = y0 - brush; P.v1 = y1 + brush;
				P.off = off;
				P.brk = brk;
				const double segLen = ra[segs[sg].second] - ra[segs[sg].first];
				P.splits = p.checkEnable
					? int(std::round(p.checkAmount * ClampD(segLen / (8.0 * brush), 0.5, 3.0))) : 0;
				patches.push_back(std::move(P));
			}
		}
	}

	                                                                                      
	if (p.knotAmount > 0 && p.knotSize > 0.0 && total > 0.0) {
		const uint64_t ks = seed ^ 0x4b4e4f54ULL;            
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.knotAmount) * 30 && accepted < p.knotAmount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(ks, k, ch); };
			V pos;
			const int32_t pi = samplePoint(ks, k, 0, pos);
			const V n = s.polyN[size_t(pi)];
			if (std::abs(m::la::dot(n, s.polyG[size_t(pi)])) > 0.7) continue;
			if (mask && !mask->Keep(pos, int32_t(k), kStream + 2)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			Patch P;
			P.shape = Shape::Blobs;
			P.knot = true;
			P.id = 0x200000ULL + k;
			P.sc = sc;
			P.n = n;
			GrainFrame(P.n, s.polyG[size_t(pi)], P.t, P.b);
			P.parts.push_back(s.Part(pi));
			P.o = pos;
			P.R = p.knotSize * Lerp(0.7, 1.3, rnd(3)) * sc;
			P.Rn = P.R;
			P.D = p.knotDepth * Lerp(0.7, 1.3, rnd(4)) * sc;
			const double e = p.knotStretch * Lerp(0.85, 1.15, rnd(5));
			P.ca = 1.0; P.sa = 0.0; P.invE = 1.0 / e;
			P.blobs.push_back({0.0, 0.0, P.R});
			P.u0 = -P.R * e * 1.1; P.u1 = P.R * e * 1.1;
			P.v0 = -P.R * 1.1; P.v1 = P.R * 1.1;
			P.off = V(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			P.brk = false;
			P.splits = p.checkEnable ? p.knotChecks : 0;
			patches.push_back(std::move(P));
			++accepted;
		}
	}

	std::vector<Out> outs(patches.size());
	ProjectDamageParallelFor(0, int64_t(patches.size()), [&](int64_t i) {
		BuildPatch(cx, patches[size_t(i)], outs[size_t(i)]);
	});

	                                                                                        
	std::vector<std::vector<m::Manifold>> freeSplits(size_t(p.checkEnable ? p.checkGlobal : 0));
	if (!freeSplits.empty() && total > 0.0 && p.checkLengthMax > 0.0 && p.checkWidth > 0.0) {
		const uint64_t gs = seed ^ 0x53504c54ULL;            
		ProjectDamageParallelFor(0, int64_t(freeSplits.size()), [&](int64_t ki) {
			const uint64_t k = uint64_t(ki);
			auto rnd = [&](uint64_t ch) { return Random01(gs, k, ch); };
			V pos;
			const int32_t pi = samplePoint(gs, k, 0, pos);
			const V n = s.polyN[size_t(pi)];
			if (std::abs(m::la::dot(n, s.polyG[size_t(pi)])) > 0.7) return;               
			if (mask && !mask->Keep(pos, int32_t(k), kStream + 3)) return;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) return;
			V t, b;
			GrainFrame(n, s.polyG[size_t(pi)], t, b);
			const double len = Lerp(p.checkLengthMin, p.checkLengthMax, std::pow(rnd(10), 1.2)) * sc;
			const double w = p.checkWidth * Lerp(0.5, 1.2, rnd(11)) * sc;
			const double dep = p.checkDepth * Lerp(0.5, 1.2, rnd(12)) * sc;
			const double ang = (rnd(13) - 0.5) * 0.05;
			const double reach = 2.0 * (dep + w) + 1e-3 * s.diag;
			WalkSplit(cx, pos, n, t, b, -0.5 * len, 0.0, ang, len, w, dep, reach, 0x47000000ULL + k,
				std::vector<int32_t>{s.Part(pi)}, [](double, double) { return 0.0; }, freeSplits[size_t(k)]);
		});
	}

	for (Out& o : outs) {
		if (!o.cutter.IsEmpty()) cutters.push_back(std::move(o.cutter));
		for (auto& c : o.splits) cracks.push_back(std::move(c));
		for (auto& b : o.slivers) splinters.push_back(std::move(b));
	}
	for (auto& f : freeSplits)
		for (auto& c : f) cracks.push_back(std::move(c));
}
PROJECT_DAMAGE_NS_MACRO_END
