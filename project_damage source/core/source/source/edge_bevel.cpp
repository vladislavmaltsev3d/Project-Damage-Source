// обрабатываем форму фаски и изношенные участки рёбер
// ProjectDamage edge bevel — GPL-3.0-or-later; see LICENSE.
//
// Recipe: edge-bevel-v1. Kerb-style edge profiles cut into the source mesh,
// with an edge wear layer on top:
//   1. edges: every edge with exactly two faces whose normals turn by more
//      than `angle` and that is convex. With grease pencil strokes, only the
//      edges a stroke runs along (within paintRadius, roughly parallel, over
//      paintCover of the edge). Partial mode keeps only the drawn stretch;
//   2. runs: picked edges are joined into runs where they meet two at a
//      vertex and turn less than maxTurn with matching faces, so arc length,
//      taper, wander and the paint fade are continuous along a run. Each run
//      has a "top" face (closer to `up`) and a "side" face;
//   3. cutter: per edge, a straight sweep of one cross-section polygon (the
//      profile curve from the top face to the side face, closed by three
//      points floating above the edge) through stations along the edge.
//      Station sizes vary (taper, wear wander, paint fade, mask), the frame
//      does not, so every sweep is a simple valid solid. Neighbouring edges
//      of a run overlap slightly at the joint; at a free end of a run the
//      sweep runs on past the vertex when that space is outside the solid
//      (a box corner), and stops flat at the vertex otherwise (an edge that
//      runs into a wall). Where three rounded runs end at one box corner, a
//      corner cutter (box minus sphere) makes the corner spherical;
//   4. sizes: scene units, or fractions of the local width across the top
//      face measured with a ray through the solid, clamped to maxShare of the
//      local width and height so opposite bevels never cut through;
//   5. edge wear: the bevel size wanders along the run, the bevel surface is
//      dented, flattened spall chips are knocked out of the two bevel borders
//      and the crest, and EdgeWearMask gives a 0..1 point weight.
// Random draws: SplitMix64 counter channels keyed by (run, item, channel).
// Channels: chips 1 arc, 2 line, 3 size, 4 depth, 5 stretch, 6 twist,
// 7 big, 8-10 noise offset; run 50-52 noise offsets.
#include "manifold/manifold.h"
#include "edge_bevel.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
constexpr double kPi = 3.14159265358979323846;

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
double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double e0, double e1, double x)
{
	if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
                                    
double Noise(const V& p) { return (ProjectDamageNoise(p) - 0.5) * 2.0; }

double SegDist(const V& p, const V& a, const V& b, double* tOut = nullptr)
{
	const V ab = b - a;
	const double l2 = m::la::dot(ab, ab);
	double t = l2 > 1e-24 ? m::la::dot(p - a, ab) / l2 : 0.0;
	t = ClampD(t, 0.0, 1.0);
	if (tOut) *tOut = t;
	return m::la::length(p - (a + ab * t));
}

                                                                              
                
struct MeshEdge
{
	int32_t v0 = 0, v1 = 0;                            
	int32_t f0 = -1, f1 = -1;
	bool sharp = false;
};

struct Topo
{
	std::vector<V> polyN, polyC;
	std::vector<MeshEdge> edges;
	double diag = 1.0;
};

// основной вход: собираем геометрию по переданным настройкам
void BuildTopo(const MeshView& mv, double angleDeg, const std::vector<bool>* avoid, Topo& t)
{
	const int32_t np = mv.npolys;
	t.polyN.assign((size_t)np, V(0.0));
	t.polyC.assign((size_t)np, V(0.0));
	V lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
	for (int32_t i = 0; i < mv.npts; i++)
	{
		lo = m::la::min(lo, mv.pts[i]);
		hi = m::la::max(hi, mv.pts[i]);
	}
	t.diag = std::max(1e-9, m::la::length(hi - lo));
	std::map<std::pair<int32_t, int32_t>, size_t> key;
	std::vector<int> count;
	for (int32_t f = 0; f < np; f++)
	{
		const ProjectDamagePoly& q = mv.polys[f];
		const int32_t idx[4] = { q.a, q.b, q.c, q.d };
		const int n = q.c == q.d ? 3 : 4;
		V nn(0.0), cc(0.0);
		for (int k = 0; k < n; k++)
		{
			const V& a = mv.pts[idx[k]];
			const V& b = mv.pts[idx[(k + 1) % n]];
			nn.x += (a.y - b.y) * (a.z + b.z);
			nn.y += (a.z - b.z) * (a.x + b.x);
			nn.z += (a.x - b.x) * (a.y + b.y);
			cc = cc + a;
		}
		t.polyN[(size_t)f] = GuidedNormal(mv, f, Unit(nn, V(0.0)));
		t.polyC[(size_t)f] = cc / double(n);
		for (int k = 0; k < n; k++)
		{
			const int32_t a = idx[k], b = idx[(k + 1) % n];
			if (a == b) continue;
			const auto kk = std::make_pair(std::min(a, b), std::max(a, b));
			auto it = key.find(kk);
			if (it == key.end())
			{
				key.emplace(kk, t.edges.size());
				MeshEdge e;
				e.v0 = a; e.v1 = b; e.f0 = f;
				t.edges.push_back(e);
				count.push_back(1);
			}
			else
			{
				MeshEdge& e = t.edges[it->second];
				count[it->second]++;
				if (e.f1 < 0 && e.v0 == b && e.v1 == a) e.f1 = f;
			}
		}
	}
	const double cosT = std::cos(ClampD(angleDeg, 1.0, 179.0) * kPi / 180.0);
	for (size_t i = 0; i < t.edges.size(); i++)
	{
		MeshEdge& e = t.edges[i];
		if (count[i] != 2 || e.f1 < 0) continue;
		const V& nA = t.polyN[(size_t)e.f0];
		const V& nB = t.polyN[(size_t)e.f1];
		if (m::la::length(nA) < 0.5 || m::la::length(nB) < 0.5) continue;
		if (m::la::dot(nA, nB) > cosT) continue;
		const V mid = (mv.pts[e.v0] + mv.pts[e.v1]) * 0.5;
		                                                      
		const double s0 = m::la::dot(nA, t.polyC[(size_t)e.f1] - mid);
		const double s1 = m::la::dot(nB, t.polyC[(size_t)e.f0] - mid);
		if (s0 >= -1e-9 * t.diag || s1 >= -1e-9 * t.diag) continue;
		if (avoid)
		{
			const size_t na = avoid->size();
			if (((size_t)e.f0 < na && (*avoid)[(size_t)e.f0]) || ((size_t)e.f1 < na && (*avoid)[(size_t)e.f1]))
				continue;
		}
		e.sharp = true;
	}
}

                                                                              
          
struct Strokes
{
	struct S { V a, b, d; };
	std::vector<S> segs;
	double radius = 4.0, cosAlign = 0.7;
	bool Hit(const V& p, const V& dir) const
	{
		for (const S& s : segs)
		{
			if (std::abs(m::la::dot(s.d, dir)) < cosAlign) continue;
			if (SegDist(p, s.a, s.b) <= radius) return true;
		}
		return false;
	}
};

                                                                              
                                
struct RunEdge
{
	int32_t edge;                            
	int32_t p0, p1;                  
	int32_t fTop, fSide;
};
struct Run
{
	std::vector<RunEdge> e;
	bool closed = false;
};

struct Frame { V t, nA, nB, dA, dB; };

Frame MakeFrame(const MeshView& mv, const Topo& tp, const RunEdge& re)
{
	Frame f;
	f.t = Unit(mv.pts[re.p1] - mv.pts[re.p0], V(1.0, 0.0, 0.0));
	f.nA = tp.polyN[(size_t)re.fTop];
	f.nB = tp.polyN[(size_t)re.fSide];
	f.dA = Unit(m::la::cross(f.nA, f.t), V(0.0));
	if (m::la::dot(f.dA, f.nB) > 0.0) f.dA = -f.dA;
	f.dB = Unit(m::la::cross(f.nB, f.t), V(0.0));
	if (m::la::dot(f.dB, f.nA) > 0.0) f.dB = -f.dB;
	return f;
}

                                                     
bool Inside(const ProjectDamageBVH& bvh, const V& p, double diag)
{
	static const V dirs[3] = {
		Unit(V(0.5773, 0.6123, 0.5401), V(1, 0, 0)),
		Unit(V(-0.7071, 0.3013, 0.6396), V(1, 0, 0)),
		Unit(V(0.2117, -0.8312, -0.5138), V(1, 0, 0)) };
	int votes = 0;
	for (const V& d : dirs)
	{
		V o = p;
		int hits = 0;
		double remain = 4.0 * diag;
		for (int k = 0; k < 256 && remain > 0.0; k++)
		{
			double h = 0.0;
			if (!bvh.Raycast(o, d, remain, &h)) break;
			hits++;
			const double step = h + 1e-7 * diag;
			o = o + d * step;
			remain -= step;
		}
		if (hits & 1) votes++;
	}
	return votes >= 2;
}

                                                                              
                                                                          
                                                                
void Profile(const EdgeBevelParams& p, double a, double b, int segs, std::vector<std::pair<double, double>>& out)
{
	out.clear();
	const double q = ClampD(p.roundness, 0.3, 12.0);
	if (p.profile == 1 || p.profile == 2)
	{
		for (int i = 0; i <= segs; i++)
		{
			const double th = (kPi * 0.5) * double(i) / double(segs);                       
			const double c = std::cos(th), s = std::sin(th);
			const double pc = std::pow(std::max(0.0, c), 2.0 / q);
			const double ps = std::pow(std::max(0.0, s), 2.0 / q);
			if (p.profile == 1)
				out.emplace_back(a - a * ps, b - b * pc);                              
			else
				out.emplace_back(a * pc, b * ps);                                       
		}
		out.front() = { a, 0.0 };
		out.back() = { 0.0, b };
		return;
	}
	if (p.profile == 3)
	{
		out.emplace_back(a, 0.0);
		out.emplace_back(a, b);
		out.emplace_back(0.0, b);
		return;
	}
	                                                                          
	                                                                            
	                                      
	double lt = ClampD(p.filletTop, 0.0, 1.0) * a;
	double ls = ClampD(p.filletSide, 0.0, 1.0) * b;
	if (lt <= 1e-12 && ls <= 1e-12)
	{
		out.emplace_back(a, 0.0);
		out.emplace_back(0.0, b);
		return;
	}
	const double sx = a - lt, qy = b - ls;                                           
	const double L = std::sqrt(sx * sx + qy * qy);
	if (L <= 1e-12) { out.emplace_back(a, 0.0); out.emplace_back(0.0, b); return; }
	const double ux = -sx / L, uy = qy / L;                                  
	                                                                          
	double kt = lt, ks = ls;
	if (kt + ks > 0.98 * L) { const double f = 0.98 * L / (kt + ks); kt *= f; ks *= f; }
	const int n = std::max(2, segs / 2);
	auto bez = [&](double x0, double y0, double x1, double y1, double x2, double y2, bool skipFirst)
	{
		for (int i = skipFirst ? 1 : 0; i <= n; i++)
		{
			const double t = double(i) / double(n), u = 1.0 - t;
			out.emplace_back(u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
		}
	};
	                                       
	if (lt > 1e-12) bez(a, 0.0, sx, 0.0, sx + ux * kt, uy * kt, false);
	else out.emplace_back(a, 0.0);
	                                        
	if (ls > 1e-12) bez(-ux * ks, qy - uy * ks, 0.0, qy, 0.0, b, false);
	else out.emplace_back(0.0, b);
}

                    
struct Station { V pos; double a, b; };

                                                                      
m::Manifold Sweep(const EdgeBevelParams& p, const Frame& f, const std::vector<Station>& st,
	int segs, double rough, const V& roughOff, double diag)
{
	if (st.size() < 2) return m::Manifold();
	std::vector<std::pair<double, double>> prof;
	Profile(p, 1.0, 1.0, segs, prof);                                          
	const int K = (int)prof.size();
	const int M = K + 3;
	const int R = (int)st.size();
	m::MeshGL64 mg;
	mg.numProp = 3;
	mg.vertProperties.reserve((size_t)R * M * 3);
	const V bis = Unit(f.nA + f.nB, f.nA);
	for (int r = 0; r < R; r++)
	{
		const Station& s = st[(size_t)r];
		const double a = std::max(s.a, 1e-6 * diag), b = std::max(s.b, 1e-6 * diag);
		std::vector<std::pair<double, double>> pr;
		Profile(p, a, b, segs, pr);
		const double mg0 = 0.35 * std::max(a, b) + 1e-5 * diag;
		const double lift = 1e-5 * std::min(a, b);
		auto push = [&](const V& v) { mg.vertProperties.push_back(v.x); mg.vertProperties.push_back(v.y); mg.vertProperties.push_back(v.z); };
		for (int k = 0; k < K; k++)
		{
			V v = s.pos + f.dA * pr[(size_t)k].first + f.dB * pr[(size_t)k].second + bis * lift;
			if (rough > 0.0 && k > 0 && k < K - 1)
			{
				const double sc = 0.6 * std::max(1e-6, 0.5 * (a + b));
				v = v + bis * (rough * 0.12 * std::min(a, b) * Noise(v / sc + roughOff));
			}
			push(v);
		}
		push(s.pos + f.dB * b + f.nB * mg0);            
		push(s.pos + (f.nA + f.nB) * mg0);              
		push(s.pos + f.dA * a + f.nA * mg0);            
	}
	auto vid = [&](int r, int j) { return (uint64_t)(r * M + ((j % M) + M) % M); };
	std::vector<uint64_t>& tv = mg.triVerts;
	for (int r = 0; r + 1 < R; r++)
		for (int j = 0; j < M; j++)
		{
			const uint64_t a0 = vid(r, j), a1 = vid(r, j + 1), b0 = vid(r + 1, j), b1 = vid(r + 1, j + 1);
			tv.insert(tv.end(), { a0, b0, b1, a0, b1, a1 });
		}
	const int o2 = K + 1;
	for (int j = 0; j < M; j++)
	{
		const int j1 = (j + 1) % M;
		if (j == o2 || j1 == o2) continue;
		tv.insert(tv.end(), { vid(0, o2), vid(0, j), vid(0, j1) });
		tv.insert(tv.end(), { vid(R - 1, o2), vid(R - 1, j1), vid(R - 1, j) });
	}
	                 
	double vol = 0.0;
	const auto& vp = mg.vertProperties;
	for (size_t i = 0; i + 2 < tv.size(); i += 3)
	{
		const V A(vp[tv[i] * 3], vp[tv[i] * 3 + 1], vp[tv[i] * 3 + 2]);
		const V B(vp[tv[i + 1] * 3], vp[tv[i + 1] * 3 + 1], vp[tv[i + 1] * 3 + 2]);
		const V C(vp[tv[i + 2] * 3], vp[tv[i + 2] * 3 + 1], vp[tv[i + 2] * 3 + 2]);
		vol += m::la::dot(A, m::la::cross(B, C));
	}
	if (vol < 0.0)
		for (size_t i = 0; i + 2 < tv.size(); i += 3) std::swap(tv[i + 1], tv[i + 2]);
	m::Manifold out(mg);
	if (out.Status() != m::Manifold::Error::NoError || out.IsEmpty()) return m::Manifold();
	return out;
}

}             

                                                                              
void PlaceEdgeBevel(const CoreInput& in, const EdgeBevelParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& bevels, std::vector<m::Manifold>& wear, EdgeWearField* field)
{
	if (!pIn.enable) return;
	const MeshView& mv = in.view;
	if (mv.npts <= 0 || mv.npolys <= 0 || !mv.pts || !mv.polys) return;
	EdgeBevelParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.profile = std::max(0, std::min(p.profile, 3));
	p.sizeTop = std::max(0.0, p.sizeTop);
	p.sizeSide = std::max(0.0, p.sizeSide);
	if (p.sizeTop <= 0.0 || p.sizeSide <= 0.0) return;
	p.maxShare = ClampD(p.maxShare, 0.01, 0.99);
	p.taper = ClampD(p.taper, 0.0, 1.0);
	p.paintRadius = std::max(1e-6, p.paintRadius);
	p.paintTaper = std::max(0.0, p.paintTaper);
	p.wearUneven = ClampD(p.wearUneven, 0.0, 1.0);
	p.wearUnevenScale = std::max(1e-6, p.wearUnevenScale);
	p.wearRough = ClampD(p.wearRough, 0.0, 1.0);
	if (p.wearChipMax < p.wearChipMin) std::swap(p.wearChipMin, p.wearChipMax);
	if (!p.wearEnable) { p.wearUneven = 0.0; p.wearRough = 0.0; p.wearChips = 0.0; }

	const bool haveStrokes = paintStrokes && !paintStrokes->empty();
	if (!haveStrokes && p.drawnOnly) return;

	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4544474542564c31ULL)                
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4542;                             

	Topo tp;
	BuildTopo(mv, p.angle, p.avoid ? in.avoidPoly : nullptr, tp);
	const double diag = tp.diag;
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0.0, 0.0, 1.0));

	Strokes sk;
	sk.radius = p.paintRadius;
	sk.cosAlign = std::cos(ClampD(p.paintAlign, 0.0, 90.0) * kPi / 180.0);
	if (haveStrokes)
		for (const auto& s : *paintStrokes)
			for (size_t i = 0; i + 1 < s.size(); i++)
			{
				const V a = s[i], b = s[i + 1];
				if (!std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z)) continue;
				sk.segs.push_back({ a, b, Unit(b - a, V(1.0, 0.0, 0.0)) });
			}
	if (haveStrokes && sk.segs.empty()) return;

	                                                                                 
	std::vector<char> picked(tp.edges.size(), 0);
	for (size_t i = 0; i < tp.edges.size(); i++)
	{
		const MeshEdge& e = tp.edges[i];
		if (!e.sharp) continue;
		if (sk.segs.empty()) { picked[i] = 1; continue; }
		const V a = mv.pts[e.v0], b = mv.pts[e.v1];
		const double L = m::la::length(b - a);
		const V d = Unit(b - a, V(1.0, 0.0, 0.0));
		const int n = (int)ClampD(std::ceil(L / (0.5 * p.paintRadius)), 2.0, 400.0);
		int hit = 0;
		for (int k = 0; k <= n; k++)
			if (sk.Hit(a + (b - a) * (double(k) / n), d)) hit++;
		const double cover = double(hit) / double(n + 1);
		if (p.paintPartial ? hit > 0 : cover >= p.paintCover) picked[i] = 1;
	}

	                                                                                   
	std::unordered_map<int32_t, std::vector<int32_t>> inc;
	for (size_t i = 0; i < tp.edges.size(); i++)
		if (picked[i]) { inc[tp.edges[i].v0].push_back((int32_t)i); inc[tp.edges[i].v1].push_back((int32_t)i); }
	const double cosTurn = std::cos(ClampD(p.maxTurn, 0.0, 89.0) * kPi / 180.0);
	auto other = [&](int32_t ei, int32_t v) { return tp.edges[(size_t)ei].v0 == v ? tp.edges[(size_t)ei].v1 : tp.edges[(size_t)ei].v0; };
	                                                                         
	auto joins = [&](int32_t ei, int32_t ej, int32_t v) -> bool
	{
		const V pv = mv.pts[v];
		const V din = Unit(pv - mv.pts[other(ei, v)], V(0.0));
		const V dout = Unit(mv.pts[other(ej, v)] - pv, V(0.0));
		if (m::la::dot(din, dout) < cosTurn) return false;
		const MeshEdge& a = tp.edges[(size_t)ei];
		const MeshEdge& b = tp.edges[(size_t)ej];
		const V a0 = tp.polyN[(size_t)a.f0], a1 = tp.polyN[(size_t)a.f1];
		const V b0 = tp.polyN[(size_t)b.f0], b1 = tp.polyN[(size_t)b.f1];
		const double s1 = std::min(m::la::dot(a0, b0), m::la::dot(a1, b1));
		const double s2 = std::min(m::la::dot(a0, b1), m::la::dot(a1, b0));
		return std::max(s1, s2) >= cosTurn;
	};
	auto nextAt = [&](int32_t ei, int32_t v) -> int32_t
	{
		auto it = inc.find(v);
		if (it == inc.end() || it->second.size() != 2) return -1;
		const int32_t ej = it->second[0] == ei ? it->second[1] : it->second[0];
		if (ej == ei) return -1;
		return joins(ei, ej, v) ? ej : -1;
	};
	std::vector<char> used(tp.edges.size(), 0);
	std::vector<Run> runs;
	for (size_t s = 0; s < tp.edges.size(); s++)
	{
		if (!picked[s] || used[s]) continue;
		                                                       
		int32_t cur = (int32_t)s, v = tp.edges[s].v0;
		bool closed = false;
		for (size_t guard = 0; guard < tp.edges.size(); guard++)
		{
			const int32_t pe = nextAt(cur, v);
			if (pe < 0 || used[(size_t)pe]) break;
			if (pe == (int32_t)s) { closed = true; break; }
			v = other(pe, v);
			cur = pe;
		}
		                             
		Run run;
		run.closed = closed;
		int32_t e = closed ? (int32_t)s : cur;
		int32_t p0 = closed ? tp.edges[s].v0 : v;
		for (size_t guard = 0; guard <= tp.edges.size(); guard++)
		{
			if (used[(size_t)e]) break;
			used[(size_t)e] = 1;
			RunEdge re;
			re.edge = e; re.p0 = p0; re.p1 = other(e, p0);
			run.e.push_back(re);
			const int32_t ne = nextAt(e, re.p1);
			if (ne < 0 || used[(size_t)ne]) break;
			p0 = re.p1;
			e = ne;
		}
		                                             
		V prevTop(0.0);
		for (size_t k = 0; k < run.e.size(); k++)
		{
			RunEdge& re = run.e[k];
			const MeshEdge& me = tp.edges[(size_t)re.edge];
			const V n0 = tp.polyN[(size_t)me.f0], n1 = tp.polyN[(size_t)me.f1];
			bool zeroTop;
			if (k == 0)
			{
				const double u0 = m::la::dot(n0, up), u1 = m::la::dot(n1, up);
				zeroTop = std::abs(u0 - u1) > 1e-6 ? u0 > u1 : me.f0 < me.f1;
			}
			else
				zeroTop = m::la::dot(n0, prevTop) >= m::la::dot(n1, prevTop);
			re.fTop = zeroTop ? me.f0 : me.f1;
			re.fSide = zeroTop ? me.f1 : me.f0;
			prevTop = tp.polyN[(size_t)re.fTop];
		}
		if (!run.e.empty()) runs.push_back(std::move(run));
	}
	if (runs.empty()) return;

	const int segs = std::max(4, (int)std::lround(12.0 * p.detail));
	const double cosRay = 1e-4 * diag;

	struct CornerEnd { int32_t v; double a, b; Frame f; };
	std::vector<CornerEnd> corners;

	if (field)
	{
		field->segs.clear();
		field->chips.clear();
		field->width = std::max(0.0, p.wearMaskWidth);
		field->noise = ClampD(p.wearMaskNoise, 0.0, 1.0);
		field->noiseScale = std::max(1e-6, p.wearMaskScale);
		field->noiseOffset = V(Random01(seed, 999, 1) * 97.0, Random01(seed, 999, 2) * 97.0, Random01(seed, 999, 3) * 97.0);
	}

	int64_t chipCounter = 0;
	for (size_t ri = 0; ri < runs.size(); ri++)
	{
		const Run& run = runs[ri];
		const size_t ne = run.e.size();
		std::vector<Frame> fr(ne);
		std::vector<double> len(ne), arc0(ne);
		double total = 0.0;
		for (size_t k = 0; k < ne; k++)
		{
			fr[k] = MakeFrame(mv, tp, run.e[k]);
			len[k] = m::la::length(mv.pts[run.e[k].p1] - mv.pts[run.e[k].p0]);
			arc0[k] = total;
			total += len[k];
		}
		if (total <= 1e-12) continue;

		                                                       
		auto widthAt = [&](const V& pos, const Frame& f, double* tSide) -> double
		{
			double wTop = 0.0, wSide = 0.0;
			const double dlt = std::min(1e-3 * diag, 0.05 * total);
			if (!bvh.Raycast(pos + f.dB * dlt - f.nB * cosRay, -f.nB, 4.0 * diag, &wTop)) wTop = 0.2 * diag;
			if (!bvh.Raycast(pos + f.dA * dlt - f.nA * cosRay, -f.nA, 4.0 * diag, &wSide)) wSide = 0.2 * diag;
			if (tSide) *tSide = wSide;
			return wTop;
		};
		                  
		double est;
		{
			double ts = 0.0;
			const double tw = widthAt(mv.pts[run.e[0].p0] * 0.5 + mv.pts[run.e[0].p1] * 0.5, fr[0], &ts);
			est = p.sizeMode == 1 ? 0.5 * (p.sizeTop + p.sizeSide) * tw : 0.5 * (p.sizeTop + p.sizeSide);
			est = std::max(est, 1e-4 * diag);
		}
		double step = 0.75 * est;
		if (p.wearUneven > 0.0) step = std::min(step, p.wearUnevenScale / 4.0);
		if (p.paintPartial && haveStrokes) step = std::min(step, std::max(p.paintTaper / 3.0, p.paintRadius / 2.0));
		step = std::max(step / p.detail, std::max(total / 3000.0, 1e-4 * diag));

		                                                             
		struct S { V pos; double arc; int32_t edge; double tTop, tSide; double mult; bool cov; };
		std::vector<S> st;
		std::vector<std::pair<size_t, size_t>> range(ne);
		for (size_t k = 0; k < ne; k++)
		{
			const V a = mv.pts[run.e[k].p0], b = mv.pts[run.e[k].p1];
			const int n = (int)ClampD(std::ceil(len[k] / step), 1.0, 4000.0);
			const size_t first = st.empty() ? 0 : st.size() - 1;
			for (int j = (k == 0 ? 0 : 1); j <= n; j++)
			{
				const double t = double(j) / n;
				S s;
				s.pos = a + (b - a) * t;
				s.arc = arc0[k] + len[k] * t;
				s.edge = (int32_t)k;
				st.push_back(s);
			}
			range[k] = { first, st.size() - 1 };
		}
		const size_t ns = st.size();
		for (size_t i = 0; i < ns; i++)
			st[i].tTop = widthAt(st[i].pos, fr[(size_t)st[i].edge], &st[i].tSide);
		                                                                      
		for (int pass = 0; pass < 3; pass++)
		{
			std::vector<double> t0(ns), t1(ns);
			for (size_t i = 0; i < ns; i++)
			{
				const size_t a = i == 0 ? (run.closed ? ns - 2 : 0) : i - 1;
				const size_t b = i + 1 >= ns ? (run.closed ? 1 : ns - 1) : i + 1;
				t0[i] = 0.25 * st[a].tTop + 0.5 * st[i].tTop + 0.25 * st[b].tTop;
				t1[i] = 0.25 * st[a].tSide + 0.5 * st[i].tSide + 0.25 * st[b].tSide;
			}
			for (size_t i = 0; i < ns; i++) { st[i].tTop = t0[i]; st[i].tSide = t1[i]; }
		}
		                          
		std::vector<double> ramp(ns, 1.0);
		if (p.paintPartial && haveStrokes)
		{
			for (size_t i = 0; i < ns; i++) st[i].cov = sk.Hit(st[i].pos, fr[(size_t)st[i].edge].t);
			std::vector<double> dist(ns, std::numeric_limits<double>::infinity());
			const int laps = run.closed ? 2 : 1;
			double last = -std::numeric_limits<double>::infinity();
			for (int lap = 0; lap < laps; lap++)
				for (size_t i = 0; i < ns; i++)
				{
					const double ar = st[i].arc + lap * total;
					if (!st[i].cov) last = ar;
					dist[i] = std::min(dist[i], ar - last);
				}
			last = std::numeric_limits<double>::infinity();
			for (int lap = 0; lap < laps; lap++)
				for (size_t ii = ns; ii-- > 0;)
				{
					const double ar = st[ii].arc - lap * total;
					if (!st[ii].cov) last = ar;
					dist[ii] = std::min(dist[ii], last - ar);
				}
			for (size_t i = 0; i < ns; i++)
				ramp[i] = !st[i].cov ? 0.0 : (p.paintTaper > 0.0 ? Smooth(0.0, p.paintTaper, dist[i]) : 1.0);
		}
		const V off(Random01(seed, ri, 50) * 131.0, Random01(seed, ri, 51) * 131.0, Random01(seed, ri, 52) * 131.0);
		std::vector<double> sa(ns), sb(ns);
		std::vector<char> alive(ns, 0);
		for (size_t i = 0; i < ns; i++)
		{
			double mlt = 1.0;
			if (p.taper > 0.0 && !run.closed)
			{
				const double f = st[i].arc / total;
				mlt *= 1.0 - p.taper * (p.taperFlip ? 1.0 - f : f);
			}
			if (p.wearUneven > 0.0)
				mlt *= std::max(0.15, 1.0 + 1.3 * p.wearUneven * Noise(st[i].pos / p.wearUnevenScale + off));
			if (mask) mlt *= mask->SizeScale(st[i].pos);
			mlt = std::max(0.02, mlt);
			alive[i] = ramp[i] > 0.02 ? 1 : 0;
			mlt *= std::max(0.02, ramp[i]);
			double a = p.sizeTop, b = p.sizeSide;
			if (p.sizeMode == 1) { a *= st[i].tTop; b *= st[i].tTop; }
			a *= mlt; b *= mlt;
			const double k = std::min({ 1.0, p.maxShare * st[i].tTop / std::max(a, 1e-12), p.maxShare * st[i].tSide / std::max(b, 1e-12) });
			sa[i] = std::max(a * k, 1e-6 * diag);
			sb[i] = std::max(b * k, 1e-6 * diag);
		}

		std::vector<std::pair<double, double>> crestProf;
		const V roughOff(Random01(seed, ri, 53) * 113.0, Random01(seed, ri, 54) * 113.0, Random01(seed, ri, 55) * 113.0);
		                                                                              
		for (size_t k = 0; k < ne; k++)
		{
			const Frame& f = fr[k];
			const size_t i0 = range[k].first, i1 = range[k].second;
			size_t i = i0;
			while (i <= i1)
			{
				if (!alive[i]) { i++; continue; }
				size_t j = i;
				while (j + 1 <= i1 && alive[j + 1]) j++;
				std::vector<Station> sw;
				const size_t a0 = i > i0 ? i - 1 : i;                                                  
				const size_t a1 = j < i1 ? j + 1 : j;
				                                                  
				if (a0 == i0 && alive[i0])
				{
					const bool runStart = k == 0 && !run.closed;
					if (runStart)
					{
						const double r = std::max(sa[i0], sb[i0]);
						const V probe = st[i0].pos - f.t * (0.5 * std::min(sa[i0], sb[i0]))
							+ (f.dA + f.dB) * (0.25 * std::min(sa[i0], sb[i0]));
						if (!Inside(bvh, probe, diag))
						{
							sw.push_back({ st[i0].pos - f.t * (1.2 * r + 1e-4 * diag), sa[i0], sb[i0] });
							corners.push_back({ run.e[k].p0, sa[i0], sb[i0], f });
						}
					}
					else
					{
						const Frame& fp = fr[k == 0 ? ne - 1 : k - 1];
						const double turn = std::acos(ClampD(m::la::dot(fp.t, f.t), -1.0, 1.0));
						const double ext = std::max(sa[i0], sb[i0]) * (std::tan(0.5 * turn) * 1.3 + 0.03);
						sw.push_back({ st[i0].pos - f.t * ext, sa[i0], sb[i0] });
					}
				}
				for (size_t q = a0; q <= a1; q++) sw.push_back({ st[q].pos, sa[q], sb[q] });
				if (a1 == i1 && alive[i1])
				{
					const bool runEnd = k + 1 == ne && !run.closed;
					if (runEnd)
					{
						const double r = std::max(sa[i1], sb[i1]);
						const V probe = st[i1].pos + f.t * (0.5 * std::min(sa[i1], sb[i1]))
							+ (f.dA + f.dB) * (0.25 * std::min(sa[i1], sb[i1]));
						if (!Inside(bvh, probe, diag))
						{
							sw.push_back({ st[i1].pos + f.t * (1.2 * r + 1e-4 * diag), sa[i1], sb[i1] });
							corners.push_back({ run.e[k].p1, sa[i1], sb[i1], f });
						}
					}
					else
					{
						const Frame& fn = fr[(k + 1) % ne];
						const double turn = std::acos(ClampD(m::la::dot(fn.t, f.t), -1.0, 1.0));
						const double ext = std::max(sa[i1], sb[i1]) * (std::tan(0.5 * turn) * 1.3 + 0.03);
						sw.push_back({ st[i1].pos + f.t * ext, sa[i1], sb[i1] });
					}
				}
				m::Manifold c = Sweep(p, f, sw, segs, p.wearRough, roughOff, diag);
				if (!c.IsEmpty()) bevels.push_back(std::move(c));
				if (field)
				{
					                                                                     
					auto crest = [&](double a, double b)
					{
						Profile(p, a, b, segs, crestProf);
						double d = std::numeric_limits<double>::max();
						for (const auto& q : crestProf) d = std::min(d, m::la::length(f.dA * q.first + f.dB * q.second));
						return d;
					};
					double cPrev = crest(sa[a0], sb[a0]);
					for (size_t q = a0; q < a1; q++)
					{
						const double cNext = crest(sa[q + 1], sb[q + 1]);
						field->segs.push_back({ st[q].pos, st[q + 1].pos, cPrev, cNext });
						cPrev = cNext;
					}
				}
				i = j + 1;
			}
		}

		                                                                               
		if (p.wearChips > 0.0)
		{
			const double want = p.wearChips * total / 100.0;
			int n = (int)std::floor(want);
			if (Random01(seed, ri, 60) < want - n) n++;
			n = std::min(n, 20000);
			std::vector<std::pair<double, double>> prof;
			for (int c = 0; c < n; c++)
			{
				const uint64_t cid = (uint64_t)ri * 100003ULL + (uint64_t)c;
				const double arc = Random01(seed, cid, 1) * total;
				size_t i = 0;
				while (i + 1 < ns && st[i + 1].arc < arc) i++;
				const size_t j = std::min(i + 1, ns - 1);
				if (!alive[i] || !alive[j]) continue;
				const double t = st[j].arc > st[i].arc ? (arc - st[i].arc) / (st[j].arc - st[i].arc) : 0.0;
				const Frame& f = fr[(size_t)st[j].edge];
				const V E = st[i].pos + (st[j].pos - st[i].pos) * t;
				const double a = Lerp(sa[i], sa[j], t), b = Lerp(sb[i], sb[j], t);
				const double line = Random01(seed, cid, 2);
				V P, N;
				if (line < 0.42) { P = E + f.dA * a; N = f.nA; }
				else if (line < 0.84) { P = E + f.dB * b; N = f.nB; }
				else
				{
					Profile(p, a, b, segs, prof);
					const auto mid = prof[prof.size() / 2];
					P = E + f.dA * mid.first + f.dB * mid.second;
					N = Unit(f.nA + f.nB, f.nA);
				}
				if (mask && !mask->Keep(P, (int32_t)(chipCounter & 0x7fffffff), kStream)) { chipCounter++; continue; }
				chipCounter++;
				const bool big = Random01(seed, cid, 7) < p.wearBig;
				const double s = std::min(Lerp(p.wearChipMin, p.wearChipMax, Random01(seed, cid, 3)) * (big ? 2.2 : 1.0),
					0.9 * std::min(a, b) * (big ? 1.6 : 1.0));
				if (s < 1e-5 * diag) continue;
				const double depth = s * p.wearChipDepth * Lerp(0.7, 1.3, Random01(seed, cid, 4));
				const double rx = s * std::max(1.0, p.wearChipStretch) * Lerp(0.8, 1.2, Random01(seed, cid, 5));
				const double ry = s;
				const double rz = std::max(depth * 1.6, 0.25 * s);
				const double tw = (Random01(seed, cid, 6) - 0.5) * 0.6;
				const V T0 = Unit(f.t - N * m::la::dot(f.t, N), f.t);
				const V B0 = m::la::cross(N, T0);
				const V T = T0 * std::cos(tw) + B0 * std::sin(tw);
				const V B = m::la::cross(N, T);
				const V C = P + N * (rz - depth);
				const V noff(Random01(seed, cid, 8) * 50.0, Random01(seed, cid, 9) * 50.0, Random01(seed, cid, 10) * 50.0);
				const int cs = std::max(8, (int)std::lround(14.0 * p.detail));
				m::Manifold sph = m::Manifold::Sphere(1.0, cs).Warp([noff](m::vec3& v)
				{
					const double r = 1.0 + 0.28 * Noise(v * 1.6 + noff) + 0.1 * Noise(v * 4.1 + noff * 1.7);
					v = v * r;
					if (v.z < -0.35) v.z = -0.35 - (v.z + 0.35) * 0.4;                                         
				});
				const m::mat3x4 xf(T * rx, B * ry, N * rz, C);
				m::Manifold chip = sph.Transform(xf);
				if (chip.Status() != m::Manifold::Error::NoError || chip.IsEmpty()) continue;
				wear.push_back(std::move(chip));
				if (field) field->chips.push_back({ P, std::max(rx, ry) * 1.1 });
			}
		}
	}

	                                                                                  
	if (p.roundCorners && p.profile == 1 && std::abs(p.roundness - 2.0) < 0.5 && !corners.empty())
	{
		std::map<int32_t, std::vector<const CornerEnd*>> byV;
		for (const CornerEnd& c : corners) byV[c.v].push_back(&c);
		for (const auto& kv : byV)
		{
			const auto& ends = kv.second;
			if (ends.size() != 3) continue;
			double r = 0.0;
			bool ok = true;
			for (const CornerEnd* c : ends)
			{
				if (std::abs(c->a - c->b) > 0.15 * std::max(c->a, c->b)) ok = false;
				if (std::abs(m::la::dot(c->f.nA, c->f.nB)) > 0.17) ok = false;
				r += 0.5 * (c->a + c->b) / 3.0;
			}
			for (const CornerEnd* c : ends)
				if (std::abs(0.5 * (c->a + c->b) - r) > 0.15 * r) ok = false;
			if (!ok) continue;
			std::vector<V> nrm;
			for (const CornerEnd* c : ends)
				for (const V& n : { c->f.nA, c->f.nB })
				{
					bool dup = false;
					for (const V& q : nrm) if (m::la::dot(q, n) > 0.999) dup = true;
					if (!dup) nrm.push_back(n);
				}
			if (nrm.size() != 3) continue;
			const m::mat3 Nm = m::la::transpose(m::mat3(nrm[0], nrm[1], nrm[2]));                    
			if (std::abs(m::la::determinant(Nm)) < 0.3) continue;
			const m::mat3 Ni = m::la::inverse(Nm);
			const V Vp = mv.pts[kv.first];
			const double mg0 = 0.4 * r + 1e-4 * diag;
			std::vector<V> box;
			for (int b = 0; b < 8; b++)
			{
				const V o((b & 1) ? mg0 : -r * 1.0005, (b & 2) ? mg0 : -r * 1.0005, (b & 4) ? mg0 : -r * 1.0005);
				box.push_back(Vp + Ni * o);
			}
			const V c = Vp + Ni * V(-r, -r, -r);
			const int cs = std::max(24, (int)std::lround(48.0 * p.detail));
			m::Manifold cut = m::Manifold::Hull(box) - m::Manifold::Sphere(r, cs).Translate(c);
			if (cut.Status() == m::Manifold::Error::NoError && !cut.IsEmpty())
				bevels.push_back(std::move(cut));
		}
	}
}

                                                                              
std::vector<float> EdgeWearMask(const EdgeWearField& fd, const EVec* pts, size_t n)
{
	std::vector<float> out(n, 0.0f);
	if (n == 0 || (fd.segs.empty() && fd.chips.empty())) return out;
	const double w = fd.width;
	double reach = 0.0;
	V lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
	for (const auto& s : fd.segs)
	{
		reach = std::max(reach, std::max(s.ra, s.rb));
		lo = m::la::min(lo, m::la::min(s.a, s.b)); hi = m::la::max(hi, m::la::max(s.a, s.b));
	}
	for (const auto& c : fd.chips) { reach = std::max(reach, c.r); lo = m::la::min(lo, c.c); hi = m::la::max(hi, c.c); }
	reach += w * 1.6 + 1e-9;
	const double cell = std::max(reach, m::la::length(hi - lo) / 256.0);
	lo = lo - V(reach); hi = hi + V(reach);
	auto cellOf = [&](const V& p) { return m::ivec3((int)std::floor((p.x - lo.x) / cell), (int)std::floor((p.y - lo.y) / cell), (int)std::floor((p.z - lo.z) / cell)); };
	auto key = [](int x, int y, int z) { return ((int64_t)x * 73856093) ^ ((int64_t)y * 19349663) ^ ((int64_t)z * 83492791); };
	std::unordered_map<int64_t, std::vector<int32_t>> grid;                                 
	auto insert = [&](const V& a, const V& b, double r, int32_t id)
	{
		const m::ivec3 c0 = cellOf(m::la::min(a, b) - V(r)), c1 = cellOf(m::la::max(a, b) + V(r));
		for (int x = c0.x; x <= c1.x; x++)
			for (int y = c0.y; y <= c1.y; y++)
				for (int z = c0.z; z <= c1.z; z++)
					grid[key(x, y, z)].push_back(id);
	};
	for (size_t i = 0; i < fd.segs.size(); i++)
		insert(fd.segs[i].a, fd.segs[i].b, std::max(fd.segs[i].ra, fd.segs[i].rb) + w * 1.6, (int32_t)i);
	for (size_t i = 0; i < fd.chips.size(); i++)
		insert(fd.chips[i].c, fd.chips[i].c, fd.chips[i].r + w, ~(int32_t)i);
	ProjectDamageParallelFor(0, (int64_t)n, [&](int64_t pi)
	{
		const V p = pts[(size_t)pi];
		if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) return;
		const m::ivec3 c = cellOf(p);
		auto it = grid.find(key(c.x, c.y, c.z));
		if (it == grid.end()) return;
		const double nz = fd.noise > 0.0 ? Noise(p / fd.noiseScale + fd.noiseOffset) + 0.5 * Noise(p / (0.37 * fd.noiseScale) + fd.noiseOffset * 1.3) : 0.0;
		const double shift = w * fd.noise * nz;
		double best = 0.0;
		for (int32_t id : it->second)
		{
			double v;
			if (id >= 0)
			{
				const auto& s = fd.segs[(size_t)id];
				double t;
				const double d = SegDist(p, s.a, s.b, &t);
				const double r = Lerp(s.ra, s.rb, t) + 0.15 * w;
				v = 1.0 - Smooth(r, r + w, d - shift);
			}
			else
			{
				const auto& ch = fd.chips[(size_t)(~id)];
				const double d = m::la::length(p - ch.c);
				v = 1.0 - Smooth(ch.r, ch.r + 0.5 * w, d - 0.5 * shift);
			}
			best = std::max(best, v);
			if (best >= 1.0) break;
		}
		out[(size_t)pi] = (float)ClampD(best, 0.0, 1.0);
	});
	return out;
}

PROJECT_DAMAGE_NS_MACRO_END
