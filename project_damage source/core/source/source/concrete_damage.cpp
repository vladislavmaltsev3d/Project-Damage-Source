// создаём выбоины бетона и открываем внутреннюю арматуру
#include "rebar_lod.h"
// ProjectDamage concrete damage — GPL-3.0-or-later; see LICENSE.
//
// Recipe: concrete-damage-v1. A procedural appearance model of spalled,
// reinforced concrete:
//   1. patches: a spall patch is a local frame on the surface (origin, normal,
//      and a tangent frame aligned with the rebar grid) plus a 2D signed field
//      s(u,v) that is > 0 inside the outline. Automatic mode builds the field
//      from a cluster of overlapping, stretched blobs (area-weighted seeds,
//      optionally pulled toward sharp edges). Paint mode builds it from the
//      drawn strokes: a closed stroke is the outline itself (signed distance
//      to the polygon), an open stroke is swept with Paint Radius. Strokes are
//      split where the surface normal turns, so a stroke may wrap a column;
//   2. the cutter is a height-field slab over a grid in the patch frame. Each
//      grid column casts one ray onto the surface; the slab top floats above
//      the surface and the slab bottom is the surface minus the spall depth.
//      Outside the outline the bottom floats ABOVE the surface (over the 3x3
//      neighbourhood maximum), so the slab cuts exactly the s > 0 region and
//      always stays a valid closed solid (a graph over the grid). Columns
//      whose ray misses (past an object edge) are extrapolated, which lets a
//      patch at a corner bite into the side face like a real corner spall;
//   3. depth profile: stepped terraces (plaster skin, then deeper core layers
//      each inset with its own ragged rim), a pitted aggregate floor, a
//      dig-out around the bars, a wall-thickness cap, and - when Hole is on -
//      a jagged opening that runs through the whole wall;
//   4. cracks: V-groove strips walked outward from the rim (and some across
//      the floor), meandering, tapering, sometimes forking;
//   5. rebar: an object-space anchored grid of ribbed tubes at cover depth.
//      Bars are kept only where the spall gets close to them, then continue
//      Embed into the intact concrete, so the union welds them in. Exposed
//      runs bow outward a little; bars crossing a hole may be snapped with
//      their ends bent out.
// Candidates use SplitMix64 counter channels like fabric tears and surface
// deposits, keyed by seed (main seed and Concrete Seed), patch id and channel.
//
// Channel map (auto patch k, candidate c): 0 triangle (c*8), 1-2 barycentric,
// 3 size, 4 depth, 5 stretch, 6 stretch angle, 7 lobe count, 10+ lobes,
// 40-42 noise offset, 43 hole. Paint stroke k: 3 depth, 43 hole, 40-42
// noise. Cracks use (patch id, 200 + crack * 16 + sub-channel). Bars use
// (grid line index, family) hashes so they agree between patches.
#include "manifold/manifold.h"
#include "concrete_damage.h"
#include "surface_chart.h"
#include "damage_volume.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include "surface_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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

V Rotate(const V& v, const V& axis, double ang)
{
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + m::la::cross(axis, v) * s + axis * (m::la::dot(axis, v) * (1.0 - c));
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

                                                                      
                                                         
m::Manifold Tube(const std::vector<V>& raw, const std::vector<double>& rawR, int sides)
{
	std::vector<V> pts;
	std::vector<double> rad;
	for (size_t i = 0; i < raw.size(); ++i) {
		const double minSeg = 0.2 * rawR[i];
		if (pts.empty() || m::la::length(raw[i] - pts.back()) > minSeg) { pts.push_back(raw[i]); rad.push_back(rawR[i]); }
	}
	if (pts.size() < 2) return m::Manifold();
	const size_t n = pts.size();
	std::vector<V> tan(n);
	for (size_t i = 0; i < n; ++i) {
		if (i == 0) tan[i] = Unit(pts[1] - pts[0], V(0, 0, 1));
		else if (i + 1 == n) tan[i] = Unit(pts[i] - pts[i - 1], V(0, 0, 1));
		else tan[i] = Unit(Unit(pts[i] - pts[i - 1], V(0, 0, 1)) + Unit(pts[i + 1] - pts[i], V(0, 0, 1)), tan[i - 1]);
	}
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	V N = AnyPerp(tan[0]);
	for (size_t i = 0; i < n; ++i) {
		if (i > 0) N = Unit(N - tan[i] * m::la::dot(N, tan[i]), AnyPerp(tan[i]));
		const V Bv = m::la::cross(tan[i], N);
		double rr = rad[i];
		if (i > 0 && i + 1 < n) {
			const double ch = m::la::dot(Unit(pts[i] - pts[i - 1], tan[i]), tan[i]);
			rr /= std::max(0.6, ch);
		}
		for (int k = 0; k < sides; ++k) {
			const double a = 2.0 * m::kPi * k / sides;
			push(pts[i] + (N * std::cos(a) + Bv * std::sin(a)) * rr);
		}
	}
	const uint32_t c0 = uint32_t(n * sides), c1 = c0 + 1;
	push(pts[0]);
	push(pts[n - 1]);
	auto R = [&](size_t i, int k) { return uint32_t(i * sides + size_t((k % sides + sides) % sides)); };
	for (size_t i = 0; i + 1 < n; ++i)
		for (int k = 0; k < sides; ++k) {
			const uint32_t a = R(i, k), b = R(i + 1, k), c = R(i + 1, k + 1), d = R(i, k + 1);
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
		}
	for (int k = 0; k < sides; ++k) {
		mesh.triVerts.insert(mesh.triVerts.end(), {c0, R(0, k), R(0, k + 1)});
		mesh.triVerts.insert(mesh.triVerts.end(), {c1, R(n - 1, k + 1), R(n - 1, k)});
	}
	return Orient(mesh);
}

                                                                                  

struct Surf {
	SurfaceChartIndex chart;
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> polyA;
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
	                                                                          
	                                                                 
    // ищем поверхность рядом с этой выбоиной, а не первый объект перед ней
    bool RaySurface(const V& org, const V& normal, double maxDist, double reference,
                    double minFacing, double& distance, int32_t& polygon) const
    {
        const double epsilon = std::max(1e-7, diag * 1e-9);
        double walked = 0.0, best = 1e300;
        polygon = -1;
        while (walked < maxDist) {
            double hit = 0.0;
            int32_t face = -1;
            if (!Ray(org - normal * walked, -normal, maxDist - walked, hit, face)) break;
            const double travel = walked + hit;
            const double score = std::abs(travel - reference);
            if (m::la::dot(polyN[size_t(face)], normal) > minFacing && score < best) {
                best = score; distance = travel; polygon = face;
            }
            if (travel >= reference && polygon >= 0) break;
            walked = travel + epsilon;
        }
        return polygon >= 0;
    }

	bool NormalNear(const V& P, double snap, V& N) const
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
				if (score < best) { best = score; N = pn; ok = true; }
			}
		}
		return ok && best <= 1.5 * snap;
	}
};

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, bool useAvoid, Surf& s, bool organic)
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
	if (organic) s.chart.Build(v);
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.lo = lo; s.hi = hi;
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                           
                                   
struct EdgeGrid {
	std::vector<std::pair<V, V>> segs;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	double cell = 1.0;
	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	void Build(const Surf& s, double cellSize)
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
    std::shared_ptr<DamageObjectVolume> volume;
	Shape shape = Shape::Blobs;
	uint64_t id = 0;                       
	V o, n, t, b;                                                                 
	std::vector<Blob> blobs;                                  
	double ca = 1.0, sa = 0.0, invE = 1.0;
	std::vector<V> poly;                                                           
	std::vector<double> arc;                                                    
	double strokeLen = 0.0, coreA = -1e300, coreB = 1e300, brush = 0.0;
	double R = 1.0;                                         
	double Rn = 1.0;                                              
	double D = 1.0;                         
	bool hole = false;
	int cracks = 0;
	V off;                                   
	double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

                                                                            
                                                                             
double RawField(const Patch& P, double x, double y, double* arcOut = nullptr)
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
		if (arcOut) *arcOut = bestArc;
		const double endDist = std::min(bestArc, P.strokeLen - bestArc);
		const double rr = P.brush * (0.55 + 0.45 * Smooth(0.0, 2.0 * P.brush, endDist));
		return rr - best;
	}
	}
	return -1.0;
}

                                            
void Frame(const ConcreteDamageParams& p, const V& n, V& t, V& b)
{
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	V h = m::la::cross(up, n);
	if (m::la::length(h) < 0.3) {                                             
		const V x = std::abs(up.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
		h = x - n * m::la::dot(x, n);
	}
	t = Unit(h - n * m::la::dot(h, n), AnyPerp(n));
	t = Rotate(t, n, p.rebarAngle * m::kPi / 180.0);
	b = m::la::cross(n, t);
}

                                                                                      

struct Out {
	m::Manifold cutter;
	std::vector<m::Manifold> cracks, bars;
};

struct Grid {
	int nu = 0, nv = 0;
	double cu = 1.0, cv = 1.0;
	std::vector<double> h, d, s, thick, arc;
	std::vector<char> valid, holeF, owned;
	size_t I(int i, int j) const { return size_t(i) * size_t(nv + 1) + size_t(j); }
	                                                                       
	bool Sample(const Patch& P, double u, double v, double& hh, double& dd, double& th, bool& hole, bool& own) const
	{
		const double fu = (u - P.u0) / cu, fv = (v - P.v0) / cv;
		if (fu < 0.0 || fv < 0.0 || fu > nu || fv > nv) return false;
		const int i = std::min(nu - 1, int(fu)), j = std::min(nv - 1, int(fv));
		const double a = fu - i, c = fv - j;
		const size_t k00 = I(i, j), k10 = I(i + 1, j), k01 = I(i, j + 1), k11 = I(i + 1, j + 1);
		const bool ok = valid[k00] && valid[k10] && valid[k01] && valid[k11];
		auto bl = [&](const std::vector<double>& f) {
			return (f[k00] * (1 - a) + f[k10] * a) * (1 - c) + (f[k01] * (1 - a) + f[k11] * a) * c;
		};
		hh = bl(h); dd = bl(d); th = bl(thick);
		const size_t kn = (a < 0.5 ? (c < 0.5 ? k00 : k01) : (c < 0.5 ? k10 : k11));
		hole = holeF[kn] != 0;
		own = owned[kn] != 0;
		return ok;
	}
};

struct Ctx {
	const ConcreteDamageParams* p;
	const Surf* s;
	uint64_t seed;
	double barR, barSpX, barSpY, cover, embed;
	double offX, offY;                
};

// строим одну область повреждения и её внутренние детали
void BuildPatch(const Ctx& cx, const Patch& P, Out& out)
{
	const ConcreteDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const double ampOut = jag * 0.5 * P.Rn;
	const double featOut = 0.55 * P.Rn * std::max(0.05, p.outlineScale);
	const double pad = 0.6 * ampOut + 0.08 * P.R + 1e-3;
	const double u0 = P.u0 - pad, u1 = P.u1 + pad, v0 = P.v0 - pad, v1 = P.v1 + pad;
	const double extent = std::max(u1 - u0, v1 - v0);
	if (!(extent > 0.0)) return;

	Grid g;
	double cell = extent / (110.0 * p.detail);
	cell = std::max(cell, extent / 500.0);
	g.nu = std::max(4, int(std::ceil((u1 - u0) / cell)));
	g.nv = std::max(4, int(std::ceil((v1 - v0) / cell)));
	g.cu = (u1 - u0) / g.nu;
	g.cv = (v1 - v0) / g.nv;
	cell = std::max(g.cu, g.cv);
	Patch Q = P;                       
	Q.u0 = u0; Q.v0 = v0; Q.u1 = u1; Q.v1 = v1;
	const size_t N = size_t(g.nu + 1) * size_t(g.nv + 1);
	g.h.assign(N, 0.0); g.d.assign(N, 0.0); g.s.assign(N, -1.0); g.thick.assign(N, -1.0); g.arc.assign(N, 0.0);
	g.valid.assign(N, 0); g.holeF.assign(N, 0); g.owned.assign(N, 1);
	std::vector<char> forbid(N, 0);
	const auto chart = p.organic ? s.chart.Select(P.o,P.n,extent*1.5,s.polyN) : std::vector<char>();

	const V center = (s.lo + s.hi) * 0.5 - P.o;
	const V half = (s.hi - s.lo) * 0.5;
	const double offset = m::la::dot(center, P.n);
	const double span = std::abs(P.n.x) * half.x + std::abs(P.n.y) * half.y + std::abs(P.n.z) * half.z;
	const double rayPad = std::max(1e-3, s.diag * 1e-5);
	const double Hs = std::max(0.3 * extent + P.D + 1e-3, offset + span + rayPad);
	const double drop = std::max(0.6 * extent + 2.0 * P.D, span - offset + rayPad);
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double u = u0 + i * g.cu, v = v0 + j * g.cv;
			const V org = P.o + P.t * u + P.b * v + P.n * Hs;
			double dist = 0.0;
			int32_t poly = -1;
			if (s.RaySurface(org, P.n, Hs + drop, Hs, p.organic ? 0.3 : 0.05, dist, poly) && (!p.organic || chart[size_t(poly)])) {
				g.h[k] = Hs - dist;
				g.valid[k] = 1;
				forbid[k] = s.Avoided(poly) ? 1 : 0;
				const V S = P.o + P.t * u + P.b * v + P.n * g.h[k];
				double th = 0.0;
				int32_t tp = -1;
				if (s.Ray(S - P.n * (1e-5 * s.diag), -P.n, 2.0 * s.diag, th, tp)) g.thick[k] = th;
			}
		}
	                                                                    
	{
		// соседняя глубина не должна превращаться в длинную перемычку на углу
        ConnectSurfaceChart(g,u0,v0,cell,(u1-u0)/g.nu,(v1-v0)/g.nv);
        for (size_t k = 0; k < N; ++k) if (!g.valid[k]) forbid[k] = 0;
		std::vector<char> known(g.valid);
		bool any = false;
		for (char c : known) any |= c != 0;
		if (!any) return;
		for (int pass = 0; pass < g.nu + g.nv + 2; ++pass) {
			std::vector<char> nk(known);
			bool changed = false;
			for (int i = 0; i <= g.nu; ++i)
				for (int j = 0; j <= g.nv; ++j) {
					const size_t k = g.I(i, j);
					if (known[k]) continue;
					double sh = 0.0, st = 0.0;
					int c = 0, ct = 0;
					for (int di = -1; di <= 1; ++di)
						for (int dj = -1; dj <= 1; ++dj) {
							const int a = i + di, b = j + dj;
							if (a < 0 || b < 0 || a > g.nu || b > g.nv) continue;
							const size_t q = g.I(a, b);
							if (!known[q]) continue;
							sh += g.h[q]; ++c;
							if (g.thick[q] > 0.0) { st += g.thick[q]; ++ct; }
						}
					if (c == 0) continue;
					g.h[k] = sh / c;
					if (ct) g.thick[k] = st / ct;
					nk[k] = 1;
					changed = true;
				}
			known.swap(nk);
			if (!changed) break;
		}
	}
	                                                                 
	for (size_t k = 0; k < N; ++k) if (!(g.thick[k] > 0.0)) g.thick[k] = -1.0;

	                                   
    const auto objectField=SampleDamageObject(g,P,u0,v0,g.cu,g.cv);
	double sMax = -1e300;
	size_t kMax = 0;
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double u = u0 + i * g.cu, v = v0 + j * g.cv;
			const V W = P.o + P.t * u + P.b * v + P.n * g.h[k];
			double arc = 0.0;
			double sv = (P.volume?objectField.distance[k]:RawField(P, u, v, &arc)) + ampOut * Fbm(W / featOut + P.off, 5);
            if(p.profile==2) {
                const double angle=p.woodAngle*3.141592653589793/180.;
                const double across=u*std::cos(angle)+v*std::sin(angle);
                const double along=-u*std::sin(angle)+v*std::cos(angle);
                const double width=std::max(cell*3,p.woodWidth);
                                                                                   
                const double board=std::floor(across/width);
                const double grain=Fbm(V(across/(width*.10),along/(width*2.5),0)+P.off,3);
                const double end=Fbm(V(board*3.17,0,0)+P.off,2);
                sv+=p.woodSplinter*(grain*.7+end*.3);
            }
			g.arc[k] = arc;
			if (P.shape == Shape::Stroke) g.owned[k] = (arc >= P.coreA && arc <= P.coreB) ? 1 : 0;
			if (forbid[k] || (p.organic && !g.valid[k])) sv = std::min(sv, -cell);
			g.s[k] = sv;
			if (sv > sMax) { sMax = sv; kMax = k; }
		}
	const double WallClassic = 1.2 * cell + (p.profile==2 ? 0.0 : ClampD(p.wallSoft, 0.0, 1.0) * 0.3 * std::max(sMax, 0.0));
	if (sMax < 1.5 * WallClassic) return;                       
	                                                                       
	                                                                   
	const bool v2 = p.profile == 1;
	const double Wall = v2 ? std::max(WallClassic, ClampD(p.smoothWidth, 0.05, 1.0) * sMax) : WallClassic;

	                
	const int L = (v2 || p.profile==2) ? 1 : std::max(1, std::min(p.terraces, 4));
	const double skin = L == 1 ? 1.0 : ClampD(p.terraceSkin, 0.0, 1.0);
	const double inset = ClampD(p.terraceInset, 0.0, 0.5) * sMax;
	const double ampT = jag * 0.18 * sMax;
	                                                                         
	                                 
	const double barStack = (p.rebarBarsX && p.rebarBarsY) ? 4.9 * cx.barR : 2.0 * cx.barR;
	const double exposeDepth = p.rebarEnable ? cx.cover + barStack * ClampD(p.rebarExpose, 0.0, 1.0) : 0.0;
	const double agg = std::max(1e-3, p.aggregateSize);
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double margin = std::max(0.3 * P.D, 2.0 * cell);
	const double lift = 0.4 * margin;
	const double holeThr = (1.0 - ClampD(p.holeSize, 0.0, 1.0)) * sMax;
	const double ampH = ClampD(p.holeJagged, 0.0, 2.0) * 0.25 * sMax;
	                                                                          
	                                                                       
	                                                                      
	const double rimLip = std::max({0.02 * P.D, 1e-4 * extent, std::max(0.0,p.edgeWallDepth)*0.01*P.Rn});
	std::vector<char> outside(N, 0);
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double sv = g.s[k];
			if (sv <= 0.0) { outside[k] = 1; g.d[k] = 0.0; continue; }
			const double u = u0 + i * g.cu, v = v0 + j * g.cv;
			const V W = P.o + P.t * u + P.b * v + P.n * g.h[k];
			double d = 0.0, core = 0.0;
			for (int l = 0; l < L; ++l) {
				const double w = l == 0 ? skin : (1.0 - skin) / (L - 1);
				double sl = sv - l * inset;
				if (l > 0) sl += ampT * Fbm(W / (0.45 * P.Rn) + P.off + V(31.7 * l, 11.3 * l, -7.1 * l), 4);
				double st = Smooth(0.0, Wall, sl);
				if (v2) { const double x = ClampD(sl / Wall, 0.0, 1.0); st = x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
				d += P.D * w * st;
				if (l == L - 1) core = st;
			}
			if (exposeDepth > 0.0) d = std::max(d, core * exposeDepth);
			if (p.roughness > 0.0 && p.profile!=2) {
                V sample = W;
                if (p.surfaceDistortion > 0.0) {
                    const V q = W / (3.0 * agg) + P.off;
                    sample += V(Fbm(q + V(13.1, 2.7, 7.3), 3),
                                Fbm(q + V(4.2, 19.4, -3.1), 3),
                                Fbm(q + V(-8.7, 5.2, 23.6), 3)) * (agg * ClampD(p.surfaceDistortion, 0.0, 4.0));
                }
				const double big = pd_surface_noise::Fbm(sample / (0.35 * P.Rn) + P.off + V(5.5, 1.5, 9.5), p.surfaceDetail, 0.5);
				const double pit = 1.0 - 2.0 * std::abs(pd_surface_noise::Fbm(sample / agg + P.off + V(-3.3, 8.1, 2.2), p.surfaceDetail, 0.5));
                const double mix = ClampD(p.surfaceRoughness, 0.0, 1.0);
				const double r = ClampD(p.roughness, 0.0, 2.0) * 0.35 * P.D * ((1.0-mix) * big + mix * pit) * Smooth(0.0, 2.0 * Wall, sv) * ClampD(p.surfaceStrength, 0.0, 4.0);
				d = std::max(d + r, 0.3 * d);
			}
			if(p.profile==2) {
                                                                       
                                                                          
                const double wall=Smooth(0.0,cell*1.2,sv);
                d=std::max(P.D,exposeDepth)*wall;
                if(g.thick[k]>0 && P.D>=g.thick[k] && wall>.99) {
                    d=g.thick[k]+margin;g.holeF[k]=1;
                }
            }
            d = std::max(d, rimLip);
			if (g.thick[k] > 0.0 && p.profile!=2) d = std::min(d, cap * g.thick[k]);
			if (P.hole && p.holeEnable) {
				const double sh = sv - holeThr + ampH * Fbm(W / (0.3 * sMax) + P.off + V(-21.0, 4.0, 13.0), 4);
				if (sh > 0.0) {
					const double f = Smooth(0.0, 0.7 * Wall, sh);
					const double through = (g.thick[k] > 0.0 ? g.thick[k] : 3.0 * P.D + extent) + margin;
					d = Lerp(d, through, f);
					if (f > 0.5) g.holeF[k] = 1;
				}
			}
			if (d <= 1e-6) { outside[k] = 1; d = 0.0; }
            if(P.volume)d=std::min(objectField.depth[k],d*std::min(1.0,objectField.depth[k]/std::max(P.D,1e-7)));
			g.d[k] = d;
		}

	       
	                                                                           
	                                                                          
	                                                                         
	                                                                         
	                                                                         
	                                                                       
	                                                                         
	                                                                           
	                                                                       
	                                                                   
	                                                                          
	std::vector<V> top(N), bot(N);
	static const int kNb[6][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}};
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			double hmax = -1e300;
			for (int di = -1; di <= 1; ++di)
				for (int dj = -1; dj <= 1; ++dj) {
					const int a = i + di, b = j + dj;
					if (a < 0 || b < 0 || a > g.nu || b > g.nv) continue;
					hmax = std::max(hmax, g.h[g.I(a, b)]);
				}
			const double u = u0 + i * g.cu, v = v0 + j * g.cv;
			const V base = P.o + P.t * u + P.b * v;
			top[k] = base + P.n * (hmax + margin);
			double up = hmax + lift - g.h[k];                                
			if (outside[k] && g.s[k] < 0.0) {
				double want = 0.0;
				int cnt = 0;
				for (const auto& nb : kNb) {
					const int a = i + nb[0], b = j + nb[1];
					if (a < 0 || b < 0 || a > g.nu || b > g.nv) continue;
					const size_t q = g.I(a, b);
					if (outside[q] || !(g.s[q] > 0.0)) continue;
					want += g.d[q] * (-g.s[k]) / std::max(g.s[q], 1e-9 * extent);
					++cnt;
				}
				if (cnt) {
					double bulge = 0.0;
					if (i > 0 && i < g.nu) bulge = std::max(bulge, 0.5 * (g.h[g.I(i - 1, j)] + g.h[g.I(i + 1, j)]) - g.h[k]);
					if (j > 0 && j < g.nv) bulge = std::max(bulge, 0.5 * (g.h[g.I(i, j - 1)] + g.h[g.I(i, j + 1)]) - g.h[k]);
					up = std::min(up, std::max(want / cnt, 1e-5 * extent) + std::max(bulge, 0.0));
				}
			}
			bot[k] = base + P.n * (outside[k] ? g.h[k] + up : g.h[k] - g.d[k]);
		}
	out.cutter = SlabSolid(top, bot, g.nu, g.nv);

	auto rnd = [&](uint64_t ch) { return Random01(cx.seed ^ 0x435241434bULL, P.id, ch); };

	                                                                                 
	if (p.crackEnable && P.cracks > 0 && p.crackLengthMax > 0.0 && p.crackWidth > 0.0) {
		const double uc = u0 + double(kMax / size_t(g.nv + 1)) * g.cu;
		const double vc = v0 + double(kMax % size_t(g.nv + 1)) * g.cv;
		auto sAt = [&](double u, double v) {
			const double fu = (u - u0) / g.cu, fv = (v - v0) / g.cv;
			if (fu < 0.0 || fv < 0.0 || fu > g.nu || fv > g.nv) return -1.0;
			const int i = std::min(g.nu - 1, int(fu)), j = std::min(g.nv - 1, int(fv));
			const double a = fu - i, c = fv - j;
			return (g.s[g.I(i, j)] * (1 - a) + g.s[g.I(i + 1, j)] * a) * (1 - c)
				+ (g.s[g.I(i, j + 1)] * (1 - a) + g.s[g.I(i + 1, j + 1)] * a) * c;
		};
		auto floorAt = [&](double u, double v) {                                                 
			double hh, dd, th; bool hl, own;
			if (!g.Sample(Q, u, v, hh, dd, th, hl, own)) return 0.0;
			return hl ? -1.0 : dd;
		};
		const double reach = Hs;
		struct Seed { double x, y, ang, len, w, dep; uint64_t key; };
		std::vector<Seed> todo;
		for (int c = 0; c < P.cracks; ++c) {
			const uint64_t ch = 200 + uint64_t(c) * 16;
			double ang = 2.0 * m::kPi * (c + 0.8 * rnd(ch)) / P.cracks;
			const bool interior = rnd(ch + 1) < ClampD(p.crackInterior, 0.0, 1.0);
			double x = uc, y = vc;
			if (!interior) {                      
				const V dir(std::cos(ang), std::sin(ang), 0.0);
				for (int st = 0; st < 4 * (g.nu + g.nv); ++st) {
					const double nx = x + dir.x * cell, ny = y + dir.y * cell;
					if (sAt(nx, ny) <= 0.0) break;
					x = nx; y = ny;
				}
				x -= std::cos(ang) * Wall; y -= std::sin(ang) * Wall;
			} else {
				x += (rnd(ch + 2) - 0.5) * 0.6 * sMax;
				y += (rnd(ch + 3) - 0.5) * 0.6 * sMax;
			}
			ang += (rnd(ch + 4) - 0.5) * 0.7;
			const double len = Lerp(p.crackLengthMin, p.crackLengthMax, std::pow(rnd(ch + 5), 1.4));
			todo.push_back({x, y, ang, len, p.crackWidth * Lerp(0.6, 1.3, rnd(ch + 6)), p.crackDepth * Lerp(0.6, 1.3, rnd(ch + 7)), ch});
		}
		for (size_t ti = 0; ti < todo.size() && ti < 64; ++ti) {
			const Seed sd = todo[ti];
			const double step = std::max(1.5 * sd.w, sd.len / 48.0);
			const int ns = std::max(2, int(std::ceil(sd.len / step)));
			const V noff(rnd(sd.key + 8) * 1000.0, rnd(sd.key + 9) * 1000.0, 0.0);
			std::vector<V> topv, botv;
			double ang = sd.ang, x = sd.x, y = sd.y;
			int rows = 0;
			for (int i = 0; i <= ns; ++i) {
				const double f = double(i) / ns;
				if (i > 0) {
					ang += ClampD(p.crackMeander, 0.0, 2.0) * 2.2 * Fbm(V(f * sd.len / (0.25 * sd.len + 1e-6), 0.5, 0.0) + noff, 3) * (step / (0.2 * sd.len + 1e-6));
					x += std::cos(ang) * step;
					y += std::sin(ang) * step;
				}
				const V org = P.o + P.t * x + P.b * y + P.n * reach;
				double dist = 0.0;
				int32_t poly = -1;
				if (!s.Ray(org, -P.n, reach + drop, dist, poly) || m::la::dot(s.polyN[size_t(poly)], P.n) < 0.3 || s.Avoided(poly)) break;
				const V S = org - P.n * dist;
				const V dirW = Unit(P.t * std::cos(ang) + P.b * std::sin(ang), P.t);
				const V side = Unit(m::la::cross(P.n, dirW), P.b);
				const double taper = 1.0 - 0.85 * std::pow(f, 1.5);
				const double hw = 0.5 * sd.w * taper + 1e-4;
				double dep = sd.dep * (1.0 - 0.75 * std::pow(f, 1.5));
				const double fl = floorAt(x, y);
				if (fl < 0.0) dep = 0.05 * sd.dep;                                 
				else dep += fl;
				const double mc = 0.25 * sd.dep + 2.0 * hw;
				topv.push_back(S - side * hw + P.n * mc); topv.push_back(S + P.n * mc); topv.push_back(S + side * hw + P.n * mc);
				botv.push_back(S - side * hw + P.n * (0.3 * mc)); botv.push_back(S - P.n * dep); botv.push_back(S + side * hw + P.n * (0.3 * mc));
				++rows;
				if (i > 0 && i < ns && ti < size_t(P.cracks) && sd.len > 4.0 * step
					&& std::abs(f - Lerp(0.3, 0.7, rnd(sd.key + 10))) < 0.5 / ns
					&& rnd(sd.key + 11) < ClampD(p.crackBranch, 0.0, 1.0)) {
					const double sgn = rnd(sd.key + 12) < 0.5 ? -1.0 : 1.0;
					todo.push_back({x, y, ang + sgn * Lerp(0.45, 0.9, rnd(sd.key + 13)), sd.len * (1.0 - f) * Lerp(0.4, 0.8, rnd(sd.key + 14)),
						sd.w * taper * 0.8, sd.dep * 0.8, sd.key + 1000});
				}
			}
			if (rows >= 3) {
				m::Manifold c = SlabSolid(topv, botv, rows - 1, 2);
				if (!c.IsEmpty()) out.cracks.push_back(std::move(c));
			}
		}
	}

	                                                                                        
	if (p.generatedGeometry && p.rebarEnable && (p.rebarBarsX || p.rebarBarsY) && cx.barR > 0.0) {
		const double r = cx.barR;
		const int sides = RebarLodSides(std::max(4, std::min(p.rebarSides, 16)),p.rebarResolution);
		const double ribs = ClampD(p.rebarRibs, 0.0, 1.0);
		const double pitch = 1.4 * r;
		for (int fam = 0; fam < 2; ++fam) {
			                                                                        
			if (fam == 0 && !p.rebarBarsX) continue;
			if (fam == 1 && !p.rebarBarsY) continue;
			const bool outer = (fam == 0) == (p.rebarOuter == 0);
			const double layer = cx.cover + r + (outer ? 0.0 : 1.9 * r);
			const double sp = fam == 0 ? cx.barSpY : cx.barSpX;
			const V across = fam == 0 ? P.b : P.t;
			const double base = m::la::dot(P.o, across);
			const double phase = fam == 0 ? cx.offY : cx.offX;
			const double lo = (fam == 0 ? v0 : u0), hi = (fam == 0 ? v1 : u1);
			const double alo = (fam == 0 ? u0 : v0), ahi = (fam == 0 ? u1 : v1);
			const int64_t k0 = int64_t(std::floor((base + lo - phase) / sp)) - 1;
			const int64_t k1 = int64_t(std::ceil((base + hi - phase) / sp)) + 1;
			for (int64_t kk = k0; kk <= k1; ++kk) {
				const uint64_t key = (uint64_t(kk) * 2 + uint64_t(fam)) ^ 0x52454241ULL;            
				if (Random01(cx.seed, key, 0) >= ClampD(p.rebarAmount, 0.0, 1.0)) continue;
				const double jit = (Random01(cx.seed, key, 1) - 0.5) * 0.3 * sp * ClampD(p.rebarJitter, 0.0, 1.0);
				const double c = kk * sp + phase + jit - base;
				if (c < lo || c > hi) continue;
				const double ds = ribs > 0.0 ? 0.25 * pitch : std::max(r, 0.5 * cell);
				double dsUse = ds;
				if ((ahi - alo) / dsUse > 3000.0) dsUse = (ahi - alo) / 3000.0;
				const int ns = int(std::floor((ahi - alo) / dsUse));
				if (ns < 2) continue;
				struct Smp { V ctr; bool ok, exp, hole, own; double d; };
				std::vector<Smp> sm(size_t(ns + 1));
				const V woff(Random01(cx.seed, key, 2) * 1000.0, Random01(cx.seed, key, 3) * 1000.0, 0.0);
				for (int i = 0; i <= ns; ++i) {
					const double a = alo + i * dsUse;
					const double u = fam == 0 ? a : c, v = fam == 0 ? c : a;
					double hh, dd, th; bool hl, own;
					Smp& q = sm[size_t(i)];
					q.ok = g.Sample(Q, u, v, hh, dd, th, hl, own);
					q.ok = q.ok && (th < 0.0 || th > layer + 2.0 * r);
					q.own = own;
					q.hole = hl;
					q.d = dd;
					q.exp = q.ok && dd > layer - r;                                        
					const V W = P.o + P.t * u + P.b * v;
					const double wa = ClampD(p.rebarWobble, 0.0, 4.0) * r;
					const double w1 = wa * Fbm(V(a / (0.7 * sp), 0.3, 0.0) + woff, 2);
					const double w2 = wa * Fbm(V(a / (0.7 * sp), 5.3, 0.0) + woff, 2);
					q.ctr = W + P.n * (hh - layer + w1) + across * w2;
				}
				                                                                 
				const int ext = int(std::ceil(cx.embed / dsUse));
				std::vector<char> keep(size_t(ns + 1), 0);
				for (int i = 0; i <= ns; ++i) {
					if (!sm[size_t(i)].ok || !(sm[size_t(i)].d > 0.5 * (layer - r))) continue;
					for (int e = std::max(0, i - ext); e <= std::min(ns, i + ext); ++e) keep[size_t(e)] = 1;
				}
				for (int i = 0; i <= ns; ++i) if (!sm[size_t(i)].ok || !sm[size_t(i)].own) keep[size_t(i)] = 0;
				int i = 0;
				while (i <= ns) {
					if (!keep[size_t(i)]) { ++i; continue; }
					int j = i;
					while (j + 1 <= ns && keep[size_t(j + 1)]) ++j;
					                    
					int ex = 0;
					for (int q = i; q <= j; ++q) ex += sm[size_t(q)].exp ? 1 : 0;
					if (ex * dsUse >= 2.0 * r && j - i >= 2) {
						std::vector<V> ctr(size_t(j - i + 1));
						std::vector<double> rad(size_t(j - i + 1), r);
						for (int q = i; q <= j; ++q) {
							ctr[size_t(q - i)] = sm[size_t(q)].ctr;
							if (ribs > 0.0 && (q % 4) == 0) rad[size_t(q - i)] = r * (1.0 + ribs);
						}
						                                 
						int h0 = -1, h1 = -1;
						for (int q = i; q <= j; ++q)
							if (sm[size_t(q)].hole) { if (h0 < 0) h0 = q; h1 = q; }
							else if (h0 >= 0) break;
						const uint64_t runKey = key * 131 + uint64_t(i);
						const bool snap = h0 >= 0 && h1 - h0 >= 4
							&& Random01(cx.seed ^ P.id, runKey, 4) < ClampD(p.rebarBroken, 0.0, 1.0);
						const double bend = ClampD(p.rebarBend, 0.0, 3.0);
						if (!snap) {
							                            
							int e0 = -1, e1 = -1;
							for (int q = i; q <= j; ++q) if (sm[size_t(q)].exp) { if (e0 < 0) e0 = q; e1 = q; }
							if (e1 > e0)
								for (int q = e0; q <= e1; ++q) {
									const double f = double(q - e0) / (e1 - e0);
									ctr[size_t(q - i)] = ctr[size_t(q - i)] + P.n * (bend * 0.6 * r * std::sin(m::kPi * f) * (sm[size_t(q)].hole ? 2.5 : 1.0));
								}
							if(p.rebarResolution<1.0) {
                                const auto ids=RebarLodIndices(ctr,r,p.rebarResolution);
                                RebarLodSelect(ctr,ids);RebarLodSelect(rad,ids);
                                for(auto& rr:rad)rr=std::min(rr,r)+p.rebarResolution*std::max(0.0,rr-r);
                            }
                            m::Manifold bar = Tube(ctr, rad, sides);
							if (!bar.IsEmpty()) out.bars.push_back(std::move(bar));
						} else {
							const int brk = h0 + int(std::floor((h1 - h0) * Lerp(0.3, 0.7, Random01(cx.seed ^ P.id, runKey, 5))));
							const int gap = std::max(1, int(std::ceil(0.6 * r / dsUse)));
							const double holeHalf = std::max(dsUse, 0.5 * (h1 - h0) * dsUse);
							const double side = Random01(cx.seed ^ P.id, runKey, 6) - 0.5;
							for (int piece = 0; piece < 2; ++piece) {
								const int a = piece == 0 ? i : brk + gap, b = piece == 0 ? brk - gap : j;
								if (b - a < 2) continue;
								std::vector<V> pc;
								std::vector<double> pr;
								for (int q = a; q <= b; ++q) {
									V c3 = ctr[size_t(q - i)];
									const int from = piece == 0 ? h0 : h1;
									const double dIn = piece == 0 ? double(q - from) * dsUse : double(from - q) * dsUse;
									if (dIn > 0.0) {
										const double f = dIn / holeHalf;
										c3 = c3 + P.n * (bend * 0.5 * holeHalf * f * f) + across * (side * bend * 0.3 * holeHalf * f * f);
									}
									pc.push_back(c3);
									pr.push_back(rad[size_t(q - i)]);
								}
								                      
								const size_t tip = piece == 0 ? pr.size() - 1 : 0;
								pr[tip] *= 0.7;
								if (pr.size() > 2) pr[piece == 0 ? tip - 1 : 1] *= 0.85;
								if(p.rebarResolution<1.0) {
                                    const auto ids=RebarLodIndices(pc,r,p.rebarResolution);
                                    RebarLodSelect(pc,ids);RebarLodSelect(pr,ids);
                                    for(auto& rr:pr)rr=std::min(rr,r)+p.rebarResolution*std::max(0.0,rr-r);
                                }
                                m::Manifold bar = Tube(pc, pr, sides);
								if (!bar.IsEmpty()) out.bars.push_back(std::move(bar));
							}
						}
					}
					i = j + 1;
				}
			}
		}
	}
}

}               

void PlaceConcreteDamage(const CoreInput& in, const ConcreteDamageParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& rebar)
{
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	ConcreteDamageParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.amount = std::max(0, std::min(p.amount, 500));
	p.lobes = std::max(1, std::min(p.lobes, 8));
	p.crackAmount = std::max(0, std::min(p.crackAmount, 32));
	p.stretch = ClampD(p.stretch, 1.0, 6.0);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	if (p.depthMax < p.depthMin) std::swap(p.depthMin, p.depthMax);
	if (p.crackLengthMax < p.crackLengthMin) std::swap(p.crackLengthMin, p.crackLengthMax);
	p.sizeMin = std::max(1e-4, p.sizeMin);
	p.depthMin = std::max(1e-4, p.depthMin);
	p.paintRadius = std::max(1e-4, p.paintRadius);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	const bool paint = p.mode == 1;
    const bool proximity = p.mode == 2;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && !proximity && p.amount <= 0) return;

	Surf s;
	BuildSurf(in, p.avoid, s, p.organic);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x434f4e4352455445ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4344;                             

	Ctx cx;
	cx.p = &p;
	cx.s = &s;
	cx.seed = seed;
	const double rs = std::max(1e-4, p.rebarScale);
	const double dens = std::max(0.05, p.rebarDensity);
	cx.barR = std::max(0.0, p.rebarRadius) * rs;
	cx.barSpX = std::max(4.0 * cx.barR + 1e-4, p.rebarSpacingX * rs / dens);
	cx.barSpY = std::max(4.0 * cx.barR + 1e-4, p.rebarSpacingY * rs / dens);
	cx.cover = std::max(0.0, p.rebarCover) * rs;
	cx.embed = std::max(0.0, p.rebarEmbed) * rs;
	cx.offX = Random01(seed, 0x4f4658ULL, 0) * cx.barSpX;
	cx.offY = Random01(seed, 0x4f4659ULL, 0) * cx.barSpY;

	std::vector<Patch> patches;
    std::vector<std::shared_ptr<DamageObjectVolume>> objectVolumes;

    if (proximity) {
        for(size_t i=0;i<p.proximity.size();++i) {
            const auto& point=p.proximity[i];
            if(!point.mesh_points.empty()) {
                auto volume=std::make_shared<DamageObjectVolume>(point);objectVolumes.push_back(volume);
                AppendDamageObjectPatches(s,volume,volume->extent*2.0,patches,[&](const V& n,V& t,V& b){Frame(p,n,t,b);});
                continue;
            }
            if(!(point.radius>0.0))continue;
            Patch patch;patch.id=i+1;patch.shape=Shape::Blobs;
            patch.o=V(point.x,point.y,point.z);patch.n=Unit(V(point.nx,point.ny,point.nz),V(0,0,1));
            Frame(p,patch.n,patch.t,patch.b);patch.R=point.radius;patch.Rn=point.radius;
            patch.D=Lerp(p.depthMin,p.depthMax,Random01(seed,i+1,3));
            patch.u0=patch.v0=-point.radius;patch.u1=patch.v1=point.radius;
            patch.blobs.push_back({0.0,0.0,point.radius});
            patch.off=V(Random01(seed,i+1,40)*1000.,Random01(seed,i+1,41)*1000.,Random01(seed,i+1,42)*1000.);
            patch.hole=p.holeEnable && Random01(seed,i+1,43)<ClampD(p.holeChance,0.0,1.0);
            patch.cracks=p.crackEnable?p.crackAmount:0;
            patches.push_back(std::move(patch));
        }
    } else if (!paint) {
		                                                                             
		std::vector<double> cdf(size_t(v.npolys));
		double total = 0.0;
		for (int32_t i = 0; i < v.npolys; ++i) {
			total += s.Avoided(i) ? 0.0 : s.polyA[size_t(i)];
			cdf[size_t(i)] = total;
		}
		if (total <= 0.0) return;
		const double bias = ClampD(p.edgeBias, 0.0, 1.0);
		EdgeGrid eg;
		if (bias > 0.0) eg.Build(s, p.sizeMax);
		const int cands = 1 + int(std::round(bias * 6.0));
		const uint64_t as = seed ^ 0x4155544fULL;            
		std::vector<std::pair<V, double>> placed;
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.amount) * 30 && accepted < p.amount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(as, k, ch); };
			double bestD = 1e300;
			V pos(0.0);
			int32_t pi = -1;
			for (int c = 0; c < cands; ++c) {
				const double pick = Random01(as, k, uint64_t(c) * 8) * total;
				int32_t ti = int32_t(std::upper_bound(cdf.begin(), cdf.end(), pick) - cdf.begin());
				if (ti >= v.npolys) ti = v.npolys - 1;
				const ProjectDamagePoly& q = v.polys[ti];
				double r1 = Random01(as, k, uint64_t(c) * 8 + 1), r2 = Random01(as, k, uint64_t(c) * 8 + 2);
				V A = v.pts[q.a], B = v.pts[q.b], C = v.pts[q.c];
				if (q.c != q.d && Random01(as, k, uint64_t(c) * 8 + 3) < 0.5) { B = v.pts[q.c]; C = v.pts[q.d]; }
				if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
				const V cand = A * (1.0 - r1 - r2) + B * r1 + C * r2;
				const double dEdge = cands > 1 ? eg.Nearest(cand, 2) : 0.0;
				if (pi < 0 || dEdge < bestD) { bestD = dEdge; pos = cand; pi = ti; }
			}
			if (pi < 0) continue;
			if (mask && !mask->Keep(pos, int32_t(k), kStream)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			Patch P;
			P.shape = Shape::Blobs;
			P.id = k;
			P.n = s.polyN[size_t(pi)];
			Frame(p, P.n, P.t, P.b);
			P.o = pos;
			P.R = Lerp(p.sizeMin, p.sizeMax, rnd(3)) * sc;
			P.Rn = P.R;
			P.D = Lerp(p.depthMin, p.depthMax, rnd(4)) * sc;
			const double e = Lerp(1.0, p.stretch, rnd(5));
			const double ang = rnd(6) * m::kPi;
			P.ca = std::cos(ang); P.sa = std::sin(ang); P.invE = 1.0 / e;
			const int nb = 1 + int(std::floor(rnd(7) * p.lobes));
			P.blobs.push_back({0.0, 0.0, 0.55 * P.R});
			for (int b = 1; b < std::min(nb, p.lobes); ++b) {
				const double a = rnd(10 + uint64_t(b) * 3) * 2.0 * m::kPi;
				const double dist = Lerp(0.45, 0.85, rnd(11 + uint64_t(b) * 3)) * P.R;
				P.blobs.push_back({std::cos(a) * dist, std::sin(a) * dist, Lerp(0.3, 0.55, rnd(12 + uint64_t(b) * 3)) * P.R});
			}
			double ext = 0.0;
			for (const Blob& b : P.blobs) ext = std::max(ext, std::sqrt(b.x * b.x + b.y * b.y) + b.r);
			ext *= e;
			P.u0 = P.v0 = -ext;
			P.u1 = P.v1 = ext;
			P.off = V(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			P.hole = p.holeEnable && rnd(43) < ClampD(p.holeChance, 0.0, 1.0);
			P.cracks = p.crackEnable ? int(std::round(p.crackAmount * Lerp(0.6, 1.4, rnd(44)))) : 0;
			bool clash = false;
			for (const auto& q : placed)
				if (m::la::length(q.first - pos) < 0.6 * (q.second + P.R)) { clash = true; break; }
			if (clash) continue;
			placed.push_back({pos, P.R});
			patches.push_back(std::move(P));
			++accepted;
		}
	} else {
		                                                                             
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
			const bool hole = p.holeEnable && rnd(43) < ClampD(p.holeChance, 0.0, 1.0);
			const V off(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			const double brush = p.paintRadius * sc;

			if (closed) {
				if (gapEnd < 1e-6 * len + 1e-9) pts.pop_back();
				V nsum(0.0), cen(0.0);
				int hits = 0;
				const size_t stride = std::max<size_t>(1, pts.size() / 48);
				for (size_t i = 0; i < pts.size(); i += stride) {
					V N;
					if (s.NormalNear(pts[i], p.paintSnap, N)) { nsum = nsum + N; ++hits; }
				}
				for (const V& q : pts) cen = cen + q;
				cen = cen / double(pts.size());
				if (hits > 0 && m::la::length(nsum) / hits > 0.5) {
					Patch P;
					P.shape = Shape::Polygon;
					P.id = 0x100000ULL + si * 64;
					P.n = Unit(nsum, V(0, 0, 1));
					Frame(p, P.n, P.t, P.b);
					P.o = cen;
					double area = 0.0, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
					for (const V& q : pts) {
						const V d = q - cen;
						const V l(m::la::dot(d, P.t), m::la::dot(d, P.b), 0.0);
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
					P.hole = hole;
					P.cracks = p.crackEnable ? p.crackAmount : 0;
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
			std::vector<V> rn(rp.size(), V(0.0));
			std::vector<char> rok(rp.size(), 0);
			for (size_t i = 0; i < rp.size(); ++i) {
				V N;
				if (s.NormalNear(rp[i], p.paintSnap, N)) { rn[i] = N; rok[i] = 1; }
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
				const double coreA = sg == 0 ? -1e300 : ra[segs[sg].first];
				const double coreB = sg + 1 == segs.size() ? 1e300 : ra[segs[sg].second];
				const double fa = std::max(0.0, ra[segs[sg].first] - 2.0 * brush);
				const double fb = std::min(len, ra[segs[sg].second] + 2.0 * brush);
				V nsum(0.0);
				for (size_t i = segs[sg].first; i <= segs[sg].second; ++i) if (rok[i]) nsum = nsum + rn[i];
				if (m::la::length(nsum) < 1e-9) continue;
				Patch P;
				P.shape = Shape::Stroke;
				P.id = 0x100000ULL + si * 64 + sg + 1;
				P.n = Unit(nsum, V(0, 0, 1));
				Frame(p, P.n, P.t, P.b);
				P.o = rp[(segs[sg].first + segs[sg].second) / 2];
				double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
				for (size_t i = 0; i < rp.size(); ++i) {
					if (ra[i] < fa - 1e-9 || ra[i] > fb + 1e-9) continue;
					const V d = rp[i] - P.o;
					const V l(m::la::dot(d, P.t), m::la::dot(d, P.b), 0.0);
					P.poly.push_back(l);
					P.arc.push_back(ra[i]);
					x0 = std::min(x0, l.x); x1 = std::max(x1, l.x); y0 = std::min(y0, l.y); y1 = std::max(y1, l.y);
				}
				if (P.poly.size() < 2) continue;
				P.strokeLen = len;
				P.coreA = coreA; P.coreB = coreB;
				P.brush = brush;
				P.R = brush;
				P.Rn = brush;
				P.D = D;
				P.u0 = x0 - brush; P.u1 = x1 + brush; P.v0 = y0 - brush; P.v1 = y1 + brush;
				P.off = off;
				P.hole = hole;
				const double segLen = ra[segs[sg].second] - ra[segs[sg].first];
				P.cracks = p.crackEnable
					? int(std::round(p.crackAmount * ClampD(segLen / (8.0 * brush), 0.5, 3.0))) : 0;
				patches.push_back(std::move(P));
			}
		}
	}
	if (patches.empty() && objectVolumes.empty()) return;

	std::vector<Out> outs(patches.size());
	ProjectDamageParallelFor(0, int64_t(patches.size()), [&](int64_t i) {
        const auto& patch=patches[size_t(i)];
        if(patch.volume) {
            auto settings=p;settings.thicknessCap=0.;settings.crackEnable=false;settings.holeEnable=false;
            auto local=cx;local.p=&settings;BuildPatch(local,patch,outs[size_t(i)]);
            auto& result=outs[size_t(i)];result.cutter=result.cutter^patch.volume->solid;result.cracks.clear();
            FilterDamageObjectGeometry(result.bars,*patch.volume);
        } else BuildPatch(cx,patch,outs[size_t(i)]);
	});
    for(const auto& volume:objectVolumes)if(std::none_of(patches.begin(),patches.end(),[&](const auto& patch){return patch.volume==volume;}))cutters.push_back(volume->solid.AsOriginal());
	for (Out& o : outs) {
		if (!o.cutter.IsEmpty()) cutters.push_back(std::move(o.cutter));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
		for (auto& b : o.bars) rebar.push_back(std::move(b));
	}
}
PROJECT_DAMAGE_NS_MACRO_END
