// формируем вмятины и разрывы металла от воздействия
// ProjectDamage metal deform — GPL-3.0-or-later; see LICENSE.
//
// Recipe: metal-deform-v1. Dented, buckled and crumpled sheet metal displaced
// into the source mesh itself, driven by the objects of a target collection
// (the impactors):
//   1. refine near the impactors: longest-edge bisection of the triangles in
//      the zone only, so a large panel is not subdivided everywhere; UVs and
//      every other property channel are interpolated, faceID and the run
//      original id are kept, seams stay welded through the merge table.
//      Pass 1 refines around the impactors; pass 2 refines out to the radius
//      of the dents pass 1 found, with the edge length sized to the budget;
//   2. contacts: every refined vertex casts a ray along its smooth normal
//      each way into the impactors. Inside a closed impactor it must leave
//      along the shorter way: that distance is its imprint depth. Close to
//      one, it gets a proximity dent that fades with the gap.
//      - Sheet (the wall behind is thinner than sheetThickness): only the
//        face the impactor meets first is pushed; the other face takes the
//        same push, probed at the point straight through the wall, so both
//        faces move together and the panel keeps its thickness.
//      - Solid: the push follows the impactor's travel (the mean push of all
//        its contacts), so faces around a hit edge or over a curved surface
//        move together; points on a sharp edge get the imprint along it too.
//      - Faces at a steep angle to an impactor's travel (a panel's rim strip)
//        and points on sharp source edges of sheets are not pushed by
//        themselves: they follow their neighbours through the kernels;
//   3. dent field: contacts are thinned to a grid of samples and grouped into
//      dents (connected clusters). Each sample spreads a kernel of radius
//      falloffMin + falloff x depth, a mix of a soft bowl and a cone (the
//      cone's envelope over a contact area gives the creased plastic hinge).
//      Within a dent kernels combine with a very sharp power mean (seamless,
//      never above the strongest kernel); dents combine with a smooth max,
//      which rounds the ridge where two dents meet. A solid vertex keeps its
//      exact imprint on top; sheet samples are lifted by the kernel's loss to
//      the next sample, so the envelope clears the impactor between them.
//      A raised ring sits just outside each dent. A sheet's dent reaches
//      through its thickness; a solid's fades with depth and is capped at
//      thicknessCap x the wall;
//   4. surface detail per dent, blended between the two strongest dents:
//      radial buckles on a stadium chart around the dent (folds at jittered
//      slots around its perimeter, each with its own length, bend, sign and
//      sometimes a fork), crumpled facets where the metal went deepest, and a
//      fine hammered grain. All of it is evaluated in the dent's plane, so
//      both faces of a sheet get the same detail. Under the impactor the
//      detail may only push deeper;
//   5. displacement along the blended push direction, x mask weight, with
//      Avoid Selection kept flat. Three marks per vertex (dent, crease,
//      contact) are kept for the output point attributes.
// Noise and buckle slots are keyed by the quantised dent centre, so adding a
// second impactor never reshuffles the first dent's buckles.
//
// Recipe metal-crush-v1 (style 1, Crush): crash damage around proximity
// empties. Each empty's display sphere is a crumple zone, its contact point
// the surface point nearest the empty (or hit along its +Z), and its strength
// fades with the empty's distance from the surface:
//   1. the zone is refined like pass 2 above;
//   2. the shove: the metal at the front moves in along the crush direction,
//      the zone behind it is squeezed, the metal past it stays put; the zone
//      fades out sideways over a hinge band, and the front is uneven;
//   3. the length the squeeze took out buckles up: one big chevron buckle
//      (panels tent into a peak) and finer accordion folds across the crush,
//      triangle waves with rounded tips sized so their arc length gives the
//      lost length back; a few sharp crease families in random directions
//      leave flat, angular facets between them on the crushed metal;
//   4. a sheet moves as its mid-surface: both faces evaluate the field at the
//      same mid-surface point and sit half the wall off its deformed normal,
//      so the wall keeps its thickness through every fold. Which way a fold
//      stands out comes from a field (away from the crush axis, up, back
//      toward the empty) that does not depend on which face is asking.
#include "manifold/manifold.h"
#include "metal_deform.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

struct MetalDeformMarks
{
	std::vector<EVec>  pos;
	std::vector<float> dent, crease, contact;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	double cell = 1.0;
	double reach = 1.0;
};

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

double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double e0, double e1, double x)
{
	if (e1 == e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double Noise(const V& q) { return ProjectDamageNoise(q); }
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

int64_t CellOf(double v, double cell) { return (int64_t)std::floor(v / cell); }

                                                                            
                                               
struct Zone
{
	double cell = 1.0;
	std::unordered_set<uint64_t> cells;

	void AddBox(const V& lo, const V& hi)
	{
		const int64_t x0 = CellOf(lo.x, cell), x1 = CellOf(hi.x, cell);
		const int64_t y0 = CellOf(lo.y, cell), y1 = CellOf(hi.y, cell);
		const int64_t z0 = CellOf(lo.z, cell), z1 = CellOf(hi.z, cell);
		for (int64_t z = z0; z <= z1; z++)
			for (int64_t y = y0; y <= y1; y++)
				for (int64_t x = x0; x <= x1; x++)
					cells.insert(CellKey(x, y, z));
	}
	bool Point(const V& p) const
	{
		return cells.count(CellKey(CellOf(p.x, cell), CellOf(p.y, cell), CellOf(p.z, cell))) != 0;
	}
	bool Box(const V& lo, const V& hi) const
	{
		const int64_t x0 = CellOf(lo.x, cell), x1 = CellOf(hi.x, cell);
		const int64_t y0 = CellOf(lo.y, cell), y1 = CellOf(hi.y, cell);
		const int64_t z0 = CellOf(lo.z, cell), z1 = CellOf(hi.z, cell);
		if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 50000)
			return true;                                                               
		for (int64_t z = z0; z <= z1; z++)
			for (int64_t y = y0; y <= y1; y++)
				for (int64_t x = x0; x <= x1; x++)
					if (cells.count(CellKey(x, y, z)))
						return true;
		return false;
	}
};

                                                                   
struct RMesh
{
	size_t np = 3;
	std::vector<double>   prop;
	std::vector<int32_t>  geo;                                                        
	std::vector<uint32_t> tri;
	std::vector<uint32_t> fid;
	std::vector<int32_t>  run;
	std::vector<uint32_t> mergeFrom, mergeTo;
	bool hasFid = false;

	size_t NumVert() const { return prop.size() / np; }
	size_t NumTri() const { return tri.size() / 3; }
	V Pos(size_t i) const { return V(prop[i * np], prop[i * np + 1], prop[i * np + 2]); }
};

uint64_t PairKey(uint32_t a, uint32_t b)
{
	const uint32_t lo = std::min(a, b), hi = std::max(a, b);
	return ((uint64_t)lo << 32) | hi;
}

                                                                          
                                                                             
template <typename ZoneFn>
void Refine(RMesh& M, double L, size_t budget, ZoneFn inZone)
{
	const double L2 = L * L;
	for (int pass = 0; pass < 40; pass++)
	{
		const size_t nt = M.NumTri();
		if (nt >= budget)
			break;
		auto len2 = [&](uint32_t a, uint32_t b) {
			const V d = M.Pos((size_t)M.geo[a]) - M.Pos((size_t)M.geo[b]);
			return m::la::dot(d, d);
		};
		auto gkey = [&](uint32_t a, uint32_t b) { return PairKey((uint32_t)M.geo[a], (uint32_t)M.geo[b]); };
		std::unordered_set<uint64_t> marked;
		for (size_t t = 0; t < nt; t++)
		{
			const uint32_t* v = &M.tri[t * 3];
			const V a = M.Pos(v[0]), b = M.Pos(v[1]), c = M.Pos(v[2]);
			bool longEdge = false;
			for (int k = 0; k < 3; k++)
				if (len2(v[k], v[(k + 1) % 3]) > L2) longEdge = true;
			if (!longEdge)
				continue;
			const V lo(std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z }));
			const V hi(std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z }));
			if (!inZone(lo, hi))
				continue;
			for (int k = 0; k < 3; k++)
				if (len2(v[k], v[(k + 1) % 3]) > L2)
					marked.insert(gkey(v[k], v[(k + 1) % 3]));
		}
		if (marked.empty())
			break;
		auto longest = [&](const uint32_t* v) {
			int best = 0;
			double bl = -1.0;
			uint64_t bk = 0;
			for (int k = 0; k < 3; k++)
			{
				const double l = len2(v[k], v[(k + 1) % 3]);
				const uint64_t key = gkey(v[k], v[(k + 1) % 3]);
				if (l > bl || (l == bl && key < bk)) { bl = l; best = k; bk = key; }
			}
			return best;
		};
		                                                                       
		for (bool changed = true; changed;)
		{
			changed = false;
			for (size_t t = 0; t < nt; t++)
			{
				const uint32_t* v = &M.tri[t * 3];
				bool any = false;
				for (int k = 0; k < 3 && !any; k++)
					any = marked.count(gkey(v[k], v[(k + 1) % 3])) != 0;
				if (!any)
					continue;
				const int k = longest(v);
				if (marked.insert(gkey(v[k], v[(k + 1) % 3])).second)
					changed = true;
			}
		}
		if (nt + 2 * marked.size() > budget)
			break;                                                  
		std::unordered_map<uint64_t, uint32_t> propMid, geoMid;
		auto mid = [&](uint32_t a, uint32_t b) -> uint32_t {
			const uint64_t pk = PairKey(a, b);
			const auto it = propMid.find(pk);
			if (it != propMid.end())
				return it->second;
			const uint32_t idx = (uint32_t)M.NumVert();
			for (size_t c = 0; c < M.np; c++)
				M.prop.push_back(0.5 * (M.prop[a * M.np + c] + M.prop[b * M.np + c]));
			const uint64_t gk = gkey(a, b);
			const auto gt = geoMid.find(gk);
			if (gt == geoMid.end())
			{
				geoMid.emplace(gk, idx);
				M.geo.push_back((int32_t)idx);
			}
			else
			{
				const uint32_t canon = gt->second;
				for (size_t c = 0; c < 3; c++)
					M.prop[idx * M.np + c] = M.prop[canon * M.np + c];
				M.geo.push_back((int32_t)canon);
				M.mergeFrom.push_back(idx);
				M.mergeTo.push_back(canon);
			}
			propMid.emplace(pk, idx);
			return idx;
		};
		std::vector<uint32_t> nTri;
		std::vector<uint32_t> nFid;
		std::vector<int32_t> nRun;
		nTri.reserve(M.tri.size() * 2);
		auto emit = [&](uint32_t a, uint32_t b, uint32_t c, size_t parent) {
			nTri.push_back(a); nTri.push_back(b); nTri.push_back(c);
			if (M.hasFid) nFid.push_back(M.fid[parent]);
			nRun.push_back(M.run[parent]);
		};
		for (size_t t = 0; t < nt; t++)
		{
			const uint32_t v[3] = { M.tri[t * 3], M.tri[t * 3 + 1], M.tri[t * 3 + 2] };
			bool mk[3];
			bool any = false;
			for (int k = 0; k < 3; k++) { mk[k] = marked.count(gkey(v[k], v[(k + 1) % 3])) != 0; any = any || mk[k]; }
			if (!any) { emit(v[0], v[1], v[2], t); continue; }
			const int k = longest(v);
			const uint32_t a = v[k], b = v[(k + 1) % 3], c = v[(k + 2) % 3];
			const bool splitBC = mk[(k + 1) % 3], splitCA = mk[(k + 2) % 3];
			const uint32_t mab = mid(a, b);
			if (splitCA)
			{
				const uint32_t mca = mid(c, a);
				emit(a, mab, mca, t);
				emit(mab, c, mca, t);
			}
			else
				emit(a, mab, c, t);
			if (splitBC)
			{
				const uint32_t mbc = mid(b, c);
				emit(mab, b, mbc, t);
				emit(mab, mbc, c, t);
			}
			else
				emit(mab, b, c, t);
		}
		M.tri.swap(nTri);
		M.fid.swap(nFid);
		M.run.swap(nRun);
	}
}

struct Impactors
{
	std::vector<V> pts;
	std::vector<ProjectDamagePoly> polys;
	std::vector<V> triN;
	std::vector<int32_t> owner;
	std::vector<double> strength;
	ProjectDamageBVH bvh;
	V lo = V(1e300), hi = V(-1e300);
	double maxStrength = 0.0;

	                                                                      
	                                                
	bool Probe(const V& org, const V& dir, double maxD, double& dist, bool& exiting, int32_t& who) const
	{
		int32_t tri = -1;
		if (!bvh.Raycast(org, dir, maxD, &dist, &tri) || tri < 0)
			return false;
		exiting = m::la::dot(triN[(size_t)tri], dir) > 0.0;
		who = owner[(size_t)tri];
		return true;
	}
};

struct Contact
{
	double d0 = 0.0;                   
	V dir = V(0.0);                        
	double touch = 0.0;                                                         
	int32_t who = -1;                    
	bool sheet = false;                                
};

struct Sample
{
	V c, dir;
	double d0 = 0.0, R = 1.0, off = 0.0, lfade = 1.0, side = 1.0, rinf = 1.0, tip = 0.0;
	bool sheet = false;
	int32_t cluster = -1;
	int32_t vert = -1;
};

struct Dent
{
	V center, dir, e1, e2;
	double depth = 0.0, R = 1.0, halfLen = 0.0, rho = 1.0, perim = 1.0;
	uint64_t key = 0;
	V offCrumple, offRough;
};

                                                                           
                                                            
bool LoadRMesh(const m::MeshGL64& gl, RMesh& M)
{
	M.np = gl.numProp;
	if (M.np < 3 || gl.NumVert() == 0 || gl.NumTri() == 0)
		return false;
	M.prop = gl.vertProperties;
	M.tri.assign(gl.triVerts.begin(), gl.triVerts.end());
	M.hasFid = gl.faceID.size() == gl.NumTri();
	if (M.hasFid) M.fid.assign(gl.faceID.begin(), gl.faceID.end());
	M.run.assign(gl.NumTri(), 0);
	if (gl.runIndex.size() >= 2)
		for (size_t r = 0; r + 1 < gl.runIndex.size(); r++)
			for (size_t t = gl.runIndex[r] / 3; t < gl.runIndex[r + 1] / 3 && t < gl.NumTri(); t++)
				M.run[t] = (int32_t)r;
	M.geo.resize(gl.NumVert());
	for (size_t i = 0; i < M.geo.size(); i++) M.geo[i] = (int32_t)i;
	for (size_t k = 0; k < gl.mergeFromVert.size() && k < gl.mergeToVert.size(); k++)
	{
		M.geo[gl.mergeFromVert[k]] = (int32_t)gl.mergeToVert[k];
		M.mergeFrom.push_back((uint32_t)gl.mergeFromVert[k]);
		M.mergeTo.push_back((uint32_t)gl.mergeToVert[k]);
	}
	return true;
}

                                                                            
                                                                           
bool FinishDeform(RMesh& M, const m::MeshGL64& gl, const std::vector<V>& delta,
	const std::vector<float>& mDent, const std::vector<float>& mCrease, const std::vector<float>& mContact,
	double edge, m::Manifold& out, std::shared_ptr<const MetalDeformMarks>& marksOut)
{
	const size_t nv = M.NumVert();
	bool any = false;
	for (size_t i = 0; i < nv; i++)
		if (M.geo[i] == (int32_t)i && (delta[i].x != 0.0 || delta[i].y != 0.0 || delta[i].z != 0.0))
			any = true;
	if (!any)
		return false;
	std::vector<V> moved(nv);
	for (size_t i = 0; i < nv; i++)
		if (M.geo[i] == (int32_t)i)
			moved[i] = M.Pos(i) + delta[i];
	auto marks = std::make_shared<MetalDeformMarks>();
	marks->cell = std::max(2.0 * edge, 1e-3);
	marks->reach = 1.5 * edge;
	for (size_t i = 0; i < nv; i++)
	{
		const size_t g = (size_t)M.geo[i];
		for (size_t c = 0; c < 3; c++)
			M.prop[i * M.np + c] = c == 0 ? moved[g].x : (c == 1 ? moved[g].y : moved[g].z);
	}
	for (size_t i = 0; i < nv; i++)
	{
		if (M.geo[i] != (int32_t)i)
			continue;
		if (mDent[i] == 0.0f && mCrease[i] == 0.0f && mContact[i] == 0.0f)
			continue;
		const V q = M.Pos(i);
		const int32_t idx = (int32_t)marks->pos.size();
		marks->pos.push_back(q);
		marks->dent.push_back(mDent[i]);
		marks->crease.push_back(mCrease[i]);
		marks->contact.push_back(mContact[i]);
		marks->grid[CellKey(CellOf(q.x, marks->cell), CellOf(q.y, marks->cell), CellOf(q.z, marks->cell))].push_back(idx);
	}

	                                                                                          
	m::MeshGL64 mesh;
	mesh.numProp = (uint32_t)M.np;
	mesh.vertProperties = std::move(M.prop);
	mesh.tolerance = gl.tolerance;
	                                                        
	const size_t runs = std::max<size_t>(gl.runOriginalID.size(), 1);
	std::vector<std::vector<size_t>> byRun(runs);
	for (size_t t = 0; t < M.NumTri(); t++)
		byRun[std::min((size_t)std::max(M.run[t], 0), runs - 1)].push_back(t);
	mesh.triVerts.reserve(M.tri.size());
	if (M.hasFid) mesh.faceID.reserve(M.NumTri());
	if (!gl.runOriginalID.empty())
	{
		mesh.runIndex.push_back(0);
		mesh.runOriginalID = gl.runOriginalID;
		mesh.runTransform = gl.runTransform;
	}
	for (size_t r = 0; r < runs; r++)
	{
		for (const size_t t : byRun[r])
		{
			for (int k = 0; k < 3; k++) mesh.triVerts.push_back(M.tri[t * 3 + k]);
			if (M.hasFid) mesh.faceID.push_back(M.fid[t]);
		}
		if (!gl.runOriginalID.empty())
			mesh.runIndex.push_back((uint64_t)mesh.triVerts.size());
	}
	mesh.mergeFromVert.assign(M.mergeFrom.begin(), M.mergeFrom.end());
	mesh.mergeToVert.assign(M.mergeTo.begin(), M.mergeTo.end());
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0)
		return false;
	out = std::move(result);
	marksOut = std::move(marks);
	return true;
}


                                                                                     

                                                                                    
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) return a;
	const V bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
	const V cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

                                                               
void VertexNormals(const RMesh& M, std::vector<V>& nrm)
{
	const size_t nv = M.NumVert(), nt = M.NumTri();
	nrm.assign(nv, V(0.0));
	for (size_t t = 0; t < nt; t++)
	{
		const size_t id[3] = { M.tri[t * 3], M.tri[t * 3 + 1], M.tri[t * 3 + 2] };
		const V pt[3] = { M.Pos(id[0]), M.Pos(id[1]), M.Pos(id[2]) };
		const V fn = Unit(m::la::cross(pt[1] - pt[0], pt[2] - pt[0]), V(0.0));
		for (int k = 0; k < 3; k++)
		{
			const V e0 = Unit(pt[(k + 1) % 3] - pt[k], V(0.0));
			const V e1 = Unit(pt[(k + 2) % 3] - pt[k], V(0.0));
			const double ang = std::acos(ClampD(m::la::dot(e0, e1), -1.0, 1.0));
			nrm[(size_t)M.geo[id[k]]] += fn * ang;
		}
	}
	for (V& n : nrm) n = Unit(n, V(0.0));
}

                                            
constexpr int kFacetWaves = 4;
constexpr double kBigFold = 2.5;                                             
struct FacetWave { V d, off; double scale = 1.0, phase = 0.0, amp = 1.0; };

                                           
struct Crush
{
	V O, P, dir, e1, e2;                                                                   
	V up;                                                                                    
	m::mat3 inv;                                                               
	double D = 0.0;                                           
	double Lz = 1.0;                                               
	double R = 1.0;                                
	double Rmax = 1.0;                                
	double I = 1.0;                                              
	V offSkew, offFold, offAmp, offFacet;
	V chevAxis;                                                                           
	double chevAt = 0.0, chevSlope = 0.0, chevPhase = 0.0;
	std::array<FacetWave, kFacetWaves> waves;
};

bool ApplyMetalCrush(const CoreInput& in, const MetalDeformParams& p, Int32 mainSeed,
	const IFieldMask* mask, const m::Manifold& src, m::Manifold& out,
	std::shared_ptr<const MetalDeformMarks>& marksOut)
{
	if (p.empties.empty())
		return false;
	if (src.Status() != m::Manifold::Error::NoError || src.NumTri() == 0)
		return false;
	const uint64_t seed = 0x4d4554414c435253ULL                
		^ ((uint64_t)(uint32_t)mainSeed * 0x9e3779b97f4a7c15ULL)
		^ ((uint64_t)(uint32_t)p.seed * 0xc2b2ae3d27d4eb4fULL);

	m::MeshGL64 gl = src.GetMeshGL64();
	RMesh M;
	if (!LoadRMesh(gl, M))
		return false;

	                                                               
	std::vector<V> sPts;
	std::vector<ProjectDamagePoly> sPolys;
	{
		std::vector<int32_t> remap(M.NumVert(), -1);
		for (size_t i = 0; i < M.NumVert(); i++)
			if (M.geo[i] == (int32_t)i) { remap[i] = (int32_t)sPts.size(); sPts.push_back(M.Pos(i)); }
		for (size_t t = 0; t < M.NumTri(); t++)
		{
			const int32_t a = remap[(size_t)M.geo[M.tri[t * 3]]], b = remap[(size_t)M.geo[M.tri[t * 3 + 1]]],
				c = remap[(size_t)M.geo[M.tri[t * 3 + 2]]];
			sPolys.push_back({ a, b, c, c });
		}
	}
	ProjectDamageBVH srcBvh;
	srcBvh.Build(MeshView{ sPts.data(), (int32_t)sPts.size(), sPolys.data(), (int32_t)sPolys.size() });
	V bLo(1e300), bHi(-1e300);
	for (const V& q : sPts)
	{
		bLo = V(std::min(bLo.x, q.x), std::min(bLo.y, q.y), std::min(bLo.z, q.z));
		bHi = V(std::max(bHi.x, q.x), std::max(bHi.y, q.y), std::max(bHi.z, q.z));
	}
	const double diag = m::la::length(bHi - bLo);

	                                                                                     
	const double depthK = ClampD(p.crushDepth, 0.0, 0.6);
	std::vector<Crush> crushes;
	for (size_t ei = 0; ei < p.empties.size(); ei++)
	{
		const MetalCrushEmpty& E = p.empties[ei];
		bool finite = std::isfinite(E.size) && std::isfinite(E.strength);
		for (double q : E.rel) finite = finite && std::isfinite(q);
		if (!finite || E.strength <= 0.0 || E.size <= 0.0)
			continue;
		Crush C;
		C.O = V(E.rel[0], E.rel[1], E.rel[2]);
		const V X(E.rel[3], E.rel[4], E.rel[5]), Y(E.rel[6], E.rel[7], E.rel[8]), Z(E.rel[9], E.rel[10], E.rel[11]);
		const m::mat3 Ms(X * E.size, Y * E.size, Z * E.size);
		if (!(std::abs(m::la::determinant(Ms)) > 1e-12))
			continue;
		C.inv = m::la::inverse(Ms);
		                                    
		bool found = false;
		if (p.crushAim == 1)
		{
			const V shot = Unit(Z, V(0, 0, 1));
			double d = 0.0;
			if (srcBvh.Raycast(C.O, shot, 4.0 * diag + 4.0 * E.size, &d))
			{
				C.P = C.O + shot * d;
				C.dir = shot;
				found = true;
			}
		}
		double gap = 0.0;
		if (!found)
		{
			double best = 1e300;
			V bestN(0.0);
			for (const ProjectDamagePoly& q : sPolys)
			{
				const V a = sPts[(size_t)q.a], b = sPts[(size_t)q.b], c = sPts[(size_t)q.c];
				const V cp = ClosestOnTri(C.O, a, b, c);
				const double d2 = m::la::dot(cp - C.O, cp - C.O);
				if (d2 < best) { best = d2; C.P = cp; bestN = m::la::cross(b - a, c - a); }
			}
			if (best >= 1e300)
				continue;
			const V nP = Unit(bestN, V(0, 0, 1));
			C.dir = Unit(C.P - C.O, -nP);
			                                                               
			if (m::la::dot(C.dir, nP) > 0.0)
				C.dir = -nP;
			else
				gap = std::sqrt(best);
		}
		else
			gap = m::la::length(C.P - C.O);
		C.e1 = X - C.dir * m::la::dot(X, C.dir);
		if (m::la::length(C.e1) < 0.2 * m::la::length(X))
			C.e1 = m::la::cross(Y, C.dir);
		C.e1 = Unit(C.e1, AnyPerp(C.dir));
		C.e2 = m::la::cross(C.dir, C.e1);
		C.up = Z - C.dir * m::la::dot(Z, C.dir);
		if (m::la::length(C.up) < 0.3 * m::la::length(Z))
			C.up = Y - C.dir * m::la::dot(Y, C.dir);
		C.up = Unit(C.up, AnyPerp(C.dir));
		                                                         
		const double ext = 1.0 / std::max(m::la::length(C.inv * C.dir), 1e-12);
		const double r1 = 1.0 / std::max(m::la::length(C.inv * C.e1), 1e-12);
		const double r2 = 1.0 / std::max(m::la::length(C.inv * C.e2), 1e-12);
		C.R = 0.5 * (r1 + r2);
		C.Rmax = std::max(r1, r2);
		C.Lz = std::max(p.crushLength, 0.05) * ext;
		C.I = std::min(E.strength, 4.0);
		if (p.crushProximity > 0.0)
			C.I *= 1.0 - Smooth(0.25 * p.crushProximity * ext, p.crushProximity * ext, gap);
		if (C.I <= 0.02)
			continue;
		C.D = std::min(depthK * C.Lz * C.I, 0.6 * C.Lz);
		if (C.D <= 0.0)
			continue;
		const uint64_t key = (uint64_t)ei + 1;
		C.offSkew = NoiseOffset(seed, key, 1);
		C.offFold = NoiseOffset(seed, key, 4);
		C.offAmp = NoiseOffset(seed, key, 7);
		C.offFacet = NoiseOffset(seed, key, 10);
		{
			const double a = kPi * Random01(seed, key, 30);
			C.chevAxis = C.e1 * std::cos(a) + C.e2 * std::sin(a);
			C.chevAt = (Random01(seed, key, 31) - 0.5) * C.R;
			C.chevSlope = 0.25 + 0.5 * Random01(seed, key, 32);
			C.chevPhase = Random01(seed, key, 33);
		}
		for (int k = 0; k < kFacetWaves; k++)
		{
			FacetWave& fw = C.waves[(size_t)k];
			const uint64_t wk = key * 131ULL + (uint64_t)k;
			const double z = 2.0 * Random01(seed, wk, 20) - 1.0, a = 2.0 * kPi * Random01(seed, wk, 21);
			const double rz = std::sqrt(std::max(0.0, 1.0 - z * z));
			fw.d = V(rz * std::cos(a), rz * std::sin(a), z);
			fw.scale = 0.7 + 0.9 * Random01(seed, wk, 22);
			fw.phase = Random01(seed, wk, 23);
			fw.amp = (0.5 + 0.5 * Random01(seed, wk, 24)) * 0.8;
			fw.off = NoiseOffset(seed, wk, 25);
		}
		crushes.push_back(C);
	}
	if (crushes.empty())
		return false;

	                                                                                       
	const double lambda = std::max(p.foldSize, 0.05);
	const double det = std::max(p.detail, 0.05);
	double L = std::max(std::max(p.edgeLength, 1e-3), lambda / 16.0) / det;
	const size_t budget = (size_t)std::max(p.maxTris, 1000);
	{
		struct Ball { V c; double r; };
		std::vector<Ball> balls;
		for (const Crush& C : crushes)
		{
			const double half = 0.95 * C.Lz;
			balls.push_back({ C.P + C.dir * (0.45 * C.Lz), std::sqrt(half * half + 1.69 * C.Rmax * C.Rmax) + 2.0 * L });
		}
		auto inZone = [&](const V& lo, const V& hi) {
			for (const Ball& b : balls)
			{
				const V cl(ClampD(b.c.x, lo.x, hi.x), ClampD(b.c.y, lo.y, hi.y), ClampD(b.c.z, lo.z, hi.z));
				const V d = cl - b.c;
				if (m::la::dot(d, d) <= b.r * b.r)
					return true;
			}
			return false;
		};
		Refine(M, 4.0 * L, budget, inZone);
		double zoneArea = 0.0;
		for (size_t t = 0; t < M.NumTri(); t++)
		{
			const V a = M.Pos(M.tri[t * 3]), b = M.Pos(M.tri[t * 3 + 1]), c = M.Pos(M.tri[t * 3 + 2]);
			const V ctr = (a + b + c) / 3.0;
			if (inZone(ctr, ctr))
				zoneArea += 0.5 * m::la::length(m::la::cross(b - a, c - a));
		}
		L = std::max(L, std::sqrt(zoneArea / (0.433 * 0.9 * (double)budget)) * 1.1);
		Refine(M, L, budget, inZone);
	}
	const size_t nv = M.NumVert();
	const size_t nt = M.NumTri();
	std::vector<V> nrm;
	VertexNormals(M, nrm);
	if (in.view.guideField) for (size_t i=0;i<nv;++i) nrm[i]=GuidedPointNormal(in.view,M.Pos(i),nrm[i]);

	                                                                                         
	std::vector<double> weight(nv, 1.0);
	const bool haveView = in.view.npolys > 0 && in.view.pts != nullptr;
	if (p.avoid && in.avoidPoly != nullptr && haveView
		&& std::find(in.avoidPoly->begin(), in.avoidPoly->end(), true) != in.avoidPoly->end())
	{
		ProjectDamageBVH viewBvh;
		viewBvh.Build(in.view);
		const std::vector<bool>& avoid = *in.avoidPoly;
		std::vector<uint8_t> avoidTri(nt, 0);
		ProjectDamageParallelFor(0, (int64_t)nt, [&](int64_t t)
		{
			const V a = M.Pos(M.tri[t * 3]), b = M.Pos(M.tri[t * 3 + 1]), c = M.Pos(M.tri[t * 3 + 2]);
			const V fn = Unit(m::la::cross(b - a, c - a), V(0, 0, 1));
			const V ctr = (a + b + c) / 3.0;
			const double eps = std::max(1e-3, L * 0.05);
			double hit = 0.0;
			int32_t poly = -1;
			if (viewBvh.Raycast(ctr + fn * eps, -fn, 2.0 * eps, &hit, &poly)
				&& poly >= 0 && (size_t)poly < avoid.size() && avoid[(size_t)poly])
				avoidTri[(size_t)t] = 1;
		});
		                                                                         
		for (size_t t = 0; t < nt; t++)
			if (avoidTri[t])
				for (int k = 0; k < 3; k++)
					weight[(size_t)M.geo[M.tri[t * 3 + k]]] = 0.0;
	}
	const bool maskOn = mask != nullptr && mask->Active() && mask->Amount() > 0.0;

	                                                                                           
	const double sk = ClampD(p.crushSkew, 0.0, 1.0);
	const double hinge = ClampD(p.crushHinge, 0.03, 1.0);
	const double foldK = std::max(p.fold, 0.0);
	const double wander = ClampD(p.foldWander, 0.0, 2.0);
	const double lift = ClampD(p.foldLift, -1.0, 1.0);
	                                                                         
	                                                       
	const double tipMin = 2.0 * kPi * 1.5 * L / lambda;
	const double sharpK = std::min(1.0 - 0.5 * std::pow(1.0 - ClampD(p.foldSharp, 0.0, 1.0), 1.5), 1.0 - 0.5 * tipMin * tipMin);
	const double asinK = std::asin(std::max(sharpK, 1e-6));
	const double crumK = std::max(p.crushCrumple, 0.0);
	const double facetS = std::max(p.crushFacet, 1e-3);
	struct Marks { double dent = 0.0, crease = 0.0, contact = 0.0; };

	                                                                       
	                                                                         
	                                                        
	auto field = [&](const V& x, const V& n, Marks* mk) -> V {
		V bulk(0.0), detail(0.0);
		for (const Crush& C : crushes)
		{
			const V q = x - C.P;
			const double s = m::la::dot(q, C.dir);
			if (s > 1.4 * C.Lz)
				continue;
			const V lat = q - C.dir * s;
			const double r = m::la::length(C.inv * lat);
			if (r > 1.35)
				continue;
			const double lx = m::la::dot(lat, C.e1) / C.R, ly = m::la::dot(lat, C.e2) / C.R;
			                                                                
			                                                  
			const double n1 = Fbm(V(lx * 1.3, ly * 1.3, 0.0) + C.offSkew, 3);
			const double n2 = Fbm(V(lx * 1.1, ly * 1.1, 7.3) + C.offSkew, 2);
			const double n3 = Fbm(V(lx * 0.9, ly * 0.9, 13.1) + C.offSkew, 2);
			const double s2 = s + sk * 0.3 * C.Lz * n1;
			const double rr = r * (1.0 + sk * 0.3 * n2);
			const double wl = 1.0 - Smooth(1.0 - hinge, 1.0, rr);
			if (wl <= 0.0)
				continue;
			const double t = s2 / C.Lz;
			const double prof = 1.0 - Smooth(0.0, 1.0, t);
			const double dprof = t > 0.0 && t < 1.0 ? 6.0 * t * (1.0 - t) / C.Lz : 0.0;
			                                    
			const double Dl = std::min(C.D * (1.0 + sk * 0.5 * n3) * (0.7 + 0.3 * (1.0 - std::min(rr * rr, 1.0))), 0.6 * C.Lz);
			const double W = wl * prof;
			                                                                
			                                              
			bulk += C.dir * (Dl * W);
			                                                                 
			                                          
			const double eps = std::min(Dl * wl * dprof, 0.92);
			const double nd = m::la::dot(n, C.dir);
			const double epsS = eps * (1.0 - nd * nd);
			                                                                   
			                                                                   
			                                                                
			                   
			const V qo = x - C.O;
			const V latO = qo - C.dir * m::la::dot(qo, C.dir);
			const double lo = m::la::length(latO);
			const V g = Unit(Unit(latO, V(0.0)) * Smooth(0.0, 0.3 * C.R, lo) + C.up * 0.7 - C.dir * 0.5, C.up);
			const double sg = m::la::dot(n, g);
			const V v = sg >= 0.0 ? n : -n;
			const double wv = Smooth(0.05, 0.25, std::abs(sg));
			                                                                   
			                                                                
			                                                                 
			                                                                 
			                               
			const double fx = m::la::dot(lat, C.e1) / lambda, fy = m::la::dot(lat, C.e2) / lambda;
			const double warp = wander * 0.8 * lambda * Fbm(V(fx / 1.7, fy / 1.7, 0.0) + C.offFold, 2);
			auto foldWave = [&](double ph) { return std::asin(sharpK * std::sin(2.0 * kPi * ph)) / asinK; };
			auto foldAmp = [&](double e, double lam) { return foldK * 0.25 * lam * std::sqrt(std::max(0.0, 1.0 - (1.0 - e) * (1.0 - e))); };
			const double lamB = kBigFold * lambda;
			const double chev = (m::la::dot(lat, C.chevAxis) - C.chevAt) * C.chevSlope;
			const double phiB = (s2 + std::abs(chev) + 0.5 * warp) / lamB + C.chevPhase;
			const double triB = foldWave(phiB);
			const double ampB = foldAmp(0.6 * epsS, lamB);
			const double phi = (s2 + warp) / lambda;
			const double tri = foldWave(phi);
			const double amp = foldAmp(0.4 * epsS, lambda);
			const double mod = 0.15 + 1.3 * Smooth(0.25, 0.75, Noise(V(fx / 2.2, fy / 2.2, phi * 0.37) + C.offAmp));
			detail += v * ((ampB * (triB + lift) + amp * mod * (tri + 0.5 * lift)) * wv);
			                                                               
			                                                              
			                                                              
			                                                   
			double ridge = 0.0;
			const double crushed = std::max(std::pow(W, 0.6), 4.0 * wl * (1.0 - wl) * prof);
			if (crumK > 0.0 && crushed > 0.0)
			{
				const double af = crumK * 0.15 * Dl * crushed * (0.4 + 0.6 * std::max(std::min(1.0, 4.0 * epsS), std::abs(nd)));
				double h = 0.0;
				for (int k = 0; k < kFacetWaves; k++)
				{
					const FacetWave& fw = C.waves[(size_t)k];
					const double ph = m::la::dot(x, fw.d) / (facetS * fw.scale) + fw.phase;
					const double tw = std::asin(0.97 * std::sin(2.0 * kPi * ph)) / std::asin(0.97);
					const double fm = Smooth(0.3, 0.7, Noise(x / (2.2 * facetS * fw.scale) + fw.off));
					h += fw.amp * tw * fm;
					ridge = std::max(ridge, Smooth(0.8, 0.98, std::abs(tw)) * fm);
				}
				detail += v * (0.55 * af * h * wv);
				ridge *= std::min(1.0, af / std::max(0.01 * C.Lz, 1e-9));
			}
			if (p.roughness > 0.0)
				detail += v * (p.roughness * Smooth(0.0, 0.2, W) * Fbm(x / std::max(1.0, 3.0 * L) + C.offFacet, 2) * wv);
			if (mk != nullptr)
			{
				mk->dent = std::max(mk->dent, W * std::min(C.I, 1.0));
				const double tip = std::max(Smooth(0.7, 0.97, std::abs(triB)) * std::min(1.0, ampB / (0.06 * lamB)),
					Smooth(0.7, 0.97, std::abs(tri)) * std::min(1.0, amp * mod / (0.06 * lambda)));
				mk->crease = std::max(mk->crease, std::max(tip, ridge) * wv);
				mk->contact = std::max(mk->contact, (1.0 - Smooth(0.0, 0.12 * C.Lz, s2)) * wl
					* Smooth(0.2, 0.6, std::abs(nd)) * std::min(C.I, 1.0));
			}
		}
		return bulk + detail;
	};

	const double sheetT = std::max(p.sheetThickness, 0.0);
	const double hD = std::max(0.5 * L, 1e-3);
	std::vector<V> delta(nv, V(0.0));
	std::vector<float> mDent(nv, 0.0f), mCrease(nv, 0.0f), mContact(nv, 0.0f);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii)
	{
		const size_t i = (size_t)ii;
		if (M.geo[i] != (int32_t)i)
			return;
		const V x = M.Pos(i);
		const V n = nrm[i];
		if (m::la::dot(n, n) == 0.0)
			return;
		Marks mk;
		                                                                    
		                                                             
		                                                                      
		                                                   
		                                                                   
		                                                                 
		double wall = 0.0;
		bool sheet = false;
		V ns = n;
		if (sheetT > 0.0)
		{
			const double eps = std::max(1e-4, 0.01 * L);
			int32_t hp = -1;
			if (srcBvh.Raycast(x - n * eps, -n, 1.5 * sheetT, &wall, &hp) && hp >= 0)
			{
				const ProjectDamagePoly& q = sPolys[(size_t)hp];
				ns = -Unit(m::la::cross(sPts[(size_t)q.b] - sPts[(size_t)q.a], sPts[(size_t)q.c] - sPts[(size_t)q.a]), -n);
				if (m::la::dot(ns, n) > 0.2 && srcBvh.Raycast(x - n * eps, -ns, sheetT, &wall))
				{
					wall += eps * m::la::dot(ns, n);
					sheet = true;
				}
			}
		}
		V moved;
		if (sheet)
		{
			const V n = ns;
			const V mid = x - n * (0.5 * wall);
			const V u0 = field(mid, n, &mk);
			if (mk.dent <= 0.0 && m::la::dot(u0, u0) == 0.0)
				return;
			const V t1 = AnyPerp(n), t2 = m::la::cross(n, t1);
			const V d1 = t1 * (2.0 * hD) + field(mid + t1 * hD, n, nullptr) - field(mid - t1 * hD, n, nullptr);
			const V d2 = t2 * (2.0 * hD) + field(mid + t2 * hD, n, nullptr) - field(mid - t2 * hD, n, nullptr);
			const V nn = Unit(m::la::cross(d1, d2), n);
			moved = mid + u0 + nn * (0.5 * wall);
		}
		else
		{
			const V u0 = field(x, n, &mk);
			if (mk.dent <= 0.0 && m::la::dot(u0, u0) == 0.0)
				return;
			moved = x + u0;
		}
		double w = weight[i];
		if (maskOn)
			w *= 1.0 - mask->Amount() * (1.0 - ClampD(mask->Weight(x), 0.0, 1.0));
		delta[i] = (moved - x) * w;
		mDent[i] = (float)(ClampD(mk.dent, 0.0, 1.0) * w);
		mCrease[i] = (float)(ClampD(mk.crease, 0.0, 1.0) * w);
		mContact[i] = (float)(ClampD(mk.contact, 0.0, 1.0) * w);
	});
	return FinishDeform(M, gl, delta, mDent, mCrease, mContact, L, out, marksOut);
}

}             

// основной вход: собираем геометрию по переданным настройкам
bool ApplyMetalDeform(const CoreInput& in, const MetalDeformParams& p, Int32 mainSeed,
	const IFieldMask* mask, const m::Manifold& src, m::Manifold& out,
	std::shared_ptr<const MetalDeformMarks>& marksOut)
{
	if (!p.enable)
		return false;
	if (p.style == 1)
		return ApplyMetalCrush(in, p, mainSeed, mask, src, out, marksOut);
	if (p.impactors.empty())
		return false;
	if (src.Status() != m::Manifold::Error::NoError || src.NumTri() == 0)
		return false;
	const uint64_t seed = 0x4d4554414c5f4446ULL                
		^ ((uint64_t)(uint32_t)mainSeed * 0x9e3779b97f4a7c15ULL)
		^ ((uint64_t)(uint32_t)p.seed * 0xc2b2ae3d27d4eb4fULL);

	                                                                                 
	Impactors imp;
	for (size_t o = 0; o < p.impactors.size(); o++)
	{
		const MetalDeformImpactor& I = p.impactors[o];
		const double s = std::max(I.strength, 0.0);
		imp.strength.push_back(s);
		if (s <= 0.0 || I.pts.size() < 9 || I.tris.size() < 3)
			continue;
		const int32_t base = (int32_t)imp.pts.size();
		const int32_t npt = (int32_t)(I.pts.size() / 3);
		for (int32_t i = 0; i < npt; i++)
			imp.pts.push_back(V(I.pts[(size_t)i * 3], I.pts[(size_t)i * 3 + 1], I.pts[(size_t)i * 3 + 2]));
		for (size_t t = 0; t + 2 < I.tris.size(); t += 3)
		{
			const int32_t a = I.tris[t], b = I.tris[t + 1], c = I.tris[t + 2];
			if (a < 0 || b < 0 || c < 0 || a >= npt || b >= npt || c >= npt)
				continue;
			const V pa = imp.pts[(size_t)(base + a)], pb = imp.pts[(size_t)(base + b)], pc = imp.pts[(size_t)(base + c)];
			const V cr = m::la::cross(pb - pa, pc - pa);
			if (m::la::length(cr) <= 1e-14)
				continue;
			imp.polys.push_back({ base + a, base + b, base + c, base + c });
			imp.triN.push_back(Unit(cr, V(0, 0, 1)));
			imp.owner.push_back((int32_t)o);
			for (const V& q : { pa, pb, pc })
			{
				imp.lo = V(std::min(imp.lo.x, q.x), std::min(imp.lo.y, q.y), std::min(imp.lo.z, q.z));
				imp.hi = V(std::max(imp.hi.x, q.x), std::max(imp.hi.y, q.y), std::max(imp.hi.z, q.z));
			}
		}
		imp.maxStrength = std::max(imp.maxStrength, s);
	}
	if (imp.polys.empty())
		return false;
	imp.bvh.Build(MeshView{ imp.pts.data(), (int32_t)imp.pts.size(), imp.polys.data(), (int32_t)imp.polys.size() });

	                                                                                   
	m::MeshGL64 gl = src.GetMeshGL64();
	RMesh M;
	if (!LoadRMesh(gl, M))
		return false;

	ProjectDamageBVH viewBvh;
	const bool haveView = in.view.npolys > 0 && in.view.pts != nullptr;
	if (haveView)
		viewBvh.Build(in.view);

	double L = std::max(p.edgeLength, 1e-3) / std::max(p.detail, 0.05);
	const size_t budget = (size_t)std::max(p.maxTris, 1000);
	const double reach = std::max(p.proximity, 0.0) * imp.maxStrength;
	const double rMax = std::max(p.radiusMax, 0.1);
	const double fMin = std::max(p.falloffMin, 0.0);
	const double fK = std::max(p.falloff, 0.0);
	const double sheetT = std::max(p.sheetThickness, 0.0);
	const V ext = imp.hi - imp.lo;
	const double sceneD = m::la::length(ext) + 4.0 * rMax + 10.0;

	                                                                
	std::vector<V> nrm;
	std::vector<double> edgeDot;
	std::vector<Contact> con;
	auto computeNormals = [&]() {
		const size_t nv = M.NumVert(), nt = M.NumTri();
		nrm.assign(nv, V(0.0));
		for (size_t t = 0; t < nt; t++)
		{
			const size_t id[3] = { M.tri[t * 3], M.tri[t * 3 + 1], M.tri[t * 3 + 2] };
			const V pt[3] = { M.Pos(id[0]), M.Pos(id[1]), M.Pos(id[2]) };
			const V fn = Unit(m::la::cross(pt[1] - pt[0], pt[2] - pt[0]), V(0.0));
			for (int k = 0; k < 3; k++)
			{
				const V e0 = Unit(pt[(k + 1) % 3] - pt[k], V(0.0));
				const V e1 = Unit(pt[(k + 2) % 3] - pt[k], V(0.0));
				const double ang = std::acos(ClampD(m::la::dot(e0, e1), -1.0, 1.0));
				nrm[(size_t)M.geo[id[k]]] += fn * ang;
			}
		}
		for (V& n : nrm) n = Unit(n, V(0.0));
		                                                                  
		                                                     
		edgeDot.assign(nv, 1.0);
		for (size_t t = 0; t < nt; t++)
		{
			const size_t id[3] = { M.tri[t * 3], M.tri[t * 3 + 1], M.tri[t * 3 + 2] };
			const V fn = Unit(m::la::cross(M.Pos(id[1]) - M.Pos(id[0]), M.Pos(id[2]) - M.Pos(id[0])), V(0.0));
			if (m::la::dot(fn, fn) == 0.0)
				continue;
			for (int k = 0; k < 3; k++)
			{
				const size_t g = (size_t)M.geo[id[k]];
				edgeDot[g] = std::min(edgeDot[g], m::la::dot(fn, nrm[g]));
			}
		}
	};
	                                                                        
	                                                                            
	auto probeAt = [&](const V& pos, const V& n, bool leadOnly, Contact& C) {
		double du = 0.0, dd = 0.0;
		bool exU = false, exD = false;
		int32_t wu = -1, wd = -1;
		const bool hu = imp.Probe(pos, n, sceneD, du, exU, wu);
		const bool hd = imp.Probe(pos, -n, sceneD, dd, exD, wd);
		if (hu && hd && exU && exD)
		{
			                                      
			const bool inward = dd <= du;
			if (!inward && leadOnly)
				return;
			const double e = inward ? dd : du;
			const double s = imp.strength[(size_t)(inward ? wd : wu)];
			C.d0 = (e + std::max(p.clearance, 0.0)) * std::max(p.depthScale, 0.0) * s;
			C.dir = inward ? -n : n;
			C.touch = 1.0;
			C.who = inward ? wd : wu;
			return;
		}
		if (reach <= 0.0 || p.proximityDepth <= 0.0)
			return;
		double best = 0.0;
		auto near = [&](double gap, int32_t who, const V& dir) {
			const double r = std::max(p.proximity, 0.0) * imp.strength[(size_t)who];
			if (r <= 0.0 || gap >= r)
				return;
			const double f = 1.0 - gap / r;
			const double d = p.proximityDepth * imp.strength[(size_t)who] * f * f;
			if (d > best) { best = d; C.d0 = d; C.dir = dir; C.touch = f; C.who = who; }
		};
		if (hu && !exU) near(du, wu, -n);
		if (hd && !exD && !leadOnly) near(dd, wd, n);
	};
	auto computeContacts = [&](const Zone& near) {
		const size_t nv = M.NumVert();
		con.assign(nv, Contact());
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii)
		{
			const size_t i = (size_t)ii;
			if (M.geo[i] != (int32_t)i)
				return;
			const V n = nrm[i];
			if (m::la::dot(n, n) == 0.0)
				return;
			const V pos = M.Pos(i);
			if (!near.Point(pos))
				return;
			                                                                 
			                                                                  
			                                                 
			if (edgeDot[i] < 0.82)
				return;
			                                                                
			                                                                 
			                                                                
			                                                              
			                                          
			double wall = 1e30;
			if (haveView && sheetT > 0.0)
			{
				const double eps = std::max(1e-3, 0.05 * L);
				double hit = 0.0;
				if (viewBvh.Raycast(pos - n * eps, -n, sheetT + eps, &hit))
					wall = hit + eps;
			}
			const bool sheet = wall <= sheetT;
			Contact own;
			probeAt(pos, n, sheet, own);
			if (sheet)
			{
				Contact far;
				probeAt(pos - n * wall, -n, true, far);
				if (far.d0 > own.d0)
				{
					far.touch = 0.0;                                         
					own = far;
				}
			}
			own.sheet = sheet;
			con[i] = own;
		});
		                                                                     
		                                                                      
		                                                                    
		                                                                
		std::vector<V> approach(imp.strength.size(), V(0.0));
		for (size_t i = 0; i < nv; i++)
			if (con[i].d0 > 0.0 && con[i].who >= 0)
				approach[(size_t)con[i].who] += con[i].dir * con[i].d0;
		for (V& a : approach) a = Unit(a, V(0.0));
		for (size_t i = 0; i < nv; i++)
			if (con[i].d0 > 0.0 && con[i].who >= 0
				&& m::la::dot(con[i].dir, approach[(size_t)con[i].who]) < 0.45)
				con[i] = Contact();
		                                                                    
		                                                                      
		                                                                    
		                                                                    
		                                       
		ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii)
		{
			const size_t i = (size_t)ii;
			if (M.geo[i] != (int32_t)i)
				return;
			Contact& C = con[i];
			const bool edgePt = edgeDot[i] < 0.82;
			if (C.sheet || (C.d0 <= 0.0 && !edgePt))
				return;
			const V pos = M.Pos(i);
			if (!near.Point(pos))
				return;
			if (edgePt && haveView && sheetT > 0.0 && C.d0 <= 0.0)
			{
				                                  
				double hit = 0.0;
				if (viewBvh.Raycast(pos - nrm[i] * 1e-3, -nrm[i], sheetT, &hit))
					return;
			}
			for (size_t o = 0; o < approach.size(); o++)
			{
				if (C.d0 > 0.0 && C.who != (int32_t)o)
					continue;
				const V a = approach[o];
				if (m::la::dot(a, a) == 0.0 || m::la::dot(a, nrm[i]) > -0.3)
					continue;                                             
				double e = 0.0, b = 0.0;
				bool exA = false, exB = false;
				int32_t wa = -1, wb = -1;
				const bool inside = imp.Probe(pos, a, sceneD, e, exA, wa) && exA && wa == (int32_t)o
					&& imp.Probe(pos, -a, sceneD, b, exB, wb) && exB;
				if (inside)
				{
					C.d0 = (e + std::max(p.clearance, 0.0)) * std::max(p.depthScale, 0.0) * imp.strength[o];
					C.dir = a;
					C.touch = 1.0;
					C.who = (int32_t)o;
				}
				else if (C.d0 > 0.0)
					C.dir = a;                                                        
			}
		});
	};

	                                                                                    
	{
		const double margin = reach + 2.0 * L + 0.5 * fMin;
		Zone z;
		z.cell = std::max({ margin, 4.0 * L, std::max({ ext.x, ext.y, ext.z }) / 200.0, 1e-3 });
		for (const ProjectDamagePoly& q : imp.polys)
		{
			const V a = imp.pts[(size_t)q.a], b = imp.pts[(size_t)q.b], c = imp.pts[(size_t)q.c];
			z.AddBox(V(std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z })) - V(margin),
				V(std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z })) + V(margin));
		}
		const double zoneArea = 2.0 * (double)z.cells.size() * z.cell * z.cell;
		const double L1 = std::max(L, std::sqrt(zoneArea / (0.433 * (double)budget)) * 1.25);
		Refine(M, L1, budget, [&](const V& lo, const V& hi) { return z.Box(lo, hi); });
		computeNormals();
		computeContacts(z);
	}

	                                                                     
	const double prof = ClampD(p.profile, 0.0, 1.0);
	                                                                     
	                                                        
	auto kernel = [&](double t, double tip) {
		const double u = 1.0 - t * t;
		const double soft = u * u;
		const double tr = tip > 0.0 ? (std::sqrt(t * t + tip * tip) - tip) / (std::sqrt(1.0 + tip * tip) - tip) : t;
		const double cone = std::pow(std::max(0.0, 1.0 - tr), 1.4);
		return Lerp(soft, cone, prof);
	};

	                                                                                        
	std::vector<Sample> samples;
	double hCell = 1.0;
	auto buildSamples = [&]() {
		samples.clear();
		const size_t nv = M.NumVert();
		                                                                  
		                                                                           
		std::vector<double> ds;
		for (size_t i = 0; i < nv; i++)
			if (con[i].d0 > 0.0) ds.push_back(con[i].d0);
		if (ds.empty())
			return;
		std::nth_element(ds.begin(), ds.begin() + (ptrdiff_t)(ds.size() / 2), ds.end());
		const double rMed = std::min(rMax, fMin + fK * ds[ds.size() / 2]);
		hCell = std::max({ 1.5 * L, std::max(fMin, 0.5) / 4.0, 0.1 * rMed });
		std::unordered_map<uint64_t, int32_t> pick;
		for (size_t i = 0; i < nv; i++)
		{
			if (con[i].d0 <= 0.0)
				continue;
			const V pos = M.Pos(i);
			const uint64_t key = CellKey(CellOf(pos.x, hCell), CellOf(pos.y, hCell), CellOf(pos.z, hCell))
				^ (con[i].dir.x + con[i].dir.y + con[i].dir.z > 0.0 ? 0x8000000000000000ULL : 0ULL);
			auto it = pick.find(key);
			if (it == pick.end())
				pick.emplace(key, (int32_t)i);
			else if (con[i].d0 > con[(size_t)it->second].d0)
				it->second = (int32_t)i;
		}
		std::vector<int32_t> verts;
		verts.reserve(pick.size());
		for (const auto& kv : pick) verts.push_back(kv.second);
		std::sort(verts.begin(), verts.end());
		samples.resize(verts.size());
		ProjectDamageParallelFor(0, (int64_t)verts.size(), [&](int64_t k)
		{
			const size_t i = (size_t)verts[(size_t)k];
			Sample& S = samples[(size_t)k];
			S.vert = (int32_t)i;
			S.c = M.Pos(i);
			S.dir = con[i].dir;
			S.d0 = con[i].d0;
			S.R = std::min(rMax, fMin + fK * S.d0);
			S.R = std::max(S.R, 2.0 * L);
			                                                               
			const V n = nrm[i];
			S.side = m::la::dot(S.dir, n) < 0.0 ? 1.0 : -1.0;
			double wall = 1e30;
			if (haveView)
			{
				double hit = 0.0;
				const double eps = std::max(1e-3, 0.05 * L);
				if (viewBvh.Raycast(S.c - n * eps, -n, sceneD, &hit))
					wall = hit + eps;
			}
			S.sheet = wall <= sheetT;
			                                                                      
			                                                             
			                                            
			S.tip = std::min(0.3, 1.2 * hCell / S.R);
			if (S.sheet)
				S.d0 /= std::max(kernel(std::min(1.1 * hCell / S.R, 0.5), S.tip), 0.5);
			S.off = S.sheet ? 1.25 * wall + 2.0 * L : 0.0;
			S.lfade = std::max(S.R, 2.5 * S.d0);
			const double lat = 1.35 * S.R;
			const double dep = S.off + S.lfade;
			S.rinf = std::sqrt(lat * lat + dep * dep);
		});
	};
	buildSamples();
	if (samples.empty())
		return false;

	                                                                                    
	{
		double maxR = 0.0;
		for (const Sample& S : samples) maxR = std::max(maxR, S.R);
		struct Ball { V c; double r; };
		std::vector<Ball> balls;
		for (const Sample& S : samples)
			balls.push_back({ S.c, 1.4 * S.R + 2.0 * L + (S.sheet ? S.off : 0.0) });
		double maxBall = 0.0;
		for (const Ball& b : balls) maxBall = std::max(maxBall, b.r);
		const double zc = std::max(maxBall, 4.0 * L);
		std::unordered_map<uint64_t, std::vector<int32_t>> zg;
		for (size_t i = 0; i < balls.size(); i++)
		{
			const Ball& b = balls[i];
			for (int64_t z = CellOf(b.c.z - b.r, zc); z <= CellOf(b.c.z + b.r, zc); z++)
				for (int64_t y = CellOf(b.c.y - b.r, zc); y <= CellOf(b.c.y + b.r, zc); y++)
					for (int64_t x = CellOf(b.c.x - b.r, zc); x <= CellOf(b.c.x + b.r, zc); x++)
						zg[CellKey(x, y, z)].push_back((int32_t)i);
		}
		                                                              
		auto inZone = [&](const V& lo, const V& hi) {
			const int64_t x0 = CellOf(lo.x, zc), x1 = CellOf(hi.x, zc);
			const int64_t y0 = CellOf(lo.y, zc), y1 = CellOf(hi.y, zc);
			const int64_t z0 = CellOf(lo.z, zc), z1 = CellOf(hi.z, zc);
			if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096)
				return true;
			for (int64_t z = z0; z <= z1; z++)
				for (int64_t y = y0; y <= y1; y++)
					for (int64_t x = x0; x <= x1; x++)
					{
						const auto it = zg.find(CellKey(x, y, z));
						if (it == zg.end())
							continue;
						for (const int32_t bi : it->second)
						{
							const Ball& b = balls[(size_t)bi];
							const V cl(ClampD(b.c.x, lo.x, hi.x), ClampD(b.c.y, lo.y, hi.y), ClampD(b.c.z, lo.z, hi.z));
							const V d = cl - b.c;
							if (m::la::dot(d, d) <= b.r * b.r)
								return true;
						}
					}
			return false;
		};
		                                                                
		Refine(M, 4.0 * L, budget, inZone);
		double zoneArea = 0.0;
		for (size_t t = 0; t < M.NumTri(); t++)
		{
			const V a = M.Pos(M.tri[t * 3]), b = M.Pos(M.tri[t * 3 + 1]), c = M.Pos(M.tri[t * 3 + 2]);
			const V ctr = (a + b + c) / 3.0;
			if (inZone(ctr, ctr))
				zoneArea += 0.5 * m::la::length(m::la::cross(b - a, c - a));
		}
		const double L2 = std::sqrt(zoneArea / (0.433 * 0.9 * (double)budget)) * 1.1;
		if (L2 > L)
			L = L2;
		Refine(M, L, budget, inZone);
		computeNormals();
		                                                                         
		Zone nearZ;
		const double margin = reach + 2.0 * L + 0.5 * fMin;
		nearZ.cell = std::max({ margin, 4.0 * L, 1e-3 });
		for (const ProjectDamagePoly& q : imp.polys)
		{
			const V a = imp.pts[(size_t)q.a], b = imp.pts[(size_t)q.b], c = imp.pts[(size_t)q.c];
			nearZ.AddBox(V(std::min({ a.x, b.x, c.x }), std::min({ a.y, b.y, c.y }), std::min({ a.z, b.z, c.z })) - V(margin),
				V(std::max({ a.x, b.x, c.x }), std::max({ a.y, b.y, c.y }), std::max({ a.z, b.z, c.z })) + V(margin));
		}
		computeContacts(nearZ);
		buildSamples();
		if (samples.empty())
			return false;
	}
	const size_t nv = M.NumVert();
	const size_t nt = M.NumTri();

	                                                                                      
	std::vector<int32_t> parent(samples.size());
	for (size_t i = 0; i < parent.size(); i++) parent[i] = (int32_t)i;
	auto find = [&](int32_t x) { while (parent[(size_t)x] != x) { parent[(size_t)x] = parent[(size_t)parent[(size_t)x]]; x = parent[(size_t)x]; } return x; };
	{
		std::unordered_map<uint64_t, std::vector<int32_t>> cells;
		auto ck = [&](const V& c) { return std::array<int64_t, 3>{ CellOf(c.x, hCell), CellOf(c.y, hCell), CellOf(c.z, hCell) }; };
		for (size_t i = 0; i < samples.size(); i++)
		{
			const auto c = ck(samples[i].c);
			cells[CellKey(c[0], c[1], c[2])].push_back((int32_t)i);
		}
		for (size_t i = 0; i < samples.size(); i++)
		{
			const auto c = ck(samples[i].c);
			for (int64_t dz = -1; dz <= 1; dz++)
				for (int64_t dy = -1; dy <= 1; dy++)
					for (int64_t dx = -1; dx <= 1; dx++)
					{
						const auto it = cells.find(CellKey(c[0] + dx, c[1] + dy, c[2] + dz));
						if (it == cells.end())
							continue;
						for (const int32_t j : it->second)
						{
							if (m::la::dot(samples[i].dir, samples[(size_t)j].dir) <= 0.0)
								continue;
							if (m::la::length(samples[i].c - samples[(size_t)j].c) > 2.0 * hCell)
								continue;
							const int32_t a = find((int32_t)i), b = find(j);
							if (a != b) parent[(size_t)std::max(a, b)] = std::min(a, b);
						}
					}
		}
	}
	std::vector<Dent> dents;
	{
		std::unordered_map<int32_t, int32_t> rootToDent;
		for (size_t i = 0; i < samples.size(); i++)
		{
			const int32_t r = find((int32_t)i);
			auto it = rootToDent.find(r);
			if (it == rootToDent.end())
			{
				it = rootToDent.emplace(r, (int32_t)dents.size()).first;
				dents.emplace_back();
			}
			samples[i].cluster = it->second;
		}
		std::vector<double> wsum(dents.size(), 0.0);
		std::vector<V> csum(dents.size(), V(0.0)), dsum(dents.size(), V(0.0));
		for (const Sample& S : samples)
		{
			Dent& D = dents[(size_t)S.cluster];
			wsum[(size_t)S.cluster] += S.d0;
			csum[(size_t)S.cluster] += S.c * S.d0;
			dsum[(size_t)S.cluster] += S.dir * S.d0;
			D.depth = std::max(D.depth, S.d0);
			D.R = std::max(D.R, S.R);
		}
		for (size_t d = 0; d < dents.size(); d++)
		{
			Dent& D = dents[d];
			D.center = csum[d] / std::max(wsum[d], 1e-30);
			D.dir = Unit(dsum[d], V(0, 0, -1));
			D.e1 = AnyPerp(D.dir);
			D.e2 = m::la::cross(D.dir, D.e1);
		}
		                                           
		std::vector<std::array<double, 3>> cov(dents.size(), { 0.0, 0.0, 0.0 });
		for (const Sample& S : samples)
		{
			const Dent& D = dents[(size_t)S.cluster];
			const V q = S.c - D.center;
			const double x = m::la::dot(q, D.e1), y = m::la::dot(q, D.e2);
			auto& c = cov[(size_t)S.cluster];
			c[0] += x * x; c[1] += x * y; c[2] += y * y;
		}
		for (size_t d = 0; d < dents.size(); d++)
		{
			Dent& D = dents[d];
			const auto& c = cov[d];
			const double ang = 0.5 * std::atan2(2.0 * c[1], c[0] - c[2]);
			const V a1 = D.e1 * std::cos(ang) + D.e2 * std::sin(ang);
			D.e1 = Unit(a1, D.e1);
			D.e2 = m::la::cross(D.dir, D.e1);
		}
		std::vector<std::array<double, 4>> box(dents.size(), { 1e300, -1e300, 1e300, -1e300 });
		for (const Sample& S : samples)
		{
			const Dent& D = dents[(size_t)S.cluster];
			const V q = S.c - D.center;
			const double x = m::la::dot(q, D.e1), y = m::la::dot(q, D.e2);
			auto& b = box[(size_t)S.cluster];
			b[0] = std::min(b[0], x); b[1] = std::max(b[1], x);
			b[2] = std::min(b[2], y); b[3] = std::max(b[3], y);
		}
		for (size_t d = 0; d < dents.size(); d++)
		{
			Dent& D = dents[d];
			const auto& b = box[d];
			D.center += D.e1 * (0.5 * (b[0] + b[1])) + D.e2 * (0.5 * (b[2] + b[3]));
			D.rho = std::max(0.5 * (b[3] - b[2]), hCell);
			D.halfLen = std::max(0.0, 0.5 * (b[1] - b[0]) - D.rho);
			D.perim = 4.0 * D.halfLen + 2.0 * kPi * D.rho;
			                                                       
			const double q = std::max(D.R, 1.0);
			const uint64_t key = CellKey(CellOf(D.center.x, q), CellOf(D.center.y, q), CellOf(D.center.z, q));
			D.key = key;
			D.offCrumple = NoiseOffset(seed, key, 9);
			D.offRough = NoiseOffset(seed, key, 12);
		}
	}

	                                                                                        
	double maxInf = 0.0, dRef = 0.0;
	for (const Sample& S : samples) { maxInf = std::max(maxInf, S.rinf); dRef = std::max(dRef, S.d0); }
	const double gCell = std::max(0.5 * maxInf, 4.0 * L);
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	for (size_t i = 0; i < samples.size(); i++)
	{
		const Sample& S = samples[i];
		const int64_t x0 = CellOf(S.c.x - S.rinf, gCell), x1 = CellOf(S.c.x + S.rinf, gCell);
		const int64_t y0 = CellOf(S.c.y - S.rinf, gCell), y1 = CellOf(S.c.y + S.rinf, gCell);
		const int64_t z0 = CellOf(S.c.z - S.rinf, gCell), z1 = CellOf(S.c.z + S.rinf, gCell);
		for (int64_t z = z0; z <= z1; z++)
			for (int64_t y = y0; y <= y1; y++)
				for (int64_t x = x0; x <= x1; x++)
					grid[CellKey(x, y, z)].push_back((int32_t)i);
	}

	                                                                                    
	std::vector<double> Dv(nv, 0.0), lobe(nv, 0.0), lobeD0(nv, 0.0);
	std::vector<V> dirV(nv, V(0.0));
	std::vector<int32_t> top1(nv, -1), top2(nv, -1);
	std::vector<double> val1(nv, 0.0), val2(nv, 0.0);
	std::vector<uint8_t> sheetV(nv, 0);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii)
	{
		const size_t i = (size_t)ii;
		if (M.geo[i] != (int32_t)i)
			return;
		const V pos = M.Pos(i);
		const auto it = grid.find(CellKey(CellOf(pos.x, gCell), CellOf(pos.y, gCell), CellOf(pos.z, gCell)));
		                                                                         
		                                                                             
		const bool own = con[i].d0 > 0.0 && !con[i].sheet;
		if (it == grid.end() && !own)
			return;
		static const std::vector<int32_t> kEmpty;
		const std::vector<int32_t>& list = it == grid.end() ? kEmpty : it->second;
		auto eval = [&](const Sample& S, double& t, double& atten) -> double {
			const V q = pos - S.c;
			const double a = m::la::dot(q, S.dir);
			const double lat2 = std::max(0.0, m::la::dot(q, q) - a * a);
			const double lr = 1.35 * S.R;
			if (lat2 >= lr * lr)
				return -1.0;
			t = std::sqrt(lat2) / S.R;
			const double am = a * S.side;
			atten = am >= 0.0 ? 1.0 - Smooth(S.off, S.off + S.lfade, am)
				: 1.0 - Smooth(0.5 * S.R, 1.35 * S.R, -am);
			if (atten <= 0.0)
				return -1.0;
			return t < 1.0 ? S.d0 * kernel(t, S.tip) * atten : 0.0;
		};
		                                                                      
		                                                                   
		                                                                          
		struct Slot { int32_t id; double vmax, num, den; V ds; };
		Slot slots[6];
		int ns = 0;
		auto slotOf = [&](int32_t c) -> Slot* {
			for (int k = 0; k < ns; k++) if (slots[k].id == c) return &slots[k];
			if (ns < 6) { slots[ns] = Slot{ c, 0.0, 0.0, 0.0, V(0.0) }; return &slots[ns++]; }
			return nullptr;                                                           
		};
		double bl = 0.0, bld = 0.0, bestV = 0.0;
		bool sheetBest = false;
		V lobeDir(0.0);
		for (const int32_t si : list)
		{
			const Sample& S = samples[(size_t)si];
			double t = 0.0, at = 0.0;
			const double v = eval(S, t, at);
			if (v < 0.0)
				continue;
			Slot* sl = slotOf(S.cluster);
			if (sl != nullptr && v > sl->vmax) sl->vmax = v;
			if (v > bestV) { bestV = v; sheetBest = S.sheet; }
			if (t > 0.6)
			{
				const double g = (t - 1.0) / 0.28;
				const double lb = S.d0 * std::exp(-g * g) * at * Smooth(0.6, 0.85, t) * (1.0 - Smooth(1.15, 1.35, t));
				if (lb > bl) { bl = lb; bld = S.d0; lobeDir = S.dir; }
			}
		}
		for (const int32_t si : list)
		{
			const Sample& S = samples[(size_t)si];
			double t = 0.0, at = 0.0;
			const double v = eval(S, t, at);
			if (v <= 0.0)
				continue;
			Slot* sl = slotOf(S.cluster);
			if (sl == nullptr || sl->vmax <= 0.0)
				continue;
			const double w = std::pow(v / sl->vmax, 160.0);
			sl->num += w * v; sl->den += w; sl->ds += S.dir * w;
		}
		double D = 0.0;
		V ds(0.0);
		int32_t c1 = -1, c2 = -1;
		double v1 = 0.0, v2 = 0.0;
		auto smax = [](double a, double b) {
			const double k = 0.2 * std::min(a, b);
			if (k <= 0.0) return std::max(a, b);
			const double h = std::max(k - std::abs(a - b), 0.0) / k;
			return std::max(a, b) + h * h * k * 0.25;
		};
		for (int k = 0; k < ns; k++)
		{
			const Slot& sl = slots[k];
			if (sl.den <= 0.0)
				continue;
			const double dc = sl.num / sl.den;
			D = smax(D, dc);
			const double w = std::pow(dc / std::max(bestV, 1e-300), 8.0);
			ds += Unit(sl.ds, V(0.0)) * w;
			if (dc > v1) { c2 = c1; v2 = v1; c1 = sl.id; v1 = dc; }
			else if (dc > v2) { c2 = sl.id; v2 = dc; }
		}
		if (own)
		{
			                                    
			D = smax(D, con[i].d0);
			ds += con[i].dir * std::pow(con[i].d0 / std::max({ bestV, con[i].d0, 1e-300 }), 8.0);
			if (c1 < 0)
			{
				                                                                     
				int32_t bestS = -1;
				double bd = 1e300;
				for (const int32_t si : list)
				{
					const double d = m::la::length(samples[(size_t)si].c - pos);
					if (d < bd) { bd = d; bestS = si; }
				}
				if (bestS >= 0) { c1 = samples[(size_t)bestS].cluster; v1 = con[i].d0; }
			}
		}
		if (c1 < 0)
		{
			                                    
			for (int k = 0; k < ns; k++)
				if (c1 < 0 || slots[k].vmax > v1) { c1 = slots[k].id; v1 = 1e-9; }
			if (D == 0.0) ds = lobeDir;
		}
		Dv[i] = D;
		dirV[i] = Unit(ds, own ? con[i].dir : lobeDir);
		lobe[i] = bl;
		lobeD0[i] = bld;
		top1[i] = c1; top2[i] = c2; val1[i] = v1; val2[i] = v2;
		sheetV[i] = sheetBest ? 1 : 0;
	});

	                                                                                     
	std::vector<double> slope(nv, 0.0), area(nv, 0.0), lapSum(nv, 0.0), lapCnt(nv, 0.0);
	double edgeSum = 0.0;
	size_t edgeCnt = 0;
	for (size_t t = 0; t < nt; t++)
	{
		const size_t g[3] = { (size_t)M.geo[M.tri[t * 3]], (size_t)M.geo[M.tri[t * 3 + 1]], (size_t)M.geo[M.tri[t * 3 + 2]] };
		const V a = M.Pos(g[0]), b = M.Pos(g[1]), c = M.Pos(g[2]);
		const V cr = m::la::cross(b - a, c - a);
		const double A2 = m::la::length(cr);
		if (A2 <= 1e-20)
			continue;
		const bool live = Dv[g[0]] != 0.0 || Dv[g[1]] != 0.0 || Dv[g[2]] != 0.0;
		if (live)
		{
			const V n = cr / A2;
			const V grad = (m::la::cross(n, c - b) * Dv[g[0]] + m::la::cross(n, a - c) * Dv[g[1]]
				+ m::la::cross(n, b - a) * Dv[g[2]]) / A2;
			const double gm = m::la::length(grad);
			for (int k = 0; k < 3; k++) { slope[g[k]] += gm * A2; area[g[k]] += A2; }
			for (int k = 0; k < 3; k++)
			{
				lapSum[g[k]] += Dv[g[(k + 1) % 3]] + Dv[g[(k + 2) % 3]];
				lapCnt[g[k]] += 2.0;
			}
			edgeSum += m::la::length(b - a);
			edgeCnt++;
		}
	}
	const double edge = edgeCnt > 0 ? std::max(edgeSum / (double)edgeCnt, 1e-4) : L;
	for (size_t i = 0; i < nv; i++)
		if (area[i] > 0.0) slope[i] /= area[i];

	                                                                                     
	std::vector<double> weight(nv, 1.0);
	if (p.avoid && in.avoidPoly != nullptr && haveView
		&& std::find(in.avoidPoly->begin(), in.avoidPoly->end(), true) != in.avoidPoly->end())
	{
		const std::vector<bool>& avoid = *in.avoidPoly;
		std::vector<uint8_t> avoidTri(nt, 0);
		ProjectDamageParallelFor(0, (int64_t)nt, [&](int64_t t)
		{
			const V a = M.Pos(M.tri[t * 3]), b = M.Pos(M.tri[t * 3 + 1]), c = M.Pos(M.tri[t * 3 + 2]);
			bool live = false;
			for (int k = 0; k < 3; k++) live = live || Dv[(size_t)M.geo[M.tri[t * 3 + k]]] != 0.0 || lobe[(size_t)M.geo[M.tri[t * 3 + k]]] != 0.0;
			if (!live)
				return;
			const V fn = Unit(m::la::cross(b - a, c - a), V(0, 0, 1));
			const V ctr = (a + b + c) / 3.0;
			const double eps = std::max(1e-3, edge * 0.05);
			double hit = 0.0;
			int32_t poly = -1;
			if (viewBvh.Raycast(ctr + fn * eps, -fn, 2.0 * eps, &hit, &poly)
				&& poly >= 0 && (size_t)poly < avoid.size() && avoid[(size_t)poly])
				avoidTri[(size_t)t] = 1;
		});
		for (size_t t = 0; t < nt; t++)
			if (avoidTri[t])
				for (int k = 0; k < 3; k++)
					weight[(size_t)M.geo[M.tri[t * 3 + k]]] = 0.0;
	}
	const bool maskOn = mask != nullptr && mask->Active() && mask->Amount() > 0.0;

	                                                                                     
	const double creaseK = std::max(p.crease, 0.0);
	const double spacing = std::max(p.creaseSpacing, 1e-3);
	const double cLen = std::max(p.creaseLength, 1e-3);
	const double cSharp = ClampD(p.creaseSharp, 0.0, 1.0);
	const double cBranch = ClampD(p.creaseBranch, 0.0, 1.0);
	const double crumK = std::max(p.crumple, 0.0);
	const double crumS = std::max(p.crumpleScale, 1e-3);
	const double cap = std::max(p.thicknessCap, 0.0);

	                                                                       
	                                                                         
	                                                                           
	                                                                          
	                                                                         
	                                                                       
	                            
	auto buckle = [&](const Dent& D, const V& pos, double& ridgeOut) -> double {
		ridgeOut = 0.0;
		const V q = pos - D.center;
		const double x = m::la::dot(q, D.e1), y = m::la::dot(q, D.e2);
		const double Lh = D.halfLen, rho = D.rho;
		const double sx = ClampD(x, -Lh, Lh);
		const double rad = std::sqrt((x - sx) * (x - sx) + y * y);
		const bool side = x >= -Lh && x <= Lh;
		double phi;
		if (side)
			phi = y >= 0.0 ? x + Lh : 2.0 * Lh + kPi * rho + (Lh - x);
		else if (x > Lh)
			phi = 2.0 * Lh + rho * std::atan2(x - Lh, y);
		else
			phi = 4.0 * Lh + kPi * rho + rho * std::atan2(-(x + Lh), -y);
		const double rr = rad - rho;
		const double maxLen = D.R * cLen;
		if (rr > maxLen || rr < -rho)
			return 0.0;
		                                                             
		const double sc = side ? 1.0 : (rho + std::max(rr, 0.0)) / rho;
		const double P = std::max(D.perim, 1e-6);
		const int N = std::max(3, (int)std::lround(P / spacing));
		const double sp = P / N;
		const int k0 = (int)std::floor(phi / sp);
		const double rise = Smooth(-0.15 * rho, 0.15 * D.R, rr);
		double sum = 0.0, strong = 0.0;
		auto fold = [&](double center, double len, double amp, double sgn, double r0) {
			if (rr < r0 || rr > len)
				return;
			double d = (phi - center);
			d -= P * std::round(d / P);
			const double w = std::max(4.0 * edge, sp * 0.3 * (1.0 + 0.8 * std::max(rr, 0.0) / D.R));
			const double xx = std::abs(d * sc) / w;
			if (xx >= 1.0)
				return;
			const double soft = 0.5 * (1.0 + std::cos(kPi * xx));
			                                                                    
			                                                               
			const double tip = std::max(0.12, 1.5 * edge / w);
			const double xr = std::sqrt(xx * xx + tip * tip) - tip;
			const double sharp = std::pow(std::max(0.0, 1.0 - xr / (std::sqrt(1.0 + tip * tip) - tip)), 1.8);
			const double prof = Lerp(soft, sharp, cSharp);
			const double along = (r0 > -rho ? Smooth(r0, r0 + 0.15 * D.R, rr) : rise)
				* (1.0 - Smooth(0.5 * len, len, rr));
			const double v = amp * prof * along;
			sum += sgn * v;
			strong = std::max(strong, v);
		};
		for (int dk = -2; dk <= 2; dk++)
		{
			const int k = k0 + dk;
			const uint64_t fk = (D.key * 4099ULL) ^ (uint64_t)(((k % N) + N) % N);
			if (Random01(seed, fk, 0) > 0.7)
				continue;                                   
			const double len = maxLen * (0.3 + 0.7 * Random01(seed, fk, 2));
			const double amp = 0.3 + 0.7 * Random01(seed, fk, 3);
			const double sgn = Random01(seed, fk, 4) < 0.65 ? -1.0 : 1.0;                                
			const double bend = (Random01(seed, fk, 5) * 2.0 - 1.0) * 0.3;
			const double base = (k + 0.5 + (Random01(seed, fk, 1) - 0.5) * 0.6) * sp;
			const double center = base + bend * std::max(rr, 0.0) / sc;
			fold(center, len, amp, sgn, -rho);
			if (cBranch > 0.0 && Random01(seed, fk, 6) < cBranch)
			{
				                                                    
				const double rb = len * (0.25 + 0.35 * Random01(seed, fk, 7));
				const double veer = (Random01(seed, fk, 8) < 0.5 ? -1.0 : 1.0) * (0.35 + 0.4 * Random01(seed, fk, 9));
				const double cb = base + bend * std::max(rr, 0.0) / sc + veer * std::max(rr - rb, 0.0) / sc;
				fold(cb, rb + (len - rb) * 0.8, amp * 0.65, sgn, rb);
			}
		}
		ridgeOut = ClampD(strong, 0.0, 1.0);
		return ClampD(sum, -1.0, 1.0);
	};

	std::vector<double> disp(nv, 0.0);
	std::vector<float> mDent(nv, 0.0f), mCrease(nv, 0.0f), mContact(nv, 0.0f);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii)
	{
		const size_t i = (size_t)ii;
		if (M.geo[i] != (int32_t)i)
			return;
		const double D0 = Dv[i];
		if (D0 == 0.0 && lobe[i] == 0.0)
			return;
		const V pos = M.Pos(i);
		double w = weight[i];
		if (maskOn)
			w *= 1.0 - mask->Amount() * (1.0 - ClampD(mask->Weight(pos), 0.0, 1.0));
		const int32_t c1 = top1[i];
		double D = D0;
		                               
		if (p.bulge > 0.0 && lobe[i] > 0.0)
			D -= p.bulge * lobe[i] * (1.0 - Smooth(0.0, 0.3 * std::max(lobeD0[i], 1e-9), D0));
		                                                                    
		auto detail = [&](const Dent& d, double& ridge) -> double {
			ridge = 0.0;
			double h = 0.0;
			const double slopeRef = std::max(d.depth / std::max(d.R, 1e-6), 1e-6);
			const double sEnv = Smooth(0.05, 0.45, slope[i] / slopeRef);
			if (creaseK > 0.0)
			{
				double r = 0.0;
				const double b = buckle(d, pos, r);
				h += b * creaseK * 0.25 * d.depth * (0.35 + 0.65 * sEnv);
				ridge = r * (0.35 + 0.65 * sEnv);
			}
			                                                                  
			                                                 
			const double rel = ClampD(D0 / std::max(d.depth, 1e-9), 0.0, 1.0);
			const V qp = pos - d.dir * m::la::dot(pos - d.center, d.dir);
			if (crumK > 0.0 && rel > 0.0)
			{
				const V cq = qp / crumS + d.offCrumple;
				const double f1 = 1.0 - std::abs(2.0 * Noise(cq) - 1.0);
				const double f2 = 1.0 - std::abs(2.0 * Noise(cq * 2.3 + V(5.1, 7.7, 1.3)) - 1.0);
				const double facet = (f1 * f1 + 0.5 * f2 * f2) / 1.5 - 0.42;
				h += crumK * 0.1 * d.depth * std::pow(rel, 1.5) * (0.4 + 0.6 * sEnv) * facet * 2.0;
			}
			if (p.roughness > 0.0)
				h += p.roughness * Smooth(0.0, 0.25, rel) * Fbm(qp / std::max(1.0, 3.0 * edge) + d.offRough, 2);
			                                                                 
			                  
			if (h < 0.0)
				h *= 1.0 - Smooth(0.75, 0.97, rel);
			return h;
		};
		                                                                     
		                 
		double ridgeMark = 0.0, depthB = 0.0, radB = 1.0;
		if (c1 >= 0)
		{
			const Dent& d1 = dents[(size_t)c1];
			double r1 = 0.0;
			double h = detail(d1, r1);
			ridgeMark = r1; depthB = d1.depth; radB = d1.R;
			if (top2[i] >= 0 && val2[i] > 0.0)
			{
				const Dent& d2 = dents[(size_t)top2[i]];
				double r2 = 0.0;
				const double h2 = detail(d2, r2);
				const double w1 = 1.0, w2 = std::pow(val2[i] / std::max(val1[i], 1e-300), 4.0);
				const double ws = w1 + w2;
				h = (h * w1 + h2 * w2) / ws;
				ridgeMark = (r1 * w1 + r2 * w2) / ws;
				depthB = (d1.depth * w1 + d2.depth * w2) / ws;
				radB = (d1.R * w1 + d2.R * w2) / ws;
			}
			D += h;
		}
		const V dir = Unit(dirV[i], V(0.0));
		if (m::la::dot(dir, dir) == 0.0)
			return;
		D *= w;
		                                                              
		const V n = nrm[i];
		if (cap > 0.0 && !sheetV[i] && haveView && D > 0.0 && m::la::dot(dir, n) < 0.0)
		{
			const double eps = 1e-3;
			double hit = 0.0;
			if (viewBvh.Raycast(pos - n * eps, -n, D / cap + eps, &hit))
				D = std::min(D, cap * (hit + eps));
		}
		disp[i] = D;
		        
		if (c1 >= 0)
		{
			mDent[i] = (float)ClampD(D0 / std::max(depthB, 1e-9), 0.0, 1.0) * (float)w;
			const double lapK = lapCnt[i] > 0.0 ? std::abs(lapSum[i] / lapCnt[i] - D0) : 0.0;
			const double hinge = ClampD(lapK / (edge * edge) * radB * radB / (std::max(depthB, 1e-9) * 16.0), 0.0, 1.0);
			mCrease[i] = (float)(ClampD(std::max(ridgeMark, hinge), 0.0, 1.0) * w);
		}
		mContact[i] = (float)(con[i].touch * w);
	});

	std::vector<V> delta(nv, V(0.0));
	for (size_t i = 0; i < nv; i++)
		if (M.geo[i] == (int32_t)i && disp[i] != 0.0)
			delta[i] = Unit(dirV[i], V(0.0)) * disp[i];
	return FinishDeform(M, gl, delta, mDent, mCrease, mContact, edge, out, marksOut);
}

void EvaluateMetalDeformAttributes(const MetalDeformMarks* marks, const std::vector<EVec>& points,
	std::vector<float>& dent, std::vector<float>& crease, std::vector<float>& contact)
{
	dent.assign(points.size(), 0.0f);
	crease.assign(points.size(), 0.0f);
	contact.assign(points.size(), 0.0f);
	if (marks == nullptr || marks->pos.empty())
		return;
	const double cell = marks->cell;
	ProjectDamageParallelFor(0, (int64_t)points.size(), [&](int64_t ii)
	{
		const V p = points[(size_t)ii];
		const int64_t cx = CellOf(p.x, cell), cy = CellOf(p.y, cell), cz = CellOf(p.z, cell);
		double best = marks->reach * marks->reach;
		int32_t bi = -1;
		for (int64_t dz = -1; dz <= 1; dz++)
			for (int64_t dy = -1; dy <= 1; dy++)
				for (int64_t dx = -1; dx <= 1; dx++)
				{
					const auto it = marks->grid.find(CellKey(cx + dx, cy + dy, cz + dz));
					if (it == marks->grid.end())
						continue;
					for (const int32_t k : it->second)
					{
						const V d = marks->pos[(size_t)k] - p;
						const double d2 = m::la::dot(d, d);
						if (d2 < best) { best = d2; bi = k; }
					}
				}
		if (bi < 0)
			return;
		const float f = (float)(1.0 - Smooth(0.3, 1.0, std::sqrt(best) / marks->reach));
		dent[(size_t)ii] = marks->dent[(size_t)bi] * f;
		crease[(size_t)ii] = marks->crease[(size_t)bi] * f;
		contact[(size_t)ii] = marks->contact[(size_t)bi] * f;
	});
}

PROJECT_DAMAGE_NS_MACRO_END
