// добавляем мелкие поры, вмятины и отслоения бетона
// ProjectDamage concrete pitting — GPL-3.0-or-later; see LICENSE.
//
// Recipe: concrete-pits-v1. The surface of cast concrete: air voids (bug
// holes), shallow chipped dents and patches where the cement skin has flaked
// off down to the sand and aggregate, over a fine sand grain. Displaced into
// the base mesh itself, like Plaster Displacement:
//   1. refine: the triangles under a painted weight (or all of them) are split
//      down to edgeLength / detail, graded toward the unpainted rest
//      (RefineNearDamage / RefineToLength: run / faceID kept, UVs
//      interpolated, exact surface); then one more pass to a size field:
//      about a third of the radius inside each pore (graded out around it)
//      and aggregateSize / 3 inside the flaked patches, so the fine
//      triangles go only where they are seen;
//   2. height field H(p), a pure function of the object-space position:
//        - pores: elliptic, lobed pits with sheer walls, a rounded floor and a
//          crumbled rim; scattered by area over the surface, gathered into
//          bands and patches by a low noise, many small and few large;
//        - dents: wide, shallow dishes, broken off steeply on one side;
//        - flaking: a domain-warped noise above a threshold (set from the
//          coverage, via the noise's own measured distribution) takes the
//          skin off in 1-4 terraces with ragged edges; the floor is rough
//          sand with rounded aggregate grains, and pores gather there;
//        - surface: fine sand grain and a broad, low waviness;
//   3. displacement: every vertex moves along its angle-weighted smooth normal
//      by H x zone weight, capped by the local wall thickness.
// Nothing tiles: every noise here hashes its 64-bit lattice coordinates
// (no 256-entry permutation table, so no period), and the pores and dents are
// individually seeded points. ConcretePitsHeightMap evaluates the very same
// field on a flat tile with every lattice wrapped to the tile, which gives a
// seamless map for texturing.
//
// Channel map (pore key = i, dent key = 2^32 + i, merged void j of a pore =
// key ^ (j + 1) << 48): 0 triangle, 1-2 barycentric, 3 accept, 4 size,
// 5 stretch, 6 angle, 7 depth, 11 rim, 12 merged?, 13 merged count, 14-16
// merged offset / size. Global noise offsets use key 2^40.
#include "manifold/manifold.h"
#include "concrete_pits.h"
#include "adaptive_refine.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
constexpr double kPi = 3.14159265358979323846;

uint64_t Mix(uint64_t z)
{
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

double Random01(uint64_t seed, uint64_t candidate, uint64_t channel)
{
	const uint64_t z = Mix(seed ^ (candidate * 0x9e3779b97f4a7c15ULL) ^ ((channel + 1) * 0xd1b54a32d192ed03ULL));
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}

double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double e0, double e1, double x)
{
	if (e1 == e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}
V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

                                                                                
                                                                               
                                                                             
                                         
struct Wrap { int64_t x = 0, y = 0; };

int64_t WrapI(int64_t i, int64_t per) { if (per <= 0) return i; i %= per; return i < 0 ? i + per : i; }

uint64_t LatticeHash(int64_t x, int64_t y, int64_t z, uint64_t seed)
{
	return Mix(seed ^ Mix((uint64_t)x * 0x8cb92ba72f3d8dd7ULL ^ Mix((uint64_t)y * 0xd6e8feb86659fd93ULL ^ Mix((uint64_t)z + 0x2545f4914f6cdd1dULL))));
}

double GradDot(uint64_t h, double x, double y, double z)
{
	static const double g[16][3] = {
		{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
		{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},{1,1,0},{-1,1,0},{0,-1,1},{0,-1,-1} };
	const double* v = g[h & 15];
	return v[0] * x + v[1] * y + v[2] * z;
}

double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

           
double GNoise(const V& q, uint64_t seed, const Wrap& w)
{
	const double xf = std::floor(q.x), yf = std::floor(q.y), zf = std::floor(q.z);
	const int64_t X = (int64_t)xf, Y = (int64_t)yf, Z = (int64_t)zf;
	const double x = q.x - xf, y = q.y - yf, z = q.z - zf;
	const double u = Fade(x), v = Fade(y), t = Fade(z);
	double c[8];
	for (int k = 0; k < 8; k++)
	{
		const int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
		const uint64_t h = LatticeHash(WrapI(X + dx, w.x), WrapI(Y + dy, w.y), Z + dz, seed);
		c[k] = GradDot(h >> 7, x - dx, y - dy, z - dz);
	}
	const double x00 = Lerp(c[0], c[1], u), x10 = Lerp(c[2], c[3], u);
	const double x01 = Lerp(c[4], c[5], u), x11 = Lerp(c[6], c[7], u);
	return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), t);
}

                                                                       
double Fbm(const V& q, int octaves, uint64_t seed, Wrap w)
{
	double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
	for (int o = 0; o < octaves; ++o)
	{
		sum += amp * GNoise(q * f + V(17.13 * o, -9.31 * o, 5.77 * o), seed + (uint64_t)o * 0x51ED27ULL, w);
		norm += amp;
		amp *= 0.5;
		f *= 2.0;
		w.x *= 2; w.y *= 2;
	}
	return sum / norm;
}

                                                                             
                                                     
struct Freq { double f = 1.0; Wrap w; };
Freq MakeFreq(double size, double tile)
{
	Freq r;
	r.f = 1.0 / std::max(size, 1e-4);
	if (tile > 0.0)
	{
		const int64_t n = std::max<int64_t>(1, (int64_t)std::llround(tile * r.f));
		r.f = (double)n / tile;
		r.w.x = r.w.y = n;
	}
	return r;
}

uint64_t CellKey(int64_t x, int64_t y, int64_t z)
{
	return (uint64_t)(x & 0x1FFFFF) | ((uint64_t)(y & 0x1FFFFF) << 21) | ((uint64_t)(z & 0x1FFFFF) << 42);
}

                                                                               
struct Pit
{
	V c, n, du, dv;
	double a = 1.0, b = 1.0;                             
	double depth = 0.0;
	double rim = 0.0;                                    
	double reach = 1.0;
	uint64_t nseed = 0;
	bool dent = false;
};

struct Field
{
	const ConcretePitsParams* p = nullptr;
	double tile = 0.0;                                  
	double edge = 0.0;                                                            
	double edgeFlake = 0.0;                                      
	uint64_t seed = 0;
	Freq fUnd, fGrain, fFlake, fFlakeJag, fAgg, fCluster;
	V offUnd, offGrain, offFlake, offWarp, offJag, offAgg, offCluster;
	std::vector<double> flakeSorted;                                              

	std::vector<Pit> pits;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	double cell = 1.0;

	double FlakeNoise(const V& pos) const
	{
		V q = pos * fFlake.f + offFlake;
		                                                                        
		const V qw = q;
		q += V(Fbm(qw + offWarp, 2, seed + 11, fFlake.w), Fbm(qw + offWarp * 1.7, 2, seed + 12, fFlake.w), 0.0) * 0.45;
		double v = Fbm(q, 4, seed + 13, fFlake.w);
		const double jag = ClampD(p->flakeJagged, 0.0, 2.0);
		if (jag > 0.0)
			v += jag * 0.07 * Fbm(pos * fFlakeJag.f + offJag, 3, seed + 14, fFlakeJag.w);
		return v;
	}

	                                                       
	double FlakeThreshold(double c) const
	{
		if (flakeSorted.empty() || c <= 0.0) return 1e9;
		const double k = ClampD(1.0 - c, 0.0, 1.0) * (double)(flakeSorted.size() - 1);
		const size_t i = (size_t)k;
		const double t = k - (double)i;
		return i + 1 < flakeSorted.size() ? Lerp(flakeSorted[i], flakeSorted[i + 1], t) : flakeSorted.back();
	}

	                                                       
	double FlakeMask(const V& pos, double w) const
	{
		if (!p->flakeEnable || p->flakeCoverage <= 0.0 || w <= 0.0) return 0.0;
		const double thr = FlakeThreshold(p->flakeCoverage * w);
		const double v = FlakeNoise(pos);
		return Smooth(thr, thr + 0.012, v);
	}

	double Cluster(const V& pos) const
	{
		const double c = ClampD(p->clustering, 0.0, 1.0);
		if (c <= 0.0) return 1.0;
		const double n = Fbm(pos * fCluster.f + offCluster, 3, seed + 21, fCluster.w);
		return ClampD(1.0 + c * 3.0 * n, 0.0, 1.0 + 2.0 * c);
	}

	                                                                  
	double Aggregate(const V& pos) const
	{
		const V q = pos * fAgg.f + offAgg;
		const int64_t cx = (int64_t)std::floor(q.x), cy = (int64_t)std::floor(q.y), cz = (int64_t)std::floor(q.z);
		double d1 = 1e30, d2 = 1e30;
		for (int64_t dz = -1; dz <= 1; dz++)
			for (int64_t dy = -1; dy <= 1; dy++)
				for (int64_t dx = -1; dx <= 1; dx++)
				{
					const int64_t x = cx + dx, y = cy + dy, z = cz + dz;
					const uint64_t h = LatticeHash(WrapI(x, fAgg.w.x), WrapI(y, fAgg.w.y), z, seed + 31);
					const V f = V((double)x + 0.15 + 0.7 * double(h & 0xFFFF) / 65535.0,
						(double)y + 0.15 + 0.7 * double((h >> 16) & 0xFFFF) / 65535.0,
						(double)z + 0.15 + 0.7 * double((h >> 32) & 0xFFFF) / 65535.0);
					const V d = q - f;
					const double dd = m::la::dot(d, d);
					if (dd < d1) { d2 = d1; d1 = dd; }
					else if (dd < d2) d2 = dd;
				}
		                                                              
		const double edgeDist = std::sqrt(d2) - std::sqrt(d1);
		return Smooth(0.0, 0.35, edgeDist) * (1.0 - 0.5 * std::sqrt(d1));
	}

	double FlakeLayer(const V& pos, double w) const
	{
		if (!p->flakeEnable || p->flakeCoverage <= 0.0 || p->flakeDepth <= 0.0 || w <= 0.0) return 0.0;
		const double thr = FlakeThreshold(p->flakeCoverage * w);
		const double v = FlakeNoise(pos);
		if (v < thr - 0.01) return 0.0;
		const int L = std::max(1, std::min(p->flakeLayers, 4));
		                                                                    
		const double spread = 0.06 + 0.03 * (4 - L);
		double steps = 0.0;
		for (int i = 0; i < L; i++)
		{
			const double t = thr + spread * i;
			steps += Smooth(t, t + 0.012, v);
		}
		const double in = Smooth(thr, thr + 0.012, v);
		double h = -p->flakeDepth * steps / L;
		if (p->flakeRough > 0.0)
		{
			const double fade = edgeFlake > 0.0 ? Smooth(1.0 * edgeFlake, 2.5 * edgeFlake, p->aggregateSize) : 1.0;
			const double agg = fade > 0.0 ? Aggregate(pos) : 0.5;
			const double sand = Fbm(pos * (fAgg.f * 3.0) + offAgg * 1.3, 2, seed + 32, Wrap{ fAgg.w.x * 3, fAgg.w.y * 3 });
			                                                                   
			const double crumb = 1.0 - std::abs(GNoise(pos * (fAgg.f * 2.0) + offAgg * 0.7, seed + 33, Wrap{ fAgg.w.x * 2, fAgg.w.y * 2 }));
			h += in * p->flakeDepth * p->flakeRough * (fade * (0.35 * (agg - 0.45) + 0.45 * (crumb - 0.6)) + 0.3 * sand);
		}
		return h;
	}

	double PitHeight(const Pit& s, const V& pos, const V& nrm) const
	{
		const V d = pos - s.c;
		if (m::la::dot(d, d) > s.reach * s.reach) return 0.0;
		if (m::la::dot(nrm, s.n) < 0.5) return 0.0;                                
		const double wn = m::la::dot(d, s.n);
		if (std::abs(wn) > 0.5 * s.reach + 2.0 * s.depth) return 0.0;
		const double u = m::la::dot(d, s.du), v = m::la::dot(d, s.dv);
		const double x = u / s.a, y = v / s.b;
		double rho = std::sqrt(x * x + y * y);
		const double r = std::max(s.a, s.b);
		                                                              
		const Wrap none;
		if (s.dent)
		{
			const double jag = 0.18 * GNoise(V(x * 1.4, y * 1.4, 0.5), s.nseed, none);
			rho /= 1.0 + jag;
			if (rho >= 1.0) return 0.0;
			const double chip = ClampD(p->dentChip, 0.0, 1.0);
			const double k = 2.0 + 7.0 * chip * Smooth(-0.3, 0.9, x);
			const double prof = 1.0 - std::pow(rho, k);
			const double side = Lerp(1.0, 0.3 + 0.7 * Smooth(-1.0, 0.7, x), chip);
			const double facet = 1.0 + 0.2 * GNoise(V(x * 2.2, y * 2.2, 3.1), s.nseed + 1, none);
			return -s.depth * prof * side * facet;
		}
		const double jagK = ClampD(p->poreJagged, 0.0, 1.5);
		const double lobe = jagK * (0.28 * GNoise(V(x * 1.2, y * 1.2, 0.5), s.nseed, none)
			+ 0.1 * GNoise(V(x * 3.1, y * 3.1, 1.5), s.nseed + 1, none));
		rho /= std::max(0.35, 1.0 + lobe);
		if (rho < 1.0)
		{
			const double k = 2.0 + 10.0 * ClampD(p->poreSharp, 0.0, 1.0);
			const double prof = std::pow(std::max(0.0, 1.0 - std::pow(rho, k)), 0.55);
			                                            
			const double floorN = 1.0 + 0.22 * GNoise(V(u, v, 7.0) * (2.0 / r), s.nseed + 2, none);
			return -s.depth * prof * floorN;
		}
		if (s.rim > 0.0 && rho < 1.0 + s.rim)
		{
			const double t = (rho - 1.0) / s.rim;
			const double chips = 0.25 + 0.75 * Smooth(-0.3, 0.5, GNoise(V(x * 2.5, y * 2.5, 4.0), s.nseed + 3, none));
			return -s.depth * 0.22 * (1.0 - t) * (1.0 - t) * chips;
		}
		return 0.0;
	}

	double PitLayer(const V& pos, const V& nrm) const
	{
		if (pits.empty()) return 0.0;
		const auto it = grid.find(CellKey((int64_t)std::floor(pos.x / cell),
			(int64_t)std::floor(pos.y / cell), (int64_t)std::floor(pos.z / cell)));
		if (it == grid.end()) return 0.0;
		double h = 0.0;
		for (const int32_t si : it->second)
			h = std::min(h, PitHeight(pits[(size_t)si], pos, nrm));
		return h;
	}

	                                                                        
	                                                            
	double Height(const V& pos, const V& nrm, double w) const
	{
		const ConcretePitsParams& P = *p;
		double h = 0.0;
		if (P.undulation > 0.0)
			h += P.undulation * w * Fbm(pos * fUnd.f + offUnd, 3, seed + 41, fUnd.w);
		if (P.grain > 0.0)
		{
			const double fade = edge > 0.0 ? Smooth(1.5 * edge, 3.0 * edge, P.grainSize) : 1.0;
			if (fade > 0.0)
				h += P.grain * w * fade * Fbm(pos * fGrain.f + offGrain, 2, seed + 42, fGrain.w);
		}
		h += FlakeLayer(pos, w);
		h += PitLayer(pos, nrm);
		return h * std::max(P.strength, 0.0);
	}

	void BuildGrid()
	{
		double maxReach = 1e-3;
		for (const Pit& s : pits) maxReach = std::max(maxReach, s.reach);
		cell = maxReach;
		grid.clear();
		for (int32_t si = 0; si < (int32_t)pits.size(); si++)
		{
			const Pit& s = pits[(size_t)si];
			const int64_t x0 = (int64_t)std::floor((s.c.x - s.reach) / cell), x1 = (int64_t)std::floor((s.c.x + s.reach) / cell);
			const int64_t y0 = (int64_t)std::floor((s.c.y - s.reach) / cell), y1 = (int64_t)std::floor((s.c.y + s.reach) / cell);
			const int64_t z0 = (int64_t)std::floor((s.c.z - s.reach) / cell), z1 = (int64_t)std::floor((s.c.z + s.reach) / cell);
			for (int64_t z = z0; z <= z1; z++)
				for (int64_t y = y0; y <= y1; y++)
					for (int64_t x = x0; x <= x1; x++)
						grid[CellKey(x, y, z)].push_back(si);
		}
	}
};

uint64_t SeedOf(Int32 mainSeed, int32_t own)
{
	return 0x434f4e435049545fULL                
		^ ((uint64_t)(uint32_t)mainSeed * 0x9e3779b97f4a7c15ULL)
		^ ((uint64_t)(uint32_t)own * 0xc2b2ae3d27d4eb4fULL);
}

V Offset(uint64_t seed, uint64_t ch)
{
	const uint64_t gk = 1ULL << 40;
	return V(Random01(seed, gk, ch) * 512.0, Random01(seed, gk, ch + 1) * 512.0, Random01(seed, gk, ch + 2) * 512.0);
}

void InitField(Field& F, const ConcretePitsParams& p, uint64_t seed, double tile, double edge)
{
	F.p = &p;
	F.tile = tile;
	F.edge = edge;
	F.seed = seed;
	F.fUnd = MakeFreq(p.undulationScale, tile);
	F.fGrain = MakeFreq(p.grainSize, tile);
	F.fFlake = MakeFreq(p.flakeScale, tile);
	F.fFlakeJag = MakeFreq(p.flakeScale * 0.08, tile);
	F.fAgg = MakeFreq(std::max(p.aggregateSize, 1e-3), tile);
	F.fCluster = MakeFreq(p.clusterScale, tile);
	F.offUnd = Offset(seed, 0); F.offGrain = Offset(seed, 3); F.offFlake = Offset(seed, 6);
	F.offWarp = Offset(seed, 9); F.offJag = Offset(seed, 12); F.offAgg = Offset(seed, 15);
	F.offCluster = Offset(seed, 18);
	if (tile > 0.0)
	{
		                                                                   
		                                                                         
	}
	if (p.flakeEnable && p.flakeCoverage > 0.0)
	{
		                                                                     
		                                                                 
		const int N = 2048;
		F.flakeSorted.resize(N);
		const double span = std::max(p.flakeScale, 1e-3) * 40.0;
		for (int i = 0; i < N; i++)
		{
			const V q(Random01(seed, 7777, (uint64_t)i * 3) * span, Random01(seed, 7777, (uint64_t)i * 3 + 1) * span,
				tile > 0.0 ? 0.0 : Random01(seed, 7777, (uint64_t)i * 3 + 2) * span);
			F.flakeSorted[(size_t)i] = F.FlakeNoise(q);
		}
		std::sort(F.flakeSorted.begin(), F.flakeSorted.end());
	}
}

                                                                            
                        
bool MakePit(const ConcretePitsParams& p, uint64_t seed, uint64_t key, bool dent, const V& c, const V& n,
	double w, double minRadius, Pit& s)
{
	const double dMin = std::max(std::min(dent ? p.dentSizeMin : p.poreSizeMin, dent ? p.dentSizeMax : p.poreSizeMax), 1e-4);
	const double dMax = std::max(dent ? p.dentSizeMin : p.poreSizeMin, dent ? p.dentSizeMax : p.poreSizeMax);
	const double bias = dent ? 0.4 : ClampD(p.poreSmallBias, 0.0, 1.0);
	const double t = std::pow(Random01(seed, key, 4), 1.0 + 4.0 * bias);
	const double diam = dMin * std::pow(dMax / dMin, t);
	const double r = 0.5 * diam;
	if (r < minRadius) return false;
	const double st = 1.0 + 1.5 * ClampD(dent ? 0.3 : p.poreStretch, 0.0, 1.0) * Random01(seed, key, 5);
	s.dent = dent;
	s.c = c;
	s.n = n;
	const V h = AnyPerp(n);
	const double ang = Random01(seed, key, 6) * 2.0 * kPi;
	s.du = Unit(h * std::cos(ang) + m::la::cross(n, h) * std::sin(ang), h);
	s.dv = m::la::cross(n, s.du);
	s.a = r * std::sqrt(st);
	s.b = r / std::sqrt(st);
	const double depthK = dent ? p.dentDepth : p.poreDepth;
	s.depth = std::max(depthK, 0.0) * diam * (0.55 + 0.9 * Random01(seed, key, 7)) * (0.45 + 0.55 * ClampD(w, 0.0, 1.0));
	s.rim = dent ? 0.0 : ClampD(p.poreRim, 0.0, 2.0) * (0.4 + 0.6 * Random01(seed, key, 11));
	const double lobe = dent ? 0.2 : 0.4 * ClampD(p.poreJagged, 0.0, 1.5);
	s.reach = std::max(s.a, s.b) * (1.0 + lobe) * (1.0 + s.rim) + 1e-4;
	s.nseed = Mix(seed ^ (key * 0x9e3779b97f4a7c15ULL) ^ 0x50495453ULL);
	return true;
}

                                                                            
double PitEdge(const Pit& q, double fineEdge)
{
	return std::max(fineEdge, (q.dent ? 0.25 : 0.3) * std::min(q.a, q.b));
}

                                                                       
struct PitGrid
{
	const std::vector<Pit>* pits = nullptr;
	std::unordered_map<uint64_t, std::vector<int32_t>> map;
	double cell = 1.0;
	template <class R> void Build(const std::vector<Pit>& all, R reachOf)
	{
		pits = &all;
		double mx = 1e-3;
		for (const Pit& q : all) mx = std::max(mx, reachOf(q));
		cell = mx;
		for (int32_t i = 0; i < (int32_t)all.size(); i++)
		{
			const Pit& q = all[(size_t)i];
			const double r = reachOf(q);
			const int64_t x0 = (int64_t)std::floor((q.c.x - r) / cell), x1 = (int64_t)std::floor((q.c.x + r) / cell);
			const int64_t y0 = (int64_t)std::floor((q.c.y - r) / cell), y1 = (int64_t)std::floor((q.c.y + r) / cell);
			const int64_t z0 = (int64_t)std::floor((q.c.z - r) / cell), z1 = (int64_t)std::floor((q.c.z + r) / cell);
			for (int64_t z = z0; z <= z1; z++)
				for (int64_t y = y0; y <= y1; y++)
					for (int64_t x = x0; x <= x1; x++)
						map[CellKey(x, y, z)].push_back(i);
		}
	}
	template <class F> void Visit(const V& c, F fn) const
	{
		if (map.empty()) return;
		const auto it = map.find(CellKey((int64_t)std::floor(c.x / cell), (int64_t)std::floor(c.y / cell), (int64_t)std::floor(c.z / cell)));
		if (it == map.end()) return;
		for (const int32_t i : it->second) fn((*pits)[(size_t)i]);
	}
};

                                                                      
                                                                         
                                                                            
                                                                           
                                                                        
                                                                             
template <class S>
m::Manifold RefineBySize(const m::Manifold& base, S sizeAt, int maxLevels, size_t maxTris)
{
	m::MeshGL64 mg = base.GetMeshGL64();
	const size_t np = mg.numProp;
	if (np < 3 || mg.NumTri() == 0) return base;
	std::vector<uint64_t> canon(mg.NumVert());
	for (size_t v = 0; v < canon.size(); ++v) canon[v] = v;
	for (size_t i = 0; i < mg.mergeFromVert.size(); ++i) canon[(size_t)mg.mergeFromVert[i]] = mg.mergeToVert[i];
	for (size_t v = 0; v < canon.size(); ++v)
	{
		uint64_t r = canon[v];
		int guard = 0;
		while (canon[(size_t)r] != r && guard++ < 64) r = canon[(size_t)r];
		canon[v] = r;
	}
	std::vector<double> props(mg.vertProperties.begin(), mg.vertProperties.end());
	std::vector<std::pair<uint64_t, uint64_t>> merges;
	for (size_t i = 0; i < mg.mergeFromVert.size(); ++i) merges.push_back({ mg.mergeFromVert[i], mg.mergeToVert[i] });
	struct Tri { uint64_t v[3]; uint64_t face; uint32_t run; uint32_t fresh; };
	std::vector<Tri> tris;
	tris.reserve(mg.NumTri());
	const size_t nRun = mg.runIndex.size() > 1 ? mg.runIndex.size() - 1 : 1;
	for (size_t r = 0; r < nRun; ++r)
	{
		const size_t t0 = mg.runIndex.size() > 1 ? mg.runIndex[r] / 3 : 0;
		const size_t t1 = mg.runIndex.size() > 1 ? mg.runIndex[r + 1] / 3 : mg.NumTri();
		for (size_t t = t0; t < t1; ++t)
		{
			Tri T;
			for (int k = 0; k < 3; ++k) T.v[k] = mg.triVerts[3 * t + (size_t)k];
			T.face = t < mg.faceID.size() ? mg.faceID[t] : t;
			T.run = (uint32_t)r;
			T.fresh = 1;
			tris.push_back(T);
		}
	}
	const size_t startTris = tris.size();
	auto pos = [&](uint64_t v) { return V(props[v * np], props[v * np + 1], props[v * np + 2]); };
	auto key = [](uint64_t a, uint64_t b) { return a < b ? (a << 32) | b : (b << 32) | a; };
	for (int level = 0; level < maxLevels && tris.size() < maxTris; ++level)
	{
		std::vector<char> red(tris.size(), 0);
		ProjectDamageParallelFor(0, (int64_t)tris.size(), [&](int64_t t)
		{
			const Tri& T = tris[(size_t)t];
			if (!T.fresh) return;                                                 
			const V a = pos(T.v[0]), b = pos(T.v[1]), c = pos(T.v[2]);
			const double longest = std::max({ m::la::length(b - a), m::la::length(c - b), m::la::length(a - c) });
			if (longest > sizeAt((a + b + c) / 3.0, (int32_t)T.face)) red[(size_t)t] = 1;
		});
		size_t want = 0;
		for (char r : red) want += (size_t)r;
		if (want == 0) break;
		if (tris.size() + 3 * want > maxTris) break;
		std::unordered_map<uint64_t, uint8_t> marked;
		marked.reserve(want * 3);
		for (size_t t = 0; t < tris.size(); ++t)
			if (red[t])
				for (int k = 0; k < 3; ++k) marked[key(canon[tris[t].v[k]], canon[tris[t].v[(k + 1) % 3]])] = 1;
		std::unordered_map<uint64_t, uint64_t> midOf, geoMid;
		midOf.reserve(marked.size() * 2);
		geoMid.reserve(marked.size());
		auto mid = [&](uint64_t a, uint64_t b)
		{
			const uint64_t pk = key(a, b);
			auto it = midOf.find(pk);
			if (it != midOf.end()) return it->second;
			const uint64_t nv = props.size() / np;
			for (size_t j = 0; j < np; ++j) props.push_back(0.5 * (props[a * np + j] + props[b * np + j]));
			const uint64_t gk = key(canon[a], canon[b]);
			auto g = geoMid.find(gk);
			if (g == geoMid.end()) { geoMid[gk] = nv; canon.push_back(nv); }
			else { canon.push_back(canon[g->second]); merges.push_back({ nv, g->second }); }
			midOf[pk] = nv;
			return nv;
		};
		std::vector<Tri> next;
		next.reserve(tris.size() + 3 * want);
		for (size_t t = 0; t < tris.size(); ++t)
		{
			const Tri& T = tris[t];
			bool e[3];
			int n = 0;
			for (int k = 0; k < 3; ++k) { e[k] = marked.count(key(canon[T.v[k]], canon[T.v[(k + 1) % 3]])) > 0; n += e[k]; }
			if (n == 0) { next.push_back(T); next.back().fresh = 0; continue; }
			if (n == 3)
			{
				const uint64_t m0 = mid(T.v[0], T.v[1]), m1 = mid(T.v[1], T.v[2]), m2 = mid(T.v[2], T.v[0]);
				next.push_back({ { T.v[0], m0, m2 }, T.face, T.run, 1 });
				next.push_back({ { m0, T.v[1], m1 }, T.face, T.run, 1 });
				next.push_back({ { m2, m1, T.v[2] }, T.face, T.run, 1 });
				next.push_back({ { m0, m1, m2 }, T.face, T.run, 1 });
				continue;
			}
			if (n == 2)
			{
				int k = 0;
				while (e[k]) ++k;
				const uint64_t x = T.v[k], y = T.v[(k + 1) % 3], z = T.v[(k + 2) % 3];
				const uint64_t m1 = mid(y, z), m2 = mid(z, x);
				                                                      
				if (m::la::length(pos(x) - pos(m1)) <= m::la::length(pos(y) - pos(m2)))
				{
					next.push_back({ { x, y, m1 }, T.face, T.run, 1 });
					next.push_back({ { x, m1, m2 }, T.face, T.run, 1 });
				}
				else
				{
					next.push_back({ { x, y, m2 }, T.face, T.run, 1 });
					next.push_back({ { y, m1, m2 }, T.face, T.run, 1 });
				}
				next.push_back({ { m2, m1, z }, T.face, T.run, 1 });
				continue;
			}
			int k = 0;
			while (!e[k]) ++k;
			const uint64_t a = T.v[k], b = T.v[(k + 1) % 3], c = T.v[(k + 2) % 3];
			const uint64_t mm = mid(a, b);
			next.push_back({ { a, mm, c }, T.face, T.run, 1 });
			next.push_back({ { mm, b, c }, T.face, T.run, 1 });
		}
		tris.swap(next);
	}
	if (tris.size() == startTris) return base;
	std::stable_sort(tris.begin(), tris.end(), [](const Tri& x, const Tri& y) { return x.run < y.run; });
	m::MeshGL64 out;
	out.numProp = (uint32_t)np;
	out.vertProperties = std::move(props);
	out.triVerts.reserve(tris.size() * 3);
	out.faceID.reserve(tris.size());
	out.runOriginalID = mg.runOriginalID;
	out.runTransform = mg.runTransform;
	size_t run = 0;
	out.runIndex.push_back(0);
	for (size_t t = 0; t < tris.size(); ++t)
	{
		while (run < tris[t].run) { out.runIndex.push_back(t * 3); ++run; }
		for (int k = 0; k < 3; ++k) out.triVerts.push_back(tris[t].v[k]);
		out.faceID.push_back(tris[t].face);
	}
	while (out.runIndex.size() < mg.runIndex.size()) out.runIndex.push_back(tris.size() * 3);
	if (mg.runIndex.size() <= 1) { out.runIndex.clear(); out.runOriginalID.clear(); out.runTransform.clear(); }
	for (const auto& mp : merges) { out.mergeFromVert.push_back(mp.first); out.mergeToVert.push_back(mp.second); }
	m::Manifold refined(out);
	if (refined.Status() != m::Manifold::Error::NoError || refined.IsEmpty()) return base;
	return refined;
}

                                                                           
                                                                     
void AddPore(const ConcretePitsParams& p, uint64_t seed, uint64_t key, bool dent, const V& c, const V& n,
	double w, double minRadius, std::vector<Pit>& out, size_t& skipped)
{
	Pit s;
	if (!MakePit(p, seed, key, dent, c, n, w, minRadius, s)) { skipped++; return; }
	out.push_back(s);
	if (dent || Random01(seed, key, 12) >= ClampD(p.poreMerge, 0.0, 1.0)) return;
	const int extra = 1 + (int)(Random01(seed, key, 13) * 3.0);
	const double r = std::max(s.a, s.b);
	for (int j = 0; j < extra; j++)
	{
		const uint64_t sk = key ^ ((uint64_t)(j + 1) << 48);
		const double ang = Random01(seed, sk, 14) * 2.0 * kPi;
		const double dist = r * (0.55 + 0.6 * Random01(seed, sk, 15));
		const V at = c + (s.du * std::cos(ang) + s.dv * std::sin(ang)) * dist;
		Pit t;
		if (!MakePit(p, seed, sk, false, at, n, w, minRadius, t)) continue;
		const double k = (0.35 + 0.45 * Random01(seed, sk, 16)) * r / std::max(t.a, t.b);
		if (k < 1.0)
		{
			t.a *= k; t.b *= k; t.reach *= k;
			t.depth *= std::sqrt(k);
			if (std::max(t.a, t.b) < minRadius) continue;
		}
		out.push_back(t);
	}
}

}             

// основной вход: собираем геометрию по переданным настройкам
bool ApplyConcretePits(const CoreInput& in, const ConcretePitsParams& p, Int32 mainSeed,
	const IFieldMask* mask, const m::Manifold& base, m::Manifold& out, std::string* log)
{
	if (!p.enable || in.view.npolys <= 0 || in.view.npts <= 0)
		return false;
	if (base.Status() != m::Manifold::Error::NoError || base.NumTri() == 0)
		return false;
	const uint64_t seed = SeedOf(mainSeed, p.seed);
	const MeshView& view = in.view;

	                                                                                 
	const bool haveW = p.weights.size() == (size_t)view.npts;
	auto Wv = [&](int32_t i) { return haveW ? ClampD(p.weights[(size_t)i], 0.0, 1.0) : 1.0; };
	std::vector<bool> avoidPoly((size_t)view.npolys, false);
	if (p.avoid && in.avoidPoly != nullptr)
		for (size_t i = 0; i < avoidPoly.size() && i < in.avoidPoly->size(); i++)
			avoidPoly[i] = (*in.avoidPoly)[i];

	                                                                                   
	struct Tri { V a, b, c, n; double area; int32_t ia, ib, ic, poly; };
	std::vector<Tri> tris;
	tris.reserve((size_t)view.npolys * 2);
	double zoneArea = 0.0;
	std::vector<m::Box> zones;
	for (int32_t i = 0; i < view.npolys; i++)
	{
		if (avoidPoly[(size_t)i]) continue;
		const ProjectDamagePoly& q = view.polys[i];
		const int k = q.c == q.d ? 1 : 2;
		for (int h = 0; h < k; h++)
		{
			const int32_t ia = q.a, ib = h == 0 ? q.b : q.c, ic = h == 0 ? q.c : q.d;
			if (Wv(ia) <= 0.0 && Wv(ib) <= 0.0 && Wv(ic) <= 0.0) continue;
			const V a = view.pts[ia], b = view.pts[ib], c = view.pts[ic];
			const V cr = m::la::cross(b - a, c - a);
			const double ar = 0.5 * m::la::length(cr);
			if (ar <= 1e-14) continue;
			tris.push_back({ a, b, c, cr / (2.0 * ar), ar, ia, ib, ic, i });
			zoneArea += ar;
			m::Box bx;
			bx.Union(a); bx.Union(b); bx.Union(c);
			zones.push_back(bx);
		}
	}
	if (tris.empty() || zoneArea <= 0.0)
		return false;

	                                                                                    
	                                                                      
	                                                                           
	                                                                          
	double L = std::max(p.edgeLength, 1e-3) / std::max(p.detail, 0.05);
	const double budget = (double)std::max(p.maxTris, 1000);
	L = std::max(L, std::sqrt(zoneArea / (0.433 * budget * 0.45)));
	const double fineEdge = std::min(L, std::max(p.fineEdge, 1e-3) / std::max(p.detail, 0.05));
	const double flakeEdge = ClampD(0.3 * p.aggregateSize, fineEdge, L);

	const bool maskOn = mask != nullptr && mask->Active() && mask->Amount() > 0.0;
	auto maskW = [&](const V& pos) { return maskOn ? 1.0 - mask->Amount() * (1.0 - ClampD(mask->Weight(pos), 0.0, 1.0)) : 1.0; };

	                                                                               
	Field F;
	InitField(F, p, seed, 0.0, L);
	F.edgeFlake = flakeEdge;
	std::vector<double> cdf(tris.size());
	double acc = 0.0;
	for (size_t i = 0; i < tris.size(); i++) { acc += tris[i].area; cdf[i] = acc; }
	const double minR = 1.5 * fineEdge;
	size_t skippedSmall = 0;
	for (int kind = 0; kind < 2; kind++)
	{
		const bool dent = kind == 1;
		const double dens = dent ? p.dentDensity : p.poreDensity;
		if (dens <= 0.0) continue;
		const double cl = ClampD(p.clustering, 0.0, 1.0);
		const double boost = dent ? 1.0 : (1.0 + 2.0 * cl) * (1.0 + (p.flakeEnable ? std::max(p.flakePores, 0.0) : 0.0));
		const double maxDens = dens * boost / 100.0;                           
		double want = maxDens * acc;
		const double cap = 3.0e6;
		const double thin = want > cap ? cap / want : 1.0;
		const int64_t count = (int64_t)std::ceil(want * thin);
		const uint64_t base_key = dent ? (1ULL << 32) : 0;
		for (int64_t i = 0; i < count; i++)
		{
			const uint64_t key = base_key + (uint64_t)i;
			const double pick = Random01(seed, key, 0) * acc;
			const size_t ti = std::min((size_t)(std::lower_bound(cdf.begin(), cdf.end(), pick) - cdf.begin()), tris.size() - 1);
			const Tri& T = tris[ti];
			double r1 = Random01(seed, key, 1), r2 = Random01(seed, key, 2);
			if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
			const V c = T.a + (T.b - T.a) * r1 + (T.c - T.a) * r2;
			double w = (1.0 - r1 - r2) * Wv(T.ia) + r1 * Wv(T.ib) + r2 * Wv(T.ic);
			w *= maskW(c);
			if (w <= 0.0) continue;
			double here = dens * w;
			if (!dent)
			{
				here *= F.Cluster(c);
				if (p.flakeEnable) here *= 1.0 + std::max(p.flakePores, 0.0) * F.FlakeMask(c, w);
			}
			if (Random01(seed, key, 3) * dens * boost >= here) continue;
			AddPore(p, seed, key, dent, c, T.n, w, minR, F.pits, skippedSmall);
		}
	}
	F.BuildGrid();
	auto weightIn = [&](int32_t poly, const V& pos) -> double
	{
		if (poly < 0 || poly >= view.npolys) return 0.0;
		if (avoidPoly[(size_t)poly]) return 0.0;
		const ProjectDamagePoly& q = view.polys[poly];
		if (!haveW) return 1.0;
		const int k = q.c == q.d ? 1 : 2;
		double best = 1e30, wBest = 0.0;
		for (int h = 0; h < k; h++)
		{
			const int32_t ia = q.a, ib = h == 0 ? q.b : q.c, ic = h == 0 ? q.c : q.d;
			const V a = view.pts[ia], b = view.pts[ib], c = view.pts[ic];
			const V e0 = b - a, e1 = c - a, d = pos - a;
			const double d00 = m::la::dot(e0, e0), d01 = m::la::dot(e0, e1), d11 = m::la::dot(e1, e1);
			const double d20 = m::la::dot(d, e0), d21 = m::la::dot(d, e1);
			const double den = d00 * d11 - d01 * d01;
			if (std::abs(den) < 1e-30) continue;
			double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
			double u = 1.0 - v - w;
			const double out = std::max({ 0.0, -u, -v, -w });
			u = std::max(u, 0.0); v = std::max(v, 0.0); w = std::max(w, 0.0);
			const double s = u + v + w;
			if (s <= 0.0) continue;
			if (out < best) { best = out; wBest = (u * Wv(ia) + v * Wv(ib) + w * Wv(ic)) / s; }
		}
		return wBest;
	};
	                                                                                 
	size_t allTris = 0;
	for (int32_t i = 0; i < view.npolys; i++) allTris += view.polys[i].c == view.polys[i].d ? 1 : 2;
	m::Manifold refined;
	if (tris.size() >= allTris || zones.size() > 200000)
	{
		                                                 
		refined = base.RefineToLength(L);
	}
	else
	{
		                                                                    
		                                                                  
		                                                                     
		                        
		for (int guard = 0; guard < 40; guard++)
		{
			double est = 0.0;
			for (const Tri& T : tris)
			{
				const double le = std::max({ m::la::length(T.b - T.a), m::la::length(T.c - T.b), m::la::length(T.a - T.c) });
				const int k = le > L ? (int)std::ceil(std::log2(le / L)) : 0;
				est += std::pow(4.0, k);
			}
			if (est <= budget * 0.45) break;
			L *= 1.25;
		}
		refined = RefineNearDamage(base, zones, L, 0.35, 24, (size_t)(budget * 2.0));
	}
	if (refined.Status() != m::Manifold::Error::NoError || refined.NumTri() == 0)
		return false;
	{
		                                                                 
		const double grade = 0.35;
		PitGrid pg;
		pg.Build(F.pits, [&](const Pit& q) { return q.reach + (L - PitEdge(q, fineEdge)) / grade; });
		const bool flakes = p.flakeEnable && p.flakeCoverage > 0.0 && p.flakeDepth > 0.0 && flakeEdge < 0.9 * L;
		auto sizeAt = [&](const V& c, int32_t face) -> double
		{
			double best = 1e300;
			pg.Visit(c, [&](const Pit& q)
			{
				const double d = std::max(0.0, m::la::length(c - q.c) - q.reach);
				best = std::min(best, PitEdge(q, fineEdge) + grade * d);
			});
			if (flakes && best > flakeEdge)
			{
				const double w = weightIn(face, c) * maskW(c);
				if (w > 0.0 && F.FlakeNoise(c) >= F.FlakeThreshold(p.flakeCoverage * w) - 0.02)
					best = flakeEdge;
			}
			return best;
		};
		m::Manifold r = RefineBySize(refined, sizeAt, 10, (size_t)budget);
		if (r.Status() == m::Manifold::Error::NoError && r.NumTri() > 0) refined = std::move(r);
	}
	m::MeshGL64 mesh = refined.GetMeshGL64();
	const size_t np = mesh.numProp;
	const size_t nv = mesh.NumVert();
	const size_t nt = mesh.NumTri();
	if (np < 3 || nv == 0 || nt == 0)
		return false;
	std::vector<int32_t> geo(nv);
	for (size_t i = 0; i < nv; i++) geo[i] = (int32_t)i;
	for (size_t k = 0; k < mesh.mergeFromVert.size() && k < mesh.mergeToVert.size(); k++)
		geo[mesh.mergeFromVert[k]] = (int32_t)mesh.mergeToVert[k];
	for (size_t i = 0; i < nv; i++)                             
	{
		int32_t r = geo[i];
		int guard = 0;
		while (geo[(size_t)r] != r && guard++ < 64) r = geo[(size_t)r];
		geo[i] = r;
	}
	auto P = [&](size_t i) { return V(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]); };

	                                                                      
	std::vector<V> nrm(nv, V(0.0));
	std::vector<int64_t> triOf(nv, -1);
	for (size_t t = 0; t < nt; t++)
	{
		const size_t id[3] = { mesh.triVerts[t * 3], mesh.triVerts[t * 3 + 1], mesh.triVerts[t * 3 + 2] };
		const V pt[3] = { P(id[0]), P(id[1]), P(id[2]) };
		const V fn = Unit(m::la::cross(pt[1] - pt[0], pt[2] - pt[0]), V(0.0));
		for (int k = 0; k < 3; k++)
		{
			const V e0 = Unit(pt[(k + 1) % 3] - pt[k], V(0.0));
			const V e1 = Unit(pt[(k + 2) % 3] - pt[k], V(0.0));
			const double ang = std::acos(ClampD(m::la::dot(e0, e1), -1.0, 1.0));
			nrm[(size_t)geo[id[k]]] += fn * ang;
			if (triOf[(size_t)geo[id[k]]] < 0) triOf[(size_t)geo[id[k]]] = (int64_t)t;
		}
	}
	if (view.guideField) for (size_t i=0;i<nv;++i) nrm[i]=GuidedPointNormal(view,P(i),Unit(nrm[i],V(0.0)));
	const double edge = L;

	                                                                             
	ProjectDamageBVH bvh;
	bool bvhBuilt = false;
	auto needBvh = [&]() { if (!bvhBuilt) { bvh.Build(view); bvhBuilt = true; } };
	const bool haveFaceID = mesh.faceID.size() == nt;
	std::vector<double> weight(nv, 0.0);
	if (!haveFaceID) needBvh();
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t i)
	{
		if (geo[(size_t)i] != (int32_t)i || triOf[(size_t)i] < 0) return;
		const V pos = P((size_t)i);
		int32_t poly = -1;
		if (haveFaceID)
			poly = (int32_t)mesh.faceID[(size_t)triOf[(size_t)i]];
		if (poly < 0 || poly >= view.npolys)
		{
			if (!bvhBuilt) return;
			const V n = Unit(nrm[(size_t)i], V(0, 0, 1));
			double hit = 0.0;
			const double eps = std::max(1e-3, edge);
			if (!bvh.Raycast(pos + n * eps, -n, 4.0 * eps, &hit, &poly)) return;
		}
		weight[(size_t)i] = weightIn(poly, pos);
	});


	                                                                               
	std::vector<double> disp(nv, 0.0);
	const double capK = std::max(p.thicknessCap, 0.0);
	if (capK > 0.0) needBvh();
	const double feather = std::max(p.zoneFeather, 1e-3);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t i)
	{
		if (geo[(size_t)i] != (int32_t)i || weight[(size_t)i] <= 0.0) return;
		const V n = Unit(nrm[(size_t)i], V(0.0));
		if (m::la::dot(n, n) == 0.0) return;
		const V pos = P((size_t)i);
		const double w = weight[(size_t)i] * maskW(pos);
		if (w <= 0.0) return;
		double d = F.Height(pos, n, w) * Smooth(0.0, feather, w);
		if (capK > 0.0 && d < 0.0)
		{
			const double eps = 1e-3;
			double hit = 0.0;
			if (bvh.Raycast(pos - n * eps, -n, -d / capK, &hit))
				d = std::max(d, -capK * (hit + eps));
		}
		disp[(size_t)i] = d;
	});
	std::vector<V> moved(nv);
	for (size_t i = 0; i < nv; i++)
		if (geo[i] == (int32_t)i)
			moved[i] = P(i) + Unit(nrm[i], V(0.0)) * disp[i];
	for (size_t i = 0; i < nv; i++)
	{
		const size_t g = (size_t)geo[i];
		if (disp[g] == 0.0) continue;
		const V q = moved[g];
		mesh.vertProperties[i * np] = q.x;
		mesh.vertProperties[i * np + 1] = q.y;
		mesh.vertProperties[i * np + 2] = q.z;
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0)
	{
		if (log) *log = "[ProjectDamage] Concrete Pitting: the displaced mesh failed Manifold's checks - skipped";
		return false;
	}
	if (log)
		*log = "[ProjectDamage] Concrete Pitting: " + std::to_string(F.pits.size()) + " pores/dents, "
			+ std::to_string(nt) + " triangles, edge " + std::to_string(edge)
			+ (skippedSmall ? ", " + std::to_string(skippedSmall) + " pores below the mesh resolution left to the texture" : std::string());
	out = std::move(result);
	return true;
}

bool ConcretePitsHeightMap(const ConcretePitsParams& p, Int32 mainSeed, double tileSize,
	int32_t res, std::vector<float>& out)
{
	if (tileSize <= 0.0 || res < 8 || res > 16384)
		return false;
	const uint64_t seed = SeedOf(mainSeed, p.seed);
	const double S = tileSize;
	const double px = S / (double)res;
	Field F;
	InitField(F, p, seed, S, 0.0);
	const V up(0.0, 0.0, 1.0);
	                                                                         
	                                                              
	const double area = S * S;
	std::vector<Pit> base;
	for (int kind = 0; kind < 2; kind++)
	{
		const bool dent = kind == 1;
		const double dens = dent ? p.dentDensity : p.poreDensity;
		if (dens <= 0.0) continue;
		const double cl = ClampD(p.clustering, 0.0, 1.0);
		const double boost = dent ? 1.0 : (1.0 + 2.0 * cl) * (1.0 + (p.flakeEnable ? std::max(p.flakePores, 0.0) : 0.0));
		const double want = std::min(dens * boost / 100.0 * area, 4.0e6);
		const int64_t count = (int64_t)std::ceil(want);
		const uint64_t base_key = dent ? (1ULL << 32) : 0;
		for (int64_t i = 0; i < count; i++)
		{
			const uint64_t key = base_key + (uint64_t)i;
			const V c(Random01(seed, key, 1) * S, Random01(seed, key, 2) * S, 0.0);
			double here = dens;
			if (!dent)
			{
				here *= F.Cluster(c);
				if (p.flakeEnable) here *= 1.0 + std::max(p.flakePores, 0.0) * F.FlakeMask(c, 1.0);
			}
			if (Random01(seed, key, 3) * dens * boost >= here) continue;
			size_t skipped = 0;
			AddPore(p, seed, key, dent, c, up, 1.0, 0.5 * px, base, skipped);
		}
	}
	for (const Pit& s : base)
		for (int oy = -1; oy <= 1; oy++)
			for (int ox = -1; ox <= 1; ox++)
			{
				Pit g = s;
				g.c = s.c + V(ox * S, oy * S, 0.0);
				if (g.c.x + g.reach < 0.0 || g.c.x - g.reach > S || g.c.y + g.reach < 0.0 || g.c.y - g.reach > S) continue;
				F.pits.push_back(g);
			}
	F.BuildGrid();
	out.assign((size_t)res * (size_t)res, 0.0f);
	ProjectDamageParallelFor(0, (int64_t)res, [&](int64_t y)
	{
		for (int32_t x = 0; x < res; x++)
		{
			const V pos((x + 0.5) * px, (y + 0.5) * px, 0.0);
			out[(size_t)y * (size_t)res + (size_t)x] = (float)F.Height(pos, up, 1.0);
		}
	});
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
