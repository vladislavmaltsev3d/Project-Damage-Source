// деформируем поверхность как глину, сохраняя допустимую толщину
// ProjectDamage clay — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Recipe: clay-v2 (v1 + self-intersection safety). Hand-modelled clay, displaced into the base mesh.
//
//   1. refine: base.RefineToLength(R / resolution), the edge grown to the
//      triangle budget. Run original id, faceID and properties (UVs) kept;
//   2. weld: one geometric point per position (mergeFromVert/mergeToVert),
//      so the seams of the property mesh never open;
//   3. weight per point: Avoid polygons 0, zone weights (inverse-distance over
//      the source polygon's corners) x the main ProjectDamage mask;
//   4. softening: lambda/mu smoothing (lambda .5, mu -.34) weighted by
//      the point weight. Corners and edges round off, flat faces stay, the
//      volume is kept (a plain Laplacian would shrink the whole object). The
//      pass count scales with (softness)^2 and (R / resolution / edge)^2, so
//      the rounding radius is resolution independent;
//   5. displacement along the smooth normal, sum of:
//        lumps  - 2-octave fBm, amplitude 1.5% of R
//        presses- thumb prints scattered by area: flat-bottomed elliptic dents
//                 (smoothstep profile), a raised rim of displaced clay and
//                 spiral fingerprint ridges (faded when finer than the mesh
//                 can carry), with a smooth facing weight
//        smears - long, shallow presses with the rim pushed to the stroke end
//        grain  - high-frequency fBm (needs a fine resolution)
//        cracks - V grooves along Voronoi edges where a coverage noise allows
//      inward travel capped at thicknessCap x the local wall thickness;
//   6. no self-intersections (clay-v2):
//        - fold limit: the displacement is soft-clamped to 0.4 x the local
//          convex radius (inward) and concave radius (outward), curvatures
//          spread over two rings - the offset surface cannot fold;
//        - two smoothing passes on the displacement remove mesh-frequency
//          teeth on rims and concave fillets;
//        - fold relaxation where an edge would be squeezed below 35%;
//        - a final triangle-triangle check (plus flipped-triangle check)
//          diffuses the displacement around anything still crossing, and as a
//          last resort returns that neighbourhood to the softened surface;
//   7. sag per connected piece: points drop along gravity by height^2 and the
//      lower part spreads out from the piece's axis (smooth, monotonic);
//   8. rebuild the Manifold from the edited MeshGL64 and check it.
//
// Pure function of its inputs: presses and smears use counter-based hashes
// keyed by (seed, kind, index).
#include "clay.h"
#include "manifold/manifold.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
constexpr double kClayPi = 3.14159265358979323846;

inline double ClayClamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline double ClaySmooth01(double x) { x = ClayClamp01(x); return x * x * (3.0 - 2.0 * x); }
inline double ClayDot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V ClayCross(const V& a, const V& b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline double ClayLen(const V& a) { return std::sqrt(ClayDot(a, a)); }
inline V ClayUnit(const V& a, const V& fallback) { const double l = ClayLen(a); return l > 1e-30 ? a / l : fallback; }

inline uint64_t ClayMix(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}
inline uint64_t ClayHash3(int x, int y, int z, uint64_t salt)
{
	return ClayMix((uint64_t)(uint32_t)x ^ ClayMix((uint64_t)(uint32_t)y ^ ClayMix((uint64_t)(uint32_t)z ^ ClayMix(salt))));
}
inline double ClayU01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }

double ClayPerlin(const V& p, uint64_t salt)
{
	const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const int X = (int)fx, Y = (int)fy, Z = (int)fz;
	const double x = p.x - fx, y = p.y - fy, z = p.z - fz;
	auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
	const double u = fade(x), v = fade(y), w = fade(z);
	auto g = [&](int i, int j, int k) {
		const double a = x - i, b = y - j, c = z - k;
		switch ((int)(ClayHash3(X + i, Y + j, Z + k, salt) % 12u)) {
		case 0: return a + b;  case 1: return -a + b; case 2: return a - b;  case 3: return -a - b;
		case 4: return a + c;  case 5: return -a + c; case 6: return a - c;  case 7: return -a - c;
		case 8: return b + c;  case 9: return -b + c; case 10: return b - c; default: return -b - c;
		}
	};
	const double x00 = g(0, 0, 0) + u * (g(1, 0, 0) - g(0, 0, 0)), x10 = g(0, 1, 0) + u * (g(1, 1, 0) - g(0, 1, 0));
	const double x01 = g(0, 0, 1) + u * (g(1, 0, 1) - g(0, 0, 1)), x11 = g(0, 1, 1) + u * (g(1, 1, 1) - g(0, 1, 1));
	const double y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
	return y0 + w * (y1 - y0);
}

                   
double ClayFbm(V p, int octaves, double rough, double lac, uint64_t salt)
{
	double amp = 1.0, sum = 0.0, norm = 0.0;
	for (int i = 0; i < octaves; ++i) { sum += ClayPerlin(p, salt + (uint64_t)i) * amp; norm += amp; amp *= rough; p = p * lac; }
	return sum / norm;
}

                                                         
double ClayVoronoiEdge(const V& p)
{
	const V c(std::floor(p.x), std::floor(p.y), std::floor(p.z)), l = p - c;
	auto site = [&](int i, int j, int k) {
		const uint64_t h = ClayHash3((int)c.x + i, (int)c.y + j, (int)c.z + k, 0xC1A7ull);
		return V(i, j, k) + V(ClayU01(h), ClayU01(ClayMix(h + 1)), ClayU01(ClayMix(h + 2))) * 0.9 - l;
	};
	V toClosest(0.0); double best = 1e30;
	for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
		const V d = site(i, j, k); const double dd = ClayDot(d, d);
		if (dd < best) { best = dd; toClosest = d; }
	}
	double edge = 8.0;
	for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
		const V d = site(i, j, k), perp = d - toClosest;
		const double pl = ClayDot(perp, perp);
		if (pl > 1e-8) edge = std::min(edge, ClayDot((toClosest + d) * 0.5, perp / std::sqrt(pl)));
	}
	return std::max(0.0, edge);
}

                                                                     
struct ClayMark
{
	V c, n, u, v;                                                            
	double r = 1.0;                       
	double aspect = 1.0;                           
	double depth = 0.0;
	double rim = 0.0;                                      
	double prints = 0.0;                                                    
	V whorl{ 0.0 };                                                         
	bool smear = false;
	double Reach() const { return r * std::max(1.0, aspect) * 1.7; }
};

double ClayMarkHeight(const ClayMark& k, const V& pos, const V& nrm)
{
	const V d = pos - k.c;
	const double h = ClayDot(d, k.n);
	const double hMax = 0.6 * k.r;
	                                                                          
	                                           
	const double facing = ClaySmooth01((ClayDot(nrm, k.n) - 0.15) / 0.4);
	if (std::fabs(h) > hMax || facing <= 0.0) return 0.0;
	const double su = ClayDot(d, k.u) / (k.r * k.aspect), sv = ClayDot(d, k.v) / k.r;
	const double rho = std::sqrt(su * su + sv * sv);
	if (rho > 1.7) return 0.0;
	double depth = k.depth;
	double rim = k.rim;
	if (k.smear) {
		depth *= 0.75 + 0.25 * (1.0 - su);                                                   
		rim *= ClaySmooth01((su + 0.2) / 1.2);                                                    
	}
	double z = -depth * ClaySmooth01((1.0 - rho) / 0.65);                           
	z += rim * depth * 0.5 * std::exp(-((rho - 1.08) / 0.27) * ((rho - 1.08) / 0.27));
	if (k.prints > 0.0 && rho < 1.0) {
		const double pu = su * k.aspect - k.whorl.x, pv = sv - k.whorl.y;                   
		const double rr = std::sqrt(pu * pu + pv * pv), th = std::atan2(pv, pu);
		const double phase = rr * 13.0 + 0.35 * th / (2.0 * kClayPi) + 0.6 * std::sin(th * 2.0) * rr;
		z += k.prints * depth * 0.08 * std::cos(2.0 * kClayPi * phase) * (1.0 - rho * rho);
	}
	return z * facing * ClaySmooth01(1.0 - std::fabs(h) / hMax);
}

                                                              
bool ClaySegTri(const V& p, const V& q, const V& a, const V& b, const V& c)
{
	const V e1 = b - a, e2 = c - a, d = q - p, h = ClayCross(d, e2);
	const double det = ClayDot(e1, h);
	if (std::fabs(det) < 1e-20) return false;
	const double f = 1.0 / det;
	const V s = p - a;
	const double u = f * ClayDot(s, h);
	if (u < 0.0 || u > 1.0) return false;
	const V qq = ClayCross(s, e1);
	const double v = f * ClayDot(d, qq);
	if (v < 0.0 || u + v > 1.0) return false;
	const double t = f * ClayDot(e2, qq);
	return t > 1e-9 && t < 1.0 - 1e-9;
}
bool ClayTriTri(const V* A, const V* B)
{
	for (int e = 0; e < 3; ++e) {
		if (ClaySegTri(A[e], A[(e + 1) % 3], B[0], B[1], B[2])) return true;
		if (ClaySegTri(B[e], B[(e + 1) % 3], A[0], A[1], A[2])) return true;
	}
	return false;
}

struct ClayMarkGrid
{
	double cell = 1.0;
	std::unordered_map<uint64_t, std::vector<int32_t>> cells;
	static uint64_t Key(int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576); }
	void Build(const std::vector<ClayMark>& marks)
	{
		cell = 1e-9;
		for (const ClayMark& k : marks) cell = std::max(cell, k.Reach());
		for (size_t i = 0; i < marks.size(); ++i) {
			const V& c = marks[i].c;
			cells[Key((int)std::floor(c.x / cell), (int)std::floor(c.y / cell), (int)std::floor(c.z / cell))].push_back((int32_t)i);
		}
	}
	template <class F> void Visit(const V& p, F&& f) const
	{
		const int x = (int)std::floor(p.x / cell), y = (int)std::floor(p.y / cell), z = (int)std::floor(p.z / cell);
		for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
			auto it = cells.find(Key(x + i, y + j, z + k));
			if (it != cells.end()) for (int32_t id : it->second) f(id);
		}
	}
};

}             

// основной вход: собираем геометрию по переданным настройкам
bool ApplyClay(const CoreInput& in, const ClayParams& p, Int32 mainSeed, const IFieldMask* mask,
	const m::Manifold& base, m::Manifold& out, std::string* log)
{
	if (!p.enable || base.Status() != m::Manifold::Error::NoError || base.NumTri() == 0) return false;
	const MeshView& view = in.view;

	                                                                                   
	const m::Box bb = base.BoundingBox();
	const V ext = bb.max - bb.min;
	const double R = p.refSize > 0.0 ? p.refSize : std::max({ ext.x, ext.y, ext.z });
	if (!(R > 0.0)) return false;
	double L = R / std::max(8.0, p.resolution);
	const double area = base.SurfaceArea();
	const double budget = (double)std::max<int32_t>(5000, p.maxTris);
	L = std::max(L, std::sqrt(area * 2.31 / budget));                                              
	m::Manifold refined = base.RefineToLength(L);
	if (refined.Status() != m::Manifold::Error::NoError || refined.NumTri() == 0) refined = base;

	m::MeshGL64 mesh = refined.GetMeshGL64();
	const size_t np = mesh.numProp, nv = mesh.NumVert(), nt = mesh.NumTri();
	if (np < 3 || nv == 0 || nt == 0) return false;

	                                                                                      
	std::vector<int32_t> geo(nv);
	std::iota(geo.begin(), geo.end(), 0);
	for (size_t k = 0; k < mesh.mergeFromVert.size() && k < mesh.mergeToVert.size(); ++k)
		geo[(size_t)mesh.mergeFromVert[k]] = (int32_t)mesh.mergeToVert[k];
	for (size_t i = 0; i < nv; ++i) {
		int32_t r = geo[i]; int guard = 0;
		while (geo[(size_t)r] != r && guard++ < 64) r = geo[(size_t)r];
		geo[i] = r;
	}
	std::vector<V> pos(nv);
	for (size_t i = 0; i < nv; ++i)
		pos[i] = V(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]);
	std::vector<uint32_t> tv(nt * 3);
	for (size_t t = 0; t < nt * 3; ++t) tv[t] = (uint32_t)geo[(size_t)mesh.triVerts[t]];

	                                    
	std::vector<std::vector<int32_t>> nbr(nv);
	for (size_t t = 0; t < nt; ++t)
		for (int e = 0; e < 3; ++e) {
			const int32_t a = (int32_t)tv[t * 3 + (size_t)e], b = (int32_t)tv[t * 3 + (size_t)((e + 1) % 3)];
			if (a != b) { nbr[(size_t)a].push_back(b); nbr[(size_t)b].push_back(a); }
		}
	for (auto& l : nbr) { std::sort(l.begin(), l.end()); l.erase(std::unique(l.begin(), l.end()), l.end()); }

	                                                                                      
	std::vector<int64_t> triOf(nv, -1);
	for (size_t t = 0; t < nt; ++t) for (int e = 0; e < 3; ++e) if (triOf[tv[t * 3 + (size_t)e]] < 0) triOf[tv[t * 3 + (size_t)e]] = (int64_t)t;
	const bool haveFace = mesh.faceID.size() == nt;
	const bool zones = p.zoneWeights.size() == (size_t)view.npts && view.pts != nullptr;
	const bool fieldMask = mask != nullptr && mask->Active();
	std::vector<double> weight(nv, 1.0);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
		const size_t i = (size_t)ii;
		if (geo[i] != (int32_t)i) return;
		double w = 1.0;
		const int64_t poly = haveFace && triOf[i] >= 0 ? (int64_t)mesh.faceID[(size_t)triOf[i]] : -1;
		if (poly >= 0 && poly < view.npolys) {
			if (p.avoid && in.avoidPoly && (size_t)poly < in.avoidPoly->size() && (*in.avoidPoly)[(size_t)poly]) w = 0.0;
			else if (zones) {
				const ProjectDamagePoly& q = view.polys[poly];
				const int32_t c[4] = { q.a, q.b, q.c, q.d };
				double sw = 0.0, sv = 0.0;
				for (int k = 0; k < (q.c == q.d ? 3 : 4); ++k) {
					const V d = pos[i] - view.pts[c[k]];
					const double iw = 1.0 / (ClayDot(d, d) + 1e-12 * R * R);
					sw += iw; sv += iw * p.zoneWeights[(size_t)c[k]];
				}
				w = sw > 0.0 ? ClayClamp01(sv / sw) : 1.0;
			}
		}
		if (fieldMask && w > 0.0) w *= ClayClamp01(mask->Weight(pos[i]));
        if(p.paintOnly) {
            double best=1e300;
            for(size_t j=0;j+5<p.paintSegments.size();j+=6) {
                const V a(p.paintSegments[j],p.paintSegments[j+1],p.paintSegments[j+2]);
                const V b(p.paintSegments[j+3],p.paintSegments[j+4],p.paintSegments[j+5]);
                const auto d=b-a;const double t=ClayClamp01(ClayDot(pos[i]-a,d)/std::max(ClayDot(d,d),1e-20));
                best=std::min(best,m::la::length(pos[i]-(a+d*t)));
            }
            w*=1.0-ClaySmooth01(best/std::max(p.paintRadius,1e-6));
        }
		weight[i] = w;
	});

	                                                                                      
	{
		const double s = ClayClamp01(p.softness / 100.0);
		const double refEdge = R / 140.0;
		const int passes = (int)std::min(900.0, std::round(220.0 * s * s * (refEdge / L) * (refEdge / L)));
		std::vector<V> tmp(nv);
		for (int it = 0; it < passes * 2; ++it) {
			const double f = (it & 1) ? -0.34 : 0.5;                                                             
			ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
				const size_t i = (size_t)ii;
				tmp[i] = pos[i];
				if (geo[i] != (int32_t)i || nbr[i].empty() || weight[i] <= 0.0) return;
				V avg(0.0);
				for (int32_t j : nbr[i]) avg += pos[(size_t)j];
				avg = avg / (double)nbr[i].size();
				tmp[i] = pos[i] + (avg - pos[i]) * (f * weight[i]);
			});
			pos.swap(tmp);
		}
	}

	                                                                                        
	std::vector<V> nrm(nv, V(0.0));
	std::vector<double> triArea(nt);
	for (size_t t = 0; t < nt; ++t) {
		const V& a = pos[tv[t * 3]]; const V& b = pos[tv[t * 3 + 1]]; const V& c = pos[tv[t * 3 + 2]];
		const V fn = ClayCross(b - a, c - a);
		triArea[t] = 0.5 * ClayLen(fn);
		for (int e = 0; e < 3; ++e) nrm[tv[t * 3 + (size_t)e]] += fn;
	}
	for (auto& n : nrm) n = ClayUnit(n, V(0.0));
	if (view.guideField) for (size_t i=0;i<nv;++i) nrm[i]=GuidedPointNormal(view,pos[i],nrm[i]);

	                                                                                        
	std::vector<ClayMark> marks;
	{
		std::vector<double> cdf(nt);
		double acc = 0.0;
		for (size_t t = 0; t < nt; ++t) { acc += triArea[t]; cdf[t] = acc; }
		const double total = acc;
		const uint64_t seed = ClayMix(((uint64_t)(uint32_t)mainSeed << 32) ^ (uint64_t)(uint32_t)p.seed ^ 0xC1A7C1A7ull);
		auto place = [&](bool smear, double coverage, double aspect) {
			const double r0 = std::max(1e-6, R * p.pressSize / 100.0) * (smear ? 0.55 : 1.0);
			const double one = kClayPi * r0 * r0 * aspect;
			const int count = (int)std::min(20000.0, std::round(coverage * total / one));
			for (int i = 0; i < count; ++i) {
				const uint64_t h = ClayMix(seed ^ ClayMix(((uint64_t)smear << 40) + (uint64_t)i));
				auto rnd = [&](int ch) { return ClayU01(ClayMix(h + (uint64_t)ch * 0x9E37ull)); };
				const double pick = rnd(0) * total;
				const size_t t = (size_t)(std::lower_bound(cdf.begin(), cdf.end(), pick) - cdf.begin());
				if (t >= nt) continue;
				double a = rnd(1), b = rnd(2);
				if (a + b > 1.0) { a = 1.0 - a; b = 1.0 - b; }
				const uint32_t i0 = tv[t * 3], i1 = tv[t * 3 + 1], i2 = tv[t * 3 + 2];
				ClayMark k;
				k.c = pos[i0] * (1.0 - a - b) + pos[i1] * a + pos[i2] * b;
				const double wHere = weight[i0] * (1.0 - a - b) + weight[i1] * a + weight[i2] * b;
				if (rnd(9) > wHere) continue;                                                             
				k.n = ClayUnit(nrm[i0] * (1.0 - a - b) + nrm[i1] * a + nrm[i2] * b, V(0, 0, 1));
				const V helper = std::fabs(k.n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0);
				const V t0 = ClayUnit(ClayCross(k.n, helper), V(1, 0, 0)), t1 = ClayCross(k.n, t0);
				const double ang = rnd(3) * 2.0 * kClayPi;
				k.u = t0 * std::cos(ang) + t1 * std::sin(ang);
				k.v = ClayCross(k.n, k.u);
				k.r = r0 * (0.7 + 0.6 * rnd(4));
				k.aspect = smear ? aspect * (0.75 + 0.5 * rnd(5)) : 1.0 + 0.45 * rnd(5);
				k.depth = k.r * (p.pressDepth / 100.0) * (smear ? 0.35 : 0.5) * (0.7 + 0.6 * rnd(6));
				k.rim = ClayClamp01(p.pressRim / 100.0) * (smear ? 1.4 : 1.0);
				                                                                              
				const double period = k.r / 13.0;
				k.prints = smear ? 0.0 : ClayClamp01(p.fingerprints / 100.0) * ClaySmooth01((period - 2.0 * L) / (3.0 * L));
				k.whorl = V((rnd(7) - 0.5) * 0.5, (rnd(8) - 0.5) * 0.5, 0.0);
				k.smear = smear;
				marks.push_back(k);
			}
		};
		place(false, 1.8 * ClayClamp01(p.presses / 100.0), 1.0);
		place(true, 1.2 * ClayClamp01(p.smears / 100.0), std::max(1.0, std::min(8.0, p.smearLength)));
	}
	ClayMarkGrid grid;
	if (!marks.empty()) grid.Build(marks);
	double maxDepth = 0.0;
	for (const ClayMark& k : marks) maxDepth = std::max(maxDepth, k.depth);

	                                                                                        
	                                          
	                                                                         
	                                                                       
	                                                                           
	                                         
	                                                                             
	                                                                        
	                                                                     
	                                                                         
	                                                    
	ProjectDamageBVH bvh;
	const double cap = ClayClamp01(p.thicknessCap);
	const bool haveBvh = cap > 0.0 && view.pts && view.polys && view.npolys > 0;
	if (haveBvh) bvh.Build(view);
	const double lumpAmp = 0.015 * R * ClayClamp01(p.lumps / 100.0);
	const double lumpF = 3.0 * std::max(1e-3, p.lumpScale) / R;
	const double grainAmp = 0.0012 * R * ClayClamp01(p.grain / 100.0);
	const double grainF = 70.0 * std::max(1e-3, p.grainScale) / R;
	const double crackCov = ClayClamp01(p.cracks / 100.0);
	const double crackW = 0.006 * R * (0.2 + 1.6 * ClayClamp01(p.crackWidth / 100.0));
	const double crackD = 0.008 * R * (0.2 + 1.6 * ClayClamp01(p.crackDepth / 100.0));
	const uint64_t nSalt = (uint64_t)(uint32_t)p.seed * 977u + (uint64_t)(uint32_t)mainSeed;
	const double floorD = 1.6 * maxDepth;                                                      

	                                                                            
	std::vector<double> kConvex(nv, 0.0), kConcave(nv, 0.0);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
		const size_t i = (size_t)ii;
		if (geo[i] != (int32_t)i) return;
		for (int32_t j : nbr[i]) {
			const V e = pos[(size_t)j] - pos[i];
			const double l2 = ClayDot(e, e);
			if (l2 <= 0.0) continue;
			const double k = 2.0 * ClayDot(e, nrm[i]) / l2;                                            
			if (k > 0.0) kConcave[i] = std::max(kConcave[i], k); else kConvex[i] = std::max(kConvex[i], -k);
		}
	});

	                                                                      
	                                                                      
	                                                                        
	                       
	auto spreadK = [&](std::vector<double>& k) {
		std::vector<double> t = k;
		for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i) for (int32_t j : nbr[i]) t[i] = std::max(t[i], k[(size_t)j]);
		for (int pass = 0; pass < 2; ++pass) {
			std::vector<double> u = t;
			for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i && !nbr[i].empty()) {
				double sum = t[i];
				for (int32_t j : nbr[i]) sum += t[(size_t)j];
				u[i] = std::max(t[i] * 0.5, sum / (double)(nbr[i].size() + 1));
			}
			t.swap(u);
		}
		k.swap(t);
	};
	spreadK(kConvex); spreadK(kConcave);
	std::vector<double> limIn(nv, 1e30), limOut(nv, 1e30);
	for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i) {
		if (kConvex[i] > 0.0) limIn[i] = 0.4 / kConvex[i];
		if (kConcave[i] > 0.0) limOut[i] = 0.4 / kConcave[i];
	}
	                                                                      
	auto softLimit = [](double d, double lim) { return lim >= 1e29 ? d : lim * std::tanh(d / lim); };

	std::vector<V> dispV(nv, V(0.0));
	std::vector<double> dScalar(nv, 0.0);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
		const size_t i = (size_t)ii;
		if (geo[i] != (int32_t)i || weight[i] <= 0.0) return;
		const V n = nrm[i];
		if (ClayDot(n, n) == 0.0) return;
		const V x = pos[i];
		                                         
		double d = 0.0;
		if (lumpAmp > 0.0) d += lumpAmp * ClayFbm(x * lumpF, 2, 0.45, 2.1, 11u + nSalt);
		if (grainAmp > 0.0) d += grainAmp * ClayFbm(x * grainF, 2, 0.5, 2.3, 23u + nSalt);
		if (crackCov > 0.0) {
			const double cov = 0.5 + 0.5 * ClayFbm(x * (2.0 / R), 2, 0.5, 2.0, 37u + nSalt);
			const double gate = ClaySmooth01((cov - (1.0 - crackCov)) / 0.12);
			if (gate > 0.0) {
				const double e = ClayVoronoiEdge(x * (9.0 / R) + V((double)(nSalt % 97u) * 0.37)) * (R / 9.0);
				const double jag = 1.0 + 0.5 * ClayFbm(x * (40.0 / R), 1, 0.5, 2.0, 41u + nSalt);
				if (e < crackW * jag) d -= crackD * gate * (1.0 - e / (crackW * jag));
			}
		}
		                                                                                
		if (!marks.empty()) {
			double m = 0.0;
			grid.Visit(x, [&](int32_t id) { m += ClayMarkHeight(marks[(size_t)id], x, n); });
			d += m < 0.0 && floorD > 0.0 ? -floorD * std::tanh(-m / floorD) : m;
		}
		d *= weight[i];
		                                                                          
		                                                                          
		d = d < 0.0 ? -softLimit(-d, limIn[i]) : softLimit(d, limOut[i]);
		if (haveBvh && d < 0.0) {
			const double eps = 1e-4 * R;
			double hit = 0.0;
			if (bvh.Raycast(x - n * eps, -n, -d / cap, &hit)) d = std::max(d, -cap * (hit + eps));
		}
		dScalar[i] = d;
	});
	                                                                         
	                                                                         
	                                                                       
	              
	for (int pass = 0; pass < 2; ++pass) {
		std::vector<double> next = dScalar;
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
			const size_t i = (size_t)ii;
			if (geo[i] != (int32_t)i || nbr[i].empty()) return;
			double sum = 0.0;
			for (int32_t j : nbr[i]) sum += dScalar[(size_t)j];
			next[i] = 0.5 * dScalar[i] + 0.5 * sum / (double)nbr[i].size();
		});
		dScalar.swap(next);
	}
	for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i) dispV[i] = nrm[i] * dScalar[i];

	                                                                                           
	const double sag = ClayClamp01(p.sag / 100.0);
	const V gDir = ClayUnit(V(p.gravX, p.gravY, p.gravZ), V(0, 0, -1));
	std::vector<V> sagV(nv, V(0.0));
	if (sag > 0.0) {
		std::vector<int32_t> par(nv); std::iota(par.begin(), par.end(), 0);
		auto find = [&](int32_t a) { while (par[(size_t)a] != a) { par[(size_t)a] = par[(size_t)par[(size_t)a]]; a = par[(size_t)a]; } return a; };
		for (size_t t = 0; t < nt; ++t) for (int e = 0; e < 2; ++e) {
			const int32_t a = find((int32_t)tv[t * 3 + (size_t)e]), b = find((int32_t)tv[t * 3 + (size_t)e + 1]);
			if (a != b) par[(size_t)std::max(a, b)] = std::min(a, b);
		}
		struct Piece { double lo = 1e300, hi = -1e300; V sum{ 0.0 }; double cnt = 0.0; };
		std::unordered_map<int32_t, Piece> pieces;
		for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i) {
			Piece& pc = pieces[find((int32_t)i)];
			const double hgt = -ClayDot(pos[i], gDir);
			pc.lo = std::min(pc.lo, hgt); pc.hi = std::max(pc.hi, hgt); pc.sum += pos[i]; pc.cnt += 1.0;
		}
		const double bul = ClayClamp01(p.bulge / 100.0);
		for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i && weight[i] > 0.0) {
			const Piece& pc = pieces[find((int32_t)i)];
			const double H = std::max(1e-9, pc.hi - pc.lo);
			const double t = ClayClamp01((-ClayDot(pos[i], gDir) - pc.lo) / H);
			const V axis = pc.sum / pc.cnt;
			V radial = pos[i] - axis; radial = radial - gDir * ClayDot(radial, gDir);
			const double w = weight[i];
			sagV[i] = gDir * (sag * 0.22 * H * t * t * w) + radial * (sag * bul * 0.2 * (1.0 - t) * (1.0 - t) * w);
		}
	}

	                                                                                            
	                                                                             
	                                                                          
	                                                                          
	                                                                          
	                                                                            
	size_t folded = 0;
	for (int it = 0; it < 40; ++it) {
		std::vector<uint8_t> squeezed(nv, 0);
		std::atomic<size_t> count{ 0 };
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
			const size_t i = (size_t)ii;
			if (geo[i] != (int32_t)i) return;
			const V xi = pos[i] + dispV[i] + sagV[i];
			for (int32_t j : nbr[i]) {
				const V e0 = pos[(size_t)j] - pos[i];
				const V e1 = pos[(size_t)j] + dispV[(size_t)j] + sagV[(size_t)j] - xi;
				if (ClayDot(e1, e0) < 0.35 * ClayDot(e0, e0)) { squeezed[i] = 1; count++; return; }
			}
		});
		if (count == 0) break;
		if (it == 0) folded = count;
		std::vector<V> next = dispV;
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
			const size_t i = (size_t)ii;
			if (!squeezed[i] || nbr[i].empty()) return;
			V avg(0.0);
			for (int32_t j : nbr[i]) avg += dispV[(size_t)j];
			avg = avg / (double)nbr[i].size();
			next[i] = dispV[i] * 0.4 + avg * 0.6;
		});
		dispV.swap(next);
	}

	                                                                                             
	std::vector<double> scale(nv, 1.0);
	std::vector<V> fin(nv);
	auto build = [&]() {
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
			const size_t i = (size_t)ii;
			if (geo[i] == (int32_t)i) fin[i] = pos[i] + dispV[i] * scale[i] + sagV[i];
		});
	};
	                                                                       
	                                                                    
	const double cell = 2.5 * L;
	std::vector<V> triN0(nt);
	for (size_t t = 0; t < nt; ++t)
		triN0[t] = ClayCross(pos[tv[t * 3 + 1]] - pos[tv[t * 3]], pos[tv[t * 3 + 2]] - pos[tv[t * 3]]);
	auto findBad = [&](std::vector<uint8_t>& bad) -> size_t {
		std::fill(bad.begin(), bad.end(), (uint8_t)0);
		auto key = [](int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576); };
		std::unordered_map<uint64_t, std::vector<int32_t>> cells;
		cells.reserve(nt);
		std::vector<V> lo(nt), hi(nt);
		for (size_t t = 0; t < nt; ++t) {
			const V& a = fin[tv[t * 3]]; const V& b = fin[tv[t * 3 + 1]]; const V& c = fin[tv[t * 3 + 2]];
			lo[t] = m::la::min(a, m::la::min(b, c)); hi[t] = m::la::max(a, m::la::max(b, c));
			for (int z = (int)std::floor(lo[t].z / cell); z <= (int)std::floor(hi[t].z / cell); ++z)
				for (int y = (int)std::floor(lo[t].y / cell); y <= (int)std::floor(hi[t].y / cell); ++y)
					for (int x = (int)std::floor(lo[t].x / cell); x <= (int)std::floor(hi[t].x / cell); ++x)
						cells[key(x, y, z)].push_back((int32_t)t);
		}
		std::vector<uint8_t> triBad(nt, 0);
		ProjectDamageParallelFor(0, (int64_t)nt, [&](int64_t ti) {
			const size_t t = (size_t)ti;
			const uint32_t* a = &tv[t * 3];
			const V A[3] = { fin[a[0]], fin[a[1]], fin[a[2]] };
			if (ClayDot(ClayCross(A[1] - A[0], A[2] - A[0]), triN0[t]) <= 0.0) triBad[t] = 1;
			for (int z = (int)std::floor(lo[t].z / cell); z <= (int)std::floor(hi[t].z / cell); ++z)
				for (int y = (int)std::floor(lo[t].y / cell); y <= (int)std::floor(hi[t].y / cell); ++y)
					for (int x = (int)std::floor(lo[t].x / cell); x <= (int)std::floor(hi[t].x / cell); ++x) {
						auto it = cells.find(key(x, y, z));
						if (it == cells.end()) continue;
						for (int32_t o : it->second) {
							if ((size_t)o <= t) continue;
							if (lo[(size_t)o].x > hi[t].x || hi[(size_t)o].x < lo[t].x || lo[(size_t)o].y > hi[t].y || hi[(size_t)o].y < lo[t].y
								|| lo[(size_t)o].z > hi[t].z || hi[(size_t)o].z < lo[t].z) continue;
							const uint32_t* b = &tv[(size_t)o * 3];
							bool shared = false;
							for (int u = 0; u < 3 && !shared; ++u) for (int v = 0; v < 3; ++v) if (a[u] == b[v]) { shared = true; break; }
							if (shared) continue;
							const V B[3] = { fin[b[0]], fin[b[1]], fin[b[2]] };
							if (ClayTriTri(A, B)) { triBad[t] = 1; triBad[(size_t)o] = 1; }
						}
					}
		});
		size_t count = 0;
		for (size_t t = 0; t < nt; ++t) if (triBad[t]) { ++count; for (int e = 0; e < 3; ++e) bad[tv[t * 3 + (size_t)e]] = 1; }
		return count;
	};
	auto grow = [&](std::vector<uint8_t>& mark) {                    
		std::vector<uint8_t> next = mark;
		for (size_t i = 0; i < nv; ++i) if (mark[i]) for (int32_t j : nbr[i]) next[(size_t)j] = 1;
		mark.swap(next);
	};
	build();
	std::vector<uint8_t> bad(nv, 0);
	size_t crossing = findBad(bad), firstCrossing = crossing;
	int rounds = 0;
	for (; crossing > 0 && rounds < 16; ++rounds) {
		                                                                         
		                                                                       
		                                                                       
		                            
		const bool last = rounds >= 9;
		std::vector<uint8_t> zone = bad;
		for (int g = 0; g < 3; ++g) grow(zone);
		for (int pass = 0; pass < 6; ++pass) {
			std::vector<V> next = dispV;
			ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
				const size_t i = (size_t)ii;
				if (!zone[i] || geo[i] != (int32_t)i || nbr[i].empty()) return;
				V avg(0.0);
				for (int32_t j : nbr[i]) avg += dispV[(size_t)j];
				next[i] = dispV[i] * 0.5 + avg * (0.5 / (double)nbr[i].size());
			});
			dispV.swap(next);
		}
		for (size_t i = 0; i < nv; ++i) if (bad[i]) dispV[i] = dispV[i] * (last ? 0.0 : 0.6);
		build();
		crossing = findBad(bad);
	}
	for (int extra = 0; crossing > 0 && extra < 6; ++extra) {
		                                                                      
		                                                                   
		            
		std::vector<uint8_t> zone = bad;
		for (int g = 0; g < 3 + 2 * extra; ++g) grow(zone);
		for (size_t i = 0; i < nv; ++i) if (zone[i]) dispV[i] = V(0.0);
		build();
		crossing = findBad(bad);
	}
	for (size_t i = 0; i < nv; ++i) if (geo[i] == (int32_t)i) pos[i] = fin[i];

	                                                                                          
	for (size_t i = 0; i < nv; ++i) {
		const V& q = pos[(size_t)geo[i]];
		mesh.vertProperties[i * np] = q.x;
		mesh.vertProperties[i * np + 1] = q.y;
		mesh.vertProperties[i * np + 2] = q.z;
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0) {
		if (log) *log = "[ProjectDamage] Clay: the deformed mesh failed Manifold's checks - skipped (lower Presses depth or raise Thickness Cap)";
		return false;
	}
	out = std::move(result);
	if (log) {
		*log = "[ProjectDamage] Clay: " + std::to_string(nt) + " triangles, " + std::to_string(marks.size()) + " hand marks";
		if (folded)
			*log += ", " + std::to_string(folded) + " squeezed points relaxed";
		if (firstCrossing)
			*log += ", " + std::to_string(firstCrossing) + " crossing triangles relaxed in " + std::to_string(rounds) + " passes"
				+ (crossing ? " (" + std::to_string(crossing) + " left)" : std::string());
	}
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
