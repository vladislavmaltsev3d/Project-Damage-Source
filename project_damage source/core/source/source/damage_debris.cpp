// получаем обломки из объёма, потерянного при повреждении
// ProjectDamage damage debris — GPL-3.0-or-later; see LICENSE.
//
// Recipe: damage-debris-v1. See damage_debris.h.
//   1. removed = source - result (one Boolean);
//   2. Decompose into connected regions;
//   3. drop dust (V < minVolume) and films (2V/A < minThickness);
//   4. chunkSize > 0: a region whose largest bounding-box side exceeds
//      chunkSize is split by a plane through (near) its centre, normal mostly
//      across its longest side and turned by chunkJitter; each half is
//      decomposed again and queued. The largest piece is always split
//      first, so when maxPieces stops the splitting the rubble is even;
//   5. halves only pass the dust test (minVolume): the film test is for
//      finding regions; applied to halves it would throw away half of every
//      thin wedge (edge chips) and stop the split. Sort largest first.
// BuildDebrisMesh: outer faces (runs with the source's original id) keep the
// source UVs and get their source polygon (material) by a nearest-triangle
// lookup on a uniform grid; inner faces get basePoly -1 and a box projection.
#include "manifold/manifold.h"
#include "damage_debris.h"
#include "project_damage_internal.h"                    
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

double Random01(uint64_t seed, uint64_t candidate, uint64_t channel)
{
	uint64_t z = seed ^ (candidate * 0x9e3779b97f4a7c15ULL) ^ ((channel + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}

bool Keep(const m::Manifold& piece, double minVol, double minThick)
{
	const double v = piece.Volume();
	if (!(v > minVol)) return false;
	const double a = piece.SurfaceArea();
	if (a <= 0.0) return false;
	return 2.0 * v / a >= minThick;
}

                                           
double DistSq(const m::vec3& p, const m::vec3& a, const m::vec3& b, const m::vec3& c)
{
	const m::vec3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return m::la::dot(ap, ap);
	const m::vec3 bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return m::la::dot(bp, bp);
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { const m::vec3 q = a + ab * (d1 / (d1 - d3)); return m::la::dot(p - q, p - q); }
	const m::vec3 cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return m::la::dot(cp, cp);
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { const m::vec3 q = a + ac * (d2 / (d2 - d6)); return m::la::dot(p - q, p - q); }
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		const m::vec3 q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
		return m::la::dot(p - q, p - q);
	}
	const double den = 1.0 / (va + vb + vc);
	const m::vec3 q = a + ab * (vb * den) + ac * (vc * den);
	return m::la::dot(p - q, p - q);
}

                                                                           
                                                                             
                                       
struct SourceLookup {
	                                                             
	struct Tri { m::vec3 a, b, c, n; Int32 poly; Int32 v[3]; m::vec3 cn[3]; };
	std::vector<Tri> tris;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	m::vec3 lo{0.0};
	double cell = 1.0;
	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	void Build(const MeshView& v, double smoothAngleDeg = 60.0)
	{
		m::vec3 hi(-std::numeric_limits<double>::infinity());
		lo = m::vec3(std::numeric_limits<double>::infinity());
		double edge = 0.0;
		for (Int32 i = 0; i < v.npolys; ++i) {
			const ProjectDamagePoly& q = v.polys[i];
			const Int32 ids[2][3] = {{q.a, q.b, q.c}, {q.a, q.c, q.d}};
			for (int k = 0; k < (q.c == q.d ? 1 : 2); ++k) {
				Tri t{v.pts[ids[k][0]], v.pts[ids[k][1]], v.pts[ids[k][2]], m::vec3(0.0), i,
					{ids[k][0], ids[k][1], ids[k][2]}, {m::vec3(0.0), m::vec3(0.0), m::vec3(0.0)}};
				const m::vec3 cr = m::la::cross(t.b - t.a, t.c - t.a);
				const double l = m::la::length(cr);
				t.n = l > 0.0 ? cr / l : m::vec3(0.0);
				tris.push_back(t);
				lo = m::la::min(lo, m::la::min(t.a, m::la::min(t.b, t.c)));
				hi = m::la::max(hi, m::la::max(t.a, m::la::max(t.b, t.c)));
				edge += m::la::length(t.b - t.a) + m::la::length(t.c - t.b) + m::la::length(t.a - t.c);
			}
		}
		if (tris.empty()) return;
		                                                                
		                                                                     
		{
			const double cosLim = std::cos(std::min(180.0, std::max(0.0, smoothAngleDeg)) * 3.14159265358979323846 / 180.0);
			std::vector<std::vector<int32_t>> around(size_t(std::max(0, v.npts)));
			for (size_t i = 0; i < tris.size(); ++i)
				for (int k = 0; k < 3; ++k) around[size_t(tris[i].v[k])].push_back(int32_t(i));
			auto cornerAngle = [](const Tri& t, int k) {
				const m::vec3 P[3] = {t.a, t.b, t.c};
				const m::vec3 e1 = P[(k + 1) % 3] - P[k], e2 = P[(k + 2) % 3] - P[k];
				const double l = m::la::length(e1) * m::la::length(e2);
				return l > 0.0 ? std::acos(std::max(-1.0, std::min(1.0, m::la::dot(e1, e2) / l))) : 0.0;
			};
			for (size_t i = 0; i < tris.size(); ++i) {
				Tri& t = tris[i];
				for (int k = 0; k < 3; ++k) {
					m::vec3 sum(0.0);
					for (int32_t j : around[size_t(t.v[k])]) {
						const Tri& o = tris[size_t(j)];
						if (m::la::dot(o.n, t.n) < cosLim) continue;
						int kk = 0;
						while (kk < 3 && o.v[kk] != t.v[k]) ++kk;
						if (kk < 3) sum += o.n * cornerAngle(o, kk);
					}
					const double l = m::la::length(sum);
					t.cn[k] = l > 1e-12 ? sum / l : t.n;
				}
			}
		}
		cell = std::max(1e-6, 1.5 * edge / (3.0 * double(tris.size())));
		const double diag = m::la::length(hi - lo);
		cell = std::max(cell, diag / 512.0);
		tol = std::max(1e-6, 1e-4 * diag);
		for (size_t i = 0; i < tris.size(); ++i) {
			const Tri& t = tris[i];
			const m::vec3 a = m::la::min(t.a, m::la::min(t.b, t.c)), b = m::la::max(t.a, m::la::max(t.b, t.c));
			const int64_t x0 = int64_t(std::floor((a.x - lo.x) / cell)), x1 = int64_t(std::floor((b.x - lo.x) / cell));
			const int64_t y0 = int64_t(std::floor((a.y - lo.y) / cell)), y1 = int64_t(std::floor((b.y - lo.y) / cell));
			const int64_t z0 = int64_t(std::floor((a.z - lo.z) / cell)), z1 = int64_t(std::floor((b.z - lo.z) / cell));
			for (int64_t x = x0; x <= x1; ++x)
				for (int64_t y = y0; y <= y1; ++y)
					for (int64_t z = z0; z <= z1; ++z) grid[Key(x, y, z)].push_back(int32_t(i));
		}
	}
	double tol = 1e-4;                                              
	                                                                         
	                                                                         
	                                                                  
	Int32 Find(const m::vec3& p, const m::vec3& n, bool* opposite = nullptr) const
	{
		if (opposite) *opposite = false;
		if (tris.empty()) return -1;
		const int64_t cx = int64_t(std::floor((p.x - lo.x) / cell)), cy = int64_t(std::floor((p.y - lo.y) / cell)),
			cz = int64_t(std::floor((p.z - lo.z) / cell));
		double best = std::numeric_limits<double>::infinity();
		Int32 poly = -1;
		for (int r = 0; r <= 4; ++r) {
			for (int64_t x = cx - r; x <= cx + r; ++x)
				for (int64_t y = cy - r; y <= cy + r; ++y)
					for (int64_t z = cz - r; z <= cz + r; ++z) {
						if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != r) continue;
						auto it = grid.find(Key(x, y, z));
						if (it == grid.end()) continue;
						for (int32_t ti : it->second) {
							const Tri& t = tris[size_t(ti)];
							if (m::la::dot(t.n, n) < 0.3) continue;                               
							const double d = DistSq(p, t.a, t.b, t.c);
							if (d < best) { best = d; poly = ti; }
						}
					}
			if (poly >= 0 && std::sqrt(best) < r * cell) break;                                  
			if (r >= 1 && poly < 0) break;                                                     
		}
		if (poly >= 0 && std::sqrt(best) > tol) poly = -1;                                  
		if (poly < 0 && opposite) {
			                                        
			auto it = grid.find(Key(cx, cy, cz));
			if (it != grid.end())
				for (int32_t ti : it->second) {
					const Tri& t = tris[size_t(ti)];
					if (m::la::dot(t.n, n) < -0.9 && std::sqrt(DistSq(p, t.a, t.b, t.c)) <= tol) { *opposite = true; break; }
				}
		}
		return poly;
	}
};

}               

// основной вход: собираем геометрию по переданным настройкам
void BuildDebrisMesh(const std::vector<m::Manifold>& pieces, uint32_t baseId, const MeshView& source,
	const DamageDebrisParams& p, DebrisMesh& out)
{
	if (pieces.empty()) return;
	SourceLookup look;
	look.Build(source, p.smoothAngle);
	                                                              
	auto smoothAt = [&](Int32 t, const m::vec3& x) {
		const SourceLookup::Tri& T = look.tris[size_t(t)];
		const m::vec3 v0 = T.b - T.a, v1 = T.c - T.a, v2 = x - T.a;
		const double d00 = m::la::dot(v0, v0), d01 = m::la::dot(v0, v1), d11 = m::la::dot(v1, v1);
		const double d20 = m::la::dot(v2, v0), d21 = m::la::dot(v2, v1);
		const double den = d00 * d11 - d01 * d01;
		double w1 = den != 0.0 ? (d11 * d20 - d01 * d21) / den : 0.0, w2 = den != 0.0 ? (d00 * d21 - d01 * d20) / den : 0.0;
		w1 = std::max(0.0, std::min(1.0, w1)); w2 = std::max(0.0, std::min(1.0 - w1, w2));
		const m::vec3 nn = T.cn[0] * (1.0 - w1 - w2) + T.cn[1] * w1 + T.cn[2] * w2;
		const double l = m::la::length(nn);
		return l > 1e-12 ? nn / l : T.n;
	};
	const double s = 1.0 / std::max(1e-9, p.innerUvScale);
	for (size_t piece = 0; piece < pieces.size(); ++piece) {
		const m::MeshGL dm = pieces[piece].GetMeshGL();
		const size_t np = dm.numProp;
		const bool hasUv = np >= 5;
		out.sourceUvs = out.sourceUvs || hasUv;
		                                                                     
		                                                          
		std::vector<Int32> slot;
		const Int32 welded = project_damage::BuildWeldSlots(dm, slot);
		const Int32 nv = welded > 0 ? welded : Int32(dm.NumVert());
		const Int32 off = Int32(out.points.size());
		out.points.resize(out.points.size() + size_t(nv));
		for (size_t j = 0; j < dm.NumVert(); ++j) {
			const Int32 sl = slot.empty() ? Int32(j) : slot[j];
			out.points[size_t(off + sl)] = EVec(dm.vertProperties[j * np], dm.vertProperties[j * np + 1], dm.vertProperties[j * np + 2]);
		}
		for (size_t r = 0; r + 1 < dm.runIndex.size(); ++r) {
			const bool outer = r < dm.runOriginalID.size() && dm.runOriginalID[r] == baseId;
			for (size_t t = dm.runIndex[r] / 3; t < dm.runIndex[r + 1] / 3; ++t) {
				uint32_t v[3];
				m::vec3 q[3];
				for (int k = 0; k < 3; ++k) {
					v[k] = dm.triVerts[3 * t + size_t(k)];
					q[k] = m::vec3(dm.vertProperties[v[k] * np], dm.vertProperties[v[k] * np + 1], dm.vertProperties[v[k] * np + 2]);
				}
				const m::vec3 cr = m::la::cross(q[1] - q[0], q[2] - q[0]);
				const double l = m::la::length(cr);
				const m::vec3 n = l > 0.0 ? cr / l : m::vec3(0.0, 0.0, 1.0);
				Int32 bp = -1, srcTri = -1;
				if (outer) {
					bool film = false;
					srcTri = look.Find((q[0] + q[1] + q[2]) / 3.0, n, &film);
					if (srcTri >= 0) bp = look.tris[size_t(srcTri)].poly;
					                                                          
					                                                        
					                                                     
					if (film) continue;
				}
				auto id = [&](int k) { return off + (slot.empty() ? Int32(v[k]) : slot[v[k]]); };
				out.polys.push_back({id(0), id(1), id(2), id(2)});
				out.piece.push_back(Int32(piece));
				out.basePoly.push_back(bp);
				                                                            
				const double an[3] = {std::abs(n.x), std::abs(n.y), std::abs(n.z)};
				const int ax = an[0] >= an[1] && an[0] >= an[2] ? 0 : (an[1] >= an[2] ? 1 : 2);
				double uv[8];
				for (int k = 0; k < 3; ++k) {
					if (bp >= 0 && hasUv) {
						uv[2 * k] = dm.vertProperties[v[k] * np + 3];
						uv[2 * k + 1] = dm.vertProperties[v[k] * np + 4];
					} else {
						const m::vec3& w = q[k];
						uv[2 * k] = (ax == 0 ? w.y : w.x) * s;
						uv[2 * k + 1] = (ax == 2 ? w.y : w.z) * s;
					}
				}
				uv[6] = uv[4]; uv[7] = uv[5];
				out.uvs.insert(out.uvs.end(), uv, uv + 8);
				                                                           
				for (int k = 0; k < 4; ++k) {
					const m::vec3 cn = srcTri >= 0 ? smoothAt(srcTri, q[std::min(k, 2)]) : n;
					out.normals.push_back(cn.x); out.normals.push_back(cn.y); out.normals.push_back(cn.z);
				}
			}
		}
	}
}

void ExtractDamageDebris(const m::Manifold& source, const m::Manifold& result,
	const DamageDebrisParams& pIn, Int32 mainSeed, std::vector<m::Manifold>& pieces)
{
	if (!pIn.enable || source.IsEmpty()) return;
	if (!(std::abs(source.Volume()) > 0.0)) return;
	const double minVol = std::max(0.0, pIn.minVolume);
	const double minThick = std::max(0.0, pIn.minThickness);
	const double chunk = std::max(0.0, pIn.chunkSize);
	const double jitter = std::min(1.0, std::max(0.0, pIn.chunkJitter));
	const size_t cap = size_t(std::max(1, std::min(pIn.maxPieces, 100000)));
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4445425249533031ULL)                
		^ (uint64_t(uint32_t(pIn.seed)) * 0x9e3779b97f4a7c15ULL);

	const m::Manifold removed = source - result;
	if (removed.IsEmpty() || removed.Status() != m::Manifold::Error::NoError) return;

	                                                                    
	struct Item { double longest; uint64_t order; m::Manifold piece; };
	auto later = [](const Item& a, const Item& b) {
		return a.longest != b.longest ? a.longest < b.longest : a.order > b.order;
	};
	std::priority_queue<Item, std::vector<Item>, decltype(later)> queue(later);
	uint64_t order = 0;
	auto push = [&](m::Manifold&& piece) {
		const m::vec3 e = piece.BoundingBox().Size();
		queue.push(Item{std::max({e.x, e.y, e.z}), order++, std::move(piece)});
	};
	for (m::Manifold& r : removed.Decompose())
		if (Keep(r, minVol, minThick)) push(std::move(r));

	std::vector<m::Manifold> done;
	uint64_t draw = 0;
	while (!queue.empty()) {
		Item it = queue.top();
		queue.pop();
		m::Manifold piece = std::move(it.piece);
		const m::Box bb = piece.BoundingBox();
		const m::vec3 ext = bb.Size();
		if (chunk <= 0.0 || it.longest <= chunk || done.size() + queue.size() + 1 >= cap) {
			done.push_back(std::move(piece));
			continue;
		}
		                                                                    
		const uint64_t k = draw++;
		auto rnd = [&](uint64_t ch) { return Random01(seed, k, ch); };
		m::vec3 axis(ext.x >= ext.y && ext.x >= ext.z ? 1.0 : 0.0,
			ext.y > ext.x && ext.y >= ext.z ? 1.0 : 0.0, ext.z > ext.x && ext.z > ext.y ? 1.0 : 0.0);
		m::vec3 r(rnd(0) * 2.0 - 1.0, rnd(1) * 2.0 - 1.0, rnd(2) * 2.0 - 1.0);
		m::vec3 n = axis + r * (1.5 * jitter);
		const double nl = m::la::length(n);
		n = nl > 1e-12 ? n / nl : axis;
		const m::vec3 c = bb.Center() + ext * ((m::vec3(rnd(3), rnd(4), rnd(5)) - 0.5) * (0.3 * jitter));
		const auto halves = piece.SplitByPlane(n, m::la::dot(n, c));
		bool any = false;
		for (const m::Manifold* h : {&halves.first, &halves.second})
			for (m::Manifold& part : h->Decompose())
				if (part.Volume() > minVol) { push(std::move(part)); any = true; }
		if (!any) done.push_back(std::move(piece));                                                
	}
	std::stable_sort(done.begin(), done.end(),
		[](const m::Manifold& a, const m::Manifold& b) { return a.Volume() > b.Volume(); });
	if (done.size() > cap) done.resize(cap);                                      
	for (m::Manifold& d : done) pieces.push_back(std::move(d));
}
PROJECT_DAMAGE_NS_MACRO_END
