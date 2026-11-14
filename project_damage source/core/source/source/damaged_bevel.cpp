// строим фаску с локальными сколами вдоль рёбер
// ProjectDamage damaged edge bevel — GPL-3.0-or-later; see LICENSE.
//
// Recipe: damaged-bevel-v1. A worn concrete edge in one click:
//   1. bevel: one clean sweep per straight piece of a run (collinear mesh
//      edges share it) cuts the chamfer. Its profile blends from a straight
//      chamfer (Smoothness 0) to a quarter circle (0.6 and up), and Profile
//      Shape pushes it out toward the old sharp edge (0 = that profile,
//      1 = a full outward bulge, like Blender's Bevel Profile Shape); the width
//      wanders slowly (uneven), so the sweep needs only coarse stations, or
//      just its two ends when uneven is 0. (Fast path: it replaced the Edge
//      Bevel module's per-edge chamfer, about 3-6x faster overall.)
//   2. edges and runs: the same picking and run rules as the Edge Bevel
//      (copied, so both pick the same edges): convex edges sharper than
//      `angle`, or the ones strokes run along; runs join edges meeting two at
//      a vertex and turning less than maxTurn;
//   3. damage factor: each run gets spaced damaged stretches whose lengths
//      add up to exactly damageFactor x the run length (DamagedBevelPatches).
//      1 = the whole run, 0 = none. Stretches have random lengths around
//      patchLength and random gaps; Placement Random makes the lengths
//      log-normal and the gaps heavy tailed (groups of stretches with long
//      clean runs between), and varies each stretch's depth and end taper.
//      Inner stretch ends feather, a stretch that reaches a free end of the
//      run stays at full depth to the end;
//   4. breakage: per edge of a damaged stretch, one sweep of a noisy
//      cross-section from the top face to the side face (a slightly scooped
//      spall surface), through dense stations. Depth along each face wanders
//      in bites (Damage Amount sets how many per length), the two faces bite
//      independently, and high-frequency noise tears the border (jagged,
//      lowered by Smoothness). Every ring lies in its own plane across the
//      edge, so the sweep is always a simple valid solid;
//   5. chips: flattened noisy spalls on the break borders inside the
//      stretches, and loose ones just outside (chipScatter);
//   6. masks: EdgeWearField for the chamfer crest ("edge_wear") and one for
//      the breakage and chips ("bevel_damage"), both evaluated with
//      EdgeWearMask after the Boolean.
// Smooth Damage Surface (smoothDamage, smoothDamageAmount) lowers the tears,
// softens the chunk steps, relaxes the reach along the run and rounds the
// chips. DamagedBevelSegments rebuilds the same cutters and returns the
// broken-off volume for Fill Segments (physics).
// Random draws: SplitMix64 counter channels keyed by (run, item, channel).
// Run channels: 10-19 patches, 40-41 per-stretch depth and taper (keyed by stretch), 20-31 noise offsets, 32 scoop, 33-34 chunk
// levels (keyed by run and cell). Chip channels:
// 1 arc, 2 face, 3 size, 4 depth, 5 stretch, 6 twist, 7-9 noise offset.
#include "manifold/manifold.h"
#include "damaged_bevel.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
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
double Mix(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double e0, double e1, double x)
{
	if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
                                    
double Noise(const V& p) { return (ProjectDamageNoise(p) - 0.5) * 2.0; }
                                                
double Fbm(const V& p, double octaves, double gain)
{
	double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
	const int n = (int)std::ceil(std::max(1.0, octaves));
	for (int i = 0; i < n; i++)
	{
		const double w = std::min(1.0, octaves - i);
		sum += w * amp * Noise(p * f + V(17.3 * i, 5.1 * i, 11.7 * i));
		norm += w * amp;
		amp *= gain;
		f *= 2.03;
	}
	return norm > 0.0 ? 1.4 * sum / norm : 0.0;
}

double SegDist(const V& p, const V& a, const V& b)
{
	const V ab = b - a;
	const double l2 = m::la::dot(ab, ab);
	double t = l2 > 1e-24 ? m::la::dot(p - a, ab) / l2 : 0.0;
	t = ClampD(t, 0.0, 1.0);
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

struct RunEdge { int32_t edge, p0, p1, fTop, fSide; double t0=0.0,t1=1.0; };
V RunStart(const MeshView& mv,const RunEdge& e) { return mv.pts[e.p0]+(mv.pts[e.p1]-mv.pts[e.p0])*e.t0; }
V RunEnd(const MeshView& mv,const RunEdge& e) { return mv.pts[e.p0]+(mv.pts[e.p1]-mv.pts[e.p0])*e.t1; }
struct Run { std::vector<RunEdge> e; bool closed = false; };
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

                                                                              
                      
std::vector<Run> BuildRuns(const MeshView& mv, const Topo& tp, const DamagedBevelParams& p,
	const std::vector<std::vector<EVec>>* paintStrokes, bool& ok)
{
	std::vector<Run> runs;
	ok = true;
	const bool haveStrokes = paintStrokes && !paintStrokes->empty();
	if (!haveStrokes && p.drawnOnly) { ok = false; return runs; }
	Strokes sk;
	sk.radius = std::max(1e-6, p.paintRadius);
	sk.cosAlign = std::cos(ClampD(p.paintAlign, 0.0, 90.0) * kPi / 180.0);
	if (haveStrokes)
		for (const auto& s : *paintStrokes)
			for (size_t i = 0; i + 1 < s.size(); i++)
			{
				const V a = s[i], b = s[i + 1];
				if (!std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z)) continue;
				sk.segs.push_back({ a, b, Unit(b - a, V(1.0, 0.0, 0.0)) });
			}
	if (haveStrokes && sk.segs.empty()) { ok = false; return runs; }

    if(haveStrokes) {
        auto insideZone=[&](const V& point) {
            for(const auto& path:*paintStrokes) {
                for(size_t i=0;i+1<path.size();++i) if(SegDist(point,path[i],path[i+1])<=sk.radius) return true;
                if(path.size()<4 || m::la::length(path.front()-path.back())>2*sk.radius) continue;
                V normal(0.0);
                for(size_t i=1;i+1<path.size();++i) normal+=m::la::cross(path[i]-path[0],path[i+1]-path[0]);
                if(m::la::length(normal)<1e-10) continue;
                normal=Unit(normal,V(0,0,1));
                if(std::abs(m::la::dot(point-path[0],normal))>sk.radius) continue;
                const int drop=std::abs(normal.x)>std::abs(normal.y)?(std::abs(normal.x)>std::abs(normal.z)?0:2):(std::abs(normal.y)>std::abs(normal.z)?1:2);
                const int u=(drop+1)%3,v=(drop+2)%3;
                bool inside=false;
                for(size_t i=0,j=path.size()-1;i<path.size();j=i++) {
                    const auto a=path[i],b=path[j];
                    if((a[v]>point[v])!=(b[v]>point[v]) && point[u]<(b[u]-a[u])*(point[v]-a[v])/(b[v]-a[v])+a[u]) inside=!inside;
                }
                if(inside) return true;
            }
            return false;
        };
        for(size_t i=0;i<tp.edges.size();++i) {
            const auto& e=tp.edges[i];if(!e.sharp) continue;
            const V a=mv.pts[e.v0],d=mv.pts[e.v1]-a;
            const int steps=(int)ClampD(std::ceil(m::la::length(d)/std::max(sk.radius*.2,1e-6)),8.,8192.);
            double start=-1.;bool previous=insideZone(a);
            if(previous) start=0.;
            for(int k=1;k<=steps;++k) {
                const double t=double(k)/steps;bool hit=insideZone(a+d*t);
                double boundary=t;
                if(hit!=previous) {
                    double lo=double(k-1)/steps,hi=t;
                    for(int n=0;n<16;++n) {double mid=(lo+hi)*.5;if(insideZone(a+d*mid)==previous)lo=mid;else hi=mid;}
                    boundary=(lo+hi)*.5;
                    if(hit) start=boundary;
                }
                if(start>=0 && ((!hit&&previous)||(k==steps&&hit))) {
                    Run r;RunEdge re;re.edge=(int32_t)i;re.p0=e.v0;re.p1=e.v1;re.fTop=e.f0;re.fSide=e.f1;re.t0=start;re.t1=boundary;
                    if((re.t1-re.t0)*m::la::length(d)>1e-7) {r.e.push_back(re);runs.push_back(r);}
                    start=-1.;
                }
                previous=hit;
            }
        }
        return runs;
    }
	std::vector<char> picked(tp.edges.size(), 0);
	for (size_t i = 0; i < tp.edges.size(); i++)
	{
		const MeshEdge& e = tp.edges[i];
		if (!e.sharp) continue;
		if (sk.segs.empty()) { picked[i] = 1; continue; }
		const V a = mv.pts[e.v0], b = mv.pts[e.v1];
		const double L = m::la::length(b - a);
		const V d = Unit(b - a, V(1.0, 0.0, 0.0));
		const int n = (int)ClampD(std::ceil(L / (0.5 * sk.radius)), 2.0, 400.0);
		int hit = 0;
		for (int k = 0; k <= n; k++)
			if (sk.Hit(a + (b - a) * (double(k) / n), d)) hit++;
		if (double(hit) / double(n + 1) >= p.paintCover) picked[i] = 1;
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
	const V up = Unit(V(p.upX, p.upY, p.upZ), V(0.0, 0.0, 1.0));
	std::vector<char> used(tp.edges.size(), 0);
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
			re.edge = e; re.p0 = p0; re.p1 = other(e, p0); re.fTop = -1; re.fSide = -1;
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
	return runs;
}

                                                                          
                                       
                                                                              
                                           
struct Ring { V pos; double a, b; double w = 1.0; };

                                                                          
                                                              
                                                                             
                                     
                                                                       
                                                                            
                                                                               
                                                    
constexpr double kProfileOutR = 5.0;
inline void ChamferProfile(double a, double b, double th, double round, double shape, double& x, double& y)
{
	const double c = std::max(0.0, std::cos(th)), sn = std::max(0.0, std::sin(th));
	x = Mix(a * c * c, a * (1.0 - sn), round);
	y = Mix(b * sn * sn, b * (1.0 - c), round);
	if (shape > 0.0)
	{
		const double ox = a * (1.0 - std::pow(sn, 2.0 / kProfileOutR));
		const double oy = b * (1.0 - std::pow(c, 2.0 / kProfileOutR));
		x = Mix(x, ox, shape);
		y = Mix(y, oy, shape);
	}
}

                                                                            
                                                                               
                                                                          
                                                                      
m::Manifold SpallSweep(const Frame& f, const std::vector<Ring>& st, int segs, double q,
	double rough, double roughScale, double octaves, const V& off, double diag, double round = 0.0,
	double shape = 0.0)
{
	const int R = (int)st.size();
	if (R < 2) return m::Manifold();
	const int K = segs + 1;
	const int M = K + 3;
	m::MeshGL64 mg;
	mg.numProp = 3;
	mg.vertProperties.reserve((size_t)R * M * 3);
	auto push = [&](const V& v) { mg.vertProperties.push_back(v.x); mg.vertProperties.push_back(v.y); mg.vertProperties.push_back(v.z); };
	const double e = 2.0 / std::max(0.5, q);
	for (int r = 0; r < R; r++)
	{
		const Ring& s = st[(size_t)r];
		const double a = std::max(s.a, 1e-6 * diag), b = std::max(s.b, 1e-6 * diag);
		for (int k = 0; k < K; k++)
		{
			const double th = (kPi * 0.5) * double(k) / double(segs);
			const double c = std::cos(th), sn = std::sin(th);
			double x = a * std::pow(std::max(0.0, c), e);
			double y = b * std::pow(std::max(0.0, sn), e);
			if (s.w < 1.0)
			{
				                                                                            
				double cx, cy;
				ChamferProfile(a, b, th, round, shape, cx, cy);
				x = Mix(cx, x, s.w);
				y = Mix(cy, y, s.w);
			}
			if (k == 0) { x = a; y = 0.0; }
			if (k == K - 1) { x = 0.0; y = b; }
			const V base = s.pos + f.dA * x + f.dB * y;
			double sc = 1.0;
			if (rough > 0.0 && s.w > 0.0)
				sc = std::max(0.35, 1.0 + s.w * rough * Fbm(base / roughScale + off, octaves, 0.55));
			push(s.pos + f.dA * (x * sc) + f.dB * (y * sc));
		}
		const double mg0 = 0.35 * std::max(a, b) + 1e-5 * diag;
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

                                                                     
m::Manifold Chip(const V& P, const V& N, const V& along, double s, double depth, double stretch,
	double twist, double bumpy, const V& noff, int cs)
{
	const V T0 = Unit(along - N * m::la::dot(along, N), Unit(m::la::cross(N, V(0.31, 0.53, 0.79)), V(1, 0, 0)));
	const V B0 = m::la::cross(N, T0);
	const V T = T0 * std::cos(twist) + B0 * std::sin(twist);
	const V B = m::la::cross(N, T);
	const double rx = s * stretch, ry = s;
	const double rz = std::max(depth * 1.6, 0.25 * s);
	const V C = P + N * (rz - depth);
	m::Manifold sph = m::Manifold::Sphere(1.0, cs).Warp([noff, bumpy](m::vec3& v)
	{
		const double r = 1.0 + bumpy * (0.3 * Noise(v * 1.6 + noff) + 0.12 * Noise(v * 4.3 + noff * 1.7));
		v = v * std::max(0.4, r);
		if (v.z < -0.35) v.z = -0.35 - (v.z + 0.35) * 0.4;                                      
	});
	m::Manifold chip = sph.Transform(m::mat3x4(T * rx, B * ry, N * rz, C));
	if (chip.Status() != m::Manifold::Error::NoError || chip.IsEmpty()) return m::Manifold();
	return chip;
}

}             

                                                                              
std::vector<std::pair<double, double>> DamagedBevelPatches(double length, bool closed,
	double factor, double patchLength, uint64_t seed, uint64_t run, double randomness)
{
	std::vector<std::pair<double, double>> out;
	factor = ClampD(factor, 0.0, 1.0);
	const double r = ClampD(randomness, 0.0, 1.0);
	if (!(length > 0.0) || factor <= 0.0) return out;
	if (factor >= 0.999) { out.emplace_back(0.0, length); return out; }
	const double dmg = factor * length, gap = length - dmg;
	patchLength = std::max(1e-9, patchLength);
	const uint64_t key = run * 8191;
	                                                                              
	const double base = dmg / patchLength;
	int n = (int)std::lround(base * Mix(1.0, Mix(0.55, 1.6, Random01(seed, run, 16)), r));
	n = std::max(1, std::min(n, 4096));
	                                                                            
	while (n > 1 && gap / (closed ? n : n + 1) < 0.33 * dmg / n) n--;
	                                          
	auto gauss = [&](uint64_t item, uint64_t ch) {
		const double u1 = std::max(1e-12, Random01(seed, item, ch)), u2 = Random01(seed, item, ch + 1);
		return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
	};
	                                                                      
	std::vector<double> wp((size_t)n), wg((size_t)(closed ? n : n + 1));
	double sp = 0.0, sg = 0.0;
	for (int i = 0; i < n; i++)
	{
		const double even = 0.55 + 0.9 * Random01(seed, key + (uint64_t)i, 10);
		const double wild = std::exp(0.75 * ClampD(gauss(key + (uint64_t)i, 17), -2.5, 2.5));
		wp[(size_t)i] = Mix(even, wild, r);
		sp += wp[(size_t)i];
	}
	                                                                          
	                                                          
	for (size_t j = 0; j < wg.size(); j++)
	{
		const double even = 0.35 + 1.3 * Random01(seed, key + j, 11);
		const double e = -std::log(std::max(1e-12, Random01(seed, key + j, 19)));                 
		const double wild = std::pow(e, 1.8) + 0.04;
		double w = Mix(even, wild, r);
		                                                                             
		if (!closed && (j == 0 || j + 1 == wg.size())) w *= 0.9 * Random01(seed, key + j, 12);
		wg[j] = w; sg += w;
	}
	                                                                     
	const size_t nInner = closed ? wg.size() : (wg.size() > 2 ? wg.size() - 2 : 0);
	const double minGap = std::min(0.12 * patchLength, 0.5 * gap / std::max<size_t>(1, nInner));
	const double free = std::max(0.0, gap - minGap * double(nInner));
	auto gapLen = [&](size_t j) {
		const bool inner = closed || (j > 0 && j + 1 < wg.size());
		return (inner ? minGap : 0.0) + free * wg[j] / std::max(sg, 1e-12);
	};
	double at = closed ? Random01(seed, run, 13) * length : gapLen(0);
	for (int i = 0; i < n; i++)
	{
		const double len = dmg * wp[(size_t)i] / sp;
		out.emplace_back(at, at + len);
		at += len;
		                                                                               
		const size_t gj = closed ? (size_t)i : (size_t)i + 1;
		if (closed || i + 1 < n) at += gapLen(gj);
	}
	if (!closed)
	{
		                                                     
		const double over = out.back().second - length;
		if (over > 0.0) for (auto& pr : out) { pr.first -= over; pr.second -= over; }
		if (out.front().first < 0.0) out.front().first = 0.0;
	}
	return out;
}

void PlaceDamagedBevel(const CoreInput& in, const DamagedBevelParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>* bevels, std::vector<m::Manifold>* damage,
	EdgeWearField* wearField, EdgeWearField* damageField)
{
	if (!pIn.enable) return;
	const MeshView& mv = in.view;
	if (mv.npts <= 0 || mv.npolys <= 0 || !mv.pts || !mv.polys) return;
	DamagedBevelParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.width = std::max(0.0, p.width);
	p.smoothness = ClampD(p.smoothness, 0.0, 1.0);
	p.damage = ClampD(p.damage, 0.0, 1.0);
	p.damageAmount = ClampD(p.damageAmount, 0.0, 1.0);
	p.damageFactor = ClampD(p.damageFactor, 0.0, 1.0);
	p.damageDepth = std::max(0.0, p.damageDepth);
	p.patchLength = std::max(1e-6, p.patchLength);
	p.chipScatter = ClampD(p.chipScatter, 0.0, 1.0);
	p.jagged = ClampD(p.jagged, 0.0, 1.0);
	p.maxShare = ClampD(p.maxShare, 0.01, 0.99);
	p.placementRandom = ClampD(p.placementRandom, 0.0, 1.0);
	p.smoothDamageAmount = ClampD(p.smoothDamageAmount, 0.0, 1.0);
	p.profileShape = ClampD(p.profileShape, 0.0, 1.0);

	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x444d47424556314cULL)                
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4442;                             
	for (EdgeWearField* fd : { wearField, damageField })
	{
		if (!fd) continue;
		fd->segs.clear();
		fd->chips.clear();
		fd->width = std::max(0.0, p.maskWidth);
		fd->noise = ClampD(p.maskNoise, 0.0, 1.0);
		fd->noiseScale = std::max(1e-6, p.maskScale);
		const uint64_t c = fd == wearField ? 998 : 999;
		fd->noiseOffset = V(Random01(seed, c, 1) * 97.0, Random01(seed, c, 2) * 97.0, Random01(seed, c, 3) * 97.0);
	}
	const double D = p.damageDepth * p.damage;
	                                                                              
	const bool doChamfer = p.width > 0.0 && (bevels || wearField);
	const bool doDamage = D > 0.0 && p.damageFactor > 0.0 && (damage || damageField);
	if (!doChamfer && !doDamage) return;

	Topo tp;
	BuildTopo(mv, p.angle, p.avoid ? in.avoidPoly : nullptr, tp);
	const double diag = tp.diag;
	bool ok = true;
	const std::vector<Run> runs = BuildRuns(mv, tp, p, paintStrokes, ok);
	if (!ok || runs.empty()) return;
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	const double eps = 1e-4 * diag;

	const double s = p.smoothness;
	const double amount = p.damageAmount;
	                                                                       
	                                                                             
	const double ks = p.smoothDamage ? p.smoothDamageAmount : 0.0;
	const double prand = p.placementRandom;
	                                                                       
	                                                        
	const double biteLen = std::max(1e-6, D * Mix(2.6, 0.9, amount));
	const double shoulder = Mix(Mix(0.1, 0.45, s), 0.5, ks);                                             
	                                                                     
	const double rough = 0.5 * p.jagged * (1.0 - 0.8 * s) * (1.0 - 0.85 * ks);
	const double octaves = Mix(Mix(4.0, 1.5, s), 1.0, ks);
	const int segs = std::max(5, (int)std::lround(10.0 * p.detail)) + (int)std::lround(3.0 * ks);
	const int chipRes = std::max(6, (int)std::lround(10.0 * p.detail));
	                                                                           
	                                                                        
	                                                              
	const double round = ClampD(1.6 * s, 0.0, 1.0);
	const double shape = p.profileShape;
	const double bend = std::max(round, shape);
	const int segsCh = bend < 0.05 ? 1 : std::max(3, (int)std::lround(10.0 * p.detail * bend));
	                                                                        
	const double unevenScale = std::max(8.0 * p.width, 0.6 * p.patchLength);
	const double uneven = ClampD(p.uneven, 0.0, 1.0);
	double step = 1e30;
	if (doDamage)
	{
		step = std::min(D / 2.5, p.patchLength / 14.0);
		if (rough > 0.0) step = std::min(step, D / (1.5 + 3.0 * p.jagged));
		step = std::min(step, biteLen / 7.0);
		step = std::max(step / p.detail, 1e-4 * diag);
	}
	const double coarse = uneven > 0.0 ? unevenScale / (1.5 + 2.5 * p.detail) : 1e30;
	const V offW(Random01(seed, 997, 1) * 131.0, Random01(seed, 997, 2) * 131.0, Random01(seed, 997, 3) * 131.0);
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
			len[k] = m::la::length(RunEnd(mv,run.e[k]) - RunStart(mv,run.e[k]));
			arc0[k] = total;
			total += len[k];
		}
		if (total <= 1e-12) continue;

		                                                                             
		const auto patches = doDamage ? DamagedBevelPatches(total, run.closed, p.damageFactor, p.patchLength, seed, ri, prand)
			: std::vector<std::pair<double, double>>();
		if (patches.empty() && !doChamfer) continue;
		const bool whole = p.damageFactor >= 0.999;
		                                                                            
		std::vector<double> pDepth(patches.size()), pTaper(patches.size());
		for (size_t j = 0; j < patches.size(); j++)
		{
			const uint64_t pid = (uint64_t)ri * 65537ULL + j;
			pDepth[j] = whole ? 1.0 : Mix(1.0, Mix(0.7, 1.2, Random01(seed, pid, 40)), prand);
			pTaper[j] = Mix(0.3, Mix(0.12, 0.45, Random01(seed, pid, 41)), prand);
		}
		                                                               
		auto envelope = [&](double a, int* which) -> double
		{
			for (size_t j = 0; j < patches.size(); j++)
			{
				const double s0 = patches[j].first, s1 = patches[j].second;
				for (int lap = 0; lap < (run.closed ? 2 : 1); lap++)
				{
					const double x = a + lap * total;
					if (x < s0 - 1e-12 || x > s1 + 1e-12) continue;
					if (which) *which = (int)j;
					if (whole) return 1.0;
					const double taper = std::min(pTaper[j] * (s1 - s0), 1.6 * D);
					double e = 1.0;
					                                                                          
					if (run.closed || s0 > 1e-9) e = std::min(e, Smooth(0.0, taper, x - s0));
					if (run.closed || s1 < total - 1e-9) e = std::min(e, Smooth(0.0, taper, s1 - x));
					return e * pDepth[j];
				}
			}
			if (which) *which = -1;
			return 0.0;
		};

		                                                                     
		                                                                     
		                                                                      
		                                                                       
		auto nearPatch = [&](double arc) {
			const double margin = 2.0 * step;
			for (const auto& pr : patches)
				for (int lap = -1; lap <= (run.closed ? 1 : 0); lap++)
				{
					if (!run.closed && lap != 0) continue;
					const double x = arc + lap * total;
					if (x > pr.first - margin && x < pr.second + margin) return true;
				}
			return false;
		};
		struct St { V pos; double arc; int32_t edge; double tTop, tSide, env; bool coarse; };
		std::vector<St> st;
		std::vector<std::pair<size_t, size_t>> range(ne);
		for (size_t k = 0; k < ne; k++)
		{
			const V a = RunStart(mv,run.e[k]), b = RunEnd(mv,run.e[k]);
			std::vector<std::pair<double, bool>> ts;                                        
			if (k == 0) ts.push_back({ 0.0, false });
			ts.push_back({ 1.0, false });
			if (coarse < 1e29)
			{
				const double ncRun = std::max(1.0, std::ceil(total / coarse)), cs = total / ncRun;
				for (double c = std::ceil(arc0[k] / cs) * cs; c < arc0[k] + len[k]; c += cs)
				{
					const double t = (c - arc0[k]) / std::max(len[k], 1e-12);
					if (t > 1e-6 && t < 1.0 - 1e-6) ts.push_back({ t, true });
				}
			}
			if (!patches.empty())
			{
				const int n = (int)ClampD(std::ceil(len[k] / step), 1.0, 20000.0);
				for (int j = 1; j < n; j++)
					if (nearPatch(arc0[k] + len[k] * j / n)) ts.push_back({ double(j) / n, false });
			}
			for (const auto& pr : patches)
				for (double edgeArc : { pr.first, pr.second, pr.first - total, pr.second - total })
				{
					const double t = (edgeArc - arc0[k]) / std::max(len[k], 1e-12);
					if (t > 1e-6 && t < 1.0 - 1e-6) ts.push_back({ t, false });
				}
			std::sort(ts.begin(), ts.end());
			std::vector<std::pair<double, bool>> uq;
			for (const auto& t : ts)
			{
				if (!uq.empty() && std::abs(t.first - uq.back().first) < 1e-7) { uq.back().second = uq.back().second || t.second; continue; }
				uq.push_back(t);
			}
			const size_t first = st.empty() ? 0 : st.size() - 1;
			for (const auto& t : uq)
			{
				St q;
				q.pos = a + (b - a) * t.first;
				q.arc = arc0[k] + len[k] * t.first;
				q.edge = (int32_t)k;
				q.tTop = q.tSide = 0.0;
				q.env = patches.empty() ? 0.0 : envelope(q.arc, nullptr);
				q.coarse = t.second;
				st.push_back(q);
			}
			range[k] = { first, st.size() - 1 };
		}
		const size_t ns = st.size();
		                                                                           
		                                                                       
		                                                                        
		                                                                          
		for (size_t i = 0; i < ns; i++)
		{
			const bool near = st[i].env > 0.0 || (i > 0 && st[i - 1].env > 0.0) || (i + 1 < ns && st[i + 1].env > 0.0);
			if (!near && !doChamfer) continue;
			const Frame& f = fr[(size_t)st[i].edge];
			const double dlt = std::min(1e-3 * diag, 0.05 * total);
			double wTop = 0.0, wSide = 0.0;
			if (!bvh.Raycast(st[i].pos + f.dB * dlt - f.nB * eps, -f.nB, 4.0 * diag, &wTop)) wTop = 0.2 * diag;
			if (!bvh.Raycast(st[i].pos + f.dA * dlt - f.nA * eps, -f.nA, 4.0 * diag, &wSide)) wSide = 0.2 * diag;
			st[i].tTop = wTop;
			st[i].tSide = wSide;
		}

		                                                                           
		const V offA(Random01(seed, ri, 20) * 131.0, Random01(seed, ri, 21) * 131.0, Random01(seed, ri, 22) * 131.0);
		const V offB(Random01(seed, ri, 23) * 131.0, Random01(seed, ri, 24) * 131.0, Random01(seed, ri, 25) * 131.0);
		const V offJ(Random01(seed, ri, 26) * 131.0, Random01(seed, ri, 27) * 131.0, Random01(seed, ri, 28) * 131.0);
		const V offR(Random01(seed, ri, 29) * 131.0, Random01(seed, ri, 30) * 131.0, Random01(seed, ri, 31) * 131.0);
		const double q = Mix(1.1, 1.55, Random01(seed, ri, 32));                                
		const double floorDepth = Mix(0.1, 0.4, amount);                               
		                                                                     
		                                                  
		auto chunk = [&](double arc, uint64_t ch) -> double
		{
			const double x = arc / biteLen + 0.3 * Noise(V(arc / biteLen * 0.7, 3.1 * double(ch), 0.5) + offA);
			const double fi = std::floor(x);
			const uint64_t c0 = (uint64_t)(int64_t)fi + 0x40000000ULL;
			const double h0 = Random01(seed, ri * 0x100000001ULL + c0, ch);
			const double h1 = Random01(seed, ri * 0x100000001ULL + c0 + 1, ch);
			return Mix(h0, h1, Smooth(0.5 - shoulder, 0.5 + shoulder, x - fi));
		};
		std::vector<double> sa(ns, 0.0), sb(ns, 0.0);
		std::vector<char> alive(ns, 0);
		for (size_t i = 0; i < ns; i++)
		{
			if (st[i].env <= 0.0) continue;
			const V& P = st[i].pos;
			                                                       
			double bA = chunk(st[i].arc, 33) + 0.25 * Fbm(P / biteLen + offA, 1.5, 0.5);
			double bB = chunk(st[i].arc, 34) + 0.25 * Fbm(P / biteLen + offB, 1.5, 0.5);
			if (ks > 0.0)
			{
				                                                         
				bA = Mix(bA, 0.55 + 0.45 * Fbm(P / (2.5 * biteLen) + offA, 1.0, 0.5), 0.6 * ks);
				bB = Mix(bB, 0.55 + 0.45 * Fbm(P / (2.5 * biteLen) + offB, 1.0, 0.5), 0.6 * ks);
			}
			const double jag = rough > 0.0 ? 0.55 * rough * Fbm(P / std::max(1e-6, 0.35 * D) + offJ, octaves, 0.6) : 0.0;
			double mlt = st[i].env * (mask ? mask->SizeScale(P) : 1.0);
			double a = p.width + D * mlt * Mix(floorDepth, 1.0, ClampD(bA, 0.0, 1.0)) * (1.0 + jag);
			double b = p.width + D * mlt * Mix(floorDepth, 1.0, ClampD(bB, 0.0, 1.0)) * (1.0 - 0.7 * jag);
			a = std::min(a, p.maxShare * st[i].tTop);
			b = std::min(b, p.maxShare * st[i].tSide);
			sa[i] = std::max(a, 1e-6 * diag);
			sb[i] = std::max(b, 1e-6 * diag);
		}
		if (ks > 0.0)
		{
			                                                                 
			                                                               
			double meanGap = 0.0;
			for (size_t i = 1; i < ns; i++) meanGap += st[i].arc - st[i - 1].arc;
			meanGap = ns > 1 ? meanGap / double(ns - 1) : 1.0;
			const int passes = (int)ClampD(std::lround(ks * 2.5 * biteLen / std::max(meanGap, 1e-9)), 0.0, 80.0);
			std::vector<double> ta(ns), tb(ns);
			for (int it = 0; it < passes; it++)
			{
				for (size_t i = 0; i < ns; i++)
				{
					ta[i] = sa[i]; tb[i] = sb[i];
					if (st[i].env <= 0.0) continue;
					const size_t lo = i > 0 ? i - 1 : (run.closed ? ns - 1 : i);
					const size_t hi = i + 1 < ns ? i + 1 : (run.closed ? 0 : i);
					const double al = st[lo].env > 0.0 ? sa[lo] : sa[i], ah = st[hi].env > 0.0 ? sa[hi] : sa[i];
					const double bl = st[lo].env > 0.0 ? sb[lo] : sb[i], bh = st[hi].env > 0.0 ? sb[hi] : sb[i];
					ta[i] = 0.5 * sa[i] + 0.25 * (al + ah);
					tb[i] = 0.5 * sb[i] + 0.25 * (bl + bh);
				}
				sa.swap(ta); sb.swap(tb);
			}
			for (size_t i = 0; i < ns; i++)
			{
				if (st[i].env <= 0.0) continue;
				sa[i] = std::min(sa[i], p.maxShare * st[i].tTop);
				sb[i] = std::min(sb[i], p.maxShare * st[i].tSide);
			}
		}
		for (size_t i = 0; i < ns; i++)
			alive[i] = (st[i].env > 0.01 && (sa[i] > p.width * 1.02 + eps || sb[i] > p.width * 1.02 + eps)) ? 1 : 0;

		                                                                     
		                                                                  
		std::vector<std::pair<size_t, size_t>> piece;
		for (size_t k = 0; k < ne; k++)
		{
			const bool same = !piece.empty() && k > 0
				&& m::la::dot(fr[k].t, fr[k - 1].t) > 0.99995
				&& m::la::dot(fr[k].nA, fr[k - 1].nA) > 0.99995 && m::la::dot(fr[k].nB, fr[k - 1].nB) > 0.99995;
			if (same) piece.back().second = k;
			else piece.push_back({ k, k });
		}
		                                                                     
		                                                          
		for (const auto& pc : piece)
		{
			st[range[pc.first].first].coarse = true;
			st[range[pc.second].second].coarse = true;
		}

		                                                                       
		                                                                        
		                    
		auto endRing = [&](size_t k, size_t i, bool atStart, double a, double b, double w, Ring& out) -> bool
		{
			const Frame& f = fr[k];
			const double r = std::max(a, b);
			const double sgn = atStart ? -1.0 : 1.0;
			const bool free = !run.closed && (atStart ? k == 0 : k + 1 == ne);
			if (free)
			{
				const V probe = st[i].pos + f.t * (sgn * 0.5 * std::min(a, b)) + (f.dA + f.dB) * (0.25 * std::min(a, b));
				if (Inside(bvh, probe, diag)) return false;
				out = { st[i].pos + f.t * (sgn * (1.2 * r + eps)), a, b, w };
				return true;
			}
			const Frame& fo = fr[atStart ? (k == 0 ? ne - 1 : k - 1) : (k + 1) % ne];
			const double turn = std::acos(ClampD(m::la::dot(fo.t, f.t), -1.0, 1.0));
			out = { st[i].pos + f.t * (sgn * r * (std::tan(0.5 * turn) * 1.3 + 0.03)), a, b, w };
			return true;
		};

		                                                                              
		if (doChamfer)
		{
			auto chamferAt = [&](size_t i, double& a, double& b)
			{
				double wa = 1.0, wb = 1.0;
				if (uneven > 0.0)
				{
					wa = std::max(0.3, 1.0 + 0.8 * uneven * Fbm(st[i].pos / unevenScale + offW, 2.0, 0.5));
					wb = std::max(0.3, 1.0 + 0.8 * uneven * Fbm(st[i].pos / unevenScale + offW + V(37.1, 11.3, 5.7), 2.0, 0.5));
				}
				a = std::max(1e-6 * diag, std::min(p.width * wa, p.maxShare * st[i].tTop));
				b = std::max(1e-6 * diag, std::min(p.width * wb, p.maxShare * st[i].tSide));
			};
			                                                                        
			auto crest = [&](double a, double b)
			{
				double d = std::numeric_limits<double>::max();
				for (int j = 0; j <= 16; j++)
				{
					const double th = (kPi * 0.5) * j / 16.0;
					double x, y;
					ChamferProfile(a, b, th, round, shape, x, y);
					d = std::min(d, std::sqrt(x * x + y * y));
				}
				return d;
			};
			for (const auto& pc : piece)
			{
				const size_t k = pc.first, kEnd = pc.second;
				const size_t i0 = range[k].first, i1 = range[kEnd].second;
				std::vector<size_t> idx;
				for (size_t i = i0; i <= i1; i++)
					if (st[i].coarse || i == i0 || i == i1) idx.push_back(i);
				std::vector<Ring> sw;
				std::vector<double> ca(idx.size()), cb(idx.size());
				for (size_t j = 0; j < idx.size(); j++) chamferAt(idx[j], ca[j], cb[j]);
				Ring e;
				if (endRing(k, idx.front(), true, ca.front(), cb.front(), 0.0, e)) sw.push_back(e);
				for (size_t j = 0; j < idx.size(); j++) sw.push_back({ st[idx[j]].pos, ca[j], cb[j], 0.0 });
				if (endRing(kEnd, idx.back(), false, ca.back(), cb.back(), 0.0, e)) sw.push_back(e);
				if (bevels)
				{
					m::Manifold c = SpallSweep(fr[k], sw, segsCh, 1.0, 0.0, 1.0, 1.0, V(0.0), diag, round, shape);
					if (!c.IsEmpty()) bevels->push_back(std::move(c));
				}
				if (wearField)
					for (size_t j = 0; j + 1 < idx.size(); j++)
						wearField->segs.push_back({ st[idx[j]].pos, st[idx[j + 1]].pos, crest(ca[j], cb[j]), crest(ca[j + 1], cb[j + 1]) });
			}
		}
		if (patches.empty()) continue;

		                                                                              
		for (const auto& pc : piece)
		{
			const size_t k = pc.first, kEnd = pc.second;
			const Frame& f = fr[k];
			const size_t i0 = range[k].first, i1 = range[kEnd].second;
			size_t i = i0;
			while (i <= i1)
			{
				if (!alive[i]) { i++; continue; }
				size_t j = i;
				while (j + 1 <= i1 && alive[j + 1]) j++;
				std::vector<Ring> sw;
				const size_t a0 = i > i0 ? i - 1 : i;
				const size_t a1 = j < i1 ? j + 1 : j;
				auto shrink = [&](size_t idx) { return Ring{ st[idx].pos, std::max(sa[idx], 1e-6 * diag), std::max(sb[idx], 1e-6 * diag) }; };
				Ring e;
				if (a0 == i0 && alive[i0] && endRing(k, i0, true, sa[i0], sb[i0], 1.0, e)) sw.push_back(e);
				for (size_t qi = a0; qi <= a1; qi++) sw.push_back(shrink(qi));
				if (a1 == i1 && alive[i1] && endRing(kEnd, i1, false, sa[i1], sb[i1], 1.0, e)) sw.push_back(e);
				const double rScale = std::max(1e-6, 0.45 * D);
				if (damage)
				{
					m::Manifold c = SpallSweep(f, sw, segs, q, rough, rScale, octaves, offR, diag);
					if (!c.IsEmpty()) damage->push_back(std::move(c));
				}
				if (damageField)
					for (size_t qi = a0; qi < a1; qi++)
						damageField->segs.push_back({ st[qi].pos, st[qi + 1].pos,
							0.8 * std::max(sa[qi], sb[qi]), 0.8 * std::max(sa[qi + 1], sb[qi + 1]) });
				i = j + 1;
			}
		}

		                                                                              
		auto stationAt = [&](double arc, double& a, double& b, size_t& edge, V& pos) -> bool
		{
			if (run.closed) { arc = std::fmod(arc, total); if (arc < 0.0) arc += total; }
			if (arc < 0.0 || arc > total) return false;
			size_t i = 0;
			while (i + 1 < ns && st[i + 1].arc < arc) i++;
			const size_t j = std::min(i + 1, ns - 1);
			const double t = st[j].arc > st[i].arc ? ClampD((arc - st[i].arc) / (st[j].arc - st[i].arc), 0.0, 1.0) : 0.0;
			edge = (size_t)st[j].edge;
			pos = st[i].pos + (st[j].pos - st[i].pos) * t;
			a = Mix(sa[i], sa[j], t); b = Mix(sb[i], sb[j], t);
			if (a <= 0.0 || b <= 0.0)
			{
				                                                      
				a = b = std::max(p.width, 1e-6 * diag);
				const double tw = Mix(st[i].tTop, st[j].tTop, t), ts = Mix(st[i].tSide, st[j].tSide, t);
				if (tw > 0.0) a = std::min(a, p.maxShare * tw);
				if (ts > 0.0) b = std::min(b, p.maxShare * ts);
			}
			return true;
		};
		                                                              
		const double chipRate = Mix(0.08, 0.5, amount) / std::max({ D, 0.5 * p.damageDepth, 1e-9 });
		for (size_t pj = 0; pj < patches.size(); pj++)
		{
			const double s0 = patches[pj].first, s1 = patches[pj].second;
			const double plen = s1 - s0;
			const double want = chipRate * plen;
			int nIn = (int)std::floor(want);
			const uint64_t pid = (uint64_t)ri * 65537ULL + pj;
			if (Random01(seed, pid, 14) < want - nIn) nIn++;
			int nOut = whole ? 0 : (int)std::lround(p.chipScatter * Mix(0.3, 1.2, amount) * Random01(seed, pid, 15) * 2.0);
			nIn = std::min(nIn, 4000);
			for (int c = 0; c < nIn + nOut; c++)
			{
				const uint64_t cid = pid * 10007ULL + (uint64_t)c;
				const bool loose = c >= nIn;
				double arc;
				if (!loose) arc = s0 + Random01(seed, cid, 1) * plen;
				else
				{
					                                                  
					const double reach = std::min(0.5 * p.patchLength, 4.0 * D) * (0.2 + Random01(seed, cid, 1));
					arc = Random01(seed, cid, 2) < 0.5 ? s0 - reach : s1 + reach;
				}
				double a = 0.0, b = 0.0;
				size_t edge = 0;
				V E;
				if (!stationAt(arc, a, b, edge, E)) continue;
				const Frame& f = fr[edge];
				                                                                   
				                                                                     
				const bool top = Random01(seed, cid, 2) < 0.5;
				const V P = top ? E + f.dA * a : E + f.dB * b;
				const V N = top ? f.nA : f.nB;
				if (mask && !mask->Keep(P, (int32_t)(chipCounter & 0x7fffffff), kStream)) { chipCounter++; continue; }
				chipCounter++;
				double sz = D * (loose ? Mix(0.15, 0.4, Random01(seed, cid, 3)) : Mix(0.3, 0.8, Random01(seed, cid, 3)));
				if (mask) sz *= mask->SizeScale(P);
				sz = std::min(sz, 0.9 * std::max({ a, b, 0.4 * D }));                                          
				if (sz < 2e-5 * diag) continue;
				const double depth = sz * Mix(0.4, 0.65, Random01(seed, cid, 4));
				const double stretch = Mix(1.0, 2.2, Random01(seed, cid, 5));
				const double twist = (Random01(seed, cid, 6) - 0.5) * 0.8;
				const V noff(Random01(seed, cid, 7) * 50.0, Random01(seed, cid, 8) * 50.0, Random01(seed, cid, 9) * 50.0);
				if (damage)
				{
					m::Manifold ch = Chip(P, N, f.t, sz, depth, stretch, twist, (1.0 - 0.7 * s) * (1.0 - 0.8 * ks), noff, chipRes);
					if (ch.IsEmpty()) continue;
					damage->push_back(std::move(ch));
				}
				if (damageField) damageField->chips.push_back({ P, sz * stretch * 1.1 });
			}
		}
	}
}

void DamagedBevelSegments(const CoreInput& in, const DamagedBevelParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<double>& points, std::vector<int32_t>& tris)
{
	points.clear();
	tris.clear();
	if (!p.enable) return;
	std::vector<m::Manifold> bevels, damage;
	PlaceDamagedBevel(in, p, mainSeed, mask, paintStrokes, &bevels, &damage, nullptr, nullptr);
	if (damage.empty()) return;
	const MeshView& mv = in.view;
	m::MeshGL64 g;
	g.numProp = 3;
	g.vertProperties.reserve((size_t)mv.npts * 3);
	for (int32_t i = 0; i < mv.npts; i++)
	{
		g.vertProperties.push_back(mv.pts[i].x);
		g.vertProperties.push_back(mv.pts[i].y);
		g.vertProperties.push_back(mv.pts[i].z);
	}
	for (int32_t i = 0; i < mv.npolys; i++)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		g.triVerts.insert(g.triVerts.end(), { (uint64_t)q.a, (uint64_t)q.b, (uint64_t)q.c });
		if (q.c != q.d) g.triVerts.insert(g.triVerts.end(), { (uint64_t)q.a, (uint64_t)q.c, (uint64_t)q.d });
	}
	m::Manifold solid(g);
	if (solid.Status() != m::Manifold::Error::NoError || solid.IsEmpty()) return;
	                                                                       
	m::Manifold body = bevels.empty() ? solid : solid - m::Manifold::BatchBoolean(bevels, m::OpType::Add);
	const m::Manifold seg = body ^ m::Manifold::BatchBoolean(damage, m::OpType::Add);
	if (seg.IsEmpty()) return;
	const m::MeshGL64 o = seg.GetMeshGL64();
	const size_t np = o.NumVert(), stride = o.numProp;
	points.resize(np * 3);
	for (size_t v = 0; v < np; v++)
		for (int c = 0; c < 3; c++) points[v * 3 + (size_t)c] = o.vertProperties[v * stride + (size_t)c];
	tris.resize(o.triVerts.size());
	for (size_t t = 0; t < o.triVerts.size(); t++) tris[t] = (int32_t)o.triVerts[t];
}

PROJECT_DAMAGE_NS_MACRO_END
