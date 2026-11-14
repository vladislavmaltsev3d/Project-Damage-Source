// добавляем слои штукатурки, следы инструмента и мелкий рельеф
// ProjectDamage plaster displacement — GPL-3.0-or-later; see LICENSE.
//
// Recipe: plaster-displace-v1. A procedural relief of hand-troweled plaster
// (smeared ridges, flat trowel strokes, fine sand grain, hairline cracks),
// displaced into the source mesh itself:
//   1. refine: the source Manifold is subdivided flat with RefineToLength, so
//      the shape does not change; Manifold interpolates every property
//      channel (UVs) and keeps the run original id and faceID per triangle;
//   2. height field H(p, n), evaluated once per welded vertex:
//        - base coat: a mid level plus broad, low waviness;
//        - trowel strokes, coat by coat and in a fixed order (painter's
//          algorithm): each stroke is a local frame on the surface (origin,
//          normal, blade direction) and a ragged, slightly bent rounded
//          rectangle. Inside it the height is pulled toward the stroke's own
//          tilted, striated plane; along its rim the displaced plaster stands
//          up as a lip that is crisp on the trowel side and slopes away on
//          the outside, strongest at the trailing end;
//        - grain: fine roughness and small pits, faded out where the mesh is
//          too coarse to carry it;
//        - cracks: borders of a domain-warped 3D Worley network (object space,
//          so they run over corners and curved surfaces), a share of them
//          cracked, as a V groove with a stepped offset between the two sides
//          and a smeared filler bead beside it;
//   3. displacement: every vertex moves along its angle-weighted smooth normal
//      by (H - sink * strength) x mask weight, capped by the local wall
//      thickness (one inward ray per vertex); the rebuilt Manifold keeps the
//      merge table, so UV seams stay closed.
// Strokes use SplitMix64 counter channels keyed by (coat, stroke) and channel.
//
// Channel map (stroke key = coat * 2^20 + index): 0 triangle, 1-2 barycentric,
// 3 length, 4 width, 5 angle jitter, 6 cross, 7 level, 8 tilt, 9 curve,
// 10 ridge strength, 11-16 noise offsets, 17 taper, 18 lip side. Global noise offsets use key 2^40.
#include "manifold/manifold.h"
#include "plaster_displace.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

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

double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Smooth(double e0, double e1, double x)
{
	if (e1 == e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

                           
double SNoise(const V& q) { return 2.0 * ProjectDamageNoise(q) - 1.0; }

double Fbm(V q, int octaves)
{
	double sum = 0.0, amp = 1.0, norm = 0.0;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * SNoise(q);
		norm += amp;
		amp *= 0.5;
		q = q * 2.03 + V(17.13, -9.31, 5.77);
	}
	return sum / norm;
}

V Rotate(const V& v, const V& axis, double ang)
{
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + m::la::cross(axis, v) * s + axis * (m::la::dot(axis, v) * (1.0 - c));
}

V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

V NoiseOffset(uint64_t seed, uint64_t key, uint64_t ch)
{
	return V(Random01(seed, key, ch) * 256.0, Random01(seed, key, ch + 1) * 256.0,
		Random01(seed, key, ch + 2) * 256.0);
}

uint64_t CellKey(int64_t x, int64_t y, int64_t z)
{
	return (uint64_t)(x & 0x1FFFFF) | ((uint64_t)(y & 0x1FFFFF) << 21) | ((uint64_t)(z & 0x1FFFFF) << 42);
}

struct Stroke
{
	V c, n, du, dv;
	double a = 1.0, b = 1.0;                                                   
	double level = 0.0, tiltK = 0.0, curveK = 0.0, ridgeK = 1.0;
	double taperK = 0.0, sideK = 1.0;                                                
	double reach = 1.0;                                          
	V offJag, offStreak, offRidge;
};

struct Field
{
	const PlasterDisplaceParams* p = nullptr;
	double strength = 0.0, edge = 1.0;                                      
	std::vector<Stroke> strokes;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	double cell = 1.0;
	V offUnd, offGrain, offPit, offWarp1, offWarp2, offWarp3, offWarp4, offBreak, offWidth, offLip;
	uint64_t crackSeed = 0;

	double StrokeLayer(const V& pos, const V& nrm, double H) const
	{
		if (strokes.empty())
			return H;
		const auto it = grid.find(CellKey((int64_t)std::floor(pos.x / cell),
			(int64_t)std::floor(pos.y / cell), (int64_t)std::floor(pos.z / cell)));
		if (it == grid.end())
			return H;
		const PlasterDisplaceParams& P = *p;
		const double rw = std::max(P.ridgeWidth, 1e-3);
		const double soft = std::max(1.2 * edge, 0.04);
		const double innerW = rw * 0.5 * (1.0 - ClampD(P.ridgeSharp, 0.0, 1.0)) + 1.2 * edge;
		const double outExp = 1.2 + 2.0 * ClampD(P.ridgeSharp, 0.0, 1.0);
		for (const int32_t si : it->second)
		{
			const Stroke& s = strokes[(size_t)si];
			const V d = pos - s.c;
			if (m::la::dot(d, d) > s.reach * s.reach)
				continue;
			if (m::la::dot(nrm, s.n) < 0.35)
				continue;                                                  
			const double w = m::la::dot(d, s.n);
			if (std::abs(w) > 0.35 * s.reach + 4.0 * strength)
				continue;                                                          
			const double u = m::la::dot(d, s.du);
			double v = m::la::dot(d, s.dv);
			v -= s.curveK * u * u / (2.0 * s.a);                                     
			                                 
			                                                               
			const double bu = s.b * (1.0 - s.taperK * 0.5 * (1.0 - ClampD(u / s.a, -1.0, 1.0)));
			const double r = 0.45 * bu;
			const double qx = std::abs(u) - s.a + r, qy = std::abs(v) - bu + r;
			const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
			double sd = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
			if (P.jagged > 0.0)
				sd += P.jagged * s.b * (0.3 * Fbm(V(u, v, 0.0) * (1.3 / s.b) + s.offJag, 2)
					+ 0.04 * SNoise(V(u, v, 0.0) * (5.5 / s.b) + s.offJag * 1.7));
			if (sd > 1.05 * rw)
				continue;
			double plateau = s.level + s.tiltK * (u / s.a);
			if (P.streaks > 0.0)
				plateau += P.streaks * strength * 0.12
					* SNoise(V(u * 0.45 / s.b, v * 6.5 / s.b, 0.0) + s.offStreak);
			                           
			const double t = Smooth(0.0, soft, -sd) * ClampD(P.flatten, 0.0, 1.0);
			H = Lerp(H, plateau, t);
			                                                
			if (P.ridgeHeight > 0.0 && sd > -innerW)
			{
				const double trail = Lerp(1.0, Smooth(-0.4, 0.9, u / s.a), ClampD(P.ridgeTrail, 0.0, 1.0));
				                                                              
				const double side = Lerp(1.0, Smooth(-0.7, 0.7, s.sideK * v / s.b), 0.6);
				                                                   
				const double along = 0.12 + 0.88 * Smooth(0.3, 0.62, ProjectDamageNoise(V(u, v, 0.0) * (1.4 / s.b) + s.offRidge));
				const double lipH = P.ridgeHeight * strength * s.ridgeK * trail * side * along;
				const double prof = sd < 0.0
					? Smooth(-innerW, 0.0, sd)
					: std::pow(std::max(0.0, 1.0 - sd / rw), outExp);
				if (prof > 0.0 && lipH > 0.0)
				{
					const double cand = std::max(H, plateau + lipH * prof);
					H = Lerp(H, cand, std::min(1.0, prof * 4.0));
				}
			}
		}
		return H;
	}

	double CrackLayer(const V& pos) const
	{
		const PlasterDisplaceParams& P = *p;
		if (!P.crackEnable || P.crackAmount <= 0.0 || (P.crackDepth <= 0.0 && P.crackStep <= 0.0))
			return 0.0;
		const double S = std::max(P.crackScale, 1e-3);
		V q = pos / S;
		const double mea = ClampD(P.crackMeander, 0.0, 2.0);
		if (mea > 0.0)
		{
			const V q2 = q * 2.0;
			q += V(Fbm(q2 + offWarp1, 2), Fbm(q2 + offWarp2, 2), Fbm(q2 + offWarp3, 2)) * (0.32 * mea);
			const V q7 = q * 7.0 + offWarp4;
			q += V(SNoise(q7), SNoise(q7 + V(31.7, 0, 0)), SNoise(q7 + V(0, 47.3, 0))) * (0.05 * mea);
		}
		const int64_t cx = (int64_t)std::floor(q.x), cy = (int64_t)std::floor(q.y), cz = (int64_t)std::floor(q.z);
		double d1 = 1e30, d2 = 1e30;
		V f1(0.0), f2(0.0);
		uint64_t id1 = 0, id2 = 0;
		for (int64_t dz = -1; dz <= 1; dz++)
			for (int64_t dy = -1; dy <= 1; dy++)
				for (int64_t dx = -1; dx <= 1; dx++)
				{
					const int64_t x = cx + dx, y = cy + dy, z = cz + dz;
					const uint64_t id = CellKey(x, y, z);
					const V f = V((double)x, (double)y, (double)z)
						+ V(0.1 + 0.8 * Random01(crackSeed, id, 0), 0.1 + 0.8 * Random01(crackSeed, id, 1),
							0.1 + 0.8 * Random01(crackSeed, id, 2));
					const V df = q - f;
					const double dd = m::la::dot(df, df);
					if (dd < d1) { d2 = d1; f2 = f1; id2 = id1; d1 = dd; f1 = f; id1 = id; }
					else if (dd < d2) { d2 = dd; f2 = f; id2 = id; }
				}
		const double sep = m::la::length(f2 - f1);
		if (sep < 1e-9)
			return 0.0;
		const double dist = (d2 - d1) / (2.0 * sep) * S;                                     
		const uint64_t lo = std::min(id1, id2), hi = std::max(id1, id2);
		if (Random01(crackSeed ^ lo, hi, 7) >= P.crackAmount)
			return 0.0;
		const double depth = std::max(P.crackDepth, 0.0);
		const double wHalf = 0.5 * std::max(P.crackWidth, 0.0);
		const double reach = std::max(wHalf, 0.75 * edge) * 4.0 + S * 0.25;
		if (dist > reach)
			return 0.0;
		                                           
		const double bu = ClampD(P.crackBreakup, 0.0, 1.0);
		const double run = ProjectDamageNoise(pos * (2.5 / S) + offBreak);
		const double mod = bu > 0.0 ? Smooth(0.25 + 0.3 * bu, 0.4 + 0.3 * bu, run) : 1.0;
		if (mod <= 0.0)
			return 0.0;
		const double w = std::max(wHalf * (0.6 + 0.8 * ProjectDamageNoise(pos * (4.0 / S) + offWidth)), 0.75 * edge);
		double h = 0.0;
		const double x = dist / w;
		if (x < 1.0)
			h -= depth * mod * (1.0 - x) * (1.0 - x);
		if (P.crackLip > 0.0)
		{
			const double g = (dist - 2.2 * w) / (1.6 * w);
			h += P.crackLip * depth * mod * std::exp(-g * g) * (0.4 + 0.6 * ProjectDamageNoise(pos * (9.0 / S) + offLip));
		}
		if (P.crackStep > 0.0)
		{
			const double side = Random01(crackSeed, id1, 9) > Random01(crackSeed, id2, 9) ? 0.5 : -0.5;
			h += P.crackStep * mod * side * std::exp(-dist / (S * 0.08));
		}
		return h;
	}

	double Height(const V& pos, const V& nrm) const
	{
		const PlasterDisplaceParams& P = *p;
		double H = 0.5 * strength;
		if (P.undulation > 0.0)
			H += P.undulation * strength * 0.5 * Fbm(pos / std::max(P.undulationScale, 1e-3) + offUnd, 3);
		H = StrokeLayer(pos, nrm, H);
		if (P.grain > 0.0)
		{
			const double gs = std::max(P.grainSize, 1e-4);
			const double fade = Smooth(1.5 * edge, 3.0 * edge, gs);
			if (fade > 0.0)
			{
				H += P.grain * fade * Fbm(pos / gs + offGrain, 2);
				if (P.pits > 0.0)
				{
					const double th = 0.5 - 0.22 * ClampD(P.pits, 0.0, 1.0);
					const double pn = ProjectDamageNoise(pos / (gs * 1.3) + offPit);
					H -= 2.5 * P.grain * fade * Smooth(th, th - 0.06, pn);
				}
			}
		}
		H += CrackLayer(pos);
		return H;
	}
};

}             

// основной вход: собираем геометрию по переданным настройкам
bool ApplyPlasterDisplacement(const CoreInput& in, const PlasterDisplaceParams& p,
	Int32 mainSeed, const IFieldMask* mask, m::Manifold& out,
    std::vector<uint8_t>* affectedFaces, const IFieldMask* zone)
{
	if (!p.enable || in.box == nullptr || in.view.npolys <= 0)
		return false;
	const m::Manifold& box = *in.box;
	if (box.Status() != m::Manifold::Error::NoError || box.NumTri() == 0)
		return false;
	const uint64_t seed = 0x504c41535445525fULL                
		^ ((uint64_t)(uint32_t)mainSeed * 0x9e3779b97f4a7c15ULL)
		^ ((uint64_t)(uint32_t)p.seed * 0xc2b2ae3d27d4eb4fULL);

	                                                                             
	struct Tri { V a, b, c, n; double area; int32_t poly; };
	std::vector<Tri> tris;
	tris.reserve((size_t)in.view.npolys * 2);
	double totalArea = 0.0;
	for (int32_t i = 0; i < in.view.npolys; i++)
	{
		const ProjectDamagePoly& q = in.view.polys[i];
		const int k = q.c == q.d ? 1 : 2;
		for (int h = 0; h < k; h++)
		{
			const V a = in.view.pts[q.a];
			const V b = in.view.pts[h == 0 ? q.b : q.c];
			const V c = in.view.pts[h == 0 ? q.c : q.d];
			const V cr = m::la::cross(b - a, c - a);
			const double ar = 0.5 * m::la::length(cr);
			if (ar <= 1e-14)
				continue;
			tris.push_back({ a, b, c, cr / (2.0 * ar), ar, i });
			totalArea += ar;
		}
	}
	if (tris.empty() || totalArea <= 0.0)
		return false;

	                                                                               
	double L = std::max(p.edgeLength, 1e-3) / std::max(p.detail, 0.05);
	const double budget = (double)std::max(p.maxTris, 1000);
	L = std::max(L, std::sqrt(totalArea / (0.433 * budget)) * 1.25);
	m::Manifold refined = box.RefineToLength(L);
	if (refined.Status() != m::Manifold::Error::NoError)
		return false;
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
	auto P = [&](size_t i) { return V(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]); };

	                                                   
	std::vector<V> nrm(nv, V(0.0));
	for (size_t t = 0; t < nt; t++)
	{
		const size_t i0 = mesh.triVerts[t * 3], i1 = mesh.triVerts[t * 3 + 1], i2 = mesh.triVerts[t * 3 + 2];
		const V a = P(i0), b = P(i1), c = P(i2);
		const V fn = Unit(m::la::cross(b - a, c - a), V(0.0));
		const size_t id[3] = { i0, i1, i2 };
		const V pt[3] = { a, b, c };
		for (int k = 0; k < 3; k++)
		{
			const V e0 = Unit(pt[(k + 1) % 3] - pt[k], V(0.0));
			const V e1 = Unit(pt[(k + 2) % 3] - pt[k], V(0.0));
			const double ang = std::acos(ClampD(m::la::dot(e0, e1), -1.0, 1.0));
			nrm[(size_t)geo[id[k]]] += fn * ang;
		}
	}
	if (in.view.guideField) for (size_t i=0;i<nv;++i) nrm[i]=GuidedPointNormal(in.view,P(i),Unit(nrm[i],V(0.0)));
	                                                                          
	double edgeSum = 0.0;
	for (size_t t = 0; t < nt; t++)
		edgeSum += m::la::length(P(mesh.triVerts[t * 3 + 1]) - P(mesh.triVerts[t * 3]));
	const double edge = std::max(edgeSum / (double)nt, 1e-4);

	                                                                               
	std::vector<double> weight(nv, 1.0);
	ProjectDamageBVH bvh;
	bool bvhBuilt = false;
	auto needBvh = [&]() { if (!bvhBuilt) { bvh.Build(in.view); bvhBuilt = true; } };
	if (p.avoid && in.avoidPoly != nullptr
		&& std::find(in.avoidPoly->begin(), in.avoidPoly->end(), true) != in.avoidPoly->end())
	{
		needBvh();
		const std::vector<bool>& avoid = *in.avoidPoly;
		std::vector<uint8_t> avoidTri(nt, 0);
		ProjectDamageParallelFor(0, (int64_t)nt, [&](int64_t t)
		{
			const V a = P(mesh.triVerts[t * 3]), b = P(mesh.triVerts[t * 3 + 1]), c = P(mesh.triVerts[t * 3 + 2]);
			const V fn = Unit(m::la::cross(b - a, c - a), V(0, 0, 1));
			const V ctr = (a + b + c) / 3.0;
			const double eps = std::max(1e-3, edge * 0.05);
			double hit = 0.0;
			int32_t poly = -1;
			if (bvh.Raycast(ctr + fn * eps, -fn, 2.0 * eps, &hit, &poly)
				&& poly >= 0 && (size_t)poly < avoid.size() && avoid[(size_t)poly])
				avoidTri[(size_t)t] = 1;
		});
		for (size_t t = 0; t < nt; t++)
			if (avoidTri[t])
				for (int k = 0; k < 3; k++)
					weight[(size_t)geo[mesh.triVerts[t * 3 + k]]] = 0.0;
	}
	const bool maskOn = mask != nullptr && mask->Active() && mask->Amount() > 0.0;

	                                                                               
	Field F;
	F.p = &p;
	F.strength = std::max(p.strength, 0.0);
	F.edge = edge;
	const uint64_t gk = 1ULL << 40;
	F.offUnd = NoiseOffset(seed, gk, 0);   F.offGrain = NoiseOffset(seed, gk, 3);
	F.offPit = NoiseOffset(seed, gk, 6);   F.offWarp1 = NoiseOffset(seed, gk, 9);
	F.offWarp2 = NoiseOffset(seed, gk, 12); F.offWarp3 = NoiseOffset(seed, gk, 15);
	F.offWarp4 = NoiseOffset(seed, gk, 18); F.offBreak = NoiseOffset(seed, gk, 21);
	F.offWidth = NoiseOffset(seed, gk, 24); F.offLip = NoiseOffset(seed, gk, 27);
	F.crackSeed = seed ^ 0x435241434b53ULL;              

	const int coats = std::max(0, std::min(p.coats, 6));
	if (coats > 0 && p.coverage > 0.0 && F.strength > 0.0)
	{
		std::vector<double> cdf(tris.size());
		double acc = 0.0;
		for (size_t i = 0; i < tris.size(); i++) { acc += tris[i].area; cdf[i] = acc; }
		const double lMin = std::max(std::min(p.lengthMin, p.lengthMax), 1e-3), lMax = std::max(p.lengthMin, p.lengthMax);
		const double wMin = std::max(std::min(p.widthMin, p.widthMax), 1e-3), wMax = std::max(p.widthMin, p.widthMax);
		const V up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
		const double kMaxStrokes = 60000.0;
		double want = 0.0;
		for (int k = 0; k < coats; k++)
		{
			const double sc = std::pow(0.88, k);
			want += p.coverage * totalArea / (0.5 * (lMin + lMax) * 0.5 * (wMin + wMax) * 0.88 * sc * sc);
		}
		const double thin = want > kMaxStrokes ? kMaxStrokes / want : 1.0;
		double maxReach = 0.0;
		for (int k = 0; k < coats; k++)
		{
			const double sc = std::pow(0.88, k);
			const int count = (int)std::ceil(thin * p.coverage * totalArea
				/ (0.5 * (lMin + lMax) * 0.5 * (wMin + wMax) * 0.88 * sc * sc));
			for (int i = 0; i < count; i++)
			{
				const uint64_t key = ((uint64_t)k << 20) + (uint64_t)i;
				const double pick = Random01(seed, key, 0) * acc;
				const size_t ti = std::min((size_t)(std::lower_bound(cdf.begin(), cdf.end(), pick) - cdf.begin()), tris.size() - 1);
				const Tri& T = tris[ti];
				double r1 = Random01(seed, key, 1), r2 = Random01(seed, key, 2);
				if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
				Stroke s;
				s.c = T.a + (T.b - T.a) * r1 + (T.c - T.a) * r2;
				s.n = T.n;
				s.a = 0.5 * sc * Lerp(lMin, lMax, Random01(seed, key, 3));
				s.b = 0.5 * sc * Lerp(wMin, wMax, Random01(seed, key, 4));
				V h = m::la::cross(up, s.n);
				if (m::la::length(h) < 0.25)
				{
					                                                                         
					const V x = V(1, 0, 0) - s.n * s.n.x;
					h = m::la::length(x) > 0.25 ? x : AnyPerp(s.n);
				}
				h = Unit(h, AnyPerp(s.n));
				const double kPi = 3.14159265358979323846;
				double ang = p.angle * kPi / 180.0
					+ (Random01(seed, key, 5) * 2.0 - 1.0) * ClampD(p.angleRandom, 0.0, 1.0) * 0.5 * kPi;
				if (Random01(seed, key, 6) < p.crossChance)
					ang += 0.5 * kPi;
				s.du = Rotate(h, s.n, ang);
				s.dv = m::la::cross(s.n, s.du);
				s.level = F.strength * (0.15 + 0.85 * Random01(seed, key, 7));
				s.tiltK = p.tilt * F.strength * 0.5 * (Random01(seed, key, 8) * 2.0 - 1.0);
				s.curveK = ClampD(p.curve, 0.0, 1.0) * 0.6 * (Random01(seed, key, 9) * 2.0 - 1.0);
				s.ridgeK = 0.5 + 0.5 * Random01(seed, key, 10);
				s.taperK = 0.6 * Random01(seed, key, 17);
				s.sideK = Random01(seed, key, 18) < 0.5 ? -1.0 : 1.0;
				s.offJag = NoiseOffset(seed, key, 11);
				s.offStreak = NoiseOffset(seed, key, 14);
				s.offRidge = s.offJag + V(37.1, 11.9, 3.3);
				s.reach = std::sqrt(s.a * s.a + s.b * s.b) + std::abs(s.curveK) * s.a
					+ p.jagged * s.b * 0.3 + 1.1 * std::max(p.ridgeWidth, 1e-3);
				maxReach = std::max(maxReach, s.reach);
				F.strokes.push_back(s);
			}
		}
		                                                                          
		                                                            
		F.cell = std::max(maxReach, 1e-3);
		for (int32_t si = 0; si < (int32_t)F.strokes.size(); si++)
		{
			const Stroke& s = F.strokes[(size_t)si];
			const int64_t x0 = (int64_t)std::floor((s.c.x - s.reach) / F.cell), x1 = (int64_t)std::floor((s.c.x + s.reach) / F.cell);
			const int64_t y0 = (int64_t)std::floor((s.c.y - s.reach) / F.cell), y1 = (int64_t)std::floor((s.c.y + s.reach) / F.cell);
			const int64_t z0 = (int64_t)std::floor((s.c.z - s.reach) / F.cell), z1 = (int64_t)std::floor((s.c.z + s.reach) / F.cell);
			for (int64_t z = z0; z <= z1; z++)
				for (int64_t y = y0; y <= y1; y++)
					for (int64_t x = x0; x <= x1; x++)
						F.grid[CellKey(x, y, z)].push_back(si);
		}
	}

	                                                                               
	std::vector<double> disp(nv, 0.0);
	const double sinkLevel = ClampD(p.sink, 0.0, 1.0) * F.strength;
	const double cap = std::max(p.thicknessCap, 0.0);
	if (cap > 0.0)
		needBvh();
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t i)
	{
		if (geo[(size_t)i] != (int32_t)i || weight[(size_t)i] <= 0.0)
			return;
		const V n = Unit(nrm[(size_t)i], V(0.0));
		if (m::la::dot(n, n) == 0.0)
			return;
		const V pos = P((size_t)i);
		double w = weight[(size_t)i];
		if (maskOn)
			w *= 1.0 - mask->Amount() * (1.0 - ClampD(mask->Weight(pos), 0.0, 1.0));
		if (zone != nullptr && zone->Active())
			w *= ClampD(zone->Weight(pos), 0.0, 1.0);
		if (w <= 0.0)
			return;
		double d = (F.Height(pos, n) - sinkLevel) * w;
		if (cap > 0.0 && d < 0.0)
		{
			                                                                  
			const double eps = 1e-3;
			double hit = 0.0;
			if (bvh.Raycast(pos - n * eps, -n, -d / cap, &hit))
				d = std::max(d, -cap * (hit + eps));
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
		if (disp[g] == 0.0)
			continue;
		const V q = moved[g];
		mesh.vertProperties[i * np] = q.x;
		mesh.vertProperties[i * np + 1] = q.y;
		mesh.vertProperties[i * np + 2] = q.z;
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0)
		return false;
    if (affectedFaces) {
        affectedFaces->assign((size_t)in.view.npolys, 0);
        for (size_t t = 0; t < nt && t < mesh.faceID.size(); ++t) {
            const size_t face = mesh.faceID[t];
            if (face >= affectedFaces->size()) continue;
            for (int k = 0; k < 3; ++k)
                if (std::abs(disp[(size_t)geo[mesh.triVerts[t*3+k]]]) > 1e-8)
                    (*affectedFaces)[face] = 1;
        }
    }
	out = std::move(result);
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
