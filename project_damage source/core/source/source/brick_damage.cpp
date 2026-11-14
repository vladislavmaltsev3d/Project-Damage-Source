// вскрываем стену и размещаем кирпичи за её наружным слоем
// ProjectDamage brick damage — GPL-3.0-or-later; see LICENSE.
//
// Recipe: brick-damage-v1. A procedural appearance model of render (stucco,
// plaster) broken off a brick wall, and of the brickwork behind it broken
// brick by brick:
//   1. patches: the same patch system as Concrete / Lath Damage. A patch is a
//      local frame on the surface (origin, normal, and a tangent frame aligned
//      with the courses) plus a 2D signed field s(u,v) that is > 0 inside the
//      outline. Automatic mode builds the field from a cluster of overlapping,
//      stretched blobs (area-weighted seeds, optionally pulled toward sharp
//      edges). Paint mode builds it from the drawn strokes: a closed stroke is
//      the outline itself, an open stroke is swept with Paint Radius, and
//      strokes are split where the surface normal turns;
//   2. the cutter is a height-field slab over a grid in the patch frame (one
//      ray per grid column), floating above the surface outside the outline,
//      so it carves exactly the s > 0 region out of the source mesh and is
//      always a valid closed solid. The grid is the base resolution plus a
//      pair of lines hugging every brick edge, so joints, knocked-out bricks
//      and loose bricks get crisp, near-vertical walls at any Detail;
//   3. depth profile: stepped render coats down to the brick face. Below that
//      the bricks come from an object-space brick lattice (running, stack,
//      English, Flemish or header bond; leaves behind the front one have
//      their own joints). Each brick is one decision: a leaf-w brick is
//      knocked out when the field at its centre is above a threshold that
//      steps inward with w (plus per-brick jitter), so the break follows the
//      bond in steps, deeper leaves breaking back further inside. Joints fall
//      out with a missing neighbour. Bricks next to a gap may be snapped with
//      a rough fracture line; front bricks may be loose (pushed in and tilted,
//      or pulled out: the slot is cut and a tilted box is added back). Brick
//      faces get jitter, tilt, pitting and chipped edges, joints are recessed
//      and eroded, the back leaves are partly covered in collar-joint mortar,
//      and scraps of render can stay stuck on the bricks. Hole breaks through
//      the wall, also brick-stepped;
//   4. cracks: V-groove strips walked outward from the rim, meandering,
//      tapering, sometimes forking (same as Concrete Damage).
//   5. materials (splitMaterials, BrickDamageOut overload): every grid node
//      records which part its floor belongs to - snapped-brick fracture
//      (breaks), mortar joint (seams), back leaf / collar mortar (behind) or
//      render / front brick / hole - and the slab is built as one closed
//      solid per part over the cells of that part (a cell takes the part of
//      its deepest corner). The parts tile the legacy cutter exactly.
//   6. sharp edge (sharpEdge, default): one render coat at full depth right
//      up to the outline, and every slab is clipped to the exact outline by
//      marching squares on the field (the zero crossing on each cell edge),
//      so the render drops in one vertical wall instead of a soft, terraced
//      slope. With materials, a band a hair wide just inside the outline is
//      its own solid (render): its outer wall is the render edge. The band
//      and the brick parts share their walls vertex for vertex.
// Candidates use SplitMix64 counter channels like fabric tears and concrete
// damage, keyed by seed (main seed and Brick Seed), patch id and channel.
//
// Channel map (auto patch k, candidate c): 0 triangle (c*8), 1-2 barycentric,
// 3 size, 4 depth, 5 stretch, 6 stretch angle, 7 lobe count, 10+ lobes,
// 40-42 noise offset, 43 hole, 44 crack count. Paint stroke k: 3 depth,
// 43 hole, 40-42 noise. Cracks use (patch id, 200 + crack * 16 + sub-channel).
// Style Low-poly (BuildPatchLowPoly) swaps steps 2-4 for Building
// Destruction's masonry technique: 2D CrossSection unions in the patch plane
// (render outline, merged runs of knocked-out bricks per course and leaf,
// joints, pockets for pushed-in bricks, the hole), each swept along the normal
// once, with plain box bricks and stair-step cracks along the joints. Same
// lattice and the same per-brick draws, so the two styles agree brick for brick.
//
// Bricks use (leaf, course, index) keys: 1 break jitter, 2 missing, 3-5 face,
// 6-13 snapped, 20-24 loose, 30 hole jitter, 40-42 pulled-out pose.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "brick_damage.h"
#include <set>
#include "surface_chart.h"
#include "damage_volume.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
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

                                                                              
                                                                            
                                                                             
                                                                           
                                                                
m::Manifold MaskedSlabSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw,
	const std::vector<char>& use)
{
	auto U = [&](int i, int j) {
		return i >= 0 && j >= 0 && i < ns && j < nw && use[size_t(i) * size_t(nw) + size_t(j)] != 0;
	};
	m::MeshGL mesh;
	mesh.numProp = 3;
	std::unordered_map<uint64_t, uint32_t> ids;                                             
	auto vid = [&](int i, int j, int ci, int cj) {
		                                                                       
		const bool a = U(i - 1, j - 1), b = U(i, j - 1), c = U(i - 1, j), d = U(i, j);
		uint64_t slot = 0;
		if ((a && d && !b && !c) || (b && c && !a && !d))
			slot = 1 + uint64_t((ci - (i - 1)) * 2 + (cj - (j - 1)));
		const uint64_t key = (uint64_t(uint32_t(i)) << 36) | (uint64_t(uint32_t(j)) << 4) | slot;
		auto it = ids.find(key);
		if (it != ids.end()) return it->second;
		const size_t n = size_t(i) * size_t(nw + 1) + size_t(j);
		const uint32_t id = uint32_t(mesh.vertProperties.size() / 3);
		for (const V& q : {bot[n], top[n]}) {
			mesh.vertProperties.push_back(float(q.x));
			mesh.vertProperties.push_back(float(q.y));
			mesh.vertProperties.push_back(float(q.z));
		}
		ids.emplace(key, id);
		return id;
	};
	auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
	};
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			if (!U(i, j)) continue;
			const uint32_t b00 = vid(i, j, i, j), b10 = vid(i + 1, j, i, j);
			const uint32_t b11 = vid(i + 1, j + 1, i, j), b01 = vid(i, j + 1, i, j);
			const uint32_t t00 = b00 + 1, t10 = b10 + 1, t11 = b11 + 1, t01 = b01 + 1;
			quad(t00, t10, t11, t01);
			quad(b00, b01, b11, b10);
			if (!U(i, j - 1)) quad(b00, b10, t10, t00);
			if (!U(i, j + 1)) quad(b01, t01, t11, b11);
			if (!U(i - 1, j)) quad(b00, t00, t01, b01);
			if (!U(i + 1, j)) quad(b10, b11, t11, t10);
		}
	if (mesh.triVerts.empty()) return m::Manifold();
	return Orient(mesh);
}

                                                                                  

struct MasonryGuide {
    bool enabled = false;
    V n = V(0, 1, 0);
    double low = 0.0, high = 0.0, depth = 0.0;
    m::Manifold solid;
};

struct Surf {
    MasonryGuide masonry;
	SurfaceChartIndex chart;
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> polyA;
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
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}


// сглаживаем направление кладки, чтобы мелкие выступы не разворачивали кирпичи
void BuildMasonryGuide(Surf& s, const BrickDamageParams& p)
{
    if (p.organic || p.brickMode != 1 || p.style == 2) return;
    const MeshView& v = *s.v;
    if (v.npolys == 0) return;
    const auto largest = std::max_element(s.polyA.begin(), s.polyA.end());
    V n = s.polyN[size_t(largest - s.polyA.begin())];
    const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
    if (std::abs(m::la::dot(n, up)) > 0.25) return;
    double total = 0.0, aligned = 0.0, weights[2] = {0.0, 0.0};
    V sum(0.0);
    for (size_t i = 0; i < s.polyN.size(); ++i) {
        total += s.polyA[i];
        const double c = m::la::dot(s.polyN[i], n);
        if (std::abs(c) < 0.995) continue;
        aligned += s.polyA[i];
        sum += s.polyN[i] * (s.polyA[i] * (c < 0.0 ? -1.0 : 1.0));
    }
    if (!(aligned > 0.65 * total)) return;
    n = Unit(sum, n);
    std::vector<std::pair<double, double>> heights[2];
    std::vector<V> centers(size_t(v.npolys));
    for (int32_t i = 0; i < v.npolys; ++i) {
        const auto& q = v.polys[i];
        const V c = q.c == q.d ? (v.pts[q.a]+v.pts[q.b]+v.pts[q.c])/3.0
            : (v.pts[q.a]+v.pts[q.b]+v.pts[q.c]+v.pts[q.d])/4.0;
        centers[size_t(i)] = c;
        const double a = m::la::dot(s.polyN[size_t(i)], n);
        if (std::abs(a) < 0.995) continue;
        const int side = a > 0.0 ? 1 : 0;
        heights[side].push_back({m::la::dot(c, n), s.polyA[size_t(i)]});
        weights[side] += s.polyA[size_t(i)];
    }
    if (std::min(weights[0], weights[1]) < 0.2 * aligned) return;
    double level[2], spread = 0.0;
    for (int side = 0; side < 2; ++side) {
        auto& h = heights[side];
        std::sort(h.begin(), h.end());
        auto quantile = [&](double f) {
            double a = 0.0;
            for (const auto& x : h) { a += x.second; if (a >= f * weights[side]) return x.first; }
            return h.back().first;
        };
        level[side] = quantile(0.5);
        spread = std::max(spread, quantile(0.99) - quantile(0.01));
    }
    const double thickness = level[1] - level[0];
    const double brickWidth = std::max(1e-3, p.brickWidth * p.brickScale);
    const V t = Unit(m::la::cross(up, n), AnyPerp(n)), b = m::la::cross(n, t);
    double u0=1e300,u1=-1e300,v0=1e300,v1=-1e300;
    for (int32_t i=0;i<v.npts;++i) {
        const double u=m::la::dot(v.pts[i],t), w=m::la::dot(v.pts[i],b);
        u0=std::min(u0,u);u1=std::max(u1,u);v0=std::min(v0,w);v1=std::max(v1,w);
    }
    if (!(thickness > 1e-6 * s.diag && thickness < 0.35 * std::min(u1-u0,v1-v0))) return;
    if (spread < std::max(1e-5*s.diag, 0.1*p.mortar*p.brickScale) || spread > 2.0*brickWidth) return;
    m::MeshGL64 mesh;
    mesh.numProp=3;
    for (int32_t i=0;i<v.npts;++i)for(int k=0;k<3;++k)mesh.vertProperties.push_back(v.pts[i][k]);
    for (int32_t i=0;i<v.npolys;++i) {
        const auto& q=v.polys[i];
        mesh.triVerts.insert(mesh.triVerts.end(),{uint32_t(q.a),uint32_t(q.b),uint32_t(q.c)});
        if(q.c!=q.d)mesh.triVerts.insert(mesh.triVerts.end(),{uint32_t(q.a),uint32_t(q.c),uint32_t(q.d)});
    }
    mesh.Merge();
    m::Manifold solid(mesh);
    if(solid.Status()!=m::Manifold::Error::NoError || solid.IsEmpty())return;
    if(solid.Volume()<0.0) {
        for(size_t i=0;i<mesh.triVerts.size();i+=3)std::swap(mesh.triVerts[i+1],mesh.triVerts[i+2]);
        solid=m::Manifold(mesh);
    }
    if(solid.Status()!=m::Manifold::Error::NoError || solid.IsEmpty())return;
    s.masonry.enabled=true;s.masonry.n=n;s.masonry.low=level[0];s.masonry.high=level[1];
    s.masonry.depth=std::min(0.5*(p.renderMin+p.renderMax),0.2*thickness);
    s.masonry.solid=std::move(solid);
    const double mid=0.5*(level[0]+level[1]);
    for (size_t i=0;i<s.polyN.size();++i) {
        const double h=m::la::dot(centers[i],n);
        s.polyN[i]=n*(h>=mid ? 1.0 : -1.0);
    }
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
    bool masonry = false;
    double masonryPlane = 0.0, masonryLimit = 0.0;
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
	                                                                       
	                                                                         
	                                                                        
	                                                                          
	                                                                  
	struct Corner { V n; double d = 0.0, sigma = 1.0, sinA = 1.0; bool low = true; };
	std::vector<Corner> corners;
	bool hasSelf = false;                                                                      
	double selfD = 0.0;
	bool hasLat = false;                                                           
	double latU = 0.0, latV = 0.0;
	uint64_t faceKey = 0;                                           
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

                                                                      
                                             
void Frame(const BrickDamageParams& p, const V& n, V& t, V& b)
{
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	V h = m::la::cross(up, n);
	if (m::la::length(h) < 0.3) {                                                           
		const V x = std::abs(up.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
		h = x - n * m::la::dot(x, n);
	}
	t = Unit(h - n * m::la::dot(h, n), AnyPerp(n));
	b = m::la::cross(n, t);
}

                                                                           
                                                                         
                                                                          
                                                                           
                                                                             
                                                                          
                                                                             
                                                                          
                                                                           
                                                                         
                                                                              
bool FoldOutline(const Surf& s, const BrickDamageParams& p, bool canFold, std::vector<V>& pts, V& nOut)
{
	const size_t np = pts.size();
	if (np < 3) return false;
	std::vector<V> pn(np, V(0.0));
	std::vector<int> cl(np, -1);
	struct Cl { V n; double w = 0.0, d = 0.0; };
	std::vector<Cl> cls;
	for (size_t i = 0; i < np; ++i) {
		V N;
		if (!s.NormalNear(pts[i], p.paintSnap, N)) continue;
		const double w = 0.5 * (m::la::length(pts[(i + 1) % np] - pts[i]) + m::la::length(pts[i] - pts[(i + np - 1) % np]));
		int c = -1;
		for (size_t k = 0; k < cls.size(); ++k)
			if (m::la::dot(cls[k].n, N) > 0.996) { c = int(k); break; }                                
		if (c < 0) { cls.push_back({N, 0.0, 0.0}); c = int(cls.size()) - 1; }
		cls[size_t(c)].w += w;
		pn[i] = N;
		cl[i] = c;
	}
	if (cls.size() < 2 || cls.size() > 3) return false;
	int main = 0;
	for (size_t k = 1; k < cls.size(); ++k) if (cls[k].w > cls[size_t(main)].w) main = int(k);
	const V n = cls[size_t(main)].n;
	for (size_t k = 0; k < cls.size(); ++k) {
		if (int(k) == main) continue;
		const double c = m::la::dot(n, cls[k].n);
		if (c > 0.906 || c < -0.866) return false;                                                 
	}
	                                                 
	for (size_t k = 0; k < cls.size(); ++k) {
		std::vector<double> ds;
		for (size_t i = 0; i < np; ++i) if (cl[i] == int(k)) ds.push_back(m::la::dot(cls[k].n, pts[i]));
		std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end());
		cls[k].d = ds[ds.size() / 2];
	}
	const double dm = cls[size_t(main)].d;
	                                                                         
	V cm(0.0);
	int nm = 0;
	for (size_t i = 0; i < np; ++i)
		if (cl[i] == main) { cm = cm + pts[i]; ++nm; }
	if (nm == 0) return false;
	cm = cm / double(nm);
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	const bool mainStands = std::abs(m::la::dot(n, up)) <= 0.7;
	std::vector<V> outPts(np);
	for (size_t i = 0; i < np; ++i) {
		const V& q = pts[i];
		const double hm = m::la::dot(n, q) - dm;
		int c = cl[i];
		                                                                    
		                                                            
		if (c >= 0 && c != main && std::abs(hm) <= std::abs(m::la::dot(cls[size_t(c)].n, q) - cls[size_t(c)].d)) c = main;
		if (c < 0 || c == main) { outPts[i] = q - n * hm; continue; }
		const V N = cls[size_t(c)].n;
		const V e = m::la::cross(n, N);
		const double ee = m::la::dot(e, e);
		const V c0 = (m::la::cross(N, e) * dm + m::la::cross(e, n) * cls[size_t(c)].d) / ee;                      
		const V eu = e / std::sqrt(ee);
		const V along = eu * m::la::dot(q - c0, eu);
		const V foot = c0 + along;                                                                   
		const double r = m::la::length(q - foot);
		const bool fold = canFold && mainStands && std::abs(m::la::dot(N, up)) <= 0.7;
		if (!fold || r < 1e-9) { outPts[i] = foot; continue; }
		V g = Unit(m::la::cross(eu, n), AnyPerp(n));                                                      
		if (m::la::dot(g, cm - foot) > 0.0) g = -g;                                            
		outPts[i] = foot + g * r;
	}
	pts.swap(outPts);
	nOut = n;
	return true;
}

                                                                            
                            
m::Manifold Box(const V c[8])
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (int i = 0; i < 8; ++i) {
		mesh.vertProperties.push_back(float(c[i].x));
		mesh.vertProperties.push_back(float(c[i].y));
		mesh.vertProperties.push_back(float(c[i].z));
	}
	mesh.triVerts = {0, 2, 1, 0, 3, 2,   4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4,   1, 2, 6, 1, 6, 5,   2, 3, 7, 2, 7, 6,   3, 0, 4, 3, 4, 7};
	return Orient(mesh);
}

                                                                                    
                                                                             
                                                                        
                                                                             
                                                                              
                                                                          

struct Lattice {
	int bond = 0;
	double Lp = 26.0;                                        
	double Hp = 13.0;                                                                       
	double Pv = 7.5;                                      
	double m = 1.0;                     
	double offU = 0.0, offV = 0.0;
	double runOff = 0.5;                                     
	double leaf = 13.0;                                             
};

struct Cell {
	int64_t k = 0, j = 0;
	double a = 0.0, len = 1.0;                          
};

int64_t Course(const Lattice& L, double Vv) { return int64_t(std::floor((Vv - L.offV) / L.Pv)); }
double CourseV(const Lattice& L, int64_t k) { return L.offV + double(k) * L.Pv; }

Cell Locate(const Lattice& L, int w, int64_t k, double U)
{
	Cell c;
	c.k = k;
	const bool odd = (k & 1) != 0;
	double o = L.offU + double(w) * 0.37 * L.Lp;                                   
	double pitch = L.Lp;
	switch (L.bond) {
	case 1: break;                                                    
	case 2:                                                                                                       
		if (odd) { pitch = L.Hp; o -= 0.5 * L.Hp; }
		break;
	case 3: {                                                                                                             
		const double Pf = L.Lp + L.Hp;
		if (odd) o += 0.25 * L.Lp - L.Lp;
		const double t = U - o;
		const double per = std::floor(t / Pf);
		const double r = t - per * Pf;
		if (r < L.Lp) { c.j = 2 * int64_t(per); c.a = o + per * Pf; c.len = L.Lp; }
		else { c.j = 2 * int64_t(per) + 1; c.a = o + per * Pf + L.Lp; c.len = L.Hp; }
		return c;
	}
	case 4:                                                                 
		pitch = L.Hp;
		if (odd) o += 0.5 * L.Hp;
		break;
	default:                                                                             
		if (odd) o += L.runOff * L.Lp;
		break;
	}
	const double jj = std::floor((U - o) / pitch);
	c.j = int64_t(jj);
	c.a = o + jj * pitch;
	c.len = pitch;
	return c;
}

uint64_t BrickKey(int w, int64_t k, int64_t j)
{
	uint64_t z = uint64_t(k) * 0x9e3779b97f4a7c15ULL ^ (uint64_t(j) + 0x632be59bd9b4e019ULL) * 0xbf58476d1ce4e5b9ULL
		^ (uint64_t(w) + 1) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return z;
}

                                                                                        

struct Out {
    bool masonry = false;
	m::Manifold cutter;
	std::vector<m::Manifold> cracks, bricks;
	std::vector<uint64_t> brickKeys;
	m::Manifold breaks, seams, behind;                         
	m::Manifold render;                                                                         
	m::Manifold mortar;                                                                                
};

                                                            
enum Part : char { kRender = 0, kBreak = 1, kSeam = 2, kBehind = 3 };

                                                                            
                                                                              
                                                                                                          
                                                                     
                                                                        
                                                                            
struct Model {
	bool ok = false;                          
	bool solid = false;                                                   
	std::vector<V> pts;
	std::vector<uint32_t> tris;
	                                                                      
	                                                                           
	                                                         
	static const int SX = 160, SY = 48, HX = 64;
	std::vector<float> face, head;                      
	double Sample(const std::vector<float>& m, int nx, double x, double y) const                   
	{
		const double fx = ClampD(0.5 * x + 0.5, 0.0, 1.0) * (nx - 1), fy = ClampD(0.5 * y + 0.5, 0.0, 1.0) * (SY - 1);
		const int i = std::min(nx - 2, int(fx)), j = std::min(SY - 2, int(fy));
		const double a = fx - i, c = fy - j;
		auto at = [&](int ii, int jj) { return double(m[size_t(jj) * size_t(nx) + size_t(ii)]); };
		return (at(i, j) * (1 - a) + at(i + 1, j) * a) * (1 - c) + (at(i, j + 1) * (1 - a) + at(i + 1, j + 1) * a) * c;
	}
};

bool PrepareModel(const MeshView& mv, bool flip, Model& md)
{
	if (mv.npts < 4 || mv.npolys < 4 || !mv.pts || !mv.polys) return false;
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < mv.npts; ++i) {
		const V& q = mv.pts[i];
		if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
		lo = m::la::min(lo, q); hi = m::la::max(hi, q);
	}
	const V ext = hi - lo;
	int ax[3] = {0, 1, 2};                                     
	std::sort(ax, ax + 3, [&](int a, int b) { return ext[a] > ext[b] || (ext[a] == ext[b] && a < b); });
	const int aL = ax[0], aD = ax[1], aH = ax[2];
	if (!(ext[aH] > 1e-9)) return false;
	md.pts.resize(size_t(mv.npts));
	for (int32_t i = 0; i < mv.npts; ++i) {
		const V& q = mv.pts[i];
		const double x = 2.0 * (q[aL] - lo[aL]) / ext[aL] - 1.0;
		const double y = 2.0 * (q[aH] - lo[aH]) / ext[aH] - 1.0;
		double z = (q[aD] - lo[aD]) / ext[aD];
		if (flip) z = 1.0 - z;
		md.pts[size_t(i)] = V(x, y, z);
	}
	for (int32_t i = 0; i < mv.npolys; ++i) {
		const ProjectDamagePoly& q = mv.polys[i];
		auto okI = [&](int32_t a) { return a >= 0 && a < mv.npts; };
		if (!okI(q.a) || !okI(q.b) || !okI(q.c) || !okI(q.d)) return false;
		md.tris.insert(md.tris.end(), {uint32_t(q.a), uint32_t(q.b), uint32_t(q.c)});
		if (q.c != q.d) md.tris.insert(md.tris.end(), {uint32_t(q.a), uint32_t(q.c), uint32_t(q.d)});
	}
	                                                                         
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (const V& q : md.pts) { mesh.vertProperties.push_back(float(q.x)); mesh.vertProperties.push_back(float(q.y)); mesh.vertProperties.push_back(float(q.z)); }
	mesh.triVerts = md.tris;
	mesh.Merge();
	m::Manifold test(mesh);
	md.solid = test.Status() == m::Manifold::Error::NoError && !test.IsEmpty();
	                                                                            
	std::vector<EVec> np(md.pts.begin(), md.pts.end());
	std::vector<ProjectDamagePoly> nq;
	for (size_t k = 0; k < md.tris.size(); k += 3)
		nq.push_back({int32_t(md.tris[k]), int32_t(md.tris[k + 1]), int32_t(md.tris[k + 2]), int32_t(md.tris[k + 2])});
	ProjectDamageBVH bvh;
	bvh.Build(MeshView{np.data(), int32_t(np.size()), nq.data(), int32_t(nq.size())});
	auto shoot = [&](const V& org, const V& dir, double span) {
		double t = 0.0;
		int32_t poly = -1;
		                                                        
		if (!bvh.Raycast(org, dir, 4.0, &t, &poly)) return 0.5f;
		return float(ClampD((t - 1.0) / span, 0.0, 1.0));
	};
	md.face.assign(size_t(Model::SX) * Model::SY, 0.0f);
	md.head.assign(size_t(Model::HX) * Model::SY, 0.0f);
	for (int j = 0; j < Model::SY; ++j) {
		const double y = -1.0 + 2.0 * j / (Model::SY - 1);
		for (int i = 0; i < Model::SX; ++i) {
			const double x = -1.0 + 2.0 * i / (Model::SX - 1);
			md.face[size_t(j) * Model::SX + size_t(i)] = shoot(V(x, y, -1.0), V(0, 0, 1), 1.0);
		}
		for (int i = 0; i < Model::HX; ++i) {
			const double z = double(i) / (Model::HX - 1);                                            
			md.head[size_t(j) * Model::HX + size_t(i)] = shoot(V(-2.0, y, z), V(1, 0, 0), 2.0);
		}
	}
	md.ok = true;
	return true;
}

                                                                               
template <typename F>
m::Manifold Instance(const Model& md, F place)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(md.pts.size() * 3);
	for (const V& q : md.pts) {
		const V w = place(q);
		mesh.vertProperties.push_back(float(w.x));
		mesh.vertProperties.push_back(float(w.y));
		mesh.vertProperties.push_back(float(w.z));
	}
	mesh.triVerts = md.tris;
	mesh.Merge();
	return Orient(mesh);
}

                                                                             
                                                                          
struct Grid {
	std::vector<double> us, vs;
	int nu = 0, nv = 0;
	std::vector<double> h, d, s, thick;
	std::vector<char> valid, holeF, owned;
	size_t I(int i, int j) const { return size_t(i) * size_t(nv + 1) + size_t(j); }
	bool Cellof(double u, double v, int& i, int& j, double& a, double& c) const
	{
		if (u < us.front() || u > us.back() || v < vs.front() || v > vs.back()) return false;
		i = std::max(0, std::min(nu - 1, int(std::upper_bound(us.begin(), us.end(), u) - us.begin()) - 1));
		j = std::max(0, std::min(nv - 1, int(std::upper_bound(vs.begin(), vs.end(), v) - vs.begin()) - 1));
		a = ClampD((u - us[size_t(i)]) / (us[size_t(i) + 1] - us[size_t(i)]), 0.0, 1.0);
		c = ClampD((v - vs[size_t(j)]) / (vs[size_t(j) + 1] - vs[size_t(j)]), 0.0, 1.0);
		return true;
	}
	double Bl(const std::vector<double>& f, int i, int j, double a, double c) const
	{
		return (f[I(i, j)] * (1 - a) + f[I(i + 1, j)] * a) * (1 - c) + (f[I(i, j + 1)] * (1 - a) + f[I(i + 1, j + 1)] * a) * c;
	}
	double SAt(double u, double v) const
	{
		int i, j; double a, c;
		if (!Cellof(u, v, i, j, a, c)) return -1e9;
		return Bl(s, i, j, a, c);
	}
	                                                               
	bool Sample(double u, double v, double& hh, double& dd, bool& hole, bool& own) const
	{
		int i, j; double a, c;
		if (!Cellof(u, v, i, j, a, c)) return false;
		hh = Bl(h, i, j, a, c); dd = Bl(d, i, j, a, c);
		const int ii = a < 0.5 ? i : i + 1, jj = c < 0.5 ? j : j + 1;
		hole = holeF[I(ii, jj)] != 0;
		own = owned[I(ii, jj)] != 0;
		return valid[I(i, j)] && valid[I(i + 1, j)] && valid[I(i, j + 1)] && valid[I(i + 1, j + 1)];
	}
};

                                                                              
                                                                              
                
std::vector<double> Lines(double lo, double hi, double cell, std::vector<double> edges, double eps)
{
	std::sort(edges.begin(), edges.end());
	std::vector<double> e2;
	for (double e : edges) {
		if (e - eps <= lo || e + eps >= hi) continue;
		if (!e2.empty() && e - eps <= e2.back() + 0.5 * eps) continue;                        
		e2.push_back(e - eps);
		e2.push_back(e + eps);
	}
	const int n = std::max(4, int(std::ceil((hi - lo) / cell)));
	const double st = (hi - lo) / n;
	std::vector<double> out;
	out.reserve(size_t(n + 1) + e2.size());
	size_t q = 0;
	for (int i = 0; i <= n; ++i) {
		const double x = lo + i * st;
		while (q < e2.size() && e2[q] < x) out.push_back(e2[q++]);
		const bool ends = i == 0 || i == n;
		bool near = false;
		if (!ends) {
			if (q < e2.size() && e2[q] - x < 0.4 * st) near = true;
			if (q > 0 && x - e2[q - 1] < 0.4 * st) near = true;
		}
		if (!near) out.push_back(x);
	}
	while (q < e2.size()) out.push_back(e2[q++]);
	std::sort(out.begin(), out.end());
	std::vector<double> clean;
	for (double x : out)
		if (clean.empty() || x - clean.back() > 0.2 * eps) clean.push_back(x);
	clean.back() = std::max(clean.back(), hi);
	return clean;
}

                                                                           
                                                                              
                                                                           
                                                                             
                                                                  
                                                                        
                                                                        
                                                                         
                                                                           
                                                                           
                                                                            
                                                                          
                                           
enum ClipMode { kClipAll = 0, kClipInner = 1, kClipRing = 2 };
m::CrossSection Rect2(double u0, double u1, double v0, double v1);
m::CrossSection Union2(std::vector<m::CrossSection>& parts);
m::Manifold RemeshEven(const m::Manifold& in, double edge, const std::vector<m::Box>* zones, double flatCos);

m::Manifold ClippedSlabSolid(const Grid& g, const std::vector<V>& top, const std::vector<V>& bot,
	const std::vector<char>* use, ClipMode mode = kClipAll, double ring = 0.0)
{
	const int ns = g.nu, nw = g.nv;
	auto U = [&](int i, int j) {
		return i >= 0 && j >= 0 && i < ns && j < nw
			&& (!use || (*use)[size_t(i) * size_t(nw) + size_t(j)] != 0);
	};
	m::MeshGL mesh;
	mesh.numProp = 3;
	std::unordered_map<uint64_t, uint32_t> ids;                                           
	std::vector<uint64_t> keyOf;                                                      
	auto emit = [&](uint64_t key, const V& b, const V& t) {
		const uint32_t id = uint32_t(mesh.vertProperties.size() / 3);
		keyOf.push_back(key);
		for (const V& q : {b, t}) {
			mesh.vertProperties.push_back(float(q.x));
			mesh.vertProperties.push_back(float(q.y));
			mesh.vertProperties.push_back(float(q.z));
		}
		ids.emplace(key, id);
		return id;
	};
	auto node = [&](int i, int j, int ci, int cj) {
		                                                                       
		const bool a = U(i - 1, j - 1), b = U(i, j - 1), c = U(i - 1, j), d = U(i, j);
		uint64_t slot = 0;
		if ((a && d && !b && !c) || (b && c && !a && !d))
			slot = 1 + uint64_t((ci - (i - 1)) * 2 + (cj - (j - 1)));
		const size_t n = g.I(i, j);
		const uint64_t key = (uint64_t(n) << 3) | slot;
		auto it = ids.find(key);
		return it != ids.end() ? it->second : emit(key, bot[n], top[n]);
	};
	                                                                                   
	auto cross = [&](size_t a, size_t b, bool inset) {
		if (a > b) std::swap(a, b);                                     
		const uint64_t key = (uint64_t(1) << 63) | (inset ? uint64_t(1) << 62 : 0)
			| (uint64_t(a) << 1) | (b == a + 1 ? 1u : 0u);
		auto it = ids.find(key);
		if (it != ids.end()) return it->second;
		double t = ClampD(g.s[a] / (g.s[a] - g.s[b]), 1e-4, 1.0 - 1e-4);
		if (inset) {
			const size_t i = a / size_t(nw + 1), j = a % size_t(nw + 1);
			const double len = b == a + 1 ? g.vs[j + 1] - g.vs[j] : g.us[i + 1] - g.us[i];
			const double tin = g.s[a] > 0.0 ? 0.0 : 1.0;
			const double dt = std::min(ring / std::max(len, 1e-12), 0.5 * std::abs(t - tin));
			t += tin > t ? dt : -dt;
		}
		return emit(key, bot[a] + (bot[b] - bot[a]) * t, top[a] + (top[b] - top[a]) * t);
	};
	std::vector<std::vector<uint32_t>> polys;
	std::unordered_set<uint64_t> half;                            
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			if (!U(i, j)) continue;
			const int ci[4] = {i, i + 1, i + 1, i}, cj[4] = {j, j, j + 1, j + 1};
			size_t c[4];
			bool in[4];
			for (int q = 0; q < 4; ++q) { c[q] = g.I(ci[q], cj[q]); in[q] = g.s[c[q]] > 0.0; }
			auto add = [&](std::vector<uint32_t>&& poly) {
				if (poly.size() < 3) return;
				for (size_t q = 0; q < poly.size(); ++q)
					half.insert((uint64_t(poly[q]) << 32) | poly[(q + 1) % poly.size()]);
				polys.push_back(std::move(poly));
			};
			if (mode == kClipRing) {
				                                                                 
				                                                                  
				int exitQ = -1;
				for (int qq = 0; qq < 8; ++qq) {
					const int q = qq & 3, r = (q + 1) & 3;
					if (in[q] && !in[r]) {
						if (qq >= 4) break;                           
						exitQ = q;
					} else if (!in[q] && in[r] && exitQ >= 0) {
						const int er = (exitQ + 1) & 3;
						add({cross(c[exitQ], c[er], false), cross(c[q], c[r], false),
							cross(c[q], c[r], true), cross(c[exitQ], c[er], true)});
						exitQ = -1;
					}
				}
				continue;
			}
			std::vector<uint32_t> poly;
			for (int q = 0; q < 4; ++q) {
				const int r = (q + 1) & 3;
				if (in[q]) poly.push_back(node(ci[q], cj[q], i, j));
				if (in[q] != in[r]) poly.push_back(cross(c[q], c[r], mode == kClipInner));
			}
			add(std::move(poly));
		}
	for (const auto& poly : polys) {
		const size_t m = poly.size();
		for (size_t q = 1; q + 1 < m; ++q) {
			mesh.triVerts.insert(mesh.triVerts.end(), {poly[0] + 1, poly[q] + 1, poly[q + 1] + 1});
			mesh.triVerts.insert(mesh.triVerts.end(), {poly[0], poly[q + 1], poly[q]});
		}
		for (size_t q = 0; q < m; ++q) {
			const uint32_t a = poly[q], b = poly[(q + 1) % m];
			if (half.count((uint64_t(b) << 32) | a)) continue;                                  
			                                                                   
			                                                                    
			                          
			if (keyOf[a / 2] < keyOf[b / 2]) mesh.triVerts.insert(mesh.triVerts.end(), {a, b, b + 1, a, b + 1, a + 1});
			else mesh.triVerts.insert(mesh.triVerts.end(), {a, b, a + 1, b, b + 1, a + 1});
		}
	}
	if (mesh.triVerts.empty()) return m::Manifold();
	return Orient(mesh);
}

struct Ctx {
	const BrickDamageParams* p;
	const Surf* s;
	uint64_t seed;
	Lattice L;
	double W;                                                              
	double mDepth;                    
	int layers;
	bool split = false;                                       
	bool noiseCore = false;                                                                      
	double coreDepth = 0.0, coreScale = 1.0, coreNoise = 0.0;
	bool boxBricks = false;                                                            
	bool separate = false;                                                                          
	std::vector<const Model*> models;                                                                   
};

struct Brick {
	bool removed = false;                                               
	bool fracture = false;                                                       
	double fx = 0, fy = 0, fc = 0;
	double push = 0, tu = 0, tv = 0;                                          
	bool pulled = false;
	double cu = 0, cv = 0, hx = 0, hy = 0;                                                
	int64_t k = 0, j = 0;
	int w = 0;
	bool header = false;
	uint64_t key = 0;
};

// строим одну область повреждения и её внутренние детали
void BuildPatch(const Ctx& cx, const Patch& P, Out& out)
{
	const BrickDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Lattice& L = cx.L;
    out.masonry = P.masonry;
	const double leaves = double(cx.layers) * L.leaf;
	const double Dtot = P.D + leaves + cx.W + (cx.noiseCore || cx.separate ? cx.coreDepth : 0.0);                               
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const double ampOut = jag * 0.5 * P.Rn;
	const double featOut = 0.55 * P.Rn * std::max(0.05, p.outlineScale);
	double featC = std::max(1e-3, p.crumbleSize);
	const double ampC = ClampD(p.crumble, 0.0, 3.0) * 1.2 * featC;
	const double pad = 0.6 * ampOut + 0.6 * ampC + 0.08 * P.R + 1e-3;
	const double u0 = P.u0 - pad, u1 = P.u1 + pad, v0 = P.v0 - pad, v1 = P.v1 + pad;
	const double extent = std::max(u1 - u0, v1 - v0);
	if (!(extent > 0.0)) return;
	const double baseU = P.hasLat ? P.latU : m::la::dot(P.o, P.t), baseV = P.hasLat ? P.latV : m::la::dot(P.o, P.b);
    const double brickH = P.masonry ? P.masonryPlane - m::la::dot(P.o, P.n) : 0.0;
    const double brickD = P.masonry ? s.masonry.depth : P.D;

	Grid g;
	double cell = extent / (140.0 * p.detail);
	cell = std::max(cell, extent / 600.0);
	                                                          
	{
		const double eps = std::max(0.004, std::min(0.06 * L.m, 0.2 * cell));
		std::vector<double> eu, ev;
		const int64_t k0 = Course(L, baseV + v0) - 1, k1 = Course(L, baseV + v1) + 1;
		for (int64_t k = k0; k <= k1; ++k) {
			ev.push_back(CourseV(L, k) + 0.5 * L.m - baseV);
			ev.push_back(CourseV(L, k + 1) - 0.5 * L.m - baseV);
			for (int w = 0; w <= cx.layers; ++w) {
				Cell c = Locate(L, w, k, baseU + u0);
				for (int guard = 0; guard < 100000 && c.a < baseU + u1; ++guard) {
					eu.push_back(c.a + 0.5 * L.m - baseU);
					eu.push_back(c.a + c.len - 0.5 * L.m - baseU);
					c = Locate(L, w, k, c.a + c.len + 0.25 * L.m);
				}
			}
		}
		                                                                                    
		if (eu.size() > 3000) eu.clear();
		if (ev.size() > 3000) ev.clear();
		g.us = Lines(u0, u1, cell, eu, eps);
		g.vs = Lines(v0, v1, cell, ev, eps);
	}
	g.nu = int(g.us.size()) - 1;
	g.nv = int(g.vs.size()) - 1;
	if (g.nu < 2 || g.nv < 2) return;
	featC = std::max(featC, 2.0 * cell);                                              
	const size_t N = size_t(g.nu + 1) * size_t(g.nv + 1);
	g.h.assign(N, 0.0); g.d.assign(N, 0.0); g.s.assign(N, -1.0); g.thick.assign(N, -1.0);
	g.valid.assign(N, 0); g.holeF.assign(N, 0); g.owned.assign(N, 1);
	std::vector<char> forbid(N, 0);
	const auto chart = p.organic ? s.chart.Select(P.o,P.n,extent*1.5,s.polyN) : std::vector<char>();
	std::vector<double> arcs(N, 0.0);

	const double Hs = 0.3 * extent + Dtot + 1e-3;
	const double drop = 0.6 * extent + 2.0 * Dtot;
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double u = g.us[size_t(i)], v = g.vs[size_t(j)];
			const V org = P.o + P.t * u + P.b * v + P.n * Hs;
			double dist = 0.0;
			int32_t poly = -1;
			if (s.Ray(org, -P.n, Hs + drop, dist, poly) && m::la::dot(s.polyN[size_t(poly)], P.n) > (p.organic ? 0.3 : 0.05) && (!p.organic || chart[size_t(poly)])) {
				g.h[k] = Hs - dist;
				g.valid[k] = 1;
				forbid[k] = s.Avoided(poly) ? 1 : 0;
				const V S = P.o + P.t * u + P.b * v + P.n * g.h[k];
				double th = 0.0;
				int32_t tp = -1;
				if (s.Ray(S - P.n * (1e-5 * s.diag), -P.n, 2.0 * s.diag, th, tp)) g.thick[k] = th;
			}
		}
	                                                                           
	                                                                        
	                                                                        
	                                                 
	if (!P.corners.empty() && P.hasSelf) {
		const double hPlane = P.selfD - m::la::dot(P.o, P.n);
		const double lim = P.D + cx.W + 3.0 * L.m + 3.0 * cell;
		std::vector<double> over(N, -1.0);
		for (int i = 0; i <= g.nu; ++i)
			for (int j = 0; j <= g.nv; ++j) {
				const V X = P.o + P.t * g.us[size_t(i)] + P.b * g.vs[size_t(j)] + P.n * hPlane;
				double o = -1e300;
				for (const Patch::Corner& c : P.corners)
					o = std::max(o, c.sigma * (m::la::dot(X, c.n) - c.d) / std::max(0.2, c.sinA));
				over[g.I(i, j)] = o;
			}
		                                                                     
		                                                                       
		std::vector<char> hitValid(g.valid);
		for (int j = 0; j <= g.nv; ++j)
			for (int i = 0; i <= g.nu; ++i) {
				const size_t k = g.I(i, j);
				if (!(over[k] > 0.0)) continue;
				int best = -1;
				for (int d = 1; d <= g.nu && best < 0; ++d) {
					if (i - d >= 0 && !(over[g.I(i - d, j)] > 0.0)) best = i - d;
					else if (i + d <= g.nu && !(over[g.I(i + d, j)] > 0.0)) best = i + d;
				}
				g.h[k] = hPlane;
				g.valid[k] = (best >= 0 && hitValid[g.I(best, j)] && over[k] <= lim) ? 1 : 0;
				g.thick[k] = -1.0;
				forbid[k] = 0;
			}
	}
	                                                                    
	{
		if (p.organic) ConnectSurfaceChart(g,u0,v0,cell,(u1-u0)/g.nu,(v1-v0)/g.nv,&g.us,&g.vs);
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

	                                                 
    const auto objectField=SampleDamageObject(g,P,u0,v0,(u1-u0)/g.nu,(v1-v0)/g.nv,&g.us,&g.vs);
	double sMax = -1e300;
	size_t kMax = 0;
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double u = g.us[size_t(i)], v = g.vs[size_t(j)];
			const V Wp = P.o + P.t * u + P.b * v + P.n * g.h[k];
			double arc = 0.0;
			double sv = (P.volume?objectField.distance[k]:RawField(P, u, v, &arc)) + ampOut * Fbm(Wp / featOut + P.off, 5);
			if (ampC > 0.0) sv += ampC * Fbm(Wp / featC + P.off + V(-13.1, 27.9, 4.4), 3);
			arcs[k] = arc;
			if (P.shape == Shape::Stroke) g.owned[k] = (arc >= P.coreA && arc <= P.coreB) ? 1 : 0;
			if (forbid[k] || (p.organic && !g.valid[k])) sv = std::min(sv, -cell);
			g.s[k] = sv;
			if (sv > sMax) { sMax = sv; kMax = k; }
		}
	                                                                         
	                                                                
	                                                                    
	const bool v2 = p.profile == 1;
	const bool sharp = p.sharpEdge && !v2;
	const double WallClassic = 1.2 * cell + (sharp ? 0.0 : ClampD(p.wallSoft, 0.0, 1.0) * 0.3 * std::max(sMax, 0.0));
	if (sMax < 1.5 * WallClassic) return;                       
	const double Wall = v2 ? std::max(WallClassic, ClampD(p.smoothWidth, 0.05, 1.0) * sMax) : WallClassic;

	auto rnd = [&](uint64_t ch) { return Random01(cx.seed ^ 0x435241434bULL, P.id, ch); };

	                                                                                       
	                                                                           
	                                                                                
	const double brickAmp = ClampD(p.breakJagged, 0.0, 2.0) * 1.3 * L.Pv;
	std::vector<double> thr(size_t(cx.layers + 1), 1e300);
	for (int w = 0; w < cx.layers; ++w)
		thr[size_t(w)] = std::min((1.0 - ClampD(p.breakSize, 0.0, 1.0) + w * ClampD(p.layerInset, 0.0, 1.0)) * sMax, 0.97 * sMax);
	double thrH = 1e300;
	if (P.hole && p.holeEnable) {
		thrH = (1.0 - ClampD(p.holeSize, 0.0, 1.0)) * sMax;
		if (cx.layers > 0) thrH = std::min(std::max(thrH, thr[size_t(cx.layers - 1)] + 0.05 * sMax), 0.98 * sMax);
	}
	const double hy = 0.5 * (L.Pv - L.m);
	const uint64_t bseed = cx.seed ^ 0x425249434bULL;             
	auto br = [&](uint64_t key, uint64_t ch) { return Random01(bseed, key, ch); };

	                                                               
	auto geom = [&](int w, int64_t k, const Cell& c) {
		Brick b;
		b.k = k; b.j = c.j; b.w = w;
		b.header = c.len < 0.75 * L.Lp;
		b.key = BrickKey(w, k, c.j);
		b.cu = c.a + 0.5 * c.len - baseU;
		b.cv = CourseV(L, k) + 0.5 * L.Pv - baseV;
		b.hx = 0.5 * (c.len - L.m);
		b.hy = hy;
		return b;
	};
	                                                   
	auto exposed = [&](const Brick& b) {
		const double m0 = std::min(std::min(g.SAt(b.cu - b.hx, b.cv - b.hy), g.SAt(b.cu + b.hx, b.cv - b.hy)),
			std::min(g.SAt(b.cu - b.hx, b.cv + b.hy), g.SAt(b.cu + b.hx, b.cv + b.hy)));
		return m0 > Wall;
	};
	                                                          
	auto baseRemoved = [&](int w, const Brick& b) {
		if (w >= cx.layers) return false;
		const double sc = g.SAt(b.cu, b.cv);
		if (w == 0 && !exposed(b)) return false;
		if (sc > thr[size_t(w)] + (br(b.key, 1) - 0.5) * 2.0 * brickAmp) return true;
		return w == 0 && br(b.key, 2) < ClampD(p.missing, 0.0, 1.0);
	};
	auto cellAt = [&](int w, int64_t k, double U) { return geom(w, k, Locate(L, w, k, U)); };
	std::unordered_map<uint64_t, Brick> cache;
	const bool useModel = !cx.models.empty();
	                                                                      
	auto modelOf = [&](uint64_t key) -> const Model& {
		const size_t n = cx.models.size();
		return *cx.models[std::min(n - 1, size_t(br(key, 51) * double(n)))];
	};
	auto state = [&](int w, int64_t k, const Cell& c) -> const Brick& {
		const uint64_t key = BrickKey(w, k, c.j);
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		Brick b = geom(w, k, c);
		b.removed = baseRemoved(w, b);
		const double faceT = ClampD(p.faceTilt, 0.0, 20.0) * m::kPi / 180.0;
		b.push = (br(key, 3) - 0.5) * 2.0 * std::max(0.0, p.faceJitter);
		b.tu = std::tan((br(key, 4) - 0.5) * 2.0 * faceT) * 0.4;
		b.tv = std::tan((br(key, 5) - 0.5) * 2.0 * faceT);
		if (!b.removed && w < cx.layers && (w > 0 || exposed(b))) {
			                                                                              
			const double Uc = b.cu + baseU;
			const Brick nb[4] = {cellAt(w, k, Uc - b.hx - L.m), cellAt(w, k, Uc + b.hx + L.m),
				cellAt(w, k - 1, Uc), cellAt(w, k + 1, Uc)};
			const double dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
			int nOpen = 0, pick = -1;
			for (int q = 0; q < 4; ++q)
				if (baseRemoved(w, nb[q])) { ++nOpen; if (pick < 0 || br(key, 10 + uint64_t(q)) < 0.5) pick = q; }
			if (nOpen > 0 && br(key, 6) < ClampD(p.broken, 0.0, 1.0)) {
				const double ang = cx.boxBricks ? std::atan2(dirs[pick][1], dirs[pick][0])                            
					: std::atan2(dirs[pick][1] * b.hy, dirs[pick][0] * b.hx) + (br(key, 7) - 0.5) * 1.2;
				b.fx = std::cos(ang); b.fy = std::sin(ang);
				const double reach = std::abs(b.fx) * b.hx + std::abs(b.fy) * b.hy;
				b.fc = Lerp(-0.25, 0.55, br(key, 8)) * reach;
				b.fracture = true;
			} else if (w == 0 && br(key, 20) < ClampD(p.loose, 0.0, 1.0)) {
				                                                         
				const double lt = ClampD(p.looseTilt, 0.0, 30.0) * m::kPi / 180.0;
				const Brick below = nb[2];
				if (br(key, 21) < ClampD(p.looseOut, 0.0, 1.0) && p.looseOutDist > 0.0 && !baseRemoved(0, below)) {
					b.pulled = true;
					b.removed = true;
				} else {
					b.push += Lerp(0.25, 1.0, br(key, 22)) * std::max(0.0, p.looseDepth);
					b.tu += std::tan((br(key, 23) - 0.5) * 2.0 * lt) * 0.5;
					b.tv += std::tan((br(key, 24) - 0.5) * 2.0 * lt);
				}
			}
		}
		return cache.emplace(key, b).first->second;
	};

	                                                                                    
	const int Lr = v2 ? 1 : std::max(1, std::min(p.terraces, 4));
	const double skin = Lr == 1 ? 1.0 : ClampD(p.terraceSkin, 0.0, 1.0);
	const double inset = ClampD(p.terraceInset, 0.0, 0.5) * sMax;
	const double ampT = jag * 0.18 * sMax;
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double margin = std::max(0.3 * Dtot, 2.0 * cell);
	const double lift = 0.4 * margin;
	const double rough = ClampD(p.brickRough, 0.0, 2.0);
	const double chipA = ClampD(p.chip, 0.0, 2.0), chipR = std::max(1e-3, p.chipSize);
	const double jRough = ClampD(p.jointRough, 0.0, 2.0);
	const double fracR = ClampD(p.brokenRough, 0.0, 2.0) * 0.35 * L.Pv;
	const double fracRamp = cx.boxBricks ? 1e-3 * cell : 2.0 * cell + 0.2 * L.Pv;
	const double stickThr = Lerp(0.55, -0.35, ClampD(p.renderStick, 0.0, 1.0));
	const double collarThr = Lerp(0.6, -0.6, ClampD(p.collarFill, 0.0, 1.0));
	const double minCut = 0.02 * L.m + 1e-4;
	                                                                          
	                                                                       
	                                                                      
	const double rimLip = std::max({0.02 * P.D, 2.0 * minCut, std::max(0.0,p.edgeWallDepth)*0.01*P.Rn});
	std::vector<char> outside(N, 0);
	std::vector<char> coreN(N, 0);                                                     
	const double coreSkin = 0.5 * std::max(L.m, 0.05 * L.leaf);
	std::vector<char> part(N, kRender);
	                                                                        
	                                                                   
	std::vector<char> nearEdge(N, 0);
	if (sharp)
		for (int i = 0; i <= g.nu; ++i)
			for (int j = 0; j <= g.nv; ++j) {
				if (g.s[g.I(i, j)] <= 0.0) continue;
				for (int di = -1; di <= 1; ++di)
					for (int dj = -1; dj <= 1; ++dj) {
						const int a = i + di, b = j + dj;
						if (a >= 0 && b >= 0 && a <= g.nu && b <= g.nv) nearEdge[g.I(a, b)] = 1;
					}
			}
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double sv = g.s[k];
			if (sv <= 0.0 && !nearEdge[k]) { outside[k] = 1; g.d[k] = 0.0; continue; }
			const double u = g.us[size_t(i)], v = g.vs[size_t(j)];
			const V Wp = P.o + P.t * u + P.b * v + P.n * g.h[k];
			               
			double d = 0.0, core = 0.0;
			if (sharp) { d = P.D; core = 1.0; }                                            
			else for (int l = 0; l < Lr; ++l) {
				const double w = l == 0 ? skin : (1.0 - skin) / (Lr - 1);
				double sl = sv - l * inset;
				if (l > 0) {
					sl += ampT * Fbm(Wp / (0.45 * P.Rn) + P.off + V(31.7 * l, 11.3 * l, -7.1 * l), 4);
					if (ampC > 0.0) sl += ampC * Fbm(Wp / featC + P.off + V(-5.3 * l, 9.1 * l, 17.7 * l), 3);
				}
				double st = Smooth(0.0, Wall, sl);
				if (v2) { const double x = ClampD(sl / Wall, 0.0, 1.0); st = x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
				d += P.D * w * st;
				if (l == Lr - 1) core = st;
			}
			if (!sharp && p.roughness > 0.0 && P.D > 0.0) {
				const double big = Fbm(Wp / (0.3 * P.Rn) + P.off + V(5.5, 1.5, 9.5), 3);
				const double fine = Fbm(Wp / (2.0 * featC) + P.off + V(-3.3, 8.1, 2.2), 3);
				const double r = ClampD(p.roughness, 0.0, 2.0) * 0.3 * P.D * (0.6 * big + 0.4 * fine)
					* Smooth(0.0, 2.0 * Wall, sv) * (1.0 - core);
				d = std::max(d + r, 0.3 * d);
			}
			                                                                        
			double bd = 0.0;
			bool through = false, stopFront = false;
			char nodePart = kRender;                                                  
			if (core > 0.0 && !cx.separate) {
				const double U = u + baseU, Vv = v + baseV;
				const int64_t kc = Course(L, Vv);
				const double y = Vv - CourseV(L, kc) - 0.5 * L.Pv;
				int w = 0;
				for (;; ++w) {
					const Cell c = Locate(L, w, kc, U);
					const Brick& b = state(w, kc, c);
					const double x = U - (c.a + 0.5 * c.len);
					const bool inX = std::abs(x) <= b.hx, inY = std::abs(y) <= b.hy;
					bool gone = b.removed;
					double fs = -1e300;                                                                       
					if (!gone && b.fracture && inX && inY) {
						fs = b.fx * x + b.fy * y - b.fc + fracR * Fbm(Wp / (0.35 * L.Pv) + V(double(b.key % 997), 3.3, 1.1), 3);
						gone = fs > 0.0;
					}
					if (!gone && w < cx.layers && !(inX && inY)) {
						                                                   
						if (!inX) gone |= state(w, kc, Locate(L, w, kc, x < 0.0 ? c.a - 0.25 * L.m : c.a + c.len + 0.25 * L.m)).removed;
						if (!inY && !gone) gone |= state(w, y < 0.0 ? kc - 1 : kc + 1, Locate(L, w, y < 0.0 ? kc - 1 : kc + 1, U)).removed;
					}
					if (gone && w < cx.layers) continue;
					if (w == cx.layers && P.hole && p.holeEnable) {
						const Brick& hb = b;
						if (g.SAt(hb.cu, hb.cv) > thrH + (br(hb.key, 30) - 0.5) * 2.0 * brickAmp) { through = true; break; }
					}
					if (cx.noiseCore && w >= cx.layers) {
						                                                              
						                                                               
						bd = cx.layers * L.leaf + coreSkin;
						nodePart = kBehind;
						if (sv > 0.0) coreN[k] = 1;
						break;
					}
					                          
					const double base = w * L.leaf;
					const double fn = Fbm(Wp / 0.9 + V(1.7, -4.2, 8.8), 3);
					if (w > 0 && Fbm(Wp / (1.3 * L.Pv) + P.off + V(7.0, 2.0, -5.0), 3) > collarThr) {
						                                            
						bd = base + 0.45 * cx.mDepth + 0.25 * cx.mDepth * fn;
						nodePart = cx.noiseCore ? kSeam : kBehind;                                                  
					} else if (inX && inY && useModel) {
						                                                           
						double mx = x / b.hx, my = y / b.hy;
						if (br(b.key, 50) < ClampD(p.modelTurn, 0.0, 1.0)) { mx = -mx; my = -my; }                                
						const double rel = b.header
							? modelOf(b.key).Sample(modelOf(b.key).head, Model::HX, mx, my) * (2.0 * cx.L.Hp - cx.L.m)                                   
							: modelOf(b.key).Sample(modelOf(b.key).face, Model::SX, mx, my) * cx.W;
						double dep = base + b.push + b.tu * x + b.tv * y + std::min(rel, 0.5 * cx.W);
						if (fs > -fracRamp) {
							const double f = Smooth(-fracRamp, 0.0, fs);
							dep = Lerp(dep, (w + 1) * L.leaf - 0.1 * L.leaf, f * f);
						}
						bd = dep;
                        nodePart = w > 0 && !cx.noiseCore ? kBehind : kRender;
                        if (fs > -fracRamp && Smooth(-fracRamp, 0.0, fs) > 0.2) nodePart = kBreak;
					} else if (inX && inY) {
						const double e = std::min(b.hx - std::abs(x), b.hy - std::abs(y));
						double dep = base + b.push + b.tu * x + b.tv * y;
						dep += rough * 0.18 * std::max(0.0, fn);
						nodePart = w > 0 && !cx.noiseCore ? kBehind : kRender;                                          
						if (fs > -fracRamp) {
							const double f = Smooth(-fracRamp, 0.0, fs);
							dep = Lerp(dep, (w + 1) * L.leaf - 0.1 * L.leaf, f * f);
							if (f > 0.2) nodePart = kBreak;                                               
						}
						if (chipA > 0.0) {
							const double cn = 0.5 + 0.5 * Fbm(Wp / (1.6 * chipR) + V(double(b.key % 991), -7.7, 2.1), 3);
							const double reach = chipR * (0.25 + 1.3 * cn);
							const double f = 1.0 - Smooth(0.0, reach, e);
							dep += chipA * 0.8 * chipR * cn * f * f;
						}
						bd = dep;
					} else {
						const double jn = Fbm(Wp / (1.2 * L.m + 0.5) + V(-2.2, 6.6, 0.4), 3);
						bd = base + cx.mDepth * (w == 0 ? 1.0 : 0.6) * (1.0 + 0.6 * jRough * jn);
						nodePart = kSeam;
					}
					stopFront = w == 0;
					break;
				}
				bd = std::max(bd, -0.5 * P.D);
			}
			d += core * bd;
            if (P.masonry) d += core * std::max(0.0, g.h[k] - brickH + brickD - P.D);
			if (core < 0.5) nodePart = kRender;                               
			if (!sharp && stopFront && core > 0.0 && p.renderStick > 0.0 && P.D > 0.0) {
				                                             
				const double st = Smooth(stickThr - 0.06, stickThr + 0.06, Fbm(Wp / (2.5 * L.Pv) + P.off + V(-9.0, 14.0, 3.0), 3));
				d = Lerp(d, std::min(d, P.D * (0.25 + 0.35 * std::max(0.0, Fbm(Wp / 1.7 + V(4.0, 4.0, 4.0), 2)))), st);
				if (st > 0.5) nodePart = kRender;                              
			}
			if (!sharp && sv > 0.0) d = std::max(d, rimLip);
			if (g.thick[k] > 0.0) d = std::min(d, cap * g.thick[k]);
			if (through) {
				d = (g.thick[k] > 0.0 ? g.thick[k] : 3.0 * Dtot + extent) + margin;
				g.holeF[k] = 1;
				nodePart = cx.noiseCore ? kBehind : kRender;                                               
			}
			if (d <= minCut) { outside[k] = 1; d = 0.0; nodePart = kRender; }
            if(P.volume)d=std::min(objectField.depth[k],d);
			g.d[k] = d;
			part[k] = nodePart;
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
			const V base = P.o + P.t * g.us[size_t(i)] + P.b * g.vs[size_t(j)];
			top[k] = base + P.n * (hmax + margin);
			double up = hmax + lift - g.h[k];                                
			if (!sharp && outside[k] && g.s[k] < 0.0) {
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
	                                                                       
	                                                                       
	                                                                         
	                                                                         
	const bool ringed = sharp && cx.split && P.D > 0.0;
	const double ringW = std::min(0.06 * L.m + 1e-3, 0.25 * cell);
	const ClipMode inner = ringed ? kClipInner : kClipAll;
	bool split = false;
	if (cx.split) {
		                                                                      
		                                                                  
		                                                                 
		                                                                     
		const size_t NC = size_t(g.nu) * size_t(g.nv);
		std::vector<char> cellPart(NC, kRender);
		bool any = false;
		for (int i = 0; i < g.nu; ++i)
			for (int j = 0; j < g.nv; ++j) {
				const size_t c4[4] = {g.I(i, j), g.I(i + 1, j), g.I(i, j + 1), g.I(i + 1, j + 1)};
				size_t deep = c4[0];
				bool brk = false;
				for (size_t q : c4) {
					if (g.d[q] > g.d[deep] || (g.d[q] == g.d[deep] && part[q] > part[deep])) deep = q;
					brk |= part[q] == kBreak;
				}
				char cp = brk ? char(kBreak) : part[deep];
				                                                                   
				                                                             
				                                                               
				                                      
				size_t shallow = c4[0];
				double dmax = g.d[c4[0]];
				for (size_t q : c4) {
					if (g.d[q] < g.d[shallow]) shallow = q;
					dmax = std::max(dmax, g.d[q]);
				}
				const double span = std::max(g.us[size_t(i) + 1] - g.us[size_t(i)], g.vs[size_t(j) + 1] - g.vs[size_t(j)]);
				if (dmax - g.d[shallow] > 2.0 * span)
					cp = brk ? char(kBreak) : (part[shallow] == kBehind ? char(kBehind) : char(kRender));
				if (sharp && !(g.s[c4[0]] > 0.0 || g.s[c4[1]] > 0.0 || g.s[c4[2]] > 0.0 || g.s[c4[3]] > 0.0))
					cp = 4;                                         
				cellPart[size_t(i) * size_t(g.nv) + size_t(j)] = cp;
				any |= cp != kRender && cp != 4;
			}
		if (any) {
			m::Manifold solids[4];
			bool ok = true;
			for (int pt = 0; pt < 4 && ok; ++pt) {
				std::vector<char> use(NC, 0);
				bool has = false;
				for (size_t c = 0; c < NC; ++c) if (cellPart[c] == pt) { use[c] = 1; has = true; }
				if (!has) continue;
				solids[pt] = sharp ? ClippedSlabSolid(g, top, bot, &use, inner, ringW) : MaskedSlabSolid(top, bot, g.nu, g.nv, use);
				ok = !solids[pt].IsEmpty();
			}
			if (ok) {
				                                                                
				out.cutter = std::move(solids[kRender]);
				out.breaks = std::move(solids[kBreak]);
				out.seams = std::move(solids[kSeam]);
				out.behind = std::move(solids[kBehind]);
				split = true;
			}
		}
	}
	if (!split)
		out.cutter = sharp ? ClippedSlabSolid(g, top, bot, nullptr, inner, ringW) : SlabSolid(top, bot, g.nu, g.nv);
	if (ringed && !out.cutter.IsEmpty()) out.render = ClippedSlabSolid(g, top, bot, nullptr, kClipRing, ringW);
	                                                                           
	                                                                          
	                                                                           
	if (v2 && cx.split && P.D > 0.0) {
		std::vector<V> coatBottom(N);
		for (int j=0;j<=g.nv;++j) for (int i=0;i<=g.nu;++i) {
			const size_t k=g.I(i,j);
			const V base=P.o+P.t*g.us[size_t(i)]+P.b*g.vs[size_t(j)];
			coatBottom[k]=base+P.n*(g.h[k]-std::min(g.d[k],P.D));
		}
		const m::Manifold coat=SlabSolid(top,coatBottom,g.nu,g.nv);
		if (!coat.IsEmpty() && coat.Status()==m::Manifold::Error::NoError) {
			m::Manifold* parts[]={&out.cutter,&out.breaks,&out.seams,&out.behind};
			for (m::Manifold* piece:parts) if (!piece->IsEmpty()) {
				m::Manifold skin=(*piece)^coat;
				m::Manifold rest=(*piece)-coat;
				if (skin.Status()==m::Manifold::Error::NoError && rest.Status()==m::Manifold::Error::NoError) {
					out.render=out.render+skin;
					*piece=std::move(rest);
				}
			}
		}
	}


	                                                                                        
	                                                                        
	                                                                            
	                                                                         
	                                                                           
	                                                           
	if (cx.noiseCore && cx.coreDepth > 0.0) {
		std::vector<m::CrossSection> cells;
		std::vector<double> hs;
		for (int j = 0; j < g.nv; ++j) {
			int run = -1;
			for (int i = 0; i <= g.nu; ++i) {
				const bool on = i < g.nu && coreN[g.I(i, j)] && coreN[g.I(i + 1, j)] && coreN[g.I(i, j + 1)] && coreN[g.I(i + 1, j + 1)];
				if (on) { if (run < 0) run = i; hs.push_back(g.h[g.I(i, j)]); continue; }
				if (run >= 0) cells.push_back(Rect2(g.us[size_t(run)], g.us[size_t(i)], g.vs[size_t(j)], g.vs[size_t(j + 1)]));
				run = -1;
			}
		}
		const double coreInset = std::max(1e-3, 0.02 * L.m);
		m::CrossSection foot = cells.empty() ? m::CrossSection()
			: Union2(cells);
		if (!foot.IsEmpty()) {
			                                                              
			                                                           
			const double open = std::max(0.6 * L.m, 1.5 * cell);
			foot = foot.Offset(-open, m::CrossSection::JoinType::Miter)
				.Offset(open - coreInset, m::CrossSection::JoinType::Miter).Simplify(0.1 * coreInset);
		}
		if (!foot.IsEmpty() && foot.Area() > 4.0 * cell * cell) {
			std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
			const double hp = hs[hs.size() / 2];
			const double back = P.D + cx.layers * L.leaf;                                     
			const double hTop = hp - back - 0.5 * coreSkin, hBot = hp - back - cx.coreDepth;
			const double S = cx.coreScale;
			const double A = std::min(cx.coreNoise, 0.2 * S);
			const double edge = std::max(1.5 * cell, 0.25 * S);
			const int nDiv = std::max(1, int(std::ceil((hTop - hBot) / edge)) - 1);
			m::Manifold pocket = m::Manifold::Extrude(foot.ToPolygons(), hTop - hBot, nDiv)
				.Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * hBot))
				.RefineToLength(edge);
			const V o1 = P.off + V(19.3, -8.7, 5.1), o2 = P.off + V(-4.4, 13.7, -21.2), o3 = P.off + V(8.8, 2.2, 31.9);
			pocket = pocket.Warp([&](m::vec3& q) {
				const V rel = q - P.o;
				const double uu = m::la::dot(rel, P.t), vv = m::la::dot(rel, P.b), hn = m::la::dot(rel, P.n);
				                                                                     
				int ci, cj; double ca, cc;
				const double hs0 = g.Cellof(uu, vv, ci, cj, ca, cc) ? g.Bl(g.h, ci, cj, ca, cc) : hp;
				const double fade = Smooth(0.0, 2.0 * A + 1e-9, hTop - hn);
				const V w = q / S;
				const V disp(Fbm(w + o1, 3), Fbm(w + o2, 3), Fbm(w + o3, 3));
				q = q + P.n * (hs0 - hp) + disp * (A * fade);
			});
			if (pocket.Status() == m::Manifold::Error::NoError && !pocket.IsEmpty()) {
				pocket = pocket.AsOriginal();
				m::Manifold& into = cx.split ? out.behind : out.cutter;
				into = into.IsEmpty() ? pocket : (into + pocket).AsOriginal();
			}
		}
	}

	const uint64_t nKey = uint64_t(int64_t(std::llround(P.n.x * 1000.0))) * 0x9e3779b97f4a7c15ULL
		^ uint64_t(int64_t(std::llround(P.n.y * 1000.0))) * 0xbf58476d1ce4e5b9ULL
		^ uint64_t(int64_t(std::llround(P.n.z * 1000.0))) * 0x94d049bb133111ebULL;

	                                                                                         
	                                                                          
	                                                                         
	                                                                           
	                                                                         
	                                                                           
	                                                                      
	                                                                          
	if (cx.separate) {
		std::vector<m::CrossSection> cs;
		auto zc = [&](size_t a, size_t b, m::vec2 pa, m::vec2 pb) {
			if (a > b) { std::swap(a, b); std::swap(pa, pb); }
			const double t = ClampD(g.s[a] / (g.s[a] - g.s[b]), 1e-4, 1.0 - 1e-4);
			return pa + (pb - pa) * t;
		};
		for (int j = 0; j < g.nv; ++j) {
			int run = -1;
			for (int i = 0; i <= g.nu; ++i) {
				bool in[4] = {false, false, false, false};
				size_t c[4] = {0, 0, 0, 0};
				int nin = 0;
				if (i < g.nu) {
					const int ci[4] = {i, i + 1, i + 1, i}, cj[4] = {j, j, j + 1, j + 1};
					for (int q = 0; q < 4; ++q) { c[q] = g.I(ci[q], cj[q]); in[q] = g.s[c[q]] > 0.0; nin += in[q] ? 1 : 0; }
				}
				if (nin == 4) { if (run < 0) run = i; continue; }
				if (run >= 0) cs.push_back(Rect2(g.us[size_t(run)], g.us[size_t(i)], g.vs[size_t(j)], g.vs[size_t(j + 1)]));
				run = -1;
				if (nin == 0) continue;
				const m::vec2 pc[4] = {{g.us[size_t(i)], g.vs[size_t(j)]}, {g.us[size_t(i + 1)], g.vs[size_t(j)]},
					{g.us[size_t(i + 1)], g.vs[size_t(j + 1)]}, {g.us[size_t(i)], g.vs[size_t(j + 1)]}};
				m::SimplePolygon poly;
				for (int q = 0; q < 4; ++q) {
					const int r = (q + 1) & 3;
					if (in[q]) poly.push_back(pc[q]);
					if (in[q] != in[r]) poly.push_back(zc(c[q], c[r], pc[q], pc[r]));
				}
				if (poly.size() >= 3) cs.push_back(m::CrossSection(m::Polygons{poly}, m::CrossSection::FillRule::Positive));
			}
		}
		const double ledge = std::max(1e-3, 0.3 * L.m);                                       
		m::CrossSection foot = cs.empty() ? m::CrossSection()
			: Union2(cs).Offset(-ledge, m::CrossSection::JoinType::Miter).Simplify(1e-3 * cell);
		{
			                                                                     
			std::vector<m::CrossSection> on;
			for (int j = 0; j < g.nv; ++j) {
				int run = -1;
				for (int i = 0; i <= g.nu; ++i) {
					const bool ok = i < g.nu && g.valid[g.I(i, j)] && g.valid[g.I(i + 1, j)] && g.valid[g.I(i, j + 1)] && g.valid[g.I(i + 1, j + 1)];
					if (ok) { if (run < 0) run = i; continue; }
					if (run >= 0) on.push_back(Rect2(g.us[size_t(run)], g.us[size_t(i)], g.vs[size_t(j)], g.vs[size_t(j + 1)]));
					run = -1;
				}
			}
			if (!foot.IsEmpty()) foot = on.empty() ? m::CrossSection() : (foot ^ Union2(on).Offset(-ledge, m::CrossSection::JoinType::Miter));
		}
		if (!foot.IsEmpty() && foot.Area() > 4.0 * cell * cell) {
			std::vector<double> hs;
			double thickMax = 0.0;
			for (size_t k = 0; k < N; ++k)
				if (g.s[k] > 0.0) { hs.push_back(g.h[k]); thickMax = std::max(thickMax, g.thick[k]); }
			std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end());
			const double hp = hs[hs.size() / 2];
			const double back = P.D + cx.W;                             
			const double topD = P.D > 0.0 ? P.D - std::min(0.25 * P.D, 0.2) : -margin;
			double botD = back + cx.coreDepth;
			if (P.hole && p.holeEnable) botD = std::max(botD, (thickMax > 0.0 ? thickMax : 3.0 * botD) + margin);
			const double S = cx.coreScale, A = std::min(cx.coreNoise, 0.2 * S);
			const double edge = std::max(1.5 * cell, 0.25 * S);
			const int nDiv = std::max(1, int(std::ceil((botD - topD) / edge)) - 1);
			m::Manifold pocket;
			if (p.remesh) {
				                                                                
				                                                               
				                                                                 
				                                                               
				                                
				const m::Rect fb = foot.Bounds();
				const double H = botD - topD, ov = 0.5 * edge;
				const m::Manifold prism = m::Manifold::Extrude(foot.ToPolygons(), H + 2.0 * ov, std::max(1, int(std::ceil((H + 2.0 * ov) / edge)) - 1))
					.Translate({0.0, 0.0, -ov});
				const m::Manifold box = m::Manifold::Cube({fb.max.x - fb.min.x + 2.0 * edge, fb.max.y - fb.min.y + 2.0 * edge, H})
					.Translate({fb.min.x - edge, fb.min.y - edge, 0.0}).RefineToLength(edge);
				pocket = (prism ^ box).Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * (hp - botD)));
			} else {
				pocket = m::Manifold::Extrude(foot.ToPolygons(), botD - topD, nDiv)
					.Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * (hp - botD)))
					.RefineToLength(edge);
			}
			const V o1 = P.off + V(19.3, -8.7, 5.1), o2 = P.off + V(-4.4, 13.7, -21.2), o3 = P.off + V(8.8, 2.2, 31.9);
			pocket = pocket.Warp([&](m::vec3& q) {
				const V rel = q - P.o;
				const double uu = m::la::dot(rel, P.t), vv = m::la::dot(rel, P.b), hn = m::la::dot(rel, P.n);
				int ci, cj; double ca, cc;
				const double hs0 = g.Cellof(uu, vv, ci, cj, ca, cc) ? g.Bl(g.h, ci, cj, ca, cc) : hp;
				                                                                  
				const double fade = Smooth(back, back + 2.0 * A + 1e-9, hp - hn);
				const V w = q / S;
				const V disp(Fbm(w + o1, 3), Fbm(w + o2, 3), Fbm(w + o3, 3));
				const double follow = P.masonry
                    ? Lerp(hs0 - hp, brickH - hp + P.D - brickD, Smooth(topD, back, hp - hn)) : hs0 - hp;
                q = q + P.n * follow + disp * (A * fade);
			});
			if (pocket.Status() == m::Manifold::Error::NoError && !pocket.IsEmpty()) {
				pocket = pocket.AsOriginal();
				m::Manifold& into = cx.split ? out.behind : out.cutter;
				into = into.IsEmpty() ? pocket : (into + pocket).AsOriginal();
			}

			                                                                     
			                                                                 
			                                                                  
            if (p.generatedGeometry) {
			const double pd = P.masonry ? P.masonryPlane : P.hasSelf ? P.selfD : m::la::dot(P.o, P.n) + hp;
			const uint64_t planeKey = (uint64_t(int64_t(std::llround(P.n.x * 1000.0))) * 0x2545f4914f6cdd1dULL)
				^ (uint64_t(int64_t(std::llround(P.n.y * 1000.0))) * 0x9e3779b97f4a7c15ULL)
				^ (uint64_t(int64_t(std::llround(P.n.z * 1000.0))) * 0xc2b2ae3d27d4eb4fULL)
				^ (uint64_t(int64_t(std::llround(pd / std::max(1e-6, 0.05 * L.Pv)))) * 0x165667b19e3779f9ULL);
			const m::Rect bb = foot.Bounds();
			const double lt = ClampD(p.looseTilt, 0.0, 30.0) * m::kPi / 180.0;
			const int64_t k0 = Course(L, baseV + bb.min.y) - 1, k1 = Course(L, baseV + bb.max.y) + 1;
			                                                                   
			                                                                    
			                                   
			auto cornerTrim = [&](m::Manifold x, int64_t kc, bool joint) {
				for (const Patch::Corner& c : P.corners) {
					const bool wins = ((kc % 2 + 2) % 2 == 0) == c.low;
					const double my = c.d - P.D - 0.5 * cx.W;
					const double lim = wins ? 0.5 * cx.W : -(0.5 * cx.W + (joint ? 0.0 : L.m));
					x = x.TrimByPlane(c.n * -c.sigma, -c.sigma * my - lim);
					if (x.IsEmpty()) break;
				}
				return x;
			};
			                                                                
			                                                                  
			                                                                  
			                
			const bool joints = p.jointFill && L.m > 0.0 && cx.mDepth < cx.W;
			struct JB { double u0, u1, v0, v1; bool on; };
			std::vector<std::pair<int64_t, std::vector<JB>>> rows;
			for (int64_t kc = k0; kc <= k1; ++kc) {
				if (joints) rows.push_back({kc, {}});
				Cell c = Locate(L, 0, kc, baseU + bb.min.x - L.Lp);
				for (int guard = 0; guard < 100000 && c.a < baseU + bb.max.x + L.Lp; ++guard) {
					const Cell next = Locate(L, 0, kc, c.a + c.len + 0.25 * L.m);
					const Brick& b = state(0, kc, c);
					const Cell cur = c;
					c = next;
					const double bu0 = cur.a - baseU + 0.5 * L.m, bu1 = cur.a + cur.len - baseU - 0.5 * L.m;
					const double bv0 = CourseV(L, kc) - baseV + 0.5 * L.m, bv1 = bv0 + L.Pv - L.m;
					if (joints) rows.back().second.push_back({bu0, bu1, bv0, bv1, false});
					if (b.removed && !b.pulled) continue;                            
					const m::CrossSection r = Rect2(bu0, bu1, bv0, bv1);
					const double ra = r.Area();
					if (!(ra > 0.0)) continue;
					const m::CrossSection inside = r ^ foot;
					const double ia = inside.Area();
					if (ia < 0.2 * ra) continue;                                      
					m::Manifold bm = m::Manifold::Extrude((ia > 0.999 * ra ? r : inside).ToPolygons(), cx.W).Translate({0.0, 0.0, -cx.W});
					const double cu = 0.5 * (bu0 + bu1), cv = 0.5 * (bv0 + bv1);
					if (b.fracture) {
						                                           
						const double tz = cx.boxBricks ? 0.0 : (br(b.key, 9) - 0.5) * 0.8;
						V nrm(-b.fx, -b.fy, tz);
						nrm = nrm / m::la::length(nrm);
						const V p0(cu + b.fx * b.fc, cv + b.fy * b.fc, -0.5 * cx.W);
						bm = bm.TrimByPlane(nrm, m::la::dot(nrm, p0));
						if (bm.IsEmpty() || bm.Volume() < 0.15 * ra * cx.W) continue;
					}
					const m::Box lb = bm.BoundingBox();                                                
					double push = b.push, ax = std::atan(b.tv), ay = -std::atan(b.tu);
					if (b.pulled) {
						                                          
						push = -Lerp(0.3, 1.0, br(b.key, 40)) * std::min(std::max(0.0, p.looseOutDist), 0.6 * cx.W);
						ax += (br(b.key, 41) - 0.5) * 2.0 * lt;
						ay += (br(b.key, 42) - 0.5) * 2.0 * lt;
					}
					if (ax != 0.0 || ay != 0.0)
						bm = bm.Translate({-cu, -cv, 0.5 * cx.W}).Rotate(ax * 180.0 / m::kPi, ay * 180.0 / m::kPi, 0.0)
							.Translate({cu, cv, -0.5 * cx.W});
					int ci, cj; double ca, cc;
					const double hc = P.masonry ? brickH : g.Cellof(cu, cv, ci, cj, ca, cc) ? g.Bl(g.h, ci, cj, ca, cc) : hp;
                    bm = bm.Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * (hc - brickD - push)));
                    if (P.masonry) bm = (bm.TrimByPlane(P.n, P.masonryLimit) ^ s.masonry.solid);
					if (!P.corners.empty()) {
						bm = cornerTrim(bm, kc, false);
						if (bm.IsEmpty() || bm.Volume() < 0.2 * ra * cx.W) continue;
					}
					if (bm.Status() != m::Manifold::Error::NoError || bm.IsEmpty()) continue;
					out.bricks.push_back(bm.AsOriginal());
					out.brickKeys.push_back(b.key ^ P.faceKey ^ planeKey);
					if (joints && !b.pulled) {
						JB& jb = rows.back().second.back();
						jb.on = true;
						jb.u0 = std::max(jb.u0, lb.min.x); jb.u1 = std::min(jb.u1, lb.max.x);
						jb.v0 = std::max(jb.v0, lb.min.y); jb.v1 = std::min(jb.v1, lb.max.y);
					}
				}
			}
			                                                                   
			                                                                  
			if (joints) {
				                                                                 
				                                                                  
				                                                               
				                                                 
				std::vector<m::Manifold> parts;
				auto runs = [&](const std::vector<JB>& R, double va, double vb) {
					std::vector<m::CrossSection> rc;
					for (size_t i = 0; i < R.size(); ++i) {
						if (!R[i].on) continue;
						const bool lo = i > 0 && R[i - 1].on && R[i - 1].u1 >= R[i].u0 - 1.01 * L.m;
						const bool hi = i + 1 < R.size() && R[i + 1].on && R[i + 1].u0 <= R[i].u1 + 1.01 * L.m;
						rc.push_back(Rect2(R[i].u0 - (lo ? 0.5 * L.m : 0.0), R[i].u1 + (hi ? 0.5 * L.m : 0.0), va, vb));
					}
					return Union2(rc);
				};
				for (size_t ri = 0; ri < rows.size(); ++ri) {
					const std::vector<JB>& R = rows[ri].second;
					const int64_t kc = rows[ri].first;
					std::vector<m::CrossSection> rects;
					for (const JB& jb : R) if (jb.on) rects.push_back(Rect2(jb.u0, jb.u1, jb.v0, jb.v1));
					if (rects.empty()) continue;
					const double cv0 = CourseV(L, kc) - baseV, cv1 = cv0 + L.Pv;
					                                                              
					m::CrossSection jc = runs(R, cv0 + 0.5 * L.m, cv1 - 0.5 * L.m) - Union2(rects);
					                                                                    
					if (ri + 1 < rows.size() && rows[ri + 1].first == kc + 1)
						jc = jc + (runs(R, cv1 - 0.5 * L.m, cv1 + 0.5 * L.m) ^ runs(rows[ri + 1].second, cv1 - 0.5 * L.m, cv1 + 0.5 * L.m));
					jc = jc ^ foot;
					if (jc.IsEmpty() || jc.Area() < 1e-6 * L.Pv * L.Pv) continue;
					m::Manifold mj = m::Manifold::Extrude(jc.ToPolygons(), cx.W - cx.mDepth).Translate({0.0, 0.0, -cx.W})
						.Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * ((P.masonry ? brickH : hp) - brickD)));
					if (!P.masonry) mj = mj.Warp([&](m::vec3& q) {
						const V rel = q - P.o;
						int ci, cj; double ca, cc;
						if (g.Cellof(m::la::dot(rel, P.t), m::la::dot(rel, P.b), ci, cj, ca, cc)) q = q + P.n * (g.Bl(g.h, ci, cj, ca, cc) - hp);
					});
					if (P.masonry) mj = (mj.TrimByPlane(P.n, P.masonryLimit) ^ s.masonry.solid);
                    if (!P.corners.empty()) mj = cornerTrim(mj, kc, true);
					if (mj.Status() == m::Manifold::Error::NoError && !mj.IsEmpty()) parts.push_back(mj);
				}
				if (!parts.empty()) {
					m::Manifold all = m::Manifold::BatchBoolean(parts, m::OpType::Add);
					if (all.Status() == m::Manifold::Error::NoError && !all.IsEmpty()) out.mortar = all.AsOriginal();
				}
			}
            }
		}
	}

	                                                                                          
	if (p.generatedGeometry && !cx.separate && p.looseOut > 0.0 && p.loose > 0.0) {
		std::vector<uint64_t> keys;
		for (const auto& kv : cache) if (kv.second.pulled) keys.push_back(kv.first);
		std::sort(keys.begin(), keys.end());                         
		const double lt = ClampD(p.looseTilt, 0.0, 30.0) * m::kPi / 180.0;
		for (uint64_t key : keys) {
			const Brick& b = cache[key];
			double hh = 0.0, dd = 0.0;
			bool hl = false, own = true;
			if (!g.Sample(b.cu, b.cv, hh, dd, hl, own) || !own || hl) continue;
			const double outD = std::min(std::max(0.0, p.looseOutDist), 0.6 * cx.W) * Lerp(0.3, 1.0, br(key, 40));
			const double zf = hh - P.D + outD, zb = zf - cx.W;                          
			const double x0 = -b.hx + 0.03 * L.m, x1 = b.hx - 0.03 * L.m;
			const double y0 = -b.hy - 1.3 * L.m, y1 = b.hy - 0.05 * L.m;                               
			const V C = P.o + P.t * b.cu + P.b * b.cv;
			const V pivot = C + P.b * y0 + P.n * zb;
			const double pitch = Lerp(0.3, 1.0, br(key, 41)) * lt;                  
			const double yaw = (br(key, 42) - 0.5) * lt;
			const V xs[2] = {P.t * x0, P.t * x1}, ys[2] = {P.b * y0, P.b * y1}, zs[2] = {P.n * zb, P.n * zf};
			V c[8];
			const int order[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (int f = 0; f < 2; ++f)
				for (int q = 0; q < 4; ++q) {
					V w = C + xs[order[q][0]] + ys[order[q][1]] + zs[f] - pivot;
					w = Rotate(w, P.t, pitch);
					w = Rotate(w, P.b, yaw);
					c[f * 4 + q] = pivot + w;
				}
			m::Manifold bx;
			if (useModel && modelOf(key).solid) {
				                                                                  
				bx = Instance(modelOf(key), [&](const V& q) {
					const double fx = 0.5 * q.x + 0.5, fy = 0.5 * q.y + 0.5, fz = 1.0 - q.z;                        
					auto face = [&](int f) {
						const V a = c[f * 4 + 0], bb = c[f * 4 + 1], cc = c[f * 4 + 2], dd2 = c[f * 4 + 3];
						return (a * (1 - fx) + bb * fx) * (1 - fy) + (dd2 * (1 - fx) + cc * fx) * fy;
					};
					return face(0) * (1 - fz) + face(1) * fz;
				});
			}
			if (bx.IsEmpty()) bx = Box(c);
			if (!bx.IsEmpty()) { out.bricks.push_back(std::move(bx)); out.brickKeys.push_back(key ^ nKey ^ 0x50554c4cULL); }
		}
	}

	                                                                                 
	if (p.crackEnable && P.cracks > 0 && p.crackLengthMax > 0.0 && p.crackWidth > 0.0) {
		const double uc = g.us[kMax / size_t(g.nv + 1)];
		const double vc = g.vs[kMax % size_t(g.nv + 1)];
		auto floorAt = [&](double u, double v) {                                               
			double hh, dd; bool hl, own;
			if (!g.Sample(u, v, hh, dd, hl, own)) return 0.0;
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
					if (g.SAt(nx, ny) <= 0.0) break;
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
}

                                                                                        
                                                                              
                                                                           
                                                                             
                                                                            
                                                                             

m::CrossSection Rect2(double u0, double u1, double v0, double v1)
{
	if (!(u1 > u0) || !(v1 > v0)) return m::CrossSection();
	return m::CrossSection(m::Rect(m::vec2(u0, v0), m::vec2(u1, v1)));
}

m::CrossSection Union2(std::vector<m::CrossSection>& parts)
{
	if (parts.empty()) return m::CrossSection();
	return m::CrossSection::BatchBoolean(parts, m::OpType::Add);
}

                                                                            
                                                                              
                                                                          
                                                                           
                                                                           
                                                                   
                                                                          
m::Manifold RemeshEven(const m::Manifold& in, double edge, const std::vector<m::Box>* zones, double flatCos)
{
	if (in.IsEmpty() || !(edge > 0.0)) return in;
	m::MeshGL64 mg = in.GetMeshGL64();
	const size_t np = mg.numProp;
	if (np < 3 || mg.triVerts.empty()) return in;
	std::vector<double>& prop = mg.vertProperties;
	std::vector<uint64_t> pid(prop.size() / np);
	for (size_t i = 0; i < pid.size(); ++i) pid[i] = i;
	for (size_t i = 0; i < mg.mergeFromVert.size(); ++i) pid[size_t(mg.mergeFromVert[i])] = mg.mergeToVert[i];
	for (size_t i = 0; i < pid.size(); ++i) { uint64_t r = pid[i]; for (int g = 0; g < 8 && pid[size_t(r)] != r; ++g) r = pid[size_t(r)]; pid[i] = r; }
	struct Tri { uint64_t v[3]; uint32_t run; uint64_t face; bool dead; };
	std::vector<Tri> tris;
	const size_t nRun = mg.runIndex.size() > 1 ? mg.runIndex.size() - 1 : 0;
	tris.reserve(mg.triVerts.size() / 3 * 2);
	for (size_t t = 0; t < mg.triVerts.size() / 3; ++t) {
		Tri T;
		for (int q = 0; q < 3; ++q) T.v[q] = mg.triVerts[3 * t + size_t(q)];
		T.run = 0;
		for (size_t r = 0; r < nRun; ++r) if (3 * t >= mg.runIndex[r] && 3 * t < mg.runIndex[r + 1]) { T.run = uint32_t(r); break; }
		T.face = t < mg.faceID.size() ? mg.faceID[t] : t;
		T.dead = false;
		tris.push_back(T);
	}
	auto P = [&](uint64_t v) { return V(prop[size_t(v) * np], prop[size_t(v) * np + 1], prop[size_t(v) * np + 2]); };
	auto ek = [&](uint64_t a, uint64_t b) {
		a = pid[size_t(a)]; b = pid[size_t(b)];
		if (a > b) std::swap(a, b);
		return std::make_pair(a, b);
	};
	struct PH { size_t operator()(const std::pair<uint64_t, uint64_t>& k) const { return size_t(k.first * 0x9e3779b97f4a7c15ULL ^ (k.second + 0x632be59bd9b4e019ULL)); } };
	std::unordered_map<std::pair<uint64_t, uint64_t>, std::array<int64_t, 2>, PH> em;
	auto addE = [&](int64_t t) {
		for (int q = 0; q < 3; ++q) {
			auto& e = em.try_emplace(ek(tris[size_t(t)].v[q], tris[size_t(t)].v[(q + 1) % 3]), std::array<int64_t, 2>{-1, -1}).first->second;
			if (e[0] < 0) e[0] = t; else e[1] = t;
		}
	};
	auto delE = [&](int64_t t) {
		for (int q = 0; q < 3; ++q) {
			auto it = em.find(ek(tris[size_t(t)].v[q], tris[size_t(t)].v[(q + 1) % 3]));
			if (it == em.end()) continue;
			if (it->second[0] == t) { it->second[0] = it->second[1]; it->second[1] = -1; }
			else if (it->second[1] == t) it->second[1] = -1;
			if (it->second[0] < 0) em.erase(it);
		}
	};
	for (size_t t = 0; t < tris.size(); ++t) addE(int64_t(t));
	for (const auto& kv : em) if (kv.second[1] < 0) return in;                          
	auto inZone = [&](const Tri& T) {
		if (!zones) return true;
		m::Box b;
		for (int q = 0; q < 3; ++q) b.Union(P(T.v[q]));
		for (const m::Box& z : *zones)
			if (b.min.x <= z.max.x + edge && b.max.x >= z.min.x - edge && b.min.y <= z.max.y + edge && b.max.y >= z.min.y - edge
				&& b.min.z <= z.max.z + edge && b.max.z >= z.min.z - edge) return true;
		return false;
	};
	std::vector<uint64_t> mergeFrom, mergeTo;
	auto newVert = [&](uint64_t a, uint64_t b) {
		const uint64_t nv = prop.size() / np;
		for (size_t c = 0; c < np; ++c) prop.push_back(0.5 * (prop[size_t(a) * np + c] + prop[size_t(b) * np + c]));
		pid.push_back(nv);
		return nv;
	};
	                                                               
	auto split = [&](int64_t t, int q) {
		const uint64_t a = tris[size_t(t)].v[q], b = tris[size_t(t)].v[(q + 1) % 3];
		auto it = em.find(ek(a, b));
		if (it == em.end() || it->second[1] < 0) return false;
		const int64_t u = it->second[0] == t ? it->second[1] : it->second[0];
		int qu = -1;
		for (int r = 0; r < 3; ++r)
			if (ek(tris[size_t(u)].v[r], tris[size_t(u)].v[(r + 1) % 3]) == ek(a, b)) qu = r;
		if (qu < 0) return false;
		const uint64_t ua = tris[size_t(u)].v[qu], ub = tris[size_t(u)].v[(qu + 1) % 3];                    
		const uint64_t m1 = newVert(a, b);
		uint64_t m2 = m1;
		if (ua != b || ub != a) {                                                     
			m2 = newVert(ub, ua);
			pid[size_t(m2)] = m1;
			mergeFrom.push_back(m2); mergeTo.push_back(m1);
		}
		delE(t); delE(u);
		for (int side = 0; side < 2; ++side) {
			const int64_t s0 = side == 0 ? t : u;
			const int qq = side == 0 ? q : qu;
			const uint64_t mm = side == 0 ? m1 : m2;
			Tri T = tris[size_t(s0)];
			const uint64_t x = T.v[qq], y = T.v[(qq + 1) % 3], z = T.v[(qq + 2) % 3];
			tris[size_t(s0)].v[0] = x; tris[size_t(s0)].v[1] = mm; tris[size_t(s0)].v[2] = z;
			T.v[0] = mm; T.v[1] = y; T.v[2] = z;
			tris.push_back(T);
			addE(s0);
			addE(int64_t(tris.size()) - 1);
		}
		return true;
	};
	const double e2 = edge * edge;
	for (int pass = 0; pass < 64; ++pass) {
		bool any = false;
		const size_t n = tris.size();
		for (size_t t = 0; t < n; ++t) {
			if (tris[t].dead || !inZone(tris[t])) continue;
			int best = -1;
			double bl = e2;
			for (int q = 0; q < 3; ++q) {
				const double l = m::la::length2(P(tris[t].v[(q + 1) % 3]) - P(tris[t].v[q]));
				if (l > bl) { bl = l; best = q; }
			}
			if (best >= 0 && split(int64_t(t), best)) any = true;
		}
		if (!any) break;
	}
	                               
	auto nrm = [&](uint64_t a, uint64_t b, uint64_t c) { return m::la::cross(P(b) - P(a), P(c) - P(a)); };
	auto ang = [&](uint64_t apex, uint64_t a, uint64_t b) {
		const V x = P(a) - P(apex), y = P(b) - P(apex);
		return std::atan2(m::la::length(m::la::cross(x, y)), m::la::dot(x, y));
	};
	for (int pass = 0; pass < 32; ++pass) {
		bool any = false;
		for (size_t t = 0; t < tris.size(); ++t) {
			if (!inZone(tris[t])) continue;
			for (int q = 0; q < 3; ++q) {
				const uint64_t a = tris[t].v[q], b = tris[t].v[(q + 1) % 3], c = tris[t].v[(q + 2) % 3];
				auto it = em.find(ek(a, b));
				if (it == em.end() || it->second[1] < 0) continue;
				const int64_t u = it->second[0] == int64_t(t) ? it->second[1] : it->second[0];
				const Tri& U = tris[size_t(u)];
				if (U.run != tris[t].run) continue;
				int qu = -1;
				for (int r = 0; r < 3; ++r) if (U.v[r] == b && U.v[(r + 1) % 3] == a) qu = r;
				if (qu < 0) continue;                           
				const uint64_t d = U.v[(qu + 2) % 3];
				if (pid[size_t(c)] == pid[size_t(d)] || em.count(ek(c, d))) continue;
				const V n1 = nrm(a, b, c), n2 = nrm(b, a, d);
				const double l1 = m::la::length(n1), l2 = m::la::length(n2);
				if (!(l1 > 0.0 && l2 > 0.0) || m::la::dot(n1, n2) < flatCos * l1 * l2) continue;
				if (ang(c, a, b) + ang(d, b, a) <= m::kPi + 1e-9) continue;
				const V f1 = nrm(c, a, d), f2 = nrm(d, b, c);
				if (m::la::dot(f1, n1) <= 1e-12 * l1 * l1 || m::la::dot(f2, n1) <= 1e-12 * l1 * l1) continue;
				delE(int64_t(t)); delE(u);
				tris[t].v[0] = c; tris[t].v[1] = a; tris[t].v[2] = d;
				tris[size_t(u)].v[0] = d; tris[size_t(u)].v[1] = b; tris[size_t(u)].v[2] = c;
				tris[size_t(u)].face = tris[t].face;
				addE(int64_t(t)); addE(u);
				any = true;
				break;
			}
		}
		if (!any) break;
	}
	                          
	std::vector<size_t> order(tris.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return tris[x].run < tris[y].run; });
	m::MeshGL64 out;
	out.numProp = mg.numProp;
	out.vertProperties = prop;
	out.tolerance = mg.tolerance;
	out.mergeFromVert = mg.mergeFromVert;
	out.mergeToVert = mg.mergeToVert;
	for (size_t i = 0; i < mergeFrom.size(); ++i) { out.mergeFromVert.push_back(mergeFrom[i]); out.mergeToVert.push_back(mergeTo[i]); }
	out.runOriginalID = mg.runOriginalID;
	out.runTransform = mg.runTransform;
	out.runIndex.assign(nRun + 1, 0);
	const bool faces = !mg.faceID.empty();
	for (size_t i : order) {
		for (int q = 0; q < 3; ++q) out.triVerts.push_back(tris[i].v[q]);
		if (faces) out.faceID.push_back(tris[i].face);
		if (nRun) out.runIndex[size_t(tris[i].run) + 1] += 3;
	}
	for (size_t r = 0; r < nRun; ++r) out.runIndex[r + 1] += out.runIndex[r];
	if (!nRun) out.runIndex.clear();
	m::Manifold res(out);
	if (res.Status() != m::Manifold::Error::NoError || res.IsEmpty()) return in;
	return res;
}

                                                                                         
m::Manifold Sweep(const Patch& P, const m::CrossSection& cs, double h0, double h1)
{
	if (cs.IsEmpty() || !(h1 > h0)) return m::Manifold();
	m::Manifold e = m::Manifold::Extrude(cs.ToPolygons(), h1 - h0);
	return e.Transform(m::mat3x4(P.t, P.b, P.n, P.o + P.n * h0));
}

void BuildPatchLowPoly(const Ctx& cx, const Patch& P, Out& out)
{
	const BrickDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Lattice& L = cx.L;
	const double Dtot = P.D + double(cx.layers) * L.leaf + cx.W;
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const double ampOut = jag * 0.5 * P.Rn;
	const double featOut = 0.55 * P.Rn * std::max(0.05, p.outlineScale);
	const double pad = 0.6 * ampOut + 0.08 * P.R + 1e-3;
	const double u0 = P.u0 - pad, u1 = P.u1 + pad, v0 = P.v0 - pad, v1 = P.v1 + pad;
	const double extent = std::max(u1 - u0, v1 - v0);
	if (!(extent > 0.0)) return;
	const double baseU = m::la::dot(P.o, P.t), baseV = m::la::dot(P.o, P.b);

	                                                                    
	const double cell = std::max(extent / (60.0 * p.detail), extent / 400.0);
	const int nu = std::max(4, int(std::ceil((u1 - u0) / cell)));
	const int nv = std::max(4, int(std::ceil((v1 - v0) / cell)));
	const double cu = (u1 - u0) / nu, cv = (v1 - v0) / nv;
	const size_t N = size_t(nu) * size_t(nv);
	std::vector<double> sv(N, -1.0), hv(N, 0.0), tv(N, -1.0);
	std::vector<char> ok(N, 0);
	const double Hs = 0.3 * extent + Dtot + 1e-3;
	const double drop = 0.6 * extent + 2.0 * Dtot;
	double sMax = -1e300;
	size_t kMax = 0;
	for (int i = 0; i < nu; ++i)
		for (int j = 0; j < nv; ++j) {
			const size_t k = size_t(i) * size_t(nv) + size_t(j);
			const double u = u0 + (i + 0.5) * cu, v = v0 + (j + 0.5) * cv;
			const V org = P.o + P.t * u + P.b * v + P.n * Hs;
			double dist = 0.0;
			int32_t poly = -1;
			if (!s.Ray(org, -P.n, Hs + drop, dist, poly) || m::la::dot(s.polyN[size_t(poly)], P.n) < 0.3 || s.Avoided(poly)) {
				sv[k] = -cell;
				continue;
			}
			hv[k] = Hs - dist;
			ok[k] = 1;
			const V S = P.o + P.t * u + P.b * v + P.n * hv[k];
			double th = 0.0;
			int32_t tp = -1;
			if (s.Ray(S - P.n * (1e-5 * s.diag), -P.n, 2.0 * s.diag, th, tp)) tv[k] = th;
			sv[k] = RawField(P, u, v) + ampOut * Fbm(S / featOut + P.off, 5);
			if (sv[k] > sMax) { sMax = sv[k]; kMax = k; }
		}
	if (sMax < 2.0 * cell) return;
	                                                                        
	                                             
	double hp = 0.0, thick = -1.0;
	{
		std::vector<double> hs, ts;
		for (size_t k = 0; k < N; ++k)
			if (ok[k] && sv[k] > 0.0) { hs.push_back(hv[k]); if (tv[k] > 0.0) ts.push_back(tv[k]); }
		if (!hs.empty()) { std::nth_element(hs.begin(), hs.begin() + long(hs.size() / 2), hs.end()); hp = hs[hs.size() / 2]; }
		if (!ts.empty()) { std::nth_element(ts.begin(), ts.begin() + long(ts.size() / 2), ts.end()); thick = ts[ts.size() / 2]; }
	}
	auto sAt = [&](double u, double v) {
		const double fu = (u - u0) / cu - 0.5, fv = (v - v0) / cv - 0.5;
		if (fu < 0.0 || fv < 0.0 || fu > nu - 1 || fv > nv - 1) return -1e9;
		const int i = std::min(nu - 2, int(fu)), j = std::min(nv - 2, int(fv));
		const double a = fu - i, c = fv - j;
		auto at = [&](int ii, int jj) { return sv[size_t(ii) * size_t(nv) + size_t(jj)]; };
		return (at(i, j) * (1 - a) + at(i + 1, j) * a) * (1 - c) + (at(i, j + 1) * (1 - a) + at(i + 1, j + 1) * a) * c;
	};

	                                       
	m::CrossSection S;
	{
		std::vector<m::CrossSection> cells;
		for (int i = 0; i < nu; ++i) {
			int j = 0;
			while (j < nv) {                                            
				if (sv[size_t(i) * size_t(nv) + size_t(j)] <= 0.0) { ++j; continue; }
				int e = j;
				while (e < nv && sv[size_t(i) * size_t(nv) + size_t(e)] > 0.0) ++e;
				cells.push_back(Rect2(u0 + i * cu, u0 + (i + 1) * cu, v0 + j * cv, v0 + e * cv));
				j = e;
			}
		}
		S = Union2(cells).Simplify(0.01 * cell);
	}
	if (S.IsEmpty()) return;

	                                                                                     
	const double brickAmp = ClampD(p.breakJagged, 0.0, 2.0) * 1.3 * L.Pv;
	std::vector<double> thr(size_t(cx.layers + 1), 1e300);
	for (int w = 0; w < cx.layers; ++w)
		thr[size_t(w)] = std::min((1.0 - ClampD(p.breakSize, 0.0, 1.0) + w * ClampD(p.layerInset, 0.0, 1.0)) * sMax, 0.97 * sMax);
	double thrH = 1e300;
	if (P.hole && p.holeEnable) {
		thrH = (1.0 - ClampD(p.holeSize, 0.0, 1.0)) * sMax;
		if (cx.layers > 0) thrH = std::min(std::max(thrH, thr[size_t(cx.layers - 1)] + 0.05 * sMax), 0.98 * sMax);
	}
	const uint64_t bseed = cx.seed ^ 0x425249434bULL;                                               
	auto br = [&](uint64_t key, uint64_t ch) { return Random01(bseed, key, ch); };
	const double eps = 0.01 * L.m + 1e-4;
	const double hy = 0.5 * (L.Pv - L.m);
	struct BB { double cu, cv, hx; uint64_t key; };
	auto geomOf = [&](int w, int64_t k, const Cell& c) {
		return BB{c.a + 0.5 * c.len - baseU, CourseV(L, k) + 0.5 * L.Pv - baseV, 0.5 * (c.len - L.m), BrickKey(w, k, c.j)};
	};
	auto exposed = [&](const BB& b) {
		return std::min(std::min(sAt(b.cu - b.hx, b.cv - hy), sAt(b.cu + b.hx, b.cv - hy)),
			std::min(sAt(b.cu - b.hx, b.cv + hy), sAt(b.cu + b.hx, b.cv + hy))) > 0.0;
	};
	auto removedAt = [&](int w, int64_t k, double U) {
		if (w >= cx.layers) return false;
		const BB b = geomOf(w, k, Locate(L, w, k, U));
		if (w == 0 && !exposed(b)) return false;
		if (sAt(b.cu, b.cv) > thr[size_t(w)] + (br(b.key, 1) - 0.5) * 2.0 * brickAmp) return true;
		return w == 0 && br(b.key, 2) < ClampD(p.missing, 0.0, 1.0);
	};

	const int64_t k0 = Course(L, baseV + v0), k1 = Course(L, baseV + v1);
	if (k1 - k0 > 20000) return;
	std::vector<m::CrossSection> R(size_t(cx.layers + 1));                                
	std::vector<m::CrossSection> holeParts, pockets[3];
	struct Pull { BB b; };
	std::vector<Pull> pulls;
	double holeThick = -1.0;
	for (int w = 0; w <= cx.layers; ++w) {
		std::vector<m::CrossSection> gone;
		for (int64_t k = k0; k <= k1; ++k) {
			const double vb = CourseV(L, k) - baseV;
			Cell c = Locate(L, w, k, baseU + u0);
			double runA = 0.0;
			bool inRun = false;
			auto close = [&](double end) {
				if (inRun) gone.push_back(Rect2(runA - baseU - eps, end - baseU + eps, vb - eps, vb + L.Pv + eps));
				inRun = false;
			};
			for (int guard = 0; guard < 100000 && c.a < baseU + u1; ++guard) {
				const BB b = geomOf(w, k, c);
				const double Uc = c.a + 0.5 * c.len;
				bool gn = false;
				if (w == cx.layers) {
					                                                              
					if (P.hole && p.holeEnable && sAt(b.cu, b.cv) > thrH + (br(b.key, 30) - 0.5) * 2.0 * brickAmp) {
						holeParts.push_back(Rect2(c.a - baseU - eps, c.a + c.len - baseU + eps, vb - eps, vb + L.Pv + eps));
						const V Q = P.o + P.t * b.cu + P.b * b.cv + P.n * hp;
						double th = 0.0;
						int32_t tp = -1;
						if (s.Ray(Q - P.n * (1e-4 * s.diag), -P.n, 2.0 * s.diag, th, tp)) holeThick = std::max(holeThick, th);
					}
				} else {
					gn = removedAt(w, k, Uc);
					if (!gn && (w > 0 || exposed(b))) {
						const bool left = removedAt(w, k, c.a - 0.25 * L.m), right = removedAt(w, k, c.a + c.len + 0.25 * L.m);
						if ((left || right) && br(b.key, 6) < ClampD(p.broken, 0.0, 1.0)) {
							                                                       
							const bool toL = left && (!right || br(b.key, 10) < 0.5);
							const double f = Lerp(0.3, 0.6, br(b.key, 8)) * c.len;
							if (toL) gone.push_back(Rect2(c.a - baseU - eps, c.a + f - baseU, vb - eps, vb + L.Pv + eps));
							else gone.push_back(Rect2(c.a + c.len - f - baseU, c.a + c.len - baseU + eps, vb - eps, vb + L.Pv + eps));
						} else if (w == 0 && br(b.key, 20) < ClampD(p.loose, 0.0, 1.0)) {
							if (br(b.key, 21) < ClampD(p.looseOut, 0.0, 1.0) && p.looseOutDist > 0.0 && !removedAt(0, k - 1, Uc)) {
								gn = true;                                                    
								pulls.push_back({b});
							} else {
								const int bin = std::min(2, int(br(b.key, 22) * 3.0));
								pockets[bin].push_back(Rect2(b.cu - b.hx, b.cu + b.hx, b.cv - hy, b.cv + hy));
							}
						}
					}
				}
				if (gn && !inRun) { inRun = true; runA = c.a; }
				if (!gn) close(c.a);
				c = Locate(L, w, k, c.a + c.len + 0.25 * L.m);
			}
			close(c.a);
		}
		if (w < cx.layers) {
			m::CrossSection Rw = Union2(gone);
			if (w > 0) Rw = Rw ^ R[size_t(w - 1)];                                                        
			R[size_t(w)] = Rw;
		}
	}

	                                                                                    
	const double margin = 2.0 * L.Pv + P.D;
	const double top = hp + margin;
	const double cap = (p.thicknessCap > 0.0 && thick > 0.0) ? p.thicknessCap * thick : 1e300;
	auto depthTo = [&](double d) { return hp - std::min(d, cap); };
	std::vector<m::Manifold> parts;
    std::vector<m::Manifold> regions[4];
	auto push = [&](m::Manifold mf, int region = 0) { if (!mf.IsEmpty()) { if(cx.split) regions[region].push_back(mf); parts.push_back(std::move(mf)); } };
	if (P.D > 0.0) push(Sweep(P, S, depthTo(P.D), top));
	                                                   
	auto joints = [&](int w, const m::CrossSection& vis) {
		if (vis.IsEmpty() || L.m <= 0.0 || cx.mDepth <= 0.0) return m::CrossSection();
		const m::Rect bb = vis.Bounds();
		std::vector<m::CrossSection> js;
		const int64_t ka = Course(L, baseV + bb.min.y) - 1, kb = Course(L, baseV + bb.max.y) + 1;
		if (kb - ka > 20000) return m::CrossSection();
		for (int64_t k = ka; k <= kb; ++k) {
			const double vb = CourseV(L, k) - baseV;
			js.push_back(Rect2(bb.min.x, bb.max.x, vb - 0.5 * L.m, vb + 0.5 * L.m));
			Cell c = Locate(L, w, k, baseU + bb.min.x);
			for (int guard = 0; guard < 100000 && c.a < baseU + bb.max.x + L.m; ++guard) {
				js.push_back(Rect2(c.a - baseU - 0.5 * L.m, c.a - baseU + 0.5 * L.m, vb, vb + L.Pv));
				c = Locate(L, w, k, c.a + c.len + 0.25 * L.m);
			}
		}
		return Union2(js) ^ vis;
	};
	{
		m::CrossSection vis0 = cx.layers > 0 ? S - R[0] : S;
		push(Sweep(P, joints(0, vis0), depthTo(P.D + cx.mDepth), top), 2);
	}
	const double pushD[3] = {0.35, 0.65, 1.0};
	for (int bin = 0; bin < 3; ++bin)
		if (!pockets[bin].empty()) push(Sweep(P, Union2(pockets[bin]), depthTo(P.D + pushD[bin] * std::max(0.0, p.looseDepth)), top), 1);
	m::CrossSection H = Union2(holeParts);
	if (!H.IsEmpty()) H = cx.layers > 0 ? (H ^ R[size_t(cx.layers - 1)]) : (H ^ S);
	for (int w = 0; w < cx.layers; ++w) {
		if (R[size_t(w)].IsEmpty()) break;
		push(Sweep(P, R[size_t(w)], depthTo(P.D + (w + 1) * L.leaf), top), 3);
		m::CrossSection vis = w + 1 < cx.layers ? R[size_t(w)] - R[size_t(w + 1)] : R[size_t(w)] - H;
		push(Sweep(P, joints(w + 1, vis), depthTo(P.D + (w + 1) * L.leaf + 0.6 * cx.mDepth), top), 2);
	}
	if (!H.IsEmpty() && holeThick > 0.0) push(Sweep(P, H, hp - holeThick - 0.5 * margin, top), 3);
	if (!parts.empty()) {
        if(!cx.split) out.cutter = m::Manifold::BatchBoolean(parts,m::OpType::Add);
        else {
                                                                              
                                                                                     
            m::Manifold covered, tagged[4];
            for(int k=3;k>=0;--k) if(!regions[k].empty()) {
                m::Manifold whole=m::Manifold::BatchBoolean(regions[k],m::OpType::Add);
                tagged[k]=covered.IsEmpty()?whole:whole-covered;
                covered=covered.IsEmpty()?whole:covered+whole;
            }
            if (p.sharpEdge && p.profile != 1 && P.D > 0.0) {
                                                                       
                const double hair = std::min(0.25 * P.D, 0.1 * L.m + 1e-3);
                m::Manifold render = Sweep(P, S, depthTo(P.D - hair), top);
                if (!render.IsEmpty()) {
                    for (m::Manifold& t : tagged) if (!t.IsEmpty()) t = (t - render).AsOriginal();
                    out.render = render.AsOriginal();
                }
            }
            out.cutter=std::move(tagged[0]);out.breaks=std::move(tagged[1]);
            out.seams=std::move(tagged[2]);out.behind=std::move(tagged[3]);
        }
    }

	                                                                                            
	const uint64_t nKey = uint64_t(int64_t(std::llround(P.n.x * 1000.0))) * 0x9e3779b97f4a7c15ULL
		^ uint64_t(int64_t(std::llround(P.n.y * 1000.0))) * 0xbf58476d1ce4e5b9ULL
		^ uint64_t(int64_t(std::llround(P.n.z * 1000.0))) * 0x94d049bb133111ebULL;
	const double lt = ClampD(p.looseTilt, 0.0, 30.0) * m::kPi / 180.0;
	if (p.generatedGeometry) for (const Pull& pl : pulls) {
		const BB& b = pl.b;
		const uint64_t key = b.key;
		const double outD = std::min(std::max(0.0, p.looseOutDist), 0.6 * cx.W) * Lerp(0.3, 1.0, br(key, 40));
		const double zf = hp - P.D + outD, zb = zf - cx.W;
		const double x0 = -b.hx + 0.03 * L.m, x1 = b.hx - 0.03 * L.m;
		const double y0 = -hy - 1.3 * L.m, y1 = hy - 0.05 * L.m;                               
		const V C = P.o + P.t * b.cu + P.b * b.cv;
		const V pivot = C + P.b * y0 + P.n * zb;
		const double pitch = Lerp(0.3, 1.0, br(key, 41)) * lt;
		const double yaw = (br(key, 42) - 0.5) * lt;
		const V xs[2] = {P.t * x0, P.t * x1}, ys[2] = {P.b * y0, P.b * y1}, zs[2] = {P.n * zb, P.n * zf};
		V c[8];
		const int order[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		for (int f = 0; f < 2; ++f)
			for (int q = 0; q < 4; ++q) {
				V w = C + xs[order[q][0]] + ys[order[q][1]] + zs[f] - pivot;
				w = Rotate(w, P.t, pitch);
				w = Rotate(w, P.b, yaw);
				c[f * 4 + q] = pivot + w;
			}
		m::Manifold bx = Box(c);
		if (!bx.IsEmpty()) { out.bricks.push_back(std::move(bx)); out.brickKeys.push_back(key ^ nKey ^ 0x50554c4cULL); }
	}

	                                                                                              
	if (p.crackEnable && P.cracks > 0 && p.crackLengthMax > 0.0 && p.crackWidth > 0.0 && L.m > 0.0) {
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed ^ 0x435241434bULL, P.id, ch); };
		const double ucn = u0 + (double(kMax / size_t(nv)) + 0.5) * cu, vcn = v0 + (double(kMax % size_t(nv)) + 0.5) * cv;
		std::vector<m::CrossSection> segs;
		for (int ci = 0; ci < P.cracks; ++ci) {
			const uint64_t ch = 200 + uint64_t(ci) * 16;
			const double phi = 2.0 * m::kPi * (ci + 0.8 * rnd(ch)) / P.cracks;
			const double du = std::cos(phi), dv = std::sin(phi);
			double u = ucn, v = vcn;
			bool found = false;
			for (int st = 0; st < 4 * (nu + nv); ++st) {
				u += du * 0.5 * cell; v += dv * 0.5 * cell;
				if (sAt(u, v) <= 0.0) { found = true; break; }
			}
			if (!found) continue;
			const double len = Lerp(p.crackLengthMin, p.crackLengthMax, std::pow(rnd(ch + 5), 1.4));
			const double w0 = p.crackWidth * Lerp(0.6, 1.3, rnd(ch + 6));
			const double su = du >= 0.0 ? 1.0 : -1.0, sz = dv >= 0.0 ? 1.0 : -1.0;
			const double slope = std::abs(dv) / std::max(1e-3, std::abs(du));
			const double kf = ClampD(L.Pv / (slope * L.Lp) - 0.5, 0.0, 6.0);
			                                                    
			double U = u + baseU, Vj;
			{
				const int64_t kc = Course(L, v + baseV);
				Vj = sz > 0.0 ? CourseV(L, kc + 1) : CourseV(L, kc);
			}
			double done = 0.0;
			auto seg = [&](double ua, double ub, double va, double vb2) {
				const double hw = 0.5 * std::max(0.05, w0 * (1.0 - 0.75 * ClampD(done / len, 0.0, 1.0)));
				segs.push_back(Rect2(std::min(ua, ub) - baseU - hw, std::max(ua, ub) - baseU + hw,
					std::min(va, vb2) - baseV - hw, std::max(va, vb2) - baseV + hw));
			};
			for (int it = 0; it < 2000 && done < len; ++it) {
				const int64_t kn = sz > 0.0 ? Course(L, Vj + 0.5 * L.Pv) : Course(L, Vj - 0.5 * L.Pv);
				Cell c = Locate(L, 0, kn, U);
				double hj = su > 0.0 ? c.a + c.len : c.a;                           
				const int extra = int(std::floor(kf + rnd(ch + 100 + uint64_t(it) % 50)));
				for (int e = 0; e < extra; ++e) {
					c = Locate(L, 0, kn, su > 0.0 ? hj + 0.25 * L.m : hj - 0.25 * L.m);
					hj = su > 0.0 ? c.a + c.len : c.a;
				}
				seg(U, hj, Vj, Vj);
				done += std::abs(hj - U);
				U = hj;
				seg(U, U, Vj, Vj + sz * L.Pv);
				done += L.Pv;
				Vj += sz * L.Pv;
				if (sAt(U - baseU, Vj - baseV) > 0.0) break;                                
				const V Q = P.o + P.t * (U - baseU) + P.b * (Vj - baseV) + P.n * (hp + Hs);
				double dd = 0.0;
				int32_t pq = -1;
				if (!s.Ray(Q, -P.n, Hs + drop, dd, pq) || s.Avoided(pq)) break;                  
			}
		}
		if (!segs.empty()) {
			const double dep = std::max(0.05, p.crackDepth);
			m::Manifold cr = Sweep(P, Union2(segs), depthTo(dep), top);
			if (!cr.IsEmpty()) out.cracks.push_back(std::move(cr));
		}
	}
}

}               

namespace {
                                                                            
                                                                           
                                      
struct SharpEdges {
	struct E { V a, b; int32_t p1, p2; };
	std::vector<E> e;
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
		const double tol = 1e-6 * s.diag;
		std::map<std::tuple<int64_t, int64_t, int64_t>, int32_t> weld;
		std::vector<int32_t> wid(size_t(v.npts));
		for (int32_t i = 0; i < v.npts; ++i) {
			const V& q = v.pts[i];
			const auto k = std::make_tuple(int64_t(std::llround(q.x / tol)), int64_t(std::llround(q.y / tol)), int64_t(std::llround(q.z / tol)));
			wid[size_t(i)] = weld.emplace(k, i).first->second;
		}
		std::map<std::pair<int32_t, int32_t>, std::pair<int32_t, int32_t>> edges;
		auto add = [&](int32_t a, int32_t b, int32_t poly) {
			a = wid[size_t(a)]; b = wid[size_t(b)];
			if (a == b) return;
			auto it = edges.emplace(std::make_pair(std::min(a, b), std::max(a, b)), std::make_pair(poly, -1)).first;
			if (it->second.first != poly && it->second.second < 0) it->second.second = poly;
		};
		for (int32_t i = 0; i < v.npolys; ++i) {
			const ProjectDamagePoly& q = v.polys[i];
			if (q.c == q.d) { add(q.a, q.b, i); add(q.b, q.c, i); add(q.c, q.a, i); }
			else { add(q.a, q.b, i); add(q.b, q.c, i); add(q.c, q.d, i); add(q.d, q.a, i); }
		}
		for (const auto& kv : edges) {
			const auto& f = kv.second;
			if (f.second < 0) continue;
			if (m::la::dot(s.polyN[size_t(f.first)], s.polyN[size_t(f.second)]) > 0.906) continue;                  
			const V a = v.pts[kv.first.first], b = v.pts[kv.first.second];
			const int32_t id = int32_t(e.size());
			e.push_back({a, b, f.first, f.second});
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
	                      
	std::vector<int32_t> Near(const V& p, double r) const
	{
		std::vector<int32_t> ids;
		if (e.empty()) return ids;
		const int64_t rings = std::min<int64_t>(40, int64_t(std::ceil(r / cell)));
		const int64_t cx0 = int64_t(std::floor(p.x / cell)), cy0 = int64_t(std::floor(p.y / cell)), cz0 = int64_t(std::floor(p.z / cell));
		for (int64_t dx = -rings; dx <= rings; ++dx)
			for (int64_t dy = -rings; dy <= rings; ++dy)
				for (int64_t dz = -rings; dz <= rings; ++dz) {
					auto it = grid.find(Key(cx0 + dx, cy0 + dy, cz0 + dz));
					if (it == grid.end()) continue;
					for (int32_t id : it->second) ids.push_back(id);
				}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		std::vector<int32_t> keep;
		for (int32_t id : ids) {
			const V& a = e[size_t(id)].a;
			const V ab = e[size_t(id)].b - a;
			const double l2 = m::la::dot(ab, ab);
			const double t = l2 > 0.0 ? ClampD(m::la::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
			if (m::la::length(p - (a + ab * t)) <= r) keep.push_back(id);
		}
		return keep;
	}
};

                                                                               
                                                                         
                                                                              
                                                      
                                                                              
                                                                            
                                                                              
                                                                  
void AddCornerFaces(const Ctx& cx, const Surf& s, std::vector<Patch>& patches)
{
	const BrickDamageParams& p = *cx.p;
	const Lattice& L = cx.L;
	SharpEdges se;
	se.Build(s, std::max(1e-3, p.sizeMax));
	if (se.e.empty()) return;
	const size_t n0 = patches.size();
	for (size_t pi = 0; pi < n0; ++pi) {
		Patch& P = patches[pi];
        if (P.masonry) continue;
		const double ext = std::max(std::max(std::abs(P.u0), std::abs(P.u1)), std::max(std::abs(P.v0), std::abs(P.v1)));
		const double reach = 1.3 * ext + P.D + cx.W + 2.0 * L.m;
		const std::vector<int32_t> near = se.Near(P.o, reach);
		if (near.empty()) continue;
		struct Face { V n; double d; bool avoided; };
		struct Adj { int f1, f2; V a, b; bool convex; };
		std::vector<Face> faces;
		std::vector<Adj> adj;
		const double dTol = 1e-3 * reach + 1e-6;
		auto faceOf = [&](int32_t poly, const V& onIt) {
			const V& n = s.polyN[size_t(poly)];
			const double d = m::la::dot(n, onIt);
			for (size_t f = 0; f < faces.size(); ++f)
				if (m::la::dot(faces[f].n, n) > 0.9995 && std::abs(faces[f].d - d) < dTol) {
					faces[f].avoided = faces[f].avoided || s.Avoided(poly);
					return int(f);
				}
			faces.push_back({n, d, s.Avoided(poly)});
			return int(faces.size()) - 1;
		};
		const MeshView& mv = *s.v;
		auto centroid = [&](int32_t poly) {
			const ProjectDamagePoly& q = mv.polys[poly];
			return q.c == q.d ? (mv.pts[q.a] + mv.pts[q.b] + mv.pts[q.c]) / 3.0
				: (mv.pts[q.a] + mv.pts[q.b] + mv.pts[q.c] + mv.pts[q.d]) / 4.0;
		};
		for (int32_t id : near) {
			const SharpEdges::E& E = se.e[size_t(id)];
			const int f1 = faceOf(E.p1, E.a), f2 = faceOf(E.p2, E.a);
			if (f1 == f2) continue;
			                                                    
			const bool convex = m::la::dot(faces[size_t(f2)].n, centroid(E.p1) - E.a) < 0.0;
			adj.push_back({f1, f2, E.a, E.b, convex});
		}
		                       
		int main = -1;
		double best = 0.9;
		for (size_t f = 0; f < faces.size(); ++f) {
			const double dn = m::la::dot(faces[f].n, P.n);
			if (dn > best + 1e-9 || (main >= 0 && dn > best - 1e-9 && std::abs(faces[f].d - m::la::dot(faces[f].n, P.o)) < std::abs(faces[size_t(main)].d - m::la::dot(faces[size_t(main)].n, P.o)))) {
				best = dn; main = int(f);
			}
		}
		if (main < 0) continue;
		const Face FM = faces[size_t(main)];
		                                                                     
		                                                               
		const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
		if (std::abs(m::la::dot(FM.n, up)) > 0.7) continue;
		                                                                  
		std::vector<int> act{main};
		std::vector<std::pair<V, V>> edgeOf{{V(0.0), V(0.0)}};                      
		for (const Adj& a : adj) {
			const int other = a.f1 == main ? a.f2 : (a.f2 == main ? a.f1 : -1);
			if (other < 0 || faces[size_t(other)].avoided || std::abs(m::la::dot(faces[size_t(other)].n, up)) > 0.7) continue;
			if (std::find(act.begin(), act.end(), other) != act.end()) continue;
			const double c = m::la::dot(FM.n, faces[size_t(other)].n);
			if (c > 0.906 || c < -0.866) continue;
			act.push_back(other);
			edgeOf.push_back({a.a, Unit(a.b - a.a, V(0, 0, 1))});
		}
		if (act.size() < 2) continue;
		                                                                            
		if (m::la::dot(P.n, FM.n) < 0.99999) {
			const V ax = m::la::cross(P.n, FM.n);
			const double an = std::atan2(m::la::length(ax), m::la::dot(P.n, FM.n));
			const V axu = Unit(ax, AnyPerp(P.n));
			P.n = FM.n; P.t = Unit(Rotate(P.t, axu, an), P.t); P.b = m::la::cross(P.n, P.t);
		}
		std::vector<Patch> made{P};
		std::vector<int> fidx{main};
		for (size_t i = 1; i < act.size(); ++i) {
			const Face& F = faces[size_t(act[i])];
			const V e = edgeOf[i].second;
			const V c0 = edgeOf[i].first;
			const double an = std::atan2(m::la::dot(m::la::cross(FM.n, F.n), e), m::la::dot(FM.n, F.n));
			if (m::la::dot(Rotate(FM.n, e, an), F.n) < 0.999) continue;                      
			Patch Q = P;
			Q.n = F.n;
			Q.t = Rotate(P.t, e, an);
			Q.b = Rotate(P.b, e, an);
			Q.o = c0 + Rotate(P.o - c0, e, an);
			Q.cracks = 0;
			Q.faceKey = (uint64_t(int64_t(std::llround(F.n.x * 1000.0))) * 0x9e3779b97f4a7c15ULL
				^ uint64_t(int64_t(std::llround(F.n.y * 1000.0))) * 0xbf58476d1ce4e5b9ULL
				^ uint64_t(int64_t(std::llround(F.n.z * 1000.0))) * 0x94d049bb133111ebULL) | 1ULL;
			made.push_back(Q);
			fidx.push_back(act[i]);
		}
		if (made.size() < 2) continue;
		for (size_t x = 0; x < made.size(); ++x) {
			Patch& Q = made[x];
			const Face& FX = faces[size_t(fidx[x])];
			Q.hasSelf = true;
			Q.selfD = FX.d;
			Q.corners.clear();
			for (size_t y = 0; y < made.size(); ++y) {
				if (y == x) continue;
				const Adj* hit = nullptr;
				for (const Adj& a : adj)
					if ((a.f1 == fidx[x] && a.f2 == fidx[y]) || (a.f1 == fidx[y] && a.f2 == fidx[x])) { hit = &a; break; }
				if (!hit) continue;
				const Face& FY = faces[size_t(fidx[y])];
				Patch::Corner c;
				c.n = FY.n; c.d = FY.d;
				c.sigma = hit->convex ? 1.0 : -1.0;
				const double cs = m::la::dot(FX.n, FY.n);
				c.sinA = std::sqrt(std::max(0.0, 1.0 - cs * cs));
				c.low = x < y;
				Q.corners.push_back(c);
			}
			                                                                       
			                                                              
			Q.hasLat = true;
			Q.latU = m::la::dot(made[0].o, made[0].t);                                                 
			Q.latV = m::la::dot(made[0].o, made[0].b);
			for (const Patch::Corner& c : Q.corners) {
				const double df = c.sigma * m::la::dot(Q.t, c.n);
				if (std::abs(df) < 0.5) continue;
				const double hPlane = Q.selfD - m::la::dot(Q.o, Q.n);
				const V X0 = Q.o + Q.n * (hPlane - P.D - 0.5 * cx.W);
				const double my = c.d - P.D - 0.5 * cx.W;
				const double f0 = c.sigma * (m::la::dot(X0, c.n) - my) - 0.5 * cx.W;
				const double uj = -f0 / df + (df > 0.0 ? 1.0 : -1.0) * (0.5 * L.m - 1e-4 * L.Lp);
				int64_t kr = Course(L, Q.latV);
				if (((kr % 2 + 2) % 2 == 0) != c.low) ++kr;
				const double U = Q.latU + uj;
				const Cell cl = Locate(L, 0, kr, U);
				const double jU = (U - cl.a < cl.a + cl.len - U) ? cl.a : cl.a + cl.len;
				Q.latU -= U - jU;
				break;
			}
		}
		P = made[0];
		for (size_t x = 1; x < made.size(); ++x) patches.push_back(std::move(made[x]));
	}
}

// кладка следует исходной стене; нарисованный край только открывает нужные ячейки
void BuildSurfaceBrickLayer(const Surf& surf,const BrickDamageParams& settings,
    const std::vector<Patch>& patches,BrickDamageOut& out)
{
    const MeshView& mesh=*surf.v;
    struct Face {std::vector<int> ids;V n;double area;};
    struct Link {int other,a,b;};
    std::vector<Face> faces;faces.reserve(mesh.npolys);
    std::map<std::pair<int,int>,std::vector<int>> edges;
    for(int i=0;i<mesh.npolys;++i){
        const auto& q=mesh.polys[i];Face f;f.ids={q.a,q.b,q.c};if(q.c!=q.d)f.ids.push_back(q.d);
        V cross=m::la::cross(mesh.pts[q.b]-mesh.pts[q.a],mesh.pts[q.c]-mesh.pts[q.a]);
        if(q.c!=q.d)cross+=m::la::cross(mesh.pts[q.c]-mesh.pts[q.a],mesh.pts[q.d]-mesh.pts[q.a]);
        f.area=m::la::length(cross)*.5;f.n=Unit(cross,V(0,0,1));faces.push_back(f);
        if(f.area<1e-10||surf.Avoided(i))continue;
        for(size_t j=0;j<f.ids.size();++j)edges[std::minmax(f.ids[j],f.ids[(j+1)%f.ids.size()])].push_back(i);
    }
    const V up=Unit(V(settings.upX,settings.upY,settings.upZ),V(0,0,1));
    std::vector<std::vector<Link>> links(faces.size());
    for(const auto& [edge,ids]:edges)if(ids.size()==2){
        double c=m::la::dot(faces[ids[0]].n,faces[ids[1]].n);
        // не заворачиваем вертикальные ряды на стену через крышку или дно
        if((std::abs(m::la::dot(faces[ids[0]].n,up))<.25)!=(std::abs(m::la::dot(faces[ids[1]].n,up))<.25))continue;
        if(c<(settings.corners?-.5:.999))continue;
        for(int j=0;j<2;++j)links[ids[j]].push_back({ids[1-j],edge.first,edge.second});
    }
    struct Cut {m::Manifold solid;m::Box bounds;};std::vector<Cut> cuts;
    for(const auto* group:{&out.cutters,&out.breaks,&out.seams,&out.behind,&out.render})for(const auto& c:*group)
        if(!c.IsEmpty())cuts.push_back({c,c.BoundingBox()});
    if(cuts.empty())return;
    const double bs=std::max(.001,settings.brickScale),width=settings.brickWidth*bs;
    const double depth=.5*(settings.renderMin+settings.renderMax),diag=surf.diag;
    m::MeshGL64 sourceData;sourceData.numProp=3;
    for(int i=0;i<mesh.npts;++i)for(int k=0;k<3;++k)sourceData.vertProperties.push_back(mesh.pts[i][k]);
    for(const auto& f:faces)for(size_t j=1;j+1<f.ids.size();++j)sourceData.triVerts.insert(sourceData.triVerts.end(),{uint32_t(f.ids[0]),uint32_t(f.ids[j]),uint32_t(f.ids[j+1])});
    sourceData.Merge();const m::Manifold sourceSolid(sourceData);
    const bool clip=sourceSolid.Status()==m::Manifold::Error::NoError && !sourceSolid.IsEmpty();
    auto insideCuts=[&](V pos){
        for(const auto& cut:cuts){
            if(pos.x<cut.bounds.min.x||pos.x>cut.bounds.max.x||pos.y<cut.bounds.min.y||pos.y>cut.bounds.max.y||pos.z<cut.bounds.min.z||pos.z>cut.bounds.max.z)continue;
            auto hits=cut.solid.RayCast(pos,pos+V(.8731,.3197,.3671)*(diag*4+1));
            std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
            // касание ребра не должно дважды считаться пересечением
            int count=0;double last=-1.;for(const auto& hit:hits)if(hit.distance-last>1e-9){++count;last=hit.distance;}
            if(count%2)return true;
        }return false;
    };
    std::vector<double> peaks;for(const auto& patch:patches){
        double peak=1e-6;
        for(int i=0;i<=16;++i)for(int j=0;j<=16;++j)peak=std::max(peak,RawField(patch,Lerp(patch.u0,patch.u1,i/16.),Lerp(patch.v0,patch.v1,j/16.)));
        peaks.push_back(peak);
    }
    std::vector<char> used(faces.size());
    auto sourceBounds=[&](const Face& f){m::Box box;for(int id:f.ids)box=box.Union(m::Box(mesh.pts[id],mesh.pts[id]));return box;};
    auto nearCut=[&](const Face& f){auto b=sourceBounds(f);for(const auto& c:cuts){
        const double pad=depth+width+settings.brickLength*bs;
        if(b.min.x<=c.bounds.max.x+pad&&b.max.x>=c.bounds.min.x-pad&&b.min.y<=c.bounds.max.y+pad&&b.max.y>=c.bounds.min.y-pad&&b.min.z<=c.bounds.max.z+pad&&b.max.z>=c.bounds.min.z-pad)return true;}return false;};
    // большие плоские грани задают ряды, узкие фаски продолжают их раскладку
    std::vector<int> order(faces.size());for(size_t i=0;i<order.size();++i)order[i]=int(i);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return faces[a].area>faces[b].area;});
    for(int root:order){
        if(used[root]||faces[root].area<1e-10||surf.Avoided(root))continue;
        if(!nearCut(faces[root])){used[root]=1;continue;}
        V u,v;Frame(settings,faces[root].n,u,v);
        std::map<int,V> coords;for(int id:faces[root].ids)coords[id]=V(m::la::dot(mesh.pts[id],u),m::la::dot(mesh.pts[id],v),0);
        std::vector<int> group{root};used[root]=1;
        for(size_t at=0;at<group.size();++at)for(const auto& link:links[group[at]]){
            int other=link.other;if(used[other]||surf.Avoided(other)||!nearCut(faces[other]))continue;
            V edge=mesh.pts[link.b]-mesh.pts[link.a];double length=m::la::length(edge);if(length<1e-9)continue;
            V along=edge/length,across=Unit(m::la::cross(faces[other].n,along),v);
            V axis=Unit(coords[link.b]-coords[link.a],V(1,0,0)),perp(-axis.y,axis.x,0);
            std::map<int,V> candidate;bool fits=true;
            for(int id:faces[other].ids){V rel=mesh.pts[id]-mesh.pts[link.a];V uv=coords[link.a]+axis*m::la::dot(rel,along)+perp*m::la::dot(rel,across);candidate[id]=uv;
                auto old=coords.find(id);if(old!=coords.end()&&m::la::length(old->second-uv)>std::max(1e-5,length*1e-5))fits=false;}
            if(!fits)continue;
            coords.insert(candidate.begin(),candidate.end());used[other]=1;group.push_back(other);
        }
        // общий отступ на стыке удерживает обе стороны угла вместе
        std::map<int,std::vector<V>> normals;for(int fi:group)for(int id:faces[fi].ids){auto& ns=normals[id];
            if(std::none_of(ns.begin(),ns.end(),[&](V n){return m::la::dot(n,faces[fi].n)>.99999;}))ns.push_back(faces[fi].n);}
        std::map<int,V> miters;for(const auto& [id,ns]:normals){V n(0.0);for(V a:ns)n+=a;n=Unit(n,ns[0]);double d=1.;for(V a:ns)d=std::min(d,m::la::dot(n,a));miters[id]=n/std::max(.25,d);}
        std::vector<double> uv,xyz,offset;std::vector<V> samplePoints;std::vector<ProjectDamagePoly> sampleTris;
        double u0=1e300,u1=-1e300,v0=1e300,v1=-1e300;
        for(int fi:group){const auto& ids=faces[fi].ids;
            for(size_t j=1;j+1<ids.size();++j){std::array<int,3> tri{ids[0],ids[j],ids[j+1]};
                V a=coords[tri[0]],b=coords[tri[1]],c=coords[tri[2]];if(m::la::cross(b-a,c-a).z<1e-10)continue;
                int start=int(samplePoints.size());sampleTris.push_back({start,start+1,start+2,start+2});
                for(int id:tri){V co=coords[id],normal=miters[id],pos=mesh.pts[id]-normal*depth;
                    uv.insert(uv.end(),{co.x,co.y});for(int k=0;k<3;++k){xyz.push_back(pos[k]);offset.push_back(normal[k]);}
                    samplePoints.push_back(co);u0=std::min(u0,co.x);u1=std::max(u1,co.x);v0=std::min(v0,co.y);v1=std::max(v1,co.y);}}
        }
        if(uv.empty()||u1-u0<1e-5||v1-v0<1e-5)continue;
        MeshView plane{samplePoints.data(),int(samplePoints.size()),sampleTris.data(),int(sampleTris.size())};ProjectDamageBVH tree;tree.Build(plane);
        double step=std::max(.25,std::min(settings.brickHeight*bs/2.,settings.paintRadius/3.));
        int nu=std::clamp(int(std::ceil((u1-u0)/step)),2,256),nv=std::clamp(int(std::ceil((v1-v0)/step)),2,256);
        std::vector<double> damage(size_t(nu+1)*(nv+1),3),loose(damage.size(),3),reveal(damage.size(),3);
        bool any=false;
        for(int i=0;i<=nu;++i)for(int j=0;j<=nv;++j){
            double x=Lerp(u0,u1,double(i)/nu),y=Lerp(v0,v1,double(j)/nv),distance=0;int poly=-1;
            if(!tree.Raycast(V(x,y,1),V(0,0,-1),2,&distance,&poly)||poly<0)continue;
            size_t base=size_t(poly)*6,vert=size_t(poly)*9;V a(uv[base],uv[base+1],0),b(uv[base+2],uv[base+3],0),c(uv[base+4],uv[base+5],0);
            double det=m::la::cross(b-a,c-a).z,wb=((x-a.x)*(c.y-a.y)-(y-a.y)*(c.x-a.x))/det,wc=((b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x))/det;
            auto interp=[&](const std::vector<double>& values){V result(0.0);for(int k=0;k<3;++k)result[k]=values[vert+k]*(1-wb-wc)+values[vert+3+k]*wb+values[vert+6+k]*wc;return result;};
            V pos=interp(xyz),normal=Unit(interp(offset),faces[root].n),surface=pos+interp(offset)*depth;
            size_t at=size_t(i)*(nv+1)+j;
            if(!insideCuts(pos-normal*std::min(.1,width*.05)))continue;
            bool related=false;
            double removal=3;
            for(size_t k=0;k<patches.size();++k){const auto& patch=patches[k];if(m::la::dot(normal,patch.n)<.35)continue;
                V rel=surface-patch.o;if(std::abs(m::la::dot(rel,patch.n))>std::max(settings.paintSnap*2.,patch.R))continue;
                double raw=RawField(patch,m::la::dot(rel,patch.t),m::la::dot(rel,patch.b));
                if(raw>-settings.brickLength*bs)related=true;
                double threshold=1.-ClampD(settings.breakSize,0.,1.);
                if(patch.hole&&settings.holeEnable)threshold=std::min(threshold,1.-settings.holeSize);
                if(raw>0 && settings.breakSize>0)removal=std::min(removal,1.+threshold-raw/peaks[k]);
            }if(related){reveal[at]=0;any=true;}damage[at]=ClampD(removal,0.,3.);
        }
        if(!any)continue;
        auto p=settings;p.breakSize=.55; // размер отверстия уже учтён в поле повреждения
        auto built=BuildBrickSurface(p,uv,xyz,offset,u0,u1,v0,v1,nu,nv,damage,loose,0.,1.,&reveal);
        std::map<int,std::vector<size_t>> parts;for(size_t f=0;f<built.pieces.size();++f)parts[built.pieces[f]].push_back(f);
        for(const auto& [id,polys]:parts){m::MeshGL64 data;data.numProp=3;std::map<int,int> remap;
            for(size_t f:polys)for(int j=0;j<3;++j){int src=built.triangles[3*f+j];auto it=remap.emplace(src,int(remap.size()));
                if(it.second)for(int k=0;k<3;++k)data.vertProperties.push_back(built.points[src][k]);data.triVerts.push_back(it.first->second);}
            m::Manifold solid(data);if(solid.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Invalid surface brick layer");
            if(clip)solid=solid^sourceSolid;
            if(solid.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Unable to fit brick layer inside the wall");
            if(!solid.IsEmpty()&&solid.Volume()>1e-7)(id<0?out.mortar:out.bricks).push_back(solid.AsOriginal());
        }
    }
}

void PlaceBrickDamageImpl(const CoreInput& in, const BrickDamageParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	bool split, BrickDamageOut& result, const std::vector<MeshView>* brickModels)
{
	std::vector<m::Manifold>& cutters = result.cutters;
	std::vector<m::Manifold>& cracks = result.cracks;
	std::vector<m::Manifold>& bricks = result.bricks;
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	BrickDamageParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.amount = std::max(0, std::min(p.amount, 500));
	p.lobes = std::max(1, std::min(p.lobes, 8));
	p.crackAmount = std::max(0, std::min(p.crackAmount, 32));
	p.stretch = ClampD(p.stretch, 1.0, 6.0);
	if (p.style == 1) {
		                                                                      
		                            
		p.faceJitter = p.faceTilt = p.brickRough = p.chip = p.jointRough = p.brokenRough = 0.0;
		p.looseTilt = 0.0;
		p.useModel = false;
	}
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	if (p.renderMax < p.renderMin) std::swap(p.renderMin, p.renderMax);
	if (p.crackLengthMax < p.crackLengthMin) std::swap(p.crackLengthMin, p.crackLengthMax);
	p.sizeMin = std::max(1e-4, p.sizeMin);
	p.renderMin = std::max(0.0, p.renderMin);
	p.paintRadius = std::max(1e-4, p.paintRadius);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	const bool paint = p.mode == 1;
    const bool proximity = p.mode == 2;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && !proximity && p.amount <= 0) return;

	Surf s;
	BuildSurf(in, p.avoid, s, p.organic);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4252434b44414d47ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4244;                             

	Ctx cx;
	cx.p = &p;
	cx.s = &s;
	cx.seed = seed;
	{
		const double bs = std::max(1e-4, p.brickScale);
		Lattice& L = cx.L;
		L.bond = std::max(0, std::min(p.bond, 4));
		L.m = std::max(0.0, p.mortar) * bs;
		const double len = std::max(1e-3, p.brickLength) * bs;
		const double hgt = std::max(1e-3, p.brickHeight) * bs;
		L.Lp = len + L.m;
		L.Hp = 0.5 * L.Lp;
		L.Pv = hgt + L.m;
		cx.W = std::max(1e-3, p.brickWidth) * bs;
		L.leaf = cx.W + L.m;
		L.runOff = ClampD(p.bondOffset, 0.0, 1.0);
		L.offU = Random01(seed, 0x4f4655ULL, 0) * L.Lp;
		L.offV = Random01(seed, 0x4f4656ULL, 0) * L.Pv;
		cx.mDepth = std::max(0.0, p.mortarDepth) * bs;
		cx.layers = std::max(0, std::min(p.layers, 4));
		cx.split = split;
		cx.separate = p.brickMode == 1 && p.style != 2;
		if (cx.separate) {
			cx.layers = 1;
			cx.coreDepth = std::max(0.0, p.damageDepth) * bs;
			cx.coreScale = std::max(1e-3, p.damageScale) * bs;
			cx.coreNoise = std::max(0.0, p.damageNoise) * bs;
		} else if (p.behind == 1 && p.style != 2 && cx.layers > 0) {
			cx.noiseCore = true;
			cx.coreDepth = std::max(0.0, p.damageDepth) * bs;
			cx.coreScale = std::max(1e-3, p.damageScale) * bs;
			cx.coreNoise = std::max(0.0, p.damageNoise) * bs;
		}
		cx.boxBricks = p.style == 1;
	}
	std::vector<Model> models;
	if (false && p.useModel && brickModels) {
		models.resize(std::min<size_t>(brickModels->size(), 64));
		for (size_t i = 0; i < models.size(); ++i)
			if (PrepareModel((*brickModels)[i], p.modelFlip, models[i])) cx.models.push_back(&models[i]);
	}

	BuildMasonryGuide(s, p);
	std::vector<Patch> patches;
    std::vector<std::shared_ptr<DamageObjectVolume>> objectVolumes;

    if (proximity) {
        for(size_t i=0;i<p.proximity.size();++i) {
            const auto& point=p.proximity[i];
            if(!point.mesh_points.empty()) {
                auto volume=std::make_shared<DamageObjectVolume>(point);objectVolumes.push_back(volume);
                AppendDamageObjectPatches(s,volume,0.5*(p.renderMin+p.renderMax),patches,[&](const V& n,V& t,V& b){Frame(p,n,t,b);});
                continue;
            }
            if(!(point.radius>0.0))continue;
            Patch patch;patch.id=i+1;patch.shape=Shape::Blobs;
            patch.o=V(point.x,point.y,point.z);patch.n=Unit(V(point.nx,point.ny,point.nz),V(0,0,1));
            Frame(p,patch.n,patch.t,patch.b);patch.R=point.radius;patch.Rn=point.radius;
            patch.D=Lerp(p.renderMin,p.renderMax,Random01(seed,i+1,3));
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
			P.D = Lerp(p.renderMin, p.renderMax, rnd(4)) * sc;
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
			const double D = Lerp(p.renderMin, p.renderMax, rnd(3)) * sc;
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
				                                                              
				                                                      
				V nFold(0.0);
				const bool folded = FoldOutline(s, p, cx.separate && p.corners, pts, nFold);
				for (const V& q : pts) cen = cen + q;
				cen = cen / double(pts.size());
				if (folded || (hits > 0 && m::la::length(nsum) / hits > 0.5)) {
					Patch P;
					P.shape = Shape::Polygon;
					P.id = 0x100000ULL + si * 64;
					P.n = folded ? nFold : Unit(nsum, V(0, 0, 1));
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
    if (s.masonry.enabled) for (auto& patch : patches) {
        const double side = m::la::dot(patch.n, s.masonry.n);
        if (std::abs(side) < 0.999) continue;
        patch.masonry = true;
        patch.masonryPlane = side > 0.0 ? s.masonry.high : -s.masonry.low;
        patch.masonryLimit = (side > 0.0 ? 1.0 : -1.0) * 0.5*(s.masonry.low+s.masonry.high);
    }
	if (cx.separate && p.corners && objectVolumes.empty()) AddCornerFaces(cx, s, patches);

	const bool surfaceLayer=cx.separate && p.generatedGeometry;
    auto cutSettings=p;cutSettings.generatedGeometry=surfaceLayer?false:p.generatedGeometry;
    auto cutContext=cx;cutContext.p=&cutSettings;
	std::vector<Out> outs(patches.size());
	ProjectDamageParallelFor(0, int64_t(patches.size()), [&](int64_t i) {
        const auto& patch=patches[size_t(i)];
        if(patch.volume) {
            auto settings=cutSettings;settings.thicknessCap=0.;settings.crackEnable=false;settings.holeEnable=false;
            auto local=cx;local.p=&settings;local.coreDepth=std::max(cx.coreDepth,patch.volume->extent*2.0);
            BuildPatch(local,patch,outs[size_t(i)]);
            auto& result=outs[size_t(i)];for(auto* mesh:{&result.cutter,&result.breaks,&result.seams,&result.behind,&result.render})if(!mesh->IsEmpty())*mesh=*mesh^patch.volume->solid;result.cracks.clear();
            if(!result.mortar.IsEmpty())result.mortar=result.mortar^patch.volume->solid;
            for(size_t j=result.bricks.size();j-->0;)if(!patch.volume->Exposes(result.bricks[j])){result.bricks.erase(result.bricks.begin()+j);result.brickKeys.erase(result.brickKeys.begin()+j);}
        } else if(p.style==2)BuildPatchLowPoly(cutContext,patch,outs[size_t(i)]);else BuildPatch(cutContext,patch,outs[size_t(i)]);
	});
    for(const auto& volume:objectVolumes)if(std::none_of(patches.begin(),patches.end(),[&](const auto& patch){return patch.volume==volume;}))result.behind.push_back(volume->solid.AsOriginal());
	std::unordered_set<uint64_t> placed;
    std::unordered_map<uint64_t, size_t> masonryBricks;
    std::vector<m::Manifold> masonryMortar;                                               
	for (Out& o : outs) {
		{
			m::Box z;
			bool any = false;
			for (const m::Manifold* x : {&o.cutter, &o.breaks, &o.seams, &o.behind, &o.render})
				if (!x->IsEmpty()) { z = z.Union(x->BoundingBox()); any = true; }
			if (any) result.zones.push_back(z);
		}
		if (!o.mortar.IsEmpty()) {
            if(o.masonry)masonryMortar.push_back(std::move(o.mortar));
            else result.mortar.push_back(std::move(o.mortar));
        }
		if (!o.cutter.IsEmpty()) cutters.push_back(std::move(o.cutter));
		if (!o.breaks.IsEmpty()) result.breaks.push_back(std::move(o.breaks));
		if (!o.seams.IsEmpty()) result.seams.push_back(std::move(o.seams));
		if (!o.behind.IsEmpty()) result.behind.push_back(std::move(o.behind));
		if (!o.render.IsEmpty()) result.render.push_back(std::move(o.render));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
		for (size_t i = 0; i < o.bricks.size(); ++i) {
            if(o.masonry) {
                auto entry=masonryBricks.emplace(o.brickKeys[i],bricks.size());
                if(entry.second)bricks.push_back(std::move(o.bricks[i]));
                else bricks[entry.first->second]=(bricks[entry.first->second]+o.bricks[i]).AsOriginal();
            } else if (placed.insert(o.brickKeys[i]).second) bricks.push_back(std::move(o.bricks[i]));
        }
	}
    if(!masonryMortar.empty())result.mortar.push_back(m::Manifold::BatchBoolean(masonryMortar,m::OpType::Add).AsOriginal());
    if(surfaceLayer)BuildSurfaceBrickLayer(s,p,patches,result);
}
}               

                                                                        
void PlaceBrickDamage(const CoreInput& in, const BrickDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& bricks, const std::vector<MeshView>* brickModels)
{
	BrickDamageOut out;
	PlaceBrickDamageImpl(in, p, mainSeed, mask, paintStrokes, false, out, brickModels);
	for (auto& c : out.cutters) cutters.push_back(std::move(c));
	for (auto& c : out.cracks) cracks.push_back(std::move(c));
	for (auto& b : out.bricks) bricks.push_back(std::move(b));
}

                                           
void PlaceBrickDamage(const CoreInput& in, const BrickDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	BrickDamageOut& out, const std::vector<MeshView>* brickModels)
{
	PlaceBrickDamageImpl(in, p, mainSeed, mask, paintStrokes, p.splitMaterials, out, brickModels);
}

                                                                          
                                                
void BrickDamageBricksMesh(const std::vector<m::Manifold>& bricks, BrickDamageMesh& out,
	const std::vector<m::Manifold>* mortar)
{
	out = BrickDamageMesh();
	auto add = [&](const m::Manifold& b, Int32 idx) {
		const m::MeshGL64 mg = b.GetMeshGL64();
		const int base = int(out.points.size());
		for (size_t i = 0; i + 2 < mg.vertProperties.size(); i += mg.numProp)
			out.points.push_back(EVec(mg.vertProperties[i], mg.vertProperties[i + 1], mg.vertProperties[i + 2]));
		for (size_t t = 0; t + 2 < mg.triVerts.size(); t += 3) {
			const int a = base + int(mg.triVerts[t]), bb = base + int(mg.triVerts[t + 1]), c = base + int(mg.triVerts[t + 2]);
			out.polys.push_back({a, bb, c, c});
			out.polyBrick.push_back(idx);
		}
	};
	for (const m::Manifold& b : bricks) {
		if (b.IsEmpty()) continue;
		add(b, out.bricks);
		++out.bricks;
	}
	if (mortar)
		for (const m::Manifold& j : *mortar)
			if (!j.IsEmpty()) add(j, -1);
}

                                                                             
m::Manifold BrickDamageRemesh(const m::Manifold& result, const std::vector<m::Box>& zones, double edge)
{
	if (zones.empty() || !(edge > 0.0)) return result;
	return RemeshEven(result, edge, &zones, 0.99999);
}

// кладка по развёртке управляющего меша: рисунок убирает кирпичи, а не задаёт их наклон
BrickSurfaceMesh BuildBrickSurface(const BrickDamageParams& p,
    const std::vector<double>& uv, const std::vector<double>& xyz, const std::vector<double>& offsets,
    double u0,double u1,double v0,double v1,int nu,int nv,
    const std::vector<double>& damage,const std::vector<double>& loose,
    double boundaryOffset,double opening,const std::vector<double>* reveal)
{
    BrickSurfaceMesh out;
    if(uv.empty() || uv.size()%6 || xyz.size()!=uv.size()/2*3 || offsets.size()!=xyz.size()
       || nu<1 || nv<1 || nu>1024 || nv>1024 || damage.size()!=size_t(nu+1)*size_t(nv+1)
       || loose.size()!=damage.size() || (reveal && reveal->size()!=damage.size())
       || !(u1>u0 && v1>v0))throw std::runtime_error("Invalid brick control surface");
    const double bs=std::max(.001,p.brickScale), W=std::max(.01,p.brickWidth*bs);
    Lattice L;L.bond=p.bond;L.m=std::max(0.,p.mortar*bs);L.Lp=std::max(.01,p.brickLength*bs)+L.m;
    L.Hp=.5*L.Lp;L.Pv=std::max(.01,p.brickHeight*bs)+L.m;L.runOff=p.bondOffset;
    if((u1-u0)*(v1-v0)/(L.Lp*L.Pv)>25000)throw std::runtime_error("Too many bricks: increase Brick Scale or use a smaller control surface");
    const uint64_t seed=uint64_t(uint32_t(p.seed)) ^ 0x425249434b535552ULL;
    auto random=[&](uint64_t key,int stream){return Random01(seed,key,uint64_t(stream));};
    auto field=[&](const std::vector<double>& data,double x,double y){
        double fi=ClampD((x-u0)/(u1-u0)*nu,0.,double(nu)),fj=ClampD((y-v0)/(v1-v0)*nv,0.,double(nv));
        int i=std::min(nu-1,int(fi)),j=std::min(nv-1,int(fj));double a=fi-i,b=fj-j;
        auto at=[&](int x,int y){return data[size_t(x)*(nv+1)+y];};
        return Lerp(Lerp(at(i,j),at(i+1,j),a),Lerp(at(i,j+1),at(i+1,j+1),a),b);
    };
    struct Tile {m::CrossSection section;V a,b,c,pa,pb,pc,na,nb,nc;double det;};
    std::vector<Tile> tiles;std::vector<m::CrossSection> coverage;
    for(size_t i=0;i<uv.size()/6;++i){
        auto point=[&](const std::vector<double>& data,int k){size_t j=i*9+k*3;return V(data[j],data[j+1],data[j+2]);};
        V a(uv[i*6],uv[i*6+1],0),b(uv[i*6+2],uv[i*6+3],0),c(uv[i*6+4],uv[i*6+5],0);
        double det=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);if(det<1e-10)continue;
        m::CrossSection cs(m::Polygons{{{a.x,a.y},{b.x,b.y},{c.x,c.y}}});
        coverage.push_back(cs);tiles.push_back({cs,a,b,c,point(xyz,0),point(xyz,1),point(xyz,2),point(offsets,0),point(offsets,1),point(offsets,2),det});
    }
    if(tiles.empty())return out;
    auto foot=m::CrossSection::BatchBoolean(coverage,m::OpType::Add);
    if(boundaryOffset!=0)foot=foot.Offset(boundaryOffset,m::CrossSection::JoinType::Miter);
    if(foot.IsEmpty())return out;
    // непрерывный контур из поля кисти, без ступеней размером с ячейку
    auto brushShape=[&](const std::vector<double>& values,double threshold,bool ragged){
        std::vector<m::CrossSection> cells;
        if(threshold>0)for(int i=0;i<nu;++i)for(int j=0;j<nv;++j){
            struct Sample{double x,y,d;};Sample corners[4];
            for(int k=0;k<4;++k){int ii=i+(k==1||k==2),jj=j+(k>=2);double x=u0+(u1-u0)*ii/nu,y=v0+(v1-v0)*jj/nv;
                double rough=ragged?1.+ClampD(p.breakJagged,0.,2.)*.3*Fbm(V(x,y,seed%7919)/std::max(.1,L.Pv),2):1.;
                corners[k]={x,y,values[size_t(ii)*(nv+1)+jj]*rough-threshold};}
            for(auto inds:{std::array<int,3>{0,1,2},std::array<int,3>{0,2,3}}){
                m::SimplePolygon poly;
                for(int k=0;k<3;++k){auto a=corners[inds[k]],b=corners[inds[(k+1)%3]];
                    if(a.d<=0)poly.push_back({a.x,a.y});
                    if((a.d<0)!=(b.d<0)){double t=a.d/(a.d-b.d);poly.push_back({Lerp(a.x,b.x,t),Lerp(a.y,b.y,t)});}}
                if(poly.size()>=3)cells.emplace_back(m::Polygons{poly});}}
        return m::CrossSection::BatchBoolean(cells,m::OpType::Add);
    };
    auto holes=brushShape(damage,opening,true);
    if(p.breakSize!=.55 && !holes.IsEmpty())holes=holes.Offset((p.breakSize-.55)*L.Pv,m::CrossSection::JoinType::Round);
    auto looseSelection=brushShape(loose,1.,false);
    auto visible=reveal?brushShape(*reveal,1.,false):foot;
    const double quality=ClampD(p.detail,.1,2.0);
    auto extrude=[&](const m::CrossSection& f,double depth,double front){return m::Manifold::Extrude(f.ToPolygons(),depth).Translate({0,0,front-depth});};
    int piece=0;
    auto append=[&](m::Manifold flat,int material,int id,bool exposed=false){
        if(flat.IsEmpty())return;
        flat=flat.SetProperties(3,[](double* prop,V pos,const double*){prop[0]=pos.x;prop[1]=pos.y;prop[2]=pos.z;});
        // разрезаем на гранях развёртки до переноса: углы остаются углами, а не диагональными растяжениями
        std::vector<m::Manifold> parts;auto rest=flat;
        const auto bounds=flat.BoundingBox();const double z0=bounds.min.z-1,z1=bounds.max.z+1;
        auto map=[&](m::Manifold part,const Tile& t){
            return part.Warp([&](V& q){double s=((q.x-t.a.x)*(t.c.y-t.a.y)-(q.y-t.a.y)*(t.c.x-t.a.x))/t.det;
                double r=((t.b.x-t.a.x)*(q.y-t.a.y)-(t.b.y-t.a.y)*(q.x-t.a.x))/t.det;
                V base=t.pa*(1-s-r)+t.pb*s+t.pc*r;V n=t.na*(1-s-r)+t.nb*s+t.nc*r;q=base+n*q.z;});};
        for(const auto& t:tiles){auto b=t.section.Bounds();if(b.max.x<bounds.min.x||b.min.x>bounds.max.x||b.max.y<bounds.min.y||b.min.y>bounds.max.y)continue;
            auto prism=extrude(t.section,z1-z0,z1);auto part=flat ^ prism;
            if(!part.IsEmpty()){parts.push_back(map(part,t));rest=rest-prism;}}
        // только наружный отступ экстраполируется с ближайшей грани
        if(!rest.IsEmpty())for(const auto& island:rest.Decompose()){
            auto b=island.BoundingBox();V center=(b.min+b.max)*.5;const Tile* best=&tiles.front();double distance=1e300;
            for(const auto& t:tiles){V c=(t.a+t.b+t.c)/3.;double d=(c.x-center.x)*(c.x-center.x)+(c.y-center.y)*(c.y-center.y);if(d<distance){distance=d;best=&t;}}
            parts.push_back(map(island,*best));}
        if(parts.empty())return;
        auto solid=parts.size()==1?parts[0]:m::Manifold::BatchBoolean(parts,m::OpType::Add);
        if(solid.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Control surface folds through its own brick thickness; reduce Brick Width");
        auto mesh=solid.GetMeshGL64();int off=int(out.points.size());
        if(out.points.size()+mesh.vertProperties.size()/mesh.numProp>2000000)throw std::runtime_error("Brick vertex budget exceeded; lower Resolution");
        // свойства на швах могут дублировать вершины; возвращаем общую топологию кирпича
        std::vector<int> parent(mesh.vertProperties.size()/mesh.numProp),remap(parent.size(),-1);
        for(size_t k=0;k<parent.size();++k)parent[k]=int(k);
        auto root=[&](int v){while(parent[v]!=v){parent[v]=parent[parent[v]];v=parent[v];}return v;};
        for(size_t k=0;k<mesh.mergeFromVert.size();++k)parent[root(mesh.mergeFromVert[k])]=root(mesh.mergeToVert[k]);
        for(size_t k=0;k<parent.size();++k){int r=root(int(k));
            if(remap[r]<0){size_t v=size_t(r)*mesh.numProp;remap[r]=int(out.points.size());
                out.points.emplace_back(mesh.vertProperties[v],mesh.vertProperties[v+1],mesh.vertProperties[v+2]);
                out.flat.emplace_back(mesh.vertProperties[v+3],mesh.vertProperties[v+4],mesh.vertProperties[v+5]);}
            remap[k]=remap[r];}
        size_t run=0;
        for(size_t k=0;k<mesh.triVerts.size();k+=3){while(run+1<mesh.runOriginalID.size() && k>=mesh.runIndex[run+1])++run;
            for(int a=0;a<3;++a)out.triangles.push_back(remap[mesh.triVerts[k+a]]);
            auto original=[&](int j){size_t v=size_t(mesh.triVerts[k+j])*mesh.numProp+3;return V(mesh.vertProperties[v],mesh.vertProperties[v+1],mesh.vertProperties[v+2]);};
            V n=m::la::cross(original(1)-original(0),original(2)-original(0));
            bool side=exposed && std::abs(n.z)<.5*m::la::length(n);
            out.materials.push_back(material==1?1:side?2:material);out.pieces.push_back(id);}
    };
    auto bounds=foot.Bounds();
    int64_t k0=Course(L,bounds.min.y)-1,k1=Course(L,bounds.max.y)+1;
    for(int64_t row=k0;row<=k1;++row){
        Cell c=Locate(L,0,row,bounds.min.x-L.Lp);
        for(int guard=0;guard<100000 && c.a<bounds.max.x+L.Lp;++guard){
            Cell cell=c;c=Locate(L,0,row,c.a+c.len+std::max(.0001,L.m*.25));uint64_t key=BrickKey(0,row,cell.j);
            double x0=cell.a+L.m*.5,x1=cell.a+cell.len-L.m*.5,y0=CourseV(L,row)+L.m*.5,y1=y0+L.Pv-L.m;
            if(x1<=x0||y1<=y0)continue;
            auto rect=Rect2(x0,x1,y0,y1);auto face=rect ^ foot;if(face.IsEmpty())continue;
            if(reveal && (face ^ visible).IsEmpty())continue;
            double cx=(x0+x1)*.5,cy=(y0+y1)*.5,ratio=field(damage,cx,cy);
            if(random(key,2)<p.missing)continue;
            // кисть выбирает целый кирпич; раствор его ячейки тоже пропускаем
            if(!(face ^ holes).IsEmpty())continue;
            const bool drawnLoose=!(face ^ looseSelection).IsEmpty();
            const bool nearHole=opening>0 && !holes.IsEmpty() && ratio<1.5;
            if(nearHole && random(key,6)<p.broken)continue;
            bool damaged=nearHole || drawnLoose;
            auto brick=extrude(face,W,0);
            // профиль и случайность совпадают по смыслу с Brick Damage, но основание задаёт плоскость
            double noiseAmp=std::min({W*.045,(y1-y0)*.06,(x1-x0)*.04})*p.brickRough;
            double chip=std::min({W*.12,(y1-y0)*.13,std::max(.01,p.chipSize)*.25})*p.chip;
            if(damaged){noiseAmp+=ClampD(p.brokenRough,0.,2.)*std::min(W,L.Pv)*.025;chip+=ClampD(p.brokenRough,0.,2.)*std::min(W,L.Pv)*.035;}
            if(noiseAmp>0||chip>0){
                brick=brick.RefineToLength(std::max(.5,std::min(W,L.Pv)/quality/2.));
                brick=brick.Warp([&](V& q){double n=Fbm(q/std::max(.1,p.chipSize)+V(double(key%1000)),2);
                    double edge=1.-Smooth(0.,std::max(.1,p.chipSize),std::min({std::abs(q.x-x0),std::abs(x1-q.x),std::abs(q.y-y0),std::abs(y1-q.y)}));
                    q.z+=noiseAmp*n-chip*edge*std::max(0.,n);});
            }
            bool isLoose=drawnLoose || random(key,20)<p.loose;
            double push=(random(key,3)-.5)*2*p.faceJitter,ax=(random(key,4)-.5)*2*p.faceTilt,ay=(random(key,5)-.5)*2*p.faceTilt;
            if(isLoose){push-=Lerp(.25,1.,random(key,22))*p.looseDepth;ax+=(random(key,23)-.5)*2*p.looseTilt;ay+=(random(key,24)-.5)*2*p.looseTilt;
                if(random(key,21)<p.looseOut)push+=Lerp(.3,1.,random(key,40))*p.looseOutDist;}
            brick=brick.Translate({-cx,-cy,W*.5}).Rotate(ax,ay,0).Translate({cx,cy,-W*.5+push});
            append(brick,0,piece++,damaged);
            if(p.jointFill && L.m>0 && p.mortarDepth<W && !isLoose){
                auto cellFoot=Rect2(cell.a,cell.a+cell.len,CourseV(L,row),CourseV(L,row)+L.Pv)^foot;
                auto mortarFace=cellFoot-rect;
                if(!mortarFace.IsEmpty()){
                    auto mortar=extrude(mortarFace,W-std::max(0.,p.mortarDepth),-std::max(0.,p.mortarDepth));
                    if(p.jointRough>0){mortar=mortar.RefineToLength(std::max(1.,L.Pv/quality));mortar=mortar.Warp([&](V& q){q.z+=p.jointRough*.15*L.m*Fbm(q/std::max(.1,L.m),2);});}
                    append(mortar,1,-1);
                }
            }
        }
    }
    return out;
}

PROJECT_DAMAGE_NS_MACRO_END
