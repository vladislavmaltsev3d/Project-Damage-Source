// строим повреждённую штукатурку, дранку и деревянный каркас
// ProjectDamage lath damage — GPL-3.0-or-later; see LICENSE.
//
// Recipe: lath-damage-v1. A procedural appearance model of plaster broken
// off a lath-and-plaster wall (old interior walls and ceilings):
//   1. patches: the same patch system as Concrete Damage. A patch is a local
//      frame on the surface (origin, normal, and a tangent frame aligned with
//      the lath rows) plus a 2D signed field s(u,v) that is > 0 inside the
//      outline. Automatic mode builds the field from a cluster of overlapping,
//      stretched blobs (area-weighted seeds, optionally pulled toward sharp
//      edges). Paint mode builds it from the drawn strokes: a closed stroke is
//      the outline itself, an open stroke is swept with Paint Radius, and
//      strokes are split where the surface normal turns;
//   2. the cutter is a height-field slab over a grid in the patch frame (one
//      ray per grid column), floating above the surface outside the outline,
//      so it carves exactly the s > 0 region out of the source mesh and is
//      always a valid closed solid. The outline gets a large ragged noise plus
//      a fine "crumble" noise for the broken, fluffy plaster rim;
//   3. depth profile: stepped plaster coats (finish, then deeper coats each
//      broken back with their own ragged rim) down to the plaster thickness,
//      where the lath front sits. Where the last coat is gone the cut drops
//      through the lath layer into the cavity behind it, except where plaster
//      keys (noise blobs) still fill the gaps. Hole breaks through the wall;
//   4. cracks: V-groove strips walked outward from the rim, meandering,
//      tapering, sometimes forking (same as Concrete Damage);
//   5. laths: object-space anchored rows of rectangular wooden strips whose
//      front face is flush with the back of the plaster. A strip is built only
//      where the cut exposes it, then continues Embed under intact plaster so
//      the union welds it in. Strips have staggered butt joints, per-strip
//      offset/width/thickness/tilt jitter, warp and ragged edges; some are
//      missing, some are snapped with a chunk gone, their ends pushed in or
//      out and split into splinters.
//   6. cavity smoothing (Cavity Smooth): the plaster keys get wide flanks and
//      the cut floor behind the laths is blurred with a masked (normalised)
//      box filter over the cavity zone only, so the floor between the laths
//      has no one-cell cliffs (spikes); rims and holes keep their shape;
//   7. separate planks (LathDamageOut overload): every emitted plank piece -
//      strip body plus its splinters - is unioned into one closed solid;
//   8. plank optimisation (Lath Optimize): Ramer-Douglas-Peucker over the
//      strip cross-sections drops those that interpolation reproduces within
//      a tolerance, so straight runs need only a few sections;
//   9. rim smoothing (Rim Smooth): plaster and hole walls get at least a few
//      grid cells to step down and the crumble stays above the grid scale,
//      so the side walls do not alias into one-cell spikes.
//  10. hole outline (Hole Noise Scale / Hole Detail): the hole follows the
//      crumble-free patch field plus a band-limited noise whose finest octave
//      stays above three grid cells; only the one opening around the patch
//      core is kept (stray specks dropped, plaster islands inside it opened).
//      Its traced outline is extruded into a straight prism through the wall
//      and unioned into the patch cutter, so the hole wall is a clean ruled
//      surface rather than a height-field cliff.
//  11. wall slope (Wall Slope): only the outer plaster coat carries the fine
//      crumble, the deeper coats use the crumble-free field, and a chamfer
//      slope limiter caps how much deeper a grid point may be than its
//      neighbours, so the tall walls down to the cavity have no spikes.
//  12. cavity floor (Cavity Slope): band-limited key noise, a gentler slope
//      limit between cavity points and a light relax, so the floor behind
//      the laths reads as rounded plaster mounds rather than spiky lumps.
// Candidates use SplitMix64 counter channels like fabric tears and concrete
// damage, keyed by seed (main seed and Lath Seed), patch id and channel.
//
// Channel map (auto patch k, candidate c): 0 triangle (c*8), 1-2 barycentric,
// 3 size, 4 depth, 5 stretch, 6 stretch angle, 7 lobe count, 10+ lobes,
// 40-42 noise offset, 43 hole, 44 crack count. Paint stroke k: 3 depth,
// 43 hole, 40-42 noise. Cracks use (patch id, 200 + crack * 16 + sub-channel).
// Lath rows use (row index) hashes and board segments (row, segment) hashes so
// they agree between patches; missing/broken use (patch id ^ seed, row).
#include "manifold/manifold.h"
#include "lath_damage.h"
#include "surface_chart.h"
#include "damage_volume.h"
#include "through_wall_exit.h"
#include "framing_nail_anchor.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <set>
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

                                                                            
                                                                           
                                                     
double HoleNoise(V q, double feat, double detail, double minFeat)
{
	double sum = 0.0, amp = 1.0, norm = 0.0;
	q = q / feat;
	for (int o = 0; o < 3; ++o) {
		if (o > 0 && feat < minFeat) break;
		sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += amp;
		amp *= detail;
		if (amp <= 1e-6) break;
		feat /= 2.03;
		q = q * 2.03 + V(17.13, -9.31, 5.77);
	}
	return sum / norm;
}

                                                                             
                                                                              
                                                                             
                                            
std::vector<m::vec2> HoleOutline(const std::vector<double>& f, int nu, int nv,
	double u0, double v0, double cu, double cv)
{
	auto F = [&](int i, int j) { return f[size_t(i) * size_t(nv + 1) + size_t(j)]; };
	                                                                                   
	auto Eh = [&](int i, int j) { return int64_t(2) * (int64_t(i) * (nv + 1) + j); };
	auto Ev = [&](int i, int j) { return int64_t(2) * (int64_t(i) * (nv + 1) + j) + 1; };
	auto P = [&](int64_t e) {
		const int64_t node = e / 2;
		const int i = int(node / (nv + 1)), j = int(node % (nv + 1));
		const int i2 = (e & 1) ? i : i + 1, j2 = (e & 1) ? j + 1 : j;
		const double a = F(i, j), b = F(i2, j2);
		const double t = ClampD(a / (a - b), 0.0, 1.0);
		return m::vec2(u0 + (i + t * (i2 - i)) * cu, v0 + (j + t * (j2 - j)) * cv);
	};
	                                                        
	std::unordered_map<int64_t, int64_t> next;
	for (int i = 0; i < nu; ++i)
		for (int j = 0; j < nv; ++j) {
			const bool b0 = F(i, j) > 0.0, b1 = F(i + 1, j) > 0.0, b2 = F(i + 1, j + 1) > 0.0, b3 = F(i, j + 1) > 0.0;
			                                                                                  
			const int64_t e01 = Eh(i, j), e12 = Ev(i + 1, j), e23 = Eh(i, j + 1), e30 = Ev(i, j);
			const int c = (b0 ? 1 : 0) | (b1 ? 2 : 0) | (b2 ? 4 : 0) | (b3 ? 8 : 0);
			auto seg = [&](int64_t from, int64_t to) { next[from] = to; };
			const bool centre = (F(i, j) + F(i + 1, j) + F(i + 1, j + 1) + F(i, j + 1)) > 0.0;
			switch (c) {
			case 1: seg(e30, e01); break;
			case 2: seg(e01, e12); break;
			case 3: seg(e30, e12); break;
			case 4: seg(e12, e23); break;
			case 5: if (centre) { seg(e30, e23); seg(e12, e01); } else { seg(e30, e01); seg(e12, e23); } break;
			case 6: seg(e01, e23); break;
			case 7: seg(e30, e23); break;
			case 8: seg(e23, e30); break;
			case 9: seg(e23, e01); break;
			case 10: if (centre) { seg(e01, e30); seg(e23, e12); } else { seg(e01, e12); seg(e23, e30); } break;
			case 11: seg(e23, e12); break;
			case 12: seg(e12, e30); break;
			case 13: seg(e12, e01); break;
			case 14: seg(e01, e30); break;
			default: break;
			}
		}
	std::vector<m::vec2> best;
	double bestLen = 0.0;
	std::unordered_map<int64_t, char> used;
	std::vector<int64_t> starts;
	starts.reserve(next.size());
	for (const auto& kv : next) starts.push_back(kv.first);
	std::sort(starts.begin(), starts.end());                   
	for (int64_t s0 : starts) {
		if (used.count(s0)) continue;
		std::vector<m::vec2> loop;
		int64_t e = s0;
		bool closed = false;
		for (size_t guard = 0; guard <= next.size(); ++guard) {
			used[e] = 1;
			loop.push_back(P(e));
			auto it = next.find(e);
			if (it == next.end()) break;
			e = it->second;
			if (e == s0) { closed = true; break; }
			if (used.count(e)) break;
		}
		if (!closed || loop.size() < 3) continue;
		double len = 0.0;
		for (size_t k = 0; k < loop.size(); ++k) len += m::la::length(loop[(k + 1) % loop.size()] - loop[k]);
		if (len > bestLen) { bestLen = len; best.swap(loop); }
	}
	if (best.size() < 3) return {};
	                                                    
	std::vector<m::vec2> pts;
	const double eps = 0.05 * std::min(cu, cv);
	for (const m::vec2& q : best)
		if (pts.empty() || m::la::length(q - pts.back()) > eps) pts.push_back(q);
	while (pts.size() > 3 && m::la::length(pts.front() - pts.back()) <= eps) pts.pop_back();
	if (pts.size() < 3) return {};
	std::vector<m::vec2> sm;
	sm.reserve(pts.size() * 2);
	for (size_t k = 0; k < pts.size(); ++k) {
		const m::vec2 a = pts[k], b = pts[(k + 1) % pts.size()];
		sm.push_back(a * 0.75 + b * 0.25);
		sm.push_back(a * 0.25 + b * 0.75);
	}
	double area = 0.0;
	for (size_t k = 0; k < sm.size(); ++k) {
		const m::vec2 a = sm[k], b = sm[(k + 1) % sm.size()];
		area += a.x * b.y - b.x * a.y;
	}
	if (area < 0.0) std::reverse(sm.begin(), sm.end());
	return sm;
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
	                                                                       
	                                                              
	auto P3 = [&](uint32_t idx) {
		const size_t g = size_t(idx >> 1);
		return (idx & 1u) ? top[g] : bot[g];
	};
	auto quadFit = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		if (m::la::length(P3(a) - P3(c)) <= m::la::length(P3(b) - P3(d)) * 1.0001)
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
		else
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, d, b, c, d});
	};
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			quadFit(T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			quadFit(B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
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

                                                                             
                                                                           
m::Manifold Strip(const std::vector<V>& rawC, const V& faceN, const std::vector<double>& rawW,
	const std::vector<double>& rawT, double tol = 0.0)
{
	std::vector<V> c;
	std::vector<double> hw, ht;
	for (size_t i = 0; i < rawC.size(); ++i) {
		const double minSeg = 0.05 * std::min(rawW[i], rawT[i]) + 1e-6;
		if (c.empty() || m::la::length(rawC[i] - c.back()) > minSeg) { c.push_back(rawC[i]); hw.push_back(rawW[i]); ht.push_back(rawT[i]); }
	}
	if (tol > 0.0 && c.size() > 2) {
		                                                                  
		                                                                  
		                                                
		const size_t n0 = c.size();
		std::vector<double> arc(n0, 0.0);
		for (size_t i = 1; i < n0; ++i) arc[i] = arc[i - 1] + m::la::length(c[i] - c[i - 1]);
		std::vector<char> keep(n0, 0);
		keep[0] = keep[n0 - 1] = 1;
		std::vector<std::pair<size_t, size_t>> stack{{0, n0 - 1}};
		while (!stack.empty()) {
			const auto [a, b] = stack.back();
			stack.pop_back();
			if (b - a < 2) continue;
			const double span = std::max(1e-12, arc[b] - arc[a]);
			double worst = 0.0;
			size_t at = a;
			for (size_t i = a + 1; i < b; ++i) {
				const double f = (arc[i] - arc[a]) / span;
				const double e = std::max({m::la::length(c[i] - (c[a] * (1 - f) + c[b] * f)),
					std::abs(hw[i] - Lerp(hw[a], hw[b], f)), std::abs(ht[i] - Lerp(ht[a], ht[b], f))});
				if (e > worst) { worst = e; at = i; }
			}
			if (worst > tol) {
				keep[at] = 1;
				stack.push_back({a, at});
				stack.push_back({at, b});
			}
		}
		std::vector<V> c2; std::vector<double> hw2, ht2;
		for (size_t i = 0; i < n0; ++i)
			if (keep[i]) { c2.push_back(c[i]); hw2.push_back(hw[i]); ht2.push_back(ht[i]); }
		c.swap(c2); hw.swap(hw2); ht.swap(ht2);
	}
	const size_t n = c.size();
	if (n < 2) return m::Manifold();
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(n * 12);
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	V prevT = Unit(c[1] - c[0], AnyPerp(faceN));
	for (size_t i = 0; i < n; ++i) {
		V tan;
		if (i == 0) tan = Unit(c[1] - c[0], prevT);
		else if (i + 1 == n) tan = Unit(c[i] - c[i - 1], prevT);
		else tan = Unit(Unit(c[i] - c[i - 1], prevT) + Unit(c[i + 1] - c[i], prevT), prevT);
		prevT = tan;
		const V up = Unit(faceN - tan * m::la::dot(faceN, tan), AnyPerp(tan));
		const V side = m::la::cross(tan, up);
		push(c[i] + side * hw[i] + up * ht[i]);
		push(c[i] - side * hw[i] + up * ht[i]);
		push(c[i] - side * hw[i] - up * ht[i]);
		push(c[i] + side * hw[i] - up * ht[i]);
	}
	auto R = [&](size_t i, int k) { return uint32_t(i * 4 + size_t(k & 3)); };
	for (size_t i = 0; i + 1 < n; ++i)
		for (int k = 0; k < 4; ++k) {
			const uint32_t a = R(i, k), b = R(i + 1, k), cc = R(i + 1, k + 1), d = R(i, k + 1);
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, cc, a, cc, d});
		}
	mesh.triVerts.insert(mesh.triVerts.end(), {R(0, 0), R(0, 1), R(0, 2), R(0, 0), R(0, 2), R(0, 3)});
	mesh.triVerts.insert(mesh.triVerts.end(), {R(n - 1, 0), R(n - 1, 3), R(n - 1, 2), R(n - 1, 0), R(n - 1, 2), R(n - 1, 1)});
	return Orient(mesh);
}

                                                                                  

struct Surf {
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


                                                                   
                                            
void Frame(const LathDamageParams& p, const V& n, V& t, V& b)
{
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	V h = m::la::cross(up, n);
	if (m::la::length(h) < 0.3) {                                                              
		const V x = std::abs(up.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
		h = x - n * m::la::dot(x, n);
	}
	t = Unit(h - n * m::la::dot(h, n), AnyPerp(n));
	t = Rotate(t, n, p.lathAngle * m::kPi / 180.0);
	b = m::la::cross(n, t);
}

                                                                                      

struct Out {
	m::Manifold cutter;
	std::vector<m::Manifold> cracks, laths;
	size_t nailStart = size_t(-1);
	std::vector<std::array<double,3>> surfaceFrames;
	std::vector<m::Manifold> planks;                                                 
};

struct Grid {
 bool structural=false;
 double nailSize=1.;
	int nu = 0, nv = 0;
	double cu = 1.0, cv = 1.0;
	std::vector<double> h, d, s, sb, thick, arc;                               
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
	const LathDamageParams* p;
	const Surf* s;
	uint64_t seed;
	double lw, lt, gap, pitch;                                            
	double len, joint, embed;                                                   
	double cavity;
	double offV;                             
	bool planks = false;                                                          
	                       
	double waveL = 45.0;                                     
	double studSp = 40.0, studW = 5.0, studD = 9.0, studOff = 0.0;                                       
	int64_t bundleOff = 0;                                        
	                         
	V up = V(0, 0, 1);
	double upMin = 0.0, upMax = 0.0;                                     
};

void BuildRiven(const Ctx& cx, const Patch& P, const Patch& Q, const Grid& g,
	double u0, double u1, double v0, double v1, double cell, Out& out);
void BuildFraming(const Ctx& cx, const Patch& P, const Patch& Q, const Grid& g,
	double u0, double u1, double v0, double v1, double cell, Out& out);

void BuildClassic(const Ctx&, const Patch&, const Patch&, const Grid&, double,double,double,double,double,Out&);

// строим одну область повреждения и её внутренние детали
void BuildPatch(const Ctx& cx, const Patch& P, Out& out)
{
	const LathDamageParams& p = *cx.p;
	const Surf& s = *cx.s;
	const bool laths = p.lathEnable && p.style != 2 && cx.lw > 0.0 && cx.lt > 0.0;
	const double lt = laths ? cx.lt : 0.0;
	const double Dtot = P.D + lt + cx.cavity;                               
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const double ampOut = jag * 0.5 * P.Rn;
	const double featOut = 0.55 * P.Rn * std::max(0.05, p.outlineScale);
	double featC = std::max(1e-3, p.crumbleSize);
	const double ampC = ClampD(p.crumble, 0.0, 3.0) * 1.2 * featC;
	const double pad = 0.6 * ampOut + 0.6 * ampC + 0.08 * P.R + 1e-3;
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
	const double rimS = ClampD(p.rimSmooth, 0.0, 1.0);
	featC = std::max(featC, (2.0 + rimS) * cell);                                              
	Patch Q = P;                       
	Q.u0 = u0; Q.v0 = v0; Q.u1 = u1; Q.v1 = v1;
	const size_t N = size_t(g.nu + 1) * size_t(g.nv + 1);
	g.h.assign(N, 0.0); g.d.assign(N, 0.0); g.s.assign(N, -1.0); g.sb.assign(N, -1.0); g.thick.assign(N, -1.0); g.arc.assign(N, 0.0);
	g.valid.assign(N, 0); g.holeF.assign(N, 0); g.owned.assign(N, 1);
	std::vector<char> forbid(N, 0);
	std::vector<double> holeExit;
    if (P.hole && p.holeEnable) holeExit.assign(N, -1.0);
	const auto chart = p.organic ? s.chart.Select(P.o,P.n,extent*1.5,s.polyN) : std::vector<char>();

	const double Hs = 0.3 * extent + Dtot + 1e-3;
	const double drop = 0.6 * extent + 2.0 * Dtot;
	for (int i = 0; i <= g.nu; ++i)
		for (int j = 0; j <= g.nv; ++j) {
			const size_t k = g.I(i, j);
			const double u = u0 + i * g.cu, v = v0 + j * g.cv;
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
				if (s.Ray(S - P.n * (1e-5 * s.diag), -P.n, 2.0 * s.diag, th, tp)) {
					g.thick[k] = th;
					if (!holeExit.empty() && IsOpposingWallExit(*s.v, tp, th, P.n))
                        holeExit[k] = th + 1e-5 * s.diag;
				}
			}
		}
	                                                                    
	{
		if (p.organic) ConnectSurfaceChart(g,u0,v0,cell,(u1-u0)/g.nu,(v1-v0)/g.nv);
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
			g.sb[k] = (forbid[k] || (p.organic && !g.valid[k])) ? std::min(sv, -cell) : sv;
			if (ampC > 0.0) sv += ampC * Fbm(W / featC + P.off + V(-13.1, 27.9, 4.4), 3);
			g.arc[k] = arc;
			if (P.shape == Shape::Stroke) g.owned[k] = (arc >= P.coreA && arc <= P.coreB) ? 1 : 0;
			if (forbid[k] || (p.organic && !g.valid[k])) sv = std::min(sv, -cell);
			g.s[k] = sv;
			if (sv > sMax) { sMax = sv; kMax = k; }
		}
	                                                                     
	const double WallClassic = (1.2 + 2.0 * rimS) * cell + ClampD(p.wallSoft, 0.0, 1.0) * 0.3 * std::max(sMax, 0.0);
	if (sMax < 1.5 * WallClassic) return;                       
	                                                                           
	const bool v2 = p.profile == 1;
	const double Wall = v2 ? std::max(WallClassic, ClampD(p.smoothWidth, 0.05, 1.0) * sMax) : WallClassic;

	                                                                             
	const int L = v2 ? 1 : std::max(1, std::min(p.terraces, 4));
	const double skin = L == 1 ? 1.0 : ClampD(p.terraceSkin, 0.0, 1.0);
	const double inset = ClampD(p.terraceInset, 0.0, 0.5) * sMax;
	const double ampT = jag * 0.18 * sMax;
	const double cap = p.thicknessCap > 0.0 ? p.thicknessCap : 1e9;
	const double margin = std::max(0.3 * Dtot, 2.0 * cell);
	const double lift = 0.4 * margin;
	const double holeThr = (1.0 - ClampD(p.holeSize, 0.0, 1.0)) * sMax;
	const double ampH = ClampD(p.holeJagged, 0.0, 2.0) * 0.25 * sMax;
	                                                                          
	                                                                       
	                                                                      
	const double rimLip = std::max({0.02 * P.D, 1e-4 * extent, std::max(0.0,p.edgeWallDepth)*0.01*P.Rn});
	const double keyThr = Lerp(0.5, -0.5, ClampD(p.keys, 0.0, 1.0));
	const double keyF = std::max(1e-3, p.keySize);
	const double csm = ClampD(p.cavitySmooth, 0.0, 1.0);
	const double keyFlank = 0.08 + 0.3 * csm;                                  
	std::vector<char> outside(N, 0);
	std::vector<double> coreN(N, 0.0);                                               
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
				if (l > 0) {
					const V Wp = P.o + P.t * u + P.b * v;
					sl = g.sb[k] - l * inset + ampT * HoleNoise(Wp + (P.off + V(31.7 * l, 11.3 * l, -7.1 * l)) * (0.45 * P.Rn),
						0.45 * P.Rn, 0.5, 3.0 * cell);
				}
				double st = Smooth(0.0, Wall, sl);
				if (v2) { const double x = ClampD(sl / Wall, 0.0, 1.0); st = x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
				d += P.D * w * st;
				if (l == L - 1) core = st;
			}
			if (p.roughness > 0.0) {
				const double big = Fbm(W / (0.3 * P.Rn) + P.off + V(5.5, 1.5, 9.5), 3);
				const double fine = Fbm(W / (2.0 * featC) + P.off + V(-3.3, 8.1, 2.2), 3);
				const double r = ClampD(p.roughness, 0.0, 2.0) * 0.3 * P.D * (0.6 * big + 0.4 * fine)
					* Smooth(0.0, 2.0 * Wall, sv) * (1.0 - core);
				d = std::max(d + r, 0.3 * d);
			}
			if (laths) {
				                                                                        
				const V Wk = P.o + P.t * u + P.b * v;                                                    
				const double key = Smooth(keyThr - keyFlank, keyThr + keyFlank,
					HoleNoise(Wk + (P.off + V(41.0, -17.0, 3.0)) * keyF, keyF, 0.4, 4.0 * cell));
				d += core * Lerp(lt + cx.cavity, 0.35 * lt, key);
			} else {
				d += core * cx.cavity;
			}
			d = std::max(d, rimLip);
			if (g.thick[k] > 0.0) d = std::min(d, cap * g.thick[k]);
			if (d <= 1e-6 && !(P.hole && p.holeEnable)) { outside[k] = 1; d = 0.0; }
            if(P.volume)d=std::min(objectField.depth[k],d);
			g.d[k] = d;
			coreN[k] = core;
		}

	                                                                        
	                                                                         
	                                                                         
	std::vector<double> holeW(N, 0.0);                            
	std::vector<m::vec2> holePoly;
	double holeTop = 0.0, holeBot = 0.0;
	if (P.hole && p.holeEnable) {
		const double hFeat = std::max(0.05, p.holeNoiseScale) * sMax;
		const double hDet = ClampD(p.holeDetail, 0.0, 1.0);
		std::vector<double> sh(N, -1e300);
		size_t kBest = N;
		for (int i = 0; i <= g.nu; ++i)
			for (int j = 0; j <= g.nv; ++j) {
				const size_t k = g.I(i, j);
				if (g.s[k] <= 0.0) continue;
				const V Wp = P.o + P.t * (u0 + i * g.cu) + P.b * (v0 + j * g.cv);                                   
				sh[k] = g.sb[k] - holeThr + ampH * HoleNoise(Wp + P.off * hFeat + V(-21.0, 4.0, 13.0) * hFeat, hFeat, hDet, 3.0 * cell);
				if (kBest == N || sh[k] > sh[kBest]) kBest = k;
			}
		if (kBest < N && sh[kBest] > 0.0) {
			                                                       
			std::vector<char> open(N, 0), seen(N, 0);
			std::vector<size_t> stack{kBest};
			seen[kBest] = 1;
			while (!stack.empty()) {
				const size_t k = stack.back(); stack.pop_back();
				open[k] = 1;
				const int i = int(k / size_t(g.nv + 1)), j = int(k % size_t(g.nv + 1));
				const int ni[4] = {i - 1, i + 1, i, i}, nj[4] = {j, j, j - 1, j + 1};
				for (int e = 0; e < 4; ++e) {
					if (ni[e] < 0 || nj[e] < 0 || ni[e] > g.nu || nj[e] > g.nv) continue;
					const size_t q = g.I(ni[e], nj[e]);
					if (!seen[q] && sh[q] > 0.0) { seen[q] = 1; stack.push_back(q); }
				}
			}
			                                                                    
			                                                          
			std::vector<char> outer(N, 0);
			for (int i = 0; i <= g.nu; ++i)
				for (int j = 0; j <= g.nv; ++j) {
					if (i != 0 && j != 0 && i != g.nu && j != g.nv) continue;
					const size_t k = g.I(i, j);
					if (!open[k] && !outer[k]) { outer[k] = 1; stack.push_back(k); }
				}
			while (!stack.empty()) {
				const size_t k = stack.back(); stack.pop_back();
				const int i = int(k / size_t(g.nv + 1)), j = int(k % size_t(g.nv + 1));
				const int ni[4] = {i - 1, i + 1, i, i}, nj[4] = {j, j, j - 1, j + 1};
				for (int e = 0; e < 4; ++e) {
					if (ni[e] < 0 || nj[e] < 0 || ni[e] > g.nu || nj[e] > g.nv) continue;
					const size_t q = g.I(ni[e], nj[e]);
					if (!open[q] && !outer[q]) { outer[q] = 1; stack.push_back(q); }
				}
			}
			                                                                    
			std::vector<double> fh(N, -1.0);
			double zTop = -1e300, zBot = 1e300;
			for (size_t k = 0; k < N; ++k) {
				if (open[k]) fh[k] = sh[k];
				else if (!outer[k] && g.s[k] > 0.0) fh[k] = cell;
				else fh[k] = std::min(sh[k] > -1e299 ? sh[k] : -cell, -1e-6);
				// Only real, opposing wall faces may set a through-cut depth.
				// The extrapolated height field also reaches beyond the wall at edges.
				if (fh[k] > -2.0 * cell && holeExit[k] > 0.0) {
					zTop = std::max(zTop, g.h[k]);
					zBot = std::min(zBot, g.h[k] - holeExit[k]);
				}
			}
			if (zTop > zBot) {
				holePoly = HoleOutline(fh, g.nu, g.nv, u0, v0, g.cu, g.cv);
				holeTop = zTop + margin;
				holeBot = zBot - std::max(0.01, 1e-6 * s.diag);
				if (holePoly.size() >= 3)
					for (size_t k = 0; k < N; ++k)
						if (fh[k] > 0.0) { holeW[k] = 1.0; g.holeF[k] = 1; }
			}
		}
		for (size_t k = 0; k < N; ++k)
			if (!outside[k] && g.d[k] <= 1e-6) { outside[k] = 1; g.d[k] = 0.0; }
	}

	                                                                        
	                                                                     
	                                                                       
	                                                             
	if (csm > 0.0 && laths) {
		std::vector<double> wgt(N, 0.0), val(N, 0.0);
		bool any = false;
		for (size_t k = 0; k < N; ++k)
			if (!outside[k] && holeW[k] <= 0.0 && coreN[k] > 0.5) { wgt[k] = 1.0; val[k] = g.d[k]; any = true; }
		if (any) {
			const double radius = csm * std::max(keyF, 0.5 * cx.lw);
			const int rb = std::max(1, int(std::round(radius / (1.7 * cell))));
			std::vector<double> tw(N), tv(N);
			auto pass = [&](bool alongU) {
				const int n0 = alongU ? g.nu : g.nv, n1 = alongU ? g.nv : g.nu;
				for (int b = 0; b <= n1; ++b) {
					double sw = 0.0, sv2 = 0.0;
					auto K = [&](int a) { return alongU ? g.I(a, b) : g.I(b, a); };
					for (int a = 0; a <= std::min(n0, rb); ++a) { sw += wgt[K(a)]; sv2 += val[K(a)]; }
					for (int a = 0; a <= n0; ++a) {
						tw[K(a)] = sw; tv[K(a)] = sv2;
						const int add = a + rb + 1, rem = a - rb;
						if (add <= n0) { sw += wgt[K(add)]; sv2 += val[K(add)]; }
						if (rem >= 0) { sw -= wgt[K(rem)]; sv2 -= val[K(rem)]; }
					}
				}
				wgt.swap(tw); val.swap(tv);
			};
			const std::vector<double> mask0 = wgt;
			for (int it = 0; it < 3; ++it) { pass(true); pass(false); }
			for (size_t k = 0; k < N; ++k) {
				if (mask0[k] <= 0.0 || wgt[k] <= 1e-12) continue;
				const double blurred = val[k] / wgt[k];
				const double f = csm * Smooth(0.5, 1.0, coreN[k]);
				double d = Lerp(g.d[k], blurred, f);
				if (g.thick[k] > 0.0) d = std::min(d, cap * g.thick[k]);
				g.d[k] = d;
			}
		}
	}

	                                                                         
	                                                                          
	                                                                        
	                    
	                                                                     
	                                                       
	const bool wallLim = p.wallSlope > 0.0 && p.wallSlope < 89.9;
	const bool cavLim = laths && p.cavitySlope > 0.0 && p.cavitySlope < 89.9;
	auto inCav = [&](size_t k) { return !outside[k] && holeW[k] <= 0.0 && coreN[k] > 0.5; };
	if (wallLim || cavLim) {
		const double rate = wallLim ? std::tan(ClampD(p.wallSlope, 5.0, 89.9) * m::kPi / 180.0) : 1e9;
		const double rateC = cavLim ? std::min(rate, std::tan(ClampD(p.cavitySlope, 5.0, 89.9) * m::kPi / 180.0)) : rate;
		const double diag = std::sqrt(g.cu * g.cu + g.cv * g.cv);
		auto relax = [&](int i, int j, int di, int dj) {
			const int a = i + di, b = j + dj;
			if (a < 0 || b < 0 || a > g.nu || b > g.nv) return;
			const size_t k = g.I(i, j);
			if (outside[k] || holeW[k] > 0.0) return;
			const size_t q = g.I(a, b);
			const double dist = (di != 0 && dj != 0) ? diag : (di != 0 ? g.cu : g.cv);
			const double r = (inCav(k) && inCav(q)) ? rateC : rate;
			const double lim = (outside[q] ? 0.0 : g.d[q]) + r * dist;
			if (g.d[k] > lim) g.d[k] = lim;
		};
		for (int sweep = 0; sweep < 2; ++sweep) {
			for (int i = 0; i <= g.nu; ++i)
				for (int j = 0; j <= g.nv; ++j) {
					relax(i, j, -1, 0); relax(i, j, 0, -1); relax(i, j, -1, -1); relax(i, j, -1, 1);
				}
			for (int i = g.nu; i >= 0; --i)
				for (int j = g.nv; j >= 0; --j) {
					relax(i, j, 1, 0); relax(i, j, 0, 1); relax(i, j, 1, 1); relax(i, j, 1, -1);
				}
		}
	}

	                                                                   
	                                                                     
	if (cavLim) {
		for (int it = 0; it < 2; ++it) {
			std::vector<double> nd(g.d);
			for (int i = 1; i < g.nu; ++i)
				for (int j = 1; j < g.nv; ++j) {
					const size_t k = g.I(i, j);
					if (!inCav(k)) continue;
					double sum = 0.0;
					bool all = true;
					for (int di = -1; di <= 1 && all; ++di)
						for (int dj = -1; dj <= 1; ++dj) {
							const size_t q = g.I(i + di, j + dj);
							if (!inCav(q)) { all = false; break; }
							sum += g.d[q];
						}
					if (all) nd[k] = sum / 9.0;
				}
			g.d.swap(nd);
		}
	}

	       
	                                                                           
	                                                                          
	                                                                         
	                                                                         
	                                                                         
	                                                                       
	                                                                         
	                                                                           
	                                                                       
	                                                                   
	                                                                          
	if(p.edgeWallDepth>0.0)for(size_t k=0;k<N;++k)if(!outside[k]) {
        double lip=rimLip;
        if(g.thick[k]>0.0)lip=std::min(lip,cap*g.thick[k]);
        if(P.volume)lip=std::min(lip,objectField.depth[k]);
        g.d[k]=std::max(g.d[k],lip);
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
	if (holePoly.size() >= 3 && holeTop > holeBot && !out.cutter.IsEmpty()) {
		m::Manifold prism = m::Manifold::Extrude(m::Polygons{holePoly}, holeTop - holeBot);
		if (prism.Status() == m::Manifold::Error::NoError && !prism.IsEmpty()) {
			const V org = P.o + P.n * holeBot;
			prism = prism.Transform(m::mat3x4(m::mat3(P.t, P.b, P.n), org));
			m::Manifold both = out.cutter + prism;
			if (both.Status() == m::Manifold::Error::NoError && !both.IsEmpty()) out.cutter = both;
		}
	}

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

	                                                                                         
    // полость уже готова; доски можно не создавать
    if (!p.generatedGeometry) return;
	if (p.style == 2) {
		BuildFraming(cx, P, Q, g, u0, u1, v0, v1, cell, out);
		return;
	}
	if (laths && p.style == 1) {
		BuildRiven(cx, P, Q, g, u0, u1, v0, v1, cell, out);
		return;
	}
	if (laths) BuildClassic(cx,P,Q,g,u0,u1,v0,v1,cell,out);
}

                                                                                      

                                                                               
                                                                              
                                                                          
                                     
void BuildClassic(const Ctx& cx, const Patch& P, const Patch& Q, const Grid& g, double u0, double u1, double v0, double v1, double cell, Out& out)
{
 const auto& p=*cx.p;
	{
		const double jit = ClampD(p.lathJitter, 0.0, 1.0);
		const double tilt = ClampD(p.lathTilt, 0.0, 10.0) * m::kPi / 180.0;
		const double edge = ClampD(p.lathEdge, 0.0, 2.0);
		const double warp = ClampD(p.lathWarp, 0.0, 2.0);
		const double bend = ClampD(p.lathBend, 0.0, 6.0);
		const double splinter = ClampD(p.lathSplinter, 0.0, 6.0);
		const double optTol = ClampD(p.lathOptimize, 0.0, 1.0) * 0.12 * std::min(cx.lw, cx.lt);
		const double segLen = cx.len > 0.0 ? cx.len : 120.0 * cx.lw / 3.5;                               
		const double baseV = m::la::dot(P.o, P.b), baseU = m::la::dot(P.o, P.t);
		double ds = std::max(0.5 * cell, std::min(cell, 0.5 * cx.lw));
		if ((u1 - u0) / ds > 3000.0) ds = (u1 - u0) / 3000.0;
		const int ns = int(std::floor((u1 - u0) / ds));
		const int64_t k0 = int64_t(std::floor((baseV + v0 - cx.offV) / cx.pitch)) - 1;
		const int64_t k1 = int64_t(std::ceil((baseV + v1 - cx.offV) / cx.pitch)) + 1;
		for (int64_t kk = k0; kk <= k1 && ns >= 2; ++kk) {
			const uint64_t key = uint64_t(kk) ^ 0x4c415448ULL;            
			auto kr = [&](uint64_t ch) { return Random01(cx.seed, key, ch); };
			if (kr(0) >= ClampD(p.lathAmount, 0.0, 1.0)) continue;
			const double w = cx.lw * (1.0 + (kr(2) - 0.5) * 0.3 * jit);
			const double th = cx.lt * (1.0 + (kr(3) - 0.5) * 0.4 * jit);
			const double cRow = kk * cx.pitch + cx.offV + (kr(1) - 0.5) * 0.8 * cx.gap * jit - baseV;
			if (cRow < v0 - w || cRow > v1 + w) continue;
			auto pr = [&](uint64_t ch) { return Random01(cx.seed ^ P.id, key, ch); };               
			                                                                                  
			const double miss = ClampD(p.lathMissing, 0.0, 1.0);
			if (pr(6) < miss && !(Random01(cx.seed ^ P.id, uint64_t(kk - 1) ^ 0x4c415448ULL, 6) < miss)) continue;
			const double phaseJ = kr(7) * segLen;
			const V woff(kr(8) * 1000.0, kr(9) * 1000.0, 0.0);

			struct Smp { V ctr; double hw, ht; bool ok, exp, anchor, hole, joint, own; int64_t board; };
			std::vector<Smp> sm(size_t(ns + 1));
			for (int i = 0; i <= ns; ++i) {
				const double a = u0 + i * ds;
				const double uObj = baseU + a;
				const double segF = std::floor((uObj - phaseJ) / segLen);
				const double fseg = uObj - phaseJ - segF * segLen;
				const uint64_t segKey = key * 0x9e3779b1ULL + uint64_t(int64_t(segF));
				const double slope = std::tan((Random01(cx.seed, segKey, 10) - 0.5) * 2.0 * tilt);
				const double vv = cRow + slope * (fseg - 0.5 * segLen)
					+ (Random01(cx.seed, segKey, 11) - 0.5) * 0.25 * cx.gap * jit;
				Smp& q = sm[size_t(i)];
                q.board=int64_t(segF);
				double hh = 0.0, dd = 0.0, thk = -1.0;
				bool hl = false, own = true;
				q.ok = g.Sample(Q, a, vv, hh, dd, thk, hl, own);
				q.ok = q.ok && (thk < 0.0 || thk > P.D + th + 0.25 * th);
				q.own = own;
				q.hole = q.ok && hl;
				q.exp = q.ok && dd > P.D + 0.3 * th;
				q.anchor = q.ok && !hl && (g.structural || dd < P.D);
				q.joint = cx.len > 0.0 && (fseg < 0.5 * cx.joint || fseg > segLen - 0.5 * cx.joint);
				const double wz = warp * th * Fbm(V(uObj / (0.3 * segLen), 0.37 * double(kk), 0.0) + woff, 2);
				q.hw = 0.5 * w * (1.0 + edge * 0.14 * Fbm(V(uObj / (0.9 * w), 1.7 * double(kk), 3.1) + woff, 2));
				q.ht = 0.5 * th * (1.0 + edge * 0.12 * Fbm(V(uObj / (1.3 * w), 2.9 * double(kk), -4.3) + woff, 2));
				q.ctr = P.o + P.t * a + P.b * vv + P.n * (hh - P.D - 0.5 * th + wz);
			}
			                                                             
			for (int i = 0; i <= ns; ++i) {
				Smp& q = sm[size_t(i)];
				if (q.ok && !q.hole && (i == 0 || i == ns || !sm[size_t(i - 1)].ok || !sm[size_t(i + 1)].ok)) q.anchor = true;
			}
			                                                                              
			const int ext = int(std::ceil(cx.embed / ds));
			std::vector<char> keep(size_t(ns + 1), 0);
			for (int i = 0; i <= ns; ++i) {
				if (!sm[size_t(i)].exp) continue;
				for (int e = std::max(0, i - ext); e <= std::min(ns, i + ext); ++e) keep[size_t(e)] = 1;
			}
			for (int i = 0; i <= ns; ++i) {
				const Smp& q = sm[size_t(i)];
				if (!q.ok || !q.own || q.joint || (g.structural && q.hole)) keep[size_t(i)] = 0;
			}
			                                                                      
			                                                
			auto emit = [&](int a, int b, const std::vector<double>& defl, int base, bool tipA, bool tipB, uint64_t tkey) {
                if(g.structural){tipA=tipA||(a>0 && sm[size_t(a-1)].hole);tipB=tipB||(b<ns && sm[size_t(b+1)].hole);}
				bool anchored = false;
				for (int q = a; q <= b; ++q) anchored |= sm[size_t(q)].anchor;
				if (!anchored || b - a < 1) return;
				const size_t firstSolid = out.laths.size();                                    
				auto at = [&](int q) { return sm[size_t(q)].ctr + P.n * defl[size_t(q - base)]; };
				                                                               
				                                                           
				const int bodyA = tipA ? std::min(b - 1, a + int(std::round(splinter * 1.5 * w * Lerp(0.4, 0.7, Random01(tkey, 1, 0)) / ds))) : a;
				const int bodyB = tipB ? std::max(bodyA + 1, b - int(std::round(splinter * 1.5 * w * Lerp(0.4, 0.7, Random01(tkey, 2, 0)) / ds))) : b;
				{
					std::vector<V> c;
					std::vector<double> hw, ht;
					for (int q = bodyA; q <= bodyB; ++q) { c.push_back(at(q)); hw.push_back(sm[size_t(q)].hw); ht.push_back(sm[size_t(q)].ht); }
					m::Manifold st = Strip(c, P.n, hw, ht, optTol);
					if (!st.IsEmpty()) out.laths.push_back(std::move(st));
				}
				for (int end = 0; end < 2; ++end) {
					if (!(end == 0 ? tipA : tipB) || splinter <= 0.0) continue;
					const int from = end == 0 ? bodyA : bodyB;
					const int lim = end == 0 ? a : b;
					const int dir = end == 0 ? -1 : 1;
					const uint64_t ek = tkey * 7 + uint64_t(end);
					                                                                
					                                                               
					                                                              
					                                                        
					                                                              
					                                                                 
					                                   
					const Smp& se = sm[size_t(from)];
					const int prevI = std::max(a, std::min(b, from - dir));
					V T = at(from) - at(prevI);
					if (m::la::length(T) < 1e-12) T = P.t * double(dir);
					T = T / m::la::length(T);
					const V Nn = P.n - T * m::la::dot(P.n, T);
					const V N = Nn / std::max(1e-12, m::la::length(Nn));
					const V B = m::la::cross(N, T);
					const double hwE = se.hw, htE = se.ht;
					const double sLen = splinter * 1.5 * 2.0 * hwE;
					auto toothAt = [&](double bb) {
						const double x = (bb / std::max(1e-9, hwE) + 1.0) * 2.5;
						const double k0 = std::floor(x), f = x - k0;
						const double a0 = Random01(ek, 60 + uint64_t(int64_t(k0) + 8), 0);
						const double a1 = Random01(ek, 61 + uint64_t(int64_t(k0) + 8), 0);
						const double sm01 = f * f * (3.0 - 2.0 * f);
						const double tooth = a0 + (a1 - a0) * sm01;
						return std::pow(tooth, 1.5) * (0.6 + 0.4 * std::fabs(std::sin(m::kPi * f)));
					};
					                                                                         
					const int nW = g.structural?std::clamp(int(std::round(12*p.detail)),2,24):12;
					std::vector<std::pair<double, double>> ring;                           
					for (int k = 0; k <= nW; ++k) ring.push_back({ -hwE + 2.0 * hwE * k / nW, htE });
					ring.push_back({ hwE, 0.0 });
					for (int k = nW; k >= 0; --k) ring.push_back({ -hwE + 2.0 * hwE * k / nW, -htE });
					ring.push_back({ -hwE, 0.0 });
					const int K = (int)ring.size();
					const V base = at(from) - T * (0.5 * ds);                            
					const V tipBase = at(from);
					std::vector<V> verts;
					for (int k = 0; k < K; ++k) verts.push_back(base + B * ring[size_t(k)].first + N * ring[size_t(k)].second);
					for (int k = 0; k < K; ++k)
						verts.push_back(tipBase + B * ring[size_t(k)].first + N * ring[size_t(k)].second
							+ T * (sLen * toothAt(ring[size_t(k)].first)));
					const int cBase = (int)verts.size(); verts.push_back(base);
					const int cTip = (int)verts.size(); verts.push_back(tipBase + T * (0.3 * sLen));
					std::vector<uint32_t> tri;
					for (int k = 0; k < K; ++k) {
						const int k1 = (k + 1) % K;
						tri.insert(tri.end(), { uint32_t(k), uint32_t(k1), uint32_t(K + k1), uint32_t(k), uint32_t(K + k1), uint32_t(K + k) });
						tri.insert(tri.end(), { uint32_t(cBase), uint32_t(k1), uint32_t(k) });
						tri.insert(tri.end(), { uint32_t(cTip), uint32_t(K + k), uint32_t(K + k1) });
					}
					double vol = 0.0;
					for (size_t q = 0; q < tri.size(); q += 3)
						vol += m::la::dot(verts[tri[q]], m::la::cross(verts[tri[q + 1]], verts[tri[q + 2]]));
					if (vol < 0.0) for (size_t q = 0; q < tri.size(); q += 3) std::swap(tri[q + 1], tri[q + 2]);
					m::MeshGL64 mg;
					mg.numProp = 3;
					for (const V& q : verts) { mg.vertProperties.push_back(q.x); mg.vertProperties.push_back(q.y); mg.vertProperties.push_back(q.z); }
					for (uint32_t q : tri) mg.triVerts.push_back(q);
					m::Manifold endSolid(mg);
					if (endSolid.Status() == m::Manifold::Error::NoError && !endSolid.IsEmpty())
						out.laths.push_back(std::move(endSolid));
				}
				                                                             
				if(g.structural)out.surfaceFrames.resize(out.laths.size(),{0.,0.,cRow});
				if (cx.planks && out.laths.size() > firstSolid) {
					std::vector<m::Manifold> parts(out.laths.begin() + std::ptrdiff_t(firstSolid), out.laths.end());
					m::Manifold plank = parts.size() == 1 ? parts[0] : m::Manifold::BatchBoolean(parts, m::OpType::Add);
					if (!plank.IsEmpty()) out.planks.push_back(std::move(plank));
				}
			};

			int i = 0;
			while (i <= ns) {
				if (!keep[size_t(i)]) { ++i; continue; }
				int j = i;
				while (j + 1 <= ns && keep[size_t(j + 1)] && (!g.structural || cx.len<=0. || sm[size_t(j+1)].board==sm[size_t(j)].board)) ++j;
				const int a = i, b = j;
				i = j + 1;
				int ex = 0, e0 = -1, e1 = -1, h0 = -1, h1 = -1;
				for (int q = a; q <= b; ++q) {
					if (sm[size_t(q)].exp) { ++ex; if (e0 < 0) e0 = q; e1 = q; }
					if (sm[size_t(q)].hole) { if (h0 < 0) h0 = q; h1 = q; }
				}
				if (ex * ds < w || b - a < 2) continue;
				std::vector<double> defl(size_t(b - a + 1), 0.0);
				const uint64_t runKey = key * 131 + uint64_t(int64_t(std::floor((baseU + u0 + a * ds) / (0.5 * segLen))));
				const bool overHole = h0 >= 0 && h1 - h0 >= 3;
				const uint64_t bk = cx.seed ^ P.id ^ (runKey * 0x2545F4914F6CDD1DULL);
				const int s0 = overHole ? h0 : e0, s1 = overHole ? h1 : e1;
				const int span = s1 - s0;
				int ca = 0, cb = 0;
				if (overHole) {
					if (Random01(bk, 0, 0) < ClampD(p.holeLaths, 0.0, 1.0)) {
						                                                            
						for (int q = h0; q <= h1; ++q)
							defl[size_t(q - a)] = -bend * 0.3 * th * std::sin(m::kPi * double(q - h0) / (h1 - h0));
						emit(a, b, defl, a, false, false, runKey);
						continue;
					}
					                                                                 
					ca = h0 + int(std::floor(span * 0.3 * Random01(bk, 1, 0)));
					cb = h1 - int(std::floor(span * 0.3 * Random01(bk, 2, 0)));
					if (cb - ca < 2) { ca = h0; cb = h1; }
				} else {
					if (span < 4 || pr(1000 + uint64_t(runKey % 997)) >= ClampD(p.lathBroken, 0.0, 1.0)) {
						emit(a, b, defl, a, false, false, runKey);
						continue;
					}
					                                             
					const int cl = std::max(1, int(std::round(Lerp(0.06, 0.3, Random01(bk, 1, 0)) * span)));
					ca = s0 + int(std::floor((span - cl) * Random01(bk, 2, 0)));
					cb = ca + cl;
				}
				const double maxIn = overHole ? 1e9 : 0.8 * cx.cavity;
				for (int piece = 0; piece < 2; ++piece) {
					const int pa = piece == 0 ? a : cb, pb = piece == 0 ? ca : b;
					if (pb - pa < 2) continue;
					const double sgn = (overHole && Random01(bk, 3 + uint64_t(piece), 0) < 0.4) ? 1.0 : -1.0;
					double amt = bend * th * Lerp(0.4, 1.0, Random01(bk, 5 + uint64_t(piece), 0));
					if (sgn < 0.0) amt = std::min(amt, maxIn);
					else amt = std::min(amt, 0.5 * P.D + th);                                         
					const double flen = std::max(1, std::abs((piece == 0 ? ca : cb) - (piece == 0 ? s0 : s1)));
					std::vector<double> dp(size_t(pb - pa + 1), 0.0);
					for (int q = pa; q <= pb; ++q) {
						const double dIn = piece == 0 ? double(q - s0) : double(s1 - q);
						if (dIn <= 0.0) continue;
						const double f = ClampD(dIn / flen, 0.0, 1.0);
						dp[size_t(q - pa)] = sgn * amt * f * f;
					}
					emit(pa, pb, dp, pa, piece == 1, piece == 0, bk * 3 + uint64_t(piece));
				}
			}
		}
	}
}

constexpr int kFace = 6;
constexpr int kRing = 2 * kFace;
void RectRing(const V&,const V&,const V&,double,double,std::array<V,kRing>&,std::array<double,kRing>&);
struct Section {
	V c;                     
	double tw = 0;                                        
	std::array<m::vec2, kRing> ring;
};

                                                                             
                                                                             
void SectionFrames(const std::vector<Section>& sec, const V& n, std::vector<V>& tan, std::vector<V>& up, std::vector<V>& side)
{
	const size_t cnt = sec.size();
	tan.assign(cnt, V(0.0)); up.assign(cnt, V(0.0)); side.assign(cnt, V(0.0));
	V prevT = cnt > 1 ? Unit(sec[1].c - sec[0].c, AnyPerp(n)) : AnyPerp(n);
	for (size_t i = 0; i < cnt; ++i) {
		V t;
		if (cnt < 2) t = prevT;
		else if (i == 0) t = Unit(sec[1].c - sec[0].c, prevT);
		else if (i + 1 == cnt) t = Unit(sec[i].c - sec[i - 1].c, prevT);
		else t = Unit(Unit(sec[i].c - sec[i - 1].c, prevT) + Unit(sec[i + 1].c - sec[i].c, prevT), prevT);
		prevT = t;
		const V u0 = Unit(n - t * m::la::dot(n, t), AnyPerp(t));
		const V s0 = m::la::cross(t, u0);
		const double ca = std::cos(sec[i].tw), sa = std::sin(sec[i].tw);
		tan[i] = t;
		up[i] = u0 * ca + s0 * sa;
		side[i] = m::la::cross(t, up[i]);
	}
}

                                                                          
                                                                             
                                                                
m::Manifold RivenBody(const std::vector<Section>& raw, const V& n, double tol)
{
	std::vector<Section> sec;
	for (const Section& q : raw)
		if (sec.empty() || m::la::length(q.c - sec.back().c) > 1e-4) sec.push_back(q);
	if (sec.size() < 2) return m::Manifold();
	std::vector<V> tan, up, side;
	SectionFrames(sec, n, tan, up, side);
	const size_t n0 = sec.size();
	std::vector<std::array<V, kRing>> wp(n0);
	for (size_t i = 0; i < n0; ++i)
		for (int k = 0; k < kRing; ++k)
			wp[i][size_t(k)] = sec[i].c + side[i] * sec[i].ring[size_t(k)].x + up[i] * sec[i].ring[size_t(k)].y;
	std::vector<char> keep(n0, 1);
	if (tol > 0.0 && n0 > 2) {
		std::fill(keep.begin(), keep.end(), 0);
		std::vector<double> arc(n0, 0.0);
		for (size_t i = 1; i < n0; ++i) arc[i] = arc[i - 1] + m::la::length(sec[i].c - sec[i - 1].c);
		keep[0] = keep[n0 - 1] = 1;
		std::vector<std::pair<size_t, size_t>> stack{{0, n0 - 1}};
		while (!stack.empty()) {
			const auto [a, b] = stack.back();
			stack.pop_back();
			if (b - a < 2) continue;
			const double span = std::max(1e-12, arc[b] - arc[a]);
			double worst = 0.0;
			size_t at = a;
			for (size_t i = a + 1; i < b; ++i) {
				const double f = (arc[i] - arc[a]) / span;
				double e = 0.0;
				for (int k = 0; k < kRing; ++k)
					e = std::max(e, m::la::length(wp[i][size_t(k)] - (wp[a][size_t(k)] * (1 - f) + wp[b][size_t(k)] * f)));
				if (e > worst) { worst = e; at = i; }
			}
			if (worst > tol) {
				keep[at] = 1;
				stack.push_back({a, at});
				stack.push_back({at, b});
			}
		}
	}
	std::vector<size_t> idx;
	for (size_t i = 0; i < n0; ++i) if (keep[i]) idx.push_back(i);
	const size_t cnt = idx.size();
	if (cnt < 2) return m::Manifold();
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	for (size_t i : idx)
		for (int k = 0; k < kRing; ++k) push(wp[i][size_t(k)]);
	const uint32_t cA = uint32_t(cnt * kRing), cB = cA + 1;
	push(sec[idx.front()].c);
	push(sec[idx.back()].c);
	auto R = [&](size_t i, int k) { return uint32_t(i * kRing + size_t((k + kRing) % kRing)); };
	for (size_t i = 0; i + 1 < cnt; ++i)
		for (int k = 0; k < kRing; ++k) {
			const uint32_t a = R(i, k), b = R(i + 1, k), c = R(i + 1, k + 1), d = R(i, k + 1);
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
		}
	for (int k = 0; k < kRing; ++k) {
		mesh.triVerts.insert(mesh.triVerts.end(), {cA, R(0, k), R(0, k + 1)});
		mesh.triVerts.insert(mesh.triVerts.end(), {cB, R(cnt - 1, k + 1), R(cnt - 1, k)});
	}
	return Orient(mesh);
}

                                                                           
                                                                            
                                                                           
                                                                      
m::Manifold RivenEnd(const std::array<V, kRing>& ring, const std::array<double, kRing>& acr, const V& ctr,
	const V& T, double hw, double sLen, uint64_t ek, double ds, int subdivisions=3)
{
	auto toothAt = [&](double bb) {
		const double x = (bb / std::max(1e-9, hw) + 1.0) * 2.5;
		const double k0 = std::floor(x), f = x - k0;
		const double a0 = Random01(ek, 60 + uint64_t(int64_t(k0) + 8), 0);
		const double a1 = Random01(ek, 61 + uint64_t(int64_t(k0) + 8), 0);
		const double sm01 = f * f * (3.0 - 2.0 * f);
		const double tooth = a0 + (a1 - a0) * sm01;
		return std::pow(tooth, 1.5) * (0.6 + 0.4 * std::fabs(std::sin(m::kPi * f)));
	};
	const int kSub = std::clamp(subdivisions,1,6);
	std::vector<V> pts;
	std::vector<double> bb;
	for (int k = 0; k < kRing; ++k) {
		const int k1 = (k + 1) % kRing;
		for (int s = 0; s < kSub; ++s) {
			const double f = double(s) / kSub;
			pts.push_back(ring[size_t(k)] * (1 - f) + ring[size_t(k1)] * f);
			bb.push_back(acr[size_t(k)] * (1 - f) + acr[size_t(k1)] * f);
		}
	}
	const int K = int(pts.size());
	std::vector<V> verts;
	for (int k = 0; k < K; ++k) verts.push_back(pts[size_t(k)] - T * (0.5 * ds));
	for (int k = 0; k < K; ++k) verts.push_back(pts[size_t(k)] + T * (sLen * toothAt(bb[size_t(k)])));
	const int cBase = int(verts.size()); verts.push_back(ctr - T * (0.5 * ds));
	const int cTip = int(verts.size()); verts.push_back(ctr + T * (0.3 * sLen));
	std::vector<uint32_t> tri;
	for (int k = 0; k < K; ++k) {
		const int k1 = (k + 1) % K;
		tri.insert(tri.end(), {uint32_t(k), uint32_t(k1), uint32_t(K + k1), uint32_t(k), uint32_t(K + k1), uint32_t(K + k)});
		tri.insert(tri.end(), {uint32_t(cBase), uint32_t(k1), uint32_t(k)});
		tri.insert(tri.end(), {uint32_t(cTip), uint32_t(K + k), uint32_t(K + k1)});
	}
	double vol = 0.0;
	for (size_t q = 0; q < tri.size(); q += 3)
		vol += m::la::dot(verts[tri[q]], m::la::cross(verts[tri[q + 1]], verts[tri[q + 2]]));
	if (vol < 0.0) for (size_t q = 0; q < tri.size(); q += 3) std::swap(tri[q + 1], tri[q + 2]);
	m::MeshGL64 mg;
	mg.numProp = 3;
	for (const V& q : verts) { mg.vertProperties.push_back(q.x); mg.vertProperties.push_back(q.y); mg.vertProperties.push_back(q.z); }
	for (uint32_t q : tri) mg.triVerts.push_back(q);
	m::Manifold solid(mg);
	if (solid.Status() != m::Manifold::Error::NoError) return m::Manifold();
	return solid;
}

void BuildRiven(const Ctx& cx, const Patch& P, const Patch& Q, const Grid& g,
	double u0, double u1, double v0, double v1, double cell, Out& out)
{
	const LathDamageParams& p = *cx.p;
	const double jit = ClampD(p.lathJitter, 0.0, 1.0);
	const double tilt = ClampD(p.lathTilt, 0.0, 10.0) * m::kPi / 180.0;
	const double edge = ClampD(p.lathEdge, 0.0, 2.0);
	const double warp = ClampD(p.lathWarp, 0.0, 2.0);
	const double bend = ClampD(p.lathBend, 0.0, 6.0);
	const double splinter = ClampD(p.lathSplinter, 0.0, 6.0);
	const double wave = ClampD(p.rivenWave, 0.0, 2.0);
	const double wvar = ClampD(p.rivenWidthVar, 0.0, 1.0);
	const double twist = ClampD(p.rivenTwist, 0.0, 30.0) * m::kPi / 180.0;
	const double rough = ClampD(p.rivenRough, 0.0, 1.0);
	const double optTol = ClampD(p.lathOptimize, 0.0, 1.0) * 0.12 * std::min(cx.lw, cx.lt);
	const bool studs = p.studEnable && cx.studW > 0.0 && cx.studD > 0.0;
	const double sp = cx.studSp;
	const int boardN = std::max(0, std::min(p.boardStuds, 64));
	const double boardLen = boardN > 0 ? boardN * sp : 3.0 * sp;                                          
	const int bundleRows = std::max(1, std::min(p.jointBundle, 1000));
	const double jointGap = 1.5 * cx.joint;
	const double baseV = m::la::dot(P.o, P.b), baseU = m::la::dot(P.o, P.t);
	double ds = std::max(0.5 * cell, std::min(cell, 0.5 * cx.lw));
	if ((u1 - u0) / ds > 3000.0) ds = (u1 - u0) / 3000.0;
	const int ns = int(std::floor((u1 - u0) / ds));
	const double waveAmp = 0.5 * wave * cx.lw;
	const int64_t k0 = int64_t(std::floor((baseV + v0 - waveAmp - cx.offV) / cx.pitch)) - 1;
	const int64_t k1 = int64_t(std::ceil((baseV + v1 + waveAmp - cx.offV) / cx.pitch)) + 1;
	auto studIndex = [&](double uObj) { return int64_t(std::floor((uObj - cx.studOff) / sp + 0.5)); };

	for (int64_t kk = k0; kk <= k1 && ns >= 2; ++kk) {
		const uint64_t key = uint64_t(kk) ^ 0x52495645ULL;            
		auto kr = [&](uint64_t ch) { return Random01(cx.seed, key, ch); };
		if (kr(0) >= ClampD(p.lathAmount, 0.0, 1.0)) continue;
		const double w = cx.lw * (1.0 + (kr(2) - 0.5) * 1.2 * wvar);
		const double th = cx.lt * (1.0 + (kr(3) - 0.5) * 0.5 * jit);
		const double cRow = kk * cx.pitch + cx.offV + (kr(1) - 0.5) * 0.8 * cx.gap * jit - baseV;
		if (cRow < v0 - w - waveAmp || cRow > v1 + w + waveAmp) continue;
		auto pr = [&](uint64_t ch) { return Random01(cx.seed ^ P.id, key, ch); };               
		const double miss = ClampD(p.lathMissing, 0.0, 1.0);
		if (pr(6) < miss && !(Random01(cx.seed ^ P.id, uint64_t(kk - 1) ^ 0x52495645ULL, 6) < miss)) continue;
		                                                                     
		const int64_t bundle = int64_t(std::floor(double(kk + cx.bundleOff) / bundleRows));
		const int phase = boardN > 0 ? int(Random01(cx.seed, uint64_t(bundle) ^ 0x4a4f494eULL, 0) * boardN) % boardN : 0;            
		const double jOff = (kr(12) - 0.5) * 0.3 * cx.studW;                                                  
		const V woff(kr(8) * 1000.0, kr(9) * 1000.0, 0.0);

		struct Smp { V ctr; double tw; std::array<m::vec2, kRing> ring; double hw; bool ok, exp, anchor, hole, joint, own, stud; int64_t studK, board; };
		std::vector<Smp> sm(size_t(ns + 1));
		for (int i = 0; i <= ns; ++i) {
			const double a = u0 + i * ds;
			const double uObj = baseU + a;
			const double rel = (uObj - cx.studOff - jOff) / sp - phase;
			const double bIdx = boardN > 0 ? std::floor(rel / boardN) : std::floor(rel / 3.0);
			const double fseg = (rel - bIdx * (boardN > 0 ? boardN : 3)) * sp;                                  
			const uint64_t segKey = key * 0x9e3779b1ULL + uint64_t(int64_t(bIdx));
			auto br = [&](uint64_t ch) { return Random01(cx.seed, segKey, ch); };
			const double slope = std::tan((br(10) - 0.5) * 2.0 * tilt);
			                                                                                     
			const double wv = waveAmp * (0.95 * Fbm(V(uObj / cx.waveL, 0.17 * double(kk), 5.3) + V(17.0, 3.0, 0.0), 2)
				+ 0.3 * Fbm(V(uObj / (0.6 * cx.waveL), 1.7 * double(kk), -2.1) + woff, 2));
			const double vv = cRow + slope * (fseg - 0.5 * boardLen) + (br(11) - 0.5) * 0.25 * cx.gap * jit + wv;
			Smp& q = sm[size_t(i)];
            q.board=int64_t(bIdx);
			double hh = 0.0, dd = 0.0, thk = -1.0;
			bool hl = false, own = true;
			q.ok = g.Sample(Q, a, vv, hh, dd, thk, hl, own);
			q.ok = q.ok && (thk < 0.0 || thk > P.D + th + 0.25 * th);
			q.own = own;
			q.hole = q.ok && hl;
			q.exp = q.ok && dd > P.D + 0.3 * th;
			q.anchor = q.ok && !hl && (g.structural || dd < P.D);
			q.joint = boardN > 0 && (fseg < 0.5 * jointGap || fseg > boardLen - 0.5 * jointGap);
			q.studK = studIndex(uObj);
			q.stud = studs && std::abs(uObj - (cx.studOff + double(q.studK) * sp)) < 0.5 * cx.studW;
			                                                                                   
			const double hwB = 0.5 * w * std::max(0.4, 1.0 + wvar * 0.45 * Fbm(V(uObj / (0.8 * cx.waveL), 1.31 * double(kk), 7.7) + woff, 2));
			const double sR = hwB * (1.0 + edge * 0.14 * Fbm(V(uObj / (0.9 * w), 1.7 * double(kk), 3.1) + woff, 2));
			const double sL = -hwB * (1.0 + edge * 0.14 * Fbm(V(uObj / (0.9 * w), 1.7 * double(kk), -9.4) + woff, 2));
			q.hw = 0.5 * (sR - sL);
			                                                                             
			                                                   
			const double ht = 0.5 * th * (1.0 + edge * 0.12 * Fbm(V(uObj / (1.3 * w), 2.9 * double(kk), -4.3) + woff, 2));
			const double wedge = (br(20) - 0.5) * 0.6 * rough;
			const double zL = ht * (1.0 + wedge), zR = ht * (1.0 - wedge);
			auto cham = [&](uint64_t ch, double z) {
				const double v = rough * Lerp(0.15, 0.85, br(ch)) * (0.6 + 0.8 * ProjectDamageNoise(V(uObj / (1.1 * w), double(ch), 0.37 * double(kk)) + woff));
				return std::min(0.42 * z, v * z);
			};
			const double cFR = cham(21, zR), cFL = cham(22, zL), cBL = cham(23, zL), cBR = cham(24, zR);
			const double sw = 0.28 * (sR - sL);
			const double bump = rough * 0.18 * ht;
			const double b1 = bump * Fbm(V(uObj / (1.2 * w), 2.1 * double(kk), 1.0) + woff, 2);
			const double b2 = bump * Fbm(V(uObj / (1.2 * w), 2.1 * double(kk), 6.0) + woff, 2);
			const double sM1 = Lerp(sL, sR, 0.67), sM2 = Lerp(sL, sR, 0.33);
			const double zM1 = Lerp(zL, zR, 0.67), zM2 = Lerp(zL, zR, 0.33);
			q.ring = {{
				{sR, zR - cFR}, {sR - std::min(1.3 * cFR, sw), zR}, {sM1, zM1 + b1}, {sM2, zM2 + b2},
				{sL + std::min(1.3 * cFL, sw), zL}, {sL, zL - cFL},
				{sL, -zL + cBL}, {sL + std::min(1.3 * cBL, sw), -zL}, {sM2, -zM2 + 0.4 * b1}, {sM1, -zM1 + 0.4 * b2},
				{sR - std::min(1.3 * cBR, sw), -zR}, {sR, -zR + cBR}}};
			q.tw = twist * Fbm(V(uObj / (0.7 * cx.waveL), 0.9 * double(kk), 11.0) + woff, 2);
			const double wz = warp * th * Fbm(V(uObj / (0.3 * boardLen), 0.37 * double(kk), 0.0) + woff, 2);
			q.ctr = P.o + P.t * a + P.b * vv + P.n * (hh - P.D - 0.5 * th + wz);
		}
		for (int i = 0; i <= ns; ++i) {
			Smp& q = sm[size_t(i)];
			if (q.ok && !q.hole && (i == 0 || i == ns || !sm[size_t(i - 1)].ok || !sm[size_t(i + 1)].ok)) q.anchor = true;
		}
		const int ext = int(std::ceil(cx.embed / ds));
		std::vector<char> keep(size_t(ns + 1), 0);
		for (int i = 0; i <= ns; ++i) {
			if (!sm[size_t(i)].exp) continue;
			for (int e = std::max(0, i - ext); e <= std::min(ns, i + ext); ++e) keep[size_t(e)] = 1;
		}
		for (int i = 0; i <= ns; ++i) {
			const Smp& q = sm[size_t(i)];
			if (!q.ok || !q.own || q.joint || (g.structural && q.hole)) keep[size_t(i)] = 0;
		}

		                                                                       
		                                    
		auto emit = [&](int a, int b, const std::vector<V>& pos, bool tipA, bool tipB, uint64_t tkey) {
			if (b - a < 1) return;
			const size_t firstSolid = out.laths.size();
			auto secAt = [&](int q) {
				Section s;
				s.c = pos[size_t(q - a)];
				s.tw = sm[size_t(q)].tw;
				s.ring = sm[size_t(q)].ring;
				return s;
			};
			const double hwA = sm[size_t(a)].hw, hwB = sm[size_t(b)].hw;
			const int bodyA = tipA ? std::min(b - 1, a + int(std::round(splinter * 3.0 * hwA * Lerp(0.4, 0.7, Random01(tkey, 1, 0)) / ds))) : a;
			const int bodyB = tipB ? std::max(bodyA + 1, b - int(std::round(splinter * 3.0 * hwB * Lerp(0.4, 0.7, Random01(tkey, 2, 0)) / ds))) : b;
			std::vector<Section> secs;
			for (int q = bodyA; q <= bodyB; ++q) secs.push_back(secAt(q));
			m::Manifold body = RivenBody(secs, P.n, optTol);
			if (!body.IsEmpty()) out.laths.push_back(std::move(body));
			if (splinter > 0.0 && secs.size() >= 2) {
				std::vector<V> tn, up, sd;
				SectionFrames(secs, P.n, tn, up, sd);
				for (int end = 0; end < 2; ++end) {
					if (!(end == 0 ? tipA : tipB)) continue;
					const size_t si = end == 0 ? 0 : secs.size() - 1;
					std::array<V, kRing> ring;
					std::array<double, kRing> acr;
					for (int k = 0; k < kRing; ++k) {
						const m::vec2 rk = secs[si].ring[size_t(k)];
						ring[size_t(k)] = secs[si].c + sd[si] * rk.x + up[si] * rk.y;
						acr[size_t(k)] = rk.x;
					}
					const double hwE = sm[size_t(end == 0 ? bodyA : bodyB)].hw;
					const V T = tn[si] * (end == 0 ? -1.0 : 1.0);
					m::Manifold e = RivenEnd(ring, acr, secs[si].c, T, hwE, splinter * 3.0 * hwE, tkey * 7 + uint64_t(end), ds, g.structural?std::max(1,int(std::round(3*p.detail))):3);
					if (!e.IsEmpty()) out.laths.push_back(std::move(e));
				}
			}
			if(g.structural)out.surfaceFrames.resize(out.laths.size(),{0.,0.,cRow});
			if (cx.planks && out.laths.size() > firstSolid) {
				std::vector<m::Manifold> parts(out.laths.begin() + std::ptrdiff_t(firstSolid), out.laths.end());
				m::Manifold plank = parts.size() == 1 ? parts[0] : m::Manifold::BatchBoolean(parts, m::OpType::Add);
				if (!plank.IsEmpty()) out.planks.push_back(std::move(plank));
			}
		};

		int i = 0;
		while (i <= ns) {
			if (!keep[size_t(i)]) { ++i; continue; }
			int j = i;
			while (j + 1 <= ns && keep[size_t(j + 1)] && (!g.structural || boardN<=0 || sm[size_t(j+1)].board==sm[size_t(j)].board)) ++j;
			const int a = i, b = j;
			i = j + 1;
			int ex = 0, e0 = -1, e1 = -1, h0 = -1, h1 = -1;
			for (int q = a; q <= b; ++q) {
				if (sm[size_t(q)].exp) { ++ex; if (e0 < 0) e0 = q; e1 = q; }
				if (sm[size_t(q)].hole) { if (h0 < 0) h0 = q; h1 = q; }
			}
			if (ex * ds < w || b - a < 2) continue;
			const uint64_t runKey = key * 131 + uint64_t(int64_t(std::floor((baseU + u0 + a * ds) / (0.5 * boardLen))));
			const uint64_t bk = cx.seed ^ P.id ^ (runKey * 0x2545F4914F6CDD1DULL);
			const bool overHole = h0 >= 0 && h1 - h0 >= 3;
			std::vector<char> gone(size_t(b - a + 1), 0);
			std::vector<double> sag(size_t(b - a + 1), 0.0);
			auto isStud = [&](int q) { return sm[size_t(q)].stud; };
			if (overHole) {
				                                                                 
				                                                             
				std::vector<int> cuts{h0};
				for (int q = h0 + 1; q < h1; ++q)
					if (isStud(q) && !isStud(q - 1)) {
						int r = q;
						while (r + 1 < h1 && isStud(r + 1)) ++r;
						cuts.push_back((q + r) / 2);
						q = r;
					}
				cuts.push_back(h1);
				for (size_t c = 0; c + 1 < cuts.size(); ++c) {
					const int x0 = cuts[c], x1 = cuts[c + 1];
					if (x1 - x0 < 3) continue;
					const uint64_t bay = bk + uint64_t(c) * 0x51ULL;
					if (Random01(bay, 0, 0) < ClampD(p.holeLaths, 0.0, 1.0)) {
						for (int q = x0; q <= x1; ++q)
							sag[size_t(q - a)] = -bend * 0.25 * th * std::sin(m::kPi * double(q - x0) / (x1 - x0));
						continue;
					}
					const int len = x1 - x0;
					const int cl = std::max(2, int(std::round(Lerp(0.3, 1.0, Random01(bay, 1, 0)) * len)));
					const int c0 = x0 + int(std::floor((len - std::min(cl, len)) * Random01(bay, 2, 0)));
					for (int q = c0; q <= std::min(x1, c0 + cl); ++q)
						if (!isStud(q)) gone[size_t(q - a)] = 1;
				}
			} else if (e1 - e0 >= 4 && pr(1000 + uint64_t(runKey % 997)) < ClampD(p.lathBroken, 0.0, 1.0)) {
				const int span = e1 - e0;
				const int cl = std::max(1, int(std::round(Lerp(0.06, 0.3, Random01(bk, 1, 0)) * span)));
				const int c0 = e0 + int(std::floor((span - cl) * Random01(bk, 2, 0)));
				for (int q = c0; q <= c0 + cl; ++q)
					if (!isStud(q)) gone[size_t(q - a)] = 1;
			}
			                                   
			int q = a, piece = 0;
			while (q <= b) {
				if (gone[size_t(q - a)]) { ++q; continue; }
				int r = q;
				while (r + 1 <= b && !gone[size_t(r + 1 - a)]) ++r;
				const int pa = q, pb = r;
				q = r + 1;
				const uint64_t pk = bk * 3 + uint64_t(piece++);
				if (pb - pa < 2) continue;
				bool plaster = false;
				int studHits = 0, pivot = -1;
				for (int x = pa; x <= pb; ++x) {
					plaster |= sm[size_t(x)].anchor;
					if (isStud(x) && (x == pa || !isStud(x - 1))) {
						++studHits;
						int y = x;
						while (y + 1 <= pb && isStud(y + 1)) ++y;
						pivot = (x + y) / 2;
					}
				}
				if (!plaster && studHits == 0) continue;                              
				const bool freeA = pa > a || (g.structural && pa>0 && sm[size_t(pa-1)].hole), freeB = pb < b || (g.structural && pb+1<int(sm.size()) && sm[size_t(pb+1)].hole);
				std::vector<V> pos(size_t(pb - pa + 1));
				for (int x = pa; x <= pb; ++x) pos[size_t(x - pa)] = sm[size_t(x)].ctr + P.n * sag[size_t(x - a)];
				if (!plaster && studHits == 1 && overHole && (freeA || freeB)
					&& Random01(pk, 9, 0) < ClampD(p.lathHang, 0.0, 1.0)) {
					                                                                
					double sgn = freeB && !freeA ? -1.0 : (freeA && !freeB ? 1.0 : (Random01(pk, 10, 0) < 0.5 ? -1.0 : 1.0));
					const double ang = sgn * Lerp(12.0, 42.0, Random01(pk, 11, 0)) * m::kPi / 180.0;
					const V piv = pos[size_t(pivot - pa)];
					const double reach = std::max(1.0, double(std::max(pivot - pa, pb - pivot)));
					for (int x = pa; x <= pb; ++x) {
						const V d = pos[size_t(x - pa)] - piv;
						const double f = std::abs(double(x - pivot)) / reach;
						pos[size_t(x - pa)] = piv + Rotate(d, P.n, ang) + P.n * (th * (0.3 + 1.1 * f));
					}
				} else if (freeA || freeB) {
					                                                                
					const double sgn = (overHole && Random01(pk, 3, 0) < 0.4) ? 1.0 : -1.0;
					double amt = bend * th * Lerp(0.4, 1.0, Random01(pk, 5, 0));
					if (sgn < 0.0) amt = std::min(amt, overHole ? 1e9 : 0.8 * cx.cavity);
					else amt = std::min(amt, 0.5 * P.D + th);
					                                                                     
					for (int end = 0; end < 2; ++end) {
						if (!(end == 0 ? freeA : freeB)) continue;
						int sup = end == 0 ? pb : pa;
						for (int x = end == 0 ? pa : pb; end == 0 ? x <= pb : x >= pa; x += end == 0 ? 1 : -1)
							if (isStud(x) || sm[size_t(x)].anchor) { sup = x; break; }
						const double flen = std::max(1, std::abs(sup - (end == 0 ? pa : pb)));
						for (int x = pa; x <= pb; ++x) {
							const double dIn = end == 0 ? double(sup - x) : double(x - sup);
							if (dIn <= 0.0) continue;
							const double f = ClampD(dIn / flen, 0.0, 1.0);
							pos[size_t(x - pa)] = pos[size_t(x - pa)] + P.n * (sgn * amt * f * f);
						}
					}
				}
				emit(pa, pb, pos, freeA, freeB, pk);
			}
		}
	}

	                                                                                       
	if (studs) {
		const double lt = cx.lt;
		const double dv = ds;
		const int nv = int(std::floor((v1 - v0) / dv));
		const int64_t s0 = int64_t(std::floor((baseU + u0 - cx.studW - cx.studOff) / sp));
		const int64_t s1 = int64_t(std::ceil((baseU + u1 + cx.studW - cx.studOff) / sp));
		const int ext = int(std::ceil(2.0 * cx.embed / dv));
		for (int64_t sk = s0; sk <= s1 && nv >= 2; ++sk) {
			const double uc = cx.studOff + double(sk) * sp - baseU;
			if (uc < u0 || uc > u1) continue;
			struct SS { double h, thk; bool ok, exp, own, hole; };
			std::vector<SS> st(size_t(nv + 1));
			for (int j = 0; j <= nv; ++j) {
				const double vv = v0 + j * dv;
				SS& s = st[size_t(j)];
				double hh=0., dd=0., thk=-1.; bool hl=false, own=false;
				s.ok = g.Sample(Q, uc, vv, hh, dd, thk, hl, own);
				s.h = g.structural ? P.D : hh; s.thk = thk; s.own = own; s.hole=hl;
				if(g.structural && hl)s.ok=false;
				s.exp = s.ok && (hl || dd > P.D + 0.5 * lt);
				for (int side = -1; side <= 1 && s.ok && !s.exp; side += 2) {
					double h2, d2, t2; bool l2, o2;
					if (g.Sample(Q, uc + side * 0.4 * cx.studW, vv, h2, d2, t2, l2, o2) && (l2 || d2 > P.D + 0.5 * lt)) s.exp = true;
				}
			}
			std::vector<char> keep(size_t(nv + 1), 0);
			for (int j = 0; j <= nv; ++j) {
				if (!st[size_t(j)].exp) continue;
				for (int e = std::max(0, j - ext); e <= std::min(nv, j + ext); ++e) keep[size_t(e)] = 1;
			}
			for (int j = 0; j <= nv; ++j) if (!st[size_t(j)].ok || !st[size_t(j)].own) keep[size_t(j)] = 0;
			int j = 0;
			while (j <= nv) {
				if (!keep[size_t(j)]) { ++j; continue; }
				int r = j;
				while (r + 1 <= nv && keep[size_t(r + 1)]) ++r;
				const int a = j, b = r;
				j = r + 1;
				if (b - a < 2) continue;
				                                                               
				double dep = cx.studD;
				for (int x = a; x <= b; ++x)
					if (st[size_t(x)].thk > 0.0) dep = std::min(dep, st[size_t(x)].thk - P.D - 1.05 * lt - 0.1 * st[size_t(x)].thk);
				if (dep < 0.25 * cx.studD) continue;
				const uint64_t sKey = uint64_t(sk) ^ 0x53544b31ULL;
				std::vector<V> c;
				std::vector<double> hw, ht;
				const bool freeA=g.structural && a>0 && st[size_t(a-1)].hole;
				const bool freeB=g.structural && b<nv && st[size_t(b+1)].hole;
				const int cut=std::max(1,int(std::ceil(std::min(splinter*cx.studW,(b-a)*dv*.2)/dv)));
				const int bodyA=freeA && splinter>0?a+cut:a, bodyB=freeB && splinter>0?b-cut:b;
				for (int x = bodyA; x <= bodyB; ++x) {
					const double vv = v0 + x * dv;
					const double rough01 = Fbm(V(vv / (2.0 * cx.studW), double(sk) * 1.7, 2.2) + V(Random01(cx.seed, sKey, 0) * 1000.0, 0.0, 0.0), 2);
					c.push_back(P.o + P.t * uc + P.b * vv + P.n * (st[size_t(x)].h - P.D - 1.05 * lt - 0.5 * dep));
					hw.push_back(0.5 * cx.studW * (1.0 + 0.02 * rough01));
					ht.push_back(0.5 * dep);
				}
				const size_t firstSolid = out.laths.size();
				m::Manifold s = Strip(c, P.n, hw, ht, optTol);
				if (!s.IsEmpty()) out.laths.push_back(std::move(s));
				if(g.structural && !c.empty() && splinter>0)for(int end=0;end<2;++end){
					if(!(end==0?freeA:freeB))continue;
					const size_t at=end==0?0:c.size()-1;
					std::array<V,kRing> ring;std::array<double,kRing> acr;
					RectRing(c[at],P.t,P.n,hw[at],ht[at],ring,acr);
					auto tip=RivenEnd(ring,acr,c[at],P.b*(end==0?-1.:1.),hw[at],cut*dv,sKey*7+uint64_t(end),dv,std::max(1,int(std::round(3*p.detail))));
					if(!tip.IsEmpty())out.laths.push_back(std::move(tip));
				}
				if(g.structural)out.surfaceFrames.resize(out.laths.size(),{1.,uc,0.});
				if (cx.planks && out.laths.size() > firstSolid) out.planks.push_back(out.laths.back());
			}
		}
	}
}

                                                                                        

                                                                         
                                                                              
struct FrameSmp { V base; double h, thk; bool ok, exp, anchor, hole, own; };

struct StudNailSupport { size_t solid; double from, to; };

                                                                                      
void RectRing(const V& c, const V& side, const V& up, double hw, double ht,
	std::array<V, kRing>& ring, std::array<double, kRing>& acr)
{
	for (int k = 0; k < kFace; ++k) {
		const double s = hw - 2.0 * hw * k / (kFace - 1);
		ring[size_t(k)] = c + side * s + up * ht;
		acr[size_t(k)] = s;
		ring[size_t(kFace + k)] = c - side * s - up * ht;
		acr[size_t(kFace + k)] = -s;
	}
}

void BuildFraming(const Ctx& cx, const Patch& P, const Patch& Q, const Grid& g,
	double u0, double u1, double v0, double v1, double cell, Out& out)
{
	const LathDamageParams& p = *cx.p;
	const double sw = cx.studW, sdp = cx.studD, sp = cx.studSp;
	if (!(sw > 0.0) || !(sdp > 0.0)) return;
	const double na = ClampD(p.nailAmount, 0.0, 1.0);
	// Use the usual plaster depth as a fixed reference for existing damage presets.
	const double nLen = std::max(0.0, p.nailLength) * (g.structural ? 1.0 : 1.4);
	const bool hasNails = na > 0.0 && nLen > 0.0;
	std::unordered_map<uint64_t, std::vector<StudNailSupport>> nailSupports;
	const double splinter = ClampD(p.lathSplinter, 0.0, 6.0);
	const double bend = ClampD(p.lathBend, 0.0, 6.0);
	const double optTol = ClampD(p.lathOptimize, 0.0, 1.0) * 0.03 * std::min(sw, sdp);
	const double baseV = m::la::dot(P.o, P.b), baseU = m::la::dot(P.o, P.t);
	double ds = std::max(0.5 * cell, std::min(cell, 0.5 * sw));
	if (std::max(u1 - u0, v1 - v0) / ds > 3000.0) ds = std::max(u1 - u0, v1 - v0) / 3000.0;
	const double openAt = P.D + 0.15 * sdp;                                              
	const double embed = std::max(2.0 * sw, cx.embed);
	                                                                                
	const bool upright = m::la::dot(P.b, cx.up) > 0.95;
	const bool plates = p.framePlates && upright;
	const int topN = p.doublePlate ? 2 : 1;
	const double vBot = plates ? cx.upMin - baseV + sw : -1e300;                                  
	const double vTop = plates ? cx.upMax - baseV - topN * sw : 1e300;

	auto sampleAt = [&](double u, double v, double halfSide, bool alongU) {
		FrameSmp q;
		double dd = 0.0; bool hl = false, own = true;
		q.h = 0.0; q.thk = -1.0;
		q.ok = g.Sample(Q, u, v, q.h, dd, q.thk, hl, own);
		q.own = own;
		q.hole = q.ok && hl;
		q.anchor = q.ok && !hl && (g.structural || dd < P.D);
		q.exp = q.ok && (hl || dd > openAt);
		for (int s = -1; s <= 1 && q.ok && !q.exp; s += 2) {
			double h2, d2, t2; bool l2, o2;
			const double us = alongU ? u : u + s * 0.4 * halfSide, vs = alongU ? v + s * 0.4 * halfSide : v;
			if (g.Sample(Q, us, vs, h2, d2, t2, l2, o2) && (l2 || d2 > openAt)) q.exp = true;
		}
		q.base = P.o + P.t * u + P.b * v;
		return q;
	};

	                                                                          
	                                                                           
	                                                                          
	                                                                            
	auto member = [&](bool alongU, double fixed, double a0, double a1, double half, bool endA, bool endB, uint64_t key) {
		if (a1 - a0 < 2.0 * ds) return;
		const int n = std::max(2, int(std::ceil((a1 - a0) / ds)));
		const double step = (a1 - a0) / n;
		std::vector<FrameSmp> sm(size_t(n + 1));
		for (int i = 0; i <= n; ++i) {
			const double a = a0 + i * step;
			sm[size_t(i)] = alongU ? sampleAt(a, fixed, half, true) : sampleAt(fixed, a, half, false);
		}
		const int ext = int(std::ceil(embed / step));
		std::vector<char> keep(size_t(n + 1), 0);
		for (int i = 0; i <= n; ++i) {
			if (!sm[size_t(i)].exp) continue;
			for (int e = std::max(0, i - ext); e <= std::min(n, i + ext); ++e) keep[size_t(e)] = 1;
		}
		for (int i = 0; i <= n; ++i)
			if (!sm[size_t(i)].ok || !sm[size_t(i)].own || (g.structural && sm[size_t(i)].hole)) keep[size_t(i)] = 0;
		const V dirW = alongU ? P.t : P.b;
		int i = 0;
		while (i <= n) {
			if (!keep[size_t(i)]) { ++i; continue; }
			int j = i;
			while (j + 1 <= n && keep[size_t(j + 1)]) ++j;
			const int a = i, b = j;
			i = j + 1;
			if (b - a < 2) continue;
			                                                               
			double dep = sdp;
			for (int x = a; x <= b; ++x)
				if (sm[size_t(x)].thk > 0.0) dep = std::min(dep, sm[size_t(x)].thk - P.D - 0.1 * sm[size_t(x)].thk);
			if (dep < 0.3 * sdp) continue;
			int h0 = -1, h1 = -1;
			for (int x = a; x <= b; ++x) if (sm[size_t(x)].hole) { if (h0 < 0) h0 = x; h1 = x; }
			const uint64_t rk = key * 0x9e3779b1ULL + uint64_t(a);
			std::vector<char> gone(size_t(b - a + 1), 0);
			if (h0 >= 0 && h1 - h0 >= 4 && Random01(cx.seed ^ P.id, rk, 0) >= ClampD(p.holeLaths, 0.0, 1.0)) {
				const int span = h1 - h0;
				const int cl = std::max(2, int(std::round(Lerp(0.15, 0.6, Random01(cx.seed ^ P.id, rk, 1)) * span)));
				const int c0 = h0 + int(std::floor((span - cl) * Random01(cx.seed ^ P.id, rk, 2)));
				for (int x = c0; x <= c0 + cl; ++x) gone[size_t(x - a)] = 1;
			}
			int q = a, piece = 0;
			while (q <= b) {
				if (gone[size_t(q - a)]) { ++q; continue; }
				int r = q;
				while (r + 1 <= b && !gone[size_t(r + 1 - a)]) ++r;
				const int pa = q, pb = r;
				q = r + 1;
				const uint64_t pk = rk * 3 + uint64_t(piece++);
				if (pb - pa < 2) continue;
				bool held = (pa == 0 && endA) || (pb == n && endB);
				for (int x = pa; x <= pb && !held; ++x) held = sm[size_t(x)].anchor;
				if (!held) continue;                               
				const bool freeA = pa > a || (g.structural && pa>0 && sm[size_t(pa-1)].hole), freeB = pb < b || (g.structural && pb+1<int(sm.size()) && sm[size_t(pb+1)].hole);
				const double sgn = Random01(pk, 3, 0) < 0.5 ? -1.0 : 1.0;
				const double amt = bend * 0.25 * sw * Lerp(0.3, 1.0, Random01(pk, 4, 0));
				std::vector<V> c;
				std::vector<double> hw, ht;
				const int bodyA = freeA && splinter > 0.0 ? std::min(pb - 1, pa + std::max(1, int(std::round(0.5 * splinter * half / step)))) : pa;
				const int bodyB = freeB && splinter > 0.0 ? std::max(bodyA + 1, pb - std::max(1, int(std::round(0.5 * splinter * half / step)))) : pb;
				auto ctrAt = [&](int x) {
					double off = 0.0;                                          
					if (freeA) off += sgn * amt * std::pow(ClampD(double(std::min(pb, pa + (pb - pa) / 2) - x) / std::max(1, (pb - pa) / 2), 0.0, 1.0), 2.0);
					if (freeB) off += sgn * amt * std::pow(ClampD(double(x - std::max(pa, pb - (pb - pa) / 2)) / std::max(1, (pb - pa) / 2), 0.0, 1.0), 2.0);
					return sm[size_t(x)].base + P.n * (sm[size_t(x)].h - P.D - 0.5 * dep + off);
				};
				for (int x = bodyA; x <= bodyB; ++x) { c.push_back(ctrAt(x)); hw.push_back(half); ht.push_back(0.5 * dep); }
				const size_t firstSolid = out.laths.size();
				m::Manifold body = Strip(c, P.n, hw, ht, optTol);
				if (!body.IsEmpty()) {
					if (hasNails && !alongU)
						nailSupports[key].push_back({firstSolid, a0 + bodyA * step, a0 + bodyB * step});
					out.laths.push_back(std::move(body));
				}
				for (int end = 0; end < 2 && splinter > 0.0; ++end) {
					if (!(end == 0 ? freeA : freeB)) continue;
					const int at = end == 0 ? bodyA : bodyB;
					const V T = dirW * (end == 0 ? -1.0 : 1.0);
					const V side = m::la::cross(dirW, P.n);                                          
					std::array<V, kRing> ring;
					std::array<double, kRing> acr;
					RectRing(ctrAt(at), side, P.n, half, 0.5 * dep, ring, acr);
					m::Manifold e = RivenEnd(ring, acr, ctrAt(at), T, half, splinter * 2.0 * half, pk * 7 + uint64_t(end), step, g.structural?std::max(1,int(std::round(3*p.detail))):3);
					if (!e.IsEmpty()) out.laths.push_back(std::move(e));
				}
				if(g.structural)out.surfaceFrames.resize(out.laths.size(),{alongU?0.:1.,alongU?0.:fixed,alongU?fixed:0.});
				if (cx.planks && out.laths.size() > firstSolid) {
					std::vector<m::Manifold> parts(out.laths.begin() + std::ptrdiff_t(firstSolid), out.laths.end());
					m::Manifold plank = parts.size() == 1 ? parts[0] : m::Manifold::BatchBoolean(parts, m::OpType::Add);
					if (!plank.IsEmpty()) out.planks.push_back(std::move(plank));
				}
			}
		}
	};

	const double pad = embed + sw;
	const int64_t s0 = int64_t(std::floor((baseU + u0 - pad - cx.studOff) / sp));
	const int64_t s1 = int64_t(std::ceil((baseU + u1 + pad - cx.studOff) / sp));
	auto studU = [&](int64_t k) { return cx.studOff + double(k) * sp - baseU; };
	const double vs0 = std::max(v0, vBot), vs1 = std::min(v1, vTop);
	        
	for (int64_t k = s0; k <= s1; ++k) {
		const double uc = studU(k);
		if (uc < u0 || uc > u1) continue;
		member(false, uc, vs0, vs1, 0.5 * sw, vs0 == vBot, vs1 == vTop, uint64_t(k) ^ 0x53545544ULL);
	}
	                                          
	if (plates) {
		const double bc = cx.upMin - baseV + 0.5 * sw;
		if (bc > v0 - sw && bc < v1 + sw) member(true, bc, u0, u1, 0.5 * sw, true, true, 0x504c4231ULL);
		for (int t = 0; t < topN; ++t) {
			const double tc = cx.upMax - baseV - (0.5 + t) * sw;
			if (tc > v0 - sw && tc < v1 + sw) member(true, tc, u0, u1, 0.5 * sw, true, true, 0x504c5431ULL + uint64_t(t));
		}
	}
	                                                                            
	if (p.nogginSpacing > 0.0) {
		const double ns = std::max(p.nogginSpacing, 4.0 * sw);
		const double off = Random01(cx.seed, 0x4e4f47ULL, 0) * ns;
		for (int64_t k = s0; k < s1; ++k) {
			const double ua = studU(k) + 0.5 * sw, ub = studU(k + 1) - 0.5 * sw;
			if (ub < u0 || ua > u1 || ub - ua < 2.0 * sw) continue;
			const double shift = (k & 1) ? 0.5 * sw : -0.5 * sw;
			const int64_t r0 = int64_t(std::floor((baseV + v0 - off) / ns)) - 1, r1 = int64_t(std::ceil((baseV + v1 - off) / ns)) + 1;
			for (int64_t r = r0; r <= r1; ++r) {
				const double vc = off + double(r) * ns + shift - baseV;
				if (vc < v0 || vc > v1 || vc < vBot + sw || vc > vTop - sw) continue;
				member(true, vc, std::max(ua, u0), std::min(ub, u1), 0.5 * sw, ua >= u0, ub <= u1,
					(uint64_t(k) * 1315423911ULL) ^ uint64_t(r) ^ 0x4e4f4747ULL);
			}
		}
	}
	                                                                
	out.nailStart=out.laths.size();
	if (hasNails) {
		const double pitch = 20.0 * std::max(0.1, sw / 3.8);
		const double rad = g.structural ? std::min(0.07*sw,0.18225*std::max(.1,g.nailSize)) : 0.07*sw;
		for (int64_t k = s0; k <= s1; ++k) {
			const double uc = studU(k);
			if (uc < u0 || uc > u1) continue;
			const auto support = nailSupports.find(uint64_t(k) ^ 0x53545544ULL);
			if (support == nailSupports.end()) continue;
			const int64_t r0 = int64_t(std::floor((baseV + v0) / pitch)), r1 = int64_t(std::ceil((baseV + v1) / pitch));
			for (int64_t r = r0; r <= r1; ++r) {
				const double vc = double(r) * pitch - baseV;
				if (vc < vs0 || vc > vs1) continue;
				const uint64_t nk = (uint64_t(k) * 2654435761ULL) ^ uint64_t(r) ^ 0x4e41494cULL;
				if (Random01(cx.seed, nk, 0) >= na) continue;
				const double nailU=uc+(g.structural?(Random01(cx.seed,nk,4)-0.5)*0.08*sw:0.);
				const double nailV=vc+(g.structural?(Random01(cx.seed,nk,5)-0.5)*0.08*sw:0.);
				double hh, dd, thk; bool hl, own;
				if (!g.Sample(Q, nailU, nailV, hh, dd, thk, hl, own) || hl || !own || dd < P.D + 0.3 * sdp) continue;
				const auto& segments = support->second;
				const auto segment = std::find_if(segments.begin(), segments.end(), [=](const auto& member) {
					return nailV >= member.from && nailV <= member.to;
				});
				if (segment == segments.end()) continue;
				const V nominal = P.o + P.t * nailU + P.b * nailV + P.n * (hh - P.D);
				const auto seat = FindFramingNailSupport(out.laths[segment->solid], nominal, P.n, 2.0*sdp + bend*sw + 1.0);
				if (!seat) continue;
				const double embedNail = g.structural ? std::min(.35*seat->depth, 10.*rad) : std::min(.75*seat->depth, 15.*rad);
				const double lengthVariation = Random01(cx.seed, nk, 1);
				const double len = embedNail + nLen * (g.structural ? Lerp(1.0, 1.15, lengthVariation) : Lerp(.6, 1.2, lengthVariation));
				const double tilt = Lerp(0.0, g.structural?0.1:0.25, Random01(cx.seed, nk, 2));
				const double az = Random01(cx.seed, nk, 3) * 2.0 * m::kPi;
				const V axis = Unit(P.n + (P.t * std::cos(az) + P.b * std::sin(az)) * tilt, P.n);
				const V root = seat->front - axis * embedNail;
				const int nailSides=g.structural?std::clamp(int(std::round(8*p.detail)),4,16):8;
                const int headSides=g.structural?std::clamp(int(std::round(12*p.detail)),4,24):12;
				m::Manifold shank = m::Manifold::Cylinder(g.structural?std::max(.001,len-.6*rad):len, rad, rad, nailSides);
				m::Manifold head = m::Manifold::Cylinder(1.2 * rad, 2.6 * rad, 2.6 * rad, headSides).Translate(m::vec3(0.0, 0.0, len - (g.structural?1.2:0.6) * rad));
				m::Manifold nail = shank + head;
				const V ax = Unit(m::la::cross(axis, AnyPerp(axis)), P.t), ay = m::la::cross(axis, ax);
				nail = nail.Transform(m::mat3x4(m::mat3(ax, ay, axis), root));
				if (nail.Status() != m::Manifold::Error::NoError || nail.IsEmpty()) continue;
				out.laths.push_back(nail);
				if(g.structural)out.surfaceFrames.resize(out.laths.size(),{-1.,nailU,nailV});
				if (cx.planks) out.planks.push_back(nail);
			}
		}
	}
}


}               

namespace {
void PlaceLathDamageImpl(const CoreInput& in, const LathDamageParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	bool wantPlanks, LathDamageOut& result)
{
	std::vector<m::Manifold>& cutters = result.cutters;
	std::vector<m::Manifold>& cracks = result.cracks;
	std::vector<m::Manifold> laths;
	if (!pIn.enable) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	LathDamageParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.amount = std::max(0, std::min(p.amount, 500));
	p.lobes = std::max(1, std::min(p.lobes, 8));
	p.crackAmount = std::max(0, std::min(p.crackAmount, 32));
	p.stretch = ClampD(p.stretch, 1.0, 6.0);
	if (p.sizeMax < p.sizeMin) std::swap(p.sizeMin, p.sizeMax);
	if (p.plasterMax < p.plasterMin) std::swap(p.plasterMin, p.plasterMax);
	if (p.crackLengthMax < p.crackLengthMin) std::swap(p.crackLengthMin, p.crackLengthMax);
	p.sizeMin = std::max(1e-4, p.sizeMin);
	p.plasterMin = std::max(1e-4, p.plasterMin);
	p.paintRadius = std::max(1e-4, p.paintRadius);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	const bool paint = p.mode == 1;
    const bool proximity = p.mode == 2;
	if (paint && (!paintStrokes || paintStrokes->empty())) return;
	if (!paint && !proximity && p.amount <= 0) return;

	Surf s;
	BuildSurf(in, p.avoid, s, p.organic);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4c41544844414d47ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4c44;                             

	Ctx cx;
	cx.p = &p;
	cx.s = &s;
	cx.seed = seed;
	const double ls = std::max(1e-4, p.lathScale);
	cx.lw = std::max(0.0, p.lathWidth) * ls;
	cx.lt = std::max(0.0, p.lathThickness) * ls;
	cx.gap = std::max(0.0, p.lathGap) * ls;
	cx.pitch = std::max(1e-3, cx.lw + cx.gap);
	cx.len = std::max(0.0, p.lathLength) * ls;
	if (cx.len > 0.0) cx.len = std::max(cx.len, 4.0 * cx.lw);
	cx.joint = std::max(0.15 * cx.lt, 0.25 * cx.gap);
	cx.embed = std::max(0.0, p.lathEmbed) * ls;
	cx.planks = wantPlanks;
	cx.cavity = std::max(0.0, p.cavityDepth);
	cx.offV = Random01(seed, 0x4f4656ULL, 0) * cx.pitch;
	cx.waveL = std::max(1e-3, p.rivenWaveLength) * ls;
	cx.studSp = std::max(std::max(1e-3, p.studSpacing), 2.0 * std::max(0.0, p.studWidth));
	cx.studW = std::max(0.0, p.studWidth);
	cx.studD = std::max(0.0, p.studDepth);
	cx.studOff = Random01(seed, 0x53545544ULL, 0) * cx.studSp;            
	cx.bundleOff = int64_t(Random01(seed, 0x424e444cULL, 0) * 64.0);            
	if (p.style == 2) {
		cx.cavity = cx.studD;                                               
		cx.up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
		cx.upMin = 1e300; cx.upMax = -1e300;
		for (int32_t i = 0; i < v.npts; ++i) {
			const double h = m::la::dot(V(v.pts[i]), cx.up);
			cx.upMin = std::min(cx.upMin, h); cx.upMax = std::max(cx.upMax, h);
		}
	}

	std::vector<Patch> patches;
    std::vector<std::shared_ptr<DamageObjectVolume>> objectVolumes;

    if (proximity) {
        for(size_t i=0;i<p.proximity.size();++i) {
            const auto& point=p.proximity[i];
            if(!point.mesh_points.empty()) {
                auto volume=std::make_shared<DamageObjectVolume>(point);objectVolumes.push_back(volume);
                AppendDamageObjectPatches(s,volume,0.5*(p.plasterMin+p.plasterMax),patches,[&](const V& n,V& t,V& b){Frame(p,n,t,b);});
                continue;
            }
            if(!(point.radius>0.0))continue;
            Patch patch;patch.id=i+1;patch.shape=Shape::Blobs;
            patch.o=V(point.x,point.y,point.z);patch.n=Unit(V(point.nx,point.ny,point.nz),V(0,0,1));
            Frame(p,patch.n,patch.t,patch.b);patch.R=point.radius;patch.Rn=point.radius;
            patch.D=Lerp(p.plasterMin,p.plasterMax,Random01(seed,i+1,3));
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
			P.D = Lerp(p.plasterMin, p.plasterMax, rnd(4)) * sc;
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
			const double D = Lerp(p.plasterMin, p.plasterMax, rnd(3)) * sc;
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
            auto local=cx;local.p=&settings;local.cavity=std::max(cx.cavity,patch.volume->extent*2.0);
            BuildPatch(local,patch,outs[size_t(i)]);auto& result=outs[size_t(i)];result.cutter=result.cutter^patch.volume->solid;result.cracks.clear();
            if(wantPlanks)FilterDamageObjectGeometry(result.planks,*patch.volume);
            else FilterDamageObjectGeometry(result.laths,*patch.volume);
        } else BuildPatch(cx,patch,outs[size_t(i)]);
	});
    for(const auto& volume:objectVolumes)if(std::none_of(patches.begin(),patches.end(),[&](const auto& patch){return patch.volume==volume;}))cutters.push_back(volume->solid.AsOriginal());
	for (Out& o : outs) {
		if (!o.cutter.IsEmpty()) cutters.push_back(std::move(o.cutter));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
		if (wantPlanks)
			for (auto& b : o.planks) result.planks.push_back(std::move(b));
		else
			for (auto& b : o.laths) result.planks.push_back(std::move(b));                              
	}
}
}               

                                                                  
void PlaceLathDamage(const CoreInput& in, const LathDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& laths)
{
	LathDamageOut out;
	PlaceLathDamageImpl(in, p, mainSeed, mask, paintStrokes, false, out);
	for (auto& c : out.cutters) cutters.push_back(std::move(c));
	for (auto& c : out.cracks) cracks.push_back(std::move(c));
	for (auto& l : out.planks) laths.push_back(std::move(l));
}

                                                                          
void PlaceLathDamage(const CoreInput& in, const LathDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	LathDamageOut& out)
{
	PlaceLathDamageImpl(in, p, mainSeed, mask, paintStrokes, true, out);
}


void AddRivenSurfaceNails(const Ctx& cx, Out& out, double nailSize)
{
 const auto& p=*cx.p;
 out.nailStart=out.laths.size();
 if(p.nailAmount<=0. || p.nailLength<=0. || !p.studEnable)return;
 struct Member {size_t id; m::Box box; double centre;};
 std::vector<Member> fronts,backs;
 for(size_t i=0;i<out.nailStart;++i){
  const auto& f=out.surfaceFrames[i];
  if(f[0]==0.)fronts.push_back({i,out.laths[i].BoundingBox(),f[2]});
  else if(f[0]==1.)backs.push_back({i,out.laths[i].BoundingBox(),f[1]});
 }
 std::set<std::pair<double,double>> placed;
 const double rad=std::min(.07*cx.lw,.18225*std::max(.1,nailSize));
 const int sides=std::clamp(int(std::round(8*p.detail)),4,16);
 const int headSides=std::clamp(int(std::round(12*p.detail)),4,24);
 for(const auto& front:fronts)for(const auto& back:backs){
  const auto pair=std::make_pair(back.centre,front.centre);
  if(placed.count(pair))continue;
  const auto column=int64_t(std::llround((back.centre-cx.studOff)/cx.studSp));
  const auto row=int64_t(std::llround((front.centre-cx.offV)/cx.pitch));
  const uint64_t key=uint64_t(column)*2654435761ULL ^ uint64_t(row) ^ 0x4c4e4149ULL;
  if(Random01(cx.seed,key,0)>=ClampD(p.nailAmount,0.,1.))continue;
  const double u=back.centre+(Random01(cx.seed,key,4)-.5)*.04*cx.studW;
  const double v=front.centre+(Random01(cx.seed,key,5)-.5)*.04*cx.lw;
  auto contains=[&](const m::Box& box){return u>box.min.x+rad*1.5 && u<box.max.x-rad*1.5 && v>box.min.y+rad*1.5 && v<box.max.y-rad*1.5;};
  if(!contains(front.box) || !contains(back.box))continue;
  auto heights=[&](const Member& member,double& top,double& bottom){
   const auto hits=out.laths[member.id].RayCast(V(u,v,member.box.max.z+1.),V(u,v,member.box.min.z-1.));
   if(hits.size()<2)return false;
   top=-1e30;bottom=1e30;
   for(const auto& hit:hits){top=std::max(top,hit.position.z);bottom=std::min(bottom,hit.position.z);}
   return top-bottom>rad;
  };
  double top,bottom,backTop,backBottom;
  if(!heights(front,top,bottom) || !heights(back,backTop,backBottom))continue;
  if(bottom-backTop>std::max(.1,.5*cx.lt) || bottom<backBottom)continue;
  const double root=std::max(backBottom+rad,backTop-std::min(.35*cx.studD,10.*rad));
  const double len=top-root+p.nailLength*Lerp(1.,1.15,Random01(cx.seed,key,1));
  auto nail=m::Manifold::Cylinder(std::max(.001,len-.6*rad),rad,rad,sides)+m::Manifold::Cylinder(1.2*rad,2.6*rad,2.6*rad,headSides).Translate(V(0,0,len-1.2*rad));
  nail=nail.Translate(V(u,v,root));
  if(nail.Status()!=m::Manifold::Error::NoError || nail.IsEmpty())continue;
  out.laths.push_back(std::move(nail));out.surfaceFrames.push_back({-1.,u,v});placed.insert(pair);
 }
}

std::vector<manifold::Manifold> BuildLathSurface(const LathDamageParams& p, double u0, double u1, double v0, double v1,
 int nu, int nv, const std::vector<double>& heights, const std::vector<uint8_t>& valid, const std::vector<uint8_t>& holes, std::vector<int32_t>& materials, std::vector<double>& frames, double nailSize)
{
 if(nu<1 || nv<1 || u1<=u0 || v1<=v0 || size_t(nu+1)*size_t(nv+1)!=valid.size() || holes.size()!=valid.size() || heights.size()!=valid.size())
  throw std::invalid_argument("Invalid structural surface grid");
 Ctx cx{};cx.p=&p;cx.seed=uint64_t(p.seed);cx.planks=false;
 cx.lw=std::max(.01,p.lathWidth);cx.lt=std::max(.01,p.lathThickness);cx.gap=std::max(0.,p.lathGap);cx.pitch=cx.lw+cx.gap;
 cx.len=p.lathLength;cx.joint=std::max(.03,.08*cx.lw);cx.embed=2*cx.lw;cx.cavity=p.studDepth;
 cx.offV=Random01(cx.seed,0x4c415448ULL,0)*cx.pitch;cx.waveL=std::max(.1,p.rivenWaveLength);
 cx.studSp=std::max(p.studSpacing,2*p.studWidth);cx.studW=p.studWidth;cx.studD=p.studDepth;
 cx.studOff=Random01(cx.seed,0x53545544ULL,0)*cx.studSp;cx.up=V(0,1,0);cx.upMin=v0;cx.upMax=v1;
 Patch P{};P.id=1;P.o=V(0.0);P.n=V(0,0,1);P.t=V(1,0,0);P.b=V(0,1,0);P.D=1.;P.u0=u0;P.u1=u1;P.v0=v0;P.v1=v1;
 Grid g;g.structural=true;g.nailSize=nailSize;g.nu=nu;g.nv=nv;g.cu=(u1-u0)/nu;g.cv=(v1-v0)/nv;
 g.h=heights;for(auto& h:g.h)h+=P.D;
 g.d.assign(valid.size(),P.D+std::max(cx.lt,cx.studD)*2.);g.thick.assign(valid.size(),-1.);
 g.valid.assign(valid.begin(),valid.end());g.holeF.assign(holes.begin(),holes.end());g.owned.assign(valid.size(),1);
 Out out;double cell=std::max(g.cu,g.cv);
 if(p.style==2)BuildFraming(cx,P,P,g,u0,u1,v0,v1,cell,out);
 else if(p.style==1)BuildRiven(cx,P,P,g,u0,u1,v0,v1,cell,out);
 else BuildClassic(cx,P,P,g,u0,u1,v0,v1,cell,out);
 if(p.style==1)AddRivenSurfaceNails(cx,out,nailSize);
 materials.reserve(out.laths.size());for(size_t i=0;i<out.laths.size();++i)materials.push_back(i>=out.nailStart?1:0);
 for(const auto& f:out.surfaceFrames)frames.insert(frames.end(),f.begin(),f.end());
 return std::move(out.laths);
}

PROJECT_DAMAGE_NS_MACRO_END
