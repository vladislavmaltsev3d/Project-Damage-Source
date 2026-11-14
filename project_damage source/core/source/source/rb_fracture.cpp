// предыдущий вариант подготовки объёмных осколков
// ProjectDamage rigid-body fracture — GPL-3.0-or-later; see LICENSE.
//
// Recipe: rb-fracture-v1. See rb_fracture.h / rb_fracture_params.h.
//
// 1. Volume, bounding box, a local piece size h = cbrt(V / pieces).
// 2. Hints: impact strokes -> (centre = mean of the points, radius = largest
//    distance from it, or impactRadius for a dot) + p.impacts. Density at x:
//      rho(x) = 1 + (impactDensity - 1) * max_i strength_i exp(-(d_i / r_i)^2)
// 3. Seeds (all inside the solid; inside = ray parity on the BVH):
//    background: rejection sampling of rho, Poisson-like spacing ~ h rho^-1/3;
//    clustering (size variation): a share of the seeds is placed close to
//      earlier ones -> small pieces next to big ones;
//    rubble: extra seeds packed inside each impact (concrete chips, crumbs);
//    glass: per impact a polar layout - spokes x rings with growing radii,
//      half the wall thickness under the surface -> wedge shards;
//    cut strokes: along each stroke, every spacing, pairs mirrored across
//      it (offset 0.25 spacing) at depths down through the object: the
//      Voronoi boundary of each pair runs exactly along the stroke and in
//      along the surface normal.
// 4. Wood / Fabric: everything happens in a space compressed along the
//    grain by k (stretch): A = I + (1/k - 1) g g^T. Seeds are mapped by A,
//    the cells are isotropic there, pieces map back by A^-1 -> cells k times
//    longer along the grain (splinters / weave-aligned patches).
// 5. Cell i = bounding box trimmed by the bisector plane of every seed j
//    that can bound it: the k nearest first, then any seed closer than twice
//    the cell's current radius, until none is left (exact Voronoi cell).
//    Gap: each plane moved g/2 toward its own seed.
// 6. piece = source ∩ cell, decomposed (a concave source can split a cell),
//    dust (minPiece x source volume) dropped; volume and centre of mass per
//    piece for the rigid body.
#include "manifold/manifold.h"
#include "rb_fracture.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

struct Impact { V c; double r, s; };

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
};

}               

// основной вход: собираем геометрию по переданным настройкам
void BuildRigidBodyFracture(const CoreInput& in, const m::Manifold& source, uint32_t sourceId,
	const RBFractureParams& pIn, Int32 mainSeed,
	const std::vector<std::vector<EVec>>* cutStrokes,
	const std::vector<std::vector<EVec>>* impactStrokes,
	std::vector<m::Manifold>& pieces, std::vector<RBPieceInfo>& info)
{
	(void)sourceId;
	if (!pIn.enable || source.IsEmpty() || in.view.npolys <= 0) return;
	RBFractureParams p = pIn;
	const int mat = std::max(0, std::min(4, p.material));
	                    
	const double defVar[5] = {0.45, 0.25, 0.3, 0.35, 0.75};
	const double defRubble[5] = {0.35, 0.1, 0.05, 0.2, 0.6};
	const double var = p.sizeVariation >= 0.0 ? std::min(1.0, p.sizeVariation) : defVar[mat];
	const double rubble = p.rubble >= 0.0 ? std::min(1.0, p.rubble) : defRubble[mat];
	const int nBase = std::max(1, std::min(p.pieces, p.maxPieces));
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x5242465241435431ULL)                       
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);

	Solid S;
	S.view = &in.view;
	S.bvh.Build(in.view);
	const m::Box bb = source.BoundingBox();
	S.lo = bb.min; S.hi = bb.max;
	const double volume = source.Volume();
	if (!(volume > 0.0)) return;
	const double h = std::cbrt(volume / nBase);

	                                                                             
	std::vector<Impact> imp;
	for (const RBImpact& r : p.impacts)
		imp.push_back({V(r.x, r.y, r.z), r.radius > 0.0 ? r.radius : p.impactRadius, std::max(0.0, r.strength)});
	if (impactStrokes)
		for (const std::vector<EVec>& st : *impactStrokes) {
			if (st.empty()) continue;
			V c(0.0);
			for (const EVec& q : st) c += q;
			c = c / double(st.size());
			double r = 0.0;
			for (const EVec& q : st) r = std::max(r, m::la::length(q - c));
			imp.push_back({c, r > 0.25 * p.impactRadius ? r : p.impactRadius, 1.0});
		}
	const double dens = std::max(1.0, p.impactDensity);
	auto rho = [&](const V& x) {
		double best = 0.0;
		for (const Impact& I : imp) {
			const double d = m::la::length(x - I.c) / std::max(1e-9, I.r);
			best = std::max(best, I.s * std::exp(-d * d));
		}
		return 1.0 + (dens - 1.0) * std::min(1.0, best);
	};
	const double rhoMax = imp.empty() ? 1.0 : dens;

	                                                                                 
	                                                                       
	                                                                           
	                                                        
	std::vector<V> seeds;
	std::vector<double> wts;
	const double wPrim = (mat == 4 ? 0.45 : (mat == 0 ? 0.2 : 0.0)) * h;
	const size_t cap = size_t(std::max(4, p.maxPieces));
	uint64_t draw = 0;
	auto rnd = [&](uint64_t ch) { return Rnd(seed, draw, ch); };
	auto farEnough = [&](const V& x, double minD) {
		for (const V& s : seeds) if (m::la::length(s - x) < minD) return false;
		return true;
	};
	const V ext = S.hi - S.lo;
	                                                                       
	const int primaries = std::max(1, int(std::round(nBase * (1.0 - (mat == 4 ? 0.75 : 0.5) * var))));
	for (int tries = 0; tries < primaries * 60 && int(seeds.size()) < primaries && seeds.size() < cap; ++tries, ++draw) {
		const V x = S.lo + V(ext.x * rnd(0), ext.y * rnd(1), ext.z * rnd(2));
		const double r = rho(x);
		if (rnd(3) * rhoMax > r) continue;
		if (!farEnough(x, 0.55 * h * std::cbrt(1.0 / r))) continue;
		if (!S.Inside(x)) continue;
		seeds.push_back(x);
		wts.push_back(wPrim * wPrim * (0.5 + rnd(17)));
	}
	const size_t nPrim = seeds.size();
	const double clusterR[5] = {0.35, 0.35, 0.35, 0.35, 0.16};                                 
	for (int k = 0; k < nBase - primaries && nPrim > 0 && seeds.size() < cap; ++k, ++draw) {
		const V& c = seeds[size_t(rnd(4) * double(nPrim)) % nPrim];
		const double sd = clusterR[mat] * h;
		const V x = c + V(rnd(5) - 0.5, rnd(6) - 0.5, rnd(7) - 0.5) * (2.0 * sd);
		if (m::la::length(x - c) < 0.5 * sd || !S.Inside(x)) continue;
		seeds.push_back(x);
		wts.push_back(0.0);
	}
	                                                                          
	                                                                         
	for (size_t ii = 0; ii < imp.size() && mat != 3; ++ii) {
		const Impact& I = imp[ii];
		const double R = 1.2 * I.r;
		const double vImp = 0.5 * 4.18879 * R * R * R;
		const int want = std::min(int(cap), int(std::round((dens - 1.0) * std::min(1.0, I.s) * vImp / (h * h * h))));
		for (int k = 0, tries = 0; k < want && tries < want * 40 && seeds.size() < cap; ++tries, ++draw) {
			const V x = I.c + V(rnd(13) - 0.5, rnd(14) - 0.5, rnd(15) - 0.5) * (2.0 * R);
			const double d = m::la::length(x - I.c) / R;
			if (d > 1.0 || rnd(16) > std::exp(-2.0 * d * d) || !S.Inside(x)) continue;
			if (!farEnough(x, 0.3 * h / std::cbrt(dens))) continue;
			seeds.push_back(x);
			wts.push_back(0.0);
			++k;
		}
	}
	                                   
	for (const Impact& I : imp) {
		if (mat == 3) break;                                           
		const int nr = int(std::round(rubble * nBase * 0.5 * std::min(1.0, I.s)));
		for (int k = 0, tries = 0; k < nr && tries < nr * 40 && seeds.size() < cap; ++tries, ++draw) {
			const V x = I.c + V(rnd(8) - 0.5, rnd(9) - 0.5, rnd(10) - 0.5) * (1.0 * I.r);
			if (m::la::length(x - I.c) > 0.5 * I.r || !S.Inside(x)) continue;
			if (!farEnough(x, 0.08 * h)) continue;
			seeds.push_back(x);
			wts.push_back(0.0);
			++k;
		}
	}
	                                                                          
	                                                                         
	if (mat == 3)
		for (V& x : seeds) {
			V q, n;
			if (!S.Nearest(x, q, n)) continue;
			const double T = S.Thickness(q, n);
			if (T > 0.0) x = q - n * (0.5 * T);
		}
	                                       
	if (mat == 3)
		for (const Impact& I : imp) {
			V q, n;
			if (!S.Nearest(I.c, q, n)) continue;
			const double T = S.Thickness(q, n);
			const V mid = q - n * (0.5 * std::max(T, 1e-6));
			const V t = Unit(m::la::cross(std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(0, 1, 0), n), V(1, 0, 0));
			const V b = m::la::cross(n, t);
			const int spokes = std::max(3, std::min(48, p.glassSpokes));
			const double growth = std::max(1.1, p.glassRingGrowth);
			const double phase = 6.2831853 * Rnd(seed, 99991, 1);
			double r = 0.12 * I.r;
			for (int ring = 0; r < 1.4 * I.r && seeds.size() < cap; ++ring, r *= growth)
				for (int s = 0; s < spokes && seeds.size() < cap; ++s, ++draw) {
					const double a = phase + 6.2831853 * (s + 0.5 * (ring & 1) + 0.3 * (rnd(11) - 0.5)) / spokes;
					const V x = mid + (t * std::cos(a) + b * std::sin(a)) * (r * (0.88 + 0.24 * rnd(12)));
					if (S.Inside(x)) { seeds.push_back(x); wts.push_back(0.0); }
				}
		}
	                                                                         
	std::vector<V> cutSeeds;
	if (cutStrokes) {
		const double sp = p.cutSpacing > 0.0 ? p.cutSpacing : 0.6 * h;
		struct CutFrame { V q, n, tan, side; double T; };
		std::vector<CutFrame> frames;
		for (const std::vector<EVec>& st : *cutStrokes) {
			if (st.size() < 2) continue;
			double carry = 0.0;
			for (size_t i = 0; i + 1 < st.size() && seeds.size() < cap; ++i) {
				const V a = st[i], b2 = st[i + 1];
				const double L = m::la::length(b2 - a);
				if (L < 1e-9) continue;
				const V tan = (b2 - a) / L;
				double at = carry;
				for (; at < L && seeds.size() < cap; at += sp, ++draw) {
					V q, n;
					if (!S.Nearest(a + tan * at, q, n)) continue;
					const V side = Unit(m::la::cross(n, tan), V(1, 0, 0));
					const double T = S.Thickness(q, n);
					frames.push_back({q, n, tan, side, T});
					const int layers = std::max(1, int(std::ceil(T / sp)));
					for (int l = 0; l < layers && seeds.size() + 2 <= cap; ++l) {
						const V c = q - n * (std::min(T, (l + 0.5) * sp));
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
		for (size_t si2 = 0; si2 < seeds.size(); ++si2) {
			const V& x = seeds[si2];
			bool onCut = false;
			for (const CutFrame& f : frames) {
				const V d = x - f.q;
				if (std::abs(m::la::dot(d, f.side)) < 0.9 * sp && std::abs(m::la::dot(d, f.tan)) < 0.6 * sp
					&& -m::la::dot(d, f.n) > -0.5 * sp && -m::la::dot(d, f.n) < f.T + 0.5 * sp) { onCut = true; break; }
			}
			if (!onCut) { kept.push_back(x); keptW.push_back(wts[si2]); }
		}
		seeds.swap(kept);
		wts.swap(keptW);
		for (const V& x : cutSeeds) { seeds.push_back(x); wts.push_back(0.0); }
	}
	if (seeds.size() < 2) { pieces.push_back(source); info.push_back({volume, (S.lo + S.hi) * 0.5}); return; }

	                                                                              
	const bool aniso = mat == 1 || mat == 2;
	const double k = aniso ? std::max(1.0, mat == 1 ? p.grainStretch : p.weaveStretch) : 1.0;
	const V g = Unit(V(p.grainX, p.grainY, p.grainZ), V(0, 0, 1));
	auto A = [&](const V& x) { return x + g * ((1.0 / k - 1.0) * m::la::dot(g, x)); };
	m::mat3x4 toS, fromS;
	{
		const m::mat3 Ad = m::mat3({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) + m::la::outerprod(g, g) * (1.0 / k - 1.0);
		const m::mat3 Ai = m::mat3({1, 0, 0}, {0, 1, 0}, {0, 0, 1}) + m::la::outerprod(g, g) * (k - 1.0);
		toS = m::mat3x4(Ad[0], Ad[1], Ad[2], V(0.0));
		fromS = m::mat3x4(Ai[0], Ai[1], Ai[2], V(0.0));
	}
	const m::Manifold src = aniso ? source.Transform(toS) : source;
	std::vector<V> sd(seeds.size());
	for (size_t i = 0; i < seeds.size(); ++i) sd[i] = aniso ? A(seeds[i]) : seeds[i];
	const m::Box sbb = src.BoundingBox();
	const V pad = (sbb.max - sbb.min) * 0.02 + V(1e-3);
	const m::Manifold box = m::Manifold::Cube(sbb.max - sbb.min + pad * 2.0).Translate(sbb.min - pad);

	                                                                                   
	const double gap = std::max(0.0, p.gap) * (aniso ? 1.0 : 1.0);
	const double minVol = std::max(0.0, p.minPiece) * volume;
	double wMax = 0.0;
	for (double w : wts) wMax = std::max(wMax, w);
	std::vector<std::vector<m::Manifold>> out(sd.size());
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
			cell = cell.TrimByPlane(nrm, m::la::dot(nrm, mid) + shift + 0.5 * gap);
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
			if (dj > 2.0 * R && dj * 0.5 - (wMax + 0.0) / (2.0 * dj) > R) break;
			const size_t until = std::min(order.size(), used + 16);
			for (; used < until; ++used) trim(order[used].second);
		}
		if (cell.IsEmpty()) return;
		m::Manifold part = src ^ cell;
		for (m::Manifold& pc : part.Decompose()) {
			m::Manifold back = aniso ? pc.Transform(fromS) : pc;
			if (back.Volume() > minVol) out[i].push_back(std::move(back));
		}
	});
	for (auto& list : out)
		for (m::Manifold& pc : list) {
			                                                                    
			const m::MeshGL64 mg = pc.GetMeshGL64();
			double vol = 0.0;
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
			c = std::abs(vol) > 1e-18 ? c / (24.0 * vol) : (pc.BoundingBox().min + pc.BoundingBox().max) * 0.5;
			info.push_back({vol, c});
			pieces.push_back(std::move(pc));
		}
}
PROJECT_DAMAGE_NS_MACRO_END
