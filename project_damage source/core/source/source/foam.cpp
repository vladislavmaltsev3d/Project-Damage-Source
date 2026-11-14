// строим пену из объединённых неровных объёмов
// ProjectDamage expanding foam — GPL-3.0-or-later; see LICENSE.
//
// Recipe foam-v2 (FoamParams): spray foam squeezed out of TIGHT gaps (seams
// between plates, fractures, cracks) - not along edges.
//   gaps     candidates scattered over the surface by area; a candidate is in
//            a gap when the ray along its normal meets, within Max Gap, a
//            surface facing back at it (the gap's other wall). Edges and open
//            surfaces never qualify.
//   mouth    26 rays round the mid-gap point: the share that escapes (no hit
//            within 8 x the gap) is the openness - deep in the slit almost
//            none (skipped: hidden), at the mouth about half. The mean
//            escaping direction is where the foam comes out. Walking that way
//            until both gap walls are gone finds the mouth itself.
//   blobs    one per Blob Spacing x the gap, at the mouth; Coverage leaves
//            patches of the seam empty.
//   style 0  (foam-v1) a lumpy ellipsoid per blob, 42 points / 80 triangles.
//   style 1  (foam-v2) one distance field for the whole foam, meshed with
//            surface nets on a sparse voxel grid, so the bead is a single
//            skin with merged lobes:
//              plug     fills the gap behind the mouth
//              cap      the bead over the mouth: wider than the gap and
//                       spreading onto the faces (Spread), swelling with
//                       the openness (Bulge), thicker and thinner along the
//                       seam, pulled down and fatter below (Sag)
//              lobes    cauliflower bulbs grown out of the cap, and smaller
//                       ones out of those (Cauliflower)
//              drips    runs down a wall below a seam, teardrops hanging
//                       from a ceiling seam (Drips, Drip Length)
//              skin     Lumpiness noise and shallow bubble dents
//              walls    foam inside the plates is removed (kept a little
//                       embedded so it never floats)
//              trim     patches cut flush with a knife (Trim): flat faces
//                       marked in `cut` for the open-cell shader
//            Outputs per point: UV (box projection), variation (smooth,
//            low frequency), cut, cavity (crevices between lobes and the
//            contact with the wall, from the distance field).
#include "foam.h"
#include "project_damage_bvh.h"
#include <algorithm>
#include <array>
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
double Dot(const V& a, const V& b) { return m::la::dot(a, b); }
double Clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double Smooth01(double x) { x = Clamp01(x); return x * x * (3 - 2 * x); }
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL) {}
	double U() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0); }
};
double Hash3(const V& p, uint64_t seed)
{
	const int64_t x = (int64_t)std::floor(p.x), y = (int64_t)std::floor(p.y), z = (int64_t)std::floor(p.z);
	uint64_t h = seed ^ (uint64_t)x * 73856093ULL ^ (uint64_t)y * 19349663ULL ^ (uint64_t)z * 83492791ULL;
	h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
	return double(h >> 11) * (1.0 / 9007199254740992.0);
}
double Noise3(const V& p, uint64_t seed)
{
	const V f(std::floor(p.x), std::floor(p.y), std::floor(p.z)), t = p - f;
	const V s(t.x * t.x * (3 - 2 * t.x), t.y * t.y * (3 - 2 * t.y), t.z * t.z * (3 - 2 * t.z));
	auto h = [&](double i, double j, double k) { return Hash3(f + V(i, j, k), seed) * 2 - 1; };
	auto L = [](double a, double b, double t2) { return a + (b - a) * t2; };
	return L(L(L(h(0, 0, 0), h(1, 0, 0), s.x), L(h(0, 1, 0), h(1, 1, 0), s.x), s.y),
		L(L(h(0, 0, 1), h(1, 0, 1), s.x), L(h(0, 1, 1), h(1, 1, 1), s.x), s.y), s.z);
}
                                                                                   
double NoiseFast(const V& p, uint64_t seed)
{
	const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const double tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
	const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty), sz = tz * tz * (3 - 2 * tz);
	const uint64_t X = (uint64_t)(int64_t)fx * 73856093ULL, Y = (uint64_t)(int64_t)fy * 19349663ULL, Z = (uint64_t)(int64_t)fz * 83492791ULL;
	auto h = [&](uint64_t a, uint64_t b, uint64_t c) {
		uint64_t q = seed ^ a ^ b ^ c;
		q ^= q >> 33; q *= 0xff51afd7ed558ccdULL; q ^= q >> 33;
		return double(q >> 11) * (2.0 / 9007199254740992.0) - 1.0;
	};
	const uint64_t X1 = X + 73856093ULL, Y1 = Y + 19349663ULL, Z1 = Z + 83492791ULL;
	auto L = [](double a, double b, double t2) { return a + (b - a) * t2; };
	return L(L(L(h(X, Y, Z), h(X1, Y, Z), sx), L(h(X, Y1, Z), h(X1, Y1, Z), sx), sy),
		L(L(h(X, Y, Z1), h(X1, Y, Z1), sx), L(h(X, Y1, Z1), h(X1, Y1, Z1), sx), sy), sz);
}
                                                                
double Noise01(const V& p, uint64_t seed) { return Clamp01(0.5 + 0.9 * Noise3(p, seed)); }

                                                                  
void UnitSphere(std::vector<V>& pts, std::vector<std::array<int, 3>>& tris)
{
	const double ph = (1.0 + std::sqrt(5.0)) * 0.5;
	pts = { V(-1, ph, 0), V(1, ph, 0), V(-1, -ph, 0), V(1, -ph, 0), V(0, -1, ph), V(0, 1, ph), V(0, -1, -ph), V(0, 1, -ph),
		V(ph, 0, -1), V(ph, 0, 1), V(-ph, 0, -1), V(-ph, 0, 1) };
	for (V& p : pts) p = Unit(p, p);
	std::vector<std::array<int, 3>> f = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
	std::map<std::pair<int, int>, int> mid;
	auto midOf = [&](int a, int b) {
		const auto k = std::make_pair(std::min(a, b), std::max(a, b));
		auto it = mid.find(k);
		if (it != mid.end()) return it->second;
		pts.push_back(Unit(pts[(size_t)a] + pts[(size_t)b], V(0, 0, 1)));
		return mid[k] = (int)pts.size() - 1;
	};
	for (const auto& t : f)
	{
		const int ab = midOf(t[0], t[1]), bc = midOf(t[1], t[2]), ca = midOf(t[2], t[0]);
		tris.push_back({ t[0], ab, ca }); tris.push_back({ ab, t[1], bc }); tris.push_back({ ca, bc, t[2] }); tris.push_back({ ab, bc, ca });
	}
}

struct Blob
{
	V c, n, out, seam;                      
	double w, open;
	V mouth;                                        
	double depth;                                 
};

                                                                           
void BeadsV1(const std::vector<Blob>& blobs, const FoamParams& f, const V& down, Rng& rng, FoamResult& r)
{
	std::vector<V> sp; std::vector<std::array<int, 3>> st;
	UnitSphere(sp, st);
	const uint64_t nseed = 0xF0A3ULL ^ (uint64_t)(uint32_t)f.seed;
	for (size_t bi = 0; bi < blobs.size(); ++bi)
	{
		const Blob& b = blobs[bi];
		const double var = rng.U();
		const double size = f.blobSize * (0.75 + 0.5 * rng.U());
		const double swell = 1.0 + f.bulge * std::clamp((b.open - f.minOpenness) / std::max(1e-6, 0.5 - f.minOpenness), 0.0, 1.0);
		const double rn = 0.5 * b.w * 1.15 * size * swell;
		const double ro = 0.5 * b.w * 1.3 * size * swell;
		const double rs = std::max(0.5 * b.w * f.spacing * 1.6, 1.4 * b.w) * size * (0.8 + 0.4 * swell / (1.0 + f.bulge));
		const double vertical = std::abs(Dot(b.seam, down));
		const V c = b.c + b.out * (0.6 * ro * std::clamp(b.open * 2.0, 0.0, 1.0)) + down * (f.sag * vertical * 0.3 * rs * rng.U());
		const uint32_t first = (uint32_t)r.points.size();
		const double uo = rng.U(), vo = rng.U();
		for (const V& d0 : sp)
		{
			V q = b.n * (d0.x * rn) + b.out * (d0.y * ro) + b.seam * (d0.z * rs);
			const double below = std::max(0.0, -Dot(Unit(q, V(0, 0, 1)), down * -1.0));
			q = q * (1.0 + f.sag * vertical * 0.25 * below);
			const V wp = (c + q) / std::max(1e-9, 0.6 * b.w);
			const double lump = 1.0 + f.lumpiness * 0.18 * Noise3(wp, nseed) - f.lumpiness * 0.08 * std::max(0.0, Noise3(wp * 3.1, nseed ^ 0x77) - 0.4) * 2.5;
			r.points.push_back(c + q * lump);
			r.uvs.push_back(std::atan2(d0.y, d0.x) / (2 * m::kPi) + 0.5 + uo);
			r.uvs.push_back(0.5 + 0.5 * d0.z + vo);
			r.variation.push_back((float)var);
			r.cut.push_back(0.0f);
			r.cavity.push_back(0.0f);
		}
		for (const auto& t : st)
			r.polys.push_back({ (Int32)(first + (uint32_t)t[0]), (Int32)(first + (uint32_t)t[1]), (Int32)(first + (uint32_t)t[2]), (Int32)(first + (uint32_t)t[2]) });
		++r.blobs;
	}
}

                                                                           
                                                                          
                                                 
struct Prim
{
	int kind = 0;                                                  
	V c, e0, e1, e2;                                           
	double r0 = 1, r1 = 1, r2 = 1;
	V gd; double gf = 1.0;                                                     
	V a, b; double ra = 0, rb = 0;
	double k = 0;                                       
	V lo, hi;                                     
};

double Smin(double a, double b, double k)
{
	if (k <= 0) return std::min(a, b);
	const double h = std::max(k - std::abs(a - b), 0.0) / k;
	return std::min(a, b) - h * h * k * 0.25;
}

double PrimDist(const Prim& p, const V& x)
{
	if (p.kind == 1)
	{
		const V pa = x - p.a, ba = p.b - p.a;
		const double h = std::clamp(Dot(pa, ba) / std::max(1e-30, Dot(ba, ba)), 0.0, 1.0);
		return Len(pa - ba * h) - (p.ra + (p.rb - p.ra) * h);
	}
	V q = x - p.c;
	if (p.gf > 1.0)
	{
		const double t = Dot(q, p.gd);
		if (t > 0) q = q - p.gd * (t * (1.0 - 1.0 / p.gf));
	}
	const V l(Dot(q, p.e0) / p.r0, Dot(q, p.e1) / p.r1, Dot(q, p.e2) / p.r2);
	const V l2(l.x / p.r0, l.y / p.r1, l.z / p.r2);
	const double k0 = Len(l), k1 = Len(l2);
	if (k1 < 1e-30) return -std::min(p.r0, std::min(p.r1, p.r2));
	return k0 * (k0 - 1.0) / k1;
}

struct FoamBlob
{
	V mouth, mid, n, o, s;
	double hw;                         
	double R;                         
	bool trimmed;
	double trimH;                                  
};

struct FieldCtx
{
	const std::vector<Prim>* prims;
	const std::vector<FoamBlob>* blobs;
	double lumpAmp, lumpFreq, embed;
	uint64_t nseed;
	double cull = 1e30;                                                                
};

struct FieldOut { double d; double wall; bool cut; };

                                                                             
FieldOut Field(const FieldCtx& C, const V& x, const std::vector<int>& plist, const std::vector<int>& blist)
{
	double d = 1e30;
	for (int pi : plist)
	{
		const Prim& p = (*C.prims)[(size_t)pi];
		if (x.x < p.lo.x - C.cull || x.y < p.lo.y - C.cull || x.z < p.lo.z - C.cull ||
			x.x > p.hi.x + C.cull || x.y > p.hi.y + C.cull || x.z > p.hi.z + C.cull) continue;
		d = Smin(d, PrimDist(p, x), p.k);
	}
	                                                              
	if (C.lumpAmp > 0 && d < 4.0 * C.lumpAmp + C.embed)
	{
		const V q = x * C.lumpFreq;
		const double n1 = NoiseFast(q, C.nseed), n2 = NoiseFast(q * 2.37 + V(5.1, 1.7, 3.3), C.nseed ^ 0x51);
		const double billow = 0.6 * (std::abs(n1) * 2.0 - 0.6) + 0.4 * n2;
		const double dent = std::max(0.0, NoiseFast(q * 4.3, C.nseed ^ 0x77) - 0.35);
		d += C.lumpAmp * (-billow + 1.6 * dent);
	}
	                                                                          
	double wall = -1e30, nearest = 1e30;
	const FoamBlob* nb = nullptr;
	for (int bi : blist)
	{
		const FoamBlob& b = (*C.blobs)[(size_t)bi];
		const double dist = Len(x - b.mouth);
		if (dist > 3.5 * b.R) continue;
		const double behind = Dot(x - b.mouth, b.o);
		const double side = b.hw > 0 ? b.hw - std::abs(Dot(x - b.mid, b.n)) : -1e30;                           
		wall = std::max(wall, std::max(behind, side));
		if (dist / b.R < nearest) { nearest = dist / b.R; nb = &b; }
	}
	FieldOut r{ d, wall > -1e29 ? wall : 1e30, false };
	if (wall > -1e29) r.d = std::max(r.d, -(wall + C.embed));
	if (nb && nb->trimmed)
	{
		const double plane = Dot(x - nb->mouth, nb->o) - nb->trimH;
		if (plane > r.d) { r.d = plane; r.cut = true; }
	}
	return r;
}

constexpr int B = 8;                                      
constexpr int BC = B + 1;                                   

uint64_t BlockKey(int64_t x, int64_t y, int64_t z)
{
	const uint64_t mask = (1ULL << 21) - 1;
	return ((uint64_t)(x + (1 << 20)) & mask) | (((uint64_t)(y + (1 << 20)) & mask) << 21) | (((uint64_t)(z + (1 << 20)) & mask) << 42);
}

struct Block
{
	int64_t bx, by, bz;
	std::vector<int> prims, blobs;
	std::vector<float> val;                             
	std::vector<int32_t> vert;                                    
	std::vector<V> pts, nrm;
	int32_t first = 0;                                                
	std::vector<std::array<Int32, 4>> quads;
};

void FoamV2(const std::vector<Blob>& blobs, const FoamParams& f, const V& down, double maxGap, FoamResult& r)
{
	if (blobs.empty()) return;
	const uint64_t seed = (uint64_t)(uint32_t)f.seed;
	const uint64_t nseed = 0xF0A3ULL ^ seed;
	std::vector<FoamBlob> fb;
	std::vector<Prim> prims;
	fb.reserve(blobs.size());
	                                                                             
	std::vector<double> radii;
	for (size_t i = 0; i < blobs.size(); ++i)
	{
		const Blob& b = blobs[i];
		const double we = std::max(b.w, 0.35 * maxGap);
		const double swell = 1.0 + f.bulge * Clamp01((b.open - f.minOpenness) / std::max(1e-6, 0.5 - f.minOpenness));
		                                               
		const double along = 0.7 + 0.6 * Noise01(b.mouth / (5.0 * we), nseed ^ 0x2A);
		radii.push_back(0.5 * we * f.blobSize * (0.7 + 0.55 * swell) * along);
	}
	                                                                           
	                                                   
	std::vector<double> reachS(blobs.size(), 0.0);
	{
		const double cellN = 6.0 * std::max(1e-9, maxGap);
		std::unordered_map<uint64_t, std::vector<int>> g;
		auto gk = [&](int64_t x, int64_t y, int64_t z) { return BlockKey(x, y, z); };
		auto ci = [&](const V& p) { return std::array<int64_t, 3>{ (int64_t)std::floor(p.x / cellN), (int64_t)std::floor(p.y / cellN), (int64_t)std::floor(p.z / cellN) }; };
		for (size_t i = 0; i < blobs.size(); ++i) { const auto c = ci(blobs[i].mouth); g[gk(c[0], c[1], c[2])].push_back((int)i); }
		for (size_t i = 0; i < blobs.size(); ++i)
		{
			const Blob& a = blobs[i];
			const double we = std::max(a.w, 0.35 * maxGap);
			const auto c = ci(a.mouth);
			double plus = 1e30, minus = 1e30;                                                   
			for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
			{
				auto it = g.find(gk(c[0] + dx, c[1] + dy, c[2] + dz));
				if (it == g.end()) continue;
				for (int j : it->second)
				{
					if ((size_t)j == i) continue;
					const Blob& b = blobs[(size_t)j];
					if (Dot(a.out, b.out) < 0.6) continue;
					const V dlt = b.mouth - a.mouth;
					if (std::abs(Dot(dlt, a.n)) > 1.5 * we || std::abs(Dot(dlt, a.out)) > 1.5 * we) continue;
					const double along = Dot(dlt, a.seam);
					if (along >= 0) plus = std::min(plus, Len(dlt)); else minus = std::min(minus, Len(dlt));
				}
			}
			double gapS = 0.0;
			for (double side : { plus, minus }) if (side < 10.0 * we) gapS = std::max(gapS, side);
			reachS[i] = 0.62 * gapS;
		}
	}
	std::vector<double> sorted(radii);
	std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
	const double Rmed = std::max(1e-9, sorted[sorted.size() / 2]);
	const double lumpAmp = f.lumpiness * 0.09 * Rmed;
	const double cauli = std::max(0.0, f.cauliflower);
	for (size_t i = 0; i < blobs.size(); ++i)
	{
		const Blob& b = blobs[i];
		const double we = std::max(b.w, 0.35 * maxGap);
		                                                                      
		                                           
		Rng rng(seed * 0x51ED27ULL + (uint64_t)(Hash3(b.mouth / (0.5 * we), seed ^ 0xB10B) * 9007199254740992.0) + 7);
		const double R = radii[i] * (0.85 + 0.3 * rng.U());
		const V n = b.n, o = b.out, s = b.seam;
		const double trimN = Noise01(b.mouth / (9.0 * we), nseed ^ 0x7E);
		const bool trimmed = f.trim > 0 && trimN < f.trim * 1.25 - 0.12;
		fb.push_back({ b.mouth, b.c, n, o, s, 0.5 * b.w, R, trimmed, 0.12 * R });
		                                                               
		const V gp = down - o * Dot(down, o);
		const double gl = Len(gp);
		const V gu = Unit(gp, V(0.0));
		const double hang = Dot(down, o);                                                     
		const double sag = std::max(0.0, f.sag);
		auto add = [&](Prim p) {
			V lo, hi;
			if (p.kind == 1)
			{
				const double rr = std::max(p.ra, p.rb);
				lo = V(std::min(p.a.x, p.b.x) - rr, std::min(p.a.y, p.b.y) - rr, std::min(p.a.z, p.b.z) - rr);
				hi = V(std::max(p.a.x, p.b.x) + rr, std::max(p.a.y, p.b.y) + rr, std::max(p.a.z, p.b.z) + rr);
			}
			else
			{
				const double rr = std::max(p.r0, std::max(p.r1, p.r2)) * std::max(1.0, p.gf);
				lo = p.c - V(rr); hi = p.c + V(rr);
			}
			p.lo = lo; p.hi = hi;
			prims.push_back(p);
		};
		                                                          
		{
			Prim p; p.kind = 0;
			p.c = b.c + o * (0.5 * b.depth);
			p.e0 = n; p.e1 = o; p.e2 = s;
			p.r0 = std::max(0.5 * b.w * 1.05, 0.25 * R); p.r1 = 0.5 * b.depth + 0.35 * R; p.r2 = std::max(R * 1.1, f.spacing * we);
			p.k = 0.3 * R;
			add(p);
		}
		                               
		const double rn = R * (0.85 + 0.45 * std::max(0.0, f.spread));
		const double ro = R * (0.8 + 0.15 * rng.U()) * (1.0 + 0.45 * sag * std::max(0.0, hang));
		const double rs = std::max(std::max(R * 1.15, f.spacing * we * 1.4), reachS[i]);
		const V capC = b.mouth + o * (0.12 * R) + gu * (0.22 * sag * gl * R);
		{
			Prim p; p.kind = 0;
			p.c = capC; p.e0 = n; p.e1 = o; p.e2 = s;
			p.r0 = rn; p.r1 = ro; p.r2 = rs;
			if (gl > 0.2 && sag > 0) { p.gd = gu; p.gf = 1.0 + 0.35 * sag * gl; }
			if (hang > 0.3 && sag > 0) { p.gd = down; p.gf = 1.0 + 0.3 * sag * hang; }
			p.k = 0.45 * R;
			add(p);
		}
		                                                    
		const int lobes = (int)std::floor(cauli * (2.0 + 3.0 * rng.U()) + rng.U());
		for (int l = 0; l < lobes; ++l)
		{
			double a1 = (2.0 * rng.U() - 1.0) * 1.25, a2 = (2.0 * rng.U() - 1.0) * 0.9;
			V dir = Unit(o * (0.55 + 0.6 * rng.U()) + n * a1 + s * a2 + gu * (0.35 * sag * gl), o);
			                                                                     
			const V dl(Dot(dir, n) / rn, Dot(dir, o) / ro, Dot(dir, s) / rs);
			const double t = 1.0 / std::max(1e-9, Len(dl));
			                                                          
			const bool glob = rng.U() < 0.12 * std::min(1.0, cauli);
			const double lr = R * (glob ? 0.7 + 0.35 * rng.U() : 0.25 + 0.38 * rng.U()) * std::min(1.5, 0.6 + 0.4 * cauli);
			const V lc = capC + dir * (t * (glob ? 0.55 + 0.15 * rng.U() : 0.74 + 0.16 * rng.U()));
			Prim p; p.kind = 0;
			p.c = lc; p.e0 = n; p.e1 = o; p.e2 = s;
			p.r0 = lr * (0.9 + 0.25 * rng.U()); p.r1 = lr * (0.8 + 0.2 * rng.U()); p.r2 = lr * (0.9 + 0.3 * rng.U());
			p.k = 0.35 * lr;
			add(p);
			const int sub = (int)std::floor(cauli * rng.U() * 2.2);
			for (int q = 0; q < sub; ++q)
			{
				const V d2 = Unit(dir * 0.8 + n * (2 * rng.U() - 1) + s * (2 * rng.U() - 1) + o * 0.3, dir);
				const double sr = lr * (0.35 + 0.25 * rng.U());
				Prim c; c.kind = 0;
				c.c = lc + d2 * (lr * (0.75 + 0.15 * rng.U()));
				c.e0 = n; c.e1 = o; c.e2 = s; c.r0 = sr; c.r1 = sr * (0.85 + 0.2 * rng.U()); c.r2 = sr;
				c.k = 0.4 * sr;
				add(c);
			}
		}
		                                                                    
		if (f.drips > 0 && !trimmed)
		{
			const double u = rng.U();
			const double L = std::max(0.0, f.dripLength) * R * (0.35 + 0.95 * rng.U());
			const double wob = 2.0 * rng.U() - 1.0;
			if (gl > 0.5 && std::abs(Dot(gu, s)) < 0.8 && u < f.drips * 0.55 * gl && L > 0.2 * R)
			{
				                                                                         
				const V side = Unit(m::la::cross(gu, o), s);
				const int steps = 3;
				V at = capC + gu * (0.35 * rn) + o * (-0.05 * R);
				for (int q = 1; q <= steps; ++q)
				{
					const double tq = q / (double)steps;
					const double rr = R * (0.78 - 0.16 * tq) * (0.85 + 0.3 * rng.U());
					at = at + gu * (L / steps) + side * ((rng.U() - 0.5 + 0.4 * wob) * 0.25 * L / steps);
					Prim t; t.kind = 0; t.c = at; t.e0 = gu; t.e1 = o; t.e2 = side;
					t.r0 = rr * (1.05 + 0.25 * rng.U()); t.r1 = rr * 0.6; t.r2 = rr * (0.95 + 0.25 * rng.U());
					t.k = 0.55 * rr;
					add(t);
				}
			}
			else if (hang > 0.55 && u < f.drips * 0.45 * hang && L > 0.2 * R)
			{
				                                         
				const V a = capC + o * (0.6 * ro);
				const V e = a + down * (0.7 * L) + n * (wob * 0.12 * L);
				Prim p; p.kind = 1; p.a = a; p.b = e; p.ra = 0.42 * R; p.rb = 0.22 * R; p.k = 0.3 * R;
				add(p);
				Prim t; t.kind = 0; t.c = e + down * (0.18 * R); t.e0 = n; t.e1 = down; t.e2 = s;
				t.r0 = 0.38 * R; t.r1 = 0.48 * R; t.r2 = 0.38 * R; t.k = 0.22 * R;
				add(t);
			}
		}
	}

	                                                                           
	double h = std::max(1e-6, std::max(0.02, f.resolution) * Rmed);
	const double embed0 = 0.18;                                           
	const int64_t budget = std::max<int64_t>(20000, f.maxCells);
	std::vector<Block> blocks;
	std::unordered_map<uint64_t, int> blockOf;
	double margin = 0.0;
	FieldCtx C{ &prims, &fb, lumpAmp, 1.0 / std::max(1e-9, 0.7 * Rmed), embed0 * Rmed, nseed };
	for (int attempt = 0; attempt < 6; ++attempt)
	{
		blocks.clear(); blockOf.clear();
		const double bs = B * h;
		double kmax = 0.0;
		for (const Prim& p : prims) kmax = std::max(kmax, p.k);
		margin = kmax + 2.0 * lumpAmp + 2.5 * h;
		C.cull = margin;
		                                                                                
		const int64_t candidateCap = 6 * budget / (B * B * B) + 1024;
		auto touch = [&](const V& lo, const V& hi, int idx, bool prim) -> bool {
			const int64_t x0 = (int64_t)std::floor((lo.x - margin) / bs), x1 = (int64_t)std::floor((hi.x + margin) / bs);
			const int64_t y0 = (int64_t)std::floor((lo.y - margin) / bs), y1 = (int64_t)std::floor((hi.y + margin) / bs);
			const int64_t z0 = (int64_t)std::floor((lo.z - margin) / bs), z1 = (int64_t)std::floor((hi.z + margin) / bs);
			for (int64_t x = x0; x <= x1; ++x) for (int64_t y = y0; y <= y1; ++y) for (int64_t z = z0; z <= z1; ++z)
			{
				const uint64_t key = BlockKey(x, y, z);
				auto it = blockOf.find(key);
				int bi;
				if (it == blockOf.end())
				{
					if (!prim) continue;                                                          
					if ((int64_t)blocks.size() + 1 > candidateCap) return false;
					bi = (int)blocks.size(); blockOf[key] = bi;
					blocks.push_back(Block()); blocks.back().bx = x; blocks.back().by = y; blocks.back().bz = z;
				}
				else bi = it->second;
				(prim ? blocks[(size_t)bi].prims : blocks[(size_t)bi].blobs).push_back(idx);
			}
			return true;
		};
		bool ok = true;
		for (size_t i = 0; i < prims.size() && ok; ++i) ok = touch(prims[i].lo, prims[i].hi, (int)i, true);
		if (ok)
			for (size_t i = 0; i < fb.size(); ++i)
				touch(fb[i].mouth - V(3.5 * fb[i].R), fb[i].mouth + V(3.5 * fb[i].R), (int)i, false);
		if (ok)
		{
			                                                                                     
			std::vector<char> keep(blocks.size(), 1);
			const double reachB = 1.6 * (0.5 * std::sqrt(3.0) * bs) + 3.0 * lumpAmp;
			ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t bi) {
				const Block& bk = blocks[(size_t)bi];
				const V c((bk.bx * B + 0.5 * B) * h, (bk.by * B + 0.5 * B) * h, (bk.bz * B + 0.5 * B) * h);
				const double d = Field(C, c, bk.prims, bk.blobs).d;
				keep[(size_t)bi] = std::abs(d) < reachB;
			});
			std::vector<Block> kept;
			kept.reserve(blocks.size());
			for (size_t i = 0; i < blocks.size(); ++i) if (keep[i]) kept.push_back(std::move(blocks[i]));
			blocks.swap(kept);
			blockOf.clear();
			for (size_t i = 0; i < blocks.size(); ++i) blockOf[BlockKey(blocks[i].bx, blocks[i].by, blocks[i].bz)] = (int)i;
			ok = (int64_t)blocks.size() * B * B * B <= budget;
		}
		if (ok) break;
		h *= 1.35;
		if (attempt == 5) return;
	}
	                
	ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t bi) {
		Block& bk = blocks[(size_t)bi];
		bk.val.resize((size_t)BC * BC * BC);
		                                                                                     
		constexpr int H = B / 2;
		double oct[2][2][2]; bool near[2][2][2];
		const double reachO = 1.6 * (0.5 * std::sqrt(3.0) * H * h) + 3.0 * lumpAmp;
		for (int a = 0; a < 2; ++a) for (int b2 = 0; b2 < 2; ++b2) for (int c = 0; c < 2; ++c)
		{
			const V x((bk.bx * B + H * a + 0.5 * H) * h, (bk.by * B + H * b2 + 0.5 * H) * h, (bk.bz * B + H * c + 0.5 * H) * h);
			oct[a][b2][c] = Field(C, x, bk.prims, bk.blobs).d;
			near[a][b2][c] = std::abs(oct[a][b2][c]) < reachO;
		}
		auto lo = [&](int i) { return i < H ? 0 : (i == H ? 0 : 1); };
		auto hi = [&](int i) { return i < H ? 0 : 1; };
		for (int i = 0; i < BC; ++i) for (int j = 0; j < BC; ++j) for (int k = 0; k < BC; ++k)
		{
			bool any = false;
			for (int a = lo(i); a <= hi(i) && !any; ++a) for (int b2 = lo(j); b2 <= hi(j) && !any; ++b2) for (int c = lo(k); c <= hi(k) && !any; ++c)
				any = near[a][b2][c];
			float v;
			if (any)
			{
				const V x((bk.bx * B + i) * h, (bk.by * B + j) * h, (bk.bz * B + k) * h);
				v = (float)Field(C, x, bk.prims, bk.blobs).d;
			}
			else v = (float)oct[lo(i)][lo(j)][lo(k)];
			bk.val[(size_t)(i * BC + j) * BC + k] = v;
		}
	});
	auto cornerVal = [&](const Block& bk, int i, int j, int k) { return bk.val[(size_t)(i * BC + j) * BC + k]; };
	                                                                          
	auto eval = [&](const Block& bk, const V& x) { return Field(C, x, bk.prims, bk.blobs); };
	ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t bi) {
		Block& bk = blocks[(size_t)bi];
		bk.vert.assign((size_t)B * B * B, -1);
		for (int i = 0; i < B; ++i) for (int j = 0; j < B; ++j) for (int k = 0; k < B; ++k)
		{
			float v[8]; int inside = 0;
			for (int c = 0; c < 8; ++c) { v[c] = cornerVal(bk, i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)); inside += v[c] < 0; }
			if (inside == 0 || inside == 8) continue;
			V sum(0.0); int cnt = 0;
			static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
			for (const auto& e : E)
			{
				const float a = v[e[0]], b2 = v[e[1]];
				if ((a < 0) == (b2 < 0)) continue;
				const double t = a / (double)(a - b2);
				const V pa((e[0] & 1), ((e[0] >> 1) & 1), ((e[0] >> 2) & 1)), pb((e[1] & 1), ((e[1] >> 1) & 1), ((e[1] >> 2) & 1));
				sum += pa + (pb - pa) * t; ++cnt;
			}
			const V cellLo((bk.bx * B + i) * h, (bk.by * B + j) * h, (bk.bz * B + k) * h);
			V x = cellLo + (sum / (double)cnt) * h;
			                                                                            
			const double e = 0.25 * h;
			const double d0 = eval(bk, x).d;
			const V g((eval(bk, x + V(e, 0, 0)).d - d0) / e, (eval(bk, x + V(0, e, 0)).d - d0) / e, (eval(bk, x + V(0, 0, e)).d - d0) / e);
			const double g2 = Dot(g, g);
			if (g2 > 1e-12)
			{
				V step = g * (d0 / g2);
				const double sl = Len(step);
				if (sl > 0.5 * h) step = step * (0.5 * h / sl);
				x = x - step;
			}
			bk.nrm.push_back(Unit(g, V(0, 0, 1)));
			const V cc = cellLo + V(0.5 * h);
			const V off = x - cc;
			x = cc + V(std::clamp(off.x, -1.0 * h, 1.0 * h), std::clamp(off.y, -1.0 * h, 1.0 * h), std::clamp(off.z, -1.0 * h, 1.0 * h));
			bk.vert[(size_t)(i * B + j) * B + k] = (int32_t)bk.pts.size();
			bk.pts.push_back(x);
		}
	});
	int32_t total = 0;
	for (Block& bk : blocks) { bk.first = total; total += (int32_t)bk.pts.size(); }
	if (total <= 0) return;
	                                     
	auto cellVert = [&](int64_t gi, int64_t gj, int64_t gk) -> int32_t {
		const int64_t bx = (int64_t)std::floor((double)gi / B), by = (int64_t)std::floor((double)gj / B), bz = (int64_t)std::floor((double)gk / B);
		auto it = blockOf.find(BlockKey(bx, by, bz));
		if (it == blockOf.end()) return -1;
		const Block& bk = blocks[(size_t)it->second];
		const int32_t lv = bk.vert[(size_t)((gi - bx * B) * B + (gj - by * B)) * B + (gk - bz * B)];
		return lv < 0 ? -1 : bk.first + lv;
	};
	                                                                                       
	ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t bi) {
		Block& bk = blocks[(size_t)bi];
		for (int i = 0; i < B; ++i) for (int j = 0; j < B; ++j) for (int k = 0; k < B; ++k)
		{
			const float v0 = cornerVal(bk, i, j, k);
			const int64_t gi = bk.bx * B + i, gj = bk.by * B + j, gk = bk.bz * B + k;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float v1 = axis == 0 ? cornerVal(bk, i + 1, j, k) : axis == 1 ? cornerVal(bk, i, j + 1, k) : cornerVal(bk, i, j, k + 1);
				if ((v0 < 0) == (v1 < 0)) continue;
				int32_t q[4];
				if (axis == 0) { q[0] = cellVert(gi, gj - 1, gk - 1); q[1] = cellVert(gi, gj, gk - 1); q[2] = cellVert(gi, gj, gk); q[3] = cellVert(gi, gj - 1, gk); }
				else if (axis == 1) { q[0] = cellVert(gi - 1, gj, gk - 1); q[1] = cellVert(gi - 1, gj, gk); q[2] = cellVert(gi, gj, gk); q[3] = cellVert(gi, gj, gk - 1); }
				else { q[0] = cellVert(gi - 1, gj - 1, gk); q[1] = cellVert(gi, gj - 1, gk); q[2] = cellVert(gi, gj, gk); q[3] = cellVert(gi - 1, gj, gk); }
				if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
				                                                 
				if (v0 < 0) bk.quads.push_back({ q[0], q[1], q[2], q[3] });
				else bk.quads.push_back({ q[3], q[2], q[1], q[0] });
			}
		}
	});
	r.points.resize((size_t)total);
	r.uvs.assign((size_t)total * 2, 0.0);
	r.variation.assign((size_t)total, 0.0f);
	r.cut.assign((size_t)total, 0.0f);
	r.cavity.assign((size_t)total, 0.0f);
	const double uvScale = 1.0 / std::max(1e-9, 4.0 * Rmed);
	const uint64_t vseed = nseed ^ 0xC0FFEEULL;
	ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t bi) {
		const Block& bk = blocks[(size_t)bi];
		for (size_t lv = 0; lv < bk.pts.size(); ++lv)
		{
			const size_t gv = (size_t)bk.first + lv;
			const V x = bk.pts[lv];
			r.points[gv] = x;
			const V nrm = bk.nrm[lv];
			                                             
			const V an(std::abs(nrm.x), std::abs(nrm.y), std::abs(nrm.z));
			double u, v;
			if (an.x >= an.y && an.x >= an.z) { u = x.y; v = x.z; }
			else if (an.y >= an.z) { u = x.x; v = x.z; }
			else { u = x.x; v = x.y; }
			r.uvs[gv * 2] = u * uvScale; r.uvs[gv * 2 + 1] = v * uvScale;
			r.variation[gv] = (float)Noise01(x / (2.5 * Rmed), vseed);
			const FieldOut here = eval(bk, x);
			r.cut[gv] = here.cut ? 1.0f : 0.0f;
			                                                                       
			double occ = 0.0, wsum = 0.0;
			for (int sI = 1; sI <= 2; ++sI)
			{
				const double dlt = sI * 0.3 * Rmed;
				const FieldOut fo = eval(bk, x + nrm * dlt);
				const double sd = std::min(fo.d, fo.wall + C.embed);
				const double wgt = 1.0 / sI;
				occ += wgt * Clamp01((dlt - sd) / dlt); wsum += wgt;
			}
			r.cavity[gv] = (float)Smooth01(1.15 * occ / wsum);
		}
	});
	for (const Block& bk : blocks) r.polys.insert(r.polys.end(), bk.quads.begin(), bk.quads.end());
	r.blobs = (int)blobs.size();
}

}               

bool FoamAlongPath(const std::vector<V>& centres, const std::vector<V>& normals,
	const std::vector<V>& tangents, const FoamParams& f, FoamResult& r)
{
	r = FoamResult();
	const size_t n = std::min(centres.size(), std::min(normals.size(), tangents.size()));
	const double w = std::max(1e-6, f.maxGap);
	const V down = Unit(V(-f.upX, -f.upY, -f.upZ), V(0, 0, -1));
	const uint64_t cseed = 0xC0BEULL ^ (uint64_t)(uint32_t)f.seed;
	std::vector<Blob> blobs;
	for (size_t i = 0; i < n && (int)blobs.size() < f.maxBlobs; ++i)
	{
		const V out = Unit(normals[i], V(0, 0, 1));
		V seam = tangents[i] - out * Dot(tangents[i], out);
		seam = Unit(seam, Unit(m::la::cross(out, std::abs(out.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0)));
		const V nn = Unit(m::la::cross(seam, out), V(1, 0, 0));
		const V mouth = centres[i];
		if (f.coverage < 1.0 && Noise01(mouth / (6.0 * w), cseed) > f.coverage * 1.2 - 0.1) continue;
		                                                                   
		const double depth = 0.08 * w;
		blobs.push_back({ mouth - out * depth, nn, out, seam, 0.0, 0.5, mouth, depth });                                
	}
	if (blobs.empty()) return false;
	if (f.style != 0) FoamV2(blobs, f, down, w, r);
	else
	{
		for (Blob& b : blobs) b.w = 0.45 * w;                                
		Rng rng((uint64_t)(uint32_t)f.seed + 101);
		BeadsV1(blobs, f, down, rng, r);
	}
	return r.blobs > 0 && !r.polys.empty();
}

// основной вход: собираем геометрию по переданным настройкам
bool SprayFoam(const MeshView& mv, const FoamParams& f, FoamResult& r)
{
	r = FoamResult();
	if (mv.npolys <= 0) return false;
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	std::vector<V> fn((size_t)mv.npolys);
	struct Tri { V a, b, c, n; double area; };
	std::vector<Tri> tris;
	double area = 0.0;
	std::vector<double> cdf;
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		V s(0.0);
		for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k) s += m::la::cross(V(mv.pts[ids[k]]) - V(mv.pts[ids[0]]), V(mv.pts[ids[k + 1]]) - V(mv.pts[ids[0]]));
		fn[(size_t)i] = GuidedNormal(mv, i, Unit(s, V(0, 0, 1)));
		for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k)
		{
			const V A = mv.pts[ids[0]], B = mv.pts[ids[k]], C = mv.pts[ids[k + 1]];
			const double a = 0.5 * Len(m::la::cross(B - A, C - A));
			tris.push_back({ A, B, C, fn[(size_t)i], a });
			area += a; cdf.push_back(area);
		}
	}
	if (!(area > 0.0)) return false;
	const V down = Unit(V(-f.upX, -f.upY, -f.upZ), V(0, 0, -1));
	                                      
	std::vector<V> dirs;
	for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z)
		if (x || y || z) dirs.push_back(Unit(V(x, y, z), V(0, 0, 1)));
	Rng rng((uint64_t)(uint32_t)f.seed + 101);
	const bool v2 = f.style != 0;
	const uint64_t cseed = 0xC0BEULL ^ (uint64_t)(uint32_t)f.seed;
	std::vector<Blob> blobs;
	                
	std::unordered_map<uint64_t, std::vector<int>> grid;
	const double cell = std::max(1e-9, f.maxGap * std::max(0.2, f.spacing));
	auto key = [&](const V& p) {
		return ((uint64_t)(int64_t)std::floor(p.x / cell) * 73856093ULL) ^ ((uint64_t)(int64_t)std::floor(p.y / cell) * 19349663ULL) ^ ((uint64_t)(int64_t)std::floor(p.z / cell) * 83492791ULL);
	};
	const size_t candidates = (size_t)std::min(4.0e6, area * f.density);
	for (size_t c = 0; c < candidates && (int)blobs.size() < f.maxBlobs; ++c)
	{
		const size_t ti = (size_t)(std::upper_bound(cdf.begin(), cdf.end(), rng.U() * area) - cdf.begin());
		if (ti >= tris.size()) continue;
		double u = rng.U(), v = rng.U();
		if (u + v > 1) { u = 1 - u; v = 1 - v; }
		const Tri& t = tris[ti];
		const V p = t.a + (t.b - t.a) * u + (t.c - t.a) * v;
		const V n = t.n;
		                                                                        
		double d = 0.0; int32_t hp = -1;
		if (!bvh.Raycast(p + n * (1e-5 * f.maxGap), n, f.maxGap, &d, &hp) || hp < 0) continue;
		if (Dot(fn[(size_t)hp], n) > -0.5) continue;
		if (d < f.minGap) continue;
		const V mid = p + n * (0.5 * d);
		                                                                  
		const double minD = std::max(1e-9, f.spacing * std::max(d, 0.3 * f.maxGap));
		auto crowded = [&](const V& at, bool mouth) {
			for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
			{
				auto it = grid.find(key(at + V(dx * cell, dy * cell, dz * cell)));
				if (it == grid.end()) continue;
				for (int bi : it->second)
					if (Len((mouth ? blobs[(size_t)bi].mouth : blobs[(size_t)bi].c) - at) < minD) return true;
			}
			return false;
		};
		if (!v2 && crowded(mid, false)) continue;
		                           
		                                                                       
		                                                               
		const double reach = std::max(8.0 * d, 2.0 * f.maxGap);
		const V t1 = Unit(m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
		const V t2 = m::la::cross(n, t1);
		V outSum(0.0); int escaped = 0, tilted = 0;
		for (const V& dl : dirs)
		{
			if (std::abs(dl.z) < 1e-9) continue;                                        
			const V dd = Unit(t1 * dl.x + t2 * dl.y + n * dl.z, n);
			++tilted;
			double hd = 0.0;
			if (!bvh.Raycast(mid, dd, reach, &hd)) { outSum += dd; ++escaped; }
		}
		const double open = escaped / (double)std::max(1, tilted);
		if (open < f.minOpenness) continue;                                    
		V out = Unit(outSum - n * Dot(outSum, n), V(0.0));
		if (Len(out) < 1e-9) out = Unit(m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
		const V seam = Unit(m::la::cross(n, out), V(0, 0, 1));
		Blob b{ mid, n, out, seam, d, open, mid, 0.0 };
		if (v2)
		{
			                                                             
			const double we = std::max(d, 0.35 * f.maxGap);
			const double side = 0.5 * d + 0.75 * we;
			double ahead = 0.0;
			const double maxWalk = 10.0 * we;
			const double limit = bvh.Raycast(mid, out, maxWalk, &ahead) ? ahead : maxWalk;
			auto walled = [&](double s2) {
				const V q = mid + out * s2;
				double hd = 0.0;
				return bvh.Raycast(q, n, side, &hd) || bvh.Raycast(q, n * -1.0, side, &hd);
			};
			const double step = 0.35 * we;
			double s2 = 0.0;
			while (s2 < limit && walled(s2)) s2 += step;
			if (s2 > 0.0)
			{
				double lo = std::max(0.0, s2 - step), hi = std::min(s2, limit);
				for (int it = 0; it < 4; ++it) { const double md = 0.5 * (lo + hi); (walled(md) ? lo : hi) = md; }
				s2 = hi;
			}
			b.depth = std::min(s2, limit);
			b.mouth = mid + out * b.depth;
			if (crowded(b.mouth, true)) continue;
			                     
			if (f.coverage < 1.0 && Noise01(b.mouth / (6.0 * we), cseed) > f.coverage * 1.2 - 0.1) continue;
		}
		blobs.push_back(b);
		grid[key(v2 ? b.mouth : mid)].push_back((int)blobs.size() - 1);
	}
	if (v2) FoamV2(blobs, f, down, f.maxGap, r);
	else BeadsV1(blobs, f, down, rng, r);
	return r.blobs > 0 && !r.polys.empty();
}

PROJECT_DAMAGE_NS_MACRO_END
