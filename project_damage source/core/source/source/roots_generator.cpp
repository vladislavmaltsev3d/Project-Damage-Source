// растим корни по поверхности и заданным направлениям
// ProjectDamage roots generator — GPL-3.0-or-later; see LICENSE.
//
// Built on the ivy growth model (ivy_generator.cpp), but roots never leave the
// surface: every root grows node by node from a seed, the step a normalized
// blend of the primary direction (a running average of past steps), a random
// direction, DOWNHILL (world down projected onto the surface: nothing on flat
// ground, full on a wall), SPREAD (away from its seed) and the pull of the
// nearest Grease Pencil / curve guide, projected onto the surface's tangent
// plane; the new point is snapped back onto the surface, so roots crawl over
// any shape and wrap around edges, and a step that runs into the surface
// (an inside corner, a rock) turns to climb it. Each seed starts Roots per Seed main roots
// radiating evenly around it. A root stops at its length (Root Length +-
// Length Random); a living root forks with Branching Probability per step at
// Branch Angle, for up to Max Levels generations, each child Child Length of
// the parent's remaining length and Child Radius of its radius there.
// Birth: the path is smoothed, put back onto the surface and, over outside
// edges, refined with points snapped between the floating positions, so the
// tube wraps the edge instead of cutting the chord; the tube's axis is then
// smoothed wherever that keeps it out of the surface (inside corners).
// One tube per root on that path, its section flattened along
// the surface normal and sunk by Embed (0 = lying on the surface, 1 = buried);
// radius tapering base -> tip ((1 - s / L)^Taper down to Tip Radius). A main
// root's first Flare share rises out of the ground and thickens - the
// buttress where it leaves the trunk. UVs as on the ivy branches: U around, V
// along in the same metric scale.
#include "roots_generator.h"
#include "project_damage_bvh.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
	double U()
	{
		s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
		return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
	}
	V Dir() { for (;;) { V v(U() * 2 - 1, U() * 2 - 1, U() * 2 - 1); const double l = Len(v); if (l > 1e-3 && l <= 1.0) return v / l; } }
};

                                                                              
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const V bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	const V cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

struct TriGrid
{
	std::vector<V> a, b, c, n;
	V lo, cell;
	int nx = 1, ny = 1, nz = 1;
	std::vector<std::vector<int>> bins;
	void Build(const MeshView& mv)
	{
		for (Int32 i = 0; i < mv.npolys; ++i)
		{
			const ProjectDamagePoly& q = mv.polys[i];
			const Int32 ids[4] = { q.a, q.b, q.c, q.d };
			for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k)
			{
				const V A = mv.pts[ids[0]], B = mv.pts[ids[k]], C = mv.pts[ids[k + 1]];
				a.push_back(A); b.push_back(B); c.push_back(C);
				n.push_back(Unit(m::la::cross(B - A, C - A), V(0, 0, 1)));
			}
		}
		V l(1e300), h(-1e300);
		for (size_t i = 0; i < a.size(); ++i)
			for (const V& p : { a[i], b[i], c[i] }) { l = m::la::min(l, p); h = m::la::max(h, p); }
		const V ext = m::la::max(h - l, V(1e-6));
		const double target = std::cbrt(std::max<double>(1.0, (double)a.size()) / 2.0);
		const double edge = std::max({ ext.x, ext.y, ext.z }) / std::max(1.0, target);
		nx = std::max(1, std::min(128, (int)std::ceil(ext.x / edge)));
		ny = std::max(1, std::min(128, (int)std::ceil(ext.y / edge)));
		nz = std::max(1, std::min(128, (int)std::ceil(ext.z / edge)));
		lo = l; cell = V(ext.x / nx, ext.y / ny, ext.z / nz);
		bins.assign((size_t)nx * ny * nz, {});
		for (size_t i = 0; i < a.size(); ++i)
		{
			const V tl = m::la::min(a[i], m::la::min(b[i], c[i])), th = m::la::max(a[i], m::la::max(b[i], c[i]));
			int i0[3], i1[3];
			Cells(tl, i0); Cells(th, i1);
			for (int x = i0[0]; x <= i1[0]; ++x) for (int y = i0[1]; y <= i1[1]; ++y) for (int z = i0[2]; z <= i1[2]; ++z)
				bins[((size_t)x * ny + y) * nz + z].push_back((int)i);
		}
	}
	void Cells(const V& p, int* o) const
	{
		o[0] = std::clamp((int)std::floor((p.x - lo.x) / cell.x), 0, nx - 1);
		o[1] = std::clamp((int)std::floor((p.y - lo.y) / cell.y), 0, ny - 1);
		o[2] = std::clamp((int)std::floor((p.z - lo.z) / cell.z), 0, nz - 1);
	}
	                                                   
	bool Nearest(const V& p, double maxD, V& out, V& nrm, std::vector<uint32_t>& stamp, uint32_t& tick) const
	{
		int i0[3], i1[3];
		Cells(p - V(maxD), i0); Cells(p + V(maxD), i1);
		double best = maxD * maxD;
		bool found = false;
		++tick;
		for (int x = i0[0]; x <= i1[0]; ++x) for (int y = i0[1]; y <= i1[1]; ++y) for (int z = i0[2]; z <= i1[2]; ++z)
			for (int t : bins[((size_t)x * ny + y) * nz + z])
			{
				if (stamp[(size_t)t] == tick) continue;
				stamp[(size_t)t] = tick;
				const V q = ClosestOnTri(p, a[(size_t)t], b[(size_t)t], c[(size_t)t]);
				const V d = q - p;
				const double d2 = m::la::dot(d, d);
				if (d2 < best) { best = d2; out = q; nrm = n[(size_t)t]; found = true; }
			}
		return found;
	}
	                                                                         
	                                                                         
	                                                                          
	                                                                         
	                                                         
	V AvgNormal(const V& q, const V& pos, double rad, const V& nearest, bool flipAll, std::vector<uint32_t>& stamp, uint32_t& tick) const
	{
		int i0[3], i1[3];
		Cells(pos - V(rad), i0); Cells(pos + V(rad), i1);
		V sum(0.0);
		++tick;
		for (int x = i0[0]; x <= i1[0]; ++x) for (int y = i0[1]; y <= i1[1]; ++y) for (int z = i0[2]; z <= i1[2]; ++z)
			for (int t : bins[((size_t)x * ny + y) * nz + z])
			{
				if (stamp[(size_t)t] == tick) continue;
				stamp[(size_t)t] = tick;
				const double d = Len(ClosestOnTri(pos, a[(size_t)t], b[(size_t)t], c[(size_t)t]) - pos);
				if (d >= rad) continue;
				const V f = n[(size_t)t];
				const V v = q - ClosestOnTri(q, a[(size_t)t], b[(size_t)t], c[(size_t)t]);
				const double side = m::la::dot(v, f), lv = Len(v);
				const bool flip = std::abs(side) > 0.9 * lv && lv > 1e-12 ? side < 0 : flipAll;
				sum += (flip ? f * -1.0 : f) * (1.0 - d / rad);
			}
		return Unit(sum, nearest);
	}
};

struct RNode { V pos, n, dir; double length = 0; };                                      
struct RRoot
{
	std::vector<RNode> nodes;
	bool alive = true;
	int level = 0, parent = -1, stuck = 0;
	size_t parentNode = 0;
	double target = 0;
	V origin;
};

}               

// основной вход: собираем геометрию по переданным настройкам
bool GrowRoots(const MeshView& mv, const std::vector<V>& seedsIn, const RootsParams& p, RootsResult& out,
	const std::vector<std::vector<V>>* guides)
{
	out = RootsResult();
	std::vector<V> seeds = seedsIn;
	if (seeds.empty() && guides != nullptr)
		for (const auto& g : *guides) if (!g.empty()) seeds.push_back(g.front());
	if (mv.npolys <= 0 || seeds.empty()) return false;
	TriGrid grid;
	grid.Build(mv);
	if (grid.a.empty()) return false;
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	std::vector<V> polyN((size_t)mv.npolys);
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		V s = m::la::cross(V(mv.pts[q.b]) - V(mv.pts[q.a]), V(mv.pts[q.c]) - V(mv.pts[q.a]));
		if (q.c != q.d) s += m::la::cross(V(mv.pts[q.c]) - V(mv.pts[q.a]), V(mv.pts[q.d]) - V(mv.pts[q.a]));
		polyN[(size_t)i] = GuidedNormal(mv, i, Unit(s, V(0, 0, 1)));
	}
	V lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < mv.npts; ++i) { lo = m::la::min(lo, V(mv.pts[i])); hi = m::la::max(hi, V(mv.pts[i])); }
	const double R = std::max(1e-9, 0.5 * Len(hi - lo));
	const double step = std::max(1e-6, R * p.size * 0.005);
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	const bool useGuides = guides != nullptr && !guides->empty() && p.guideWeight > 0.0;
	const double guideReach = std::max(step, R * p.guideRadius);
	                                                                      
	auto guidePull = [&](const V& pos) {
		if (!useGuides) return V(0.0);
		double best = guideReach;
		V dir(0.0), toward(0.0);
		for (const auto& g : *guides)
			for (size_t i = 0; i + 1 < g.size(); ++i)
			{
				const V a = g[i], b = g[i + 1], ab = b - a;
				const double l2 = m::la::dot(ab, ab);
				if (l2 < 1e-18) continue;
				const double t = std::clamp(m::la::dot(pos - a, ab) / l2, 0.0, 1.0);
				const V q = a + ab * t;
				const double d = Len(q - pos);
				if (d < best) { best = d; dir = ab / std::sqrt(l2); toward = q - pos; }
			}
		if (Len(dir) < 1e-12) return V(0.0);
		const double fade = 1.0 - best / guideReach;
		const double drift = std::clamp((best / guideReach - 0.5) * 2.0, 0.0, 1.0);
		return Unit(dir + Unit(toward, V(0.0)) * (0.35 * drift), dir) * fade;
	};
	Rng rng((uint64_t)(uint32_t)p.seed + 7);
	const double hover = 0.25 * step;                                              
	std::vector<uint32_t> stamp(grid.a.size(), 0);
	uint32_t tick = 0;
	                                                                             
	auto snap = [&](const V& q, double maxD, const V& hint, V& pos, V& n) {
		V nn;
		if (!grid.Nearest(q, maxD, pos, nn, stamp, tick)) return false;
		double side = m::la::dot(q - pos, nn);
		if (std::abs(side) < 1e-9 * R) side = m::la::dot(nn, hint);
		n = grid.AvgNormal(q, pos, 2.0 * hover, side < 0 ? nn * -1.0 : nn, side < 0, stamp, tick);
		n = GuidedPointNormal(mv,pos,n);
		return true;
	};
	auto tangentOf = [](const V& v, const V& n) { return v - n * m::la::dot(v, n); };
	auto perpendicular = [](const V& a) { return Unit(m::la::cross(a, std::abs(a.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0)); };
	const double wsum = std::max(1e-9, p.primaryWeight + p.randomWeight + p.gravityWeight + p.spreadWeight + (useGuides ? p.guideWeight : 0.0));
	const double wp = p.primaryWeight / wsum, wr = p.randomWeight / wsum, wg = p.gravityWeight / wsum, ws = p.spreadWeight / wsum;
	const double wgd = useGuides ? p.guideWeight / wsum : 0.0;
	const double mainLen = std::max(2.0 * step, R * p.rootLength);
	const double lenRnd = std::clamp(p.lengthRandom, 0.0, 1.0);
	const int perSeed = std::clamp(p.rootsPerSeed, 1, 32);
	const int maxLevels = std::clamp(p.maxLevels, 0, 5);
	const double branchAng = std::clamp(p.branchAngle, 5.0, 90.0) * m::kPi / 180.0;
	                                                  
	std::vector<RRoot> roots;
	for (const V& s : seeds)
	{
		V q, n;
		if (!snap(s, 4.0 * R, up, q, n)) continue;
		const V t1 = Unit(tangentOf(V(1, 0, 0) - up * m::la::dot(V(1, 0, 0), up), n), perpendicular(n));
		const V t2 = m::la::cross(n, t1);
		const double phase = rng.U() * 2.0 * m::kPi;
		for (int k = 0; k < perSeed; ++k)
		{
			const double ang = phase + 2.0 * m::kPi * (k + (rng.U() - 0.5) * 0.6) / perSeed;
			RRoot r;
			r.origin = q;
			r.target = mainLen * (1.0 + lenRnd * (rng.U() * 2.0 - 1.0));
			RNode nd;
			nd.pos = q; nd.n = n;
			nd.dir = t1 * std::cos(ang) + t2 * std::sin(ang);
			r.nodes.push_back(nd);
			roots.push_back(std::move(r));
		}
	}
	if (roots.empty()) return false;
	size_t total = roots.size();
	const size_t cap = (size_t)std::max(10, p.maxNodes);
	for (int it = 0; it < p.maxSteps && total < cap; ++it)
	{
		bool any = false;
		for (size_t ri = 0; ri < roots.size() && total < cap; ++ri)
		{
			RRoot& r = roots[ri];
			if (!r.alive) continue;
			const RNode last = r.nodes.back();
			if (last.length >= r.target) { r.alive = false; continue; }
			any = true;
			const V n = last.n;
			const V downhill = tangentOf(up * -1.0, n);
			const V spread = Unit(tangentOf(last.pos - r.origin, n), V(0.0));
			                                                    
			const double gLevel = std::pow(0.5, (double)r.level);
			V d = last.dir * wp + Unit(tangentOf(rng.Dir(), n), last.dir) * wr + downhill * wg + spread * ws
				+ guidePull(last.pos) * (wgd * gLevel);
			d = Unit(tangentOf(d, n), last.dir);
			                                                                     
			                                                                
			                              
			V target = last.pos + n * hover, hint = n;
			double rest = step;
			for (int pass = 0; pass < 6 && rest > 1e-9 * step; ++pass)
			{
				double hd = 0.0; int32_t hp = -1;
				if (!bvh.Raycast(target, d, rest, &hd, &hp) || hp < 0) { target += d * rest; break; }
				V nf = polyN[(size_t)hp];
				if (m::la::dot(nf, d) > 0) nf = nf * -1.0;                           
				const V hit = target + d * std::max(0.0, hd - 0.05 * hover);
				                                                                   
				const V line = Unit(m::la::cross(hint, nf), V(0.0));
				const double along = m::la::dot(d, line);
				if (Len(line) > 0.5 && std::abs(along) > 0.5) d = line * (along < 0 ? -1.0 : 1.0);
				else d = Unit(tangentOf(d, nf), Unit(tangentOf(hint, nf), perpendicular(nf)));
				hint = nf;
				target = hit + nf * (0.5 * hover);
				rest -= hd;
			}
			V q, nn;
			if (!snap(target, 2.0 * step, hint, q, nn)) { r.alive = false; continue; }
			                                                                     
			                           
			{
				const V a = last.pos + n * hover, seg = q + nn * hover - a;
				const double sl = Len(seg);
				double hd = 0.0; int32_t hp = -1;
				if (sl > 1e-12 && bvh.Raycast(a, seg / sl, sl, &hd, &hp) && hp >= 0 && hd < sl - 0.5 * hover)
				{
					if (++r.stuck > 3) { r.alive = false; continue; }
					r.nodes.back().dir = Unit(tangentOf(rng.Dir(), n), last.dir);
					continue;
				}
			}
			const double moved = Len(q - last.pos);
			if (moved < 0.2 * step) { if (++r.stuck > 3) { r.alive = false; continue; } }
			else r.stuck = 0;
			RNode nd;
			nd.pos = q;
			nd.n = nn;
			nd.dir = Unit(tangentOf(last.dir * 0.5 + Unit(q - last.pos, last.dir) * 0.5, nn), last.dir);
			nd.length = last.length + moved;
			r.nodes.push_back(nd);
			++total;
		}
		                                                     
		const size_t nr = roots.size();
		for (size_t ri = 0; ri < nr && total < cap; ++ri)
		{
			if (!roots[ri].alive || roots[ri].nodes.size() < 4 || roots[ri].level >= maxLevels) continue;
			if (rng.U() > p.branchingProbability) continue;
			const size_t ni = 1 + (size_t)(rng.U() * (double)(roots[ri].nodes.size() - 2));
			const RNode base = roots[ri].nodes[ni];
			const double remaining = std::max(0.0, roots[ri].target - base.length);
			const double len = remaining * std::clamp(p.childLength, 0.05, 1.0) * (0.7 + 0.6 * rng.U());
			if (len < 3.0 * step) continue;
			const double ang = (branchAng + (rng.U() - 0.5) * 0.5) * (rng.U() < 0.5 ? -1.0 : 1.0);
			RRoot b;
			b.level = roots[ri].level + 1;
			b.parent = (int)ri;
			b.parentNode = ni;
			b.origin = base.pos;
			b.target = len;
			RNode nd = base;
			nd.dir = Unit(base.dir * std::cos(ang) + m::la::cross(base.n, base.dir) * std::sin(ang), base.dir);
			nd.length = 0.0;
			b.nodes.push_back(nd);
			roots.push_back(std::move(b));
			++total;
		}
		if (!any) break;
	}
	                                              
	const int sides = std::clamp(p.sides, 3, 16);
	const double baseR = std::max(1e-6, R * p.rootSize * 0.01);
	const double tip = std::clamp(p.tipRadius, 0.0, 1.0);
	const double taper = std::clamp(p.taper, 0.1, 3.0);
	const double embed = std::clamp(p.embed, 0.0, 1.0);
	const double flat = std::clamp(p.flatten, 0.0, 0.9);
	const double circ = 2.0 * m::kPi * baseR;
	std::vector<std::vector<double>> radii(roots.size());
	std::vector<std::vector<size_t>> remap(roots.size());                                 
	int made = 0;
	for (size_t ri = 0; ri < roots.size(); ++ri)
	{
		RRoot& r = roots[ri];
		const size_t n0 = r.nodes.size();
		if (n0 < 2) continue;
		                                                          
		for (int pass = 0; pass < 2; ++pass)
		{
			std::vector<V> ps(n0);
			for (size_t i = 0; i < n0; ++i)
				ps[i] = (i == 0 || i + 1 == n0) ? r.nodes[i].pos : (r.nodes[i - 1].pos + r.nodes[i].pos * 2.0 + r.nodes[i + 1].pos) * 0.25;
			for (size_t i = 0; i < n0; ++i) r.nodes[i].pos = ps[i];
		}
		                                                                  
		auto onSurface = [&](RNode& nd, const V& query, const V& hint) {
			V q, nn;
			if (!snap(query, 3.0 * step, hint, q, nn)) return;
			nd.pos = q;
			nd.n = nn;
		};
		const double lift = 2.0 * hover;
		for (RNode& nd : r.nodes) onSurface(nd, nd.pos + nd.n * lift, nd.n);
		                                                                       
		                                                                         
		                                                                       
		                                                                         
		                                               
		std::vector<RNode> fine;
		std::vector<size_t>& map = remap[ri];
		map.assign(n0, 0);
		for (size_t i = 0; i < n0; ++i)
		{
			map[i] = fine.size();
			fine.push_back(r.nodes[i]);
			if (i + 1 == n0) break;
			const RNode& A = r.nodes[i];
			const RNode& B = r.nodes[i + 1];
			const double gap = Len(B.pos - A.pos);
			const bool convex = m::la::dot(B.pos - A.pos, A.n) < -0.02 * gap || m::la::dot(A.pos - B.pos, B.n) < -0.02 * gap;
			const double ang = convex ? std::acos(std::clamp(m::la::dot(A.n, B.n), -1.0, 1.0)) : 0.0;
			const int k = std::clamp((int)std::ceil(std::max(ang / 0.25, gap / (1.5 * step))) - 1, 0, 16);
			const V ha = A.pos + A.n * lift, hb = B.pos + B.n * lift;
			for (int j = 1; j <= k; ++j)
			{
				const double t = (double)j / (k + 1);
				RNode nd;
				nd.n = Unit(A.n * (1.0 - t) + B.n * t, A.n);
				nd.pos = A.pos * (1.0 - t) + B.pos * t;
				nd.dir = Unit(A.dir * (1.0 - t) + B.dir * t, A.dir);
				onSurface(nd, ha * (1.0 - t) + hb * t, nd.n);
				fine.push_back(nd);
			}
		}
		r.nodes.swap(fine);
		const size_t n = r.nodes.size();
		r.nodes[0].length = 0.0;
		for (size_t i = 1; i < n; ++i) r.nodes[i].length = r.nodes[i - 1].length + Len(r.nodes[i].pos - r.nodes[i - 1].pos);
		{
			std::vector<V> ns(n);
			for (size_t i = 0; i < n; ++i)
				ns[i] = Unit(r.nodes[i > 0 ? i - 1 : 0].n + r.nodes[i].n * 2.0 + r.nodes[std::min(n - 1, i + 1)].n, r.nodes[i].n);
			for (size_t i = 0; i < n; ++i) r.nodes[i].n = ns[i];
		}
		const double L = std::max(1e-9, r.nodes.back().length);
		double rBase = baseR * (0.8 + 0.4 * rng.U());
		if (r.parent >= 0 && !radii[(size_t)r.parent].empty())
		{
			const auto& pm = remap[(size_t)r.parent];
			const size_t at = pm.empty() ? 0 : pm[std::min(pm.size() - 1, r.parentNode)];
			rBase = radii[(size_t)r.parent][std::min(radii[(size_t)r.parent].size() - 1, at)] * std::clamp(p.childRadius, 0.1, 1.0);
		}
		const double flareLen = r.level == 0 ? std::clamp(p.flare, 0.0, 1.0) * L : 0.0;
		std::vector<double>& rad = radii[ri];
		rad.resize(n);
		std::vector<V> centers(n);
		std::vector<double> offset(n);
		for (size_t i = 0; i < n; ++i)
		{
			const double s = r.nodes[i].length;
			double rr = rBase * (tip + (1.0 - tip) * std::pow(std::clamp(1.0 - s / L, 0.0, 1.0), taper));
			double e = embed;
			if (flareLen > 0.0 && s < flareLen)
			{
				const double w = 1.0 - s / flareLen;
				rr *= 1.0 + 0.6 * w * w;
				e = embed + (-0.3 - embed) * w;                                          
			}
			rad[i] = rr;
			offset[i] = rr * (1.0 - flat) * (1.0 - 2.0 * e);
			centers[i] = r.nodes[i].pos + r.nodes[i].n * offset[i];
		}
		                                                                       
		                                                                         
		                                                                         
		              
		for (int pass = 0; pass < 4; ++pass)
			for (size_t i = 1; i + 1 < n; ++i)
			{
				const V c = (centers[i - 1] + centers[i] * 2.0 + centers[i + 1]) * 0.25;
				V q, nq;
				if (!snap(c, 3.0 * step, r.nodes[i].n, q, nq)) continue;
				if (m::la::dot(c - q, r.nodes[i].n) >= offset[i] - 0.1 * rad[i] && Len(c - q) >= offset[i] - 0.1 * rad[i]) centers[i] = c;
			}
		const Int32 first = (Int32)out.points.size();
		double vAcc = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const V t = Unit(centers[std::min(n - 1, i + 1)] - centers[i > 0 ? i - 1 : 0], r.nodes[i].dir);
			const V nz = Unit(r.nodes[i].n - t * m::la::dot(r.nodes[i].n, t), perpendicular(t));
			const V nx = m::la::cross(t, nz);
			if (i > 0) vAcc += Len(centers[i] - centers[i - 1]);
			for (int k = 0; k <= sides; ++k)                                           
			{
				const double ang = 2.0 * m::kPi * k / sides;
				out.points.push_back(centers[i] + nx * (std::cos(ang) * rad[i]) + nz * (std::sin(ang) * rad[i] * (1.0 - flat)));
				out.uvVert.push_back({ (double)k / sides * p.uvScale, vAcc / circ * p.uvScale });
			}
		}
		const Int32 row = sides + 1;
		for (size_t i = 0; i + 1 < n; ++i)
			for (int k = 0; k < sides; ++k)
			{
				const Int32 a = first + (Int32)i * row + k, b = a + 1, c = a + row + 1, d = a + row;
				out.polys.push_back({ a, d, c, b });                     
			}
		++made;
	}
	out.roots = made;
	out.nodes = (int)total;
	return made > 0;
}

PROJECT_DAMAGE_NS_MACRO_END
