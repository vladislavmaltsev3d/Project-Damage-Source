// общие объявления: небольшие операции изменения сетки и её связей
// ProjectDamage damage generators: editing an existing mesh - GPL-3.0-or-later; see LICENSE.
//
// Header-only helpers for the "on an object" modes of the generators (torn
// metal, split wood): the object's own mesh is changed instead of building a
// new one from scratch.
//
//   MeshInput       the object's mesh: points, polygons, material per face,
//                   UV per face corner (all optional but the points and faces)
//   EditMesh        the mesh while it is edited: the faces near the damage are
//                   triangles (refined, cut, moved), the others stay as they
//                   are (quads and n-gons are kept; a split edge only inserts a
//                   vertex into them)
//   Refine          splits edges until a metric says they are short enough
//   SnapToContour   moves vertices that lie close to a cut onto it (no slivers)
//   ClipSide        keeps one side { f >= 0 } or { f <= 0 } of a field given
//                   per vertex; the cut follows the field (crossings found by
//                   the caller, usually by bisection of the real field)
//   DropSmallPieces removes loose bits left by the cuts
//   BoundaryLoops   the open borders as closed vertex loops
//   EmitEditMesh    -> GenMesh (only the used vertices)
#pragma once
#include "damage_gen_common.h"

#include <queue>

namespace pdgen {

using UV2 = std::array<double, 2>;

struct MeshInput
{
	std::vector<V3> points;
	std::vector<int32_t> faceSizes, faceVerts, faceMat;   // faceMat may be empty (all 0)
	std::vector<UV2> loopUv;                              // one per face corner, or empty
	std::vector<uint8_t> faceSmooth;                      // per face, or empty (all smooth)
	double smoothAngle = 40.0;                            // degrees: faces meeting at more are a crease
};

struct EditTri
{
	std::array<int32_t, 3> v{};
	std::array<UV2, 3> uv{};
	std::array<V3, 3> n{};                                // shading normal per corner (zero: flat)
	int32_t mat = 0;
	int32_t tag = 0;                                      // caller's use (0: the object's own surface)
};

struct EditPoly
{
	std::vector<int32_t> v;
	std::vector<UV2> uv;
	int32_t mat = 0;
};

struct EditMesh
{
	// a vertex made on the edge (a, b) at t from a (a < 0: an input vertex)
	struct Origin { int32_t a = -1, b = -1; double t = 0.0; };
	std::vector<V3> p;
	std::vector<Origin> origin;
	std::vector<uint8_t> cut;                             // 1: on a cut
	std::vector<EditTri> tris;                            // the edited part
	std::vector<EditPoly> polys;                          // untouched faces
	std::unordered_map<uint64_t, int32_t> mids;           // every edge split made by Refine

	int32_t AddVertex(const V3& q, int32_t a = -1, int32_t b = -1, double t = 0.0, uint8_t c = 0)
	{
		p.push_back(q);
		origin.push_back({ a, b, t });
		cut.push_back(c);
		return int32_t(p.size() - 1);
	}
};

inline uint64_t EdgeKey(int32_t a, int32_t b)
{
	const uint32_t x = uint32_t(a), y = uint32_t(b);
	return (uint64_t(std::min(x, y)) << 32) | uint64_t(std::max(x, y));
}

inline UV2 LerpUv(const UV2& a, const UV2& b, double t) { return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t }; }

inline V3 PolyNormal(const std::vector<V3>& P, const std::vector<int32_t>& idx)
{
	V3 n;
	const size_t k = idx.size();
	for (size_t i = 0; i < k; ++i)
	{
		const V3& a = P[idx[i]];
		const V3& b = P[idx[(i + 1) % k]];
		n.x += (a.y - b.y) * (a.z + b.z);
		n.y += (a.z - b.z) * (a.x + b.x);
		n.z += (a.x - b.x) * (a.y + b.y);
	}
	return n;
}

// Ear clipping of one polygon (indices into P) in its best-fit plane; the
// triangles are corner numbers 0..n-1 of the polygon, wound like it.
inline void TriangulatePolygon(const std::vector<V3>& P, const std::vector<int32_t>& idx, std::vector<std::array<int32_t, 3>>& out)
{
	const int n = int(idx.size());
	if (n < 3) return;
	if (n == 3) { out.push_back({ 0, 1, 2 }); return; }
	const V3 nrm = Norm(PolyNormal(P, idx));
	const V3 ax = std::abs(nrm.x) < 0.9 ? V3(1, 0, 0) : V3(0, 1, 0);
	const V3 e1 = Norm(Cross(nrm, ax)), e2 = Cross(nrm, e1);
	std::vector<V2> q(n);
	for (int i = 0; i < n; ++i) q[i] = V2(Dot(P[idx[i]], e1), Dot(P[idx[i]], e2));
	auto cross2 = [&](int a, int b, int c) { return (q[b].x - q[a].x) * (q[c].y - q[a].y) - (q[b].y - q[a].y) * (q[c].x - q[a].x); };
	if (n == 4)
	{
		// a quad: the shorter diagonal when both halves are fine
		const bool d02 = cross2(0, 1, 2) > 0 && cross2(0, 2, 3) > 0;
		const bool d13 = cross2(1, 2, 3) > 0 && cross2(1, 3, 0) > 0;
		const double l02 = Len(P[idx[0]] - P[idx[2]]), l13 = Len(P[idx[1]] - P[idx[3]]);
		if (d02 && (!d13 || l02 <= l13)) { out.push_back({ 0, 1, 2 }); out.push_back({ 0, 2, 3 }); return; }
		if (d13) { out.push_back({ 1, 2, 3 }); out.push_back({ 1, 3, 0 }); return; }
	}
	std::vector<int> rem(n);
	for (int i = 0; i < n; ++i) rem[i] = i;
	auto inside = [&](int a, int b, int c, int p)
	{
		return cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0;
	};
	int guard = 0;
	while (rem.size() > 3 && guard++ < 4 * n * n)
	{
		const int m = int(rem.size());
		bool found = false;
		for (int i = 0; i < m && !found; ++i)
		{
			const int a = rem[(i + m - 1) % m], b = rem[i], c = rem[(i + 1) % m];
			if (cross2(a, b, c) <= 1e-18) continue;
			bool ok = true;
			for (int j = 0; j < m && ok; ++j)
			{
				const int p = rem[j];
				if (p == a || p == b || p == c) continue;
				if (inside(a, b, c, p)) ok = false;
			}
			if (!ok) continue;
			out.push_back({ a, b, c });
			rem.erase(rem.begin() + i);
			found = true;
		}
		if (!found) break;
	}
	for (size_t k = 1; k + 1 < rem.size(); ++k) out.push_back({ rem[0], rem[k], rem[k + 1] });
}

// active(polygon vertex indices) -> the face is edited (triangulated), else kept.
inline EditMesh MakeEditMesh(const MeshInput& in, const std::function<bool(const std::vector<int32_t>&)>& active)
{
	EditMesh m;
	m.p = in.points;
	m.origin.assign(m.p.size(), EditMesh::Origin());
	m.cut.assign(m.p.size(), 0);
	// face normals and, per vertex, its faces: the smooth corner normals
	const size_t nf = in.faceSizes.size();
	std::vector<V3> fn(nf);
	std::vector<size_t> fstart(nf);
	std::vector<std::vector<int32_t>> vf(m.p.size());
	{
		size_t L0 = 0;
		std::vector<int32_t> ix;
		for (size_t f = 0; f < nf; ++f)
		{
			const int sz = in.faceSizes[f];
			fstart[f] = L0;
			ix.assign(in.faceVerts.begin() + L0, in.faceVerts.begin() + L0 + sz);
			fn[f] = PolyNormal(m.p, ix);
			for (int32_t v : ix) vf[v].push_back(int32_t(f));
			L0 += size_t(sz);
		}
	}
	const double cosA = std::cos(Clamp(in.smoothAngle, 0.0, 180.0) * kPi / 180.0);
	auto cornerNormal = [&](size_t f, int32_t v) -> V3
	{
		const V3 nf0 = Norm(fn[f], V3(0, 0, 0));
		if (f < in.faceSmooth.size() && !in.faceSmooth[f]) return V3(0, 0, 0);
		V3 acc;
		for (int32_t g : vf[v])
		{
			if (size_t(g) < in.faceSmooth.size() && !in.faceSmooth[g]) continue;
			if (Dot(Norm(fn[g], V3(0, 0, 0)), nf0) >= cosA) acc += fn[g];
		}
		return Norm(acc, V3(0, 0, 0));
	};
	size_t L = 0;
	std::vector<int32_t> idx;
	std::vector<UV2> uv;
	std::vector<std::array<int32_t, 3>> tri;
	for (size_t f = 0; f < in.faceSizes.size(); ++f)
	{
		const int sz = in.faceSizes[f];
		idx.assign(in.faceVerts.begin() + L, in.faceVerts.begin() + L + sz);
		uv.resize(sz);
		for (int k = 0; k < sz; ++k) uv[k] = L + k < in.loopUv.size() ? in.loopUv[L + k] : UV2{ 0.0, 0.0 };
		const int32_t mat = f < in.faceMat.size() ? in.faceMat[f] : 0;
		L += size_t(sz);
		if (sz < 3) continue;
		if (active(idx))
		{
			tri.clear();
			TriangulatePolygon(m.p, idx, tri);
			for (const auto& t : tri)
			{
				EditTri e;
				for (int k = 0; k < 3; ++k) { e.v[k] = idx[t[k]]; e.uv[k] = uv[t[k]]; e.n[k] = cornerNormal(f, idx[t[k]]); }
				e.mat = mat;
				m.tris.push_back(e);
			}
		}
		else m.polys.push_back({ idx, uv, mat });
	}
	return m;
}

// Splits every triangle edge for which split(a, b) holds, pass after pass,
// until none is left (or maxTris would be passed). Conforming: a split edge
// is split in all its triangles; kept polygons get the new vertex in
// EmitEditMesh.
inline void Refine(EditMesh& m, const std::function<bool(int32_t, int32_t)>& split, size_t maxTris, int maxPasses = 24)
{
	for (int pass = 0; pass < maxPasses; ++pass)
	{
		std::unordered_map<uint64_t, int32_t> sp;
		std::vector<uint64_t> order;
		// shading normals at the two ends (lo, hi) as the first triangle has them;
		// a crease (the triangles disagree) or a flat face keeps the straight midpoint
		struct EdgeN { V3 na, nb; bool ok = true; };
		std::unordered_map<uint64_t, EdgeN> en;
		for (const EditTri& t : m.tris)
			for (int e = 0; e < 3; ++e)
			{
				const int32_t a = t.v[e], b = t.v[(e + 1) % 3];
				const uint64_t key = EdgeKey(a, b);
				const V3& na = a < b ? t.n[e] : t.n[(e + 1) % 3];
				const V3& nb = a < b ? t.n[(e + 1) % 3] : t.n[e];
				auto it = sp.find(key);
				if (it != sp.end())
				{
					EdgeN& q = en[key];
					if (Len(q.na - na) > 1e-6 || Len(q.nb - nb) > 1e-6) q.ok = false;
					continue;
				}
				if (split(a, b)) { sp.emplace(key, -1); order.push_back(key); en[key] = { na, nb, Len(na) > 0.5 && Len(nb) > 0.5 }; }
			}
		if (sp.empty()) return;
		size_t count = 0;
		for (const EditTri& t : m.tris)
		{
			int k = 0;
			for (int e = 0; e < 3; ++e) k += sp.count(EdgeKey(t.v[e], t.v[(e + 1) % 3])) ? 1 : 0;
			count += size_t(k + 1);
		}
		if (count > maxTris) return;
		for (uint64_t key : order)
		{
			const int32_t a = int32_t(key >> 32), b = int32_t(key & 0xffffffffu);
			V3 q = (m.p[a] + m.p[b]) * 0.5;
			const EdgeN& e = en[key];
			if (e.ok)
			{
				// Phong tessellation (alpha 0.5: exact to second order on a circle)
				const V3 pa = q - e.na * Dot(q - m.p[a], e.na), pb = q - e.nb * Dot(q - m.p[b], e.nb);
				q = q * 0.5 + (pa + pb) * 0.25;
			}
			const int32_t id = m.AddVertex(q, a, b, 0.5);
			sp[key] = id;
			m.mids[key] = id;
		}
		std::vector<EditTri> nt;
		nt.reserve(count);
		auto mid = [&](int32_t a, int32_t b) { auto it = sp.find(EdgeKey(a, b)); return it == sp.end() ? -1 : it->second; };
		// corner normal of a vertex in the source triangle (a midpoint: the mean of its ends)
		auto nrmOf = [&](const EditTri& src, int32_t v) -> V3
		{
			for (int k = 0; k < 3; ++k) if (src.v[k] == v) return src.n[k];
			for (int k = 0; k < 3; ++k)
			{
				const int32_t a = src.v[k], b = src.v[(k + 1) % 3];
				auto it = sp.find(EdgeKey(a, b));
				if (it != sp.end() && it->second == v)
				{
					if (Len(src.n[k]) < 0.5 || Len(src.n[(k + 1) % 3]) < 0.5) return V3(0, 0, 0);
					return Norm(src.n[k] + src.n[(k + 1) % 3], V3(0, 0, 0));
				}
			}
			return V3(0, 0, 0);
		};
		auto put = [&](const EditTri& src, int32_t a, int32_t b, int32_t c, const UV2& ua, const UV2& ub, const UV2& uc)
		{
			EditTri e;
			e.v = { a, b, c };
			e.uv = { ua, ub, uc };
			e.n = { nrmOf(src, a), nrmOf(src, b), nrmOf(src, c) };
			e.mat = src.mat;
			e.tag = src.tag;
			nt.push_back(e);
		};
		for (const EditTri& t : m.tris)
		{
			int32_t mm[3];
			int k = 0;
			for (int e = 0; e < 3; ++e) { mm[e] = mid(t.v[e], t.v[(e + 1) % 3]); k += mm[e] >= 0 ? 1 : 0; }
			if (k == 0) { nt.push_back(t); continue; }
			const auto& v = t.v;
			const auto& u = t.uv;
			if (k == 3)
			{
				const UV2 u01 = LerpUv(u[0], u[1], 0.5), u12 = LerpUv(u[1], u[2], 0.5), u20 = LerpUv(u[2], u[0], 0.5);
				put(t, v[0], mm[0], mm[2], u[0], u01, u20);
				put(t, mm[0], v[1], mm[1], u01, u[1], u12);
				put(t, mm[2], mm[1], v[2], u20, u12, u[2]);
				put(t, mm[0], mm[1], mm[2], u01, u12, u20);
				continue;
			}
			if (k == 1)
			{
				int r = mm[0] >= 0 ? 0 : (mm[1] >= 0 ? 1 : 2);
				const int i0 = r, i1 = (r + 1) % 3, i2 = (r + 2) % 3;
				const UV2 um = LerpUv(u[i0], u[i1], 0.5);
				put(t, v[i0], mm[r], v[i2], u[i0], um, u[i2]);
				put(t, mm[r], v[i1], v[i2], um, u[i1], u[i2]);
				continue;
			}
			// two: the edge (i2, i0) is the one not split
			int r = mm[2] < 0 ? 0 : (mm[0] < 0 ? 1 : 2);
			const int i0 = r, i1 = (r + 1) % 3, i2 = (r + 2) % 3;
			const int32_t mab = mm[i0], mbc = mm[i1];
			const UV2 uab = LerpUv(u[i0], u[i1], 0.5), ubc = LerpUv(u[i1], u[i2], 0.5);
			put(t, mab, v[i1], mbc, uab, u[i1], ubc);
			if (Len(m.p[v[i0]] - m.p[mbc]) <= Len(m.p[mab] - m.p[v[i2]]))
			{
				put(t, v[i0], mab, mbc, u[i0], uab, ubc);
				put(t, v[i0], mbc, v[i2], u[i0], ubc, u[i2]);
			}
			else
			{
				put(t, v[i0], mab, v[i2], u[i0], uab, u[i2]);
				put(t, mab, mbc, v[i2], uab, ubc, u[i2]);
			}
		}
		m.tris.swap(nt);
	}
}

// A vertex closer than share x the edge length to a crossing of the field
// along one of its edges is moved onto that crossing (and gets f = 0), as the
// 2D mesher does: the cut then makes no slivers. crossing(a, b) gives t from
// a (f[a] and f[b] have strictly opposite signs). Returns the moves made
// (vertex, toward, t) so the caller can move its own per-vertex data.
struct VertexMove { int32_t v = -1, toward = -1; double t = 0.0; };
inline std::vector<VertexMove> SnapToContour(EditMesh& m, std::vector<double>& f, const std::function<double(int32_t, int32_t)>& crossing,
	double share = 0.2)
{
	const size_t n = m.p.size();
	std::vector<double> best(n, share);
	std::vector<VertexMove> mv(n);
	std::unordered_map<uint64_t, char> seen;
	for (const EditTri& t : m.tris)
		for (int e = 0; e < 3; ++e)
		{
			const int32_t a = t.v[e], b = t.v[(e + 1) % 3];
			if (!((f[a] < 0 && f[b] > 0) || (f[a] > 0 && f[b] < 0))) continue;
			if (!seen.emplace(EdgeKey(a, b), 1).second) continue;
			const int32_t lo = std::min(a, b), hi = std::max(a, b);
			const double tt = crossing(lo, hi);
			if (tt < best[lo]) { best[lo] = tt; mv[lo] = { lo, hi, tt }; }
			if (1.0 - tt < best[hi]) { best[hi] = 1.0 - tt; mv[hi] = { hi, lo, 1.0 - tt }; }
		}
	std::vector<VertexMove> moves;
	for (size_t i = 0; i < n; ++i)
		if (mv[i].v >= 0) moves.push_back(mv[i]);
	if (moves.empty()) return moves;
	std::vector<V3> np(moves.size());
	for (size_t k = 0; k < moves.size(); ++k) np[k] = m.p[moves[k].v] + (m.p[moves[k].toward] - m.p[moves[k].v]) * moves[k].t;
	// corner UVs: the moved point in each triangle's own (old) affine frame
	std::unordered_map<int32_t, size_t> moveOf;
	for (size_t k = 0; k < moves.size(); ++k) moveOf.emplace(moves[k].v, k);
	for (EditTri& t : m.tris)
		for (int c = 0; c < 3; ++c)
		{
			auto it = moveOf.find(t.v[c]);
			if (it == moveOf.end()) continue;
			const V3 a = m.p[t.v[0]], b = m.p[t.v[1]], d = m.p[t.v[2]];
			const V3 e1 = b - a, e2 = d - a, q = np[it->second] - a;
			const double d11 = Dot(e1, e1), d12 = Dot(e1, e2), d22 = Dot(e2, e2), q1 = Dot(q, e1), q2 = Dot(q, e2);
			const double den = d11 * d22 - d12 * d12;
			if (std::abs(den) < 1e-30) continue;
			const double s = (d22 * q1 - d12 * q2) / den, w = (d11 * q2 - d12 * q1) / den;
			t.uv[c] = { t.uv[0][0] + (t.uv[1][0] - t.uv[0][0]) * s + (t.uv[2][0] - t.uv[0][0]) * w,
				t.uv[0][1] + (t.uv[1][1] - t.uv[0][1]) * s + (t.uv[2][1] - t.uv[0][1]) * w };
		}
	for (size_t k = 0; k < moves.size(); ++k)
	{
		m.p[moves[k].v] = np[k];
		f[moves[k].v] = 0.0;
		m.cut[moves[k].v] = 1;
	}
	return moves;
}

// One side of the field: side +1 keeps f >= 0, -1 keeps f <= 0. New vertices
// (the crossings) are appended with their origin; crossing(a, b), a < b,
// gives t from a. A crossing is made in the same place for both sides.
inline EditMesh ClipSide(const EditMesh& m, const std::vector<double>& f, int side, const std::function<double(int32_t, int32_t)>& crossing)
{
	EditMesh o;
	o.p = m.p;
	o.origin = m.origin;
	o.cut = m.cut;
	o.mids = m.mids;
	auto g = [&](int32_t v) { return side > 0 ? f[v] : -f[v]; };
	for (const EditPoly& q : m.polys)
	{
		int in = 0, out = 0;
		for (int32_t v : q.v) { if (g(v) > 0) ++in; else if (g(v) < 0) ++out; }
		if (in >= out && (in > 0 || side > 0)) o.polys.push_back(q);
	}
	std::unordered_map<uint64_t, int32_t> cross;
	auto cid = [&](int32_t a, int32_t b) -> int32_t
	{
		const uint64_t key = EdgeKey(a, b);
		auto it = cross.find(key);
		if (it != cross.end()) return it->second;
		const int32_t lo = std::min(a, b), hi = std::max(a, b);
		const double t = crossing(lo, hi);
		const int32_t id = o.AddVertex(m.p[lo] + (m.p[hi] - m.p[lo]) * t, lo, hi, t, 1);
		cross.emplace(key, id);
		return id;
	};
	for (const EditTri& t : m.tris)
	{
		const double gv[3] = { g(t.v[0]), g(t.v[1]), g(t.v[2]) };
		if (gv[0] >= 0 && gv[1] >= 0 && gv[2] >= 0)
		{
			if (gv[0] > 0 || gv[1] > 0 || gv[2] > 0 || side > 0) o.tris.push_back(t);
			continue;
		}
		if (gv[0] <= 0 && gv[1] <= 0 && gv[2] <= 0) continue;
		int32_t pv[4];
		UV2 pu[4];
		V3 pn[4];
		int k = 0;
		for (int e = 0; e < 3; ++e)
		{
			const int e1 = (e + 1) % 3;
			if (gv[e] >= 0) { pv[k] = t.v[e]; pu[k] = t.uv[e]; pn[k] = t.n[e]; ++k; }
			if ((gv[e] > 0 && gv[e1] < 0) || (gv[e] < 0 && gv[e1] > 0))
			{
				const int32_t c = cid(t.v[e], t.v[e1]);
				const auto& org = o.origin[c];
				const double tt = org.a == t.v[e] ? org.t : 1.0 - org.t;
				pv[k] = c;
				pu[k] = LerpUv(t.uv[e], t.uv[e1], tt);
				pn[k] = Norm(t.n[e] * (1 - tt) + t.n[e1] * tt, V3(0, 0, 0));
				++k;
			}
		}
		for (int j = 1; j + 1 < k; ++j)
		{
			EditTri e;
			e.v = { pv[0], pv[j], pv[j + 1] };
			e.uv = { pu[0], pu[j], pu[j + 1] };
			e.n = { pn[0], pn[j], pn[j + 1] };
			e.mat = t.mat;
			e.tag = t.tag;
			if (e.v[0] != e.v[1] && e.v[1] != e.v[2] && e.v[0] != e.v[2]) o.tris.push_back(e);
		}
	}
	return o;
}

// Bisection (Illinois) of a scalar function on [0, 1] with fa, fb of opposite signs.
inline double RootOnEdge(const std::function<double(double)>& fn, double fa, double fb, int iters = 14)
{
	double a = 0.0, b = 1.0;
	int lastSide = 0;
	double t = fa / (fa - fb);
	for (int i = 0; i < iters; ++i)
	{
		t = Clamp(a - fa * (b - a) / (fb - fa), a + 1e-6 * (b - a), b - 1e-6 * (b - a));
		const double ft = fn(t);
		if (ft == 0.0) return t;
		if ((ft < 0) == (fa < 0))
		{
			a = t; fa = ft;
			if (lastSide == -1) fb *= 0.5;
			lastSide = -1;
		}
		else
		{
			b = t; fb = ft;
			if (lastSide == 1) fa *= 0.5;
			lastSide = 1;
		}
		if (b - a < 1e-9) break;
	}
	return Clamp(t, 0.0, 1.0);
}

// The vertices of a kept polygon with the vertices Refine put on its edges.
inline void ExpandPoly(const EditMesh& m, const EditPoly& q, std::vector<int32_t>& v, std::vector<UV2>& uv)
{
	v.clear();
	uv.clear();
	std::function<void(int32_t, int32_t, const UV2&, const UV2&)> rec = [&](int32_t a, int32_t b, const UV2& ua, const UV2& ub)
	{
		auto it = m.mids.find(EdgeKey(a, b));
		if (it == m.mids.end()) return;
		const int32_t c = it->second;
		const UV2 uc = LerpUv(ua, ub, 0.5);
		rec(a, c, ua, uc);
		v.push_back(c);
		uv.push_back(uc);
		rec(c, b, uc, ub);
	};
	const size_t k = q.v.size();
	for (size_t i = 0; i < k; ++i)
	{
		v.push_back(q.v[i]);
		uv.push_back(q.uv[i]);
		rec(q.v[i], q.v[(i + 1) % k], q.uv[i], q.uv[(i + 1) % k]);
	}
}

// Every face of the edit mesh as (vertices, uvs, mat, tag); polygons expanded.
template <class F>
inline void ForEachFace(const EditMesh& m, F&& fn)
{
	std::vector<int32_t> v;
	std::vector<UV2> uv;
	for (const EditPoly& q : m.polys)
	{
		ExpandPoly(m, q, v, uv);
		fn(v, uv, q.mat, -1);
	}
	for (const EditTri& t : m.tris)
	{
		v.assign(t.v.begin(), t.v.end());
		uv.assign(t.uv.begin(), t.uv.end());
		fn(v, uv, t.mat, t.tag);
	}
}

// Connected pieces (through shared vertices) below minArea are removed when
// they touch a cut (onlyCut) or always. Returns the number removed.
inline int DropSmallPieces(EditMesh& m, double minArea, bool onlyCut = true)
{
	if (minArea <= 0.0) return 0;
	const size_t n = m.p.size();
	std::vector<int32_t> parent(n);
	for (size_t i = 0; i < n; ++i) parent[i] = int32_t(i);
	auto find = [&](int32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
	auto unite = [&](int32_t a, int32_t b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
	std::vector<double> area(n, 0.0);
	std::vector<uint8_t> hasCut(n, 0);
	std::vector<std::vector<int32_t>> polyV(m.polys.size());
	{
		std::vector<UV2> uv;
		for (size_t i = 0; i < m.polys.size(); ++i)
		{
			ExpandPoly(m, m.polys[i], polyV[i], uv);
			for (size_t k = 1; k < polyV[i].size(); ++k) unite(polyV[i][0], polyV[i][k]);
		}
	}
	for (const EditTri& t : m.tris) { unite(t.v[0], t.v[1]); unite(t.v[0], t.v[2]); }
	for (size_t i = 0; i < m.polys.size(); ++i)
	{
		const int32_t r = find(polyV[i][0]);
		area[r] += 0.5 * Len(PolyNormal(m.p, polyV[i]));
		for (int32_t v : polyV[i]) if (m.cut[v]) hasCut[r] = 1;
	}
	for (const EditTri& t : m.tris)
	{
		const int32_t r = find(t.v[0]);
		area[r] += 0.5 * Len(Cross(m.p[t.v[1]] - m.p[t.v[0]], m.p[t.v[2]] - m.p[t.v[0]]));
		for (int32_t v : t.v) if (m.cut[v]) hasCut[r] = 1;
	}
	auto drop = [&](int32_t v) { const int32_t r = find(v); return area[r] < minArea && (!onlyCut || hasCut[r]); };
	int removed = 0;
	{
		std::vector<EditPoly> keep;
		for (size_t i = 0; i < m.polys.size(); ++i)
			if (!drop(polyV[i][0])) keep.push_back(m.polys[i]);
			else ++removed;
		m.polys.swap(keep);
	}
	{
		std::vector<EditTri> keep;
		keep.reserve(m.tris.size());
		for (const EditTri& t : m.tris)
			if (!drop(t.v[0])) keep.push_back(t);
			else ++removed;
		m.tris.swap(keep);
	}
	return removed;
}

// Open borders as loops of vertices, each in the direction its faces run it.
// onlyCut: only loops whose vertices are all on a cut.
inline std::vector<std::vector<int32_t>> BoundaryLoops(const EditMesh& m, bool onlyCut)
{
	std::unordered_map<uint64_t, int> count;
	std::vector<std::pair<int32_t, int32_t>> dir;
	ForEachFace(m, [&](const std::vector<int32_t>& v, const std::vector<UV2>&, int32_t, int32_t)
	{
		for (size_t i = 0; i < v.size(); ++i)
		{
			const int32_t a = v[i], b = v[(i + 1) % v.size()];
			count[EdgeKey(a, b)]++;
			dir.push_back({ a, b });
		}
	});
	std::unordered_map<int32_t, std::vector<int32_t>> next;
	for (const auto& e : dir)
		if (count[EdgeKey(e.first, e.second)] == 1) next[e.first].push_back(e.second);
	std::vector<std::vector<int32_t>> loops;
	std::unordered_map<uint64_t, char> used;
	for (auto& kv : next)
		for (int32_t b0 : kv.second)
		{
			const int32_t a0 = kv.first;
			if (used.count((uint64_t(uint32_t(a0)) << 32) | uint32_t(b0))) continue;
			std::vector<int32_t> loop{ a0 };
			int32_t a = a0, b = b0;
			bool closed = false;
			for (size_t guard = 0; guard < dir.size() + 1; ++guard)
			{
				used[(uint64_t(uint32_t(a)) << 32) | uint32_t(b)] = 1;
				if (b == a0) { closed = true; break; }
				loop.push_back(b);
				auto it = next.find(b);
				if (it == next.end()) break;
				int32_t c = -1;
				for (int32_t x : it->second)
					if (!used.count((uint64_t(uint32_t(b)) << 32) | uint32_t(x))) { c = x; break; }
				if (c < 0) break;
				a = b;
				b = c;
			}
			if (!closed || loop.size() < 3) continue;
			if (onlyCut)
			{
				bool all = true;
				for (int32_t v : loop) if (!m.cut[v]) { all = false; break; }
				if (!all) continue;
			}
			loops.push_back(std::move(loop));
		}
	return loops;
}

// Area-weighted vertex normals over every face (polygons expanded).
inline std::vector<V3> EditNormals(const EditMesh& m)
{
	std::vector<V3> n(m.p.size());
	ForEachFace(m, [&](const std::vector<int32_t>& v, const std::vector<UV2>&, int32_t, int32_t)
	{
		const V3 c = PolyNormal(m.p, v);
		for (int32_t i : v) n[i] += c;
	});
	for (auto& q : n) q = Norm(q, V3(0, 0, 0));
	return n;
}

// Edit mesh -> GenMesh, only the used vertices. matOf(mat, tag) picks the
// output material. Returns old vertex -> new index (-1 unused).
inline std::vector<int32_t> EmitEditMesh(const EditMesh& m, GenMesh& out, const std::function<int32_t(int32_t, int32_t)>& matOf = nullptr,
	int32_t part = -1)
{
	std::vector<int32_t> map(m.p.size(), -1);
	std::vector<int32_t> fv;
	ForEachFace(m, [&](const std::vector<int32_t>& v, const std::vector<UV2>& uv, int32_t mat, int32_t tag)
	{
		fv.resize(v.size());
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (map[v[i]] < 0) map[v[i]] = out.AddPoint(m.p[v[i]]);
			fv[i] = map[v[i]];
		}
		out.AddFaceV(fv, matOf ? matOf(mat, tag) : mat, uv, part);
	});
	return map;
}

// Geodesic distance over the triangles from a point on the surface (seeded at
// the corners of the nearest triangle), up to limit; planar unfolding of each
// triangle (exact on flat parts), Dijkstra order. Unreached: 1e30.
inline std::vector<double> SurfaceDistance(const EditMesh& m, const std::vector<std::vector<int32_t>>& vertTris, const V3& from, double limit)
{
	const size_t n = m.p.size();
	std::vector<double> d(n, 1e30);
	std::vector<uint8_t> done(n, 0);
	// nearest triangle (by distance to its corners and centre: fine for a seed)
	int best = -1;
	double bd = 1e300;
	for (size_t i = 0; i < m.tris.size(); ++i)
	{
		const auto& t = m.tris[i];
		const V3 c = (m.p[t.v[0]] + m.p[t.v[1]] + m.p[t.v[2]]) * (1.0 / 3.0);
		const double q = std::min({ Len(c - from), Len(m.p[t.v[0]] - from), Len(m.p[t.v[1]] - from), Len(m.p[t.v[2]] - from) });
		if (q < bd) { bd = q; best = int(i); }
	}
	if (best < 0) return d;
	using Item = std::pair<double, int32_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
	for (int k = 0; k < 3; ++k)
	{
		const int32_t v = m.tris[best].v[k];
		d[v] = Len(m.p[v] - from);
		pq.push({ d[v], v });
	}
	auto unfold = [&](int32_t a, int32_t b, int32_t c) -> double
	{
		// a, b known; distance to c through the triangle
		const V3 A = m.p[a], B = m.p[b], C = m.p[c];
		const double ab = Len(B - A);
		if (ab < 1e-15) return 1e30;
		const V3 ex = (B - A) * (1.0 / ab);
		const double cx = Dot(C - A, ex);
		const double cy = Len((C - A) - ex * cx);
		const double da = d[a], db = d[b];
		const double sx = (da * da - db * db + ab * ab) / (2 * ab);
		const double s2 = da * da - sx * sx;
		if (s2 < 0) return 1e30;
		const double sy = -std::sqrt(s2);
		// the ray S -> C must cross AB
		const double t = cy - sy > 1e-15 ? -sy / (cy - sy) : -1.0;
		const double xcross = sx + (cx - sx) * t;
		if (t < 0 || xcross < -1e-12 * ab || xcross > ab * (1 + 1e-12)) return 1e30;
		return std::hypot(cx - sx, cy - sy);
	};
	while (!pq.empty())
	{
		const auto [dv, v] = pq.top();
		pq.pop();
		if (done[v] || dv > d[v]) continue;
		done[v] = 1;
		if (dv > limit) break;
		for (int32_t ti : vertTris[v])
		{
			const auto& t = m.tris[ti];
			for (int k = 0; k < 3; ++k)
			{
				const int32_t w = t.v[k];
				if (w == v || done[w]) continue;
				int32_t u = -1;
				for (int j = 0; j < 3; ++j) if (t.v[j] != v && t.v[j] != w) u = t.v[j];
				double cand = d[v] + Len(m.p[w] - m.p[v]);
				if (u >= 0 && done[u]) cand = std::min(cand, unfold(v, u, w));
				if (cand < d[w]) { d[w] = cand; pq.push({ cand, w }); }
			}
		}
	}
	return d;
}

inline std::vector<std::vector<int32_t>> VertexTriangles(const EditMesh& m)
{
	std::vector<std::vector<int32_t>> vt(m.p.size());
	for (size_t i = 0; i < m.tris.size(); ++i)
		for (int32_t v : m.tris[i].v) vt[v].push_back(int32_t(i));
	return vt;
}

// Closest point on a triangle (Ericson).
inline V3 ClosestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c)
{
	const V3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const V3 bp = p - b;
	const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	const V3 cp = p - c;
	const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

// The input mesh as plain triangles (for sampling and closest points).
struct SurfaceTris
{
	std::vector<std::array<int32_t, 3>> t;
	std::vector<double> area;     // per triangle
	std::vector<V3> normal;       // unit, per triangle
	double total = 0.0;
};
inline SurfaceTris MakeSurfaceTris(const MeshInput& in)
{
	SurfaceTris s;
	size_t L = 0;
	std::vector<int32_t> idx;
	std::vector<std::array<int32_t, 3>> tri;
	for (size_t f = 0; f < in.faceSizes.size(); ++f)
	{
		const int sz = in.faceSizes[f];
		idx.assign(in.faceVerts.begin() + L, in.faceVerts.begin() + L + sz);
		L += size_t(sz);
		if (sz < 3) continue;
		tri.clear();
		TriangulatePolygon(in.points, idx, tri);
		for (const auto& t : tri)
		{
			const std::array<int32_t, 3> v{ idx[t[0]], idx[t[1]], idx[t[2]] };
			const V3 c = Cross(in.points[v[1]] - in.points[v[0]], in.points[v[2]] - in.points[v[0]]);
			s.t.push_back(v);
			s.area.push_back(0.5 * Len(c));
			s.normal.push_back(Norm(c, V3(0, 0, 0)));
			s.total += 0.5 * Len(c);
		}
	}
	return s;
}

// A vertex where separate fans of faces meet (two holes touching in a point)
// is duplicated, one copy per fan. Returns, per new vertex, the vertex it
// copies (so the caller can copy per-vertex data).
inline std::vector<int32_t> SplitPinchVertices(GenMesh& g)
{
	const size_t nv = g.points.size(), nf = g.faceSizes.size();
	std::vector<size_t> start(nf);
	{
		size_t L = 0;
		for (size_t f = 0; f < nf; ++f) { start[f] = L; L += size_t(g.faceSizes[f]); }
	}
	// incident corners per vertex
	std::vector<std::vector<std::pair<int32_t, int32_t>>> inc(nv);   // (face, corner)
	for (size_t f = 0; f < nf; ++f)
		for (int k = 0; k < g.faceSizes[f]; ++k) inc[g.faceVerts[start[f] + k]].push_back({ int32_t(f), k });
	std::vector<int32_t> copies;
	for (size_t v = 0; v < nv; ++v)
	{
		auto& cs = inc[v];
		if (cs.size() < 2) continue;
		std::vector<int32_t> grp(cs.size());
		for (size_t i = 0; i < cs.size(); ++i) grp[i] = int32_t(i);
		auto find = [&](int32_t x) { while (grp[x] != x) x = grp[x] = grp[grp[x]]; return x; };
		// neighbours of v in a face: the previous and the next corner
		auto nb = [&](const std::pair<int32_t, int32_t>& c, int32_t o[2])
		{
			const int sz = g.faceSizes[c.first];
			o[0] = g.faceVerts[start[c.first] + (c.second + sz - 1) % sz];
			o[1] = g.faceVerts[start[c.first] + (c.second + 1) % sz];
		};
		std::unordered_map<int32_t, int32_t> firstWith;
		for (size_t i = 0; i < cs.size(); ++i)
		{
			int32_t o[2];
			nb(cs[i], o);
			for (int32_t w : o)
			{
				auto it = firstWith.find(w);
				if (it == firstWith.end()) firstWith.emplace(w, int32_t(i));
				else grp[find(int32_t(i))] = find(it->second);
			}
		}
		const int32_t first = find(0);
		std::unordered_map<int32_t, int32_t> copyOf;
		for (size_t i = 0; i < cs.size(); ++i)
		{
			const int32_t gi = find(int32_t(i));
			if (gi == first) continue;
			auto it = copyOf.find(gi);
			int32_t id;
			if (it == copyOf.end())
			{
				id = g.AddPoint(g.points[v]);
				copies.push_back(int32_t(v));
				copyOf.emplace(gi, id);
			}
			else id = it->second;
			g.faceVerts[start[cs[i].first] + cs[i].second] = id;
		}
	}
	return copies;
}

// Keeps the pieces that hold an anchor vertex, plus the pieces that touch no
// cut (the object's other parts); the rest (bits a cut severed) is removed.
// When no piece has an anchor, the largest piece by vertex count is kept.
inline int KeepAnchoredPieces(EditMesh& m, const std::function<bool(int32_t)>& anchor)
{
	const size_t n = m.p.size();
	std::vector<int32_t> parent(n);
	for (size_t i = 0; i < n; ++i) parent[i] = int32_t(i);
	auto find = [&](int32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
	auto unite = [&](int32_t a, int32_t b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
	std::vector<std::vector<int32_t>> polyV(m.polys.size());
	std::vector<UV2> uv;
	for (size_t i = 0; i < m.polys.size(); ++i)
	{
		ExpandPoly(m, m.polys[i], polyV[i], uv);
		for (size_t k = 1; k < polyV[i].size(); ++k) unite(polyV[i][0], polyV[i][k]);
	}
	for (const EditTri& t : m.tris) { unite(t.v[0], t.v[1]); unite(t.v[0], t.v[2]); }
	std::vector<uint8_t> used(n, 0), hasCut(n, 0), hasAnchor(n, 0);
	std::vector<int32_t> count(n, 0);
	auto mark = [&](int32_t v) { used[v] = 1; };
	for (auto& pv : polyV) for (int32_t v : pv) mark(v);
	for (const EditTri& t : m.tris) for (int32_t v : t.v) mark(v);
	bool any = false;
	for (size_t v = 0; v < n; ++v)
	{
		if (!used[v]) continue;
		const int32_t r = find(int32_t(v));
		count[r]++;
		if (m.cut[v]) hasCut[r] = 1;
		if (anchor(int32_t(v))) { hasAnchor[r] = 1; any = true; }
	}
	int32_t biggest = -1;
	if (!any)
		for (size_t v = 0; v < n; ++v)
			if (used[v] && find(int32_t(v)) == int32_t(v) && hasCut[v] && (biggest < 0 || count[v] > count[biggest])) biggest = int32_t(v);
	auto keep = [&](int32_t v) { const int32_t r = find(v); return !hasCut[r] || hasAnchor[r] || r == biggest; };
	int removed = 0;
	std::vector<EditPoly> kp;
	for (size_t i = 0; i < m.polys.size(); ++i)
		if (keep(polyV[i][0])) kp.push_back(m.polys[i]); else ++removed;
	m.polys.swap(kp);
	std::vector<EditTri> kt;
	kt.reserve(m.tris.size());
	for (const EditTri& t : m.tris)
		if (keep(t.v[0])) kt.push_back(t); else ++removed;
	m.tris.swap(kt);
	return removed;
}

} // namespace pdgen
