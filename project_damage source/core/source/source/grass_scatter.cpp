// распределяем травинки по подходящим местам поверхности
// ProjectDamage grass scatter — GPL-3.0-or-later; see LICENSE.
//
// Recipe grass-v1 (GrassParams): dry (or fresh) grass shards on the ground,
// gathering where real debris gathers.
//   eligible  faces no steeper than Slope Limit against Up: no grass on walls
//   density   per point = Base
//               + Edges  x falloff(distance to the nearest feature edge -
//                          corners sharper than Edge Angle and open borders:
//                          the foot of a wall or kerb, crack and chip rims)
//               + Cavity x the share of 12 rays into the upper hemisphere
//                          blocked within Cavity Radius (cracks, pits,
//                          hollows: the deeper the pocket, the denser)
//             shards per unit area; candidates are scattered by area and kept
//             with probability density / peak
//   strokes   Grease Pencil, dropped on the ground: within Stroke Radius the
//             density is Stroke Density. Overlay = grass only there; Add =
//             the strokes add grass where the field is thin, the rest stays
//   blades    flat ribbons tapering to a point, lying almost flat at a random
//             yaw, bent along and across, a share broken short or folded;
//             piles: a little height where it is dense. Dry: thin, long,
//             curled, many broken pieces, flat; Fresh: wider, straighter,
//             fewer pieces, the tip lifted a little.
//             UV: U across 0..1, V base 0 .. tip 1; variation per blade 0..1,
//             tip per point 0..1.
#include "grass_scatter.h"
#include "project_damage_bvh.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = EVec;
double Len(const V& v) { return m::la::length(v); }
V Unit(const V& v, const V& fb) { const double l = Len(v); return l > 1e-12 ? v / l : fb; }
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL) {}
	double U() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0); }
};
double SegDist(const V& p, const V& a, const V& b)
{
	const V ab = b - a;
	const double l2 = m::la::dot(ab, ab);
	const double t = l2 > 1e-18 ? std::clamp(m::la::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
	return Len(p - (a + ab * t));
}
                                                                         
struct SegGrid
{
	std::vector<std::pair<V, V>> segs;
	V lo; double cell = 1.0; int nx = 1, ny = 1, nz = 1;
	std::vector<std::vector<int>> bins;
	void Build(const V& l, const V& h, double c)
	{
		lo = l - V(c); cell = std::max(1e-9, c);
		const V ext = h - l + V(2 * c);
		nx = std::clamp((int)std::ceil(ext.x / cell), 1, 256); ny = std::clamp((int)std::ceil(ext.y / cell), 1, 256); nz = std::clamp((int)std::ceil(ext.z / cell), 1, 256);
		bins.assign((size_t)nx * ny * nz, {});
		for (size_t i = 0; i < segs.size(); ++i)
		{
			const V a = segs[i].first, b = segs[i].second;
			int i0[3], i1[3];
			Cell(m::la::min(a, b) - V(c), i0); Cell(m::la::max(a, b) + V(c), i1);
			for (int x = i0[0]; x <= i1[0]; ++x) for (int y = i0[1]; y <= i1[1]; ++y) for (int z = i0[2]; z <= i1[2]; ++z)
				bins[((size_t)x * ny + y) * nz + z].push_back((int)i);
		}
	}
	void Cell(const V& p, int* o) const
	{
		o[0] = std::clamp((int)std::floor((p.x - lo.x) / cell), 0, nx - 1);
		o[1] = std::clamp((int)std::floor((p.y - lo.y) / cell), 0, ny - 1);
		o[2] = std::clamp((int)std::floor((p.z - lo.z) / cell), 0, nz - 1);
	}
	double Nearest(const V& p, double maxD) const
	{
		if (segs.empty()) return 1e300;
		int c[3]; Cell(p, c);
		double best = maxD;
		for (int x = std::max(0, c[0] - 1); x <= std::min(nx - 1, c[0] + 1); ++x)
			for (int y = std::max(0, c[1] - 1); y <= std::min(ny - 1, c[1] + 1); ++y)
				for (int z = std::max(0, c[2] - 1); z <= std::min(nz - 1, c[2] + 1); ++z)
					for (int i : bins[((size_t)x * ny + y) * nz + z]) best = std::min(best, SegDist(p, segs[(size_t)i].first, segs[(size_t)i].second));
		return best;
	}
};

}               

// основной вход: собираем геометрию по переданным настройкам
bool ScatterGrass(const MeshView& mv, const std::vector<std::vector<EVec>>& strokes, const GrassParams& g, GrassResult& r)
{
	r = GrassResult();
	if (mv.npolys <= 0) return false;
	const V up = Unit(V(g.upX, g.upY, g.upZ), V(0, 0, 1));
	const double cosSlope = std::cos(std::clamp(g.slopeLimit, 0.0, 89.9) * m::kPi / 180.0);
	                                      
	struct Tri { V a, b, c, n; double area; V guide; };
	std::vector<Tri> tris;
	std::vector<V> fn((size_t)mv.npolys);
	V lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < mv.npts; ++i) { lo = m::la::min(lo, V(mv.pts[i])); hi = m::la::max(hi, V(mv.pts[i])); }
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		V s(0.0);
		for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k) s += m::la::cross(V(mv.pts[ids[k]]) - V(mv.pts[ids[0]]), V(mv.pts[ids[k + 1]]) - V(mv.pts[ids[0]]));
		fn[(size_t)i] = GuidedNormal(mv, i, Unit(s, V(0, 0, 1)));
		if (m::la::dot(fn[(size_t)i], up) < cosSlope) continue;
		for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k)
		{
			const V A = mv.pts[ids[0]], B = mv.pts[ids[k]], C = mv.pts[ids[k + 1]];
			const V cr = m::la::cross(B - A, C - A);
			tris.push_back({ A, B, C, Unit(cr, fn[(size_t)i]), 0.5 * Len(cr), GuidedNormal(mv, i, Unit(cr, fn[(size_t)i])) });
		}
	}
	if (tris.empty()) return false;
	                                                
	SegGrid edges;
	{
		std::map<std::pair<Int32, Int32>, std::vector<Int32>> ef;
		for (Int32 i = 0; i < mv.npolys; ++i)
		{
			const ProjectDamagePoly& q = mv.polys[i];
			const Int32 ids[4] = { q.a, q.b, q.c, q.d };
			const int nv = q.c == q.d ? 3 : 4;
			for (int k = 0; k < nv; ++k) { const Int32 a = ids[k], b = ids[(k + 1) % nv]; ef[{ std::min(a, b), std::max(a, b) }].push_back(i); }
		}
		const double cosEdge = std::cos(std::clamp(g.edgeAngle, 1.0, 179.0) * m::kPi / 180.0);
		for (const auto& e : ef)
			if (e.second.size() != 2 || m::la::dot(fn[(size_t)e.second[0]], fn[(size_t)e.second[1]]) < cosEdge)
				edges.segs.push_back({ V(mv.pts[e.first.first]), V(mv.pts[e.first.second]) });
		edges.Build(lo, hi, std::max(1e-6, g.edgeRadius));
	}
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	                                                    
	std::vector<V> dirs;
	for (int i = 0; i < 12; ++i)
	{
		const double el = (i < 6 ? 25.0 : 55.0) * m::kPi / 180.0, az = (i % 6) * m::kPi / 3.0 + (i < 6 ? 0.0 : m::kPi / 6.0);
		dirs.push_back(V(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)));
	}
	const V ax0 = Unit(m::la::cross(up, std::abs(up.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
	const V ax1 = m::la::cross(up, ax0);
	auto cavity = [&](const V& p, const V& n) {
		if (g.cavityDensity <= 0.0) return 0.0;
		int blocked = 0;
		const V o = p + n * (1e-4 * g.cavityRadius + 1e-6);
		for (const V& d0 : dirs)
		{
			const V d = Unit(ax0 * d0.x + ax1 * d0.y + up * d0.z, up);
			double dist = 0.0;
			if (bvh.Raycast(o, d, g.cavityRadius, &dist)) ++blocked;
		}
		return std::clamp((blocked / 12.0 - 0.15) / 0.85, 0.0, 1.0);                               
	};
	                                                          
	std::vector<std::vector<V>> st;
	for (const auto& s : strokes)
	{
		std::vector<V> line;
		for (const V& p : s)
		{
			double best = 1e300; V q = p;
			for (const Tri& t : tris)
			{
				                                                                         
				const V pp = p - t.n * m::la::dot(p - t.a, t.n);
				const V v0 = t.b - t.a, v1 = t.c - t.a, v2 = pp - t.a;
				const double d00 = m::la::dot(v0, v0), d01 = m::la::dot(v0, v1), d11 = m::la::dot(v1, v1), d20 = m::la::dot(v2, v0), d21 = m::la::dot(v2, v1), den = d00 * d11 - d01 * d01;
				if (std::abs(den) < 1e-18) continue;
				double bv = std::clamp((d11 * d20 - d01 * d21) / den, 0.0, 1.0), bw = std::clamp((d00 * d21 - d01 * d20) / den, 0.0, 1.0 - bv);
				const V c = t.a + v0 * bv + v1 * bw;
				const double d = Len(c - p);
				if (d < best) { best = d; q = c; }
			}
			line.push_back(q);
		}
		if (line.size() >= 2) st.push_back(std::move(line));
	}
	auto strokeHit = [&](const V& p) {
		for (const auto& l : st)
			for (size_t i = 0; i + 1 < l.size(); ++i)
				if (SegDist(p, l[i], l[i + 1]) < g.strokeRadius) return true;
		return false;
	};
	const bool overlay = g.strokeMode == 0 && !st.empty();
	                                                           
	std::vector<double> cdf;
	double area = 0.0;
	for (const Tri& t : tris) { area += t.area; cdf.push_back(area); }
	const double peak = std::max({ g.baseDensity + g.edgeDensity + g.cavityDensity, g.strokeDensity, 1e-9 });
	const size_t want = (size_t)std::min<double>((double)g.maxShards * 4.0, area * peak);
	Rng rng((uint64_t)(uint32_t)g.seed + 17);
	struct Shard { V p, n; double density; };
	std::vector<Shard> shards;
	for (size_t c = 0; c < want && shards.size() < (size_t)g.maxShards; ++c)
	{
		const size_t ti = (size_t)(std::upper_bound(cdf.begin(), cdf.end(), rng.U() * area) - cdf.begin());
		if (ti >= tris.size()) continue;
		double u = rng.U(), v = rng.U();
		if (u + v > 1) { u = 1 - u; v = 1 - v; }
		const Tri& t = tris[ti];
		const V p = t.a + (t.b - t.a) * u + (t.c - t.a) * v;
		const bool inStroke = !st.empty() && strokeHit(p);
		double d = 0.0;
		if (!overlay)
		{
			const double de = g.edgeDensity > 0.0 ? std::pow(std::max(0.0, 1.0 - edges.Nearest(p, g.edgeRadius) / std::max(1e-9, g.edgeRadius)), 1.5) : 0.0;
			d = g.baseDensity + g.edgeDensity * de + g.cavityDensity * cavity(p, t.n);
			if (inStroke) d = std::max(d, g.strokeDensity);                               
		}
		else if (inStroke) d = g.strokeDensity;
		if (rng.U() * peak >= d) continue;
		shards.push_back({ p, t.guide, d });
	}
	                                                                              
	                                                                          
	if (g.shape == 1)
	{
		const double ph = (1.0 + std::sqrt(5.0)) * 0.5;
		const V ico[12] = { V(-1, ph, 0), V(1, ph, 0), V(-1, -ph, 0), V(1, -ph, 0), V(0, -1, ph), V(0, 1, ph), V(0, -1, -ph), V(0, 1, -ph),
			V(ph, 0, -1), V(ph, 0, 1), V(-ph, 0, -1), V(-ph, 0, 1) };
		const int faces[20][3] = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
			{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
		for (size_t si = 0; si < shards.size(); ++si)
		{
			const Shard& s = shards[si];
			const double var = rng.U();
			const double gather = std::clamp((s.density - g.baseDensity) / std::max(1e-9, peak - g.baseDensity), 0.0, 1.0);
			const double size = (g.stoneMin + (g.stoneMax - g.stoneMin) * std::pow(rng.U(), std::max(0.1, g.stoneSkew))) * (1.0 - g.sorting * gather);
			const V n = s.n;
			const V t0 = Unit(m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
			const V t1 = m::la::cross(n, t0);
			const double yaw = rng.U() * 2 * m::kPi, tilt = (rng.U() - 0.5) * 0.5;
			V ax = t0 * std::cos(yaw) + t1 * std::sin(yaw), ay = m::la::cross(n, ax), az = n;
			ax = Unit(ax * std::cos(tilt) + az * std::sin(tilt), ax); az = Unit(m::la::cross(ax, ay), az);
			const double sx = 0.5 * size * (0.75 + 0.5 * rng.U()), sy = 0.5 * size * (0.75 + 0.5 * rng.U());
			const double sz = 0.5 * size * g.flatness * (0.8 + 0.4 * rng.U());
			const V centre = s.p + n * (sz * (1.0 - 2.0 * std::clamp(g.embed, 0.0, 0.95)) + g.pileHeight * gather * rng.U());
			const double uo = rng.U(), vo = rng.U();
			const uint32_t first = (uint32_t)r.points.size();
			for (int k = 0; k < 12; ++k)
			{
				const V d = Unit(ico[k], V(0, 0, 1));
				const double j = 1.0 + g.angularity * (rng.U() * 2.0 - 1.0);
				const V q = centre + (ax * (d.x * sx) + ay * (d.y * sy) + az * (d.z * sz)) * j;
				r.points.push_back(q);
				r.uvs.push_back(std::atan2(d.y, d.x) / (2 * m::kPi) + 0.5 + uo);
				r.uvs.push_back(0.5 + 0.5 * d.z + vo);
				r.variation.push_back((float)var);
				r.tip.push_back((float)(0.5 + 0.5 * d.z));                                        
			}
			for (const auto& f : faces)
				r.polys.push_back({ (Int32)(first + f[0]), (Int32)(first + f[1]), (Int32)(first + f[2]), (Int32)(first + f[2]) });
			++r.shards;
		}
		return r.shards > 0;
	}
	                 
	const bool fresh = g.fresh;
	const int segs = std::clamp(g.segments, 2, 8);
	for (size_t si = 0; si < shards.size(); ++si)
	{
		const Shard& s = shards[si];
		const double var = rng.U();
		                                          
		double len = g.lengthMin + (g.lengthMax - g.lengthMin) * std::pow(rng.U(), fresh ? 0.8 : 1.4);
		const bool broken = rng.U() < (fresh ? 0.15 : 0.45);
		if (broken) len *= 0.25 + 0.4 * rng.U();
		const double width = g.width * (0.6 + 0.8 * rng.U()) * (fresh ? 1.6 : 1.0);
		const bool fold = !fresh && rng.U() < 0.18;
		                                                                    
		const V n = s.n;
		const V t0 = Unit(m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
		const V t1 = m::la::cross(n, t0);
		const double yaw = rng.U() * 2 * m::kPi;
		const V dir = t0 * std::cos(yaw) + t1 * std::sin(yaw), side = m::la::cross(n, dir);
		const double lift = g.pileHeight * std::min(1.0, s.density / peak) * rng.U() + 0.15 * width;
		const double bendAlong = (rng.U() - 0.5) * (fresh ? 0.6 : 1.6);                                             
		const double bendUp = (fresh ? 0.25 : 0.08) * rng.U();                       
		const double twist = (rng.U() - 0.5) * (fresh ? 0.4 : 1.2);
		const V base = s.p + n * lift - dir * (0.3 * len);
		const uint32_t first = (uint32_t)r.points.size();
		V cur = base, d = dir;
		for (int k = 0; k <= segs; ++k)
		{
			const double tt = (double)k / segs;
			                                                    
			if (fold && k == (int)std::round(segs * 0.55)) d = Unit(d * -0.7 + side * (rng.U() - 0.5), d);
			const double halfW = 0.5 * width * (fresh ? std::pow(1.0 - tt, 0.6) : (1.0 - 0.85 * tt)) + 1e-6;
			const double tw = twist * tt;
			const V across = Unit(side * std::cos(tw) + n * std::sin(tw), side);
			r.points.push_back(cur - across * halfW);
			r.points.push_back(cur + across * halfW);
			r.uvs.insert(r.uvs.end(), { 0.0, tt, 1.0, tt });
			r.variation.insert(r.variation.end(), { (float)var, (float)var });
			r.tip.insert(r.tip.end(), { (float)tt, (float)tt });
			                                                                             
			const double a = bendAlong / segs;
			d = Unit(d * std::cos(a) + side * std::sin(a) + n * (bendUp / segs) - n * (0.02 * tt), d);
			cur = cur + d * (len / segs);
		}
		for (int k = 0; k < segs; ++k)
		{
			const uint32_t a = first + 2 * (uint32_t)k;
			r.polys.push_back({ (Int32)a, (Int32)(a + 1), (Int32)(a + 3), (Int32)(a + 2) });
		}
		++r.shards;
	}
	return r.shards > 0;
}

PROJECT_DAMAGE_NS_MACRO_END
