// добавляем сетку только рядом с повреждением
// ProjectDamage adaptive refinement — GPL-3.0-or-later; see LICENSE.
//
// RefineNearDamage: before the Booleans, the source's triangles near the
// damage zones are split so the cut / buildup outlines are surrounded by
// well-shaped triangles that grow smoothly with distance, instead of the
// long fans Manifold otherwise draws from an outline to the far corners of
// a big source polygon (shading streaks on smooth low-poly surfaces).
//
//   size field  t(x) = nearSize + grade * dist(x, nearest damage box)
//   level loop  a triangle whose longest edge exceeds t(centroid) marks its
//               three GEOMETRIC edges; then a triangle with three marked
//               edges splits 1->4 (red), two -> 1->3 (blue), one -> 1->2
//               (green) - every split edge is split on both sides:
//               conforming, watertight, and the marks never cascade.
//   attributes  run / originalID and faceID inherited by the children;
//               the vertex properties (UVs ...) interpolated at an edge's
//               midpoint PER property edge, so UV seams survive: the two
//               property vertices of one geometric midpoint are tied with a
//               merge pair. Positions stay on the source triangles exactly.
//   limits      maxLevels passes, maxTris triangles.
#include "manifold/manifold.h"
#include "adaptive_refine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

struct Grid {
	double cell = 1.0;
	m::vec3 lo{0.0};
	std::unordered_map<uint64_t, std::vector<int32_t>> map;
	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	void Cell(const m::vec3& p, int64_t& x, int64_t& y, int64_t& z) const
	{
		x = int64_t(std::floor((p.x - lo.x) / cell));
		y = int64_t(std::floor((p.y - lo.y) / cell));
		z = int64_t(std::floor((p.z - lo.z) / cell));
	}
};

double BoxDist(const m::vec3& p, const m::Box& b)
{
	const m::vec3 d = m::la::max(m::la::max(b.min - p, p - b.max), m::vec3(0.0));
	return m::la::length(d);
}

}               

// добавляем детали около повреждения, не уплотняя весь объект
m::Manifold RefineNearDamage(const m::Manifold& base, const std::vector<m::Box>& zones,
	double nearSize, double grade, int maxLevels, size_t maxTris, const std::vector<Int32>* onlyRuns, const AdaptiveTriangleSize* triangleSize)
{
	if ((zones.empty() && onlyRuns == nullptr && triangleSize == nullptr) || nearSize <= 0.0 || base.IsEmpty() || maxLevels <= 0) return base;
	m::MeshGL64 mg = base.GetMeshGL64();
	const size_t np = mg.numProp;
	if (np < 3 || mg.NumTri() == 0) return base;

	                                                         
	std::vector<uint64_t> canon(mg.NumVert());
	for (size_t v = 0; v < canon.size(); ++v) canon[v] = v;
	for (size_t i = 0; i < mg.mergeFromVert.size(); ++i)
		canon[(size_t)mg.mergeFromVert[i]] = mg.mergeToVert[i];
	for (size_t v = 0; v < canon.size(); ++v) {                             
		uint64_t r = canon[v];
		int guard = 0;
		while (canon[(size_t)r] != r && guard++ < 64) r = canon[(size_t)r];
		canon[v] = r;
	}
	std::vector<double> props(mg.vertProperties.begin(), mg.vertProperties.end());
	std::vector<std::pair<uint64_t, uint64_t>> merges;
	for (size_t i = 0; i < mg.mergeFromVert.size(); ++i) merges.push_back({mg.mergeFromVert[i], mg.mergeToVert[i]});

	struct Tri { uint64_t v[3]; uint64_t face; size_t run; };
	std::vector<Tri> tris;
	tris.reserve(mg.NumTri());
	const size_t nRun = mg.runIndex.size() > 1 ? mg.runIndex.size() - 1 : 1;
	std::vector<char> selectedRuns(nRun, 0);
	if (onlyRuns != nullptr) {
		const std::unordered_set<Int32> selected(onlyRuns->begin(), onlyRuns->end());
		for (size_t r = 0; r < nRun && r < mg.runOriginalID.size(); ++r)
			selectedRuns[r] = selected.count((Int32)mg.runOriginalID[r]) != 0;
	}
	for (size_t r = 0; r < nRun; ++r) {
		const size_t t0 = mg.runIndex.size() > 1 ? mg.runIndex[r] / 3 : 0;
		const size_t t1 = mg.runIndex.size() > 1 ? mg.runIndex[r + 1] / 3 : mg.NumTri();
		for (size_t t = t0; t < t1; ++t) {
			Tri T;
			for (int k = 0; k < 3; ++k) T.v[k] = mg.triVerts[3 * t + (size_t)k];
			T.face = t < mg.faceID.size() ? mg.faceID[t] : t;
			T.run = r;
			tris.push_back(T);
		}
	}
	auto pos = [&](uint64_t v) { return m::vec3(props[v * np], props[v * np + 1], props[v * np + 2]); };

	                                                 
	m::vec3 zlo(1e300), zhi(-1e300);
	double zsize = 0.0;
	for (const m::Box& b : zones) { zlo = m::la::min(zlo, b.min); zhi = m::la::max(zhi, b.max); zsize += m::la::length(b.Size()); }
	Grid zg;
	zg.cell = std::max(1e-6, (zones.empty()?nearSize:zsize / zones.size()));
	zg.lo = zones.empty()?m::vec3(0.0):zlo;
	for (size_t i = 0; i < zones.size(); ++i) {
		int64_t x0, y0, z0, x1, y1, z1;
		zg.Cell(zones[i].min, x0, y0, z0);
		zg.Cell(zones[i].max, x1, y1, z1);
		if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) { x1 = x0; y1 = y0; z1 = z0; }                                
		for (int64_t x = x0; x <= x1; ++x)
			for (int64_t y = y0; y <= y1; ++y)
				for (int64_t z = z0; z <= z1; ++z) zg.map[zg.Key(x, y, z)].push_back(int32_t(i));
	}
	auto target = [&](const m::vec3& p, double longest) {
		                                                                   
		                                                             
		const double reach = grade > 0.0 ? (longest - nearSize) / grade : 0.0;
		double best = 1e300;
		int64_t cx, cy, cz;
		zg.Cell(p, cx, cy, cz);
		const int rings = std::min<int>(24, int(std::ceil(std::max(0.0, reach) / zg.cell)) + 1);
		for (int r = 0; r <= rings; ++r) {
			for (int64_t x = cx - r; x <= cx + r; ++x)
				for (int64_t y = cy - r; y <= cy + r; ++y)
					for (int64_t z = cz - r; z <= cz + r; ++z) {
						if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != r) continue;
						auto it = zg.map.find(zg.Key(x, y, z));
						if (it == zg.map.end()) continue;
						for (int32_t bi : it->second) best = std::min(best, BoxDist(p, zones[(size_t)bi]));
					}
			if (best < (r - 1) * zg.cell) break;
		}
		                                                                        
		                                                    
		return best >= 1e299 ? 1e300 : nearSize + grade * best;
	};

	for (int level = 0; level < maxLevels && tris.size() < maxTris; ++level) {
		                                               
		std::map<std::pair<uint64_t, uint64_t>, uint32_t> marked;                            
		auto gkey = [&](uint64_t a, uint64_t b) {
			const uint64_t ca = canon[a], cb = canon[b];
			return ca < cb ? std::make_pair(ca, cb) : std::make_pair(cb, ca);
		};
		std::vector<char> red(tris.size(), 0);
		size_t want = 0;
		for (size_t t = 0; t < tris.size(); ++t) {
			if (onlyRuns != nullptr && !selectedRuns[tris[t].run]) continue;
			const m::vec3 a = pos(tris[t].v[0]), b = pos(tris[t].v[1]), c = pos(tris[t].v[2]);
			const double longest = std::max({m::la::length(b - a), m::la::length(c - b), m::la::length(a - c)});
			if (longest <= nearSize) continue;
			if (onlyRuns == nullptr && longest <= (triangleSize ? (*triangleSize)(a,b,c) : target((a + b + c) / 3.0, longest))) continue;
			red[t] = 1;
			++want;
		}
		if (want == 0) break;
		if (onlyRuns == nullptr && tris.size() + 3 * want > maxTris) break;
		for (size_t t = 0; t < tris.size(); ++t)
		{
			if (!red[t]) continue;
			if (onlyRuns != nullptr)
			{
				                                                                   
				                                                           
				int kl = 0;
				double best = -1.0;
				for (int k = 0; k < 3; ++k)
				{
					const double l = m::la::length(pos(tris[t].v[(k + 1) % 3]) - pos(tris[t].v[k]));
					if (l > best) { best = l; kl = k; }
				}
				marked[gkey(tris[t].v[kl], tris[t].v[(kl + 1) % 3])] = 1;
			}
			else
				for (int k = 0; k < 3; ++k) marked[gkey(tris[t].v[k], tris[t].v[(k + 1) % 3])] = 1;
		}
		                                                                     
		                                                                 
		                                                          
		std::map<std::pair<uint64_t, uint64_t>, uint64_t> midOf;                                        
		std::map<std::pair<uint64_t, uint64_t>, uint64_t> geoMid;                                              
		auto mid = [&](uint64_t a, uint64_t b) {
			const auto pk = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
			auto it = midOf.find(pk);
			if (it != midOf.end()) return it->second;
			const uint64_t nv = props.size() / np;
			for (size_t j = 0; j < np; ++j) props.push_back(0.5 * (props[a * np + j] + props[b * np + j]));
			const auto gk = gkey(a, b);
			auto g = geoMid.find(gk);
			if (g == geoMid.end()) { geoMid[gk] = nv; canon.push_back(nv); }
			else { canon.push_back(canon[g->second]); merges.push_back({nv, g->second}); }
			midOf[pk] = nv;
			return nv;
		};
		if (onlyRuns != nullptr && marked.size() > (maxTris - tris.size()) / 2) break;
		std::vector<Tri> next;
		next.reserve(tris.size() + (onlyRuns != nullptr ? 2 * marked.size() : 3 * want));
		for (size_t t = 0; t < tris.size(); ++t) {
			const Tri& T = tris[t];
			bool e[3];
			int n = 0;
			for (int k = 0; k < 3; ++k) { e[k] = marked.count(gkey(T.v[k], T.v[(k + 1) % 3])) > 0; n += e[k]; }
			if (n == 0) { next.push_back(T); continue; }
			if (n == 3) {
				const uint64_t m0 = mid(T.v[0], T.v[1]), m1 = mid(T.v[1], T.v[2]), m2 = mid(T.v[2], T.v[0]);
				next.push_back({{T.v[0], m0, m2}, T.face, T.run});
				next.push_back({{m0, T.v[1], m1}, T.face, T.run});
				next.push_back({{m2, m1, T.v[2]}, T.face, T.run});
				next.push_back({{m0, m1, m2}, T.face, T.run});
				continue;
			}
			if (n == 2) {
				                                                       
				int k = 0;
				while (e[k]) ++k;
				const uint64_t x = T.v[k], y = T.v[(k + 1) % 3], z = T.v[(k + 2) % 3];
				const uint64_t m1 = mid(y, z), m2 = mid(z, x);
				next.push_back({{x, y, m1}, T.face, T.run});
				next.push_back({{x, m1, m2}, T.face, T.run});
				next.push_back({{m2, m1, z}, T.face, T.run});
				continue;
			}
			                                                              
			int k = 0;
			while (!e[k]) ++k;
			const uint64_t a = T.v[k], b = T.v[(k + 1) % 3], c = T.v[(k + 2) % 3];
			const uint64_t mm = mid(a, b);
			next.push_back({{a, mm, c}, T.face, T.run});
			next.push_back({{mm, b, c}, T.face, T.run});
		}
		tris.swap(next);
	}
	if (tris.size() == mg.NumTri()) return base;                         

	                                                 
	std::stable_sort(tris.begin(), tris.end(), [](const Tri& x, const Tri& y) { return x.run < y.run; });
	m::MeshGL64 out;
	out.numProp = (uint32_t)np;
	out.vertProperties = std::move(props);
	out.triVerts.reserve(tris.size() * 3);
	out.faceID.reserve(tris.size());
	out.runIndex.clear();
	out.runOriginalID = mg.runOriginalID;
	out.runTransform = mg.runTransform;
	size_t run = 0;
	out.runIndex.push_back(0);
	for (size_t t = 0; t < tris.size(); ++t) {
		while (run < tris[t].run) { out.runIndex.push_back(t * 3); ++run; }
		for (int k = 0; k < 3; ++k) out.triVerts.push_back(tris[t].v[k]);
		out.faceID.push_back(tris[t].face);
	}
	while (out.runIndex.size() < mg.runIndex.size()) out.runIndex.push_back(tris.size() * 3);
	if (mg.runIndex.size() <= 1) { out.runIndex.clear(); out.runOriginalID.clear(); out.runTransform.clear(); }
	for (const auto& mpair : merges) { out.mergeFromVert.push_back(mpair.first); out.mergeToVert.push_back(mpair.second); }
	m::Manifold refined(out);
	if (refined.Status() != m::Manifold::Error::NoError || refined.IsEmpty()) return base;
	return refined;
}
PROJECT_DAMAGE_NS_MACRO_END
