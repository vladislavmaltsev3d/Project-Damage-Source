// помечаем повреждённые грани для отдельного сглаживания
// ProjectDamage damage shading — GPL-3.0-or-later; see LICENSE.
// See damage_shading.h.
#include "manifold/manifold.h"
#include "damage_shading.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN

namespace {
                                                                            
                     
double ClosestTri(const m::vec3& p, const m::vec3& a, const m::vec3& b, const m::vec3& c, m::vec3& bary)
{
	const m::vec3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) { bary = m::vec3(1, 0, 0); return m::la::dot(ap, ap); }
	const m::vec3 bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) { bary = m::vec3(0, 1, 0); return m::la::dot(bp, bp); }
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double t = d1 / (d1 - d3); bary = m::vec3(1 - t, t, 0); const m::vec3 q = a + ab * t; return m::la::dot(p - q, p - q); }
	const m::vec3 cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) { bary = m::vec3(0, 0, 1); return m::la::dot(cp, cp); }
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double t = d2 / (d2 - d6); bary = m::vec3(1 - t, 0, t); const m::vec3 q = a + ac * t; return m::la::dot(p - q, p - q); }
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		bary = m::vec3(0, 1 - t, t);
		const m::vec3 q = b + (c - b) * t;
		return m::la::dot(p - q, p - q);
	}
	const double den = 1.0 / (va + vb + vc);
	bary = m::vec3(1.0 - vb * den - vc * den, vb * den, vc * den);
	const m::vec3 q = a + ab * (vb * den) + ac * (vc * den);
	return m::la::dot(p - q, p - q);
}

                                                                            
                                                           
struct SourceField {
	const MeshView* v = nullptr;
	std::vector<m::vec3> cn;                                 
	struct Tri { Int32 poly; int k[3]; m::vec3 n; };                       
	std::vector<Tri> tris;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;
	m::vec3 lo{0.0};
	double cell = 1.0, tol = 1e-6;
	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	m::vec3 P(Int32 poly, int k) const
	{
		const ProjectDamagePoly& q = v->polys[poly];
		const Int32 id = k == 0 ? q.a : k == 1 ? q.b : k == 2 ? q.c : q.d;
		return v->pts[id];
	}
	void Build(const MeshView& view, const std::vector<double>* hostNormals, double baseAngle)
	{
		v = &view;
		const size_t np = size_t(std::max(0, view.npolys));
		cn.assign(np * 4, m::vec3(0.0));
		m::vec3 hi(-1e300);
		lo = m::vec3(1e300);
		double edge = 0.0;
		std::vector<m::vec3> fnrm(np);
		std::vector<std::vector<std::pair<int32_t, int>>> around(size_t(std::max(0, view.npts)));
		for (size_t p = 0; p < np; ++p) {
			const ProjectDamagePoly& q = view.polys[p];
			const int n = q.c == q.d ? 3 : 4;
			m::vec3 nn(0.0);
			for (int k = 0; k < n; ++k) {
				const m::vec3 a = P(Int32(p), k), b = P(Int32(p), (k + 1) % n);
				nn += m::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
				lo = m::la::min(lo, a); hi = m::la::max(hi, a);
				edge += m::la::length(b - a);
				const Int32 id = k == 0 ? q.a : k == 1 ? q.b : k == 2 ? q.c : q.d;
				around[size_t(id)].push_back({int32_t(p), k});
			}
			const double l = m::la::length(nn);
			fnrm[p] = l > 1e-20 ? nn / l : m::vec3(0.0);
			tris.push_back({Int32(p), {0, 1, 2}, fnrm[p]});
			if (n == 4) tris.push_back({Int32(p), {0, 2, 3}, fnrm[p]});
		}
		const bool host = hostNormals != nullptr && hostNormals->size() == np * 12;
		const double cosLim = std::cos(std::min(180.0, std::max(0.0, baseAngle)) * 3.14159265358979323846 / 180.0);
		for (size_t p = 0; p < np; ++p) {
			const ProjectDamagePoly& q = view.polys[p];
			const int n = q.c == q.d ? 3 : 4;
			for (int k = 0; k < 4; ++k) {
				const int kk = std::min(k, n - 1);
				if (host) {
					const double* h = &(*hostNormals)[p * 12 + size_t(kk) * 3];
					const m::vec3 hn(h[0], h[1], h[2]);
					const double l = m::la::length(hn);
					cn[p * 4 + size_t(k)] = l > 1e-20 ? hn / l : fnrm[p];
					continue;
				}
				const Int32 id = kk == 0 ? q.a : kk == 1 ? q.b : kk == 2 ? q.c : q.d;
				m::vec3 sum(0.0);
				for (auto [o, ko] : around[size_t(id)]) {
					if (m::la::dot(fnrm[size_t(o)], fnrm[p]) < cosLim) continue;
					const ProjectDamagePoly& oq = view.polys[o];
					const int on = oq.c == oq.d ? 3 : 4;
					const m::vec3 c0 = P(o, ko), e1 = P(o, (ko + 1) % on) - c0, e2 = P(o, (ko + on - 1) % on) - c0;
					const double l2 = m::la::length(e1) * m::la::length(e2);
					sum += fnrm[size_t(o)] * (l2 > 0 ? std::acos(std::max(-1.0, std::min(1.0, m::la::dot(e1, e2) / l2))) : 0.0);
				}
				const double l = m::la::length(sum);
				cn[p * 4 + size_t(k)] = l > 1e-20 ? sum / l : fnrm[p];
			}
		}
		if (tris.empty()) return;
		cell = std::max(1e-6, 1.5 * edge / double(std::max<size_t>(1, np * 4)));
		cell = std::max(cell, m::la::length(hi - lo) / 400.0);
		tol = std::max(1e-6, 1e-4 * m::la::length(hi - lo));
		for (size_t i = 0; i < tris.size(); ++i) {
			const Tri& t = tris[i];
			m::vec3 a = P(t.poly, t.k[0]), b = a;
			for (int j = 1; j < 3; ++j) { const m::vec3 x = P(t.poly, t.k[j]); a = m::la::min(a, x); b = m::la::max(b, x); }
			for (int64_t x = int64_t(std::floor((a.x - lo.x) / cell)); x <= int64_t(std::floor((b.x - lo.x) / cell)); ++x)
				for (int64_t y = int64_t(std::floor((a.y - lo.y) / cell)); y <= int64_t(std::floor((b.y - lo.y) / cell)); ++y)
					for (int64_t z = int64_t(std::floor((a.z - lo.z) / cell)); z <= int64_t(std::floor((b.z - lo.z) / cell)); ++z)
						grid[Key(x, y, z)].push_back(int32_t(i));
		}
	}
	                                                                         
	                                                      
	bool At(const m::vec3& p, const m::vec3& n, Int32 poly, m::vec3& out) const
	{
		double best = 1e300;
		const Tri* bt = nullptr;
		m::vec3 bb(0.0);
		auto test = [&](const Tri& t) {
			if (m::la::dot(t.n, n) < 0.3) return;
			m::vec3 bary;
			const double d = ClosestTri(p, P(t.poly, t.k[0]), P(t.poly, t.k[1]), P(t.poly, t.k[2]), bary);
			if (d < best) { best = d; bt = &t; bb = bary; }
		};
		const int64_t cx = int64_t(std::floor((p.x - lo.x) / cell)), cy = int64_t(std::floor((p.y - lo.y) / cell)),
			cz = int64_t(std::floor((p.z - lo.z) / cell));
		for (int64_t x = cx - 1; x <= cx + 1; ++x)
			for (int64_t y = cy - 1; y <= cy + 1; ++y)
				for (int64_t z = cz - 1; z <= cz + 1; ++z) {
					auto it = grid.find(Key(x, y, z));
					if (it == grid.end()) continue;
					for (int32_t ti : it->second)
						if (poly < 0 || tris[size_t(ti)].poly == poly) test(tris[size_t(ti)]);
				}
		if (bt == nullptr || std::sqrt(best) > tol) return false;
		const m::vec3 nn = cn[size_t(bt->poly) * 4 + size_t(bt->k[0])] * bb.x
			+ cn[size_t(bt->poly) * 4 + size_t(bt->k[1])] * bb.y + cn[size_t(bt->poly) * 4 + size_t(bt->k[2])] * bb.z;
		const double l = m::la::length(nn);
		if (l < 1e-20) return false;
		out = nn / l;
		return true;
	}
};
}               

void ComputeDamageShading(const std::vector<EVec>& points, const std::vector<ProjectDamagePoly>& polys,
	const std::vector<unsigned char>& group, double damageAngle, double baseAngle,
	std::vector<double>& cornerNormals, const MeshView* source,
	const std::vector<double>* srcCornerNormals, const std::vector<Int32>* basePolyOf)
{
	const size_t np = polys.size();
	cornerNormals.assign(np * 12, 0.0);
	if (np == 0 || points.empty()) return;
	auto corners = [&](const ProjectDamagePoly& q, Int32 c[4]) {
		c[0] = q.a; c[1] = q.b; c[2] = q.c; c[3] = q.d;
		return q.c == q.d ? 3 : 4;
	};
	                                          
	std::vector<m::vec3> fn(np, m::vec3(0.0));
	std::vector<std::array<double, 4>> ang(np, {0.0, 0.0, 0.0, 0.0});
	std::vector<std::vector<int32_t>> around(points.size());
	for (size_t p = 0; p < np; ++p) {
		Int32 c[4];
		const int n = corners(polys[p], c);
		m::vec3 nn(0.0);
		for (int k = 0; k < n; ++k) {
			const m::vec3 a = points[(size_t)c[k]], b = points[(size_t)c[(k + 1) % n]];
			nn += m::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
		}
		const double l = m::la::length(nn);
		fn[p] = l > 1e-20 ? nn / l : m::vec3(0.0);
		for (int k = 0; k < n; ++k) {
			const m::vec3 v = points[(size_t)c[k]];
			const m::vec3 e1 = points[(size_t)c[(k + 1) % n]] - v, e2 = points[(size_t)c[(k + n - 1) % n]] - v;
			const double l2 = m::la::length(e1) * m::la::length(e2);
			ang[p][(size_t)k] = l2 > 0.0 ? std::acos(std::max(-1.0, std::min(1.0, m::la::dot(e1, e2) / l2))) : 0.0;
			around[(size_t)c[k]].push_back((int32_t)p);
		}
	}
	const double kDeg = 3.14159265358979323846 / 180.0;
	const double cosDamage = std::cos(std::min(180.0, std::max(0.0, damageAngle)) * kDeg);
	const double cosBase = std::cos(std::min(180.0, std::max(0.0, baseAngle)) * kDeg);
	auto grp = [&](size_t p) { return p < group.size() ? group[p] : (unsigned char)0; };
	                                                                           
	                                                                        
	                                                                          
	                                             
	for (size_t v = 0; v < around.size(); ++v) {
		const std::vector<int32_t>& fs = around[v];
		if (fs.empty()) continue;
		const size_t m_ = fs.size();
		std::vector<int32_t> parent(m_);
		for (size_t i = 0; i < m_; ++i) parent[i] = (int32_t)i;
		auto find = [&](int32_t x) { while (parent[(size_t)x] != x) x = parent[(size_t)x] = parent[(size_t)parent[(size_t)x]]; return x; };
		                                        
		auto nb = [&](int32_t p, Int32& prev, Int32& next) {
			Int32 c[4];
			const int n = corners(polys[(size_t)p], c);
			for (int k = 0; k < n; ++k)
				if ((size_t)c[k] == v) { prev = c[(k + n - 1) % n]; next = c[(k + 1) % n]; return; }
			prev = next = -1;
		};
		for (size_t i = 0; i < m_; ++i) {
			Int32 pi, ni;
			nb(fs[i], pi, ni);
			for (size_t j = i + 1; j < m_; ++j) {
				Int32 pj, nj;
				nb(fs[j], pj, nj);
				const bool share = (pi >= 0) && (pi == pj || pi == nj || ni == pj || ni == nj);
				if (!share) continue;
				const size_t a2 = (size_t)fs[i], b2 = (size_t)fs[j];
				if (grp(a2) != grp(b2)) continue;                                             
				const double cosLim = grp(a2) == 1 ? cosDamage : cosBase;
				if (m::la::dot(fn[a2], fn[b2]) < cosLim) continue;                  
				const int32_t ra = find((int32_t)i), rb = find((int32_t)j);
				if (ra != rb) parent[(size_t)ra] = rb;
			}
		}
		std::vector<m::vec3> sum(m_, m::vec3(0.0));
		std::vector<int> corner(m_, 0);
		for (size_t i = 0; i < m_; ++i) {
			Int32 c[4];
			const int n = corners(polys[(size_t)fs[i]], c);
			for (int k = 0; k < n; ++k) if ((size_t)c[k] == v) { corner[i] = k; break; }
			sum[(size_t)find((int32_t)i)] += fn[(size_t)fs[i]] * ang[(size_t)fs[i]][(size_t)corner[i]];
		}
		for (size_t i = 0; i < m_; ++i) {
			const size_t p = (size_t)fs[i];
			const m::vec3 s2 = sum[(size_t)find((int32_t)i)];
			const double l = m::la::length(s2);
			const m::vec3 o = l > 1e-20 ? s2 / l : fn[p];
			const int n = polys[p].c == polys[p].d ? 3 : 4;
			for (int k = 0; k < 4; ++k) {
				if (std::min(k, n - 1) != corner[i]) continue;
				cornerNormals[p * 12 + (size_t)k * 3 + 0] = o.x;
				cornerNormals[p * 12 + (size_t)k * 3 + 1] = o.y;
				cornerNormals[p * 12 + (size_t)k * 3 + 2] = o.z;
			}
		}
	}
	                                                                        
	                                                                   
	                                                             
	{
		std::vector<m::vec3> vsum(points.size(), m::vec3(0.0));
		for (size_t p = 0; p < np; ++p) {
			if (m::la::length(fn[p]) < 0.5) continue;
			Int32 c[4];
			const int n = corners(polys[p], c);
			for (int k = 0; k < n; ++k)
				vsum[(size_t)c[k]] += m::vec3(cornerNormals[p * 12 + (size_t)k * 3], cornerNormals[p * 12 + (size_t)k * 3 + 1],
					cornerNormals[p * 12 + (size_t)k * 3 + 2]);
		}
		for (size_t p = 0; p < np; ++p) {
			if (m::la::length(fn[p]) >= 0.5) continue;
			Int32 c[4];
			const int n = corners(polys[p], c);
			for (int k = 0; k < 4; ++k) {
				const m::vec3 s2 = vsum[(size_t)c[std::min(k, n - 1)]];
				const double l = m::la::length(s2);
				const m::vec3 o = l > 1e-20 ? s2 / l : m::vec3(0.0, 0.0, 1.0);
				cornerNormals[p * 12 + (size_t)k * 3 + 0] = o.x;
				cornerNormals[p * 12 + (size_t)k * 3 + 1] = o.y;
				cornerNormals[p * 12 + (size_t)k * 3 + 2] = o.z;
			}
		}
	}
	                                                                      
	if (source != nullptr && source->npolys > 0) {
		SourceField sf;
		sf.Build(*source, srcCornerNormals, baseAngle);
		const bool mapped = basePolyOf != nullptr && basePolyOf->size() == np;
		for (size_t p = 0; p < np; ++p) {
			if (grp(p) != 0 || m::la::length(fn[p]) < 0.5) continue;
			Int32 c[4];
			const int n = corners(polys[p], c);
			const Int32 sp = mapped && (*basePolyOf)[p] >= 0 && (*basePolyOf)[p] < source->npolys ? (*basePolyOf)[p] : -1;
			for (int k = 0; k < 4; ++k) {
				const m::vec3 x = points[(size_t)c[std::min(k, n - 1)]];
				m::vec3 o;
				                                                              
				                                                             
				if (!sf.At(x, fn[p], sp, o) && !(sp >= 0 && sf.At(x, fn[p], -1, o))
					&& !sf.At(x, fn[p] * -1.0, -1, o)) continue;
				cornerNormals[p * 12 + (size_t)k * 3 + 0] = o.x;
				cornerNormals[p * 12 + (size_t)k * 3 + 1] = o.y;
				cornerNormals[p * 12 + (size_t)k * 3 + 2] = o.z;
			}
		}
	}
}
PROJECT_DAMAGE_NS_MACRO_END
