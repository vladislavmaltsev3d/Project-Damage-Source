// делим объём на замкнутые осколки выбранного типа
// ProjectDamage cell fracture — GPL-3.0-or-later; see LICENSE.
//
// Recipe: cell-fracture-v1. See cell_fracture.h / cell_fracture_params.h.
//
// Basis: J. Ronnegren, "Real Time Mesh Fracturing Using 2D Voronoi Diagrams",
// BTH 2020. The thesis fractures a mesh by (Alg. 1) generating random points,
// moving them toward the hit, building a Voronoi diagram (Fortune) and
// clipping its cells with the mesh polygon (Sutherland-Hodgman), then
// (Alg. 2) extruding every clipped cell through the depth. Its limits, named
// in its own discussion and future work, are: convex 2D polygons only, flat
// planar cells that look geometric, and a small hit still splitting the whole
// object into pieces out to its corners. Here:
//
//   thesis                         cell-fracture-v1
//   2D diagram, extruded           3D cells; on thin objects (Auto) the
//                                  seeds sit on the mid-surface, so every
//                                  cell runs through the thickness - the
//                                  thesis's extrusion, on any shape
//   points moved toward the hit    density field from empties, objects and
//                                  drawn strokes; radial + ring pattern
//                                  around glass hits (its circular pattern)
//   Sutherland-Hodgman, convex     exact Boolean source ∩ cell: concave and
//                                  holed sources work, pieces may be concave
//   flat convex cells              cells refined and bent by one smooth
//                                  material noise field - rough, non-planar
//                                  break faces that still tile
//   outer pieces to the corners    Intact Remainder: the far cells stay one
//                                  piece (its "merge the outer parts")
//
// 1. Volume V, piece size h = cbrt(V / pieces). Material table (below).
// 2. Planar: Auto when the PCA thickness < planarRatio x the middle extent.
// 3. Density rho(x) = 1 + (proxDensity - 1) max_k s_k exp(-(d_k / r_k)^falloff)
//    over proximity points and (Shatter) the stroke polylines.
// 4. Seeds (inside = ray parity on the BVH): background by rejection of rho
//    with spacing ~ h rho^-1/3; clustering (size variation); extra seeds at
//    the sources; rubble packed at point sources; glass polar layout; crack
//    lines as mirrored pairs (their bisector runs along the stroke and in
//    along the surface normal). Planar: all onto the mid-surface.
// 5. Wood: everything in a space compressed along the grain by k:
//    A = I + (1/k - 1) g g^T (cells k times longer along the grain).
// 6. Cell i: padded box trimmed by the power-bisector planes (RB fracture's
//    exact Voronoi), each plane moved out by eps (gap 0) or in by gap / 2.
// 7. Relief D(y): a vector fBm (rock: ridged first octave) whose amplitude
//    is capped so |grad D| stays < 1 - the bend is then a bijection, so bent
//    cells still partition space. Wood adds a shear along the grain whose
//    noise varies ACROSS the fibres (det = 1, never folds): torn, splintery
//    ends. W_i = Warp(RefineToLength(cell_i), D).
// 8. piece_i = source ∩ W_i − ∪ {W_j : j < i, boxes overlap}. With the eps
//    dilation every point lies in some W, and goes to the first: exact
//    tiling with no overlap. Remainder = source − ∪ W_near.
// 9. Decompose, map back by A^-1. Nothing is dropped: stray bits of a cell
//    and, above Max Pieces, the smallest pieces merge into the neighbour
//    they touch most. Volume and centre of mass.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "cell_fracture.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

double Rnd(uint64_t seed, uint64_t k, uint64_t ch)
{
	uint64_t z = seed ^ (k * 0x9e3779b97f4a7c15ULL) ^ ((ch + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}

V Unit(const V& v, const V& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; }

                                                                                    
struct Material {
	double var;                                
	double rubble;                     
	double rough;                             
	int    octaves;                    
	bool   ridged;                                          
	double prim;                                                       
	double cluster;                            
	bool   planar;                          
};
                                                                           
const Material kMat[5] = {
	{0.30, 0.08, 0.45, 2, false, 0.00, 0.35, true },            
	{0.35, 0.00, 0.00, 1, false, 0.00, 0.35, true },             
	{0.45, 0.35, 0.90, 3, false, 0.20, 0.35, true },                
	{0.60, 0.12, 0.85, 3, true,  0.30, 0.40, false},            
	{0.80, 0.60, 1.00, 3, false, 0.45, 0.16, false},            
};

struct Source { V a, b; double r, s; bool segment; };

double SegDist(const V& x, const V& a, const V& b)
{
	const V ab = b - a;
	const double L2 = m::la::dot(ab, ab);
	const double t = L2 > 1e-24 ? std::max(0.0, std::min(1.0, m::la::dot(x - a, ab) / L2)) : 0.0;
	return m::la::length(x - (a + ab * t));
}

struct Solid {
	const MeshView* view = nullptr;
	ProjectDamageBVH bvh;
	V lo, hi;
	bool Inside(const V& p) const
	{
		const V dir = Unit(V(0.5773, 0.6123, 0.5401), V(1, 0, 0));
		V o = p;
		int hits = 0;
		const double eps = 1e-7 * m::la::length(hi - lo) + 1e-9;
		for (int guard = 0; guard < 256; ++guard) {
			double d = 0.0;
			int32_t poly = -1;
			if (!bvh.Raycast(o, dir, 1e30, &d, &poly)) break;
			++hits;
			o = o + dir * (d + eps);
		}
		return (hits & 1) == 1;
	}
	double Thickness(const V& p, const V& n) const
	{
		double d = 0.0;
		int32_t poly = -1;
		const double eps = 1e-6 * m::la::length(hi - lo) + 1e-9;
		return bvh.Raycast(p - n * eps, n * -1.0, 1e30, &d, &poly) ? d : 0.0;
	}
	                                                                      
	bool Nearest(const V& p, V& q, V& n) const
	{
		double best = std::numeric_limits<double>::infinity();
		const MeshView& v = *view;
		for (Int32 i = 0; i < v.npolys; ++i) {
			const ProjectDamagePoly& f = v.polys[i];
			const Int32 ids[2][3] = {{f.a, f.b, f.c}, {f.a, f.c, f.d}};
			for (int k = 0; k < (f.c == f.d ? 1 : 2); ++k) {
				const V a = v.pts[ids[k][0]], b = v.pts[ids[k][1]], c = v.pts[ids[k][2]];
				const V ab = b - a, ac = c - a, ap = p - a;
				const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
				V x;
				const V bp = p - b; const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
				const V cp = p - c; const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
				const double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
				if (d1 <= 0 && d2 <= 0) x = a;
				else if (d3 >= 0 && d4 <= d3) x = b;
				else if (vc <= 0 && d1 >= 0 && d3 <= 0) x = a + ab * (d1 / (d1 - d3));
				else if (d6 >= 0 && d5 <= d6) x = c;
				else if (vb <= 0 && d2 >= 0 && d6 <= 0) x = a + ac * (d2 / (d2 - d6));
				else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) x = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
				else { const double den = 1.0 / (va + vb + vc); x = a + ab * (vb * den) + ac * (vc * den); }
				const double dd = m::la::dot(p - x, p - x);
				if (dd < best) { best = dd; q = x; n = Unit(m::la::cross(ab, ac), V(0, 0, 1)); }
			}
		}
		return std::isfinite(best);
	}
	                                                          
	V MidSurface(const V& x) const
	{
		V q, n;
		if (!Nearest(x, q, n)) return x;
		const double T = Thickness(q, n);
		return T > 0.0 ? q - n * (0.5 * T) : x;
	}
};

                                                                        
                                    
void Principal(const MeshView& v, V axis[3], double ext[3], V& centre)
{
	const V* directionPoints = v.guidePoints ? v.guidePoints : v.pts;
	V c(0.0);
	for (Int32 i = 0; i < v.npts; ++i) c += directionPoints[i];
	c = c / double(std::max<Int32>(1, v.npts));
	double a[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
	for (Int32 i = 0; i < v.npts; ++i) {
		const V d = directionPoints[i] - c;
		const double dv[3] = {d.x, d.y, d.z};
		for (int r = 0; r < 3; ++r) for (int s = 0; s < 3; ++s) a[r][s] += dv[r] * dv[s];
	}
	double e[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	for (int sweep = 0; sweep < 32; ++sweep) {
		const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
		if (off < 1e-30) break;
		for (int pq = 0; pq < 3; ++pq) {
			const int p = pq == 2 ? 1 : 0, q = pq == 0 ? 1 : 2;
			if (std::abs(a[p][q]) < 1e-300) continue;
			const double th = 0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
			const double cs = std::cos(th), sn = std::sin(th);
			for (int k = 0; k < 3; ++k) {
				const double akp = a[k][p], akq = a[k][q];
				a[k][p] = cs * akp - sn * akq; a[k][q] = sn * akp + cs * akq;
			}
			for (int k = 0; k < 3; ++k) {
				const double apk = a[p][k], aqk = a[q][k];
				a[p][k] = cs * apk - sn * aqk; a[q][k] = sn * apk + cs * aqk;
			}
			for (int k = 0; k < 3; ++k) {
				const double ekp = e[k][p], ekq = e[k][q];
				e[k][p] = cs * ekp - sn * ekq; e[k][q] = sn * ekp + cs * ekq;
			}
		}
	}
	int order[3] = {0, 1, 2};
	std::sort(order, order + 3, [&](int x, int y) { return a[x][x] < a[y][y]; });
	for (int k = 0; k < 3; ++k) {
		const int j = order[k];
		axis[k] = Unit(V(e[0][j], e[1][j], e[2][j]), V(k == 0, k == 1, k == 2));
		double lo = 1e300, hi = -1e300;
		for (Int32 i = 0; i < v.npts; ++i) {
			const double t = m::la::dot(v.pts[i] - c, axis[k]);
			lo = std::min(lo, t); hi = std::max(hi, t);
		}
		ext[k] = v.npts > 0 ? hi - lo : 0.0;
	}
	centre = c;
}

                                                             
struct Relief {
	int    octaves = 0;
	double f0 = 1.0, amp = 0.0;
	bool   ridged = false;
	                                                         
	V      g = V(0, 0, 1);
	double splAmp = 0.0, splPerp = 1.0, splAlong = 1.0;
	V      off[4];

	static double S(const V& p) { return 2.0 * ProjectDamageNoise(EVec(p.x, p.y, p.z)) - 1.0; }
	bool Active() const { return amp > 0.0 || splAmp > 0.0; }
	double Fbm(const V& y, int c) const
	{
		double s = 0.0, a = 1.0, f = f0;
		for (int o = 0; o < octaves; ++o) {
			double n = S(y * f + off[c] + V(17.13, 31.71, 7.37) * double(o));
			if (ridged && o == 0) n = 0.5 - std::abs(n);                            
			s += a * n;
			a *= 0.5; f *= 2.0;
		}
		return s;
	}
	V operator()(const V& y) const
	{
		V d(0.0);
		if (amp > 0.0) d = V(Fbm(y, 0), Fbm(y, 1), Fbm(y, 2)) * amp;
		if (splAmp > 0.0) {
			const double along = m::la::dot(y, g);
			const V perp = y - g * along;
			double s = 0.0, a = 1.0, fp = splPerp;
			for (int o = 0; o < 2; ++o, a *= 0.55, fp *= 2.3)
				s += a * S(perp * fp + g * (along * splAlong) + off[3] + V(5.3, 11.1, 2.9) * double(o));
			d += g * (splAmp * s);
		}
		return d;
	}
};

V MassCentre(const m::Manifold& pc, double& vol)
{
	const m::MeshGL64 mg = pc.GetMeshGL64();
	vol = 0.0;
	V c(0.0);
	for (size_t t = 0; t < mg.NumTri(); ++t) {
		V q[3];
		for (int kk = 0; kk < 3; ++kk) {
			const size_t vi = size_t(mg.triVerts[3 * t + size_t(kk)]) * mg.numProp;
			q[kk] = V(mg.vertProperties[vi], mg.vertProperties[vi + 1], mg.vertProperties[vi + 2]);
		}
		const double v6 = m::la::dot(q[0], m::la::cross(q[1], q[2]));
		vol += v6;
		c += (q[0] + q[1] + q[2]) * v6;
	}
	vol /= 6.0;
	return std::abs(vol) > 1e-18 ? c / (24.0 * vol) : (pc.BoundingBox().min + pc.BoundingBox().max) * 0.5;
}

bool Overlap(const m::Box& a, const m::Box& b)
{
	return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y
		&& a.min.z <= b.max.z && b.min.z <= a.max.z;
}

                                                                       
                                                                         
                                                                            
                                                                          
                          
void MergePieces(std::vector<m::Manifold>& pcs, const std::vector<size_t>& owner, size_t cap, double tol)
{
	const size_t n = pcs.size();
	std::vector<double> vol(n);
	std::vector<m::Box> bb(n);
	ProjectDamageParallelFor(0, int64_t(n), [&](int64_t ii) {
		vol[size_t(ii)] = pcs[size_t(ii)].Volume(); bb[size_t(ii)] = pcs[size_t(ii)].BoundingBox();
	});
	std::vector<char> alive(n, 1), stray(n, 0);
	auto grown = [&](m::Box b) { b.min -= V(tol); b.max += V(tol); return b; };
	                                                                          
	auto neighbour = [&](size_t i) {
		size_t best = SIZE_MAX;
		double bestTouch = -1.0, bestOv = -1.0, bestGap = 1e300;
		const m::Box bi = grown(bb[i]);
		for (size_t j = 0; j < n; ++j) {
			if (j == i || !alive[j] || stray[j] || !Overlap(bi, bb[j])) continue;
			const double g = pcs[i].MinGap(pcs[j], 4.0 * tol);
			const double touch = g <= tol ? 1.0 : 0.0;
			V ov(0.0);
			for (int c = 0; c < 3; ++c) ov[c] = std::max(0.0, std::min(bi.max[c], bb[j].max[c]) - std::max(bi.min[c], bb[j].min[c]));
			const double o = ov.x * ov.y * ov.z;
			if (touch > bestTouch || (touch == bestTouch && (touch > 0.0 ? o > bestOv : g < bestGap))) {
				best = j; bestTouch = touch; bestOv = o; bestGap = g;
			}
		}
		if (best == SIZE_MAX) {
			const V ci = (bb[i].min + bb[i].max) * 0.5;
			double bd = 1e300;
			for (size_t j = 0; j < n; ++j) {
				if (j == i || !alive[j] || stray[j]) continue;
				const double d = m::la::length((bb[j].min + bb[j].max) * 0.5 - ci);
				if (d < bd) { bd = d; best = j; }
			}
		}
		return best;
	};
	                                                 
	std::vector<double> mainVol(n, 0.0);
	{
		std::vector<std::pair<size_t, size_t>> byOwner;
		for (size_t i = 0; i < n; ++i) byOwner.push_back({owner[i], i});
		std::sort(byOwner.begin(), byOwner.end());
		for (size_t a = 0; a < byOwner.size();) {
			size_t b = a, big = byOwner[a].second;
			for (; b < byOwner.size() && byOwner[b].first == byOwner[a].first; ++b)
				if (vol[byOwner[b].second] > vol[big]) big = byOwner[b].second;
			for (size_t q = a; q < b; ++q) {
				const size_t i = byOwner[q].second;
				if (i != big && vol[i] < 0.25 * vol[big]) stray[i] = 1;
			}
			a = b;
		}
	}
	std::vector<std::vector<size_t>> into(n);
	size_t nStray = 0;
	for (size_t i = 0; i < n; ++i)
		if (stray[i]) { const size_t j = neighbour(i); if (j != SIZE_MAX) { into[j].push_back(i); ++nStray; } else stray[i] = 0; }
	if (nStray)
		ProjectDamageParallelFor(0, int64_t(n), [&](int64_t jj) {
			const size_t j = size_t(jj);
			if (into[j].empty()) return;
			std::vector<m::Manifold> ops{pcs[j]};
			for (size_t i : into[j]) ops.push_back(pcs[i]);
			pcs[j] = m::Manifold::BatchBoolean(ops, m::OpType::Add);
			vol[j] = pcs[j].Volume(); bb[j] = pcs[j].BoundingBox();
		});
	size_t count = 0;
	for (size_t i = 0; i < n; ++i) {
		if (stray[i]) alive[i] = 0;
		stray[i] = 0;
		count += alive[i];
	}
	                                                                     
	while (count > std::max<size_t>(1, cap)) {
		size_t i = SIZE_MAX;
		for (size_t k = 0; k < n; ++k) if (alive[k] && (i == SIZE_MAX || vol[k] < vol[i])) i = k;
		alive[i] = 0;
		const size_t j = neighbour(i);
		if (j == SIZE_MAX) { alive[i] = 1; break; }
		pcs[j] = pcs[j] + pcs[i];
		vol[j] = pcs[j].Volume(); bb[j] = pcs[j].BoundingBox();
		--count;
	}
	std::vector<m::Manifold> kept;
	for (size_t i = 0; i < n; ++i) if (alive[i]) kept.push_back(std::move(pcs[i]));
	pcs.swap(kept);
}

}               

                                                                                      
                                                                      
                                                                           
                                                                      
                                                                             
                                                                     
                                                                            
                                                                     
                                                                           
                                                                     
                                                                          
                                                                          
                                    
namespace {
using V2 = m::vec2;

constexpr int kModifierGuidance = 4;

struct GlassHit { V2 c; double r; double strength = 0.0; };

                                                                        
                                                                            
                          
struct GlassWarp {
	double a[4], k[4], c[4], ph[4];
	double operator()(double th, double L) const
	{
		double w = 0.0;
		for (int i = 0; i < 4; ++i) w += a[i] * std::sin(k[i] * th + c[i] * L + ph[i]);
		return w;
	}
};

struct GlassRadial {
	double th0;                                         
	double rhoStart;                        
	V2     start;                                                          
};

double Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }

double SignedArea(const m::SimplePolygon& p)
{
	double s = 0.0;
	for (size_t i = 0; i < p.size(); ++i) s += Cross2(p[i], p[(i + 1) % p.size()]);
	return 0.5 * s;
}

                                                                          
m::SimplePolygon ClipHalf(const m::SimplePolygon& poly, const V2& n, double d)
{
	m::SimplePolygon out;
	for (size_t i = 0; i < poly.size(); ++i) {
		const V2 a = poly[i], b = poly[(i + 1) % poly.size()];
		const double da = m::la::dot(n, a) - d, db = m::la::dot(n, b) - d;
		if (da <= 0) out.push_back(a);
		if ((da < 0) != (db < 0) && std::abs(da - db) > 1e-300) out.push_back(a + (b - a) * (da / (da - db)));
	}
	return out;
}

                                                                              
void GlassNetwork(const GlassHit& H, double Rmax, const CellFractureParams& p, uint64_t seed, uint64_t hitId,
	std::vector<m::SimplePolygon>& cells, std::vector<double>& cellRho)
{
	uint64_t draw = 0;
	auto rnd = [&](uint64_t ch) { return Rnd(seed ^ (hitId * 0x632be59bd9b4e019ULL), draw, ch); };
	const double r = std::max(1e-6, H.r);
	const int spokes = std::max(3, std::min(64, p.glassSpokes));
	const double growth = std::max(1.15, p.glassRingGrowth);
	const double wander = std::max(0.0, std::min(2.0, p.glassWander));
	const double branch = std::max(0.0, std::min(1.0, p.glassBranch));
	const bool modifier = p.strokeMode == kModifierGuidance;
	const double variation = p.sizeVariation < 0.0 ? kMat[1].var : std::clamp(p.sizeVariation, 0.0, 1.0);
	const double density = std::max(1.0, p.proxDensity);
	const double falloff = std::max(0.25, p.proxFalloff);
	const double rubble = std::clamp(p.rubble, 0.0, 1.0);
	auto concentration = [&](double radius) {
		const double d = radius / r;
		return 1.0 + H.strength * ((density - 1.0) * std::exp(-std::pow(d, falloff))
			+ 4.0 * rubble * std::exp(-d * d * 12.0));
	};
	const double R0 = 0.06 * r / std::sqrt(concentration(0.0));
	const double rough = modifier ? std::clamp(p.roughness, 0.0, 1.5) : 0.0;
	const double wave = std::max(0.05, p.roughScale) * 0.28 * r;
	const double phaseX = 6.2831853 * rnd(53), phaseY = 6.2831853 * rnd(54);
	auto relief = [&](const V2& v) {
		if (rough == 0.0) return v;
		const V2 x = (v - H.c) / wave;
		const double amount = rough * 0.10 * wave;
		return v + V2(std::sin(x.y * 2.0 + phaseX) + 0.24 * std::sin(x.x * 3.0 + x.y * 4.0),
			std::sin(x.x * 2.0 + phaseY) + 0.24 * std::sin(x.y * 3.0 - x.x * 4.0)) * amount;
	};
	                                                                      
	                                                         
	std::vector<double> grid{0.0};
	const double detail = std::clamp(p.detail, 0.1, 4.0);
	const double step = rough > 0.0 ? std::min(0.075, 0.045 / detail) : 0.045;
	for (double g = R0 * 0.5; g < Rmax * 1.02; g *= 1.0 + step) grid.push_back(g);
	grid.push_back(Rmax * 1.05);
	auto snap = [&](double x) {
		size_t best = 1;
		for (size_t i = 1; i < grid.size(); ++i) if (std::abs(grid[i] - x) < std::abs(grid[best] - x)) best = i;
		return best;
	};
	GlassWarp W;
	{
		const double ks[4] = {2.0, 5.0, 11.0, 23.0};
		const double amp[4] = {0.10, 0.045, 0.018, 0.008};                        
		for (int i = 0; i < 4; ++i) {
			W.k[i] = ks[i];
			W.a[i] = amp[i] * std::min(1.0, wander);                                                   
			W.c[i] = (0.6 + 1.6 * rnd(40 + i)) * (rnd(50 + i) < 0.5 ? -1.0 : 1.0) * std::max(1.0, wander);                 
			W.ph[i] = 6.2831853 * rnd(60 + i);
		}
	}
	auto L = [&](double rho) { return std::log(std::max(rho, R0) / R0); };
	auto dir = [](double a) { return V2(std::cos(a), std::sin(a)); };
	std::vector<GlassRadial> rad;
	auto point = [&](size_t k, double rho) {
		const GlassRadial& g = rad[k];
		if (rho <= g.rhoStart * 1.0000001) return g.start;
		return H.c + dir(g.th0 + W(g.th0, L(rho))) * rho;
	};
	auto angleAt = [&](size_t k, double rho) {
		const GlassRadial& g = rad[k];
		return g.th0 + W(g.th0, L(rho));
	};
	                                  
	const double gap0 = 6.2831853 / spokes;
	const double phase = 6.2831853 * rnd(1);
	for (int s = 0; s < spokes; ++s, ++draw)
		rad.push_back({phase + gap0 * (s + (modifier ? 0.08 + 0.97 * variation : 0.42) * (rnd(2) - 0.5)), 0.0, H.c});
	std::sort(rad.begin(), rad.end(), [](const GlassRadial& a, const GlassRadial& b) { return a.th0 < b.th0; });

	struct Sector { size_t a, b; std::vector<V2> inner; size_t ia, ib; };                                         
	std::vector<Sector> sectors;
	for (size_t s = 0; s < rad.size(); ++s) sectors.push_back({s, (s + 1) % rad.size(), {H.c}, 0, 0});
	                                                                           
	std::vector<double> rings;
	                                                             
	for (double R = 0.16 * r / std::sqrt(concentration(0.0)); R < Rmax;) {
		rings.push_back(R);
		const double step = (growth - 1.0) * (R < 0.8 * r ? 0.5 : 1.0);
		R *= modifier ? 1.0 + std::max(0.065, step / std::sqrt(concentration(R))) : 1.0 + step;
	}
	rings.push_back(Rmax * 1.05);
	auto unwrap = [](double a, double ref) { while (a < ref) a += 6.2831853; while (a >= ref + 6.2831853) a -= 6.2831853; return a; };
	auto polar = [&](const V2& q, double& ang, double& rr) { const V2 d = q - H.c; rr = m::la::length(d); ang = std::atan2(d.y, d.x); };

	for (size_t j = 0; j < rings.size(); ++j) {
		const bool last = j + 1 == rings.size();
		const double R = rings[j];
		const double t = R / r;                  
		const double pRing = last ? 1.0 : (t < 1.6 ? 0.92 - 0.1 * t : std::max(0.12, 0.6 / t));
		const double pFork = t < 0.8 ? 0.15 * branch : (t < 2.2 ? branch : 0.6 * branch);
		std::vector<Sector> next;
		for (Sector& S : sectors) {
			++draw;
			if (!last && rnd(3) > pRing) { next.push_back(S); continue; }
			                                                                     
			size_t ia = snap(R * (0.82 + 0.36 * rnd(4))), ib = snap(R * (0.82 + 0.36 * rnd(5)));
			if (last) { ia = ib = grid.size() - 1; }
			ia = std::max(ia, S.ia + 2); ib = std::max(ib, S.ib + 2);
			ia = std::min(ia, grid.size() - 1); ib = std::min(ib, grid.size() - 1);
			if (ia <= S.ia || ib <= S.ib) { next.push_back(S); continue; }
			const V2 pa = point(S.a, grid[ia]), pb = point(S.b, grid[ib]);
			                                                                 
			                                                                     
			double aa, ra, ab, rb;
			polar(pa, aa, ra); polar(pb, ab, rb);
			ab = unwrap(ab, aa);                                               
			const double span = ab - aa;
			if (span > 0.9 * 6.2831853) { next.push_back(S); continue; }                                 
			const double flat = last ? 0.0 : (0.5 + 0.5 * rnd(6)) * std::max(0.0, std::min(1.0, 1.6 - span));
			const double bow = last ? 0.0 : 0.05 * (rnd(30) - 0.45);
			const double kinkU = 0.25 + 0.5 * rnd(31), kink = last ? 0.0 : 0.05 * (rnd(32) - 0.5);
			const int nIn = last ? 12 : rough > 0.0 ? 2 + 2 * int(std::ceil(2.0 * detail)) : 4;
			std::vector<V2> arc{pa};
			for (int q = 1; q <= nIn; ++q) {
				const double u = double(q) / (nIn + 1);
				const V2 circ = H.c + dir(aa + span * u) * (ra + (rb - ra) * u);
				const V2 base = circ * (1.0 - flat) + (pa + (pb - pa) * u) * flat;
				const double tent = u < kinkU ? u / kinkU : (1.0 - u) / (1.0 - kinkU);
				const double sc = 1.0 + bow * std::sin(3.14159265 * u) + kink * tent
					+ (last ? 0.0 : 0.012 * (rnd(7 + q) - 0.5));
				arc.push_back(H.c + (base - H.c) * sc);
			}
			arc.push_back(pb);
			                                                                 
			m::SimplePolygon poly;
			for (size_t g = S.ia; g <= ia; ++g) poly.push_back(g == S.ia ? S.inner.front() : point(S.a, grid[g]));
			for (size_t q = 1; q + 1 < arc.size(); ++q) poly.push_back(arc[q]);
			for (size_t g = ib + 1; g-- > S.ib;) poly.push_back(g == S.ib ? S.inner.back() : point(S.b, grid[g]));
			for (size_t q = S.inner.size(); q-- > 0;) {
				if (q == S.inner.size() - 1 || q == 0) continue;                     
				poly.push_back(S.inner[q]);
			}
			m::SimplePolygon clean;
			for (const V2& v : poly)
				if (clean.empty() || m::la::length(v - clean.back()) > 1e-9 * r) clean.push_back(v);
			while (clean.size() > 2 && m::la::length(clean.front() - clean.back()) <= 1e-9 * r) clean.pop_back();
			if (clean.size() >= 3) {
				if (SignedArea(clean) < 0) std::reverse(clean.begin(), clean.end());
				for (V2& v : clean) v = relief(v);
				cells.push_back(std::move(clean));
				cellRho.push_back(0.5 * (grid[S.ia] + grid[S.ib] + grid[ia] + grid[ib]) * 0.5);
			}
			if (last) continue;
			                                                                       
			const double width = std::abs(ab - aa) * R;
			if (rnd(20) < pFork && width > 0.35 * R * gap0 && arc.size() >= 5) {
				const size_t mid = arc.size() / 2;
				double am, rm;
				polar(arc[mid], am, rm);
				                                                              
				                                                        
				                                        
				double lo0 = rad[S.a].th0, hi0 = rad[S.b].th0;
				while (hi0 <= lo0) hi0 += 6.2831853;
				const double Lm = L(rm);
				const double amU = unwrap(am, lo0 + W(lo0, Lm));
				for (int it = 0; it < 60; ++it) {
					const double mth = 0.5 * (lo0 + hi0);
					if (mth + W(mth, Lm) < amU) lo0 = mth; else hi0 = mth;
				}
				rad.push_back({0.5 * (lo0 + hi0), rm, arc[mid]});
				const size_t c = rad.size() - 1;
				size_t ic = 0;
				while (ic < grid.size() && grid[ic] <= rm) ++ic;                                      
				std::vector<V2> left(arc.begin(), arc.begin() + mid + 1), right(arc.begin() + mid, arc.end());
				                                                                        
				next.push_back({S.a, c, left, ia, ic - 1});
				next.push_back({c, S.b, right, ic - 1, ib});
			} else next.push_back({S.a, S.b, arc, ia, ib});
		}
		sectors.swap(next);
	}
}
}               

                                                                            
                                                                      
                                                                  
static const GlassHit& NearestGlassHit(const std::vector<GlassHit>& hits, const V2& point)
{
	return *std::min_element(hits.begin(), hits.end(), [&](const GlassHit& a, const GlassHit& b) {
		return m::la::length(a.c - point) < m::la::length(b.c - point);
	});
}

static bool SplitGlassShard(const m::CrossSection& shard, const GlassHit& hit,
	const CellFractureParams& p, bool preferRing, m::CrossSection& a, m::CrossSection& b)
{
	const auto bounds = shard.Bounds();
	const V2 centre = (bounds.min + bounds.max) * 0.5;
	const double size = m::la::length(bounds.max - bounds.min);
	const double area = shard.Area();
	if (!(size > 1e-9 && area > 0.0)) return false;
	const V2 d = centre - hit.c;
	const double radius = m::la::length(d);
	const V2 across = radius > size * 0.01 ? V2(-d.y, d.x) / radius : V2(1.0, 0.0);
	const m::SimplePolygon box{centre + V2(-size,-size), centre + V2(size,-size),
		centre + V2(size,size), centre + V2(-size,size)};
	for (bool ring : {preferRing, !preferRing}) {
		if (ring && radius <= size * 0.01) continue;
		const m::CrossSection cutter = ring
			? m::CrossSection::Circle(radius, std::clamp(int(radius / size * 80 * std::max(0.5, p.detail)), 48, 512)).Translate(hit.c)
			: m::CrossSection(ClipHalf(box, across, m::la::dot(across, centre)));
		a = shard ^ cutter;
		b = shard - cutter;
		if (a.Area() > 0.045 * area && b.Area() > 0.045 * area) return true;
	}
	return false;
}

static void RefineGlassShards(std::vector<m::CrossSection>& shards, std::vector<char>& skip,
	const std::vector<GlassHit>& hits, const CellFractureParams& p,
	const std::vector<std::pair<V2,V2>>& guides, size_t count)
{
	const bool guided = !guides.empty();
	const double radius = std::max(1e-6, p.guidanceRadius);
	const double falloff = std::max(0.25, p.guidanceFalloff);
	auto score = [&](const m::CrossSection& shard) {
		if (!guided) return shard.Area();
		const auto bounds = shard.Bounds();
		const V2 centre = (bounds.min + bounds.max) * 0.5;
		const double reach = 0.35 * m::la::length(bounds.max - bounds.min);
		double distance = 1e300;
		for (const auto& guide : guides) {
			const V2 delta = guide.second - guide.first;
			const double squared = m::la::dot(delta, delta);
			const double t = squared > 0.0
				? std::clamp(m::la::dot(centre - guide.first, delta) / squared, 0.0, 1.0) : 0.0;
			distance = std::min(distance, m::la::length(centre - (guide.first + delta * t)));
			if (distance <= reach) break;
		}
		if (distance > radius + reach) return 0.0;
		return shard.Area() * std::exp(-std::max(0.0, distance - reach) / radius * falloff);
	};
	std::vector<double> weights;
	for (size_t i = 0; i < shards.size(); ++i) weights.push_back(skip[i] ? 0.0 : score(shards[i]));
	for (size_t k = 0; k < count;) {
		const auto best = std::max_element(weights.begin(), weights.end());
		if (best == weights.end() || *best <= 0.0) break;
		const size_t i = size_t(best - weights.begin());
		const auto bounds = shards[i].Bounds();
		const GlassHit& hit = NearestGlassHit(hits, (bounds.min + bounds.max) * 0.5);
		m::CrossSection a, b;
		if (!SplitGlassShard(shards[i], hit, p, k % 2 == 0, a, b)) {
			weights[i] = 0.0;
			continue;
		}
		shards[i] = std::move(a);
		shards.push_back(std::move(b));
		skip.push_back(0);
		weights[i] = score(shards[i]);
		weights.push_back(score(shards.back()));
		++k;
	}
}

static bool BuildGlassPane(const MeshView& view, const m::Manifold& source, const CellFractureParams& p,
	uint64_t seed, const V axis[3], const V& centre, const std::vector<Source>& src, size_t nPointSrc,
	const std::vector<std::vector<EVec>>* strokes, const Solid& solid, bool cracks, double volume,
	std::vector<m::Manifold>& pieces, std::vector<CellPieceInfo>& info, std::string* log)
{
	const bool modifier = p.strokeMode == kModifierGuidance;
	const V n = axis[0], t = axis[1], b = axis[2];
	auto to2 = [&](const V& x) { const V d = x - centre; return V2(m::la::dot(d, t), m::la::dot(d, b)); };
	double wlo = 1e300, whi = -1e300;
	V2 lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < view.npts; ++i) {
		const V d = view.pts[i] - centre;
		const double w = m::la::dot(d, n);
		wlo = std::min(wlo, w); whi = std::max(whi, w);
		const V2 q = to2(view.pts[i]);
		lo = V2(std::min(lo.x, q.x), std::min(lo.y, q.y)); hi = V2(std::max(hi.x, q.x), std::max(hi.y, q.y));
	}
	const double size = std::max(hi.x - lo.x, hi.y - lo.y);
	if (!(size > 0.0)) return false;
	                                                                          
	                                    
	std::vector<GlassHit> hits;
	for (size_t i = 0; i < nPointSrc; ++i) {
		if (!modifier) {
			hits.push_back({to2(src[i].a), src[i].r});
			continue;
		}
		V surface, normal;
		if (!solid.Nearest(src[i].a, surface, normal)) continue;
		const V2 centre2 = to2(surface);
		const double radius = std::clamp(src[i].r, 1e-4 * size, 8.0 * size);
		if (std::none_of(hits.begin(), hits.end(), [&](const GlassHit& other) {
			return m::la::length(centre2 - other.c) < 1e-5 * size;
		})) hits.push_back({centre2, radius, std::min(1.0, src[i].s)});
	}
	for (size_t i = nPointSrc; i < src.size(); ++i) {
		const Source& s = src[i];
		const double L = s.segment ? m::la::length(s.b - s.a) : 0.0;
		const int nh = 1 + int(L / (1.6 * s.r));
		for (int k = 0; k < nh; ++k) hits.push_back({to2(s.a + (s.b - s.a) * ((k + 0.5) / nh)), s.r * 0.7});
	}
	if (hits.empty())
		hits.push_back({(lo + hi) * 0.5 + V2(Rnd(seed, 3, 1) - 0.5, Rnd(seed, 3, 2) - 0.5) * (0.3 * size), 0.35 * size});
	                                                                              
	const double big = 4.0 * size + 1.0;
	std::vector<m::Polygons> region(hits.size());
	for (size_t i = 0; i < hits.size(); ++i) {
		m::SimplePolygon sq{hits[i].c + V2(-big, -big), hits[i].c + V2(big, -big), hits[i].c + V2(big, big), hits[i].c + V2(-big, big)};
		for (size_t j = 0; j < hits.size() && sq.size() >= 3; ++j) {
			if (j == i) continue;
			const V2 d = hits[j].c - hits[i].c;
			const double l = m::la::length(d);
			if (l < 1e-9) continue;
			const double limit = size * size * 0.08;
			auto weight = [&](const GlassHit& hit) {
				return modifier ? std::min(limit, 0.25 * hit.r * hit.r * hit.strength) : 4.0 * hit.r * hit.r;
			};
			const double wi = weight(hits[i]), wj = weight(hits[j]);
			                                             
			const V2 nn = d;
			const double off = 0.5 * (m::la::dot(hits[j].c, hits[j].c) - m::la::dot(hits[i].c, hits[i].c) - wj + wi);
			sq = ClipHalf(sq, nn, off);
		}
		if (sq.size() >= 3) region[i].push_back(sq);
	}
	                                                                      
	std::vector<m::CrossSection> leftOf;
	if (strokes && cracks)
		for (const std::vector<EVec>& st : *strokes) {
			if (st.size() < 2) continue;
			std::vector<V2> s2;
			for (const EVec& q : st) {
				const V2 v = to2(q);
				if (s2.empty() || m::la::length(v - s2.back()) > 1e-6 * size) s2.push_back(v);
			}
			if (s2.size() < 2) continue;
			const V2 t0 = m::la::normalize(s2[1] - s2[0]), t1 = m::la::normalize(s2.back() - s2[s2.size() - 2]);
			const double far = 4.0 * size;
			m::SimplePolygon poly;
			poly.push_back(s2.front() - t0 * far);
			for (const V2& v : s2) poly.push_back(v);
			poly.push_back(s2.back() + t1 * far);
			poly.push_back(s2.back() + t1 * far + V2(-t1.y, t1.x) * far);
			poly.push_back(s2.front() - t0 * far + V2(-t0.y, t0.x) * far);
			leftOf.push_back(m::CrossSection(poly, m::CrossSection::FillRule::NonZero));
		}
	                                                                              
	                                               
	const int nBase = std::max(1, std::min(p.pieces, p.maxPieces));
	const bool keepRemainder = modifier && p.keepRemainder && nPointSrc > 0 && nBase > 1;
	const bool given = !src.empty() || (strokes && cracks && !leftOf.empty());
	const size_t target = modifier ? size_t(std::max(1, nBase - int(keepRemainder)))
		: size_t(std::max(1, std::min(std::max(1, p.maxPieces), given
			? int(std::round(nBase * (1.0 + 0.5 * (std::max(1.0, p.proxDensity) - 1.0)))) : nBase)));
	                                                                 
	const m::mat3x4 toPane(V(t.x, b.x, n.x), V(t.y, b.y, n.y), V(t.z, b.z, n.z),
		-V(m::la::dot(t, centre), m::la::dot(b, centre), m::la::dot(n, centre)));
	const m::CrossSection outline = source.Transform(toPane).Project();
	m::CrossSection affected = outline, remainder;
	if (keepRemainder) {
		m::CrossSection near;
		for (const GlassHit& hit : hits) {
			const double reach = std::max(0.1, p.remainderReach) * hit.r;
			near += m::CrossSection::Circle(reach, 96).Translate(hit.c);
		}
		affected = outline ^ near;
		remainder = outline - near;
	}
	                                                                      
	                                                                         
	const double hole = std::max(0.0, std::min(1.0, p.glassHole));
	std::vector<m::CrossSection> shards;
	std::vector<char> fallen;
	CellFractureParams q = p;
	std::vector<m::CrossSection> prevShards;
	std::vector<char> prevFallen;
	for (int pass = 0;; ++pass) {
		prevShards.swap(shards); prevFallen.swap(fallen);
		shards.clear(); fallen.clear();
		for (size_t i = 0; i < hits.size(); ++i) {
			if (region[i].empty()) continue;
			double Rmax = 0.0;
			for (const V2& c : {lo, hi, V2(lo.x, hi.y), V2(hi.x, lo.y)}) Rmax = std::max(Rmax, m::la::length(c - hits[i].c));
			Rmax = Rmax * 1.1 + 1e-6;
			std::vector<m::SimplePolygon> cells;
			std::vector<double> rho;
			GlassNetwork(hits[i], Rmax, q, seed, i + 1, cells, rho);
			const m::CrossSection reg(region[i]);
			for (size_t k = 0; k < cells.size(); ++k) {
				m::CrossSection c = m::CrossSection(cells[k], m::CrossSection::FillRule::NonZero) ^ reg ^ affected;
				if (c.IsEmpty() || c.Area() < 1e-9 * size * size) continue;
				                                                                
				const bool out = rho[k] < hole * hits[i].r * (0.75 + 0.5 * Rnd(seed, 900 + i * 7919 + k, 3));
				shards.push_back(std::move(c));
				fallen.push_back(out ? 1 : 0);
			}
		}
		for (const m::CrossSection& L : leftOf) {
			std::vector<m::CrossSection> s2;
			std::vector<char> f2;
			for (size_t k = 0; k < shards.size(); ++k) {
				for (const m::CrossSection& part : {shards[k] ^ L, shards[k] - L})
					if (!part.IsEmpty()) { s2.push_back(part); f2.push_back(fallen[k]); }
			}
			shards.swap(s2); fallen.swap(f2);
		}
		size_t kept = 0;
		for (char fl : fallen) kept += fl ? 0 : 1;
		if (pass > 0 && kept < target) {                                                      
			shards.swap(prevShards); fallen.swap(prevFallen);
			break;
		}
		if (pass == 8 || double(kept) <= 1.3 * double(target) + 1.0 || (q.glassSpokes <= 3 && q.glassRingGrowth >= 40.0))
			break;
		const double f = std::sqrt(double(target) / double(kept));
		q.glassSpokes = std::max(3, int(std::round(q.glassSpokes * f)));
		q.glassRingGrowth = std::min(40.0, std::pow(std::max(1.15, q.glassRingGrowth), 1.0 / f));
		q.glassBranch *= f;
	}
	                                                                        
	                                                                    
	{
		const double tol2 = 1e-6 * size;
		auto perim = [](const m::CrossSection& c) {
			double l = 0.0;
			for (const m::SimplePolygon& poly : c.ToPolygons())
				for (size_t i = 0; i < poly.size(); ++i) l += m::la::length(poly[(i + 1) % poly.size()] - poly[i]);
			return l;
		};
		const size_t n = shards.size();
		std::vector<double> area(n), per(n), priority(n);
		std::vector<m::Rect> rb(n);
		auto densityWeight = [&](const m::Rect& bounds) {
			if (!modifier) return 1.0;
			const V2 c = (bounds.min + bounds.max) * 0.5;
			const GlassHit& hit = NearestGlassHit(hits, c);
			const double d = m::la::length(c - hit.c) / hit.r;
			return 1.0 + hit.strength * (std::max(1.0, p.proxDensity) - 1.0)
				* std::exp(-std::pow(d, std::max(0.25, p.proxFalloff)));
		};
		size_t kept = 0;
		for (size_t i = 0; i < n; ++i) {
			area[i] = shards[i].Area(); per[i] = perim(shards[i]); rb[i] = shards[i].Bounds();
			priority[i] = area[i] * densityWeight(rb[i]);
			kept += fallen[i] ? 0 : 1;
		}
		auto near = [&](const m::Rect& a, const m::Rect& c) {
			return a.min.x <= c.max.x + tol2 && c.min.x <= a.max.x + tol2 && a.min.y <= c.max.y + tol2 && c.min.y <= a.max.y + tol2;
		};
		std::vector<char> gone(n, 0);
		while (kept > target) {
			size_t i = SIZE_MAX;
			for (size_t k = 0; k < n; ++k) if (!fallen[k] && !gone[k] && (i == SIZE_MAX || priority[k] < priority[i])) i = k;
			size_t best = SIZE_MAX;
			double bestShared = tol2;
			m::CrossSection bestU;
			for (size_t j = 0; j < n; ++j) {
				if (j == i || fallen[j] || gone[j] || !near(rb[i], rb[j])) continue;
				m::CrossSection U = shards[i] + shards[j];
				const double shared = 0.5 * (per[i] + per[j] - perim(U));
				if (shared > bestShared) { bestShared = shared; best = j; bestU = std::move(U); }
			}
			if (best == SIZE_MAX) {                                        
				const V2 ci = (rb[i].min + rb[i].max) * 0.5;
				double bd = 1e300;
				for (size_t j = 0; j < n; ++j) {
					if (j == i || fallen[j] || gone[j]) continue;
					const double d = m::la::length((rb[j].min + rb[j].max) * 0.5 - ci);
					if (d < bd) { bd = d; best = j; }
				}
				if (best == SIZE_MAX) break;
				bestU = shards[i] + shards[best];
			}
			shards[best] = std::move(bestU);
			area[best] = shards[best].Area(); per[best] = perim(shards[best]); rb[best] = shards[best].Bounds();
			priority[best] = area[best] * densityWeight(rb[best]);
			gone[i] = 1;
			--kept;
		}
		std::vector<m::CrossSection> s2;
		std::vector<char> f2;
		for (size_t i = 0; i < n; ++i)
			if (!gone[i]) { s2.push_back(std::move(shards[i])); f2.push_back(fallen[i]); }
		shards.swap(s2); fallen.swap(f2);
	}
	                                                         
	const size_t kept = std::count(fallen.begin(), fallen.end(), char(0));
	if (modifier && kept < target) RefineGlassShards(shards, fallen, hits, p, {}, target - kept);

	size_t guideCount = 0;
	if (modifier && p.guidanceAmount > 0 && strokes && !strokes->empty()) {
		std::vector<std::pair<V2,V2>> guides;
		const double snap = std::max(1e-6, p.guidanceDistance);
		const double step = std::max(size / 512.0, p.guidanceRadius * 0.25);
		for (const auto& path : *strokes) {
			for (size_t i = 1; i < path.size(); ++i) {
				const V d = path[i] - path[i - 1];
				const int steps = std::clamp(int(std::ceil(m::la::length(d) / step)), 1, 256);
				V2 previous(0.0);
				bool previousValid = false;
				for (int j = 0; j <= steps; ++j) {
					const V point = path[i - 1] + d * (double(j) / steps);
					V projected, normal;
					const bool valid = solid.Nearest(point, projected, normal)
						&& m::la::length(projected - point) <= snap;
					if (valid) {
						const V2 q2 = to2(projected);
						guides.emplace_back(previousValid ? previous : q2, q2);
						previous = q2;
					}
					previousValid = valid;
				}
			}
		}
		if (!guides.empty()) {
			const size_t before = shards.size();
			const size_t budget = size_t(std::max(0, p.maxPieces - nBase));
			RefineGlassShards(shards, fallen, hits, p, guides,
				std::min(budget, size_t(p.guidanceAmount)));
			guideCount = shards.size() - before;
		}
	}
	if (!remainder.IsEmpty()) {
		shards.push_back(remainder);
		fallen.push_back(0);
	}

	const double gap = std::max(0.0, p.gap);
	const double pad = 0.05 * (whi - wlo) + 1e-4 * size;
	m::mat3x4 frame(t, b, n, centre + n * (wlo - pad));
	const double dust = 1e-9 * volume;
	std::vector<std::vector<m::Manifold>> out(shards.size());
	ProjectDamageParallelFor(0, int64_t(shards.size()), [&](int64_t ii) {
		const size_t k = size_t(ii);
		if (fallen[k]) return;
		m::CrossSection c = gap > 0.0 ? shards[k].Offset(-0.5 * gap, m::CrossSection::JoinType::Miter) : shards[k];
		if (c.IsEmpty()) return;
		const m::Manifold prism = m::Manifold::Extrude(c.ToPolygons(), (whi - wlo) + 2.0 * pad).Transform(frame);
		m::Manifold part = source ^ prism;
		for (m::Manifold& pc : part.Decompose())
			if (pc.Volume() > dust && pc.Status() == m::Manifold::Error::NoError) out[k].push_back(std::move(pc));
	});
	size_t nFallen = 0;
	for (char fl : fallen) nFallen += fl ? 1 : 0;
	std::vector<m::Manifold> all;
	std::vector<size_t> owner;
	for (size_t k = 0; k < out.size(); ++k)
		for (m::Manifold& pc : out[k]) { all.push_back(std::move(pc)); owner.push_back(k); }
	MergePieces(all, owner, modifier ? size_t(nBase) + guideCount : target, gap + 1e-4 * size);
	for (m::Manifold& pc : all) {
		double vol = 0.0;
		const V c = MassCentre(pc, vol);
		info.push_back({vol, c});
		pieces.push_back(std::move(pc));
	}
	if (log)
		*log = "cell fracture (glass pane): " + std::to_string(hits.size()) + " hits, " + std::to_string(pieces.size())
			+ (modifier ? " shards (" + std::to_string(guideCount) + " guided), "
				: " shards (target " + std::to_string(target) + "), ") + std::to_string(nFallen) + " fallen out of the hole";
	return !pieces.empty();
}

static void BuildCellFractureImpl(const CoreInput& in, const m::Manifold& source, uint32_t sourceId,
	const CellFractureParams& pIn, Int32 mainSeed,
	const std::vector<std::vector<EVec>>* strokes,
	std::vector<m::Manifold>& pieces, std::vector<CellPieceInfo>& info, std::string* log)
{
	(void)sourceId;
	if (!pIn.enable || source.IsEmpty() || in.view.npolys <= 0) return;
	const CellFractureParams& p = pIn;
	const int mat = std::max(0, std::min(4, p.material));
	const Material& M = kMat[mat];
	const bool glass = mat == 1, wood = mat == 0;
	const bool modifier = p.strokeMode == kModifierGuidance;
    const bool guidance = (modifier || p.strokeMode == 3) && p.guidanceAmount > 0 && strokes && !strokes->empty();
	const double var = p.sizeVariation >= 0.0 ? std::min(1.0, p.sizeVariation) : M.var;
	const double rubble = p.rubble >= 0.0 ? std::min(1.0, p.rubble) : M.rubble;
	const double rough = std::max(0.0, std::min(1.5, p.roughness >= 0.0 ? p.roughness : M.rough));
	const int nBase = std::max(1, std::min(p.pieces, p.maxPieces));
	const size_t cap = size_t(std::max(2, p.maxPieces));
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x43454c4c46524331ULL)                       
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);

	Solid S;
	S.view = &in.view;
	S.bvh.Build(in.view);
	const m::Box bb = source.BoundingBox();
	S.lo = bb.min; S.hi = bb.max;
	const double volume = source.Volume();
	if (!(volume > 0.0)) return;
	const double h = std::cbrt(volume / nBase);
	const V ext = S.hi - S.lo;

	                                                                                    
	V axis[3]; double pext[3]; V pc;
	Principal(in.view, axis, pext, pc);
	const bool planar = p.planar == 2 || (p.planar == 0 && M.planar && pext[0] < std::max(0.01, p.planarRatio) * pext[1]);

	                                                                                       
	const double R0 = std::max(1e-6, p.proxRadius);
	std::vector<Source> src;
	for (const CellProximity& c : p.proximity)
		if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.strength > 0.0)
			src.push_back({V(c.x, c.y, c.z), V(c.x, c.y, c.z), c.radius > 0.0 ? c.radius : R0, c.strength, false});
	const size_t nPointSrc = src.size();
	const bool shatter = p.strokeMode == 1 || p.strokeMode == 2, cracks = p.strokeMode == 0 || p.strokeMode == 2;
	if (strokes && shatter)
		for (const std::vector<EVec>& st : *strokes) {
			if (st.size() == 1) src.push_back({st[0], st[0], R0, 1.0, false});
			for (size_t i = 0; i + 1 < st.size(); ++i) src.push_back({st[i], st[i + 1], R0, 1.0, true});
		}
	const double dens = std::max(1.0, p.proxDensity);
	const double fall = std::max(0.25, p.proxFalloff);
	auto influence = [&](const V& x) {
		double best = 0.0;
		for (const Source& s : src) {
			const double d = (s.segment ? SegDist(x, s.a, s.b) : m::la::length(x - s.a)) / std::max(1e-9, s.r);
			best = std::max(best, std::min(1.0, s.s) * std::exp(-std::pow(d, fall)));
		}
		return best;
	};
	auto rho = [&](const V& x) { return 1.0 + (dens - 1.0) * influence(x); };
	const double rhoMax = src.empty() ? 1.0 : dens;
	auto isFar = [&](const V& x) {
		for (const Source& s : src) {
			const double d = s.segment ? SegDist(x, s.a, s.b) : m::la::length(x - s.a);
			if (d < std::max(0.1, p.remainderReach) * s.r) return false;
		}
		return true;
	};

	                                                                                       
	if (glass && (modifier || (planar && !guidance))) {
		const bool built = BuildGlassPane(in.view, source, p, seed, axis, pc, src, nPointSrc, strokes, S, cracks, volume, pieces, info, log);
		if (modifier || built) return;
	}

	                                                                                 
	std::vector<V> seeds;
	std::vector<double> wts;
	const double wPrim = M.prim * h;
	uint64_t draw = 0;
	auto rnd = [&](uint64_t ch) { return Rnd(seed, draw, ch); };
	auto farEnough = [&](const V& x, double minD) {
		for (const V& s : seeds) if (m::la::length(s - x) < minD) return false;
		return true;
	};
	                                                                        
	                                                                 
	const V nP = axis[0];
	auto flat = [&](const V& x) { return x - nP * m::la::dot(x - pc, nP); };
	const bool thin = planar || pext[0] < 0.2 * pext[1];
	auto around = [&](const V& a, const V& b2, double R, uint64_t c0) {
		if (!thin) return a + (b2 - a) * rnd(c0) + V(rnd(c0 + 1) - 0.5, rnd(c0 + 2) - 0.5, rnd(c0 + 3) - 0.5) * (2.0 * R);
		return flat(a + (b2 - a) * rnd(c0)) + axis[1] * ((rnd(c0 + 1) - 0.5) * 2.0 * R)
			+ axis[2] * ((rnd(c0 + 2) - 0.5) * 2.0 * R) + nP * ((rnd(c0 + 3) - 0.5) * pext[0]);
	};
	                                                                    
	const size_t baseCap = modifier && guidance ? size_t(nBase) : cap;
	const int primaries = std::max(1, int(std::round(nBase * (1.0 - (mat == 4 ? 0.75 : 0.5) * var))));
	for (int tries = 0; tries < primaries * 60 && int(seeds.size()) < primaries && seeds.size() < baseCap; ++tries, ++draw) {
		const V x = S.lo + V(ext.x * rnd(0), ext.y * rnd(1), ext.z * rnd(2));
		const double r = rho(x);
		if (rnd(3) * rhoMax > r) continue;
		if (!farEnough(x, 0.55 * h * std::cbrt(1.0 / r))) continue;
		if (!S.Inside(x)) continue;
		seeds.push_back(x);
		wts.push_back(wPrim * wPrim * (0.5 + rnd(17)));
	}
	                                                                           
	const size_t nPrim = seeds.size();
	for (int k = 0, tries = 0; k < nBase - primaries && nPrim > 0 && tries < 40 * nBase && seeds.size() < baseCap; ++tries, ++draw) {
		const V& c = seeds[size_t(rnd(4) * double(nPrim)) % nPrim];
		const double sd = M.cluster * (thin ? std::sqrt(volume / std::max(1e-12, pext[0]) / nBase) : h);
		const V x = around(c, c, sd, 5);
		if (m::la::length(thin ? flat(x) - flat(c) : x - c) < 0.5 * sd || !S.Inside(x)) continue;
		seeds.push_back(x);
		wts.push_back(0.0);
		++k;
	}
	                                                                          
	                                                                          
	for (size_t si = 0; si < src.size() && !(glass && si < nPointSrc); ++si) {
		const Source& s = src[si];
		const double R = 1.3 * s.r;
		const double len = s.segment ? m::la::length(s.b - s.a) : 0.0;
		                                                         
		int inCount = 0;
		for (int k = 0; k < 48; ++k, ++draw) {
			if (S.Inside(around(s.a, s.b, R, 20))) ++inCount;
		}
		const double vReach = (thin ? (3.14159 * R * R + 2.0 * R * len) * pext[0] : 4.18879 * R * R * R + 3.14159 * R * R * len)
			* 0.45 * (inCount / 48.0);
		const int want = std::min(int(baseCap), int(std::round((dens - 1.0) * std::min(1.0, s.s) * vReach / (h * h * h))));
		for (int k = 0, tries = 0; k < want && tries < want * 40 && seeds.size() < baseCap; ++tries, ++draw) {
			const V x = around(s.a, s.b, R, 13);
			const V xa = thin ? flat(x) : x, sa = thin ? flat(s.a) : s.a, sb = thin ? flat(s.b) : s.b;
			const double d = (s.segment ? SegDist(xa, sa, sb) : m::la::length(xa - sa)) / s.r;
			if (d > 1.3 || rnd(24) > std::exp(-std::pow(d, fall)) || !S.Inside(x)) continue;
			if (!farEnough(x, 0.45 * h / std::cbrt(rho(x)))) continue;
			seeds.push_back(x);
			wts.push_back(0.0);
			++k;
		}
	}
	                                                                  
	for (size_t si = 0; si < nPointSrc && !glass; ++si) {
		const Source& s = src[si];
		const int nr = int(std::round(rubble * nBase * 0.5 * std::min(1.0, s.s)));
		for (int k = 0, tries = 0; k < nr && tries < nr * 40 && seeds.size() < baseCap; ++tries, ++draw) {
			const V x = around(s.a, s.a, 0.5 * s.r, 8);
			if (m::la::length(thin ? flat(x) - flat(s.a) : x - s.a) > 0.5 * s.r || !S.Inside(x)) continue;
			if (!farEnough(x, 0.1 * h)) continue;
			seeds.push_back(x);
			wts.push_back(0.0);
			++k;
		}
	}
                                                                         
                                                                             
                                                                      
    if (guidance) {
        struct GuideFrame { V q, n, tangent; double length, depth; };
        std::vector<GuideFrame> frames;
        double total=0.0;
        const double radius=std::max(1e-5,p.guidanceRadius);
        const double snap=std::max(1e-6,p.guidanceDistance);
        const double eps=std::max(1e-8,m::la::length(ext)*1e-7);
        for (const auto& path:*strokes) for(size_t i=1;i<path.size();++i) {
            const V a=path[i-1], b=path[i]; const double length=m::la::length(b-a);
            if(length<eps)continue;
            const int samples=std::min(256,std::max(1,int(std::ceil(length/std::max(radius*.5,eps*10)))));
            for(int j=0;j<samples;++j) {
                V q,n; const V at=a+(b-a)*((j+.5)/samples);
                if(!S.Nearest(at,q,n)||m::la::length(q-at)>snap)continue;
                const double thickness=S.Thickness(q,n);
                const double depth=p.guidanceDepth>0?std::min(thickness,p.guidanceDepth):thickness;
                if(depth<eps*2)continue;
                V tangent=Unit((b-a)-n*m::la::dot(b-a,n),axis[1]);
                total+=length/samples;frames.push_back({q,n,tangent,total,depth});
            }
        }
        const size_t start=seeds.size();
        const size_t want=std::min<size_t>(2000,std::max(0,p.guidanceAmount));
                                                                             
                                                                    
        for(size_t tries=0;!frames.empty()&&seeds.size()-start<want&&seeds.size()<cap&&tries<want*100;++tries,++draw) {
            double pick=rnd(81)*total;
            auto it=std::lower_bound(frames.begin(),frames.end(),pick,[](const GuideFrame&f,double x){return f.length<x;});
            const auto& f=it==frames.end()?frames.back():*it;
            const V side=Unit(m::la::cross(f.n,f.tangent),axis[2]);
            const double r=radius*std::pow(rnd(82),std::max(.25,p.guidanceFalloff));
            const double angle=6.283185307179586*rnd(83);
            V x=f.q+(side*std::cos(angle)+f.tangent*std::sin(angle))*r-f.n*(eps+rnd(84)*std::max(0.,f.depth-2*eps));
            if(planar)x=flat(x);
            if(!S.Inside(x)||!farEnough(x,std::max(eps*10,radius/(10*std::sqrt(double(want))))))continue;
            seeds.push_back(x);
            wts.push_back(modifier ? wPrim * wPrim * (0.65 + 0.35 * rnd(85)) : 0.0);
        }
    }
	                                                                         
	if (planar)
		for (V& x : seeds) x = S.MidSurface(x);
	                                                                         
	if (glass)
		for (size_t si = 0; si < nPointSrc; ++si) {
			const Source& s = src[si];
			V q, n;
			if (!S.Nearest(s.a, q, n)) continue;
			const double T = S.Thickness(q, n);
			const V mid = q - n * (0.5 * std::max(T, 1e-6));
			const V t = Unit(m::la::cross(std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(0, 1, 0), n), V(1, 0, 0));
			const V b = m::la::cross(n, t);
			const int spokes = std::max(3, std::min(64, p.glassSpokes));
			const double growth = std::max(1.1, p.glassRingGrowth);
			const double phase = 6.2831853 * Rnd(seed, 99991 + si, 1);
			double r = 0.1 * s.r;
			for (int ring = 0; r < 1.5 * s.r && seeds.size() < cap; ++ring, r *= growth)
				for (int k = 0; k < spokes && seeds.size() < cap; ++k, ++draw) {
					const double a = phase + 6.2831853 * (k + 0.5 * (ring & 1) + 0.35 * (rnd(11) - 0.5)) / spokes;
					V x = mid + (t * std::cos(a) + b * std::sin(a)) * (r * (0.85 + 0.3 * rnd(12)));
					if (S.Inside(x)) { seeds.push_back(planar ? S.MidSurface(x) : x); wts.push_back(0.0); }
				}
		}
	                                                                         
	if (strokes && cracks) {
		const double sp = p.strokeSpacing > 0.0 ? p.strokeSpacing : 0.6 * h;
		struct CutFrame { V q, n, tan, side; double T; };
		std::vector<CutFrame> frames;
		std::vector<V> cutSeeds;
		for (const std::vector<EVec>& st : *strokes) {
			if (st.size() < 2) continue;
			double carry = 0.0;
			for (size_t i = 0; i + 1 < st.size() && seeds.size() + cutSeeds.size() < cap; ++i) {
				const V a = st[i], b2 = st[i + 1];
				const double L = m::la::length(b2 - a);
				if (L < 1e-9) continue;
				const V tan = (b2 - a) / L;
				double at = carry;
				for (; at < L && seeds.size() + cutSeeds.size() < cap; at += sp, ++draw) {
					V q, n;
					if (!S.Nearest(a + tan * at, q, n)) continue;
					const V side = Unit(m::la::cross(n, tan), V(1, 0, 0));
					const double T = S.Thickness(q, n);
					frames.push_back({q, n, tan, side, T});
					const int layers = planar ? 1 : std::max(1, int(std::ceil(T / sp)));
					for (int l = 0; l < layers && seeds.size() + cutSeeds.size() + 2 <= cap; ++l) {
						const V c = q - n * (planar ? 0.5 * T : std::min(T, (l + 0.5) * sp));
						for (int sgn = -1; sgn <= 1; sgn += 2) {
							const V x = c + side * (sgn * 0.5 * sp);
							if (S.Inside(x)) cutSeeds.push_back(x);
						}
					}
				}
				carry = at - L;
			}
		}
		                                                                 
		std::vector<V> kept;
		std::vector<double> keptW;
		for (size_t i = 0; i < seeds.size(); ++i) {
			const V& x = seeds[i];
			bool onCut = false;
			for (const CutFrame& f : frames) {
				const V d = x - f.q;
				if (std::abs(m::la::dot(d, f.side)) < 0.9 * sp && std::abs(m::la::dot(d, f.tan)) < 0.6 * sp
					&& -m::la::dot(d, f.n) > -0.5 * sp && -m::la::dot(d, f.n) < f.T + 0.5 * sp) { onCut = true; break; }
			}
			if (!onCut) { kept.push_back(x); keptW.push_back(wts[i]); }
		}
		seeds.swap(kept);
		wts.swap(keptW);
		for (const V& x : cutSeeds) { seeds.push_back(x); wts.push_back(0.0); }
	}
	if (seeds.size() < 2) {
		double vol = 0.0;
		const V c = MassCentre(source, vol);
		pieces.push_back(source); info.push_back({vol, c});
		if (log) *log = "cell fracture: fewer than 2 seeds, source kept whole";
		return;
	}
	                                                                       
	                                                           
	const bool remainder = p.keepRemainder && !src.empty();
	std::vector<char> far(seeds.size(), 0);
	if (remainder) {
		std::vector<size_t> idx(seeds.size());
		for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
		std::vector<char> f0(seeds.size());
		for (size_t i = 0; i < seeds.size(); ++i) f0[i] = isFar(seeds[i]) ? 1 : 0;
		std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return f0[a] < f0[b]; });
		std::vector<V> s2; std::vector<double> w2;
		for (size_t i : idx) { s2.push_back(seeds[i]); w2.push_back(wts[i]); far[s2.size() - 1] = f0[i]; }
		seeds.swap(s2); wts.swap(w2);
	}

	                                                                                 
	const double k = wood ? std::max(1.0, p.grainStretch) : 1.0;
	const V g = Unit(V(p.grainX, p.grainY, p.grainZ), V(0, 0, 1));
	m::mat3x4 toS, fromS;
	{
		const m::mat3 Ad = m::mat3({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) + m::la::outerprod(g, g) * (1.0 / k - 1.0);
		const m::mat3 Ai = m::mat3({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) + m::la::outerprod(g, g) * (k - 1.0);
		toS = m::mat3x4(Ad[0], Ad[1], Ad[2], V(0.0));
		fromS = m::mat3x4(Ai[0], Ai[1], Ai[2], V(0.0));
	}
	auto A = [&](const V& x) { return x + g * ((1.0 / k - 1.0) * m::la::dot(g, x)); };
	const bool aniso = k > 1.0;
	const m::Manifold work = aniso ? source.Transform(toS) : source;
	std::vector<V> sd(seeds.size());
	for (size_t i = 0; i < seeds.size(); ++i) sd[i] = aniso ? A(seeds[i]) : seeds[i];
	const double hw = std::cbrt(work.Volume() / nBase);                              

	                                                                                    
	Relief D;
	{
		const double lambda0 = std::max(0.05, p.roughScale) * 1.6 * hw;
		D.octaves = M.octaves;
		D.f0 = 1.0 / lambda0;
		D.ridged = M.ridged;
		                                                                      
		                                                                       
		                                                                   
		                                                       
		const double g999[5] = {1.0, 2.6, 3.95, 4.95, 5.8};
		D.amp = rough > 0.0 && D.octaves > 0 ? rough * 0.8 / (g999[std::min(4, D.octaves)] * D.f0) : 0.0;
		if (wood && p.splinter > 0.0) {
			D.g = g;                                                             
			D.splAmp = std::min(2.0, p.splinter) * 0.35 * hw;
			D.splPerp = 1.0 / (0.2 * hw);
			D.splAlong = 1.0 / (2.5 * hw);
		}
		for (int c = 0; c < 4; ++c)
			D.off[c] = V(Rnd(seed, 7000 + c, 0), Rnd(seed, 7000 + c, 1), Rnd(seed, 7000 + c, 2)) * 97.0;
	}
	const bool bend = D.Active();
	                                                                 
	double edge = 1e300;
	if (D.amp > 0.0) edge = std::min(edge, 1.0 / (D.f0 * std::pow(2.0, D.octaves - 1)) / 3.0);
	if (D.splAmp > 0.0) edge = std::min(edge, 1.0 / D.splPerp / 2.5);
	edge /= std::max(0.01, std::min(4.0, p.detail));
	edge = std::max(edge, hw / 60.0);
	if (bend) {
		                                                                   
		const double tris = double(sd.size()) * 8.0 * hw * hw / (0.433 * edge * edge);
		if (tris > 1.0e6) edge *= std::sqrt(tris / 1.0e6);
	}
	const double reach = bend ? 1.5 * (D.amp * 1.9 + D.splAmp * 1.6) : 0.0;                   
	const m::Box sbb = work.BoundingBox();
	const V pad = (sbb.max - sbb.min) * 0.02 + V(1e-3 + reach + (bend ? edge : 0.0));
	const m::Manifold box = m::Manifold::Cube(sbb.max - sbb.min + pad * 2.0).Translate(sbb.min - pad);

	                                                                                       
	const double gap = std::max(0.0, p.gap);
	const double eps = gap > 0.0 ? 0.0 : (bend ? 0.25 * edge : 1e-4 * hw);                                       
	double wMax = 0.0;
	for (double w : wts) wMax = std::max(wMax, w);
	std::vector<m::Manifold> W(sd.size());
	std::vector<m::Box> WB(sd.size());
	ProjectDamageParallelFor(0, int64_t(sd.size()), [&](int64_t ii) {
		const size_t i = size_t(ii);
		const V si = sd[i];
		std::vector<std::pair<double, size_t>> order;
		order.reserve(sd.size());
		for (size_t j = 0; j < sd.size(); ++j)
			if (j != i) order.push_back({m::la::length(sd[j] - si), j});
		std::sort(order.begin(), order.end());
		m::Manifold cell = box;
		size_t used = 0;
		auto trim = [&](size_t j) {
			const double d = m::la::length(si - sd[j]);
			if (d < 1e-12) return;
			const V nrm = (si - sd[j]) / d;
			const V mid = (si + sd[j]) * 0.5;
			const double shift = (wts[j] - wts[i]) / (2.0 * d);
			cell = cell.TrimByPlane(nrm, m::la::dot(nrm, mid) + shift + 0.5 * gap - eps);
		};
		const size_t k0 = std::min<size_t>(24, order.size());
		for (; used < k0; ++used) trim(order[used].second);
		while (used < order.size() && !cell.IsEmpty()) {
			const m::Box cb = cell.BoundingBox();
			double R = 0.0;
			for (int c = 0; c < 8; ++c) {
				const V corner((c & 1) ? cb.max.x : cb.min.x, (c & 2) ? cb.max.y : cb.min.y, (c & 4) ? cb.max.z : cb.min.z);
				R = std::max(R, m::la::length(corner - si));
			}
			const double dj = order[used].first;
			if (dj > 2.0 * R + 2.0 * eps && dj * 0.5 - wMax / (2.0 * dj) > R + eps) break;
			const size_t until = std::min(order.size(), used + 16);
			for (; used < until; ++used) trim(order[used].second);
		}
		if (cell.IsEmpty()) return;
		if (bend) cell = cell.RefineToLength(edge).Warp([&](V& v) { v = v + D(v); });
		W[i] = cell;
		WB[i] = cell.BoundingBox();
	});

	                                                                                        
	const double dust = 1e-9 * volume;                                                                
	size_t nNear = sd.size();
	if (remainder) { nNear = 0; while (nNear < sd.size() && !far[nNear]) ++nNear; }
	std::vector<std::vector<m::Manifold>> out(nNear + (remainder ? 1 : 0));
	ProjectDamageParallelFor(0, int64_t(nNear), [&](int64_t ii) {
		const size_t i = size_t(ii);
		if (W[i].IsEmpty()) return;
		const m::Box wb = work.BoundingBox();
		if (!Overlap(WB[i], wb)) return;
		m::Manifold part = work ^ W[i];
		if (part.IsEmpty()) return;
		const m::Box pb = part.BoundingBox();
		std::vector<m::Manifold> ops{part};
		for (size_t j = 0; j < i; ++j)
			if (!W[j].IsEmpty() && Overlap(WB[j], pb)) ops.push_back(W[j]);
		if (ops.size() > 1) part = m::Manifold::BatchBoolean(ops, m::OpType::Subtract);
		for (m::Manifold& pc : part.Decompose()) {
			m::Manifold back = aniso ? pc.Transform(fromS) : pc;
			if (back.Volume() > dust) out[i].push_back(std::move(back));
		}
	});
	if (remainder) {
		std::vector<m::Manifold> ops{work};
		for (size_t j = 0; j < nNear; ++j) if (!W[j].IsEmpty()) ops.push_back(W[j]);
		m::Manifold rest = ops.size() > 1 ? m::Manifold::BatchBoolean(ops, m::OpType::Subtract) : work;
		for (m::Manifold& pc : rest.Decompose()) {
			m::Manifold back = aniso ? pc.Transform(fromS) : pc;
			if (back.Volume() > dust) out[nNear].push_back(std::move(back));
		}
	}
	size_t errors = 0;
	std::vector<m::Manifold> all;
	std::vector<size_t> owner;
	for (size_t k = 0; k < out.size(); ++k)
		for (m::Manifold& pc : out[k]) {
			if (pc.Status() != m::Manifold::Error::NoError) { ++errors; continue; }
			all.push_back(std::move(pc)); owner.push_back(k);
		}
	                                                                     
	const m::Box sb = source.BoundingBox();
	MergePieces(all, owner, cap, gap + 1e-4 * m::la::length(sb.max - sb.min));
	for (m::Manifold& pc : all) {
		double vol = 0.0;
		const V c = MassCentre(pc, vol);
		info.push_back({vol, c});
		pieces.push_back(std::move(pc));
	}
	if (log) {
		double tot = 0.0;
		for (const CellPieceInfo& r : info) tot += r.volume;
		*log = "cell fracture: " + std::to_string(seeds.size()) + " seeds, " + std::to_string(pieces.size())
			+ " pieces" + (planar ? " (planar)" : "") + (remainder ? ", intact remainder" : "")
			+ ", volume kept " + std::to_string(int(std::round(100.0 * tot / volume))) + "%"
			+ (errors ? ", " + std::to_string(errors) + " failed" : "");
	}
}

// основной вход: собираем геометрию по переданным настройкам
void BuildCellFracture(const CoreInput& in, const m::Manifold& source, uint32_t sourceId,
    const CellFractureParams& p, Int32 mainSeed,
    const std::vector<std::vector<EVec>>* strokes,
    std::vector<m::Manifold>& pieces, std::vector<CellPieceInfo>& info, std::string* log)
{
    CellFractureParams q=p;
    const bool modifier=p.strokeMode==kModifierGuidance;
    const bool guided=(modifier || p.strokeMode==3) && p.guidanceAmount>0 && strokes && !strokes->empty();
    const size_t target=size_t(std::min(5000,std::max(1,p.pieces)+(guided?std::min(2000,p.guidanceAmount):0)));
    q.maxPieces=int(target);
    BuildCellFractureImpl(in,source,sourceId,q,mainSeed,strokes,pieces,info,log);
    if(pieces.empty())return;                                                       
    std::vector<size_t> owner(pieces.size());
    for(size_t i=0;i<owner.size();++i)owner[i]=i;
    MergePieces(pieces,owner,target,1e-6*m::la::length(source.BoundingBox().max-source.BoundingBox().min));
    const double eps=std::max(1e-18,std::abs(source.Volume())*1e-12);
    while((!modifier || p.material != 1) && !guided && pieces.size()<target) {
        std::vector<size_t> order(pieces.size());
        for(size_t i=0;i<order.size();++i)order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return pieces[a].Volume()>pieces[b].Volume();});
        bool split=false;
        for(size_t i:order) {
            const auto bb=pieces[i].BoundingBox();const V ext=bb.max-bb.min;
            int axes[3]={0,1,2};
            std::stable_sort(axes,axes+3,[&](int a,int b){return ext[a]>ext[b];});
            for(int axis:axes) {
                V n(0.0);n[axis]=1.0;
                auto halves=pieces[i].SplitByPlane(n,0.5*(bb.min[axis]+bb.max[axis]));
                if(p.gap>0.) {
                    const double d=0.5*(bb.min[axis]+bb.max[axis]);
                    halves.first=pieces[i].TrimByPlane(n,d+0.5*p.gap);
                    halves.second=pieces[i].TrimByPlane(-n,-d+0.5*p.gap);
                }
                if(halves.first.Status()!=m::Manifold::Error::NoError || halves.second.Status()!=m::Manifold::Error::NoError
                    || halves.first.Volume()<=eps || halves.second.Volume()<=eps)continue;
                pieces[i]=halves.first;pieces.push_back(halves.second);split=true;break;
            }
            if(split)break;
        }
        if(!split)break;
    }
    info.clear();
    for(const auto& part:pieces){double vol=0.;const V c=MassCentre(part,vol);info.push_back({vol,c});}
    if(log)*log += "; requested "+std::to_string(target)+", emitted "+std::to_string(pieces.size());
}
PROJECT_DAMAGE_NS_MACRO_END
