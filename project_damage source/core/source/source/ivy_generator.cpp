// растим ветви плюща, удерживая их у поверхности и направляющих
// ProjectDamage ivy generator — GPL-3.0-or-later; see LICENSE.
//
// An independent implementation of the classic ivy growth model (Thomas Luft,
// "Ivy Generator", 2007): every root grows node by node from a seed on the
// target surface; the step is a normalized blend of the primary direction (a
// running average of past steps), a random direction and the ADHESION vector
// (toward the nearest surface point within Max Adhesion Distance, strength
// 1 - d / D), plus gravity that grows with the free-floating length
// ((floating / Max Float Length)^0.7). A step that would pass through the
// surface is stopped on it and slides along it (climbing). A root dies when it
// floats too long; climbing nodes spawn side branches with Branching
// Probability. Lengths scale with the target's bounding radius R.
// Birth: each root becomes a tube (radius tapering toward its tip, thinner for
// higher branch levels; parallel-transport frames) with a procedural UV -
// U around the tube, V along it in the same metric scale (bark does not
// stretch). Leaves alternate left / right along the shoot like real ivy: each
// hangs on a petiole (a thin curved bark tube from the branch, arching away
// from the surface), faces away from the surface (the smoothed adhesion
// vector) and points along the growth direction, splayed toward its side,
// then tilts at random on two axes (Leaf Tilt): pitch lifts the tip off the
// surface, roll turns it about its length; the petiole bends to enter the
// tilted leaf along its axis.
// Each leaf gets the full 0..1 UV square, turned so the texture's leaf tip
// (Leaf UV Direction) runs along the leaf and its petiole point (Stem Attach)
// sits on the petiole end; the UVs are not mirrored seen from the front.
// Drawn curves: each curve becomes a stem along it, exactly as drawn (also in
// mid-air, never dying), that sprouts tendrils at Hang Density per node: they
// start sideways, fall with gravity (kept in their growth memory, so they turn
// straight down) and live up to Hang Length while floating - hanging ivy. A
// tendril that reaches the surface climbs it like any root.
// Nearest-point queries use a uniform grid of triangles; collisions the BVH.
#include "ivy_generator.h"
#include "project_damage_bvh.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
};

struct Node { V pos, primary, adhesion, smoothAdh; double length = 0, floating = 0; bool climb = false; };
struct Root
{
	std::vector<Node> nodes;
	bool alive = true;
	int parents = 0;
	bool drawn = false;                                                      
	double hangMax = 0.0;                                                               
};

}               

// основной вход: собираем геометрию по переданным настройкам
bool GrowIvy(const MeshView& mv, const std::vector<V>& seedsIn, const IvyParams& p, IvyResult& out,
	const std::vector<std::vector<V>>* guides, const std::vector<std::vector<V>>* curves)
{
	out = IvyResult();
	std::vector<V> seeds = seedsIn;
	if (seeds.empty() && guides != nullptr && p.guideWeight < 2.0)
		for (const auto& g : *guides) if (!g.empty()) seeds.push_back(g.front());
	const bool hasCurves = curves != nullptr && std::any_of(curves->begin(), curves->end(), [](const auto& c) { return c.size() >= 2; });
	if (mv.npolys <= 0 || (seeds.empty() && !hasCurves && !(guides != nullptr && p.guideWeight >= 2.0 && std::any_of(guides->begin(), guides->end(), [](const auto& g) { return g.size() >= 2; })))) return false;
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
	const double R = 0.5 * Len(hi - lo);
	const double step = std::max(1e-6, R * p.size * 0.005);
	const double maxFloat = std::max(step, R * p.maxFloatLength);
	const double maxAdh = std::max(step, R * p.maxAdhesionDistance);
	const bool useGuides = guides != nullptr && !guides->empty() && p.guideWeight > 0.0;
	const double wg0 = useGuides ? p.guideWeight : 0.0;
	const double wsum = std::max(1e-9, p.primaryWeight + p.randomWeight + p.adhesionWeight + wg0);
	const double wp = p.primaryWeight / wsum, wr = p.randomWeight / wsum, wa = p.adhesionWeight / wsum, wg = wg0 / wsum;
	const double guideReach = std::max(step, R * p.guideRadius);
	                                                                            
	                                                                        
	auto guidePull = [&](const V& pos, const V& heading) {
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
		if (p.guideWeight >= 2.0 && m::la::dot(dir, heading) < 0.0) dir = -dir;
		const double fade = 1.0 - best / guideReach;
		                                                                    
		                                                             
		const double drift = std::clamp((best / guideReach - 0.5) * 2.0, 0.0, 1.0);
		return Unit(dir + Unit(toward, V(0.0)) * (0.35 * drift), dir) * fade;
	};
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	Rng rng((uint64_t)(uint32_t)p.seed + 1);
	std::vector<uint32_t> stamp(grid.a.size(), 0);
	uint32_t tick = 0;
	auto adhesion = [&](const V& pos, V& nrmOut) {
		V q, n;
		nrmOut = V(0.0);
		if (!grid.Nearest(pos, maxAdh, q, n, stamp, tick)) return V(0.0);
		nrmOut = n;
		const double d = Len(q - pos);
		return Unit(q - pos, V(0.0)) * (1.0 - d / maxAdh);
	};
	                                        
	std::vector<Root> roots;
	for (const V& s : seeds)
	{
		V q, n;
		Root r;
		Node nd;
		nd.pos = grid.Nearest(s, 4.0 * R, q, n, stamp, tick) ? q + n * (0.25 * step) : s;
		nd.primary = up;
		nd.climb = true;
		r.nodes.push_back(nd);
		roots.push_back(std::move(r));
	}
	const size_t cap = (size_t)std::max(10, p.maxNodes);
	if (useGuides && p.guideWeight >= 2.0)
	{
		double guideLength = 0.0;
		for (const auto& g : *guides)
			for (size_t i = 1; i < g.size(); ++i) guideLength += Len(g[i] - g[i - 1]);
		const size_t guideBudget = std::max<size_t>(2, cap / std::max(4, std::min(p.maxSteps + 1, 32)));
		const double spacing = std::max({step * 12.0, guideReach * 0.35, guideLength * 2.0 / double(guideBudget)});
		for (const auto& g : *guides)
		{
			double carry = 0.0;
			for (size_t i = 1; i < g.size() && roots.size() + 2 <= cap; ++i)
			{
				const V delta = g[i] - g[i - 1];
				const double length = Len(delta);
				if (length < 1e-12) continue;
				for (double at = carry; at <= length && roots.size() + 2 <= cap; at += spacing)
				{
					const V point = g[i - 1] + delta * (at / length);
					V q, n;
					const V position = grid.Nearest(point, 4.0 * R, q, n, stamp, tick) ? q + n * (0.25 * step) : point;
					for (double direction : {-1.0, 1.0})
					{
						Root root;
						Node node;
						node.pos = position;
						node.primary = delta * (direction / length);
						node.climb = true;
						root.nodes.push_back(node);
						roots.push_back(std::move(root));
					}
				}
				carry = std::fmod(std::max(0.0, length - carry), spacing);
				carry = carry > 1e-12 ? spacing - carry : spacing;
			}
		}
	}
	size_t total = roots.size();
	                                                                        
	const double maxHang = std::max(step, R * p.hangLength);
	if (hasCurves)
		for (const auto& c : *curves)
		{
			if (c.size() < 2 || total >= cap) continue;
			Root r;
			r.drawn = true;
			r.alive = false;
			std::vector<V> pts{ c.front() };
			double carry = 0.0;
			for (size_t i = 0; i + 1 < c.size(); ++i)
			{
				const V a = c[i], d = c[i + 1] - c[i];
				const double l = Len(d);
				if (l < 1e-12) continue;
				double at = step - carry;
				for (; at <= l && pts.size() < cap - total; at += step) pts.push_back(a + d * (at / l));
				if (pts.size() >= cap - total) break;
				carry = l - (at - step);
			}
			if (pts.size() < cap - total && Len(pts.back() - c.back()) > 0.25 * step) pts.push_back(c.back());
			if (pts.size() < 2) continue;
			for (size_t i = 0; i < pts.size() && total < cap; ++i)
			{
				Node nd;
				nd.pos = pts[i];
				nd.primary = Unit(pts[std::min(pts.size() - 1, i + 1)] - pts[i > 0 ? i - 1 : 0], up);
				V nrm;
				nd.adhesion = adhesion(nd.pos, nrm);
				nd.length = i > 0 ? r.nodes.back().length + Len(pts[i] - pts[i - 1]) : 0.0;
				nd.climb = true;
				r.nodes.push_back(nd);
				++total;
			}
			if (r.nodes.size() < 2) continue;
			                                                                    
			const size_t stemCount = r.nodes.size();
			roots.push_back(std::move(r));
			const size_t ri = roots.size() - 1;
			for (size_t i = 1; i < stemCount && total < cap; ++i)
			{
				if (rng.U() > p.hangDensity) continue;
				const Node& base = roots[ri].nodes[i];
				V side = rng.Dir();
				side = Unit(side - base.primary * m::la::dot(side, base.primary), up);
				Root b;
				b.parents = 1;
				b.hangMax = maxHang * (0.4 + 0.6 * rng.U());
				Node nd = base;
				nd.primary = Unit(side - up * 0.5, up * -1.0);
				nd.floating = 0.0;
				nd.climb = false;
				b.nodes.push_back(nd);
				roots.push_back(std::move(b));
				++total;
			}
		}
	for (int it = 0; it < p.maxSteps && total < cap; ++it)
	{
		bool any = false;
		const size_t nr = roots.size();
		for (size_t ri = 0; ri < nr && total < cap; ++ri)
		{
			Root& r = roots[ri];
			if (!r.alive) continue;
			const Node last = r.nodes.back();
			if (last.floating > (r.hangMax > 0.0 ? r.hangMax : maxFloat)) { r.alive = false; continue; }
			any = true;
			V nrm;
			const V adh = adhesion(last.pos, nrm);
			V rnd = rng.Dir() + up * 0.2;
			                                                                  
			                                                                
			const double gLevel = std::pow(0.5, (double)r.parents);
			const V grow = (last.primary * wp + Unit(rnd, up) * wr + adh * wa + guidePull(last.pos, last.primary) * (wg * gLevel)) * step;
			const V grav = up * (-step * p.gravityWeight * std::pow(std::clamp(last.floating / maxFloat, 0.0, 1.0), 0.7));
			V newPos = last.pos + grow + grav;
			bool climbing = Len(adh) > 0.0 && Len(adh) >= std::max(0.0, 1.0 - 1.5 * step / maxAdh);
			                                                                   
			V segmentStart = last.pos;
            bool collisionSolved = false;
            // CrazyIvy retries collisions: resolve the remaining slide against
            // other faces too, rather than stepping through an inside corner.
            for (int collisionPass = 0; collisionPass < 6; ++collisionPass)
			{
                const V seg = newPos - segmentStart;
                const double sl = Len(seg);
                if (sl <= 1e-12) { collisionSolved = true; break; }
				double hd = 0.0; int32_t hp = -1;
				if (bvh.Raycast(segmentStart, seg / sl, sl, &hd, &hp) && hp >= 0)
				{
					const V n = polyN[(size_t)hp];
					const V hit = segmentStart + seg / sl * hd;
					const double side = m::la::dot(seg, n) < 0 ? 1.0 : -1.0;                           
					const V rest = seg * (1.0 - hd / sl);
					const V slide = rest - n * m::la::dot(rest, n);
					newPos = hit + n * (side * 0.1 * step) + slide;
                    segmentStart = hit + n * (side * 0.1 * step);
					climbing = true;
				}
                else { collisionSolved = true; break; }
			}
            if (!collisionSolved) { r.alive = false; continue; }
			Node nd;
			nd.pos = newPos;
			const double segLen = Len(newPos - last.pos);
			// Keep gravity out of the primary growth memory (CrazyIvy's rule).
            // Hanging tendrils keep it, so they turn straight down.
            nd.primary = Unit(last.primary * 0.5 + Unit(newPos - last.pos - (r.hangMax > 0.0 ? V(0.0) : grav), last.primary) * 0.5, up);
			nd.adhesion = adh;
			nd.length = last.length + segLen;
			nd.floating = climbing ? 0.0 : last.floating + segLen;
			nd.climb = climbing;
			r.nodes.push_back(nd);
			++total;
		}
		                                                                   
		const size_t nr2 = roots.size();
		for (size_t ri = 0; ri < nr2 && total < cap; ++ri)
		{
			const Root& r = roots[ri];
			if (!r.alive || r.nodes.size() < 4 || r.parents > 3) continue;
			if (rng.U() > p.branchingProbability) continue;
			const size_t ni = 1 + (size_t)(rng.U() * (double)(r.nodes.size() - 2));
			const Node& base = r.nodes[ni];
			if (!base.climb && r.hangMax <= 0.0) continue;
			Root b;
			b.parents = r.parents + 1;
			b.hangMax = r.hangMax;
			Node nd = base;
			nd.primary = Unit(base.primary * 0.3 + rng.Dir() * 0.7, up);
			nd.floating = 0.0;
			b.nodes.push_back(nd);
			roots.push_back(std::move(b));
			++total;
		}
		if (!any) break;
	}
	                                                      
	for (Root& r : roots)
	{
        if (p.adhesionSmoothing <= 0) {
            V acc(0.0);
            for (Node& nd : r.nodes) { acc = acc * 0.6 + nd.adhesion * 0.4; nd.smoothAdh = acc; }
        } else {
                                                                              
                                                                                
            constexpr double weights[11] = {1,2,4,7,9,10,9,7,4,2,1};
            std::vector<V> current, next(r.nodes.size());
            for (const Node& nd : r.nodes) current.push_back(nd.adhesion);
            for (int pass = 0; pass < std::clamp(p.adhesionSmoothing,0,5); ++pass) {
                for (size_t i = 0; i < current.size(); ++i) {
                    V sum(0.0);
                    for (int k = -5; k <= 5; ++k) {
                        const size_t j = (size_t)std::clamp((int)i+k,0,(int)current.size()-1);
                        sum += current[j] * weights[k+5];
                    }
                    next[i] = sum / 56.0;
                }
                current.swap(next);
            }
            for (size_t i=0; i<r.nodes.size(); ++i) r.nodes[i].smoothAdh = current[i];
        }
	}
	                          
	const int sides = std::clamp(p.branchSides, 3, 16);
	const double baseR = std::max(1e-6, R * p.branchSize * 0.01);
	                                                                         
	auto radiusAt = [&](const Root& r, double length) {
		const double totalLen = std::max(1e-9, r.nodes.back().length - r.nodes.front().length);
		const double t = std::clamp((length - r.nodes.front().length) / totalLen, 0.0, 1.0);                     
		return baseR * (0.35 + 0.65 * std::pow(1.0 - t, 0.6)) / (1.0 + 0.5 * r.parents);
	};
	for (const Root& r : roots)
	{
		if (r.nodes.size() < 2) continue;
		const size_t n = r.nodes.size();
		std::vector<double> rad(n);
		for (size_t i = 0; i < n; ++i) rad[i] = radiusAt(r, r.nodes[i].length);
		const Int32 first = (Int32)out.branchPoints.size();
		V prevN(0.0);
		double vAcc = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const V t = Unit(r.nodes[std::min(n - 1, i + 1)].pos - r.nodes[i > 0 ? i - 1 : 0].pos, up);
			if (i == 0) prevN = Unit(m::la::cross(t, std::abs(t.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
			const V nx = Unit(prevN - t * m::la::dot(prevN, t), prevN);
			const V ny = m::la::cross(t, nx);
			prevN = nx;
			if (i > 0) vAcc += Len(r.nodes[i].pos - r.nodes[i - 1].pos);
			                                                                
			const double circ = 2.0 * m::kPi * std::max(1e-9, baseR);
			const double vv = vAcc / circ * p.uvScale;
			for (int s = 0; s <= sides; ++s)                                           
			{
				const double ang = 2.0 * m::kPi * s / sides;
				const V q = r.nodes[i].pos + (nx * std::cos(ang) + ny * std::sin(ang)) * rad[i];
				out.branchPoints.push_back(q);
				out.branchUvVert.push_back({ (double)s / sides * p.uvScale, vv });
			}
		}
		const Int32 row = sides + 1;
		for (size_t i = 0; i + 1 < n; ++i)
			for (int s = 0; s < sides; ++s)
			{
				const Int32 a = first + (Int32)i * row + s, b = a + 1, c = a + row + 1, d = a + row;
				out.branchPolys.push_back({ a, d, c, b });
			}
	}
	                             
	const double leafBase = R * p.leafSize * 0.012;
	const int stemSides = std::clamp(sides / 2 + 1, 3, 6);
	constexpr int stemSegs = 4;
	const double stemAttach = std::clamp(p.stemAttach, 0.0, 1.0);
	const int uvDir = ((p.leafUvDirection % 4) + 4) % 4;
	                                                                            
	                                                                        
	auto leafUv = [uvDir](double s, double t) -> std::array<double, 2> {
		switch (uvDir)
		{
		case 1:  return { t, 1.0 - s };
		case 2:  return { 1.0 - s, 1.0 - t };
		case 3:  return { 1.0 - t, s };
		default: return { s, t };
		}
	};
	auto perpendicular = [](const V& a) { return Unit(m::la::cross(a, std::abs(a.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0)); };
	                                                                      
	auto addStem = [&](const V& S, const V& C1, const V& C2, const V& P, double r0, double r1) {
		const Int32 first = (Int32)out.branchPoints.size();
		const double circ = 2.0 * m::kPi * std::max(1e-9, baseR);
		V prevN(0.0), prevPos = S;
		double vAcc = 0.0;
		for (int j = 0; j <= stemSegs; ++j)
		{
			const double t = (double)j / stemSegs, u = 1.0 - t;
			const V pos = S * (u * u * u) + C1 * (3.0 * u * u * t) + C2 * (3.0 * u * t * t) + P * (t * t * t);
			const V tan = Unit((C1 - S) * (3.0 * u * u) + (C2 - C1) * (6.0 * u * t) + (P - C2) * (3.0 * t * t), Unit(P - S, up));
			if (j == 0) prevN = perpendicular(tan);
			const V nx = Unit(prevN - tan * m::la::dot(prevN, tan), perpendicular(tan));
			const V ny = m::la::cross(tan, nx);
			prevN = nx;
			vAcc += Len(pos - prevPos);
			prevPos = pos;
			const double rr = r0 + (r1 - r0) * t;
			for (int k = 0; k <= stemSides; ++k)
			{
				const double ang = 2.0 * m::kPi * k / stemSides;
				out.branchPoints.push_back(pos + (nx * std::cos(ang) + ny * std::sin(ang)) * rr);
				out.branchUvVert.push_back({ (double)k / stemSides * p.uvScale, vAcc / circ * p.uvScale });
			}
		}
		const Int32 row = stemSides + 1;
		for (int j = 0; j < stemSegs; ++j)
			for (int k = 0; k < stemSides; ++k)
			{
				const Int32 a = first + j * row + k, b = a + 1, c = a + row + 1, d = a + row;
				out.branchPolys.push_back({ a, d, c, b });
			}
	};
	if (leafBase > 0.0 && p.leafDensity > 0.0)
	for (const Root& r : roots)
	{
		if (r.nodes.size() < 2) continue;
		double sideSign = rng.U() < 0.5 ? -1.0 : 1.0;                                   
		for (size_t i = 1; i < r.nodes.size(); ++i)
		{
			const Node& nd = r.nodes[i];
			const Node& pv = r.nodes[i - 1];
			for (int k = 0; k < std::max(1, p.leafDensityMultiplier); ++k)
			{
				                                                     
                const double t = std::clamp((nd.length-r.nodes.front().length) /
                    std::max(1e-9,r.nodes.back().length-r.nodes.front().length),0.0,1.0);
                const double ground = std::max(0.0,-m::la::dot(up,Unit(nd.smoothAdh,V(0.0))));
                const double mature = std::clamp(std::pow(t,0.7)+ground*(1.-t)*(1.-t),0.0,1.0);
                                                  
                const double distribution = r.drawn ? 1.0 : 1.0-std::clamp(p.leafMaturity,0.0,1.0)*(1.0-mature);
                                                                              
                const double chance = std::clamp(p.leafDensity*(nd.climb||r.hangMax>0.0?1.0:0.5)*1.5*distribution,0.0,1.0);
				if (rng.U() > chance) continue;
				                                             
				const V tangent = Unit(r.nodes[std::min(r.nodes.size() - 1, i + 1)].pos - pv.pos, nd.primary);
				                                                                     
				V free = rng.Dir() + up * 0.8;
				free = Unit(free - tangent * m::la::dot(free, tangent), perpendicular(tangent));
				const V away = Len(nd.smoothAdh) > 1e-6 ? Unit(nd.smoothAdh * -1.0, up) : free;
				V grow = tangent - away * m::la::dot(tangent, away);
				if (Len(grow) < 0.2) grow = nd.primary - away * m::la::dot(nd.primary, away);
				if (Len(grow) < 0.2) grow = up - away * m::la::dot(up, away);
				grow = Unit(grow, perpendicular(away));
				const V sideDir = m::la::cross(away, grow) * sideSign;
				                                                                         
				const double spread = 0.5 + 0.35 * rng.U();
				const double spin = (rng.U() - 0.5) * 0.3;
				const V ax0 = Unit(grow * std::cos(spread + spin) + sideDir * std::sin(spread + spin), grow);                      
				                                                                         
				const double tilt = std::clamp(p.leafTilt, 0.0, 90.0) * m::kPi / 180.0;
				const double pitch = tilt * (-0.3 + 1.3 * rng.U()), roll = tilt * 0.8 * (rng.U() * 2.0 - 1.0);
				const V ax = ax0 * std::cos(pitch) + away * std::sin(pitch);
				const V face0 = away * std::cos(pitch) - ax0 * std::sin(pitch);
				const V right0 = m::la::cross(ax0, away);                                                                        
				const V right = right0 * std::cos(roll) + face0 * std::sin(roll);
				const V face = m::la::cross(right, ax);                                                                       
				const double size = leafBase * (0.6 + 0.8 * rng.U());
				                                                               
				const double f = rng.U();
				const V S = pv.pos + (nd.pos - pv.pos) * f;
				const double rS = radiusAt(r, pv.length + (nd.length - pv.length) * f);
				double L = size * std::max(0.0, p.stemLength) + p.leafOffset * step * rng.U();
				V P;
				if (L > 0.0)
				{
					L = std::max(L, 2.5 * rS);
					                                                               
					P = S + Unit(away * 0.55 + sideDir * 0.65 + grow * 0.5, away) * L;
					                                                                  
					const V C1 = S + away * (0.5 * L) + grow * (0.1 * L);
					const V C2 = P - ax * (0.35 * L);
					const double r0 = std::min(size * 0.02 * std::max(0.0, p.stemThickness), 0.7 * rS);
					if (r0 > 1e-9) addStem(S, C1, C2, P, r0, 0.6 * r0);
				}
				else
					P = S + away * rS;
				                                                                  
				const V base = P - ax * (stemAttach * size);
				const V lift = face * (0.15 * size);
				const V q0 = base - right * (0.5 * size), q1 = base + right * (0.5 * size);
				const V q2 = base + ax * size + right * (0.5 * size) + lift, q3 = base + ax * size - right * (0.5 * size) + lift;
				const Int32 o = (Int32)out.leafPoints.size();
				out.leafPoints.insert(out.leafPoints.end(), { q0, q1, q2, q3 });
				out.leafPolys.push_back({ o, o + 1, o + 2, o + 3 });                       
				for (const auto& uv : { leafUv(0, 0), leafUv(1, 0), leafUv(1, 1), leafUv(0, 1) })
					out.leafUvs.insert(out.leafUvs.end(), { uv[0], uv[1] });
				sideSign = -sideSign;
			}
		}
	}
	out.roots = (int)roots.size();
	out.nodes = (int)total;
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
