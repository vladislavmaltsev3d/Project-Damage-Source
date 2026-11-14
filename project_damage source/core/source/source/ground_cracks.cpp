// строим трещины, плиты и выбоины на грунте
// ProjectDamage ground cracks — GPL-3.0-or-later; see LICENSE.
//
// Recipe: ground-cracks-v1. A procedural appearance model of cracked ground
// slabs (concrete, asphalt), seen from above:
//   1. ground frame: everything works in one object-space plane frame (t, b)
//      with `up` as the third axis. A point (x, y) is projected onto the
//      ground by a ray straight down `up`; faces steeper than Max Slope, and
//      Avoid faces, are not ground, so cracks stop at slab borders and kerbs;
//   2. crack network: cracks are 2D polylines grown in that plane. Primaries
//      nucleate inside a site and grow both ways (or start on the rim of a
//      damaged area and grow outward), wander slowly (Meander) and turn
//      sharply now and then (Kink), so they read as the polygonal slab cracks
//      of ref 6. Each generation branches off the previous one at 55-95
//      degrees. A crack that runs into another one stops there (Join), which
//      gives the T-junctions of real shrinkage networks. All segments live in
//      one hash grid, so cracks of different sites join each other too;
//   3. a crack is cut as a V-groove strip: a closed slab over a 7-column
//      cross-section (chipped lip, steep wall, V bottom) whose rows follow
//      the crack. Rows that would fold over the previous row are skipped, so
//      every strip is a valid graph over the plane;
//   4. damaged area (refs 7 and 8): a height-field slab like a concrete spall.
//      The area is tiled into Voronoi plates (dart-thrown seeds, smaller toward
//      the centre, warped edges). Each plate sinks and tilts by its own amount,
//      the gaps between plates open up toward the centre, and everything fades
//      out over Rim Fade so the area blends into intact ground;
//   5. pothole (refs 6 and 8): plates inside Pothole Size of the area are gone
//      (plus a few loose ones around the rim), leaving a gravel bed at Pothole
//      Depth capped by the slab thickness. Rubble: convex chunks lying on the
//      pothole floor, half sunk into it, returned as buildup;
//   6. heave (ref 9): a primary crack may lift one side. A wedge strip along
//      the crack rises Heave Height at the crack lip and tilts back down to
//      the ground over Heave Width; it is returned as buildup, so it is
//      unioned onto the cut slab after the subtract.
// Candidates use SplitMix64 counter channels like the other ProjectDamage modules,
// keyed by seed (main seed and Ground Seed), site/stroke, crack and channel.
//
// Channel map (auto site k): 0-3 triangle and barycentric, 4 size, 5 area,
// 6 pothole, 7 stretch, 8 stretch angle, 9 lobe count, 10+ lobes (3 each),
// 40-42 noise offset, 50 + crack * 8 + sub crack seeds. Paint stroke k:
// 6 pothole, 7 heave, 8 side, 40-42 noise, 50 + crack * 8 + sub. Crack walk
// steps: (crack key, step * 4 + sub). Plates: (area seed, candidate, ch).
#include "manifold/manifold.h"
#include "ground_cracks.h"
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

uint64_t Mix(uint64_t a, uint64_t b)
{
	uint64_t z = a * 0x9e3779b97f4a7c15ULL + b + 0x632be59bd9b4e019ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

V2 Unit2(const V2& v, const V2& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

double Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }
V2 Perp(const V2& v) { return V2(-v.y, v.x); }
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

V Rotate(const V& v, const V& axis, double ang)
{
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + m::la::cross(axis, v) * s + axis * (m::la::dot(axis, v) * (1.0 - c));
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

                                                                                      

enum class Hit { Miss, Ground, Rejected };

struct Ground {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<double> polyA;
	ProjectDamageBVH bvh;
	V n, t, b;                                          
	double top = 0.0, span = 1.0, diag = 1.0, cosSlope = 0.8;
	const std::vector<bool>* avoid = nullptr;

	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}
	bool IsGround(int32_t poly) const { return m::la::dot(polyN[size_t(poly)], n) >= cosSlope && !Avoided(poly); }
	V World(double x, double y, double z) const { return t * x + b * y + n * z; }
	V2 Plane(const V& p) const { return V2(m::la::dot(p, t), m::la::dot(p, b)); }
	                                                                     
	                                                              
	Hit Height(double x, double y, double& h) const
	{
		double d = 0.0;
		int32_t poly = -1;
		if (!bvh.Raycast(World(x, y, top), -n, span, &d, &poly) || poly < 0) return Hit::Miss;
		h = top - d;
		return IsGround(poly) ? Hit::Ground : Hit::Rejected;
	}
	bool GroundAt(double x, double y, double& h) const { return Height(x, y, h) == Hit::Ground; }
	                                                                        
	double Thickness(double x, double y, double h) const
	{
		double d = 0.0;
		int32_t poly = -1;
		if (bvh.Raycast(World(x, y, h - 1e-5 * diag), -n, 2.0 * diag, &d, &poly)) return d;
		return -1.0;
	}
};

void BuildGround(const CoreInput& in, const GroundCracksParams& p, Ground& g)
{
	const MeshView& v = in.view;
	g.v = &v;
	g.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	g.polyA.assign(size_t(v.npolys), 0.0);
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
		g.polyA[size_t(i)] = 0.5 * m::la::length(cr);
		g.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
	}
	g.bvh.Build(v);
	g.n = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	const V x = std::abs(g.n.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
	g.t = Unit(x - g.n * m::la::dot(x, g.n), AnyPerp(g.n));
	g.b = m::la::cross(g.n, g.t);
	V lo(1e300), hi(-1e300);
	double zlo = 1e300, zhi = -1e300;
	for (int32_t i = 0; i < v.npts; ++i) {
		lo = m::la::min(lo, v.pts[i]);
		hi = m::la::max(hi, v.pts[i]);
		const double z = m::la::dot(v.pts[i], g.n);
		zlo = std::min(zlo, z);
		zhi = std::max(zhi, z);
	}
	g.diag = std::max(1e-6, m::la::length(hi - lo));
	g.top = zhi + 0.01 * g.diag + 1.0;
	g.span = (g.top - zlo) + 0.01 * g.diag + 1.0;
	g.cosSlope = std::cos(ClampD(p.maxSlope, 0.0, 89.0) * m::kPi / 180.0);
	g.avoid = p.avoid ? in.avoidPoly : nullptr;
}

                                                                                          

struct Blob { double x, y, r; };

struct Area {
	uint64_t id = 0;
	bool polygon = false;
	V2 c = V2(0.0);
	double R = 1.0;                             
	std::vector<Blob> blobs;                                                
	double ca = 1.0, sa = 0.0, invE = 1.0;
	std::vector<V2> poly;                                                    
	double jag = 0.0;                                             
	double sc = 1.0;                             
	bool pothole = false;
	V off = V(0.0);
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

                                                          
double AreaField(const Area& A, double x, double y)
{
	double s = -1.0;
	if (!A.polygon) {
		const double dx = x - A.c.x, dy = y - A.c.y;
		const double xs = (dx * A.ca + dy * A.sa) * A.invE, ys = -dx * A.sa + dy * A.ca;
		s = -1e300;
		const double k = 0.25 * A.R;
		for (const Blob& b : A.blobs) {
			const double d = b.r - std::sqrt((xs - b.x) * (xs - b.x) + (ys - b.y) * (ys - b.y));
			if (s <= -1e299) { s = d; continue; }
			const double h = ClampD(0.5 + 0.5 * (d - s) / k, 0.0, 1.0);
			s = Lerp(s, d, h) + k * h * (1.0 - h);                
		}
	} else {
		bool inside = false;
		double nearest = 1e300;
		const auto& xy = A.poly;
		for (size_t i = 0, j = xy.size() - 1; i < xy.size(); j = i++) {
			const V2 a = xy[j], b = xy[i];
			if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
			const V2 q(x - a.x, y - a.y), ab = b - a;
			const double den = m::la::dot(ab, ab);
			const double t = den > 0.0 ? ClampD(m::la::dot(q, ab) / den, 0.0, 1.0) : 0.0;
			nearest = std::min(nearest, m::la::length(q - ab * t));
		}
		s = inside ? nearest : -nearest;
	}
	return s + A.jag * Fbm(V(x, y, 0.37) / (0.45 * A.R) + A.off, 4);
}

                                                                                            

struct SegGrid {
	struct Seg { V2 a, b; uint32_t crack, arm, idx; };
	std::vector<Seg> segs;
	std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
	double cell = 1.0;

	uint64_t Key(int64_t x, int64_t y) const { return (uint64_t(x & 0xffffffff) << 32) | uint64_t(y & 0xffffffff); }
	int64_t C(double x) const { return int64_t(std::floor(x / cell)); }
	void Add(const V2& a, const V2& b, uint32_t crack, uint32_t arm, uint32_t idx)
	{
		const uint32_t id = uint32_t(segs.size());
		segs.push_back({a, b, crack, arm, idx});
		const int steps = std::max(1, int(std::ceil(2.0 * m::la::length(b - a) / cell)));
		uint64_t last = ~0ULL;
		for (int s = 0; s <= steps; ++s) {
			const V2 q = a + (b - a) * (double(s) / steps);
			const uint64_t k = Key(C(q.x), C(q.y));
			if (k == last) continue;
			last = k;
			auto& bucket = grid[k];
			if (bucket.empty() || bucket.back() != id) bucket.push_back(id);
		}
	}
	                                                                     
	                                                                   
	                                                           
	bool Hit(const V2& a, const V2& b, uint32_t crack, uint32_t arm, uint32_t idx, uint32_t skip,
		double& tHit, V2& pHit, bool& own) const
	{
		const int64_t x0 = C(std::min(a.x, b.x)) - 1, x1 = C(std::max(a.x, b.x)) + 1;
		const int64_t y0 = C(std::min(a.y, b.y)) - 1, y1 = C(std::max(a.y, b.y)) + 1;
		const V2 r = b - a;
		bool found = false;
		tHit = 2.0;
		for (int64_t ix = x0; ix <= x1; ++ix)
			for (int64_t iy = y0; iy <= y1; ++iy) {
				auto it = grid.find(Key(ix, iy));
				if (it == grid.end()) continue;
				for (uint32_t id : it->second) {
					const Seg& s = segs[id];
					if (s.crack == crack && s.arm == arm && s.idx + skip >= idx) continue;
					const V2 q = s.b - s.a;
					const double den = Cross2(r, q);
					if (std::abs(den) < 1e-14) continue;
					const V2 w = s.a - a;
					const double t = Cross2(w, q) / den, u = Cross2(w, r) / den;
					if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) continue;
					if (t < tHit) { tHit = t; pHit = a + r * t; own = s.crack == crack; found = true; }
				}
			}
		return found;
	}
};

                                                                                             

struct Domain { V2 c; double R; V off; };

struct Crack {
	std::vector<V2> pts;
	std::vector<double> env;                                                 
	double w = 1.0, dep = 1.0, sc = 1.0;
	int gen = 0;
	bool heave = false;
	double side = 1.0;
	uint64_t key = 0;
	int domain = -1;
};

struct Grow {
	const GroundCracksParams* p;
	const Ground* g;
	SegGrid* grid;
	const std::vector<Area>* areas;
	const std::vector<Domain>* domains;
	uint64_t seed;
};

double Env(const GroundCracksParams& p, double f)
{
	return std::max(0.06, 1.0 - ClampD(p.crackTaper, 0.0, 1.0) * std::pow(ClampD(f, 0.0, 1.0), 1.3));
}

bool InAreaCore(const Grow& G, const V2& q)
{
	for (const Area& A : *G.areas) {
		if (q.x < A.x0 || q.x > A.x1 || q.y < A.y0 || q.y > A.y1) continue;
		if (AreaField(A, q.x, q.y) > 0.15 * A.R) return true;
	}
	return false;
}

                                                                           
void Walk(const Grow& G, const V2& start, double ang0, double len, double w, uint64_t key,
	uint32_t cid, uint32_t arm, int domain, std::vector<V2>& out)
{
	const GroundCracksParams& p = *G.p;
	const double step = std::min(G.grid->cell, std::max(1.2 * w, len / (40.0 * p.detail)));
	const V off(Random01(G.seed, key, 900 + arm) * 1000.0, Random01(G.seed, key, 902 + arm) * 1000.0, 0.0);
	const double kink = ClampD(p.crackKink, 0.0, 2.0);
	const double meander = ClampD(p.crackMeander, 0.0, 2.0);
	const uint64_t ws = G.seed ^ 0x57414c4bULL;            
	V2 cur = start;
	double kinkAcc = 0.0, arc = 0.0;
	for (uint32_t i = 1; arc < len && i < 6000; ++i) {
		const uint64_t sk = Mix(key, arm);
		if (Random01(ws, sk, i * 4) < kink * step / (0.12 * len))
			kinkAcc += (Random01(ws, sk, i * 4 + 1) < 0.5 ? -1.0 : 1.0) * Lerp(0.2, 0.6, Random01(ws, sk, i * 4 + 2));
		const double heading = ang0 + meander * 1.3 * Fbm(V(arc / (0.35 * len + 1e-9), 0.5, 0.0) + off, 3)
			+ kinkAcc + kink * 0.12 * (Random01(ws, sk, i * 4 + 3) - 0.5);
		const V2 nxt = cur + V2(std::cos(heading), std::sin(heading)) * step;
		double h = 0.0;
		if (!G.g->GroundAt(nxt.x, nxt.y, h)) break;
		if (domain >= 0) {
			const Domain& D = (*G.domains)[size_t(domain)];
			const double lim = D.R * (0.9 + 0.3 * Fbm(V(nxt.x, nxt.y, 0.0) / (0.5 * D.R) + D.off, 2));
			if (m::la::length(nxt - D.c) > lim) break;
		}
		if (InAreaCore(G, nxt)) break;
		if (i > 2) {
			double tHit = 2.0;
			V2 pHit;
			bool own = false;
			if (G.grid->Hit(cur, nxt, cid, arm, i, 3, tHit, pHit, own)
				&& (own || Random01(ws, sk, 0x7fffffffULL + i) < ClampD(p.crackJoin, 0.0, 1.0))) {
				out.push_back(pHit);
				G.grid->Add(cur, pHit, cid, arm, i);
				break;
			}
		}
		out.push_back(nxt);
		G.grid->Add(cur, nxt, cid, arm, i);
		cur = nxt;
		arc += step;
	}
}

struct Seed {
	V2 p;
	double ang, len, w, dep, sc;
	int gen, domain;
	bool bidir, heave;
	double side;
	uint64_t key;
};

void GrowCrack(const Grow& G, const Seed& sd, std::vector<Crack>& cracks)
{
	const uint32_t cid = uint32_t(cracks.size());
	std::vector<V2> a, b;
	const double half = sd.bidir ? 0.5 * sd.len * Lerp(0.6, 1.4, Random01(G.seed, sd.key, 1)) : sd.len;
	Walk(G, sd.p, sd.ang, half, sd.w, sd.key, cid, 0, sd.domain, a);
	if (sd.bidir) Walk(G, sd.p, sd.ang + m::kPi, sd.len - half, sd.w, sd.key, cid, 1, sd.domain, b);
	Crack c;
	c.w = sd.w; c.dep = sd.dep; c.sc = sd.sc; c.gen = sd.gen; c.heave = sd.heave; c.side = sd.side;
	c.key = sd.key; c.domain = sd.domain;
	for (size_t i = b.size(); i-- > 0;) c.pts.push_back(b[i]);
	c.pts.push_back(sd.p);
	for (const V2& q : a) c.pts.push_back(q);
	if (c.pts.size() < 3) { cracks.push_back(Crack()); return; }                       
	                                                             
	const size_t nucleus = b.size();
	std::vector<double> arcs(c.pts.size(), 0.0);
	for (size_t i = nucleus + 1; i < c.pts.size(); ++i) arcs[i] = arcs[i - 1] + m::la::length(c.pts[i] - c.pts[i - 1]);
	for (size_t i = nucleus; i-- > 0;) arcs[i] = arcs[i + 1] + m::la::length(c.pts[i] - c.pts[i + 1]);
	const double la = arcs.back(), lb = arcs.front();
	c.env.resize(c.pts.size());
	for (size_t i = 0; i < c.pts.size(); ++i) {
		const double L = i >= nucleus ? la : lb;
		c.env[i] = Env(*G.p, L > 0.0 ? arcs[i] / std::max(L, 0.5 * sd.len) : 1.0);
	}
	cracks.push_back(std::move(c));
}

                                                                                                

struct Ctx {
	const GroundCracksParams* p;
	const Ground* g;
	uint64_t seed;
	const std::vector<Area>* areas;
};

bool NearArea(const Ctx& cx, const V2& q, double margin)
{
	for (const Area& A : *cx.areas) {
		if (q.x < A.x0 - margin - A.jag || q.x > A.x1 + margin + A.jag || q.y < A.y0 - margin - A.jag || q.y > A.y1 + margin + A.jag) continue;
		if (AreaField(A, q.x, q.y) > -margin) return true;
	}
	return false;
}

double CrackLength(const Crack& c)
{
	double L = 0.0;
	for (size_t i = 0; i + 1 < c.pts.size(); ++i) L += m::la::length(c.pts[i + 1] - c.pts[i]);
	return L;
}

                                                                        
std::vector<V2> SmoothLine(const std::vector<V2>& pts, double win)
{
	const size_t n = pts.size();
	std::vector<double> arc(n, 0.0);
	for (size_t i = 1; i < n; ++i) arc[i] = arc[i - 1] + m::la::length(pts[i] - pts[i - 1]);
	std::vector<V2> out(n);
	size_t lo = 0, hi = 0;
	V2 sum(0.0);
	for (size_t i = 0; i < n; ++i) {
		while (hi < n && arc[hi] <= arc[i] + 0.5 * win) { sum = sum + pts[hi]; ++hi; }
		while (arc[lo] < arc[i] - 0.5 * win) { sum = sum - pts[lo]; ++lo; }
		out[i] = sum / double(hi - lo);
	}
	return out;
}

struct Row {
	std::vector<V> top, bot;
	V2 L, R;                                               
};

                                                                           
                                                                         
void EmitRuns(const std::vector<Row>& rows, const std::vector<char>& ok, int nw, double maxSkip,
	const std::vector<double>& arc, std::vector<m::Manifold>& out)
{
	std::vector<const Row*> run;
	double lastArc = 0.0;
	auto flush = [&]() {
		if (run.size() >= 3) {
			std::vector<V> top, bot;
			for (const Row* r : run) {
				top.insert(top.end(), r->top.begin(), r->top.end());
				bot.insert(bot.end(), r->bot.begin(), r->bot.end());
			}
			m::Manifold s = SlabSolid(top, bot, int(run.size()) - 1, nw);
			if (!s.IsEmpty()) out.push_back(std::move(s));
		}
		run.clear();
	};
	for (size_t i = 0; i < rows.size(); ++i) {
		if (!ok[i]) {
			if (!run.empty() && arc[i] - lastArc > maxSkip) flush();
			continue;
		}
		const Row& r = rows[i];
		if (!run.empty()) {
			const Row& q = *run.back();
			const V2 A0 = q.R - q.L, A1 = r.R - r.L, ML = r.L - q.L, MR = r.R - q.R;
			const double e = 1e-9;
			const bool fwd = Cross2(A0, ML) < -e && Cross2(A0, MR) < -e && Cross2(A1, ML) < -e && Cross2(A1, MR) < -e;
			if (!fwd) {
				if (arc[i] - lastArc > maxSkip) { flush(); run.push_back(&r); lastArc = arc[i]; }
				continue;
			}
		}
		run.push_back(&r);
		lastArc = arc[i];
	}
	flush();
}

void BuildCrack(const Ctx& cx, const Crack& c, std::vector<m::Manifold>& cuts, std::vector<m::Manifold>& lifts)
{
	const GroundCracksParams& p = *cx.p;
	const Ground& g = *cx.g;
	const size_t n = c.pts.size();
	if (n < 3) return;
	std::vector<double> arc(n, 0.0);
	for (size_t i = 1; i < n; ++i) arc[i] = arc[i - 1] + m::la::length(c.pts[i] - c.pts[i - 1]);
	const double L = arc.back();
	if (L <= 1e-6) return;
	std::vector<V2> T(n);
	for (size_t i = 0; i < n; ++i) {
		const V2 d = c.pts[std::min(n - 1, i + 1)] - c.pts[i == 0 ? 0 : i - 1];
		T[i] = Unit2(d, i > 0 ? T[i - 1] : V2(1, 0));
	}
	const V off(Random01(cx.seed, c.key, 910) * 1000.0, Random01(cx.seed, c.key, 911) * 1000.0, 0.0);
	const double var = ClampD(p.crackWidthVar, 0.0, 1.0);
	const double chip = ClampD(p.crackChip, 0.0, 3.0);
	const double gapExtra = c.heave ? 0.5 * std::max(0.0, p.heaveGap) * c.sc : 0.0;
	const double feat = std::max(4.0 * c.w, 6.0 * c.sc);
	std::vector<double> hw(n), dep(n), chL(n), chR(n);
	for (size_t i = 0; i < n; ++i) {
		const double nz = Fbm(V(arc[i] / feat, 0.3, 0.0) + off, 3);
		hw[i] = std::max(0.02 * c.w, 0.5 * c.w * c.env[i] * (1.0 + var * 0.8 * nz)) + gapExtra * c.env[i];
		dep[i] = c.dep * std::pow(c.env[i], 0.6) * (1.0 + 0.25 * Fbm(V(arc[i] / feat, 3.3, 0.0) + off, 2));
		const double nl = 0.5 + 0.5 * Fbm(V(arc[i] / (0.6 * feat), 7.7, 0.0) + off, 3);
		const double nr = 0.5 + 0.5 * Fbm(V(arc[i] / (0.6 * feat), 9.1, 0.0) + off, 3);
		chL[i] = chip * 2.0 * hw[i] * Lerp(0.15, 1.4, nl);
		chR[i] = chip * 2.0 * hw[i] * Lerp(0.15, 1.4, nr);
	}

	                                                                                   
	{
		std::vector<Row> rows(n);
		std::vector<char> ok(n, 0);
		for (size_t i = 0; i < n; ++i) {
			const V2 N = Perp(T[i]), P = c.pts[i];
			double hC = 0.0, hL = 0.0, hR = 0.0;
			if (!g.GroundAt(P.x, P.y, hC)) continue;
			const double oL = hw[i] + chL[i], oR = hw[i] + chR[i];
			const V2 PL = P - N * oL, PR = P + N * oR;
			if (g.Height(PL.x, PL.y, hL) == Hit::Miss) hL = hC;
			if (g.Height(PR.x, PR.y, hR) == Hit::Miss) hR = hC;
			const double d = dep[i];
			const double mc = 0.3 * d + 2.0 * std::max(oL, oR) + 0.05 * c.sc + std::abs(hL - hR);
			const double topZ = std::max(hC, std::max(hL, hR)) + mc;
			const double lift = 0.3 * mc;
			const double lipL = std::min(0.6 * d, 0.2 * d + 0.5 * chL[i]);
			const double lipR = std::min(0.6 * d, 0.2 * d + 0.5 * chR[i]);
			const double offs[7] = {-oL, -hw[i], -0.4 * hw[i], 0.0, 0.4 * hw[i], hw[i], oR};
			const double bz[7] = {hL + lift, hC - lipL, hC - 0.8 * d, hC - d, hC - 0.8 * d, hC - lipR, hR + lift};
			Row& r = rows[i];
			for (int k = 0; k < 7; ++k) {
				const V2 q = P + N * offs[k];
				r.top.push_back(g.World(q.x, q.y, topZ));
				r.bot.push_back(g.World(q.x, q.y, bz[k]));
			}
			r.L = PL; r.R = PR;
			ok[i] = 1;
		}
		EmitRuns(rows, ok, 6, 4.0 * std::max(c.w, L / 40.0), arc, cuts);
	}

	                                                                                     
	                                                                             
	                                                                           
	                                                                           
	                                                        
	if (c.heave && p.heaveHeight > 0.0 && p.heaveWidth > 0.0 && L > 8.0 * c.w) {
		const double a0 = L * Lerp(0.05, 0.2, Random01(cx.seed, c.key, 920));
		const double a1 = L * Lerp(0.8, 0.95, Random01(cx.seed, c.key, 921));
		const double H = p.heaveHeight * c.sc;
		const double Wb = std::min(p.heaveWidth * c.sc, 0.6 * (a1 - a0));
		const std::vector<V2> sm = SmoothLine(c.pts, std::min(0.8 * Wb, 0.3 * L));
		std::vector<V2> Ts(n);
		for (size_t i = 0; i < n; ++i) Ts[i] = Unit2(sm[std::min(n - 1, i + 1)] - sm[i == 0 ? 0 : i - 1], T[i]);
		const double s = c.side;
		                                                                         
		                       
		const double embed0 = 0.35 * H + 0.3 * c.sc + 1.4 * p.crackDepth * c.sc;
		const double gapHalf = 0.6 * c.w * (1.0 + chip) + 0.15 * c.sc;
		                                  
		std::vector<double> cuts1 = {a0};
		for (int k = 0; k < 64; ++k) {
			const double next = cuts1.back() + Wb * Lerp(0.9, 1.8, Random01(cx.seed, c.key, 930 + uint64_t(k)));
			if (next > a1 - 0.6 * Wb) break;
			cuts1.push_back(next);
		}
		cuts1.push_back(a1);
		for (size_t sg = 0; sg + 1 < cuts1.size(); ++sg) {
			const double lift = H * (sg == 0 ? 1.0 : Lerp(0.4, 1.0, Random01(cx.seed, c.key, 1000 + sg)));
			const double band = Wb * Lerp(0.75, 1.1, Random01(cx.seed, c.key, 1100 + sg));
			const double bend = Lerp(0.8, 1.6, Random01(cx.seed, c.key, 1200 + sg));
			const double lo = cuts1[sg] + gapHalf, hi = cuts1[sg + 1] - gapHalf;
			                                                                 
			                                                           
			{
				size_t ia = n, ib = 0;
				for (size_t i = 0; i < n; ++i) if (arc[i] >= lo && arc[i] <= hi) { ia = std::min(ia, i); ib = std::max(ib, i); }
				if (ia >= ib || m::la::dot(Ts[ia], Ts[ib]) < 0.85) continue;
			}
			std::vector<Row> rows(n);
			std::vector<char> ok(n, 0);
			std::vector<V2> outer(n);
			for (size_t i = 0; i < n; ++i) {
				if (arc[i] < lo || arc[i] > hi) continue;
				const V2 N = Perp(T[i]) * s, Ns = Perp(Ts[i]) * s;
				const double ch = s > 0.0 ? chR[i] : chL[i];
				const V2 inner = c.pts[i] + N * (hw[i] + ch);
				outer[i] = sm[i] + Ns * (hw[i] + ch + band);
				const double li = lift * (0.85 + 0.15 * (0.5 + 0.5 * Fbm(V(arc[i] / (0.3 * Wb), 1.9 + sg, 0.0) + off, 2)));
				double hk[5];
				bool good = true;
				for (int k = 0; k < 5 && good; ++k) {
					const V2 q = inner + (outer[i] - inner) * (k / 4.0);
					good = g.GroundAt(q.x, q.y, hk[k]) && !NearArea(cx, q, 2.0 * c.w);
				}
				if (!good) continue;
				double hmin = hk[0];
				for (int k = 1; k < 5; ++k) hmin = std::min(hmin, hk[k]);
				const double th = g.Thickness(inner.x, inner.y, hk[0]);
				const double embed = th > 0.0 ? std::min(embed0, 0.8 * th) : embed0;
				Row& r = rows[i];
				for (int k = 0; k < 5; ++k) {
					const double f = k / 4.0;
					const V2 q = inner + (outer[i] - inner) * f;
					const double tz = hk[k] + li * std::pow(1.0 - f, bend) - (0.04 * li + 0.02 * c.sc) * f;
					r.top.push_back(g.World(q.x, q.y, tz));
					r.bot.push_back(g.World(q.x, q.y, hmin - embed));
				}
				                                                                        
				if (s > 0.0) { r.L = inner; r.R = outer[i]; }
				else {
					r.L = outer[i]; r.R = inner;
					std::reverse(r.top.begin(), r.top.end());
					std::reverse(r.bot.begin(), r.bot.end());
				}
				ok[i] = 1;
			}
			const size_t before = lifts.size();
			EmitRuns(rows, ok, 4, 4.0 * std::max(c.w, L / 40.0), arc, lifts);
			if (lifts.size() == before) continue;
			                                                   
			size_t first = n, last = 0;
			for (size_t i = 0; i < n; ++i) if (ok[i]) { first = std::min(first, i); last = std::max(last, i); }
			for (int e = 0; e < 2; ++e) {
				if (e == 0 && sg > 0) continue;                                   
				const size_t i = e == 0 ? first : last;
				const V2 shift = T[i] * ((e == 0 ? -1.0 : 1.0) * (gapHalf - 0.15 * c.sc));
				Crack x;
				x.w = 0.9 * c.w; x.dep = c.dep; x.sc = c.sc; x.gen = c.gen + 1;
				x.key = Mix(c.key, 0x454e44ULL + 2 * sg + uint64_t(e));
				const V2 a = c.pts[i] + shift, b = outer[i] + (outer[i] - c.pts[i]) * 0.1 + shift;
				const V2 dir = Unit2(b - a, Perp(T[i]));
				const double len = m::la::length(b - a);
				const int ns = std::max(4, int(std::ceil(len / std::max(1.5 * c.w, len / 40.0))));
				for (int k = 0; k <= ns; ++k) {
					const double f = double(k) / ns;
					const double wig = 0.35 * c.w * Fbm(V(f * len / (3.0 * c.w + 1e-9), 4.2 * e + sg, 0.0) + off, 2);
					x.pts.push_back(a + (b - a) * f + Perp(dir) * wig);
					x.env.push_back(Env(p, 0.3 + 0.7 * f));
				}
				BuildCrack(cx, x, cuts, lifts);
			}
		}
	}
}

                                                                                               

struct AreaOut {
	m::Manifold cutter;
	std::vector<m::Manifold> rubble;
};

struct PlateSeed {
	V2 q;
	double r, sink, tilt;
	V2 dir;
	bool missing;
};

// основной вход: собираем геометрию по переданным настройкам
void BuildArea(const Ctx& cx, const Area& A, AreaOut& out)
{
	const GroundCracksParams& p = *cx.p;
	const Ground& g = *cx.g;
	const double sc = A.sc;
	const double pad = 1.2 * A.jag + 0.05 * A.R + 1e-3;
	const double u0 = A.x0 - pad, u1 = A.x1 + pad, v0 = A.y0 - pad, v1 = A.y1 + pad;
	const double extent = std::max(u1 - u0, v1 - v0);
	if (!(extent > 0.0)) return;
	double cell = std::min(extent / (140.0 * p.detail), 0.45 * std::max(1e-3, p.gapWidth * sc));
	cell = std::max(cell, extent / (700.0 * std::sqrt(p.detail)));
	const int nu = std::max(4, int(std::ceil((u1 - u0) / cell)));
	const int nv = std::max(4, int(std::ceil((v1 - v0) / cell)));
	const double cu = (u1 - u0) / nu, cv = (v1 - v0) / nv;
	cell = std::max(cu, cv);
	auto I = [&](int i, int j) { return size_t(i) * size_t(nv + 1) + size_t(j); };
	const size_t N = size_t(nu + 1) * size_t(nv + 1);
	std::vector<double> h(N, 0.0), d(N, 0.0), s(N, -1.0), thick(N, -1.0);
	std::vector<char> valid(N, 0), forbid(N, 0), miss(N, 0);

	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double x = u0 + i * cu, y = v0 + j * cv;
			const Hit hit = g.Height(x, y, h[k]);
			if (hit == Hit::Miss) continue;
			valid[k] = 1;
			forbid[k] = hit == Hit::Rejected ? 1 : 0;
			if (!forbid[k]) thick[k] = g.Thickness(x, y, h[k]);
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
					double sh = 0.0;
					int c = 0;
					for (int di = -1; di <= 1; ++di)
						for (int dj = -1; dj <= 1; ++dj) {
							const int a = i + di, b = j + dj;
							if (a < 0 || b < 0 || a > nu || b > nv) continue;
							const size_t q = I(a, b);
							if (!known[q]) continue;
							sh += h[q]; ++c;
						}
					if (c == 0) continue;
					h[k] = sh / c;
					nk[k] = 1;
					changed = true;
				}
			known.swap(nk);
			if (!changed) break;
		}
	}

	double sMax = -1e300;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			s[k] = valid[k] && !forbid[k] ? AreaField(A, u0 + i * cu, v0 + j * cv) : -cell;
			sMax = std::max(sMax, s[k]);
		}
	if (sMax < 3.0 * cell) return;                       
	const double fadeW = std::max(cell, ClampD(p.rimFade, 0.0, 1.0) * sMax);
	const double potT = (1.0 - ClampD(p.potholeSize, 0.0, 1.0)) * sMax;

	                                                                                             
	const double pBase = std::max(4.0 * cell, p.plateSize * sc);
	const double cScale = ClampD(p.plateCenterScale, 0.1, 2.0);
	const double rMin = pBase * std::min(1.0, cScale), rMax = pBase * std::max(1.0, cScale);
	auto rAt = [&](double sv) { return pBase * Lerp(1.0, cScale, Smooth(0.0, sMax, sv)); };
	const uint64_t ps = cx.seed ^ Mix(A.id, 0x504c41544553ULL);              
	std::vector<PlateSeed> seeds;
	const double hcell = 0.5 * rMin;
	std::unordered_map<uint64_t, std::vector<uint32_t>> hgrid;
	auto hkey = [&](int64_t x, int64_t y) { return (uint64_t(x & 0xffffffff) << 32) | uint64_t(y & 0xffffffff); };
	const int rings = int(std::ceil(rMax / hcell)) + 1;
	{
		const double area = (u1 - u0) * (v1 - v0);
		const uint64_t cands = uint64_t(std::min(200000.0, 6.0 * area / (rMin * rMin) + 16.0));
		for (uint64_t c = 0; c < cands; ++c) {
			const double x = Lerp(u0, u1, Random01(ps, c, 0)), y = Lerp(v0, v1, Random01(ps, c, 1));
			const double sv = AreaField(A, x, y);
			if (sv < -rMax) continue;
			const double r = rAt(sv);
			const double rej = 0.75 * r;
			const int64_t cx0 = int64_t(std::floor(x / hcell)), cy0 = int64_t(std::floor(y / hcell));
			bool clash = false;
			const int rr = int(std::ceil(rej / hcell)) + 1;
			for (int64_t dx = -rr; dx <= rr && !clash; ++dx)
				for (int64_t dy = -rr; dy <= rr && !clash; ++dy) {
					auto it = hgrid.find(hkey(cx0 + dx, cy0 + dy));
					if (it == hgrid.end()) continue;
					for (uint32_t id : it->second)
						if (m::la::length(seeds[id].q - V2(x, y)) < std::min(rej, 0.75 * seeds[id].r)) { clash = true; break; }
				}
			if (clash) continue;
			PlateSeed ps1;
			ps1.q = V2(x, y);
			ps1.r = r;
			const double ws = Smooth(0.0, fadeW, sv);
			                                                                   
			                                        
			const bool level = Random01(ps, c, 6) < 0.35;
			ps1.sink = level ? 0.0 : std::max(0.0, p.plateSink) * sc * Random01(ps, c, 2) * ws;
			ps1.tilt = level ? 0.0 : std::max(0.0, p.plateTilt) * sc * Random01(ps, c, 3) * ws;
			const double a = Random01(ps, c, 4) * 2.0 * m::kPi;
			ps1.dir = V2(std::cos(a), std::sin(a));
			ps1.missing = false;
			if (A.pothole) {
				const double sp = sv - potT + 0.15 * sMax * Fbm(V(x, y, 2.1) / (0.5 * A.R) + A.off, 3);
				ps1.missing = sp > 0.0 || (sp > -0.25 * sMax && Random01(ps, c, 5) < ClampD(p.potholeLoose, 0.0, 1.0));
			}
			hgrid[hkey(cx0, cy0)].push_back(uint32_t(seeds.size()));
			seeds.push_back(ps1);
		}
	}
	if (seeds.size() < 2) return;

	const double warpAmp = ClampD(p.plateJagged, 0.0, 2.0) * 0.3;
	const double gapBase = 0.5 * std::max(0.0, p.gapWidth) * sc;
	const double gapD = std::max(0.0, p.gapDepth) * sc;
	const double chip = ClampD(p.crackChip, 0.0, 3.0);
	const double potD = std::max(0.0, p.potholeDepth) * sc;
	const double agg = std::max(1e-3, p.aggregateSize * sc);
	const double rough = ClampD(p.roughness, 0.0, 2.0);
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double sv = s[k];
			if (sv <= 0.0 || forbid[k]) continue;
			const double x = u0 + i * cu, y = v0 + j * cv;
			const double wcol = Smooth(0.0, fadeW, sv);
			const double rl = rAt(sv);
			const V2 q = V2(x, y) + V2(Fbm(V(x, y, 4.4) / (0.6 * rl) + A.off, 3), Fbm(V(x, y, 8.8) / (0.6 * rl) + A.off, 3)) * (warpAmp * rl);
			                    
			const int64_t cx0 = int64_t(std::floor(q.x / hcell)), cy0 = int64_t(std::floor(q.y / hcell));
			double d1 = 1e300, d2 = 1e300;
			int32_t s1 = -1, s2 = -1;
			for (int ring = 0; ring <= 2 * rings; ++ring) {
				for (int64_t dx = -ring; dx <= ring; ++dx)
					for (int64_t dy = -ring; dy <= ring; ++dy) {
						if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
						auto it = hgrid.find(hkey(cx0 + dx, cy0 + dy));
						if (it == hgrid.end()) continue;
						for (uint32_t id : it->second) {
							const double dd = m::la::length(seeds[id].q - q);
							if (dd < d1) { d2 = d1; s2 = s1; d1 = dd; s1 = int32_t(id); }
							else if (dd < d2) { d2 = dd; s2 = int32_t(id); }
						}
					}
				if (s2 >= 0 && (ring - 1) * hcell > d2) break;
			}
			if (s1 < 0 || s2 < 0) continue;
			const PlateSeed& A1 = seeds[size_t(s1)];
			const PlateSeed& A2 = seeds[size_t(s2)];
			const double sep = std::max(1e-9, m::la::length(A2.q - A1.q));
			const double e = (d2 * d2 - d1 * d1) / (2.0 * sep);                                
			const double gh = gapBase * (0.15 + 0.85 * wcol) * (1.0 + 0.35 * Fbm(V(x, y, 6.6) / (0.4 * rl) + A.off, 2));
			double dep = 0.0;
			const V W = g.World(x, y, h[k]);
			if (A1.missing) {
				const double big = Fbm(W / (0.3 * A.R) + A.off + V(5.5, 1.5, 9.5), 3);
				const double pit = 1.0 - 2.0 * std::abs(Fbm(W / agg + A.off + V(-3.3, 8.1, 2.2), 3));
				dep = potD * (1.0 + rough * 0.35 * (0.55 * big + 0.45 * pit));
				if (!A2.missing) dep *= Lerp(0.5, 1.0, Smooth(0.0, 2.0 * gh + 2.0 * cell, e));                   
				dep *= Smooth(0.0, 0.5 * fadeW, sv);
			} else {
				const double t = 0.5 + 0.5 * m::la::dot(A1.dir, q - A1.q) / A1.r;
				dep = wcol * (A1.sink + A1.tilt * ClampD(t, 0.0, 1.5));
				const double prof = 1.0 - Smooth(0.5 * gh, gh * (1.0 + 0.5 * chip) + 0.5 * cell, e);
				if (prof > 0.0) {
					double dg = gapD * wcol * (0.75 + 0.25 * Fbm(V(x, y, 1.2) / (0.3 * rl) + A.off, 2));
					if (A2.missing) dg = std::max(dg, 0.85 * potD);
					dep = std::max(dep, dg * prof);
				}
			}
			if (thick[k] > 0.0) dep = std::min(dep, cap * thick[k]);
			d[k] = std::max(0.0, dep);
			miss[k] = A1.missing ? 1 : 0;
		}

	                                                            
	double dMax = 0.0;
	for (double x : d) dMax = std::max(dMax, x);
	if (dMax <= 1e-6) return;
	const double margin = std::max(0.3 * dMax, 2.0 * cell);
	const double lift = 0.4 * margin;
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
			const double x = u0 + i * cu, y = v0 + j * cv;
			top[k] = g.World(x, y, hmax + margin);
			bot[k] = g.World(x, y, d[k] > 1e-6 ? h[k] - d[k] : hmax + lift);
		}
	out.cutter = SlabSolid(top, bot, nu, nv);

	                                                                                           
	if (A.pothole && p.rubbleEnable && p.rubbleAmount > 0 && p.rubbleSizeMax > 0.0) {
		const uint64_t rs = cx.seed ^ Mix(A.id, 0x5255424cULL);            
		int placed = 0;
		std::vector<std::pair<V2, double>> chunks;
		for (uint64_t c = 0; c < uint64_t(p.rubbleAmount) * 16 && placed < p.rubbleAmount; ++c) {
			const double x = Lerp(u0, u1, Random01(rs, c, 0)), y = Lerp(v0, v1, Random01(rs, c, 1));
			const int i = int(std::round((x - u0) / cu)), j = int(std::round((y - v0) / cv));
			if (i < 0 || j < 0 || i > nu || j > nv) continue;
			const size_t k = I(i, j);
			if (!miss[k] || !valid[k] || d[k] <= 1e-6) continue;
			const double size = Lerp(p.rubbleSizeMin, p.rubbleSizeMax, std::pow(Random01(rs, c, 2), 1.5)) * sc;
			if (size <= 1e-4) continue;
			                                                                      
			                                                           
			bool clear = true;
			const int rr = int(std::ceil(0.6 * size / cell)) + 1;
			for (int di = -rr; di <= rr && clear; ++di)
				for (int dj = -rr; dj <= rr && clear; ++dj) {
					const int a = i + di, b = j + dj;
					if (di * di + dj * dj > rr * rr) continue;
					if (a < 0 || b < 0 || a > nu || b > nv) { clear = false; break; }
					const size_t q = I(a, b);
					clear = miss[q] && valid[q] && d[q] > 0.3 * potD;
				}
			for (const auto& o : chunks)
				if (m::la::length(o.first - V2(x, y)) < 0.6 * (o.second + size)) { clear = false; break; }
			if (!clear) continue;
			const double floorZ = h[k] - d[k];
			const V ctr = g.World(x, y, floorZ + 0.15 * size);                                                   
			const V axis = Unit(V(Random01(rs, c, 3) - 0.5, Random01(rs, c, 4) - 0.5, Random01(rs, c, 5) - 0.5), V(1, 0, 0));
			const double ang = Random01(rs, c, 6) * 2.0 * m::kPi;
			const double ax = 0.5 * size, ay = 0.5 * size * Lerp(0.6, 1.0, Random01(rs, c, 7)),
				az = 0.5 * size * Lerp(0.3, 0.6, Random01(rs, c, 8));
			std::vector<V> pts;
			for (int q = 0; q < 14; ++q) {
				const double zz = 2.0 * Random01(rs, c, 10 + uint64_t(q) * 3) - 1.0;
				const double phi = 2.0 * m::kPi * Random01(rs, c, 11 + uint64_t(q) * 3);
				const double rr = std::sqrt(std::max(0.0, 1.0 - zz * zz)) * Lerp(0.7, 1.0, Random01(rs, c, 12 + uint64_t(q) * 3));
				const V loc(rr * std::cos(phi) * ax, rr * std::sin(phi) * ay, zz * az);
				const V wv = g.t * loc.x + g.b * loc.y + g.n * loc.z;
				pts.push_back(ctr + Rotate(wv, axis, 0.35 * ang));
			}
			                                                                     
			                        
			const double drop = potD * (0.4 * rough + 0.1) + 0.5 * sc;
			for (size_t q = 0, n0 = pts.size(); q < n0; ++q) {
				const V rel = pts[q] - ctr;
				const V lat = rel - g.n * m::la::dot(rel, g.n);
				pts.push_back(ctr + lat * 0.9 - g.n * (0.05 * size + drop));
			}
			m::Manifold chunk = m::Manifold::Hull(pts);
			if (chunk.IsEmpty() || chunk.Status() != m::Manifold::Error::NoError) continue;
			out.rubble.push_back(std::move(chunk));
			chunks.push_back({V2(x, y), size});
			++placed;
		}
	}
}

}               

void PlaceGroundCracks(const CoreInput& in, const GroundCracksParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& areaCutters, std::vector<m::Manifold>& crackCutters,
	std::vector<m::Manifold>& buildup, int32_t* rubbleCount)
{
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	GroundCracksParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.amount = std::max(0, std::min(p.amount, 200));
	p.crackAmount = std::max(0, std::min(p.crackAmount, 32));
	p.crackGenerations = std::max(0, std::min(p.crackGenerations, 4));
	p.rubbleAmount = std::max(0, std::min(p.rubbleAmount, 512));
	p.areaStretch = ClampD(p.areaStretch, 1.0, 6.0);
	p.crackBranch = ClampD(p.crackBranch, 0.0, 16.0);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	if (p.crackLengthMax < p.crackLengthMin) std::swap(p.crackLengthMin, p.crackLengthMax);
	if (p.rubbleSizeMax < p.rubbleSizeMin) std::swap(p.rubbleSizeMin, p.rubbleSizeMax);
	p.sizeMin = std::max(1e-3, p.sizeMin);
	p.crackLengthMin = std::max(1e-3, p.crackLengthMin);
	p.crackWidth = std::max(1e-4, p.crackWidth);
	p.crackDepth = std::max(1e-4, p.crackDepth);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	const bool paint = p.mode == 1;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && p.amount <= 0) return;

	Ground g;
	BuildGround(in, p, g);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x47524e44435243ULL)                 
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4743;                             

	std::vector<Area> areas;
	std::vector<Domain> domains;
	std::vector<Seed> primaries;
	std::vector<Crack> drawn;                                         
	const double meanLen = 0.5 * (p.crackLengthMin + p.crackLengthMax);

	auto radialSeeds = [&](const Area& A, int count, uint64_t key0, double sc, int dom, uint64_t rs, uint64_t rk) {
		for (int c = 0; c < count; ++c) {
			const uint64_t ch = 50 + uint64_t(c) * 8;
			const double ang = 2.0 * m::kPi * (c + 0.7 * Random01(rs, rk, ch)) / count;
			const V2 dir(std::cos(ang), std::sin(ang));
			const double stepR = 0.02 * A.R;
			V2 q = A.c;
			for (int st = 0; st < 400; ++st) {
				const V2 nq = q + dir * stepR;
				if (AreaField(A, nq.x, nq.y) <= 0.0) { q = nq; break; }
				q = nq;
			}
			Seed sd;
			sd.p = q;
			sd.ang = ang + (Random01(rs, rk, ch + 1) - 0.5) * 0.6;
			sd.len = Lerp(p.crackLengthMin, p.crackLengthMax, std::pow(Random01(rs, rk, ch + 2), 1.3)) * sc;
			sd.w = p.crackWidth * sc * Lerp(0.8, 1.2, Random01(rs, rk, ch + 3));
			sd.dep = p.crackDepth * sc * Lerp(0.8, 1.2, Random01(rs, rk, ch + 4));
			sd.sc = sc;
			sd.gen = 0;
			sd.domain = dom;
			sd.bidir = false;
			sd.heave = false;                                                  
			sd.side = 1.0;
			sd.key = Mix(key0, uint64_t(c));
			primaries.push_back(sd);
		}
	};

	if (!paint) {
		                                                                                         
		std::vector<double> cdf(size_t(v.npolys));
		double total = 0.0;
		for (int32_t i = 0; i < v.npolys; ++i) {
			total += g.IsGround(i) ? g.polyA[size_t(i)] : 0.0;
			cdf[size_t(i)] = total;
		}
		if (total <= 0.0) return;
		const uint64_t as = seed ^ 0x4155544fULL;            
		std::vector<std::pair<V2, double>> placed;
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.amount) * 30 && accepted < p.amount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(as, k, ch); };
			int32_t ti = int32_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * total) - cdf.begin());
			if (ti >= v.npolys) ti = v.npolys - 1;
			const ProjectDamagePoly& q = v.polys[ti];
			double r1 = rnd(1), r2 = rnd(2);
			V A0 = v.pts[q.a], B0 = v.pts[q.b], C0 = v.pts[q.c];
			if (q.c != q.d && rnd(3) < 0.5) { B0 = v.pts[q.c]; C0 = v.pts[q.d]; }
			if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
			const V pos = A0 * (1.0 - r1 - r2) + B0 * r1 + C0 * r2;
			if (mask && !mask->Keep(pos, int32_t(k), kStream)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			const V2 c2 = g.Plane(pos);
			double hh = 0.0;
			if (!g.GroundAt(c2.x, c2.y, hh)) continue;                        
			const double R = Lerp(p.sizeMin, p.sizeMax, rnd(4)) * sc;
			bool clash = false;
			for (const auto& o : placed)
				if (m::la::length(o.first - c2) < 0.5 * (o.second + R)) { clash = true; break; }
			if (clash) continue;
			placed.push_back({c2, R});
			++accepted;
			const int dom = int(domains.size());
			domains.push_back({c2, R, V(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0)});
			int nRad = 0;
			if (rnd(5) < ClampD(p.areaChance, 0.0, 1.0) && p.areaSize > 0.0) {
				Area A;
				A.id = Mix(k, 0x41524541ULL);
				A.c = c2;
				A.R = std::max(1e-3, p.areaSize * R);
				A.sc = sc;
				A.pothole = rnd(6) < ClampD(p.potholeChance, 0.0, 1.0);
				const double e = Lerp(1.0, p.areaStretch, rnd(7));
				const double ang = rnd(8) * m::kPi;
				A.ca = std::cos(ang); A.sa = std::sin(ang); A.invE = 1.0 / e;
				const int nb = 1 + int(std::floor(rnd(9) * 4.0));
				A.blobs.push_back({0.0, 0.0, 0.6 * A.R});
				for (int b = 1; b < nb; ++b) {
					const double a = rnd(10 + uint64_t(b) * 3) * 2.0 * m::kPi;
					const double dist = Lerp(0.35, 0.7, rnd(11 + uint64_t(b) * 3)) * A.R;
					A.blobs.push_back({std::cos(a) * dist, std::sin(a) * dist, Lerp(0.3, 0.5, rnd(12 + uint64_t(b) * 3)) * A.R});
				}
				double ext = 0.0;
				for (const Blob& b : A.blobs) ext = std::max(ext, std::sqrt(b.x * b.x + b.y * b.y) + b.r);
				ext *= e;
				A.jag = ClampD(p.areaJagged, 0.0, 2.0) * 0.22 * A.R;
				A.off = V(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
				A.x0 = c2.x - ext; A.x1 = c2.x + ext; A.y0 = c2.y - ext; A.y1 = c2.y + ext;
				nRad = int(std::round(0.6 * p.crackAmount));
				if (p.crackEnable && nRad > 0) radialSeeds(A, nRad, Mix(k, 1), sc, dom, as, k);
				areas.push_back(std::move(A));
			}
			if (p.crackEnable)
				for (int c = nRad; c < p.crackAmount; ++c) {
					const uint64_t ch = 50 + uint64_t(c) * 8;
					const double rr = 0.6 * R * std::sqrt(rnd(ch)), aa = 2.0 * m::kPi * rnd(ch + 7);
					Seed sd;
					sd.p = c2 + V2(std::cos(aa), std::sin(aa)) * rr;
					double h2 = 0.0;
					if (!g.GroundAt(sd.p.x, sd.p.y, h2)) continue;
					sd.ang = rnd(ch + 1) * m::kPi;
					sd.len = Lerp(p.crackLengthMin, p.crackLengthMax, std::pow(rnd(ch + 2), 1.3)) * sc;
					sd.w = p.crackWidth * sc * Lerp(0.8, 1.2, rnd(ch + 3));
					sd.dep = p.crackDepth * sc * Lerp(0.8, 1.2, rnd(ch + 4));
					sd.sc = sc;
					sd.gen = 0;
					sd.domain = dom;
					sd.bidir = true;
					sd.heave = rnd(ch + 5) < ClampD(p.heaveChance, 0.0, 1.0);
					sd.side = rnd(ch + 6) < 0.5 ? -1.0 : 1.0;
					sd.key = Mix(Mix(k, 2), uint64_t(c));
					primaries.push_back(sd);
				}
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
			const bool closed = pts.size() >= 4 && (gapEnd < 1e-6 * len + 1e-9 || gapEnd < ClampD(p.paintClose, 0.0, 1.0) * len);
			const V mid = pts[pts.size() / 2];
			if (mask && !mask->Keep(mid, int32_t(si), kStream + 1)) continue;
			const double sc = mask ? mask->SizeScale(mid) : 1.0;
			if (sc <= 0.0) continue;
			const V off(rnd(40) * 1000.0, rnd(41) * 1000.0, rnd(42) * 1000.0);
			auto nearGround = [&](const V& q) {
				const V2 xy = g.Plane(q);
				double h = 0.0;
				return g.GroundAt(xy.x, xy.y, h) && std::abs(m::la::dot(q, g.n) - h) <= p.paintSnap;
			};

			if (closed) {
				if (gapEnd < 1e-6 * len + 1e-9) pts.pop_back();
				int hits = 0;
				for (const V& q : pts) hits += nearGround(q) ? 1 : 0;
				if (hits * 2 < int(pts.size())) continue;
				Area A;
				A.id = Mix(0x100000ULL + si, 0x41524541ULL);
				A.polygon = true;
				double area = 0.0;
				V2 cen(0.0);
				double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
				for (const V& q : pts) {
					const V2 l = g.Plane(q);
					A.poly.push_back(l);
					cen = cen + l;
					x0 = std::min(x0, l.x); x1 = std::max(x1, l.x); y0 = std::min(y0, l.y); y1 = std::max(y1, l.y);
				}
				for (size_t i = 0, j = A.poly.size() - 1; i < A.poly.size(); j = i++)
					area += A.poly[j].x * A.poly[i].y - A.poly[i].x * A.poly[j].y;
				area = 0.5 * std::abs(area);
				if (area <= 1e-9) continue;
				A.c = cen / double(A.poly.size());
				A.R = std::sqrt(area / m::kPi);
				A.sc = sc;
				A.pothole = rnd(6) < ClampD(p.potholeChance, 0.0, 1.0);
				A.jag = ClampD(p.areaJagged, 0.0, 2.0) * 0.08 * A.R;
				A.off = off;
				A.x0 = x0; A.x1 = x1; A.y0 = y0; A.y1 = y1;
				const int dom = int(domains.size());
				domains.push_back({A.c, A.R + p.crackLengthMax * sc, off});
				if (p.crackEnable && p.crackAmount > 0) radialSeeds(A, p.crackAmount, Mix(0x100000ULL + si, 1), sc, dom, ps, si);
				areas.push_back(std::move(A));
				continue;
			}

			                                                                      
			                    
			const double w = p.crackWidth * sc;
			const double step = std::max(std::max(1.2 * w, len / 3000.0), std::min(2.0 * sc, len / 12.0));
			std::vector<V> rp;
			{
				double acc = 0.0, next = 0.0;
				for (size_t i = 0; i + 1 < pts.size(); ++i) {
					const double sl = m::la::length(pts[i + 1] - pts[i]);
					while (next <= acc + sl) {
						rp.push_back(pts[i] + (pts[i + 1] - pts[i]) * ((next - acc) / sl));
						next += step;
					}
					acc += sl;
				}
				if (rp.empty() || m::la::length(rp.back() - pts.back()) > 0.25 * step) rp.push_back(pts.back());
			}
			const bool heave = rnd(7) < ClampD(p.heaveChance, 0.0, 1.0);
			const double side = rnd(8) < 0.5 ? -1.0 : 1.0;
			std::vector<V2> xy(rp.size());
			std::vector<char> on(rp.size());
			for (size_t i = 0; i < rp.size(); ++i) { xy[i] = g.Plane(rp[i]); on[i] = nearGround(rp[i]) ? 1 : 0; }
			const double jit = std::max(0.0, p.paintJitter) * sc;
			std::vector<V2> jx(xy);
			if (jit > 0.0)
				for (size_t i = 0; i < xy.size(); ++i) {
					const V2 t2 = Unit2(xy[std::min(xy.size() - 1, i + 1)] - xy[i == 0 ? 0 : i - 1], V2(1, 0));
					const double a = i * step;
					const double fe = Smooth(0.0, 3.0 * jit + 1e-9, std::min(a, len - a));
					jx[i] = xy[i] + Perp(t2) * (jit * fe * (0.7 * Fbm(V(a / (6.0 * sc), 0.5, 0.0) + off, 3)
						+ 0.3 * Fbm(V(a / (1.5 * sc), 2.5, 0.0) + off, 2)));
				}
			size_t a0 = 0;
			int piece = 0;
			for (size_t i = 0; i <= jx.size(); ++i) {
				if (i < jx.size() && on[i]) continue;
				if (i - a0 >= 3) {
					Crack c;
					c.w = w; c.dep = p.crackDepth * sc; c.sc = sc; c.gen = 0;
					c.heave = heave; c.side = side;
					c.key = Mix(0x200000ULL + si, uint64_t(piece++));
					const size_t nPts = i - a0;
					for (size_t q = a0; q < i; ++q) {
						c.pts.push_back(jx[q]);
						const double f = std::abs(2.0 * double(q - a0) / double(nPts - 1) - 1.0);
						c.env.push_back(Env(p, f));
					}
					drawn.push_back(std::move(c));
				}
				a0 = i + 1;
			}
		}
	}

	                                                                                              
	std::vector<Crack> cracks;
	SegGrid grid;
	grid.cell = std::max(4.0 * p.crackWidth, p.crackLengthMax / 25.0);
	Grow G{&p, &g, &grid, &areas, &domains, seed};
	if (p.crackEnable) {
		                                                                 
		for (Crack& c : drawn) {
			Crack cur = c;
			cur.pts.clear(); cur.env.clear();
			uint32_t idx = 0;
			for (size_t i = 0; i < c.pts.size(); ++i) {
				if (!cur.pts.empty()) {
					double tHit; V2 pHit; bool own = false;
					const uint32_t cid = uint32_t(cracks.size());
					if (grid.Hit(cur.pts.back(), c.pts[i], cid, 0, idx + 1, 2, tHit, pHit, own) && own) {
						const V2 last = cur.pts.back();
						const double le = cur.env.back();
						if (cur.pts.size() >= 3) cracks.push_back(cur);
						cur.pts.clear(); cur.env.clear();
						cur.key = Mix(cur.key, 77);
						cur.pts.push_back(last); cur.env.push_back(le);
						idx = 0;
					}
					grid.Add(cur.pts.back(), c.pts[i], uint32_t(cracks.size()), 0, ++idx);
				}
				cur.pts.push_back(c.pts[i]);
				cur.env.push_back(c.env[i]);
			}
			if (cur.pts.size() >= 3) cracks.push_back(cur);
		}
		for (const Seed& sd : primaries) GrowCrack(G, sd, cracks);
		size_t genBegin = 0;
		for (int gen = 0; gen < p.crackGenerations; ++gen) {
			const size_t genEnd = cracks.size();
			std::vector<Seed> next;
			for (size_t ci = genBegin; ci < genEnd; ++ci) {
				const Crack& c = cracks[ci];
				if (c.pts.size() < 4 || c.gen != gen) continue;
				const double L = CrackLength(c);
				const double rate = p.crackBranch * (paint && c.domain < 0 ? p.paintBranch : 1.0) * std::pow(0.6, gen);
				const double expect = rate * L / std::max(1e-6, meanLen * c.sc);
				const uint64_t bk = Mix(c.key, 0x4252414eULL);            
				const int nb = std::min(64, int(std::floor(expect + Random01(seed, bk, 0))));
				for (int b = 0; b < nb; ++b) {
					auto r = [&](uint64_t ch) { return Random01(seed, bk, 1 + uint64_t(b) * 8 + ch); };
					const size_t at = std::min(c.pts.size() - 2, size_t(1) + size_t(Lerp(0.1, 0.9, r(0)) * double(c.pts.size() - 2)));
					const V2 tan = Unit2(c.pts[at + 1] - c.pts[at - 1], V2(1, 0));
					const double sgn = c.heave ? -c.side : (r(1) < 0.5 ? -1.0 : 1.0);                               
					const double ang = std::atan2(tan.y, tan.x) + sgn * Lerp(0.95, 1.65, r(2));
					Seed sd;
					sd.p = c.pts[at];
					sd.ang = ang;
					sd.len = L * Lerp(0.3, 0.65, r(3));
					sd.w = c.w * c.env[at] * Lerp(0.6, 0.85, r(4));
					sd.dep = c.dep * std::pow(c.env[at], 0.6) * 0.8;
					sd.sc = c.sc;
					sd.gen = gen + 1;
					sd.domain = c.domain;
					sd.bidir = false;
					sd.heave = false;
					sd.side = 1.0;
					sd.key = Mix(bk, uint64_t(b) + 1);
					if (sd.w < 0.05 * p.crackWidth * c.sc) continue;
					next.push_back(sd);
				}
			}
			genBegin = genEnd;
			for (const Seed& sd : next) GrowCrack(G, sd, cracks);
		}
	}

	                                                                                                   
	Ctx cx{&p, &g, seed, &areas};
	std::vector<std::vector<m::Manifold>> cOut(cracks.size()), lOut(cracks.size());
	ProjectDamageParallelFor(0, int64_t(cracks.size()), [&](int64_t i) {
		BuildCrack(cx, cracks[size_t(i)], cOut[size_t(i)], lOut[size_t(i)]);
	});
	std::vector<AreaOut> aOut(areas.size());
	ProjectDamageParallelFor(0, int64_t(areas.size()), [&](int64_t i) {
		BuildArea(cx, areas[size_t(i)], aOut[size_t(i)]);
	});
	const size_t rubbleStart = buildup.size();
	for (auto& o : aOut) {
		if (!o.cutter.IsEmpty()) areaCutters.push_back(std::move(o.cutter));
		for (auto& r : o.rubble) buildup.push_back(std::move(r));
	}
	if (rubbleCount != nullptr) *rubbleCount = int32_t(buildup.size() - rubbleStart);
	for (auto& v2 : cOut) for (auto& c : v2) crackCutters.push_back(std::move(c));
	for (auto& v2 : lOut) for (auto& c : v2) buildup.push_back(std::move(c));
}
PROJECT_DAMAGE_NS_MACRO_END
