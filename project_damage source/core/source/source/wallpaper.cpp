// укладываем обои на поверхность и отгибаем надорванные полосы
// ProjectDamage wallpaper — GPL-3.0-or-later; see LICENSE.
//
// Recipe: wallpaper-v1 (inline). A procedural appearance model of old
// wallpaper coming off a wall, not a paper simulation. Like inline Coating
// Peeling, the paper is the outer Thickness of the SOURCE itself and every
// feature is cut into (or fused onto) the source mesh by the main Boolean:
//   1. paper band: the source offset outward by Thickness/2 minus the source
//      offset inward by Thickness (mitred normals). The one combined cutter is
//      intersected with it, so a torn area is a recess exactly Thickness deep
//      wherever it reaches;
//   2. strips: papered faces (normal within Wall Angle of horizontal) are
//      divided into vertical strips Strip Width wide, along Seam Direction.
//      Seam x positions are the world position projected on each wall plane,
//      so all coplanar faces share one seam layout;
//   3. seams: a fine groove along every seam of every wall plane, gaping
//      wider in places (Seam Open);
//   4. torn strips: a strip loses its paper from the top edge of the wall (or
//      from a point on it) down to a ragged, usually diagonal tear front. One
//      side runs straight down a seam; with Width < 1 the other side is a
//      ragged tear. Below the long side a split can run on down the seam;
//   5. flaps: the paper that came off stays attached along the tear front,
//      folds down over it (Fold), hangs in front of the remaining paper and
//      rolls outward at its free end (Curl), tighter toward the free corner
//      (Cone). Each flap is one closed sheet; its root lies inside the
//      remaining paper, so the union fuses it into the wall;
//   6. lifted edges: short pieces of a strip edge that let go at a seam and
//      roll back;
//   7. Grease Pencil: closed contours (paint_regions format: SurfacePaintRegion,
//      the Paint Damage Surface input) become torn areas with a flap hinged on
//      their lower edge; sides close to a seam snap straight onto it.
// Materials: every face the module makes is tagged as paper front (flap
// printed side), paper back (flap glued side, torn paper edges, groove
// cores) or exposed wall (recess floor), through three reserved Manifold
// original IDs that survive the Booleans (see WallpaperIds in wallpaper.h).
// Every torn area is laid out on a grid of columns perpendicular to its hinge
// (the tear front). Each column folds in its own plane, so the flap sheet can
// never cut through itself, and the same columns give the recess cutter.
// Candidates use SplitMix64 counter channels keyed by seed (main seed and
// Wallpaper Seed), candidate index and channel, so changing an amount does
// not reshuffle the other elements.
#include "manifold/manifold.h"
#include "wallpaper.h"
#include "project_damage_adjacency.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

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

m::vec3 Unit(const m::vec3& v, const m::vec3& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

double Mix(double a, double b, double t) { return a + (b - a) * t; }
double Clamp(double x, double lo, double hi) { return std::max(lo, std::min(hi, x)); }
double Smooth(double e0, double e1, double x)
{
	const double t = Clamp((x - e0) / (e1 - e0), 0.0, 1.0);
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

                                                             
double TornEdge(double x, const m::vec3& nz)
{
	return 0.65 * Fbm(m::vec3(x, 0.37, 0.0) + nz, 3)
		+ 0.35 * (2.0 * std::abs(Fbm(m::vec3(x * 4.3, 1.91, 0.0) + nz * 1.7, 2)) - 0.5);
}

                                                                              
                                                                            
                                                                             
struct Frame {
	m::vec3 o, x, y, n;
	bool curved = false;
	m::vec3 World(const V2& p, double h) const { return o + x * p.x + y * p.y + n * h; }
	m::vec3 Dir(const V2& d) const { return x * d.x + y * d.y; }
	V2 Local(const m::vec3& q) const { return V2(m::la::dot(q, x), m::la::dot(q, y)); }
};

Frame MakeFrame(const m::vec3& normal, const m::vec3& up, const m::vec3& onPlane)
{
	Frame f;
	f.n = Unit(normal, m::vec3(0, 0, 1));
	m::vec3 y = up - f.n * m::la::dot(up, f.n);
	if (m::la::length(y) < 1e-6) {
		const m::vec3 axis = std::abs(f.n.y) < 0.9 ? m::vec3(0, 1, 0) : m::vec3(1, 0, 0);
		y = axis - f.n * m::la::dot(axis, f.n);
	}
	f.y = Unit(y, m::vec3(0, 1, 0));
	f.x = m::la::cross(f.y, f.n);
	f.o = f.n * m::la::dot(f.n, onPlane);
	return f;
}

                                                                
struct Surface {
	const ProjectDamageBVH* bvh = nullptr;
	const std::vector<uint8_t>* side = nullptr;
	const std::vector<uint8_t>* eligible = nullptr;
	const std::vector<EVec>* normals = nullptr;
	bool Height(const Frame& f, const V2& p, double probe, double& h, Int32* hitFace = nullptr) const
	{
		double dist = 0.0;
		int32_t face = -1;
		if (!bvh->Raycast(f.World(p, probe), -f.n, 2.0 * probe, &dist, &face)) return false;
		if (face < 0 || (side && !(*side)[size_t(face)])) return false;
		if (f.curved && (!(*eligible)[size_t(face)] ||
			m::la::dot(Unit((*normals)[size_t(face)], m::vec3(0.0)), f.n) < 0.45)) return false;
		if (hitFace) *hitFace = face;
		h = probe - dist;
		return true;
	}
	                                                                           
	                                                                          
	         
	double March(const Frame& f, const V2& start, const V2& dir, double maxDist, double step,
		double probe, double tol) const
	{
		if (f.curved) tol = std::max(tol, 1.5 * step);
		double h0 = 0.0;
		if (!Height(f, start, probe, h0)) return 0.0;
		double good = 0.0, prev = h0;
		for (double d = step; d <= maxDist + 1e-9; d += step) {
			double h = 0.0;
			if (Height(f, start + dir * d, probe, h) && std::abs(h - prev) <= tol) {
				good = d;
				prev = h;
				continue;
			}
			double lo = good, hi = d;
			for (int it = 0; it < 10; ++it) {
				const double mid = 0.5 * (lo + hi);
				double hm = 0.0;
				if (Height(f, start + dir * mid, probe, hm) && std::abs(hm - prev) <= tol) lo = mid;
				else hi = mid;
			}
			return lo;
		}
		return maxDist;
	}
};

                                                                        
struct Tear {
	Frame f;
	std::vector<V2> poly;                                         
	V2 A, S, D;                                                                     
	bool entryHinge = true;                                                              
	double frontAmp = 0.0, frontLen = 1.0, frontFreq = 1.0;                                   
	m::vec3 frontNz = m::vec3(0.0);
	double probe = 1.0;
	double step = 1.0;
	int nrCut = 8;
	double down = 0.0;                                                 
	double lref = 1.0;                                            
	bool flap = false;
	bool hanging = false;                                                                      
	bool floorWall = true;                                                                             
	bool foldWhole = false;                                               
	double share = 1.0, fold = 0.0, foldR = 1.0, curl = 0.0, curlFocus = 2.5, cone = 0.0, wave = 0.0, ragged = 0.0;
	m::vec3 nz = m::vec3(0.0);
	V2 lo = V2(1e300), hi = V2(-1e300);                    
	double planeD = 0.0;

	double FrontR(double s) const
	{
		if (frontAmp <= 0.0) return 0.0;
		const double taper = Smooth(0.0, 0.06 * frontLen, s) * Smooth(0.0, 0.06 * frontLen, frontLen - s);
		return frontAmp * taper * TornEdge(s * frontFreq, frontNz);
	}
	void Bounds()
	{
		lo = V2(1e300); hi = V2(-1e300);
		for (const V2& q : poly) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
	}
};

struct Column { double s = 0, rIn = 0, rOut = 0, rf = 0; bool anchor = false; };

                                                                
bool Crossings(const Tear& T, double s, double& rIn, double& rOut)
{
	rIn = 1e300; rOut = -1e300;
	int hits = 0;
	const size_t n = T.poly.size();
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		const V2 a = T.poly[j] - T.A, b = T.poly[i] - T.A;
		const double sa = m::la::dot(a, T.S), sb = m::la::dot(b, T.S);
		if ((sa > s) == (sb > s)) continue;
		const double u = (s - sa) / (sb - sa);
		const double r = m::la::dot(a + (b - a) * u, T.D);
		rIn = std::min(rIn, r);
		rOut = std::max(rOut, r);
		++hits;
	}
	return hits >= 2 && rOut > rIn;
}

                                                                             
                                                             
std::vector<Column> Columns(const Tear& T, double minLen)
{
	double sMin = 1e300, sMax = -1e300;
	for (const V2& q : T.poly) {
		const double s = m::la::dot(q - T.A, T.S);
		sMin = std::min(sMin, s); sMax = std::max(sMax, s);
	}
	std::vector<Column> best, run;
	if (!(sMax > sMin)) return best;
	const int nc = int(Clamp(std::ceil((sMax - sMin) / T.step), 4.0, 600.0));
	for (int i = 0; i <= nc; ++i) {
		Column c;
		c.s = sMin + (sMax - sMin) * Mix(0.002, 0.998, double(i) / nc);
		if (Crossings(T, c.s, c.rIn, c.rOut) && c.rOut - c.rIn > minLen) {
			run.push_back(c);
			if (run.size() > best.size()) best = run;
		} else {
			run.clear();
		}
	}
	return best;
}

                                                                            
                                                                             
                                                                             
                                                                              
                                
struct Tags { uint32_t top, bot, side; };

                                                                              
                                                                            
m::Manifold GridSolid(const std::vector<m::vec3>& top, const std::vector<m::vec3>& bot, int ni, int nj,
	const Tags& tags, bool closed = false)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(size_t(ni) * nj * 6);
	for (size_t k = 0; k < top.size(); ++k) {
		for (const m::vec3* p : {&bot[k], &top[k]}) {
			mesh.vertProperties.push_back(float(p->x));
			mesh.vertProperties.push_back(float(p->y));
			mesh.vertProperties.push_back(float(p->z));
		}
	}
	auto B = [&](int i, int j) { return uint32_t(2 * (i * nj + j)); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * nj + j) + 1); };
	std::vector<uint32_t> part[3];                        
	auto quad = [&](int pi, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		part[pi].insert(part[pi].end(), {a, b, c, a, c, d});
	};
	const int I = ni - 1, J = nj - 1;
	for (int i = 0; i < (closed ? ni : I); ++i) {
		const int next = (i + 1) % ni;
		for (int j = 0; j < J; ++j) {
			quad(0, T(i, j), T(next, j), T(next, j + 1), T(i, j + 1));
			quad(1, B(i, j), B(i, j + 1), B(next, j + 1), B(next, j));
		}
		quad(2, B(i, 0), B(next, 0), T(next, 0), T(i, 0));
		quad(2, B(i, J), T(i, J), T(next, J), B(next, J));
	}
	for (int j = 0; !closed && j < J; ++j) {
		quad(2, B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
		quad(2, B(I, j), B(I, j + 1), T(I, j + 1), T(I, j));
	}
	                            
	const uint32_t ids[3] = {tags.top, tags.bot, tags.side};
	for (int pi = 0; pi < 3; ++pi) {
		bool seen = false;
		for (int q = 0; q < pi; ++q) seen = seen || ids[q] == ids[pi];
		if (seen) continue;
		mesh.runIndex.push_back(uint32_t(mesh.triVerts.size()));
		mesh.runOriginalID.push_back(ids[pi]);
		for (int q = pi; q < 3; ++q)
			if (ids[q] == ids[pi]) mesh.triVerts.insert(mesh.triVerts.end(), part[q].begin(), part[q].end());
	}
	mesh.runIndex.push_back(uint32_t(mesh.triVerts.size()));
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3)
			std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	return solid;
}

                                                                          
                                                                              
                                                                        
m::Manifold BuildCutter(const Tear& T, const std::vector<Column>& cols, const Surface& surf, double up,
	const Tags& tags)
{
	const int ni = int(cols.size()), nj = T.nrCut + 1;
	if (ni < 2) return m::Manifold();
	std::vector<m::vec3> top(size_t(ni) * nj), bot(size_t(ni) * nj);
	std::vector<uint8_t> supported(size_t(ni), 0);
	ProjectDamageParallelFor(0, int64_t(ni), [&](int64_t ii) {
		const Column& c = cols[size_t(ii)];
		double last = 0.0;
		bool have = false;
		std::vector<double> hs(size_t(nj), 0.0);
		std::vector<char> ok(size_t(nj), 0);
		for (int j = 0; j < nj; ++j) {
			const double r = Mix(c.rIn, c.rOut, double(j) / (nj - 1));
			const V2 q = T.A + T.S * c.s + T.D * r;
			double h = 0.0;
			if (surf.Height(T.f, q, T.probe, h)) { hs[size_t(j)] = h; ok[size_t(j)] = 1; if (!have) last = h; have = true; }
		}
		supported[size_t(ii)] = have;
		for (int j = 0; j < nj; ++j) {
			if (ok[size_t(j)]) last = hs[size_t(j)];
			const double r = Mix(c.rIn, c.rOut, double(j) / (nj - 1));
			const V2 q = T.A + T.S * c.s + T.D * r;
			const size_t k = size_t(ii) * nj + j;
			top[k] = T.f.World(q, last + up);
			bot[k] = T.f.World(q, last - T.down);
		}
	});
	if (T.f.curved) {
		int first = 0, count = 0, run = 0;
		for (int i = 0; i < ni; ++i) {
			run = supported[size_t(i)] ? run + 1 : 0;
			if (run > count) { first = i - run + 1; count = run; }
		}
		if (count < 2) return {};
		if (count != ni) {
			const auto begin = first * nj, end = (first + count) * nj;
			return GridSolid({top.begin() + begin, top.begin() + end},
				{bot.begin() + begin, bot.begin() + end}, count, nj, tags);
		}
	}
	return GridSolid(top, bot, ni, nj, tags);
}

                                                                             
                                                                              
                                                                              
                                                                             
                                                                              
m::Manifold BuildFlap(const Tear& T, const std::vector<Column>& cols, const Surface& surf,
	double t, double lift, double anchorLen, const Tags& tags)
{
	struct Prep { Column c; double start, end, height; Int32 face; };
	const double clear = T.foldWhole ? 2.0 * t : 2.0 * std::sqrt(2.5 * t * T.foldR) + 0.5 * T.step;
	std::vector<Prep> run, best;
	double lmax = 1e-9;
	for (const Column& c : cols) lmax = std::max(lmax, c.rOut - c.rf);
	for (const Column& c : cols) {
		const double body = std::max(c.rIn, c.rf);
		const double tipNoise = 0.5 + 0.5 * TornEdge(c.s / std::max(1e-6, 0.12 * T.lref), T.nz + m::vec3(3.1, 0, 0));
		const double end = body + (c.rOut - body) * T.share * (1.0 - 0.45 * T.ragged * tipNoise);
		const double start = c.anchor ? c.rf - anchorLen : c.rIn;
		                                                                     
		                                                                    
		double height = 0.0;
		Int32 face = -1;
		const V2 hinge = T.A + T.S * c.s + T.D * c.rf;
		if (c.rf < c.rOut && c.rOut - body > clear && end - std::max(start, c.rf) > 2.0 * t &&
			(!T.f.curved || surf.Height(T.f, hinge, T.probe, height, &face))) {
			run.push_back({c, start, end, height, face});
			if (run.size() > best.size()) best = run;
		} else {
			run.clear();
		}
	}
	const int ni = int(best.size());
	if (ni < 2) return m::Manifold();
	const double foldRad = std::max(1e-3, T.fold);
	const int nj = int(Clamp((T.fold + T.curl) / (4.0 * m::kPi / 180.0) + lmax / (1.5 * T.step), 24.0, 220.0));
	const double lambda = std::max(4.0 * t, 0.22 * T.lref);
	std::vector<m::vec3> top(size_t(ni) * nj), bot(size_t(ni) * nj);
	ProjectDamageParallelFor(0, int64_t(ni), [&](int64_t ii) {
		const Prep& P = best[size_t(ii)];
		const Column& c = P.c;
		const double Lc = P.end - c.rf;                                                
		                                                                  
		                                                                   
		                                                                     
		                                                                   
		                           
		double fold = foldRad, Lf, curl = 0.0;
		const double focus = std::max(1.0, T.curlFocus);
		if (T.foldWhole) {
			Lf = 0.95 * Lc;
			fold = std::min(foldRad, Lf / (2.5 * t));
		} else {
			Lf = std::min(fold * T.foldR, 0.45 * Lc);
			const double share = Clamp((c.rOut - c.rf) / lmax, 0.0, 1.0);
			                                                                  
			curl = T.curl * std::pow(share, 1.0 + 4.0 * T.cone);
			                                                     
			curl = std::min(curl, (Lc - Lf) / (focus * 2.5 * t));
		}
		const double Lh = std::max(1e-9, Lc - Lf);
		double curlNow = curl;
		auto phi = [&](double sg) {
			if (sg <= 0.0) return 0.0;
			if (sg < Lf) return fold * sg / Lf;
			const double u = (sg - Lf) / Lh;
			const double w = T.wave * 0.16 * Fbm(m::vec3(sg / lambda, c.s / lambda, 0.5) + T.nz, 2)
				* Smooth(Lf, Lf + 0.5 * lambda, sg) * (1.0 - u);
			return fold + w - curlNow * std::pow(u, focus);
		};
		                                             
		constexpr int K = 600;
		std::vector<double> tx(K + 1), ty(K + 1);
		const double ds = Lc / K;
		for (int attempt = 0; attempt < 8; ++attempt) {
			tx[0] = ty[0] = 0.0;
			double minClear = 1e300;
			for (int k = 0; k < K; ++k) {
				const double a = phi((k + 0.5) * ds);
				tx[size_t(k + 1)] = tx[size_t(k)] + std::cos(a) * ds;
				ty[size_t(k + 1)] = ty[size_t(k)] + std::sin(a) * ds;
				                                                                
				if ((k + 1) * ds > Lf + 2.0 * T.foldR)
					minClear = std::min(minClear, ty[size_t(k + 1)] - t * std::abs(std::cos(a)) - 1.02 * t);
			}
			if (minClear > 1.5 * t || curlNow <= 0.0) break;
			curlNow *= 0.75;
		}
		const V2 base2 = T.A + T.S * c.s + T.D * c.rf;
		double h = P.height;
		m::vec3 Nw = T.f.n, Dw = T.f.Dir(T.D);
		if (T.f.curved) {
			const auto hitNormal = Unit((*surf.normals)[size_t(P.face)], Nw);
			Nw = Unit(hitNormal + Nw, Nw);
			Dw = Unit(Dw - Nw * m::la::dot(Dw, Nw), Dw);
		} else surf.Height(T.f, base2, T.probe, h);
		const m::vec3 base = T.f.World(base2, h) + Nw * lift;
		for (int j = 0; j < nj; ++j) {
			const double sg = Mix(P.start - c.rf, P.end - c.rf, double(j) / (nj - 1));
			double x = sg, y = 0.0;
			if (sg > 0.0) {
				const double f = Clamp(sg / ds, 0.0, double(K));
				const int k0 = std::min(K - 1, int(f));
				const double w = f - k0;
				x = Mix(tx[size_t(k0)], tx[size_t(k0 + 1)], w);
				y = Mix(ty[size_t(k0)], ty[size_t(k0 + 1)], w);
			}
			const double a = phi(sg);
			const m::vec3 p = base + Dw * x + Nw * y;
			const m::vec3 nu = Nw * std::cos(a) - Dw * std::sin(a);
			const size_t k = size_t(ii) * nj + j;
			bot[k] = p;
			top[k] = p + nu * t;
		}
	});
	                                                                         
	                                                         
	return GridSolid(top, bot, ni, nj, tags);
}

                                                                              
                                                                            
                                                      
m::Manifold BuildOffset(const CoreInput& in, const std::vector<EVec>& normals, double d,
	const ProjectDamageBVH* bvh, double eps, uint32_t tag)
{
	const MeshView& v = in.view;
	std::vector<m::vec3> vn(size_t(v.npts), m::vec3(0.0));
	auto corner = [&](Int32 p, Int32 prev, Int32 next, const m::vec3& fn) {
		const m::vec3 a = Unit(v.pts[prev] - v.pts[p], m::vec3(0.0));
		const m::vec3 b = Unit(v.pts[next] - v.pts[p], m::vec3(0.0));
		const double ang = std::acos(std::max(-1.0, std::min(1.0, m::la::dot(a, b))));
		vn[size_t(p)] += fn * ang;
	};
	for (Int32 f = 0; f < v.npolys; ++f) {
		const ProjectDamagePoly& q = v.polys[f];
		const m::vec3 fn = Unit(normals[f], m::vec3(0.0));
		if (q.c == q.d) {
			corner(q.a, q.c, q.b, fn); corner(q.b, q.a, q.c, fn); corner(q.c, q.b, q.a, fn);
		} else {
			corner(q.a, q.d, q.b, fn); corner(q.b, q.a, q.c, fn);
			corner(q.c, q.b, q.d, fn); corner(q.d, q.c, q.a, fn);
		}
	}
	for (auto& x : vn) x = Unit(x, m::vec3(0, 0, 1));
	std::vector<double> minCos(size_t(v.npts), 1.0);
	for (Int32 f = 0; f < v.npolys; ++f) {
		const ProjectDamagePoly& q = v.polys[f];
		const m::vec3 fn = Unit(normals[f], m::vec3(0.0));
		const Int32 ids[4] = {q.a, q.b, q.c, q.d};
		for (int k = 0; k < (q.c == q.d ? 3 : 4); ++k)
			minCos[size_t(ids[k])] = std::min(minCos[size_t(ids[k])], m::la::dot(vn[size_t(ids[k])], fn));
	}
	std::vector<m::vec3> moved(size_t(v.npts));
	ProjectDamageParallelFor(0, int64_t(v.npts), [&](int64_t i) {
		double s = std::abs(d) / std::max(0.35, minCos[size_t(i)]);
		if (d < 0.0 && bvh != nullptr) {
			double dist = 0.0;
			if (bvh->Raycast(v.pts[i] - vn[size_t(i)] * eps, -vn[size_t(i)], 1e9, &dist))
				s = std::min(s, 0.45 * dist);
		}
		moved[size_t(i)] = v.pts[i] + vn[size_t(i)] * (d < 0.0 ? -s : s);
	});
	m::MeshGL mesh;
	mesh.numProp = 3;
	for (const m::vec3& p : moved) {
		mesh.vertProperties.push_back(float(p.x));
		mesh.vertProperties.push_back(float(p.y));
		mesh.vertProperties.push_back(float(p.z));
	}
	for (Int32 f = 0; f < v.npolys; ++f) {
		const ProjectDamagePoly& q = v.polys[f];
		mesh.triVerts.insert(mesh.triVerts.end(), {uint32_t(q.a), uint32_t(q.b), uint32_t(q.c)});
		if (q.c != q.d)
			mesh.triVerts.insert(mesh.triVerts.end(), {uint32_t(q.a), uint32_t(q.c), uint32_t(q.d)});
	}
	mesh.runIndex = {0, uint32_t(mesh.triVerts.size())};
	mesh.runOriginalID = {tag};
	mesh.Merge();
	return m::Manifold(mesh);
}

                                                                      
                                                      
void RaggedEdge(std::vector<V2>& out, const V2& a, const V2& b, double amp, double spacing, const m::vec3& nz)
{
	const double len = m::la::length(b - a);
	const int n = std::max(1, int(std::ceil(len / std::max(1e-6, spacing))));
	const V2 d = len > 1e-12 ? (b - a) / len : V2(1, 0);
	const V2 nl(-d.y, d.x);
	const double freq = 1.0 / std::max(1e-6, 6.0 * spacing);
	for (int i = 0; i < n; ++i) {
		const double s = len * i / n;
		const double taper = Smooth(0.0, 0.08 * len, s) * Smooth(0.0, 0.08 * len, len - s);
		out.push_back(a + d * s + nl * (amp * taper * TornEdge(s * freq, nz)));
	}
}

                                                                              
                                                 
struct WallPlane {
	Frame f;
	V2 lo = V2(1e300), hi = V2(-1e300);
	double d = 0.0;
};

struct PlaneKey {
	int64_t a, b, c, d;
	bool operator==(const PlaneKey& o) const { return a == o.a && b == o.b && c == o.c && d == o.d; }
};
struct PlaneKeyHash {
	size_t operator()(const PlaneKey& k) const
	{
		uint64_t h = 1469598103934665603ULL;
		for (int64_t v : {k.a, k.b, k.c, k.d}) { h ^= uint64_t(v); h *= 1099511628211ULL; }
		return size_t(h);
	}
};

}              

WallpaperFaceIds WallpaperIds()
{
	static const uint32_t first = m::Manifold::ReserveIDs(5);
	return WallpaperFaceIds{int32_t(first), int32_t(first + 1), int32_t(first + 2),
		int32_t(first + 3), int32_t(first + 4)};
}

namespace {

                                                                           
                                                                             
                                                                             
                                                                           
                                                                            
                                          
WallpaperParams Sanitized(const WallpaperParams& in)
{
	const WallpaperParams def;
	WallpaperParams p = in;
	auto fin = [](double& x, double d) { if (!std::isfinite(x)) x = d; };
	auto pos = [&](double& x, double d, double lo) { fin(x, d); x = std::max(lo, x); };
	auto range = [&](double& lo, double& hi, double dLo, double dHi, double floor) {
		fin(lo, dLo); fin(hi, dHi);
		if (lo > hi) std::swap(lo, hi);
		lo = std::max(floor, lo);
		hi = std::max(lo, hi);
	};
	pos(p.thickness, def.thickness, 1e-3);
	pos(p.stripWidth, def.stripWidth, 1e-2);
	fin(p.seamOffset, def.seamOffset);
	fin(p.seamDirX, def.seamDirX); fin(p.seamDirY, def.seamDirY); fin(p.seamDirZ, def.seamDirZ);
	fin(p.wallAngle, def.wallAngle);
	p.finderEnable = p.finderEnable && std::isfinite(p.finderX) &&
		std::isfinite(p.finderY) && std::isfinite(p.finderZ);
	pos(p.detail, def.detail, 0.05);
	pos(p.seamWidth, def.seamWidth, 1e-3);
	pos(p.seamDepth, def.seamDepth, 0.0);
	fin(p.seamOpen, def.seamOpen);
	range(p.tearLengthMin, p.tearLengthMax, def.tearLengthMin, def.tearLengthMax, 1e-2);
	range(p.tearWidthMin, p.tearWidthMax, def.tearWidthMin, def.tearWidthMax, 1e-2);
	p.tearWidthMax = std::min(1.0, p.tearWidthMax);
	p.tearWidthMin = std::min(p.tearWidthMin, p.tearWidthMax);
	fin(p.tearDiagonal, def.tearDiagonal); fin(p.tearFromTop, def.tearFromTop); fin(p.tearJagged, def.tearJagged);
	fin(p.tearDiagonalRandomness, def.tearDiagonalRandomness); fin(p.tearFromTopRandomness, def.tearFromTopRandomness);
	fin(p.seamCrack, def.seamCrack); pos(p.seamCrackLength, def.seamCrackLength, 0.0);
	fin(p.flapAmount, def.flapAmount); fin(p.flapFold, def.flapFold);
	pos(p.flapFoldRadius, def.flapFoldRadius, 0.0);
	fin(p.flapCurl, def.flapCurl); fin(p.flapCurlFocus, def.flapCurlFocus);
	fin(p.flapCone, def.flapCone); fin(p.flapWave, def.flapWave); fin(p.flapRagged, def.flapRagged);
	fin(p.flapWaveRandomness, def.flapWaveRandomness); fin(p.flapCurlFoldRandomness, def.flapCurlFoldRandomness);
	range(p.liftLengthMin, p.liftLengthMax, def.liftLengthMin, def.liftLengthMax, 1e-2);
	range(p.liftWidthMin, p.liftWidthMax, def.liftWidthMin, def.liftWidthMax, 1e-2);
	fin(p.liftCurl, def.liftCurl);
	pos(p.paintReach, def.paintReach, 0.0);
	fin(p.paintJagged, def.paintJagged);
	pos(p.seamSnap, def.seamSnap, 0.0);
	p.tearAmount = std::max(0, std::min(1000, p.tearAmount));
	p.liftAmount = std::max(0, std::min(10000, p.liftAmount));
	p.tearSide = std::max(0, std::min(2, p.tearSide));
	return p;
}

m::vec3 WallNormal(const MeshView& mesh, const ProjectDamagePoly& q)
{
	const m::vec3 ab = mesh.pts[q.b] - mesh.pts[q.a];
	const m::vec3 ac = mesh.pts[q.c] - mesh.pts[q.a];
	m::vec3 normal = m::la::cross(ab, ac);
	if (q.c != q.d) normal += m::la::cross(ac, mesh.pts[q.d] - mesh.pts[q.a]);
	return Unit(normal, m::vec3(0.0));
}

double TriangleDistanceSquared(const m::vec3& p, const m::vec3& a, const m::vec3& b, const m::vec3& c)
{
	const m::vec3 ab = b - a, ac = c - a, ap = p - a;
	const m::vec3 n = m::la::cross(ab, ac);
	const double nn = m::la::dot(n, n);
	const double height = m::la::dot(ap, n);
	if (nn > 1e-24 &&
		m::la::dot(m::la::cross(ab, ap), n) >= 0.0 &&
		m::la::dot(m::la::cross(c - b, p - b), n) >= 0.0 &&
		m::la::dot(m::la::cross(a - c, p - c), n) >= 0.0)
		return height * height / nn;
	auto edgeDistance = [&](const m::vec3& start, const m::vec3& edge) {
		const double lengthSquared = m::la::dot(edge, edge);
		const m::vec3 delta = p - start;
		const double fraction = lengthSquared > 0.0 ? Clamp(m::la::dot(delta, edge) / lengthSquared, 0.0, 1.0) : 0.0;
		const m::vec3 gap = delta - edge * fraction;
		return m::la::dot(gap, gap);
	};
	return std::min({edgeDistance(a, ab), edgeDistance(b, c - b), edgeDistance(c, -ac)});
}

std::vector<uint8_t> FindWallpaperSide(const CoreInput& in, const WallpaperParams& p,
	const m::vec3& up, double wallSin)
{
	const MeshView& mesh = in.view;
	std::vector<m::vec3> finders;
    for (size_t i=0;i+2<p.finderPoints.size();i+=3) {
        const double x=p.finderPoints[i],y=p.finderPoints[i+1],z=p.finderPoints[i+2];
        if(std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)) finders.emplace_back(x,y,z);
    }
    if(finders.empty()) finders.emplace_back(p.finderX,p.finderY,p.finderZ);
    std::vector<m::vec3> wallNormals(size_t(mesh.npolys), m::vec3(0.0));
    std::vector<uint8_t> side(size_t(mesh.npolys), 0);
    for(Int32 f=0;f<mesh.npolys;++f) {
        const auto n=WallNormal(mesh,mesh.polys[f]);
        if(m::la::length(n)>=0.5 && std::abs(m::la::dot(n,up))<=wallSin) wallNormals[size_t(f)]=n;
    }
    ProjectDamageAdjacency adjacency;
    if(!adjacency.Build(mesh)) return side;
    for(const auto& finder:finders) {
        Int32 nearest=-1;
        double best=std::numeric_limits<double>::infinity();
        for(Int32 f=0;f<mesh.npolys;++f) {
            if(m::la::length(wallNormals[size_t(f)])<0.5) continue;
            const auto& q=mesh.polys[f];
            double d=TriangleDistanceSquared(finder,mesh.pts[q.a],mesh.pts[q.b],mesh.pts[q.c]);
            if(q.c!=q.d) d=std::min(d,TriangleDistanceSquared(finder,mesh.pts[q.a],mesh.pts[q.c],mesh.pts[q.d]));
            if(d<best) {best=d;nearest=f;}
        }
        if(nearest<0) continue;
        const auto facing=wallNormals[size_t(nearest)];
        std::vector<uint8_t> visited(size_t(mesh.npolys),0);
        std::vector<Int32> queue{nearest};visited[size_t(nearest)]=1;side[size_t(nearest)]=1;
        for(size_t i=0;i<queue.size();++i) for(Int32 next:adjacency.GetInfo(queue[i])->face) {
            if(next<0 || visited[size_t(next)] || m::la::dot(wallNormals[size_t(next)],facing)<0.7071067811865476) continue;
            visited[size_t(next)]=1;side[size_t(next)]=1;queue.push_back(next);
        }
    }
	return side;
}

struct WallChart {
	std::vector<Int32> faces;
	m::vec3 normal{0.0}, center{0.0}, x{0.0}, y{0.0};
	double radius = 0.0, span = 0.0;
	bool curved = false, wrap = false;
};

std::vector<WallChart> WallCharts(const MeshView& mesh, const std::vector<EVec>& normals,
	const std::vector<uint8_t>& eligible, const m::vec3& up, double thickness, std::vector<Int32>& chartOf)
{
	ProjectDamageAdjacency adjacency;
	if (!adjacency.Build(mesh)) return {};
	std::vector<WallChart> charts;
	for (Int32 f = 0; f < mesh.npolys; ++f) {
		if (!eligible[size_t(f)] || chartOf[size_t(f)] >= 0) continue;
		const Int32 id = Int32(charts.size());
		WallChart& chart = charts.emplace_back();
		chart.faces.push_back(f);
		chartOf[size_t(f)] = id;
		m::vec3 lo(1e300), hi(-1e300), normalSum(0.0);
		double area = 0.0;
		for (size_t i = 0; i < chart.faces.size(); ++i) {
			const Int32 face = chart.faces[i];
			const auto& q = mesh.polys[face];
			const m::vec3 n = Unit(normals[size_t(face)], m::vec3(0.0));
			const auto a = mesh.pts[q.a], b = mesh.pts[q.b], c = mesh.pts[q.c], d = mesh.pts[q.d];
			const double weight = m::la::length(m::la::cross(b - a, c - a)) +
				m::la::length(m::la::cross(c - a, d - a));
			normalSum += n * weight;
			area += weight;
			for (const auto& pos : {a, b, c, d}) {
				lo = m::la::min(lo, pos);
				hi = m::la::max(hi, pos);
			}
			for (Int32 next : adjacency.GetInfo(face)->face) {
				if (next < 0 || !eligible[size_t(next)] || chartOf[size_t(next)] >= 0 ||
					m::la::dot(n, Unit(normals[size_t(next)], m::vec3(0.0))) <= 0.5) continue;
				chartOf[size_t(next)] = id;
				chart.faces.push_back(next);
			}
		}
		chart.center = (lo + hi) * 0.5;
		chart.span = m::la::length(hi - lo);
		chart.normal = Unit(normalSum, Unit(normals[size_t(f)], m::vec3(0, 1, 0)));
		chart.wrap = area > 0.0 && m::la::length(normalSum) < 0.45 * area;
		double low = 1e300, high = -1e300, sumRadius = 0.0;
		size_t radiusCount = 0;
		for (Int32 face : chart.faces) {
			const auto& q = mesh.polys[face];
			for (Int32 point : {q.a, q.b, q.c, q.d}) {
				const m::vec3 delta = mesh.pts[point] - chart.center;
				const double depth = m::la::dot(chart.normal, delta);
				low = std::min(low, depth);
				high = std::max(high, depth);
				if (chart.wrap) {
					sumRadius += m::la::length(delta - up * m::la::dot(delta, up));
					++radiusCount;
				}
			}
		}
		chart.radius = radiusCount ? sumRadius / double(radiusCount) : 0.0;
		chart.wrap = chart.wrap && chart.radius > thickness;
		chart.curved = chart.wrap || high - low > std::max(0.03, 0.25 * thickness);
		const m::vec3 reference = std::abs(up.x) < 0.9 ? m::vec3(1, 0, 0) : m::vec3(0, 1, 0);
		chart.x = chart.wrap ? Unit(reference - up * m::la::dot(up, reference), m::vec3(1, 0, 0)) :
			MakeFrame(chart.normal, up, chart.center).x;
		chart.y = m::la::cross(up, chart.x);
	}
	return charts;
}

struct SeamPoint { m::vec3 position, normal; };

m::Manifold SweepSeam(std::vector<SeamPoint>& path, bool closed, const Surface& surf,
	const WallpaperParams& p, double detail, double down, double up, const m::vec3& noise, const Tags& tags)
{
	const double t = p.thickness;
	if (path.size() < (closed ? 3u : 2u)) return {};
	double length = 0.0;
	for (size_t i = 1; i < path.size(); ++i)
		length += m::la::length(path[i].position - path[i - 1].position);
	if (closed) length += m::la::length(path.front().position - path.back().position);
	if (length < std::max(2.0 * p.seamWidth, 2.0 * t)) return {};
	if (!closed) {
		path.front().position += Unit(path.front().position - path[1].position, m::vec3(0.0)) * (2.0 * t);
		path.back().position += Unit(path.back().position - path[path.size() - 2].position, m::vec3(0.0)) * (2.0 * t);
	}
	const double step = std::max({0.25, 1.0 / detail, length / 640.0});
	std::vector<SeamPoint> samples;
	const size_t segments = closed ? path.size() : path.size() - 1;
	for (size_t i = 0; i < segments; ++i) {
		const auto& a = path[i];
		const auto& b = path[(i + 1) % path.size()];
		const double edge = m::la::length(b.position - a.position);
		if (edge <= 1e-6) continue;
		const int n = std::max(1, int(std::ceil(edge / step)));
		for (int j = 0; j < n; ++j) {
			const double u = double(j) / n;
			const m::vec3 position = a.position * (1.0 - u) + b.position * u;
			if (!samples.empty() && m::la::length(position - samples.back().position) < 0.01 * t) continue;
			samples.push_back({position, Unit(a.normal * (1.0 - u) + b.normal * u, a.normal)});
		}
	}
	if (!closed && (samples.empty() || m::la::length(path.back().position - samples.back().position) >= 0.01 * t))
		samples.push_back(path.back());
	if (samples.size() < (closed ? 3u : 2u)) return {};

	constexpr int across = 3;
	std::vector<m::vec3> top(samples.size() * across), bottom(top.size());
	double distance = 0.0;
	for (size_t i = 0; i < samples.size(); ++i) {
		const auto& point = samples[i];
		const auto& before = samples[i ? i - 1 : closed ? samples.size() - 1 : i];
		const auto& after = samples[i + 1 < samples.size() ? i + 1 : closed ? 0 : i];
		if (i) distance += m::la::length(point.position - before.position);
		const m::vec3 lateral = Unit(m::la::cross(point.normal, after.position - before.position), m::vec3(0.0));
		if (m::la::length(lateral) < 0.5) return {};
		const double open = std::max(0.0, Fbm(m::vec3(distance / 23.0, 0.5, 0.0) + noise, 3) - 0.15);
		const double half = 0.5 * p.seamWidth * (1.0 + 4.0 * Clamp(p.seamOpen, 0.0, 1.0) * open);
		for (int j = 0; j < across; ++j) {
			m::vec3 q = point.position + lateral * (half * (j - 1));
			if (j != 1) {
				const double reach = std::max(3.0 * t, 2.0 * half);
				double hit = 0.0;
				int32_t face = -1;
				if (surf.bvh->Raycast(q + point.normal * reach, -point.normal, 2.0 * reach, &hit, &face) &&
					(!surf.side || (*surf.side)[size_t(face)]) &&
					m::la::dot(Unit((*surf.normals)[size_t(face)], point.normal), point.normal) > 0.5)
					q += point.normal * (reach - hit);
			}
			top[i * across + j] = q + point.normal * up;
			bottom[i * across + j] = q - point.normal * down;
		}
	}
	return GridSolid(top, bottom, int(samples.size()), across, tags, closed);
}

void CurvedSeams(const MeshView& mesh, const std::vector<EVec>& normals, const WallChart& chart,
	const Surface& surf, const WallpaperParams& p, const m::vec3& up, double detail, double depth,
	double clearance, const Tags& tags, std::vector<m::Manifold>& cuts)
{
	std::unordered_map<Int32, m::vec3> vertexNormals;
	double low = 1e300, high = -1e300;
	for (Int32 face : chart.faces) {
		const auto& q = mesh.polys[face];
		const m::vec3 ab = mesh.pts[q.b] - mesh.pts[q.a];
		const m::vec3 ac = mesh.pts[q.c] - mesh.pts[q.a];
		const m::vec3 areaNormal = m::la::cross(ab, ac) + m::la::cross(ac, mesh.pts[q.d] - mesh.pts[q.a]);
		const Int32 vertices[4] = {q.a, q.b, q.c, q.d};
		for (int i = 0; i < (q.c == q.d ? 3 : 4); ++i) {
			const Int32 v = vertices[i];
			vertexNormals.try_emplace(v, m::vec3(0.0)).first->second += areaNormal;
			const double u = m::la::dot(chart.x, mesh.pts[v]);
			low = std::min(low, u);
			high = std::max(high, u);
		}
	}
	const double margin = std::max(1.0, 2.0 * p.seamWidth);
	const int64_t first = chart.wrap ? 0 : int64_t(std::ceil((low + margin - p.seamOffset) / p.stripWidth));
	const int64_t end = chart.wrap ? int64_t(Clamp(std::round(2.0 * m::kPi * chart.radius / p.stripWidth), 3, 1024)) :
		std::min(first + 1024, 1 + int64_t(std::floor((high - margin - p.seamOffset) / p.stripWidth)));
	for (int64_t line = first; line < end; ++line) {
		const double angle = chart.wrap ? 2.0 * m::kPi * double(line) / double(end) + p.seamOffset / chart.radius : 0.0;
		const m::vec3 radial = chart.x * std::cos(angle) + chart.y * std::sin(angle);
		const m::vec3 axis = chart.wrap ? m::la::cross(up, radial) : chart.x;
		const double offset = (chart.wrap ? m::la::dot(axis, chart.center) :
			p.seamOffset + double(line) * p.stripWidth) + 1e-7;
		struct Link { size_t a, b; bool used = false; };
		struct Node { SeamPoint p; std::vector<size_t> links; };
		std::vector<Node> nodes;
		std::vector<Link> links;
		std::unordered_map<uint64_t, size_t> nodeOf;
		for (Int32 face : chart.faces) {
			const auto& q = mesh.polys[face];
			const Int32 tri[2][3] = {{q.a,q.b,q.c},{q.a,q.c,q.d}};
			for (int k = 0; k < (q.c == q.d ? 1 : 2); ++k) {
				size_t pair[2];
				int count = 0;
				for (int e = 0; e < 3; ++e) {
					const Int32 a = tri[k][e], b = tri[k][(e + 1) % 3];
					const double da = m::la::dot(axis, mesh.pts[a]) - offset;
					const double db = m::la::dot(axis, mesh.pts[b]) - offset;
					if ((da < 0.0) == (db < 0.0)) continue;
					const double u = da / (da - db);
					const m::vec3 pos = mesh.pts[a] * (1.0 - u) + mesh.pts[b] * u;
					if (chart.wrap && m::la::dot(radial, pos - chart.center) <= p.seamWidth) continue;
					const uint64_t key = (uint64_t(uint32_t(std::min(a, b))) << 32) | uint32_t(std::max(a, b));
					const auto found = nodeOf.try_emplace(key, nodes.size());
					if (found.second) {
						const m::vec3 n = Unit(vertexNormals.at(a) * (1.0 - u) + vertexNormals.at(b) * u,
							Unit(normals[size_t(face)], chart.normal));
						nodes.push_back({{pos, n}, {}});
					}
					pair[count++] = found.first->second;
				}
				if (count != 2 || pair[0] == pair[1]) continue;
				nodes[pair[0]].links.push_back(links.size());
				nodes[pair[1]].links.push_back(links.size());
				links.push_back({pair[0], pair[1]});
			}
		}
		auto trace = [&](size_t start, size_t link) {
			std::vector<SeamPoint> path{nodes[start].p};
			size_t current = start;
			bool closed = false;
			while (!links[link].used) {
				auto& edge = links[link];
				edge.used = true;
				current = edge.a == current ? edge.b : edge.a;
				if (current == start) { closed = true; break; }
				path.push_back(nodes[current].p);
				if (nodes[current].links.size() != 2) break;
				const auto& next = nodes[current].links;
				link = next[0] == link ? next[1] : next[0];
			}
			auto cut = SweepSeam(path, closed, surf, p, detail, depth, clearance,
				m::vec3(31.7 * double(line), 13.1, 5.3), tags);
			if (cut.Status() == m::Manifold::Error::NoError && cut.NumTri() > 0) cuts.push_back(std::move(cut));
		};
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i].links.size() != 2)
				for (size_t edge : nodes[i].links)
					if (!links[edge].used) trace(i, edge);
		for (size_t i = 0; i < links.size(); ++i)
			if (!links[i].used) trace(links[i].a, i);
	}
}

void PlaceWallpaperImpl(const CoreInput& in, const WallpaperParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<EVec>& normals,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& buildup,
	const std::vector<SurfacePaintRegion>* paint)
{
	const WallpaperFaceIds faceIds = WallpaperIds();
	const struct { uint32_t front, back, wall, flapFront, flapBack; } ids{
		uint32_t(faceIds.front), uint32_t(faceIds.back), uint32_t(faceIds.wall),
		uint32_t(faceIds.flapFront), uint32_t(faceIds.flapBack)};

	const MeshView& v = in.view;
	if (v.npolys <= 0 || normals.size() < size_t(v.npolys)) return;
	const double t = p.thickness;
	const double W = p.stripWidth;
	const double detail = Clamp(p.detail, 0.05, 4.0);
	const m::vec3 up = Unit(m::vec3(p.seamDirX, p.seamDirY, p.seamDirZ), m::vec3(0, 0, 1));
	const double wallSin = std::sin(Clamp(p.wallAngle, 0.0, 89.0) * m::kPi / 180.0);
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x57414c4c50415052ULL)                 
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	auto avoided = [&](Int32 f) {
		return p.avoid && in.avoidPoly && size_t(f) < in.avoidPoly->size() && (*in.avoidPoly)[size_t(f)];
	};
	const auto side = p.finderEnable ? FindWallpaperSide(in, p, up, wallSin) : std::vector<uint8_t>{};
	auto isWall = [&](Int32 f) {
		if (p.finderEnable) return side[size_t(f)] != 0;
		const m::vec3 n = Unit(normals[size_t(f)], m::vec3(0.0));
		return m::la::length(n) > 0.5 && std::abs(m::la::dot(n, up)) <= wallSin;
	};

	std::vector<uint8_t> eligible(size_t(v.npolys), 0);
	for (Int32 f = 0; f < v.npolys; ++f) eligible[size_t(f)] = isWall(f) && !avoided(f);
	std::vector<Int32> chartOf(size_t(v.npolys), -1);
	const auto charts = WallCharts(v, normals, eligible, up, t, chartOf);
	const auto chartAt = [&](Int32 face) -> const WallChart* {
		const Int32 id = chartOf[size_t(face)];
		return id >= 0 && charts[size_t(id)].curved ? &charts[size_t(id)] : nullptr;
	};
	ProjectDamageBVH bvh;
	bvh.Build(v);
	Surface surf;
	surf.bvh = &bvh;
	surf.eligible = &eligible;
	surf.normals = &normals;
	if (p.finderEnable) surf.side = &side;
	const double eps = std::max(1e-5, 1e-7 * m::la::length(in.bbRad));
	const double probeAuto = std::max(8.0 * t, 1.0);
	const double tol = std::max(6.0 * t, 0.25);

	                                                       
	m::Manifold band;
	{
		                                                                    
		const m::Manifold outer = BuildOffset(in, normals, 0.5 * t, nullptr, eps, ids.back);
		const m::Manifold inner = BuildOffset(in, normals, -t, &bvh, eps, ids.wall);
		if (outer.Status() == m::Manifold::Error::NoError && inner.Status() == m::Manifold::Error::NoError)
			band = outer - inner;
	}
	const bool banded = band.Status() == m::Manifold::Error::NoError && band.NumTri() > 0;
	const double downT = banded ? 1.5 * t : t;
	const double upT = 2.5 * t;

	std::vector<Tear> tears;

	                                                    
	std::vector<double> cdf, firstShare;
	double area = 0.0;
	for (Int32 f = 0; f < v.npolys; ++f) {
		const ProjectDamagePoly& q = v.polys[f];
		const bool skip = !eligible[size_t(f)];
		auto triArea = [&](Int32 a, Int32 b, Int32 c) {
			return 0.5 * m::la::length(m::la::cross(v.pts[b] - v.pts[a], v.pts[c] - v.pts[a]));
		};
		const double a1 = skip ? 0.0 : triArea(q.a, q.b, q.c);
		const double a2 = skip || q.c == q.d ? 0.0 : triArea(q.a, q.c, q.d);
		area += a1 + a2;
		cdf.push_back(area);
		firstShare.push_back(a1 + a2 > 0 ? a1 / (a1 + a2) : 1.0);
	}
	auto samplePoint = [&](uint64_t cand, uint64_t ch0, m::vec3& pos, m::vec3& n, Int32& face) {
		const double r0 = Random01(seed, cand, ch0);
		const size_t f = size_t(std::upper_bound(cdf.begin(), cdf.end(), r0 * area) - cdf.begin());
		if (f >= cdf.size()) return false;
		const ProjectDamagePoly& q = v.polys[f];
		const bool first = Random01(seed, cand, ch0 + 1) < firstShare[f] || q.c == q.d;
		const double r1 = std::sqrt(Random01(seed, cand, ch0 + 2)), r2 = Random01(seed, cand, ch0 + 3);
		pos = v.pts[q.a] * (1 - r1) + v.pts[first ? q.b : q.c] * (r1 * (1 - r2))
			+ v.pts[first ? q.c : q.d] * (r1 * r2);
		n = Unit(normals[f], m::vec3(0, 0, 1));
		face = Int32(f);
		return true;
	};
	auto sampleFrame = [&](Int32 face, const m::vec3& n, const m::vec3& pos) {
		const WallChart* chart = chartAt(face);
		Frame f = MakeFrame(chart && !chart->wrap && m::la::dot(chart->normal, n) > 0.5 ? chart->normal : n, up, pos);
		f.curved = chart != nullptr;
		return f;
	};
	auto sampleProbe = [&](Int32 face) {
		const WallChart* chart = chartAt(face);
		return chart ? std::max(probeAuto, std::min(0.36 * chart->span, 0.45 * std::max(W, p.tearLengthMax))) : probeAuto;
	};
	auto stripAt = [&](Int32 face, const Frame& f, const m::vec3& pos, double u) {
		const WallChart* chart = chartAt(face);
		if (chart && chart->wrap) {
			const auto delta = pos - chart->center;
			const double x = m::la::dot(delta, chart->x), y = m::la::dot(delta, chart->y);
			const double phi = std::atan2(y, x), radius = std::hypot(x, y);
			const double count = Clamp(std::round(2.0 * m::kPi * chart->radius / W), 3, 1024);
			const double step = 2.0 * m::kPi / count, offset = p.seamOffset / chart->radius;
			const double index = std::floor((phi - offset) / step);
			const double lower = offset + index * step - phi;
			if (radius > t && m::la::dot(f.x, m::la::cross(up, delta)) > 0.0)
				return V2(u + radius * std::sin(lower), u + radius * std::sin(lower + step));
		}
		const double start = p.seamOffset + std::floor((u - p.seamOffset) / W) * W;
		return V2(start, start + W);
	};
	auto samePlane = [&](const Tear& T, const Frame& f, double d) {
		return m::la::dot(T.f.n, f.n) > 0.995 && std::abs(T.planeD - d) < std::max(4.0 * t, 1.0);
	};
	auto overlaps = [&](const Tear& T, const Frame& f, double d, const V2& lo, const V2& hi) {
		return samePlane(T, f, d) && lo.x < T.hi.x && hi.x > T.lo.x && lo.y < T.hi.y && hi.y > T.lo.y;
	};
	auto flapSetup = [&](Tear& T, double lref) {
		T.lref = lref;
		T.flap = p.flapsEnable && p.flapAmount > 0.0;
		T.hanging = T.flap;
		T.share = Clamp(p.flapAmount, 0.0, 1.0);
		T.fold = Clamp(p.flapFold, 0.0, 180.0) * m::kPi / 180.0;
		T.foldR = std::max(3.0 * t, p.flapFoldRadius * lref);
		T.curl = Clamp(p.flapCurl, 0.0, 300.0) * m::kPi / 180.0;
		T.curlFocus = Clamp(p.flapCurlFocus, 1.0, 6.0);
		T.cone = Clamp(p.flapCone, 0.0, 1.0);
		T.wave = Clamp(p.flapWave, 0.0, 1.0);
		T.ragged = Clamp(p.flapRagged, 0.0, 1.0);
		T.step = std::max(1.5 * t, lref / (55.0 * detail));
		T.nrCut = int(Clamp(lref / T.step, 6.0, 160.0));
	};

	                                                                            
	const double diagonalRandomness = Clamp(p.tearDiagonalRandomness, 0.0, 1.0);
	const double fromTopRandomness = Clamp(p.tearFromTopRandomness, 0.0, 1.0);
	const double waveRandomness = Clamp(p.flapWaveRandomness, 0.0, 1.0);
	const double curlFoldRandomness = Clamp(p.flapCurlFoldRandomness, 0.0, 1.0);
	auto varyFlap = [&](Tear& T, uint64_t candidate) {
		if (!T.flap) return;
		if (waveRandomness > 0.0)
			T.wave = Mix(T.wave, Random01(seed, candidate, 63), waveRandomness);
		if (curlFoldRandomness > 0.0) {
			T.fold = Mix(T.fold, m::kPi * Random01(seed, candidate, 64), curlFoldRandomness);
			T.curl = Mix(T.curl, (300.0 * m::kPi / 180.0) * Random01(seed, candidate, 65), curlFoldRandomness);
		}
	};

	const size_t paintBegin = tears.size();
	if (p.paintEnable && paint) {
		for (size_t ci = 0; ci < paint->size(); ++ci) {
			const SurfacePaintRegion& r = (*paint)[ci];
			std::vector<m::vec3> pts;
			for (const EVec& q : r.points)
				if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
					&& (pts.empty() || m::la::length(q - pts.back()) > 1e-9))
					pts.push_back(q);
			if (pts.size() > 3 && m::la::length(pts.front() - pts.back()) <= 1e-9) pts.pop_back();
			if (pts.size() < 3) continue;
			m::vec3 c(0.0);
			for (const auto& q : pts) c += q;
			c /= double(pts.size());
			if (mask && !mask->Keep(c, int32_t(ci), mainSeed + 31000)) continue;
			Tear T;
			T.f = MakeFrame(r.normal, up, c);
			T.planeD = m::la::dot(T.f.n, c);
			std::vector<V2> raw;
			for (const auto& q : pts) raw.push_back(T.f.Local(q));
			V2 lo(1e300), hi(-1e300);
			for (const V2& q : raw) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
			const double size = std::max(hi.x - lo.x, hi.y - lo.y);
			if (size <= 4.0 * t) continue;
			flapSetup(T, size);
			varyFlap(T, 5000 + ci);
			T.probe = p.paintReach + 8.0 * t;
			double height = 0.0;
			Int32 face = -1;
			const double anchorProbe = T.probe + size;
			if (surf.Height(T.f, T.f.Local(pts.front()), anchorProbe, height, &face)) {
				if (chartAt(face)) {
					T.f.curved = true;
					T.probe = std::max(T.probe, std::min(anchorProbe, sampleProbe(face)));
				}
			} else if (p.finderEnable) continue;
			T.nz = m::vec3(101.3 * Random01(seed, 5000 + ci, 11), 57.1 * Random01(seed, 5000 + ci, 12),
				23.9 * Random01(seed, 5000 + ci, 13));
			                                       
			const double sp = 0.5 * T.step;
			for (size_t i = 0; i < raw.size(); ++i)
				RaggedEdge(T.poly, raw[i], raw[(i + 1) % raw.size()], p.paintJagged * 0.025 * size, sp,
					T.nz + m::vec3(0.0, 0.0, 7.3 * double(i)));
			                                                                     
			                                                
			T.Bounds();
			if (p.seamSnap > 0.0 && T.hi.x - T.lo.x > 3.0 * p.seamSnap) {
				const double xsL = p.seamOffset + std::round((T.lo.x - p.seamOffset) / W) * W;
				const double xsR = p.seamOffset + std::round((T.hi.x - p.seamOffset) / W) * W;
				const bool snapL = std::abs(xsL - T.lo.x) < p.seamSnap;
				const bool snapR = std::abs(xsR - T.hi.x) < p.seamSnap && xsR - xsL > 2.0 * p.seamSnap;
				for (V2& q : T.poly) {
					if (snapL && q.x < xsL + p.seamSnap) q.x = xsL;
					if (snapR && q.x > xsR - p.seamSnap) q.x = xsR;
				}
				T.Bounds();
			}
			                                                                
			const double third = (T.hi.x - T.lo.x) / 3.0;
			V2 pl(0, 1e300), pr(0, 1e300);
			for (const V2& q : T.poly) {
				if (q.x <= T.lo.x + third && q.y < pl.y) pl = q;
				if (q.x >= T.hi.x - third && q.y < pr.y) pr = q;
			}
			T.S = (pr.x - pl.x) > 1e-6 ? m::la::normalize(pr - pl) : V2(1, 0);
			if (T.S.x < 0.25) T.S = V2(1, 0);
			T.D = V2(-T.S.y, T.S.x);
			T.A = pl;
			                                                                 
			                                                                
			         
			T.entryHinge = false;
			T.down = downT;
			tears.push_back(std::move(T));
		}
	}
	const size_t paintEnd = tears.size();

	                                                                           
	struct Placed { m::vec3 n; double d; int64_t k; };
	std::vector<Placed> placed;
	const int markerCount = int(std::min<size_t>(200, p.flapMarkers.size() / 4));
	const int automaticCount = p.markerOnly ? 0 : p.tearAmount;
	if (p.tearsEnable && (automaticCount > 0 || markerCount > 0) && area > 0.0) {
		const int tries = markerCount + automaticCount * 48;
		int made = 0;
		for (int ci = 0; ci < tries && (ci < markerCount || made < automaticCount); ++ci) {
			const bool manual = ci < markerCount;
			const uint64_t cand = uint64_t(ci);
			auto rnd = [&](uint64_t ch) { return Random01(seed, cand, ch); };
			m::vec3 pos, n;
			Int32 face = -1;
			double markerLength = 0.0;
			if (manual) {
				const size_t off = size_t(ci) * 4;
				const m::vec3 q(p.flapMarkers[off], p.flapMarkers[off+1], p.flapMarkers[off+2]);
				markerLength = p.flapMarkers[off+3];
				if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(markerLength) || markerLength <= 0) continue;
				double best = std::numeric_limits<double>::infinity();
				for (Int32 j = 0; j < v.npolys; ++j) {
					if (!eligible[size_t(j)]) continue;
					const auto& poly = v.polys[j];
					double dist = TriangleDistanceSquared(q, v.pts[poly.a], v.pts[poly.b], v.pts[poly.c]);
					if (poly.c != poly.d) dist = std::min(dist, TriangleDistanceSquared(q, v.pts[poly.a], v.pts[poly.c], v.pts[poly.d]));
					if (dist < best) { best = dist; face = j; }
				}
				if (face < 0) continue;
				n = Unit(normals[size_t(face)], m::vec3(0, 0, 1));
				pos = q - n * m::la::dot(q - v.pts[v.polys[face].a], n);
			} else if (!samplePoint(cand, 0, pos, n, face)) continue;
			if (mask && !mask->Keep(pos, ci, mainSeed + 31100)) continue;
			const double scale = mask ? mask->SizeScale(pos) : 1.0;
			const Frame f = sampleFrame(face, n, pos);
			n = f.n;
			const double d = m::la::dot(n, pos);
			const V2 sp2 = f.Local(pos);
			const V2 strip = stripAt(face, f, pos, sp2.x);
			const double probe = sampleProbe(face);
			const int64_t k = int64_t(std::floor((sp2.x - p.seamOffset) / W));
			bool stripUsed = false;
			for (const Placed& q : placed)
				if (q.k == k && m::la::dot(q.n, n) > 0.995 && std::abs(q.d - d) < std::max(4.0 * t, 1.0)) stripUsed = true;
			const double x0 = strip.x, x1 = strip.y;
			const double W = x1 - x0;
			if (W <= 4.0 * t) continue;
			const double step = std::max(0.5, W / 80.0);
			                                                                                  
			const double xL = sp2.x - surf.March(f, sp2, V2(-1, 0), sp2.x - x0, step, probe, tol);
			const double xR = sp2.x + surf.March(f, sp2, V2(1, 0), x1 - sp2.x, step, probe, tol);
			if (xR - xL < 0.2 * W) continue;
			                                                                 
			                                            
			const double edgeOver = 2.0 * t + 0.02;
			auto wallEnds = [&](double x) {
				double h = 0.0;
				return !surf.Height(f, V2(x, sp2.y), probe, h) || (!f.curved && std::abs(h) > tol);
			};
			const bool leftEdge = wallEnds(xL - 2.0 * edgeOver), rightEdge = wallEnds(xR + 2.0 * edgeOver);
			const bool leftSeam = !leftEdge && xL <= x0 + 1e-6, rightSeam = !rightEdge && xR >= x1 - 1e-6;
			const double share = Mix(p.tearWidthMin, p.tearWidthMax, rnd(4));
			const bool longLeft = p.tearSide == 1 ? true : p.tearSide == 2 ? false : rnd(5) < 0.5;
			                                                               
			                                             
			const double xLc = leftEdge ? xL - edgeOver : xL, xRc = rightEdge ? xR + edgeOver : xR;
			double xa = xLc, xb = xRc;
			if (share < 0.98) {
				if (longLeft) xb = std::min(xR, xL + share * W);
				else xa = std::max(xL, xR - share * W);
				if (xb < xa + 0.15 * W) continue;
			}
			if (xb - xa < 0.15 * W) continue;
			double L = manual ? markerLength : Mix(p.tearLengthMin, p.tearLengthMax, rnd(6)) * scale;
			double diagonal = Clamp(p.tearDiagonal, 0.0, 1.0);
			if (diagonalRandomness > 0.0) diagonal = Mix(diagonal, rnd(61), diagonalRandomness);
			double Ls = L * (1.0 - diagonal * Mix(0.65, 0.97, rnd(7)));
			Ls = std::max(Ls, 0.04 * L);
			double fromTopChance = p.tearFromTop;
			if (fromTopRandomness > 0.0 && !manual && !stripUsed)
				fromTopChance = Mix(Clamp(fromTopChance, 0.0, 1.0), rnd(62), fromTopRandomness);
			const bool fromTop = !manual && !stripUsed && rnd(8) < fromTopChance;
			const double maxUp = 4.0 * std::max(p.tearLengthMax, L);
			const double ea = std::min(0.02 * W, 0.5), xsa = xa + ea, xsb = xb - ea;
			const double topA = sp2.y + surf.March(f, V2(xsa, sp2.y), V2(0, 1), maxUp, step, probe, tol);
			const double topB = sp2.y + surf.March(f, V2(xsb, sp2.y), V2(0, 1), maxUp, step, probe, tol);
			double yTopA, yTopB;
			if (fromTop) {
				yTopA = topA; yTopB = topB;
			} else {
				const double c = std::min(sp2.y + 0.5 * L, std::min(topA, topB) - 0.05 * L);
				yTopA = yTopB = c;
			}
			                                         
			const double xl = longLeft ? xsa : xsb;
			const double yTopL = longLeft ? yTopA : yTopB;
			const double down = surf.March(f, V2(xl, yTopL - 0.5), V2(0, -1), L + 2.0, step, probe, tol);
			if (down < L) { const double s = std::max(0.2, (down - 1.0) / L); L *= s; Ls *= s; }
			if (L < 4.0 * t) continue;
			const double yfa = yTopA - (longLeft ? L : Ls), yfb = yTopB - (longLeft ? Ls : L);

			Tear T;
			T.f = f;
			T.planeD = d;
			T.probe = probe;
			T.nz = m::vec3(101.3 * rnd(11), 57.1 * rnd(12), 23.9 * rnd(13));
			flapSetup(T, L);
			varyFlap(T, cand);
			T.down = downT;
			T.A = V2(xa, yfa);
			const V2 B(xb, yfb);
			T.S = m::la::normalize(B - T.A);
			T.D = V2(-T.S.y, T.S.x);
			T.entryHinge = false;
			T.frontLen = m::la::length(B - T.A);
			T.frontAmp = Clamp(p.tearJagged, 0.0, 1.0) * 0.035 * L;
			T.frontFreq = 1.0 / std::max(1e-6, 0.09 * L);
			T.frontNz = T.nz + m::vec3(0.0, 11.0, 0.0);
			const double spc = 0.5 * T.step;
			const double over = fromTop ? 2.0 * t + 0.02 : 0.0;
			                 
			const int nf = std::max(2, int(std::ceil(T.frontLen / spc)));
			for (int i = 0; i < nf; ++i) {
				const double s = T.frontLen * i / nf;
				T.poly.push_back(T.A + T.S * s + T.D * T.FrontR(s));
			}
			                                                                     
			const double sideAmp = Clamp(p.tearJagged, 0.0, 1.0) * 0.03 * W;
			const bool rightStraight = (!longLeft || share >= 0.98);
			const bool leftStraight = (longLeft || share >= 0.98);
			const V2 tr(xb, yTopB + over), tl(xa, yTopA + over);
			if (rightStraight) T.poly.push_back(B);
			else RaggedEdge(T.poly, B, tr, -sideAmp, spc, T.nz + m::vec3(5.0, 0, 0));
			                      
			if (fromTop) T.poly.push_back(tr);
			else RaggedEdge(T.poly, tr, tl, Clamp(p.tearJagged, 0.0, 1.0) * 0.03 * L, spc, T.nz + m::vec3(9.0, 0, 0));
			                  
			if (leftStraight) T.poly.push_back(tl);
			else RaggedEdge(T.poly, tl, T.A, -sideAmp, spc, T.nz + m::vec3(13.0, 0, 0));
			T.Bounds();
			bool clash = false;
			for (size_t i = 0; i < tears.size(); ++i)
				if (overlaps(tears[i], f, d, T.lo + V2(0.001), T.hi - V2(0.001))) clash = true;
			if (clash) continue;
			placed.push_back({n, d, k});
			if (!manual) ++made;

			                                                      
			const bool seamBelow = longLeft ? (leftSeam && xa <= x0 + 1e-6) : (rightSeam && xb >= x1 - 1e-6);
			if (seamBelow && rnd(9) < p.seamCrack) {
				Tear C;
				C.f = f; C.planeD = d; C.probe = probe; C.down = downT;
				C.nz = T.nz + m::vec3(21.0, 0, 0);
				const double xs = longLeft ? xa : xb, sgn = longLeft ? 1.0 : -1.0;
				const double yTop = (longLeft ? yfa : yfb) + 0.6;
				const double len = L * p.seamCrackLength * Mix(0.6, 1.4, rnd(10));
				const double maxDn = surf.March(f, V2(xs + sgn * ea, yTop - 0.6), V2(0, -1), len, step, probe, tol);
				const double ln = std::min(len, maxDn);
				const double w0 = std::max(0.3, std::max(4.0 * p.seamWidth, 0.012 * W));
				if (ln > 2.0) {
					C.step = std::max(1.5 * t, ln / (40.0 * detail));
					C.nrCut = 3;
					const int ns = std::max(4, int(std::ceil(ln / C.step)));
					C.poly.push_back(V2(xs, yTop));
					C.poly.push_back(V2(xs, yTop - ln));
					for (int i = ns - 1; i >= 0; --i) {
						const double u = double(i) / ns;                            
						const double y = yTop - ln * (1.0 - u);
						const double w = w0 * std::pow(u, 0.7) * (1.0 + 0.45 * TornEdge(y / 3.0, C.nz));
						C.poly.push_back(V2(xs + sgn * std::max(0.02, w), y));
					}
					C.A = V2(xs, yTop - ln);
					C.S = V2(0, 1);
					C.D = V2(sgn, 0);
					C.Bounds();
					tears.push_back(std::move(C));
				}
			}
			tears.push_back(std::move(T));
		}
	}

	                                                                             
	if (p.liftsEnable && p.liftAmount > 0 && area > 0.0) {
		const int tries = p.liftAmount * 12;
		int made = 0;
		for (int ci = 0; ci < tries && made < p.liftAmount; ++ci) {
			const uint64_t cand = 1000000 + uint64_t(ci);
			auto rnd = [&](uint64_t ch) { return Random01(seed, cand, ch); };
			m::vec3 pos, n;
			Int32 face = -1;
			if (!samplePoint(cand, 0, pos, n, face)) continue;
			if (mask && !mask->Keep(pos, ci, mainSeed + 31200)) continue;
			const double scale = mask ? mask->SizeScale(pos) : 1.0;
			const Frame f = sampleFrame(face, n, pos);
			n = f.n;
			const double d = m::la::dot(n, pos);
			const V2 sp2 = f.Local(pos);
			const V2 strip = stripAt(face, f, pos, sp2.x);
			const double probe = sampleProbe(face);
			const double x0 = strip.x, W = strip.y - strip.x;
			if (W <= 4.0 * t) continue;
			const bool left = (sp2.x - x0) < 0.5 * W;
			const double xs = left ? x0 : x0 + W;
			const double sgn = left ? 1.0 : -1.0;                           
			const double len = Mix(p.liftLengthMin, p.liftLengthMax, rnd(20)) * scale;
			const double wmax = std::min(0.3 * W, Mix(p.liftWidthMin, p.liftWidthMax, rnd(21)) * scale);
			const double y0 = sp2.y - 0.5 * len, y1 = sp2.y + 0.5 * len;
			                                                                       
			bool ok = true;
			for (int i = 0; i <= 6 && ok; ++i) {
				double h = 0.0;
				const double y = Mix(y0, y1, i / 6.0);
				for (double off : {-0.5, wmax + 0.5})
					if (!surf.Height(f, V2(xs + sgn * off, y), probe, h) || (!f.curved && std::abs(h) > tol)) ok = false;
			}
			if (!ok) continue;
			Tear T;
			T.f = f; T.planeD = d; T.probe = probe; T.down = downT;
			T.nz = m::vec3(101.3 * rnd(22), 57.1 * rnd(23), 23.9 * rnd(24));
			flapSetup(T, wmax);
			T.step = std::max(1.5 * t, std::min(len / (40.0 * detail), wmax / 6.0));
			T.nrCut = 8;
			T.foldWhole = true;
			T.fold = Clamp(p.liftCurl, 0.0, 300.0) * m::kPi / 180.0;
			T.curl = 0.0;
			T.share = 1.0;
			T.ragged = 0.0;
			T.flap = p.liftCurl > 0.0;
			T.hanging = false;
			const int ns = std::max(6, int(std::ceil(len / (0.5 * T.step))));
			T.poly.push_back(V2(xs, y0));
			T.poly.push_back(V2(xs, y1));
			for (int i = ns - 1; i >= 1; --i) {
				const double u = double(i) / ns;
				const double y = Mix(y0, y1, u);
				const double w = wmax * std::pow(std::sin(m::kPi * u), 0.45)
					* (1.0 + 0.3 * Clamp(p.tearJagged, 0.0, 1.0) * TornEdge(y / std::max(1.0, 0.3 * wmax), T.nz));
				T.poly.push_back(V2(xs + sgn * std::max(0.05 * wmax, w), y));
			}
			T.D = V2(-sgn, 0);                                     
			T.S = V2(T.D.y, -T.D.x);                               
			T.A = V2(xs + sgn * 2.0 * wmax, y0);
			T.entryHinge = true;
			T.Bounds();
			bool clash = false;
			for (size_t i = 0; i < tears.size() && !clash; ++i)
				if (overlaps(tears[i], f, d, T.lo - V2(1.0), T.hi + V2(1.0))) clash = true;
			if (clash) continue;
			tears.push_back(std::move(T));
			++made;
		}
	}

	                                                                              
	std::vector<m::Manifold> curvedCuts;
	if(p.manualSeams) {
        for(size_t i=0;i+11<p.seamEdges.size();i+=12) {
            const auto& e=p.seamEdges;
            std::vector<SeamPoint> path{{m::vec3(e[i],e[i+1],e[i+2]),m::vec3(e[i+3],e[i+4],e[i+5])},
                {m::vec3(e[i+6],e[i+7],e[i+8]),m::vec3(e[i+9],e[i+10],e[i+11])}};
            auto cut=SweepSeam(path,false,surf,p,detail,1.5*t,upT,m::vec3(0.0),Tags{ids.back,ids.wall,ids.back});
            if(cut.Status()==m::Manifold::Error::NoError && cut.NumTri()>0)curvedCuts.push_back(std::move(cut));
        }
    } else if (p.seamsEnable && p.seamDepth > 0.0) {
		const double grooveDown = std::min(1.0, p.seamDepth) * t + (p.seamDepth >= 1.0 && banded ? 0.5 * t : 0.0);
		for (const auto& chart : charts)
			if (chart.curved)
				CurvedSeams(v, normals, chart, surf, p, up, detail, grooveDown, upT,
					Tags{ids.back, p.seamDepth >= 1.0 ? ids.wall : ids.back, ids.back}, curvedCuts);
		std::unordered_map<PlaneKey, size_t, PlaneKeyHash> index;
		std::vector<WallPlane> planes;
		const double dq = std::max(1.0, 8.0 * t);
		for (Int32 f = 0; f < v.npolys; ++f) {
			if (!eligible[size_t(f)] || chartAt(f)) continue;
			const m::vec3 n = Unit(normals[size_t(f)], m::vec3(0, 0, 1));
			const ProjectDamagePoly& q = v.polys[f];
			const double d = m::la::dot(n, v.pts[q.a]);
			const PlaneKey key{int64_t(std::llround(n.x * 300.0)), int64_t(std::llround(n.y * 300.0)),
				int64_t(std::llround(n.z * 300.0)), int64_t(std::llround(d / dq))};
			auto it = index.find(key);
			if (it == index.end()) {
				WallPlane wp;
				wp.f = MakeFrame(n, up, v.pts[q.a]);
				wp.d = d;
				it = index.emplace(key, planes.size()).first;
				planes.push_back(wp);
			}
			WallPlane& wp = planes[it->second];
			const Int32 ids[4] = {q.a, q.b, q.c, q.d};
			for (int c = 0; c < (q.c == q.d ? 3 : 4); ++c) {
				const V2 l = wp.f.Local(v.pts[ids[c]]);
				wp.lo = m::la::min(wp.lo, l);
				wp.hi = m::la::max(wp.hi, l);
			}
		}
		for (size_t pi = 0; pi < planes.size(); ++pi) {
			const WallPlane& wp = planes[pi];
			const double margin = std::max(1.0, 2.0 * p.seamWidth);
			const int64_t k0 = int64_t(std::ceil((wp.lo.x + margin - p.seamOffset) / W));
			const int64_t k1 = int64_t(std::floor((wp.hi.x - margin - p.seamOffset) / W));
			for (int64_t k = k0; k <= k1 && k - k0 < 4096; ++k) {
				const double xs = p.seamOffset + double(k) * W;
				Tear G;
				G.f = wp.f; G.planeD = wp.d; G.probe = probeAuto;
				G.down = grooveDown;
				G.floorWall = p.seamDepth >= 1.0;
				G.nz = m::vec3(31.7 * double(k), 13.1 * double(pi), 5.3);
				const double y0 = wp.lo.y - 2.0 * t, y1 = wp.hi.y + 2.0 * t;
				G.step = std::max(1.5 * t, std::min(2.0, (y1 - y0) / 400.0)) / detail;
				G.step = std::max(G.step, (y1 - y0) / 600.0);
				G.nrCut = 2;
				const int ns = std::max(4, int(std::ceil((y1 - y0) / G.step)));
				std::vector<V2> rightSide;
				for (int i = 0; i <= ns; ++i) {
					const double y = Mix(y0, y1, double(i) / ns);
					const double open = std::max(0.0, Fbm(m::vec3(y / 23.0, 0.5, 0.0) + G.nz, 3) - 0.15);
					const double hw = 0.5 * p.seamWidth * (1.0 + 4.0 * Clamp(p.seamOpen, 0.0, 1.0) * open);
					G.poly.push_back(V2(xs - hw, y));
					rightSide.push_back(V2(xs + hw, y));
				}
				for (auto it = rightSide.rbegin(); it != rightSide.rend(); ++it) G.poly.push_back(*it);
				G.A = V2(xs + 10.0 * p.seamWidth, y0);
				G.S = V2(0, 1);
				G.D = V2(-1, 0);
				G.Bounds();
				tears.push_back(std::move(G));
			}
		}
	}

	                                                                               
	const double lift = -1.02 * t;                                                   
	const double anchorLen = std::max(5.0 * t, 0.6);
	std::vector<m::Manifold> cuts(tears.size()), flaps(tears.size());
	ProjectDamageParallelFor(0, int64_t(tears.size()), [&](int64_t ti) {
		const Tear& T = tears[size_t(ti)];
		std::vector<Column> cols = Columns(T, 0.05 * t);
		if (cols.size() < 2) return;
		cuts[size_t(ti)] = BuildCutter(T, cols, surf, upT, Tags{ids.back, T.floorWall ? ids.wall : ids.back, ids.back});
		if (!T.flap) return;
		for (Column& c : cols) {
			if (T.entryHinge) {
				c.rf = c.rIn;
				c.anchor = true;
			} else {
				c.rf = T.FrontR(c.s);
				c.anchor = c.rIn - c.rf < std::max(0.5 * T.step, 3.0 * t);
				if (c.anchor) c.rf = c.rIn;
			}
		}
		flaps[size_t(ti)] = T.hanging
			? BuildFlap(T, cols, surf, t, lift, anchorLen, Tags{ids.flapFront, ids.flapBack, ids.flapBack})
			: BuildFlap(T, cols, surf, t, lift, anchorLen, Tags{ids.front, ids.back, ids.back});
	});

	std::vector<m::Manifold> parts = std::move(curvedCuts);
	for (auto& c : cuts)
		if (c.Status() == m::Manifold::Error::NoError && c.NumTri() > 0) parts.push_back(std::move(c));
	if (!parts.empty()) {
		const m::Manifold all = m::Manifold::BatchBoolean(parts, m::OpType::Add);
		m::Manifold cut = banded ? all ^ band : all;
		                                                                     
		                                                                   
		if (cut.Status() != m::Manifold::Error::NoError) cut = all;
		if (cut.Status() == m::Manifold::Error::NoError && cut.NumTri() > 0) cutters.push_back(std::move(cut));
	}
	for (auto& fl : flaps)
		if (fl.Status() == m::Manifold::Error::NoError && fl.NumTri() > 0) buildup.push_back(std::move(fl));
}
}              

std::vector<uint8_t> WallpaperPaperedPolys(const CoreInput& in, const WallpaperParams& raw)
{
	std::vector<uint8_t> out;
	if (!raw.enable) return out;
	const WallpaperParams p = Sanitized(raw);
	const MeshView& v = in.view;
	if (v.npolys <= 0 || v.polys == nullptr || v.pts == nullptr) return out;
	out.assign(size_t(v.npolys), 0);
	const m::vec3 up = Unit(m::vec3(p.seamDirX, p.seamDirY, p.seamDirZ), m::vec3(0, 0, 1));
	const double wallSin = std::sin(Clamp(p.wallAngle, 0.0, 89.0) * m::kPi / 180.0);
	if (p.finderEnable) out = FindWallpaperSide(in, p, up, wallSin);
	for (Int32 f = 0; f < v.npolys; ++f) {
		if (p.avoid && in.avoidPoly && size_t(f) < in.avoidPoly->size() && (*in.avoidPoly)[size_t(f)]) {
			out[size_t(f)] = 0;
			continue;
		}
		if (!p.finderEnable) {
			const m::vec3 n = WallNormal(v, v.polys[f]);
			out[size_t(f)] = m::la::length(n) > 0.5 && std::abs(m::la::dot(n, up)) <= wallSin;
		}
	}
	return out;
}

void PlaceWallpaper(const CoreInput& in, const WallpaperParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<EVec>& normals,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& buildup,
	const std::vector<SurfacePaintRegion>* paint)
{
	if (!p.enable) return;
	                                                                      
	                                                                       
	                              
	std::vector<m::Manifold> cut, add;
	try {
		PlaceWallpaperImpl(in, Sanitized(p), mainSeed, mask, normals, cut, add, paint);
	} catch (const std::exception&) {
		return;
	}
	for (auto& c : cut) cutters.push_back(std::move(c));
	for (auto& b : add) buildup.push_back(std::move(b));
}
PROJECT_DAMAGE_NS_MACRO_END
