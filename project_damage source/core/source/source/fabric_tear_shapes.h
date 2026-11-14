// общие объявления: строим контуры отверстий и надрывов ткани
// ProjectDamage realistic fabric tear shapes — GPL-3.0-or-later; see LICENSE.
//
// Recipe fabric-real-v1 (FabricTearParams::tearStyle = 1), shared by Fabric
// Classic (fabric_flat.cpp) and Fabric Solidify (fabric_solid.cpp). Woven
// cloth does not tear into smooth lenses: it parts along its yarns. Shapes,
// all in a 2D frame whose x axis is the warp (holeDir) and y the weft:
//   Rip       a straight tear along one yarn family (Grain Snap; otherwise a
//             bias tear at any angle), opened like an eye by the two pulled
//             edges, one side more than the other, with small sideways jogs
//             where it jumped a yarn and hairline tips running on;
//   Corner    the three-cornered (L) tear of a snag on a nail: two legs along
//             the warp and the weft, the triangular flap between them folded
//             back over the line joining the leg tips (Flap Lift);
//   Worn      an abrasion hole (knees, elbows): a squarish oval along the
//             grain, small worn-through satellites around it, and the yarns
//             of one family still spanning it (denim: the white weft);
//   Puncture  a snag or stab: a small core with short slits along the grain.
// Rim: not isotropic noise. The edge is made of yarn ends - per yarn bundle
// (Yarn Size) an offset ALONG THE YARN that crosses the edge, a smooth
// mean-reverting walk eased over the neighbours - so the rim frays along the
// warp and the weft like real torn cloth, without square teeth.
// Threads come from the yarns themselves: every yarn row (both families,
// Fray Spacing apart) that crosses a hole is a gap; it either spans it
// (bridge, sagging), broke in the middle (two loose ends) or frayed (stubs
// at the rim pointing ALONG the yarn, not along the rim normal).
#pragma once
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "fabric_tears_params.h"
#include "fabric_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fabric_shapes {
namespace m = manifold;
using V2 = m::vec2;
using V3 = m::vec3;

inline double R01(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double Noise1(double x, uint64_t seed, uint64_t ch)
{
	const double f = std::floor(x), t = x - f;
	const double a = R01(seed, uint64_t(int64_t(f) + (1 << 20)), ch) * 2 - 1;
	const double b = R01(seed, uint64_t(int64_t(f) + 1 + (1 << 20)), ch) * 2 - 1;
	const double s = t * t * (3 - 2 * t);
	return a + (b - a) * s;
}
inline V2 Rot(double a) { return V2(std::cos(a), std::sin(a)); }
inline double Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }

enum Kind { kRip = 0, kCorner = 1, kWorn = 2, kPuncture = 3, kEdge = 4 };

                                                                             
                                                         
struct Yarn { V2 a, b; int kind = 0; double len = 0.0; uint64_t key = 0; int family = 0; double row = 0.0, spacing = 0.25; };
                                                                                 
                                                          
struct Flap { m::Polygons poly; V2 h0, h1, tip; double angle = 0.0; };
struct Shape
{
	int kind = kRip;
	m::Polygons holes;                                                               
	std::vector<Flap> flaps;
	std::vector<Yarn> yarns;
	double reach = 0.0;                                                               
	double pitch = 0.25;
};

inline double RingArea(const m::SimplePolygon& r)
{
	double a = 0.0;
	for (size_t i = 0, n = r.size(); i < n; ++i) a += Cross2(r[i], r[(i + 1) % n]);
	return 0.5 * a;
}
inline double RingLength(const m::SimplePolygon& r)
{
	double l = 0.0;
	for (size_t i = 0, n = r.size(); i < n; ++i) l += m::la::length(r[(i + 1) % n] - r[i]);
	return l;
}
                                                                         
inline m::Polygons OuterRings(const m::CrossSection& cs, double minArea)
{
	m::Polygons out;
	for (const auto& r : cs.ToPolygons())
		if (r.size() >= 3 && RingArea(r) > minArea) out.push_back(r);
	return out;
}
                                
inline double SegDist(const V2& q, const V2& a, const V2& b)
{
	const V2 d = b - a;
	const double l2 = m::la::dot(d, d);
	const double t = l2 > 0 ? std::clamp(m::la::dot(q - a, d) / l2, 0.0, 1.0) : 0.0;
	return m::la::length(q - (a + d * t));
}

                                                                             
                                                                             
                                                                        
                                                              
inline m::Polygons Comb(const m::Polygons& in, double pitch, double amp, uint64_t seed, uint64_t ch,
	const std::vector<std::pair<V2, V2>>& hinges = {})
{
	m::Polygons rings;
	uint64_t ring = 0;
	for (const auto& src : in)
	{
		++ring;
		const double len = RingLength(src);
		if (src.size() < 3 || !(len > 0.0)) continue;
		const int nb = std::max(6, int(std::ceil(len / pitch)));
		const double step = len / nb;
		           
		std::vector<V2> pts;
		pts.reserve((size_t)nb);
		{
			size_t e = 0;
			double at = 0.0, eStart = 0.0;
			const size_t n = src.size();
			for (int k = 0; k < nb; ++k)
			{
				const double s = k * step;
				while (e < n)
				{
					const double el = m::la::length(src[(e + 1) % n] - src[e]);
					if (s <= eStart + el || e + 1 == n) break;
					eStart += el; ++e;
				}
				const V2 a = src[e % n], b = src[(e + 1) % n];
				const double el = m::la::length(b - a);
				at = el > 0 ? std::clamp((s - eStart) / el, 0.0, 1.0) : 0.0;
				pts.push_back(a + (b - a) * at);
			}
		}
		const size_t n = pts.size();
		std::vector<V2> disp(n, V2(0.0));
		double walk = 0.0;
		for (size_t k = 0; k < n; ++k)
		{
			const V2 a = pts[k], b = pts[(k + 1) % n];
			const V2 d = b - a;
			const double l = m::la::length(d);
			if (l < 1e-12) continue;
			const V2 out(d.y / l, -d.x / l);                                                   
			const uint64_t key = ring * 1000003ULL + k;
			                                                               
			                                                              
			walk = 0.75 * walk + 0.25 * (2.0 * R01(seed, key, ch) - 1.0);
			double lev = amp * (0.35 + 0.9 * walk);
			bool hinge = false;
			const V2 mid = (a + b) * 0.5;
			for (const auto& h : hinges) if (SegDist(mid, h.first, h.second) < 1.5 * pitch) { hinge = true; break; }
			if (hinge) continue;
			const V2 ax = std::abs(out.x) >= std::abs(out.y) ? V2(out.x > 0 ? 1.0 : -1.0, 0.0) : V2(0.0, out.y > 0 ? 1.0 : -1.0);
			if (lev > 0.0)
			{
				                                                               
				                                                 
				const V2 v = ax * -1.0;
				double t = 1e300;
				for (const auto& rr : in)
					for (size_t i = 0, nr = rr.size(); i < nr; ++i)
					{
						const V2 p0 = rr[i], e = rr[(i + 1) % nr] - p0;
						const double den = Cross2(v, e);
						if (std::abs(den) < 1e-18) continue;
						const V2 w = p0 - mid;
						const double tt = Cross2(w, e) / den, uu = Cross2(w, v) / den;
						if (tt > 1e-9 && uu >= 0.0 && uu <= 1.0) t = std::min(t, tt);
					}
				if (t < 1e299) lev = std::min(lev, 0.3 * t);
			}
			disp[k] = ax * -lev;                                                               
		}
		                                                                
		                                                                  
		m::SimplePolygon r;
		r.reserve(n);
		for (size_t k = 0; k < n; ++k)
		{
			const V2 d = disp[(k + n - 1) % n] * 0.25 + disp[k] * 0.5 + disp[(k + 1) % n] * 0.25;
			r.push_back(pts[k] + d);
		}
		rings.push_back(std::move(r));
	}
	if (rings.empty()) return {};
	const m::CrossSection cs = m::CrossSection(rings, m::CrossSection::FillRule::Positive).Simplify(0.02 * pitch);
	return OuterRings(cs, 0.5 * pitch * pitch);
}

                                                                             
                                                 
inline std::vector<std::pair<V2, V2>> YarnGaps(const m::Polygons& holes, int family, double spacing, double phase)
{
	std::vector<std::pair<V2, V2>> out;
	if (holes.empty() || !(spacing > 0.0)) return out;
	double lo = 1e300, hi = -1e300;
	for (const auto& r : holes) for (const V2& q : r) { const double c = family == 0 ? q.y : q.x; lo = std::min(lo, c); hi = std::max(hi, c); }
	const int nRows = int((hi - lo) / spacing) + 2;
	if (nRows > 20000) return out;
	std::vector<double> xs;
	for (int i = 0; i < nRows; ++i)
	{
		const double c = std::floor(lo / spacing) * spacing + (i + phase) * spacing;
		if (c <= lo || c >= hi) continue;
		xs.clear();
		for (const auto& r : holes)
			for (size_t k = 0, n = r.size(); k < n; ++k)
			{
				const V2 a = r[k], b = r[(k + 1) % n];
				const double ca = family == 0 ? a.y : a.x, cb = family == 0 ? b.y : b.x;
				if ((ca > c) == (cb > c)) continue;
				const double t = (c - ca) / (cb - ca);
				xs.push_back(family == 0 ? a.x + (b.x - a.x) * t : a.y + (b.y - a.y) * t);
			}
		std::sort(xs.begin(), xs.end());
		for (size_t k = 0; k + 1 < xs.size(); k += 2)
			out.push_back(family == 0 ? std::make_pair(V2(xs[k], c), V2(xs[k + 1], c)) : std::make_pair(V2(c, xs[k]), V2(c, xs[k + 1])));
	}
	return out;
}

inline int PickKind(const FabricTearParams& p, double r)
{
	const double w[4] = { std::max(0.0, p.tearRip), std::max(0.0, p.tearCorner), std::max(0.0, p.tearWorn), std::max(0.0, p.tearPuncture) };
	const double sum = w[0] + w[1] + w[2] + w[3];
	if (!(sum > 0.0)) return kRip;
	double acc = 0.0;
	for (int k = 0; k < 4; ++k) { acc += w[k] / sum; if (r < acc) return k; }
	return kWorn;
}

                                                                            
inline void TornLine(std::vector<V2>& out, const V2& a, const V2& b, double pitch, double meander, uint64_t seed, uint64_t key, bool last)
{
	const V2 d = b - a;
	const double l = m::la::length(d);
	if (l < 1e-12) { out.push_back(a); return; }
	const V2 q(-d.y / l, d.x / l);
	const int n = std::max(4, int(l / pitch));
	const int nj = std::min(24, int(meander * l / (6.0 * pitch) * R01(seed, key, 1) + 0.5));
	for (int i = 0; i < (last ? n + 1 : n); ++i)
	{
		const double f = double(i) / n;
		double off = 0.0;
		for (int j = 0; j < nj; ++j)
		{
			const double at = R01(seed, key, 10 + j), h = (R01(seed, key, 40 + j) - 0.5) * 4.0 * pitch;
			const double s = std::clamp((f - at) * l / pitch + 0.5, 0.0, 1.0);
			off += h * s * s * (3 - 2 * s) * std::sin(m::kPi * f);
		}
		out.push_back(a + d * f + q * off);
	}
}

                                                                      
inline m::Polygons RipOutline(const FabricTearParams& p, double u0, double u1, double W, const V2& dir, double pitch,
	double lobes, double meander, double taper, bool edge, uint64_t seed, uint64_t key)
{
	const V2 q(-dir.y, dir.x);
	const double L = u1 - u0;
	const int n = std::clamp(int(L / (0.5 * pitch)), 24, 1600);
	const double tp = 0.5 + (R01(seed, key, 2) - 0.5) * 0.5;                              
	const double gam = std::log(0.5) / std::log(std::clamp(tp, 0.2, 0.8));
	const double share = Lerp(0.3, 0.7, R01(seed, key, 3));                                       
	const int nj = int(meander * L / std::max(1e-6, W) * 1.5 + R01(seed, key, 4) * 2.0);
	std::vector<V2> left, right;
	for (int i = 0; i <= n; ++i)
	{
		const double f = double(i) / n, u = Lerp(u0, u1, f);
		double c = meander * 0.25 * W * Noise1(u / std::max(1e-6, 0.5 * L), seed ^ key, 5);
		for (int j = 0; j < nj && j < 24; ++j)
		{
			const double at = R01(seed, key, 100 + j), h = (R01(seed, key, 130 + j) * 2 - 1) * (1.0 + 2.0 * meander) * pitch * 1.5;
			const double s = std::clamp((f - at) * L / (0.6 * pitch) + 0.5, 0.0, 1.0);
			c += h * s * s * (3 - 2 * s);
		}
		double base;
		if (edge) base = std::pow(std::max(0.0, 1.0 - std::max(0.0, u) / std::max(1e-9, u1)), std::max(0.2, taper));
		else base = std::pow(std::max(0.0, std::sin(m::kPi * std::pow(f, gam))), 0.75);
		const double wl = W * share * base * std::max(0.05, 1.0 + lobes * 0.35 * Noise1(u / std::max(1e-6, 0.22 * L), seed ^ key, 6));
		const double wr = W * (1 - share) * base * std::max(0.05, 1.0 + lobes * 0.35 * Noise1(u / std::max(1e-6, 0.22 * L), seed ^ key, 7));
		left.push_back(dir * u + q * (c + wl));
		right.push_back(dir * u + q * (c - wr));
	}
	m::SimplePolygon r;
	for (const V2& v : right) r.push_back(v);
	for (int i = (int)left.size() - 1; i >= 0; --i) r.push_back(left[(size_t)i]);
	if (RingArea(r) < 0) std::reverse(r.begin(), r.end());
	return { r };
}
                                                                             
inline m::SimplePolygon Hairline(const V2& a, const V2& dir, double len, double w)
{
	const V2 q(-dir.y, dir.x);
	m::SimplePolygon r = { a - dir * w + q * w, a - dir * w - q * w, a + dir * len };
	if (RingArea(r) < 0) std::reverse(r.begin(), r.end());
	return r;
}

                                                                        
                                                                          
                                                                          
                                                                              
inline void PopulateYarns(Shape& s, const FabricTearParams& p, uint64_t seed, uint64_t key,
    int kind = kPuncture, double W = 1.0, double gapRef = 1e30,
    double bridgeScale = 1.0, int keepFamily = 0,
    const std::vector<std::pair<V2, V2>>& hinges = {}, double amp = 0.0)
{
    const double pitch = s.pitch;
	                                                                
	if (!p.threadsEnable || p.maxThreads <= 0) return;
	const double spacing = std::max(std::max(1e-4, p.fraySpacing), 2.5 * std::max(1e-5, p.threadRadius));
	const double rowRatio = spacing / std::max(spacing, p.threadSpacing);                                    
	auto onHinge = [&](const V2& q) {
		for (const auto& h : hinges) if (SegDist(q, h.first, h.second) < 2.0 * pitch + amp) return true;
		return false;
	};
	for (int fam = 0; fam < 2; ++fam)
	{
		const auto gaps = YarnGaps(s.holes, fam, spacing, 0.5);
		uint64_t gi = 0;
		for (const auto& g : gaps)
		{
			if (s.yarns.size() >= size_t(p.maxThreads)) return;
			++gi;
			const uint64_t yk = key * 1000003ULL + uint64_t(fam) * 500000ULL + gi;
			const double len = m::la::length(g.second - g.first);
			                                                            
			if (len < 2.0 * std::max(1e-5, p.threadRadius) || len < 1.5 * pitch) continue;
			                                                                      
			if ((kind == kRip || kind == kEdge || kind == kCorner) && len > 1.2 * W) continue;
			const double row = (fam == 0 ? g.first.y : g.first.x) / spacing - 0.5;
			                                                               
			                                                                     
			const double coherent = 0.5 + 0.5 * Noise1(row / 4.0, seed ^ key, 1200 + fam);
			const bool h0 = onHinge(g.first), h1 = onHinge(g.second);
			double pb = 0.0;
			if (kind == kWorn) pb = fam == keepFamily ? std::min(0.92, 2.2 * p.bridgeShare) : 0.1 * p.bridgeShare;
			else pb = p.bridgeShare * bridgeScale * std::clamp(1.2 - len / std::max(1e-9, gapRef), 0.1, 1.0);
			pb *= (kind == kWorn && fam == keepFamily) ? 1.0 : rowRatio;
			if (!p.bridgeEnable) pb = 0.0;
			const double pick = 0.75 * (0.5 + 0.5 * Noise1(row / 3.0, seed ^ key, 1300 + fam)) + 0.25 * R01(seed, yk, 1);
			if (p.bridgeEnable && !h0 && !h1 && pick < pb)
			{
				Yarn y; y.a = g.first; y.b = g.second; y.kind = 0; y.len = len; y.key = yk;
				y.family = fam; y.row = row; y.spacing = spacing;
				if (s.yarns.size() < size_t(p.maxThreads)) s.yarns.push_back(y);
				continue;
			}
			const double pr = p.brokenShare * (kind == kWorn && fam == keepFamily ? 1.0 : 0.6) * rowRatio;
			if (p.brokenEnable && !h0 && !h1 && pick < pb + pr)
			{
				for (int side = 0; side < 2; ++side)
				{
					Yarn y; y.a = side ? g.second : g.first; y.b = side ? g.first : g.second; y.kind = 1;
					y.len = len * Lerp(0.2, 0.5, (0.8 * coherent + 0.2 * R01(seed, yk, 2 + side))); y.key = yk * 2 + side;
					y.family = fam; y.row = row; y.spacing = spacing;
					if (s.yarns.size() < size_t(p.maxThreads)) s.yarns.push_back(y);
				}
				continue;
			}
			if (!p.frayEnable) continue;
			for (int side = 0; side < 2; ++side)
			{
				if (side ? h1 : h0) continue;
				if (R01(seed, yk, 5 + side) > 0.85) continue;
				Yarn y; y.a = side ? g.second : g.first; y.b = side ? g.first : g.second; y.kind = 2;
				y.len = std::min(0.45 * len, Lerp(p.frayLengthMin, p.frayLengthMax, std::pow(0.8 * coherent + 0.2 * R01(seed, yk, 8 + side), 1.5)));
				y.key = yk * 2 + side;
				if (y.len > 1.5 * p.threadRadius) {
					y.family = fam; y.row = row; y.spacing = spacing;
					if (s.yarns.size() < size_t(p.maxThreads)) s.yarns.push_back(y);
				}
			}
		}
	}
    if (s.yarns.size() > size_t(p.maxThreads)) s.yarns.resize(size_t(p.maxThreads));
}

inline Shape MakeTearShape(const FabricTearParams& p, int kind, double L, double W, uint64_t seed, uint64_t key, V2 inward = V2(1, 0))
{
	Shape s;
	s.kind = kind;
	L = std::max(L, 1e-4);
	W = std::max(W, 1e-4);
	const bool edge = kind == kEdge;
	const double jag = edge ? p.edgeJagged : p.holeJagged;
	const double meander = edge ? p.edgeMeander : p.holeMeander;
	double pitch = std::max(1e-4, p.yarnSize / std::clamp(p.detail, 0.25, 4.0));
	pitch = std::max(pitch, 2.2 * (L + W) / 1500.0);                                                  
	s.pitch = pitch;
    if(p.tearStyle==2){
        s.kind=edge?kEdge:kWorn;
        s.holes=fabric_noise::Outline(p,L,W,seed,key,edge,inward);
        s.reach=L+W;
        PopulateYarns(s,p,seed,key,s.kind,W,1e30,1.,int(R01(seed,key,902)<.5));
        return s;
    }

	const double amp = std::min(pitch * (0.4 + 1.6 * jag), 0.22 * W);
	const bool snapped = R01(seed, key, 900) < std::clamp(p.grainSnap, 0.0, 1.0);
	const double jit = snapped ? (R01(seed, key, 901) - 0.5) * 0.06 : 0.0;
	m::Polygons base, extra;
	std::vector<std::pair<V2, V2>> hinges;
	int keepFamily = -1;                                                        
	double bridgeScale = 0.6, gapRef = 0.8 * W;
	if (kind == kRip || kind == kEdge)
	{
		V2 dir;
		if (edge) dir = inward;
		else dir = snapped ? Rot((R01(seed, key, 902) < 0.5 ? 0.0 : 0.5 * m::kPi) + jit) : Rot(R01(seed, key, 902) * m::kPi);
		const double u0 = edge ? -0.3 * W : -0.5 * L, u1 = edge ? L : 0.5 * L;
		                                                                         
		if (!edge) W *= Lerp(0.45, 0.75, R01(seed, key, 905));
		base = RipOutline(p, u0, u1, W, dir, pitch, p.holeLobes, meander, p.edgeTaper, edge, seed, key);
		const double hw = std::max(0.3 * pitch, std::min(0.6 * pitch, 0.08 * W));
		const double e1 = L * Lerp(0.05, 0.14, R01(seed, key, 903));
		extra.push_back(Hairline(dir * (u1 - 0.1 * L), dir, e1 + 0.1 * L, hw));
		if (!edge) extra.push_back(Hairline(dir * (u0 + 0.1 * L), dir * -1.0, L * Lerp(0.15, 0.24, R01(seed, key, 904)), hw));
		s.reach = (edge ? L : 0.5 * L) + 0.15 * L + W;
	}
	else if (kind == kCorner)
	{
		const double beta = std::floor(R01(seed, key, 910) * 4.0) * 0.5 * m::kPi + (snapped ? jit : (R01(seed, key, 911) - 0.5) * 0.4);
		const V2 ex = Rot(beta), ey = Rot(beta + 0.5 * m::kPi + (snapped ? 0.0 : (R01(seed, key, 912) - 0.5) * 0.35));
		const double L1 = L * Lerp(0.35, 0.6, R01(seed, key, 913)), L2 = L * Lerp(0.25, 0.55, R01(seed, key, 914));
		V2 C(0.0), A = ex * L1, B = ey * L2;
		const V2 ctr = (C + A + B) / 3.0;
		C -= ctr; A -= ctr; B -= ctr;
		std::vector<V2> pts;
		TornLine(pts, C, A, pitch, meander, seed, key * 7 + 1, false);
		{                             
			const int n = std::max(4, int(m::la::length(B - A) / pitch));
			for (int i = 0; i < n; ++i) pts.push_back(A + (B - A) * (double(i) / n));
		}
		TornLine(pts, B, C, pitch, meander, seed, key * 7 + 2, false);
		m::SimplePolygon r(pts.begin(), pts.end());
		if (RingArea(r) < 0) std::reverse(r.begin(), r.end());
		base = { r };
		hinges.push_back({ A, B });
		const double hw = std::max(0.25 * pitch, std::min(0.45 * pitch, 0.06 * W));
		extra.push_back(Hairline(A - ex * (0.03 * L1), ex, L1 * Lerp(0.04, 0.12, R01(seed, key, 915)), hw));
		extra.push_back(Hairline(B - ey * (0.03 * L2), ey, L2 * Lerp(0.04, 0.12, R01(seed, key, 916)), hw));
		Flap f;
		f.h0 = A; f.h1 = B; f.tip = C;
		f.angle = std::clamp(p.flapLift, 0.0, 1.0) * m::kPi * Lerp(0.55, 0.95, R01(seed, key, 917));
		s.flaps.push_back(f);                                
		s.reach = std::max(L1, L2) + W;
		bridgeScale = 0.0;
	}
	else if (kind == kWorn)
	{
		const double D = std::max(W, W + (0.55 * L - W) * R01(seed, key, 920));
		const double ar = 1.0 + 0.6 * R01(seed, key, 921);
		const bool alongX = R01(seed, key, 922) < 0.5;
		const double ax = 0.5 * D * (alongX ? ar : 1.0) / std::sqrt(ar), ay = 0.5 * D * (alongX ? 1.0 : ar) / std::sqrt(ar);
		const double ex = 2.4 + 1.2 * R01(seed, key, 923);
		double ph[7], am[7];
		for (int k = 2; k < 7; ++k) { ph[k] = R01(seed, key, 930 + k) * 2 * m::kPi; am[k] = (2 * R01(seed, key, 940 + k) - 1) / std::pow(double(k), 0.8); }
		auto radius = [&](double th, double sx, double sy) {
			const double c = std::abs(std::cos(th)) / sx, sn = std::abs(std::sin(th)) / sy;
			double r = std::pow(std::pow(c, ex) + std::pow(sn, ex), -1.0 / ex);
			double lob = 0.0;
			for (int k = 2; k < 7; ++k) lob += am[k] * std::cos(k * th + ph[k]);
			return r * std::max(0.35, 1.0 + p.holeLobes * 0.35 * lob);
		};
		auto blob = [&](const V2& c0, double sx, double sy, uint64_t sk) {
			const int n = std::clamp(int(m::kPi * (sx + sy) / (0.5 * pitch)), 24, 1600);
			const double rot = (R01(seed, sk, 1) - 0.5) * 0.3;
			m::SimplePolygon r;
			for (int i = 0; i < n; ++i)
			{
				const double th = 2 * m::kPi * i / n;
				r.push_back(c0 + Rot(th + rot) * radius(th, sx, sy));
			}
			return r;
		};
		base.push_back(blob(V2(0.0), ax, ay, key * 3 + 1));
		const int nSat = int(R01(seed, key, 924) * std::max(1, p.holeSplitMax));
		for (int k = 0; k < nSat; ++k)
		{
			const double th = R01(seed, key, 950 + k) * 2 * m::kPi;
			const double rs = 0.5 * D * Lerp(0.1, 0.28, R01(seed, key, 960 + k));
			const double dist = radius(th, ax, ay) + rs * 1.3 + 0.5 * p.holeBridge + 2.0 * pitch;
			base.push_back(blob(Rot(th) * dist, rs * Lerp(0.8, 1.3, R01(seed, key, 970 + k)), rs, key * 3 + 2 + k));
		}
		keepFamily = alongX ? 0 : 1;
		gapRef = 1e300;
		s.reach = 0.5 * D * ar + 0.5 * D + p.holeBridge + W;
	}
	else               
	{
        base=fabric_noise::Outline(p,L,W,seed,key);
        s.reach=L+W;bridgeScale=.3;gapRef=W;
	}
	m::Polygons holes = Comb(base, pitch, kind == kPuncture ? 0.5 * amp : amp, seed, key * 31 + 7, hinges);
	if (!s.flaps.empty()) s.flaps[0].poly = holes;
	for (const auto& r : extra) holes.push_back(r);
	s.holes = OuterRings(m::CrossSection(holes, m::CrossSection::FillRule::Positive), 0.5 * pitch * pitch);
	if (s.holes.empty()) { s.flaps.clear(); return s; }

	PopulateYarns(s, p, seed, key, kind, W, gapRef, bridgeScale, keepFamily, hinges, amp);
	return s;
}

                                                                         
                                                                               
                                            
struct YarnPath { std::vector<V3> path; double r0 = 0, r1 = 0; int tag = -1; };
template <class Lift>
inline void YarnPaths(const Shape& s, const FabricTearParams& p, uint64_t seed, Lift lift,
	const V3& T, const V3& inward, const V3& gT, size_t cap, std::vector<YarnPath>& out)
{
	const double tr = std::max(1e-5, p.threadRadius);
	auto unit = [](const V3& v, const V3& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; };
	for (const Yarn& y : s.yarns)
	{
		if (out.size() >= cap) return;
		V3 a, b;
		int ta = -1, tb = -1;
		if (!lift(y.a, a, ta)) continue;
        const V3 B = unit(m::la::cross(-inward, T), V3(0, 1, 0));
        const V2 delta = y.b - y.a;
        const bool hasOtherRim = lift(y.b, b, tb);
        if (!hasOtherRim) {
            if (y.kind == 0) continue;                                         
            b = a + T * delta.x + B * delta.y;
        }
        const double span = m::la::length(b - a);
        if (span < 2.0 * tr) continue;
        const V3 across = (b - a) / span;
        const V3 side = unit(m::la::cross(across, inward), T);
        const double spacing = std::max(y.spacing, 2.5 * tr);
        const double bundle = std::floor(y.row / 4.0);
        const double phase = Noise1(bundle * 0.43, seed, 1500 + y.family);
        const double skew = std::clamp(p.threadSkew, 0.0, 1.0);
        const double wobble = std::clamp(p.threadWobble, 0.0, 4.0);
        YarnPath yp;
        yp.tag = ta;
        if (y.kind == 0)
        {
            yp.r0 = yp.r1 = tr;
                                                                                 
            const int ns = std::clamp(int(std::ceil(span / std::max(2.0 * tr, 0.5 * spacing))), 8, 64);
            const double sag = std::clamp(p.threadSag, 0.0, 1.0) * span;
                                                                                   
            const double weaveQuality = std::clamp(ns * spacing / span - 1.0, 0.0, 1.0);
            yp.path.push_back(a - across * (1.5 * tr));
            for (int i = 0; i <= ns; ++i)
            {
                const double f = double(i) / ns;
                const double bell = 16.0 * f * f * (1.0 - f) * (1.0 - f);
                const V2 q = y.a + delta * f;
                const double along = y.family == 0 ? q.x : q.y;
                                                                               
                                                                              
                const double weave = (y.family == 0 ? 1.0 : -1.0) * 1.25 * tr
                    * weaveQuality * std::min(1.0, 4.0 * wobble)
                    * std::cos(m::kPi * (along / spacing - 0.5 + y.row));
                const double sway = skew * std::min(0.08 * span, spacing) * phase
                    + wobble * tr * Noise1(f * 3.0, seed, 1600 + y.family);
                yp.path.push_back(a + (b - a) * f + inward * ((sag + weave) * bell) + side * (sway * bell));
            }
            yp.path.push_back(b + across * (1.5 * tr));
        }
        else
        {
            const bool loose = y.kind == 1;
            yp.r0 = loose ? tr : 0.75 * tr;
            yp.r1 = yp.r0 * std::clamp(p.threadTaper, 0.05, loose ? 1.0 : 0.45);
            const double len = std::min(y.len, 0.48 * span);
            if (len <= 1.5 * tr) continue;
            const int ns = std::clamp(int(std::ceil(len / std::max(2.0 * tr, len / 18.0))), 6, 32);
                                                                             
                                                                           
                                                                             
            const double clump = std::clamp(((bundle * 4.0 + 1.5) - y.row) * spacing,
                                            -0.3 * len, 0.3 * len) * 0.6;
            const double angle = std::clamp(p.threadCurl, 0.0, 4.0) * (0.3 + 0.7 * std::abs(phase));
            const double droop = std::clamp(p.threadDroop, 0.0, 1.0);
            yp.path.push_back(a - across * (1.5 * tr));
            yp.path.push_back(a);
            V3 cur = a;
            for (int i = 0; i < ns; ++i)
            {
                const double f = (double(i) + 0.5) / ns, bend = f * f;
                const double theta = angle * bend;
                const double gather = clump / len * 3.0 * f * (1.0 - f);
                const V3 rowDirection = y.family == 0 ? B : T;
                const double lateral = wobble * 0.12 * bend * Noise1(f * 2.0 + y.row / 4.0, seed, 1700 + y.family);
                const V3 curlPlane = unit(inward + side * (0.5 * phase), inward);
                const V3 tangent = unit(across * std::cos(theta) + curlPlane * std::sin(theta)
                    + (inward * 0.6 + gT * 0.4) * (droop * bend) + side * lateral + rowDirection * gather, across);
                cur += tangent * (len / ns);
                yp.path.push_back(cur);
            }
        }
		(void)T; (void)tb;
		out.push_back(std::move(yp));
	}
}

                                                                       
                                                                           
                                                                        
struct Fold { V3 H, a, e, n; double R = 0.1, theta = 0.0; };
inline V3 FoldPoint(const Fold& f, const V3& P)
{
	const V3 r = P - f.H;
	const double along = m::la::dot(r, f.a), d = m::la::dot(r, f.e), z = m::la::dot(r, f.n);
	if (d <= 0.0 || f.theta <= 0.0) return P;
	const double rho = f.R - z, arc = f.R * f.theta;
	double phi = d / f.R, rest = 0.0;
	if (d > arc) { phi = f.theta; rest = d - arc; }
	const V3 axis = f.H + f.a * along + f.n * f.R;
	V3 q = axis + f.e * (rho * std::sin(phi)) - f.n * (rho * std::cos(phi));
	if (rest > 0.0) q += (f.e * std::cos(f.theta) + f.n * std::sin(f.theta)) * rest;
	return q;
}

}                             
