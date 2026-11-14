// строим бетонный пробой и трещины вокруг точки удара
// ProjectDamage concrete hit — GPL-3.0-or-later; see LICENSE.
//
// Recipe: concrete-hit-v1. A procedural model of an impact on a painted or
// plastered concrete wall, placed by empties:
//   1. placement: every empty is carried into the source's local space by the
//      host (ConcreteHitEmpty::rel). The impact point is the surface point
//      nearest to the empty (aim 0) or the first hit along its +Z (aim 1).
//      The empty's X/Y axis lengths x display size set an elliptical radius,
//      its X axis turns the pattern, and its distance to the surface fades
//      the hit (Proximity): an empty on the surface hits fully, one pulled
//      away by Proximity x radius leaves nothing. A slanted shot (aim 1)
//      stretches the hit along the shot direction;
//   2. plates: the skin around the impact is a Voronoi partition of jittered,
//      Poisson-spaced seeds in the hit's normalized (unit-disk) space, with
//      smaller plates toward the centre. Seeds inside the (jagged) unit disk
//      form the shatter zone. Each zone plate is knocked out (cut to the
//      layer depth: the substrate shows, as in the reference) or loose (cut
//      by a tilted plane so the plate reads as pushed in toward the impact).
//      Plate cutters are extruded cell polygons, refined and draped on the
//      surface along its smoothed local normal (blended to the hit normal
//      around the crater), so plate edges are crisp rather than grid-stepped
//      and stay square to a curved surface. Parts of the surface turned more
//      than ~63 degrees from the hit normal are left intact. Knocked-out
//      neighbours overlap each other; edges to remaining plates wobble and
//      get small chips bitten out;
//   3. crater: a height-field slab over a grid in the hit frame (the same
//      construction as concrete damage) with a ragged rim, a domed depth
//      profile, a pitted aggregate floor, a wall-thickness cap, and an
//      optional punch-through hole;
//   4. cracks: V-groove strips, built along the same local normal as the
//      plates, along every seam between two remaining plates
//      (this includes the outer rim of the zone), radial cracks walked out of
//      the zone (meandering, tapering, sometimes forking), and broken
//      concentric rings around it;
//   5. through (PlaceConcreteHitThrough): the same plates become loose
//      chunks. In the hit frame, gaps = Offset(zone, +g/2) minus every kept
//      plate Offset(-g/2) (Clipper2 via CrossSection, mitred corners), then
//      extruded along the hit normal from above the surface through the wall
//      (walls up to throughMaxWall thick) or chunkDepth x radius deep with a
//      gap slab under the chunks. One cutter per hit; chunks fall out of the
//      Boolean as separate pieces.
// Candidates use SplitMix64 counter channels keyed by (seed, empty, channel),
// like concrete damage and fabric tears.
//
// Channel map, per empty: 0-3 tilt/stretch keys, 10 noise offsets (10-12),
// 20 + k*4 seed candidates, 50000 + cell*8 plate states, 60000 + r*32 radial
// cracks, 70000 + ring*64 ring arcs.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "concrete_hit.h"
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

                                                                                     
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) return a;
	const V bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
	const V cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	std::vector<V> cornerN;                                                            
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
	                                                                   
	                                                                         
	                              
	V SmoothN(int32_t poly, const V& P) const
	{
		const ProjectDamagePoly& f = v->polys[poly];
		const V* cn = &cornerN[size_t(poly) * 4];
		auto bary = [&](const V& a, const V& b, const V& c, double& wa, double& wb, double& wc) {
			const V e0 = b - a, e1 = c - a, e2 = P - a;
			const double d00 = m::la::dot(e0, e0), d01 = m::la::dot(e0, e1), d11 = m::la::dot(e1, e1);
			const double d20 = m::la::dot(e2, e0), d21 = m::la::dot(e2, e1);
			const double den = d00 * d11 - d01 * d01;
			if (std::abs(den) < 1e-300) { wa = 1.0; wb = wc = 0.0; return -1.0; }
			wb = (d11 * d20 - d01 * d21) / den;
			wc = (d00 * d21 - d01 * d20) / den;
			wa = 1.0 - wb - wc;
			return std::min(wa, std::min(wb, wc));
		};
		double a0, b0, c0, a1 = 0, b1 = 0, c1 = 0;
		const double m0 = bary(v->pts[f.a], v->pts[f.b], v->pts[f.c], a0, b0, c0);
		V n;
		if (f.c != f.d && bary(v->pts[f.a], v->pts[f.c], v->pts[f.d], a1, b1, c1) > m0)
			n = cn[0] * std::max(0.0, a1) + cn[2] * std::max(0.0, b1) + cn[3] * std::max(0.0, c1);
		else
			n = cn[0] * std::max(0.0, a0) + cn[1] * std::max(0.0, b0) + cn[2] * std::max(0.0, c0);
		return Unit(n, polyN[size_t(poly)]);
	}
	                                                                      
	bool Nearest(const V& p, V& q, int32_t& poly) const
	{
		double best = 1e300;
		poly = -1;
		for (int32_t i = 0; i < v->npolys; ++i) {
			const ProjectDamagePoly& f = v->polys[i];
			const V a = v->pts[f.a], b = v->pts[f.b], c = v->pts[f.c];
			V c0 = ClosestOnTri(p, a, b, c);
			double d0 = m::la::dot(c0 - p, c0 - p);
			if (f.c != f.d) {
				const V c1 = ClosestOnTri(p, a, c, v->pts[f.d]);
				const double d1 = m::la::dot(c1 - p, c1 - p);
				if (d1 < d0) { d0 = d1; c0 = c1; }
			}
			if (d0 < best) { best = d0; q = c0; poly = i; }
		}
		return poly >= 0;
	}
};

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, bool useAvoid, Surf& s)
{
	const MeshView& v = in.view;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
		s.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
	}
	                                                                          
	                                          
	{
		std::vector<double> area(size_t(v.npolys), 0.0);
		std::vector<int32_t> start(size_t(v.npts) + 1, 0), list;
		auto corners = [&](const ProjectDamagePoly& q, int32_t* c) { c[0] = q.a; c[1] = q.b; c[2] = q.c; c[3] = q.d; return q.c == q.d ? 3 : 4; };
		for (int32_t i = 0; i < v.npolys; ++i) {
			const ProjectDamagePoly& q = v.polys[i];
			V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
			if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
			area[size_t(i)] = 0.5 * m::la::length(cr);
			int32_t c[4];
			const int nc = corners(q, c);
			for (int k = 0; k < nc; ++k) if (c[k] >= 0 && c[k] < v.npts) ++start[size_t(c[k]) + 1];
		}
		for (size_t k = 0; k < size_t(v.npts); ++k) start[k + 1] += start[k];
		list.assign(size_t(start.back()), 0);
		std::vector<int32_t> fill(start.begin(), start.end() - 1);
		for (int32_t i = 0; i < v.npolys; ++i) {
			int32_t c[4];
			const int nc = corners(v.polys[i], c);
			for (int k = 0; k < nc; ++k) if (c[k] >= 0 && c[k] < v.npts) list[size_t(fill[size_t(c[k])]++)] = i;
		}
		s.cornerN.assign(size_t(v.npolys) * 4, V(0, 0, 1));
		for (int32_t i = 0; i < v.npolys; ++i) {
			int32_t c[4];
			const int nc = corners(v.polys[i], c);
			const V& ni = s.polyN[size_t(i)];
			for (int k = 0; k < 4; ++k) {
				const int32_t vi = c[k < nc ? k : nc - 1];
				V sum(0.0);
				if (vi >= 0 && vi < v.npts)
					for (int32_t e = start[size_t(vi)]; e < start[size_t(vi) + 1]; ++e) {
						const int32_t q = list[size_t(e)];
						if (m::la::dot(s.polyN[size_t(q)], ni) > 0.82) sum = sum + s.polyN[size_t(q)] * area[size_t(q)];
					}
				s.cornerN[size_t(i) * 4 + size_t(k)] = Unit(sum, ni);
			}
		}
	}
	s.bvh.Build(v);
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                                    

                             
struct Map2 {
	double a = 1, b = 0, c = 0, d = 1;
	V2 operator()(const V2& p) const { return {a * p.x + b * p.y, c * p.x + d * p.y}; }
	Map2 Inverse() const
	{
		const double det = a * d - b * c;
		const double k = std::abs(det) > 1e-300 ? 1.0 / det : 0.0;
		return {d * k, -b * k, -c * k, a * k};
	}
};

enum class State : uint8_t { Intact, Loose, Missing };

struct Cell {
	std::vector<V2> v;                               
	std::vector<int> lab;                                                           
};

struct Hit {
	uint64_t key = 0;                               
	V o, n, t, b;                                        
	double R = 1.0;                                      
	double I = 1.0;                                      
	double T = 0.5;                            
	double Dc = 1.0;                                       
	double Hs = 1.0, drop = 1.0;
	Map2 A, Ai;                                                    
	V off;                                  
	         
	std::vector<V2> seeds;                
	std::vector<double> rho;                                      
	std::vector<char> zone;
	std::vector<State> st;
	std::vector<Cell> cells;                               
	                                                                              
	std::vector<V2> tc, tg;
	std::vector<double> tA, tK, tMax, pr;
	double core = 0.3;                                           
	double amp = 0.0, ov = 0.0, chipAmp = 0.0, feat = 1.0;
};

struct Ctx {
	const ConcreteHitParams* p;
	const Surf* s;
	uint64_t seed;
};

V2 ToFrame(const Hit& H, const V2& q) { return H.Ai(q); }

V World(const Hit& H, double u, double v, double h) { return H.o + H.t * u + H.b * v + H.n * h; }

                                                                          
                           
constexpr double kGraze = 0.45;

                                                                           
bool Height(const Hit& H, const Surf& s, double u, double v, double& h, int32_t& poly)
{
	double dist = 0.0;
	const V org = World(H, u, v, H.Hs);
	if (!s.Ray(org, -H.n, H.Hs + H.drop, dist, poly)) return false;
	if (m::la::dot(s.polyN[size_t(poly)], H.n) < kGraze) return false;
	h = H.Hs - dist;
	return true;
}

                                                                             
                                                                        
                                                                           
                                                                       
                              
bool Drape(const Hit& H, const Surf& s, double u, double v, V& S, V& N, int32_t& poly)
{
	double dist = 0.0;
	const V org = World(H, u, v, H.Hs);
	if (!s.Ray(org, -H.n, H.Hs + H.drop, dist, poly)) return false;
	if (m::la::dot(s.polyN[size_t(poly)], H.n) < kGraze) return false;
	S = org - H.n * dist;
	N = s.SmoothN(poly, S);
	if (m::la::dot(N, H.n) < 0.5 * kGraze) N = s.polyN[size_t(poly)];
	                                                                      
	                                                                      
	const V2 q = H.A({u, v});
	const double w = Smooth(1.6 * H.core, 2.4 * H.core, std::sqrt(q.x * q.x + q.y * q.y));
	N = Unit(H.n * (1.0 - w) + N * w, H.n);
	return true;
}

int NearestSeed(const Hit& H, const V2& q)
{
	int best = -1;
	double bd = 1e300;
	for (size_t i = 0; i < H.seeds.size(); ++i) {
		const double dx = q.x - H.seeds[i].x, dy = q.y - H.seeds[i].y;
		const double d = dx * dx + dy * dy;
		if (d < bd) { bd = d; best = int(i); }
	}
	return best;
}

                                                                           
void ClipCell(Cell& c, const V2& si, const V2& sj, int j)
{
	const V2 mid{0.5 * (si.x + sj.x), 0.5 * (si.y + sj.y)};
	const V2 nr{sj.x - si.x, sj.y - si.y};
	auto side = [&](const V2& q) { return (q.x - mid.x) * nr.x + (q.y - mid.y) * nr.y; };
	Cell o;
	const size_t n = c.v.size();
	for (size_t k = 0; k < n; ++k) {
		const V2& P = c.v[k];
		const V2& Q = c.v[(k + 1) % n];
		const double sp = side(P), sq = side(Q);
		const bool inP = sp <= 0.0, inQ = sq <= 0.0;
		auto cut = [&]() {
			const double t = sp / (sp - sq);
			return V2{P.x + (Q.x - P.x) * t, P.y + (Q.y - P.y) * t};
		};
		if (inP && inQ) { o.v.push_back(P); o.lab.push_back(c.lab[k]); }
		else if (inP && !inQ) { o.v.push_back(P); o.lab.push_back(c.lab[k]); o.v.push_back(cut()); o.lab.push_back(j); }
		else if (!inP && inQ) { o.v.push_back(cut()); o.lab.push_back(c.lab[k]); }
	}
	c = std::move(o);
}

                                                                    
double LooseDepth(const Hit& H, int i, const V2& x)
{
	if (i < 0 || size_t(i) >= H.st.size() || H.st[size_t(i)] != State::Loose) return 0.0;
	const V2& c = H.tc[size_t(i)];
	const V2& g = H.tg[size_t(i)];
	const double d = H.tA[size_t(i)] + ((x.x - c.x) * g.x + (x.y - c.y) * g.y) * H.tK[size_t(i)];
	return ClampD(d, 0.1 * H.tMax[size_t(i)], H.tMax[size_t(i)]);
}

                                                                        
                                                                         
                                                                        
                                                           
double SeamOffset(const Hit& H, const V2& A, const V2& B, double t, int i, int j)
{
	if (H.amp <= 0.0) return 0.0;
	const double tt = ClampD(t, 0.0, 1.0);
	const double taper = std::pow(std::sin(m::kPi * tt), 0.6);
	const V2 P{A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t};
	const double w = H.amp * taper * Fbm(V(P.x / H.feat, P.y / H.feat, 0.37 * std::min(i, j) + 0.11 * std::max(i, j)) + H.off, 3);
	return i < j ? w : -w;
}

                                                                           
                                                                            
                                                                    
void SeamPoints(const Hit& H, const V2& A, const V2& B, int i, int j, double step, double t0, double t1,
	bool wobble, std::vector<V2>& out, std::vector<double>* tOut = nullptr, double push = 0.0)
{
	const double ex = B.x - A.x, ey = B.y - A.y;
	const double len = std::sqrt(ex * ex + ey * ey);
	if (len <= 1e-12) return;
	const V2 nrm{ey / len, -ex / len};                            
	const int K = std::max(1, int(std::ceil(len / step)));
	auto emit = [&](double t) {
		const double w = (wobble ? SeamOffset(H, A, B, t, i, j) : 0.0) + push;
		out.push_back({A.x + ex * t + nrm.x * w, A.y + ey * t + nrm.y * w});
		if (tOut) tOut->push_back(t);
	};
	emit(t0);
	for (int k = 1; k < K; ++k) {
		const double t = double(k) / K;
		if (t > t0 + 1e-9 && t < t1 - 1e-9) emit(t);
	}
}

                                                                   
bool SimplePoly(const std::vector<V2>& q)
{
	const size_t n = q.size();
	auto orient = [](const V2& a, const V2& b, const V2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
	for (size_t i = 0; i < n; ++i) {
		const V2 &a = q[i], &b = q[(i + 1) % n];
		const double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
		for (size_t j = i + 2; j < n; ++j) {
			if (i == 0 && j + 1 == n) continue;
			const V2 &c = q[j], &d = q[(j + 1) % n];
			if (std::max(c.x, d.x) < x0 || std::min(c.x, d.x) > x1 || std::max(c.y, d.y) < y0 || std::min(c.y, d.y) > y1) continue;
			const double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
			if (((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0))) return false;
		}
	}
	return true;
}

struct Out { std::vector<m::Manifold> cutters, pushed, cracks, gaps; };

                                                                                       

void BuildCrater(const Ctx& cx, const Hit& H, Out& out)
{
	const ConcreteHitParams& p = *cx.p;
	const Surf& s = *cx.s;
	                                                        
	double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
	for (int k = 0; k < 64; ++k) {
		const double a = 2.0 * m::kPi * k / 64.0;
		const double rr = (1.15 + 0.6 * ClampD(p.coreJagged, 0.0, 2.0)) * H.core;
		const V2 q = ToFrame(H, {rr * std::cos(a), rr * std::sin(a)});
		u0 = std::min(u0, q.x); u1 = std::max(u1, q.x); v0 = std::min(v0, q.y); v1 = std::max(v1, q.y);
	}
	const double extent = std::max(u1 - u0, v1 - v0);
	if (!(extent > 0.0)) return;
	double cell = std::max(extent / (90.0 * p.detail), extent / 400.0);
	const int nu = std::max(4, int(std::ceil((u1 - u0) / cell)));
	const int nv = std::max(4, int(std::ceil((v1 - v0) / cell)));
	const double cu = (u1 - u0) / nu, cv = (v1 - v0) / nv;
	cell = std::max(cu, cv);
	auto I = [&](int i, int j) { return size_t(i) * size_t(nv + 1) + size_t(j); };
	const size_t N = size_t(nu + 1) * size_t(nv + 1);
	std::vector<double> h(N, 0.0), thick(N, -1.0), d(N, 0.0);
	std::vector<char> valid(N, 0), forbid(N, 0), outside(N, 0);
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double u = u0 + i * cu, v = v0 + j * cv;
			int32_t poly = -1;
			if (!Height(H, s, u, v, h[k], poly)) continue;
			valid[k] = 1;
			forbid[k] = s.Avoided(poly) ? 1 : 0;
			double th = 0.0;
			int32_t tp = -1;
			if (s.Ray(World(H, u, v, h[k]) - H.n * (1e-5 * s.diag), -H.n, 2.0 * s.diag, th, tp)) thick[k] = th;
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
	const double coreR = H.core * H.R;
	const double jag = ClampD(p.coreJagged, 0.0, 2.0);
	const double agg = std::max(1e-3, p.aggregateSize);
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double margin = std::max(0.3 * (H.Dc + H.T), 2.0 * cell);
	const double lift = 0.4 * margin;
	const double holeR = ClampD(p.holeSize, 0.0, 1.0);
	bool anyIn = false;
	for (int i = 0; i <= nu; ++i)
		for (int j = 0; j <= nv; ++j) {
			const size_t k = I(i, j);
			const double u = u0 + i * cu, v = v0 + j * cv;
			const V2 q = H.A({u, v});
			const V W = World(H, u, v, h[k]);
			const double r = std::sqrt(q.x * q.x + q.y * q.y) / H.core;
			const double re = r * (1.0 + jag * 0.5 * Fbm(W / (0.55 * coreR) + H.off + V(3.1, -7.7, 1.9), 4))
				+ jag * 0.1 * Fbm(W / (0.12 * coreR) + H.off, 2);
			if (re >= 1.0 || forbid[k]) { outside[k] = 1; continue; }
			const double prof = std::pow(1.0 - re * re, 0.55);
			double dd = H.T * 1.02 + H.Dc * prof;
			if (p.roughness > 0.0) {
				const double big = Fbm(W / (0.4 * coreR) + H.off + V(5.5, 1.5, 9.5), 3);
				const double pit = 1.0 - 2.0 * std::abs(Fbm(W / agg + H.off + V(-3.3, 8.1, 2.2), 3));
				dd += ClampD(p.roughness, 0.0, 2.0) * 0.4 * H.Dc * (0.5 * big + 0.5 * pit) * Smooth(0.0, 0.25, 1.0 - re);
				dd = std::max(dd, 0.8 * H.T);
			}
			if (thick[k] > 0.0) dd = std::min(dd, std::max(H.T * 1.02, cap * thick[k]));
			if (p.holeEnable && holeR > 0.0 && re < holeR) {
				const double f = Smooth(holeR, 0.75 * holeR, re);
				const double through = (thick[k] > 0.0 ? thick[k] : 3.0 * H.Dc + extent) + margin;
				dd = Lerp(dd, through, f);
			}
			d[k] = dd;
			anyIn = true;
		}
	if (!anyIn) return;
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
			top[k] = World(H, u, v, hmax + margin);
			bot[k] = World(H, u, v, outside[k] ? hmax + lift : h[k] - d[k]);
		}
	m::Manifold c = SlabSolid(top, bot, nu, nv);
	if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
}

                                                                                          

                                                                             
                                                                      
                                                        
void BuildPlate(const Ctx& cx, const Hit& H, int i, Out& out)
{
	const ConcreteHitParams& p = *cx.p;
	const Surf& s = *cx.s;
	const Cell& c = H.cells[size_t(i)];
	const size_t n = c.v.size();
	if (n < 3) return;
	const bool missing = H.st[size_t(i)] == State::Missing;
	auto rnd = [&](uint64_t ch) { return Random01(H.key, 50000 + uint64_t(i) * 8, ch); };

	std::vector<V2> F(n);
	for (size_t k = 0; k < n; ++k) F[k] = ToFrame(H, c.v[k]);
	                          
	std::vector<double> off(n, 0.0);
	std::vector<char> wob(n, 0);
	for (size_t k = 0; k < n; ++k) {
		const int j = c.lab[k];
		const bool nMissing = j >= 0 && H.st[size_t(j)] == State::Missing;
		if (nMissing) off[k] = H.ov;
		else {
			wob[k] = 1;
			                                                                    
			                                                                   
			if (!missing && j >= 0 && H.st[size_t(j)] == State::Loose) off[k] = 0.3 * p.crackWidth;
		}
	}
	                                                                           
	                                                                          
	                                                                           
	std::vector<V2> poly, G(n);
	const double step = std::max(0.05 * H.pr[size_t(i)], 0.6 * p.crackWidth) / std::sqrt(p.detail);
	for (int attempt = 0; attempt < 3; ++attempt) {
	poly.clear();
	if (attempt >= 1) std::fill(wob.begin(), wob.end(), 0);
	if (attempt == 2) std::fill(off.begin(), off.end(), 0.0);
	                                                           
	for (size_t k = 0; k < n; ++k) {
		const size_t a = (k + n - 1) % n;                                                  
		const V2 da{F[k].x - F[a].x, F[k].y - F[a].y}, db{F[(k + 1) % n].x - F[k].x, F[(k + 1) % n].y - F[k].y};
		const double la = std::sqrt(da.x * da.x + da.y * da.y), lb = std::sqrt(db.x * db.x + db.y * db.y);
		if (la < 1e-12 || lb < 1e-12) { G[k] = F[k]; continue; }
		const V2 na{da.y / la, -da.x / la}, nb{db.y / lb, -db.x / lb};
		                                                                       
		                                                
		const double det = na.x * nb.y - na.y * nb.x;
		if (std::abs(det) < 1e-6) {
			const double o = std::max(off[a], off[k]);
			G[k] = {F[k].x + nb.x * o, F[k].y + nb.y * o};
			continue;
		}
		const double x = (off[a] * nb.y - off[k] * na.y) / det;
		const double y = (na.x * off[k] - nb.x * off[a]) / det;
		G[k] = {F[k].x + x, F[k].y + y};
	}
	          
	for (size_t k = 0; k < n; ++k) {
		const V2& A = F[k];
		const V2& B = F[(k + 1) % n];
		const double ex = B.x - A.x, ey = B.y - A.y;
		const double l2 = ex * ex + ey * ey;
		if (l2 < 1e-18) continue;
		                                                               
		const double t0 = ((G[k].x - A.x) * ex + (G[k].y - A.y) * ey) / l2;
		const double t1 = ((G[(k + 1) % n].x - A.x) * ex + (G[(k + 1) % n].y - A.y) * ey) / l2;
		if (!wob[k]) {
			const double len = std::sqrt(l2);
			const int K = std::max(1, int(std::ceil(len / step)));
			const V2 Ga = G[k], Gb = G[(k + 1) % n];
			for (int q = 0; q < K; ++q) {
				const double t = double(q) / K;
				poly.push_back({Ga.x + (Gb.x - Ga.x) * t, Ga.y + (Gb.y - Ga.y) * t});
			}
			continue;
		}
		const size_t first = poly.size();
		std::vector<double> ts;
		SeamPoints(H, A, B, i, c.lab[k], step, t0, t1, true, poly, &ts, off[k]);
		                                                       
		if (missing && H.chipAmp > 0.0) {
			const double len = std::sqrt(l2);
			const V2 nrm{ey / len, -ex / len};
			for (size_t q = first; q < poly.size(); ++q) {
				const double t = ts[q - first];
				const V2 P{A.x + ex * t, A.y + ey * t};
				const double f = Fbm(V(P.x / (0.35 * H.feat), P.y / (0.35 * H.feat), 9.7 + 0.01 * i) + H.off, 2);
				const double bite = H.chipAmp * Smooth(0.15, 0.55, f) * std::pow(std::sin(m::kPi * ClampD(t, 0.0, 1.0)), 0.5);
				poly[q].x += nrm.x * bite;
				poly[q].y += nrm.y * bite;
			}
		}
	}
	if (SimplePoly(poly)) break;
	}
	if (poly.size() < 3 || !SimplePoly(poly)) return;
	double area = 0.0;
	for (size_t a = 0, b = poly.size() - 1; a < poly.size(); b = a++) area += poly[b].x * poly[a].y - poly[a].x * poly[b].y;
	if (area <= 0.0) return;

	m::Polygons cs(1);
	for (const V2& q : poly) cs[0].push_back(m::vec2(q.x, q.y));
	m::Manifold ex = m::Manifold::Extrude(cs, 1.0);
	if (ex.Status() != m::Manifold::Error::NoError || ex.IsEmpty()) return;
	ex = ex.RefineToLength(std::max(0.2 * H.pr[size_t(i)], 4.0 * step) / std::sqrt(p.detail));
	m::MeshGL mesh = ex.GetMeshGL();
	const size_t nvtx = mesh.vertProperties.size() / mesh.numProp;
	std::vector<V> SS(nvtx), NN(nvtx);
	std::vector<char> ok(nvtx, 0), av(nvtx, 0);
	double hsum = 0.0;
	int hcnt = 0;
	for (size_t k = 0; k < nvtx; ++k) {
		const double u = mesh.vertProperties[k * mesh.numProp], v = mesh.vertProperties[k * mesh.numProp + 1];
		int32_t poly2 = -1;
		if (Drape(H, s, u, v, SS[k], NN[k], poly2)) {
			ok[k] = 1;
			av[k] = s.Avoided(poly2) ? 1 : 0;
			hsum += m::la::dot(SS[k] - H.o, H.n);
			++hcnt;
		}
	}
	if (hcnt == 0) return;
	                                                                        
	                                                                    
	const double hfill = hsum / hcnt;
	const double margin = std::max(H.T, 0.08 * H.pr[size_t(i)]);
	const double depthMiss = H.T;
	const V2 noiseOff{rnd(0) * 100.0, rnd(1) * 100.0};
	for (size_t k = 0; k < nvtx; ++k) {
		const double u = mesh.vertProperties[k * mesh.numProp], v = mesh.vertProperties[k * mesh.numProp + 1];
		const double z = mesh.vertProperties[k * mesh.numProp + 2];
		const V S = ok[k] ? SS[k] : World(H, u, v, hfill);
		const V N = ok[k] ? NN[k] : H.n;
		                                                                       
		                                                                       
		                                      
		double dep;
		if (missing)
			dep = depthMiss * (1.0 + 0.12 * Fbm(V(u / (0.4 * H.pr[size_t(i)]) + noiseOff.x, v / (0.4 * H.pr[size_t(i)]) + noiseOff.y, 0.5), 2));
		else
			dep = LooseDepth(H, i, {u, v});
		if (!ok[k] || av[k]) dep = -0.3 * margin;                        
		const V P = S + N * Lerp(-dep, margin, ClampD(z, 0.0, 1.0));
		mesh.vertProperties[k * mesh.numProp] = float(P.x);
		mesh.vertProperties[k * mesh.numProp + 1] = float(P.y);
		mesh.vertProperties[k * mesh.numProp + 2] = float(P.z);
	}
	m::MeshGL clean;
	clean.numProp = 3;
	clean.vertProperties.reserve(nvtx * 3);
	for (size_t k = 0; k < nvtx; ++k)
		for (int q = 0; q < 3; ++q) clean.vertProperties.push_back(mesh.vertProperties[k * mesh.numProp + size_t(q)]);
	clean.triVerts = mesh.triVerts;
	m::Manifold solid = Orient(clean);
	if (!solid.IsEmpty()) (missing ? out.cutters : out.pushed).push_back(std::move(solid));
}

                                                                                            

                                                                              
                                                                    
void CrackStrip(const Hit& H, const Surf& s, const std::vector<V2>& pts, const std::vector<double>& w,
	const std::vector<double>& dep, Out& out)
{
	std::vector<V> topv, botv;
	int rows = 0;
	V prevS(0.0);
	auto flush = [&]() {
		if (rows >= 2) {
			m::Manifold c = SlabSolid(topv, botv, rows - 1, 2);
			if (!c.IsEmpty()) out.cracks.push_back(std::move(c));
		}
		topv.clear(); botv.clear(); rows = 0;
	};
	for (size_t i = 0; i < pts.size(); ++i) {
		V S, N;
		int32_t poly = -1;
		if (!Drape(H, s, pts[i].x, pts[i].y, S, N, poly) || s.Avoided(poly)) {
			flush();
			continue;
		}
		const V2 a = pts[i > 0 ? i - 1 : i], b = pts[i + 1 < pts.size() ? i + 1 : i];
		const V dirW = Unit(H.t * (b.x - a.x) + H.b * (b.y - a.y), H.t);
		                                                                      
		                                                    
		if (rows > 0) {
			const V2 pa = pts[i - 1];
			const double dF = std::sqrt((pts[i].x - pa.x) * (pts[i].x - pa.x) + (pts[i].y - pa.y) * (pts[i].y - pa.y));
			if (m::la::length(S - prevS) > 3.0 * dF / kGraze + 1e-6 * s.diag) flush();
		}
		                                                                          
		const V tan = Unit(dirW - H.n * (m::la::dot(dirW, N) / std::max(1e-6, m::la::dot(H.n, N))), dirW);
		const V side = Unit(m::la::cross(N, tan), H.b);
		const double hw = 0.5 * w[i] + 1e-4;
		                                                                      
		                                        
		const double mc = 0.25 * dep[i] + 2.0 * hw + 1.5 * H.T;
		topv.push_back(S - side * hw + N * mc); topv.push_back(S + N * mc); topv.push_back(S + side * hw + N * mc);
		botv.push_back(S - side * hw + N * (0.3 * mc)); botv.push_back(S - N * dep[i]); botv.push_back(S + side * hw + N * (0.3 * mc));
		prevS = S;
		++rows;
	}
	flush();
}

                                                                           
void BuildSeam(const Ctx& cx, const Hit& H, int i, size_t k, Out& out)
{
	const ConcreteHitParams& p = *cx.p;
	const Cell& c = H.cells[size_t(i)];
	const int j = c.lab[k];
	const V2 A = ToFrame(H, c.v[k]), B = ToFrame(H, c.v[(k + 1) % c.v.size()]);
	const double step = std::max(1.2 * p.crackWidth, 0.04 * H.pr[size_t(i)]) / std::sqrt(p.detail);
	std::vector<V2> pts;
	std::vector<double> ts;
	SeamPoints(H, A, B, i, j, step, 0.0, 1.0, true, pts, &ts);
	pts.push_back(B);                                      
	ts.push_back(1.0);
	                                                               
	{
		const double ex = B.x - A.x, ey = B.y - A.y, l = std::sqrt(ex * ex + ey * ey);
		if (l < 1e-9) return;
		const double e = 0.5 * p.crackWidth / l;
		pts.insert(pts.begin(), {A.x - ex * e, A.y - ey * e});
		ts.insert(ts.begin(), 0.0);
		pts.push_back({B.x + ex * e, B.y + ey * e});
		ts.push_back(1.0);
	}
	const V2 mid{0.5 * (c.v[k].x + c.v[(k + 1) % c.v.size()].x), 0.5 * (c.v[k].y + c.v[(k + 1) % c.v.size()].y)};
	const double rr = std::sqrt(mid.x * mid.x + mid.y * mid.y);
	const double wBase = p.crackWidth * Lerp(1.35, 0.8, ClampD(rr, 0.0, 1.0));
	std::vector<double> w(pts.size()), dep(pts.size());
	for (size_t q = 0; q < pts.size(); ++q) {
		const V2& P = pts[q];
		const double var = 1.0 + 0.3 * Fbm(V(P.x / H.feat, P.y / H.feat, 4.2) + H.off, 2);
		w[q] = wBase * var;
		dep[q] = std::max(p.crackDepth, 1.05 * H.T) + std::max(LooseDepth(H, i, P), LooseDepth(H, j, P));
	}
	CrackStrip(H, *cx.s, pts, w, dep, out);
}

struct Walk { V2 x; double ang, len, w, dep; uint64_t key; int gen; };

                                                                           
                      
void BuildWalk(const Ctx& cx, const Hit& H, Walk first, Out& out)
{
	const ConcreteHitParams& p = *cx.p;
	std::vector<Walk> todo{first};
	for (size_t ti = 0; ti < todo.size() && ti < 8; ++ti) {
		const Walk sd = todo[ti];
		auto rnd = [&](uint64_t ch) { return Random01(H.key, sd.key, ch); };
		const double step = std::max(1.5 * sd.w, sd.len / 48.0) / std::sqrt(p.detail);
		const int ns = std::max(2, int(std::ceil(sd.len / step)));
		const V noff(rnd(8) * 1000.0, rnd(9) * 1000.0, 0.0);
		std::vector<V2> pts;
		std::vector<double> w, dep;
		double ang = sd.ang;
		V2 x = sd.x;
		const double forkAt = Lerp(0.3, 0.7, rnd(10));
		const bool fork = sd.gen == 0 && rnd(11) < ClampD(p.radialBranch, 0.0, 1.0);
		for (int i = 0; i <= ns; ++i) {
			const double f = double(i) / ns;
			if (i > 0) {
				ang += ClampD(p.radialMeander, 0.0, 2.0) * 2.2 * Fbm(V(f * 4.0, 0.5, 0.0) + noff, 3) * (step / (0.2 * sd.len + 1e-6));
				x.x += std::cos(ang) * step;
				x.y += std::sin(ang) * step;
			}
			const double taper = 1.0 - 0.85 * std::pow(f, 1.5);
			pts.push_back(x);
			w.push_back(sd.w * taper + 1e-4);
			dep.push_back(sd.dep * (1.0 - 0.7 * std::pow(f, 1.5)));
			if (fork && i > 0 && i < ns && std::abs(f - forkAt) < 0.5 / ns) {
				const double sgn = rnd(12) < 0.5 ? -1.0 : 1.0;
				todo.push_back({x, ang + sgn * Lerp(0.4, 0.85, rnd(13)), sd.len * (1.0 - f) * Lerp(0.4, 0.8, rnd(14)),
					sd.w * taper * 0.8, sd.dep * 0.8, sd.key + 1000, sd.gen + 1});
			}
		}
		CrackStrip(H, *cx.s, pts, w, dep, out);
	}
}

}               

namespace {
                                                                            
                                                                          
                                            
bool PrepareHits(const CoreInput& in, const ConcreteHitParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, ConcreteHitParams& p, Surf& s, uint64_t& seedOut, std::vector<Hit>& hits)
{
	if (!pIn.enable || pIn.empties.empty()) return false;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return false;
	p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.layerThickness = std::max(1e-3, p.layerThickness);
	p.crackWidth = std::max(1e-3, p.crackWidth);
	p.crackDepth = std::max(0.0, p.crackDepth);
	p.coreSize = ClampD(p.coreSize, 0.0, 0.95);
	p.plateSize = ClampD(p.plateSize, 0.05, 2.0);
	p.radialCount = std::max(0, std::min(p.radialCount, 64));
	p.ringCount = std::max(0, std::min(p.ringCount, 8));
	if (p.radialLengthMax < p.radialLengthMin) std::swap(p.radialLengthMin, p.radialLengthMax);

	BuildSurf(in, p.avoid, s);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x434f4e4348495421ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4348;                             
	seedOut = seed;

	hits.reserve(p.empties.size());
	for (size_t ei = 0; ei < p.empties.size(); ++ei) {
		const ConcreteHitEmpty& E = p.empties[ei];
		bool finite = std::isfinite(E.size) && std::isfinite(E.strength);
		for (double q : E.rel) finite = finite && std::isfinite(q);
		if (!finite || E.strength <= 0.0 || E.size <= 0.0) continue;
		const V O(E.rel[0], E.rel[1], E.rel[2]);
		const V X(E.rel[3], E.rel[4], E.rel[5]), Y(E.rel[6], E.rel[7], E.rel[8]), Z(E.rel[9], E.rel[10], E.rel[11]);
		const double rx0 = E.size * m::la::length(X) * p.radiusScale;
		const double ry0 = E.size * m::la::length(Y) * p.radiusScale;
		if (!(rx0 > 1e-6) || !(ry0 > 1e-6)) continue;
		Hit H;
		H.key = seed ^ ((uint64_t(ei) + 1) * 0xa24baed4963ee407ULL);
		auto rnd = [&](uint64_t ch) { return Random01(H.key, 0, ch); };

		               
		V S;
		int32_t poly = -1;
		bool found = false;
		V shot = Unit(Z, V(0, 0, 1));
		if (p.aim == 1) {
			double d = 0.0;
			if (s.Ray(O, shot, 4.0 * s.diag, d, poly)) { S = O + shot * d; found = true; }
		}
		if (!found) {
			found = s.Nearest(O, S, poly);
			shot = Unit(S - O, -s.polyN[size_t(std::max(0, poly))]);
		}
		if (!found || poly < 0) continue;
		if (s.Avoided(poly)) continue;
		H.n = s.polyN[size_t(poly)];
		const double dist = m::la::length(S - O);
		const double R0 = 0.5 * (rx0 + ry0);
		double I = ClampD(E.strength, 0.0, 4.0);
		if (p.proximity > 0.0) I *= 1.0 - Smooth(0.25 * p.proximity * R0, p.proximity * R0, dist);
		if (I <= 0.02) continue;
		if (mask && !mask->Keep(S, int32_t(ei), kStream)) continue;
		const double sc = mask ? mask->SizeScale(S) : 1.0;
		if (sc <= 0.0) continue;
		const double sizeF = sc * Lerp(1.0, std::sqrt(std::min(I, 1.0)), ClampD(p.proxShrink, 0.0, 1.0));
		const double rx = rx0 * sizeF, ry = ry0 * sizeF;
		H.I = std::min(I, 1.5);
		H.R = 0.5 * (rx + ry);
		H.o = S;
		                                                          
		V tx = X - H.n * m::la::dot(X, H.n);
		if (m::la::length(tx) < 0.2 * m::la::length(X)) tx = m::la::cross(Y - H.n * m::la::dot(Y, H.n), H.n);
		H.t = Unit(tx, AnyPerp(H.n));
		H.b = m::la::cross(H.n, H.t);
		                                                            
		Map2 Dm{1.0 / rx, 0.0, 0.0, 1.0 / ry};
		Map2 St;
		if (p.aim == 1 && p.oblique > 0.0) {
			const V sp = shot - H.n * m::la::dot(shot, H.n);
			const double sn = m::la::length(sp);
			if (sn > 1e-3) {
				const double dx = m::la::dot(sp, H.t) / sn, dy = m::la::dot(sp, H.b) / sn;
				const double e = 1.0 + ClampD(p.oblique, 0.0, 3.0) * sn;
				const double k = 1.0 / e - 1.0;
				St = {1.0 + k * dx * dx, k * dx * dy, k * dx * dy, 1.0 + k * dy * dy};
			}
		}
		H.A = {Dm.a * St.a + Dm.b * St.c, Dm.a * St.b + Dm.b * St.d, Dm.c * St.a + Dm.d * St.c, Dm.c * St.b + Dm.d * St.d};
		H.Ai = H.A.Inverse();
		H.T = p.layerThickness * std::min(1.0, 0.4 + 0.6 * H.I);
		H.Dc = std::max(0.0, p.coreDepth) * H.R * H.I;
		const double Rmax = std::max(rx, ry) * (1.0 + ClampD(p.oblique, 0.0, 3.0));
		H.Hs = 0.35 * Rmax + 2.0 * (H.Dc + H.T) + 1e-3;
		H.drop = 0.8 * Rmax + 4.0 * (H.Dc + H.T) + p.crackDepth;
		H.off = V(rnd(10) * 1000.0, rnd(11) * 1000.0, rnd(12) * 1000.0);
		H.core = p.coreSize;
		const double plateR = 0.5 * p.plateSize * H.R;
		H.feat = std::max(1e-3, 0.45 * plateR);
		H.amp = ClampD(p.edgeJagged, 0.0, 2.0) * 0.14 * plateR;
		H.ov = std::max(1.5 * H.amp + p.crackWidth, 0.08 * plateR);
		H.chipAmp = ClampD(p.chip, 0.0, 1.0) * 0.3 * plateR;

		                                                             
		const double ps = p.plateSize;
		const double growth = ClampD(p.plateGrowth, 0.0, 1.0);
		const double outerR = 1.75;
		const int cand = int(std::min(30000.0, 400.0 * (outerR * outerR) / (ps * ps)));
		for (int k = 0; k < cand && H.seeds.size() < 900; ++k) {
			const double x = (Random01(H.key, 20 + uint64_t(k) * 4, 0) * 2.0 - 1.0) * outerR;
			const double y = (Random01(H.key, 20 + uint64_t(k) * 4, 1) * 2.0 - 1.0) * outerR;
			const double r = std::sqrt(x * x + y * y);
			if (r > outerR) continue;
			const double rmin = 0.85 * ps * Lerp(1.0 - 0.7 * growth, 1.0, Smooth(0.0, 1.1, r));
			bool clash = false;
			for (const V2& q : H.seeds) {
				const double dx = q.x - x, dy = q.y - y;
				if (dx * dx + dy * dy < rmin * rmin) { clash = true; break; }
			}
			if (!clash) H.seeds.push_back({x, y});
		}
		const size_t ns = H.seeds.size();
		if (ns < 3) continue;
		H.rho.resize(ns);
		H.zone.assign(ns, 0);
		H.st.assign(ns, State::Intact);
		H.cells.resize(ns);
		H.tc.assign(ns, V2{}); H.tg.assign(ns, V2{});
		H.tA.assign(ns, 0.0); H.tK.assign(ns, 0.0); H.tMax.assign(ns, 0.0); H.pr.assign(ns, plateR);
		const double zj = ClampD(p.zoneJagged, 0.0, 1.0);
		bool anyZone = false;
		for (size_t i = 0; i < ns; ++i) {
			const V2& q = H.seeds[i];
			const double r = std::sqrt(q.x * q.x + q.y * q.y);
			H.rho[i] = r + zj * 0.45 * Fbm(V(q.x * 1.3, q.y * 1.3, 0.0) + H.off, 2);
			H.zone[i] = H.rho[i] < 1.0 || r < 0.5 * ps ? 1 : 0;
			anyZone |= H.zone[i] != 0;
		}
		if (!anyZone) continue;
		const double tiltAmt = std::max(0.0, p.tilt);
		for (size_t i = 0; i < ns; ++i) {
			if (!H.zone[i]) continue;
			Cell& c = H.cells[i];
			c.v = {{-2.5, -2.5}, {2.5, -2.5}, {2.5, 2.5}, {-2.5, 2.5}};
			c.lab = {-1, -1, -1, -1};
			for (size_t j = 0; j < ns && c.v.size() >= 3; ++j)
				if (j != i) ClipCell(c, H.seeds[i], H.seeds[j], int(j));
			if (c.v.size() < 3) { H.zone[i] = 0; continue; }
			                        
			double A2 = 0.0, cxs = 0.0, cys = 0.0;
			for (size_t a = 0, b = c.v.size() - 1; a < c.v.size(); b = a++) {
				const V2 P = ToFrame(H, c.v[b]), Q = ToFrame(H, c.v[a]);
				const double cr = P.x * Q.y - Q.x * P.y;
				A2 += cr; cxs += (P.x + Q.x) * cr; cys += (P.y + Q.y) * cr;
			}
			if (A2 <= 1e-12) { H.zone[i] = 0; continue; }
			const V2 cen{cxs / (3.0 * A2), cys / (3.0 * A2)};
			H.pr[i] = std::sqrt(0.5 * A2 / m::kPi);
			auto prnd = [&](uint64_t ch) { return Random01(H.key, 50000 + uint64_t(i) * 8, ch); };
			                                                                       
			                                                                 
			                                  
			{
				V S0, N0;
				int32_t pc = -1;
				bool carried = Drape(H, s, cen.x, cen.y, S0, N0, pc) && !s.Avoided(pc);
				for (size_t a = 0; a < c.v.size() && carried; ++a) {
					const V2& q0 = c.v[a];
					const V2& q1 = c.v[(a + 1) % c.v.size()];
					const V2 f0 = ToFrame(H, q0), fm = ToFrame(H, {0.5 * (q0.x + q1.x), 0.5 * (q0.y + q1.y)});
					carried = Drape(H, s, f0.x, f0.y, S0, N0, pc) && Drape(H, s, fm.x, fm.y, S0, N0, pc);
				}
				if (!carried) { H.zone[i] = 0; continue; }
			}
			const double rho = H.rho[i];
			const double pm = ClampD(p.shatter, 0.0, 1.0) * Lerp(1.0, 0.25, Smooth(H.core, 1.0, rho)) * Lerp(0.6, 1.0, std::min(1.0, H.I));
			const bool inCore = rho < 0.75 * H.core;
			if (inCore || prnd(2) < pm) H.st[i] = State::Missing;
			else {
				H.st[i] = State::Loose;
				                                           
				const double len = std::sqrt(cen.x * cen.x + cen.y * cen.y);
				V2 g = len > 1e-9 ? V2{-cen.x / len, -cen.y / len} : V2{1.0, 0.0};
				const double ra = (prnd(3) - 0.5) * 1.3;
				g = {g.x * std::cos(ra) - g.y * std::sin(ra), g.x * std::sin(ra) + g.y * std::cos(ra)};
				const double pin = H.T * tiltAmt * std::min(1.0, H.I) * Lerp(1.0, 0.3, Smooth(H.core, 1.1, rho)) * Lerp(0.5, 1.2, prnd(4));
				H.tc[i] = cen;
				H.tg[i] = g;
				H.tMax[i] = pin;
				H.tA[i] = 0.55 * pin;
				H.tK[i] = 0.45 * pin / std::max(1e-6, H.pr[i]);
			}
		}
		hits.push_back(std::move(H));
	}
	return !hits.empty();
}

                                                                           
                                                 
void OuterCrackJobs(const Ctx& cx, const Hit* Hp, std::vector<std::function<void(Out&)>>& jobs)
{
	const ConcreteHitParams& p = *cx.p;
	const Hit& H = *Hp;
	                                
	const int nr = int(std::round(p.radialCount * Lerp(0.5, 1.0, std::min(1.0, H.I))));
	for (int r = 0; r < nr; ++r) {
		const uint64_t key = 60000 + uint64_t(r) * 32;
		auto rnd = [&](uint64_t ch) { return Random01(H.key, key, ch); };
		const double th = 2.0 * m::kPi * (r + 0.75 * rnd(0)) / nr;
		const V2 dirN{std::cos(th), std::sin(th)};
		double rr = 0.0;
		for (; rr < 1.75; rr += 0.01) {
			const int si = NearestSeed(H, {dirN.x * rr, dirN.y * rr});
			if (si < 0 || !H.zone[size_t(si)]) break;
		}
		const V2 x0 = ToFrame(H, {dirN.x * (rr - 0.02), dirN.y * (rr - 0.02)});
		const V2 dF = ToFrame(H, dirN);
		Walk w;
		w.x = x0;
		w.ang = std::atan2(dF.y, dF.x) + (rnd(1) - 0.5) * 0.5;
		w.len = Lerp(p.radialLengthMin, p.radialLengthMax, std::pow(rnd(2), 1.3)) * H.R;
		w.w = p.crackWidth * Lerp(1.0, 1.5, rnd(3));
		w.dep = std::max(p.crackDepth, 1.05 * H.T) * Lerp(0.9, 1.3, rnd(4));
		w.key = key;
		w.gen = 0;
		if (w.len > 4.0 * p.crackWidth) jobs.push_back([&cx, Hp, w](Out& o) { BuildWalk(cx, *Hp, w, o); });
	}
	                          
	for (int rg = 0; rg < p.ringCount; ++rg) {
		const uint64_t key = 70000 + uint64_t(rg) * 64;
		auto rnd = [&](uint64_t ch) { return Random01(H.key, key, ch); };
		const double ringR = Lerp(1.15, 1.6, (rg + 0.5) / p.ringCount) + (rnd(0) - 0.5) * 0.1;
		const int arcs = 9;
		const double rot = rnd(1) * 2.0 * m::kPi;
		for (int a = 0; a < arcs; ++a) {
			if (rnd(2 + uint64_t(a)) >= ClampD(p.ringFill, 0.0, 1.0)) continue;
			const double a0 = rot + 2.0 * m::kPi * a / arcs, a1 = a0 + 2.0 * m::kPi / arcs * Lerp(0.7, 1.05, rnd(20 + uint64_t(a)));
			const double arcLen = (a1 - a0) * ringR * H.R;
			const int K = std::max(4, int(std::ceil(arcLen / std::max(1.5 * p.crackWidth, arcLen / 40.0) * std::sqrt(p.detail))));
			std::vector<V2> pts;
			std::vector<double> w, dep;
			const double wb = p.crackWidth * Lerp(0.6, 1.0, rnd(40 + uint64_t(a)));
			const double db = std::max(p.crackDepth, 1.05 * H.T) * 0.8;
			for (int q = 0; q <= K; ++q) {
				const double f = double(q) / K, ang = Lerp(a0, a1, f);
				const double rj = ringR * (1.0 + 0.07 * Fbm(V(std::cos(ang) * 2.0, std::sin(ang) * 2.0, 3.0 + rg) + H.off, 3));
				pts.push_back(ToFrame(H, {std::cos(ang) * rj, std::sin(ang) * rj}));
				const double tp = std::pow(std::sin(m::kPi * f), 0.5);
				w.push_back(wb * (0.25 + 0.75 * tp));
				dep.push_back(db * (0.3 + 0.7 * tp));
			}
			jobs.push_back([&cx, Hp, pts, w, dep](Out& o) { CrackStrip(*Hp, *cx.s, pts, w, dep, o); });
		}
	}
}
                                                                            
                                                             
                                                                             
                                                                   
                                                     
                                                                            
                                                                            
                                                                         
                                                                          
                                                                  
void BuildThrough(const Ctx& cx, const Hit& H, Out& out)
{
	const ConcreteHitParams& p = *cx.p;
	const Surf& s = *cx.s;
	using CS = m::CrossSection;
	const double gap = std::max(1e-3, p.throughGap);
	std::vector<CS> zoneParts, pieceParts;
	for (size_t i = 0; i < H.seeds.size(); ++i) {
		if (!H.zone[i]) continue;
		const Cell& c = H.cells[i];
		if (c.v.size() < 3) continue;
		m::SimplePolygon poly;
		for (const V2& q : c.v) {
			const V2 f = ToFrame(H, q);
			poly.push_back(m::vec2(f.x, f.y));
		}
		CS cell(m::Polygons{poly}, CS::FillRule::NonZero);
		if (cell.Area() <= 1e-12) continue;
		zoneParts.push_back(cell);
		                                                       
		const bool inCore = H.rho[i] < 0.75 * H.core;
		const bool removed = inCore || Random01(H.key, 50000 + uint64_t(i) * 8, 6) < ClampD(p.throughRemove, 0.0, 1.0);
		if (removed) continue;
		CS piece = cell.Offset(-0.5 * gap, CS::JoinType::Miter, 3.0);
		if (piece.Area() > 1e-12) pieceParts.push_back(piece);
	}
	if (zoneParts.empty()) return;
	const CS zone = CS::BatchBoolean(zoneParts, m::OpType::Add).Offset(0.5 * gap, CS::JoinType::Miter, 3.0);
	CS gapsCS = pieceParts.empty() ? zone : zone - CS::BatchBoolean(pieceParts, m::OpType::Add);
	if (gapsCS.Area() <= 1e-12) return;

	                                                     
	double wall = 0.0;
	int32_t poly = -1;
	const bool hasBack = s.Ray(H.o - H.n * (1e-4 * H.R), -H.n, 1e9, wall, poly);
	const bool through = hasBack && wall <= std::max(0.0, p.throughMaxWall);
	const double top = H.Hs;                                                    
	const double depth = through ? wall + std::max(0.5, 0.25 * wall)
		: std::max(0.05 * H.R, ClampD(p.chunkDepth, 0.01, 4.0) * H.R);

	                                                 
	const m::mat3x4 frame({H.t.x, H.t.y, H.t.z}, {H.b.x, H.b.y, H.b.z}, {H.n.x, H.n.y, H.n.z}, {H.o.x, H.o.y, H.o.z});
	auto prism = [&](const CS& cs, double z0, double z1) {
		return m::Manifold::Extrude(cs.ToPolygons(), z1 - z0).Translate({0.0, 0.0, z0}).Transform(frame);
	};
	std::vector<m::Manifold> parts;
	parts.push_back(prism(gapsCS, -depth, top));
	if (!through)
		parts.push_back(prism(zone, -depth - gap, -depth + 1e-4 * gap));                              
	m::Manifold g = parts.size() == 1 ? parts[0] : m::Manifold::BatchBoolean(parts, m::OpType::Add);
	if (g.Status() == m::Manifold::Error::NoError && g.NumTri() > 0) out.gaps.push_back(std::move(g));
}
}               

void PlaceConcreteHit(const CoreInput& in, const ConcreteHitParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& pushed,
	std::vector<m::Manifold>& cracks)
{
	ConcreteHitParams p;
	Surf s;
	uint64_t seed = 0;
	std::vector<Hit> hits;
	if (!PrepareHits(in, pIn, mainSeed, mask, p, s, seed, hits)) return;
	Ctx cx{&p, &s, seed};

	                                                      
	std::vector<std::function<void(Out&)>> jobs;
	for (size_t hi = 0; hi < hits.size(); ++hi) {
		const Hit* Hp = &hits[hi];
		const Hit& H = *Hp;
		if (H.core > 0.0 && (H.Dc > 0.0 || H.T > 0.0)) jobs.push_back([&cx, Hp](Out& o) { BuildCrater(cx, *Hp, o); });
		for (size_t i = 0; i < H.seeds.size(); ++i) {
			if (!H.zone[i]) continue;
			if (H.st[i] == State::Missing || (H.st[i] == State::Loose && H.tMax[i] > 1e-6))
				jobs.push_back([&cx, Hp, i](Out& o) { BuildPlate(cx, *Hp, int(i), o); });
			if (!p.crackEnable || H.st[i] == State::Missing) continue;
			const Cell& c = H.cells[i];
			for (size_t k = 0; k < c.v.size(); ++k) {
				const int j = c.lab[k];
				if (j < 0) continue;
				if (H.st[size_t(j)] == State::Missing) continue;
				if (H.zone[size_t(j)] && size_t(j) < i) continue;                   
				jobs.push_back([&cx, Hp, i, k](Out& o) { BuildSeam(cx, *Hp, int(i), k, o); });
			}
		}
		if (!p.crackEnable) continue;
		OuterCrackJobs(cx, Hp, jobs);
	}

	std::vector<Out> outs(jobs.size());
	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t i) { jobs[size_t(i)](outs[size_t(i)]); });
	for (Out& o : outs) {
		for (auto& c : o.cutters) cutters.push_back(std::move(c));
		for (auto& c : o.pushed) pushed.push_back(std::move(c));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
	}
}

void PlaceConcreteHitThrough(const CoreInput& in, const ConcreteHitParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& gaps,
	std::vector<m::Manifold>& cracks)
{
	ConcreteHitParams p;
	Surf s;
	uint64_t seed = 0;
	std::vector<Hit> hits;
	if (!PrepareHits(in, pIn, mainSeed, mask, p, s, seed, hits)) return;
	Ctx cx{&p, &s, seed};
	std::vector<std::function<void(Out&)>> jobs;
	for (size_t hi = 0; hi < hits.size(); ++hi) {
		const Hit* Hp = &hits[hi];
		const Hit& H = *Hp;
		                                                             
		if (H.core > 0.0 && (H.Dc > 0.0 || H.T > 0.0)) jobs.push_back([&cx, Hp](Out& o) { BuildCrater(cx, *Hp, o); });
		jobs.push_back([&cx, Hp](Out& o) { BuildThrough(cx, *Hp, o); });
		                                                               
		if (p.crackEnable) OuterCrackJobs(cx, Hp, jobs);
	}
	std::vector<Out> outs(jobs.size());
	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t i) { jobs[size_t(i)](outs[size_t(i)]); });
	for (Out& o : outs) {
		for (auto& c : o.cutters) cutters.push_back(std::move(c));
		for (auto& c : o.gaps) gaps.push_back(std::move(c));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
	}
}
PROJECT_DAMAGE_NS_MACRO_END
