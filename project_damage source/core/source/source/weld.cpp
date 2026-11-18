// ведём сварные швы вдоль выбранных рёбер и линий
// ProjectDamage welded edges — GPL-3.0-or-later; see LICENSE.
//
// Recipe weld-v2 (WeldParams, same settings as weld-v1). Weld beads as
// separate geometry:
//   edges    mesh edges where two faces meet at a corner sharper than Angle -
//            inside (concave, the usual fillet weld), outside or both - whose
//            two vertices carry a vertex-group weight >= Mask Threshold;
//            joined into chains (continuing straight through vertices that
//            have exactly two chosen edges)
//   strokes  Grease Pencil strokes dropped on the nearest surface: a bead
//            anywhere
//   bead     a closed tube whose visible half is a real weld section: the two
//            toes sit on the plates (on a corner one on each face, legs of
//            equal length, so the chord between the toes is Width), the face
//            between them is a convex crown that runs out flat into the plates
//            (wetted toes, no sausage glued on) - an inside corner gets a
//            fillet with a slightly convex face, an outside corner a bead
//            wrapped over the edge, a surface a bead on plate. The other half
//            of the tube is buried in the metal.
//            Ripples: the overlapping solidified pool fronts ("stack of
//            dimes") - rounded scallops with a sharp groove between them,
//            phase s / Pitch shifted by Curvature x lateral^2 (arcs that
//            trail at the toes), Ripple Height of the bead height, fading
//            out at the toes. Wobble is the welder's hand: width, line,
//            height, ripple spacing and ripple height vary with it, and it
//            throws spatter (small flattened droplets on the plates near the
//            toes; none at Wobble 0). A bead starts with a round, slightly
//            high cold start and ends in a crater (the dip left where the
//            arc stopped). UV: U along (in Width units), V across.
//            profile (per vertex): 0 at the toes .. 1 on the crown.
#include "weld.h"
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
double R01(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}
double Noise1(double x, uint64_t seed)
{
	const double f = std::floor(x), t = x - f;
	const double a = R01(seed, uint64_t(int64_t(f) + (1 << 20)), 7) * 2 - 1, b = R01(seed, uint64_t(int64_t(f) + 1 + (1 << 20)), 7) * 2 - 1;
	const double s = t * t * (3 - 2 * t);
	return a + (b - a) * s;
}
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
double Smooth(double e0, double e1, double x)
{
	const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3 - 2 * t);
}

                                                                             
                                                                         
                                                                       
struct Path { std::vector<V> p, out, dl, dr; std::vector<double> concave; bool corner = false; };

                                                                          
                                                                   
double Crown(double x)
{
	const double q = std::max(0.0, 1.0 - x * x);
	return std::pow(q, 0.5 + 0.9 * x * x * x * x);
}
                                                                       
double Scallop(double fr)
{
	return 0.85 * (std::pow(std::sin(m::kPi * std::pow(std::clamp(fr, 0.0, 1.0), 0.7)), 0.75) - 0.6);
}

void AddRing(WeldResult& r, int sides, uint32_t base, int rows)
{
	const uint32_t row = (uint32_t)sides + 1;
	for (int i = 0; i + 1 < rows; ++i)
		for (int k = 0; k < sides; ++k)
		{
			const uint32_t a = base + (uint32_t)i * row + (uint32_t)k, b = a + 1, c = a + row + 1, d = a + row;
			r.polys.push_back({ (Int32)a, (Int32)d, (Int32)c, (Int32)b });             
		}
}

                                                                                
void Droplet(const V& at, const V& n, const V& t, double rad, double u, const WeldParams& w, WeldResult& r)
{
	const int sides = 8, rows = 5;
	const V a = Unit(t - n * m::la::dot(t, n), V(1, 0, 0)), b = m::la::cross(n, a);
	const uint32_t base = (uint32_t)r.points.size();
	for (int j = 0; j < rows; ++j)
	{
		const double th = -0.5 * m::kPi + m::kPi * j / (rows - 1);                         
		const double cr = std::cos(th), sr = std::sin(th);
		for (int k = 0; k <= sides; ++k)
		{
			const double ang = 2.0 * m::kPi * k / sides;
			const V q = at + (a * std::cos(ang) + b * std::sin(ang)) * (rad * cr) + n * (rad * (sr > 0 ? 0.55 * sr : 0.35 * sr) - 0.12 * rad);
			r.points.push_back(q);
			r.uvs.push_back(u * w.uvScale);
			r.uvs.push_back(((double)k / sides * 0.2 + 0.4) * w.uvScale);
			r.profile.push_back((float)(0.5 * std::max(0.0, sr)));
		}
	}
	AddRing(r, sides, base, rows);
}

void Bead(const Path& path, const WeldParams& w, uint64_t seed, WeldResult& r)
{
	if (path.p.size() < 2) return;
	                        
	std::vector<double> acc(1, 0.0);
	for (size_t i = 1; i < path.p.size(); ++i) acc.push_back(acc.back() + Len(path.p[i] - path.p[i - 1]));
	const double total = acc.back();
	if (total < 1e-9) return;
	const bool closed = path.p.size() > 2 && Len(path.p.front() - path.p.back()) < 1e-9 * std::max(1.0, total);
	                                                                                      
	const double step = std::max(1e-6, std::min(w.width / 6.0, w.pitch / 6.0) / std::max(0.25, w.detail));
	const int n = std::max(2, (int)std::ceil(total / step) + 1);
	const bool hasD = path.corner && path.dl.size() == path.p.size();
	std::vector<V> P(n), O(n), DL(n), DR(n);
	std::vector<double> CC(n, 0.0);
	size_t seg = 0;
	for (int i = 0; i < n; ++i)
	{
		const double s = total * i / (n - 1);
		while (seg + 2 < acc.size() && acc[seg + 1] < s) ++seg;
		const double t = (s - acc[seg]) / std::max(1e-12, acc[seg + 1] - acc[seg]);
		P[i] = path.p[seg] + (path.p[seg + 1] - path.p[seg]) * t;
		O[i] = Unit(path.out[seg] * (1 - t) + path.out[seg + 1] * t, V(0, 0, 1));
		if (hasD)
		{
			DL[i] = path.dl[seg] * (1 - t) + path.dl[seg + 1] * t;
			DR[i] = path.dr[seg] * (1 - t) + path.dr[seg + 1] * t;
			CC[i] = path.concave[seg] * (1 - t) + path.concave[seg + 1] * t;
		}
	}
	const int sides = std::clamp(w.sides, 6, 48);
	                                                                                   
	const int top = std::clamp((int)std::lround(sides * 0.75), 4, sides - 2);
	const uint32_t base = (uint32_t)r.points.size();
	const double H = w.height, W = w.width;
	const double wob = std::max(0.0, w.wobble);
	const double pitch = std::max(1e-9, w.pitch);
	                                                      
	const double startL = closed ? 0.0 : std::min(0.4 * total, 0.6 * W);
	const double stopL = closed ? 0.0 : std::min(0.4 * total, 0.9 * W);
	struct Drop { V at, n, t; double rad, u; };
	std::vector<Drop> drops;
	for (int i = 0; i < n; ++i)
	{
		const double s = total * i / (n - 1);
		const V t = Unit(P[std::min(n - 1, i + 1)] - P[std::max(0, i - 1)], V(1, 0, 0));
		const V out = Unit(O[i] - t * m::la::dot(O[i], t), O[i]);
		const V side = Unit(m::la::cross(t, out), V(0, 1, 0));
		                                                                 
		const double wv = 1.0 + wob * 0.30 * Noise1(s / (1.5 * W), seed) + wob * 0.06 * Noise1(s / (0.45 * W), seed ^ 0x77);
		const double hv = 1.0 + wob * 0.12 * Noise1(s / (2.3 * W), seed ^ 0x99);
		const double drift = wob * 0.08 * W * Noise1(s / (3.0 * W), seed ^ 0x55);
		                           
		double taper = 1.0, crater = 0.0, coldStart = 0.0;
		if (!closed)
		{
			const double eA = std::clamp(s / std::max(1e-9, startL), 0.0, 1.0), eB = std::clamp((total - s) / std::max(1e-9, 0.55 * stopL), 0.0, 1.0);
			taper = std::sqrt(std::min(eA, eB) * (2.0 - std::min(eA, eB)));
			                                                                  
			const double dc = (total - s - 0.55 * stopL) / std::max(1e-9, 0.45 * stopL);                            
			crater = std::exp(-dc * dc * 2.2);
			coldStart = std::exp(-std::pow((s - 0.8 * startL) / std::max(1e-9, 0.6 * startL), 2.0));
		}
		const double halfW = 0.5 * W * wv * std::max(0.05, taper);
		const double h = H * hv * std::max(0.05, taper) * (1.0 + 0.12 * coldStart);
		                                      
		V dl = -side, dr = side, nl = out, nr = out;
		double concave = 0.0;
		if (hasD)
		{
			dl = Unit(DL[i] - t * m::la::dot(DL[i], t), -side);
			dr = Unit(DR[i] - t * m::la::dot(DR[i], t), side);
			if (m::la::dot(dl, side) > 0.0) dl = -side;                                      
			if (m::la::dot(dr, side) < 0.0) dr = side;
			nl = Unit(m::la::cross(dl, t), out); if (m::la::dot(nl, out) < 0.0) nl = -nl;
			nr = Unit(m::la::cross(dr, t), out); if (m::la::dot(nr, out) < 0.0) nr = -nr;
			concave = std::clamp(CC[i], 0.0, 1.0);
		}
		                                                     
		const double half = 0.5 * Len(dl - dr);
		const double leg = halfW / std::clamp(half, 0.35, 1.0);
		const V E = P[i] + side * drift;
		const double sink = 0.08 * h;
		const V TL = E + dl * leg - nl * sink, TR = E + dr * leg - nr * sink;
		const V M = (TL + TR) * 0.5;
		                                                                       
		                                                                     
		                                                                     
		           
		V C = M;
		double crownH = h;
		if (hasD)
		{
			const V Cin = E + (M - E) * 0.6, Cout = E;
			C = Cout + (Cin - Cout) * concave;
			const V B5 = (TL + TR) * 0.25 + C * 0.5;
			const double depth = m::la::dot(E - B5, out);                                             
			                                                                               
			crownH = concave * (std::max(0.0, m::la::dot(M - B5, out)) + 0.2 * h) + (1.0 - concave) * (h + std::max(0.0, depth));
		}
		const double rippleAmp = 0.7 * w.rippleHeight * h;
		                                                                                      
		const double lag = w.curvature * 0.45 * W / pitch;
		for (int k = 0; k <= sides; ++k)
		{
			V q;
			double prof = 0.0;
			if (k <= top)
			{
				                                                                       
				const double lin = 1.0 - 2.0 * k / top, cheb = std::cos(m::kPi * k / top);
				const double x = 0.5 * (lin + cheb);
				const double u = 0.5 * (1.0 - x);                          
				const V B = TR * ((1 - u) * (1 - u)) + C * (2 * u * (1 - u)) + TL * (u * u);
				const double g = Crown(x);
				                                                                     
				const double phase = s / pitch + lag * x * x + wob * 0.3 * Noise1(s / (3.0 * pitch), seed ^ 0x3131);
				const double fl = std::floor(phase);
				const double amp = 1.0 + wob * 0.35 * (R01(seed ^ 0xABCD, uint64_t(int64_t(fl) + (1 << 24)), 3) * 2 - 1);
				const double ripple = rippleAmp * amp * Scallop(phase - fl) * std::pow(g, 1.5) * std::max(0.0, taper);
				                                                           
				const double crat = h * crater * (-0.28 * std::pow(g, 1.5) + 0.08 * Smooth(0.2, 0.7, 1.0 - g) * g);
				q = B + out * (crownH * std::pow(g, 1.0 + concave) + ripple + crat);
				prof = g * std::max(0.0, taper);
			}
			else
			{
				                                                        
				const double v = double(k - top) / double(sides - top);
				const double dig = std::max(0.35 * h, 0.4 * halfW);
				q = TL + (TR - TL) * v - out * (dig * std::sin(m::kPi * v));
			}
			r.points.push_back(q);
			r.uvs.push_back(s / std::max(1e-9, W) * w.uvScale);
			r.uvs.push_back((double)k / sides * w.uvScale);
			r.profile.push_back((float)prof);
		}
		                                                                    
		                                                
		if (wob > 0.0 && i + 1 < n)
		{
			const double ds = total / (n - 1);
			const double rate = wob * 2.0 / W;                                       
			const double expect = rate * ds;
			for (int j = 0; j < 3; ++j)
			{
				if (R01(seed ^ 0x5A77E5ULL, (uint64_t)i, (uint64_t)j) >= expect / 3.0) continue;
				const double r1 = R01(seed ^ 0x5A77E6ULL, (uint64_t)i, (uint64_t)j);
				const double r2 = R01(seed ^ 0x5A77E7ULL, (uint64_t)i, (uint64_t)j);
				const double r3 = R01(seed ^ 0x5A77E8ULL, (uint64_t)i, (uint64_t)j);
				const bool left = r1 < 0.5;
				const double dist = leg + W * (0.08 + 1.4 * r2 * r2);                                 
				const V d = left ? dl : dr, nn = left ? nl : nr;
				const double rad = W * (0.025 + 0.055 * r3 * r3) * (0.6 + 0.4 * std::min(1.0, wob));
				drops.push_back({ E + d * dist + t * (ds * r1), nn, t, rad, s / std::max(1e-9, W) });
			}
		}
	}
	AddRing(r, sides, base, n);
	for (const Drop& d : drops) Droplet(d.at, d.n, d.t, d.rad, d.u, w, r);
	++r.beads;
}

}               

// основной вход: собираем геометрию по переданным настройкам
bool BuildWelds(const MeshView& mv, const std::vector<float>& weights, const std::vector<std::vector<EVec>>& strokes,
	const WeldParams& w, WeldResult& r, const WeldEdgePairs* selectedEdges, const WeldEdgePairs* excludedEdges)
{
	r = WeldResult();
	if (mv.npolys <= 0 && !selectedEdges) return false;
	const uint64_t seed = 0x57454C44ULL ^ (uint64_t(uint32_t(w.seed)) * 0x9e3779b97f4a7c15ULL);
	               
	std::vector<V> fn((size_t)mv.npolys);
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		V s = m::la::cross(V(mv.pts[q.b]) - V(mv.pts[q.a]), V(mv.pts[q.c]) - V(mv.pts[q.a]));
		if (q.c != q.d) s += m::la::cross(V(mv.pts[q.c]) - V(mv.pts[q.a]), V(mv.pts[q.d]) - V(mv.pts[q.a]));
		fn[(size_t)i] = GuidedNormal(mv, i, Unit(s, V(0, 0, 1)));
	}
	                     
	if (w.edgesEnable)
	{
		std::map<std::pair<Int32, Int32>, std::vector<Int32>> ef;
		if (selectedEdges)
			for (const auto& edge : *selectedEdges)
			{
				const auto [a, b] = std::minmax(edge[0], edge[1]);
				if (a >= 0 && b < mv.npts && a != b && Len(V(mv.pts[b]) - V(mv.pts[a])) > 1e-12)
					ef.try_emplace({a, b});
			}
		for (Int32 i = 0; i < mv.npolys; ++i)
		{
			const ProjectDamagePoly& q = mv.polys[i];
			const Int32 ids[4] = { q.a, q.b, q.c, q.d };
			const int nv = q.c == q.d ? 3 : 4;
			for (int k = 0; k < nv; ++k)
			{
				const Int32 a = ids[k], b = ids[(k + 1) % nv];
				const std::pair<Int32, Int32> edge{std::min(a, b), std::max(a, b)};
				if (!selectedEdges) ef[edge].push_back(i);
				else if (auto it = ef.find(edge); it != ef.end()) it->second.push_back(i);
			}
		}
		if (excludedEdges)
			for (const auto& edge : *excludedEdges)
				ef.erase({std::min(edge[0], edge[1]), std::max(edge[0], edge[1])});
		const double cosLim = std::cos(w.angle * m::kPi / 180.0);
		auto weightOf = [&](Int32 v) { return weights.empty() ? 1.0f : ((size_t)v < weights.size() ? weights[(size_t)v] : 0.0f); };
		                                                          
		auto awayOf = [&](Int32 f, const std::pair<Int32, Int32>& e) {
			const V a = V(mv.pts[e.first]), b = V(mv.pts[e.second]);
			const V ed = Unit(b - a, V(1, 0, 0));
			const ProjectDamagePoly& q = mv.polys[f];
			V sum(0.0);
			for (Int32 v : { q.a, q.b, q.c, q.d })
				if (v != e.first && v != e.second) { const V d = V(mv.pts[v]) - a; sum += d - ed * m::la::dot(d, ed); }
			return Unit(sum, m::la::cross(fn[(size_t)f], ed));
		};
		struct Edge { V out, away1, away2; double concave = 0.0; };
		std::map<std::pair<Int32, Int32>, Edge> chosen;                                                  
		for (const auto& e : ef)
		{
			if (e.second.size() != 2)
			{
				if (selectedEdges)
				{
					// A chosen edge can form a bead even on an open border or a loose wire.
					const V t = Unit(V(mv.pts[e.first.second]) - V(mv.pts[e.first.first]), V(1, 0, 0));
					const V axis = std::abs(t.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0);
					const V n = e.second.empty() ? axis : fn[(size_t)e.second.front()];
					const V out = Unit(n - t * m::la::dot(n, t), Unit(axis - t * m::la::dot(axis, t), V(0, 1, 0)));
					const V side = Unit(m::la::cross(t, out), V(0, 1, 0));
					chosen[e.first] = Edge{out, -side, side, 0.0};
				}
				continue;
			}
			const Int32 f1 = e.second[0], f2 = e.second[1];
			const V n1 = fn[(size_t)f1], n2 = fn[(size_t)f2];
			if (!selectedEdges && (m::la::dot(n1, n2) > cosLim
				|| weightOf(e.first.first) < w.maskThreshold || weightOf(e.first.second) < w.maskThreshold))
				continue;
			                                                           
			const ProjectDamagePoly& q2 = mv.polys[f2];
			V off(0.0);
			for (Int32 v : { q2.a, q2.b, q2.c, q2.d })
				if (v != e.first.first && v != e.first.second) { off = V(mv.pts[v]); break; }
			const bool concave = m::la::dot(off - V(mv.pts[e.first.first]), n1) > 0.0;
			if (!selectedEdges && w.edgeType == 0 && !concave) continue;
			if (!selectedEdges && w.edgeType == 1 && concave) continue;
			                                                                     
			chosen[e.first] = Edge{ Unit(n1 + n2, n1), awayOf(f1, e.first), awayOf(f2, e.first), concave ? 1.0 : 0.0 };
		}
		                                                              
		std::unordered_map<Int32, std::vector<std::pair<Int32, Int32>>> at;
		for (const auto& c : chosen) { at[c.first.first].push_back(c.first); at[c.first.second].push_back(c.first); }
		std::map<std::pair<Int32, Int32>, bool> used;
		auto walk = [&](std::pair<Int32, Int32> e, Int32 from) {
			Path path;
			path.corner = true;
			Int32 cur = from;
			auto push = [&](Int32 v, const std::pair<Int32, Int32>& edge, Int32 a, Int32 b) {
				const Edge& E = chosen[edge];
				                                              
				const V t = Unit(V(mv.pts[b]) - V(mv.pts[a]), V(1, 0, 0));
				const V side = m::la::cross(t, E.out);
				const bool firstLeft = m::la::dot(E.away1, side) < m::la::dot(E.away2, side);
				path.p.push_back(V(mv.pts[v]));
				path.out.push_back(E.out);
				path.dl.push_back(firstLeft ? E.away1 : E.away2);
				path.dr.push_back(firstLeft ? E.away2 : E.away1);
				path.concave.push_back(E.concave);
			};
			{
				const Int32 nxt = e.first == cur ? e.second : e.first;
				push(cur, e, cur, nxt);
			}
			for (int guard = 0; guard < 1000000; ++guard)
			{
				used[e] = true;
				const Int32 nxt = e.first == cur ? e.second : e.first;
				push(nxt, e, cur, nxt);
				cur = nxt;
				const auto& list = at[cur];
				if (list.size() != 2) break;
				const std::pair<Int32, Int32> e2 = list[0] == e ? list[1] : list[0];
				if (used[e2]) break;
				e = e2;
			}
			return path;
		};
		std::vector<Path> paths;
		for (const auto& c : chosen)                                               
			for (Int32 end : { c.first.first, c.first.second })
				if (!used[c.first] && at[end].size() != 2) paths.push_back(walk(c.first, end));
		for (const auto& c : chosen)                       
			if (!used[c.first]) paths.push_back(walk(c.first, c.first.first));
		for (size_t i = 0; i < paths.size(); ++i) Bead(paths[i], w, seed ^ (i * 0x2545F4914F6CDD1DULL), r);
	}
	                                                      
	if (w.strokesEnable)
		for (size_t si = 0; si < strokes.size(); ++si)
		{
			Path path;
			for (const V& sp : strokes[si])
			{
				double best = 1e300; V q(0.0), n(0, 0, 1);
				for (Int32 i = 0; i < mv.npolys; ++i)
				{
					const ProjectDamagePoly& pq = mv.polys[i];
					const Int32 ids[4] = { pq.a, pq.b, pq.c, pq.d };
					for (int k = 1; k + 1 < (pq.c == pq.d ? 3 : 4); ++k)
					{
						const V c = ClosestOnTri(sp, mv.pts[ids[0]], mv.pts[ids[k]], mv.pts[ids[k + 1]]);
						const double d = Len(c - sp);
						if (d < best) { best = d; q = c; n = fn[(size_t)i]; }
					}
				}
				if (best > w.strokeReach) continue;                                             
				path.p.push_back(q); path.out.push_back(n);
			}
			                                                    
			for (int it = 0; it < 3; ++it)
				for (size_t i = 1; i + 1 < path.out.size(); ++i) path.out[i] = Unit(path.out[i - 1] + path.out[i] * 2.0 + path.out[i + 1], path.out[i]);
			Bead(path, w, seed ^ (0xD7A3ULL + si * 0x9E3779B97F4A7C15ULL), r);
		}
	return r.beads > 0;
}

PROJECT_DAMAGE_NS_MACRO_END
