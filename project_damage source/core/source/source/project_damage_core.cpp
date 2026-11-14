// собираем повреждения и возвращаем итоговую геометрию
// ProjectDamage - worn edge damage cut into mesh objects.
// Copyright (C) 2026 Cornelius Dammrich
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// This file is also conveyed by its author, the sole copyright holder,
// under a separate proprietary licence as part of the ProjectDamage plugin for
// Cinema 4D. That does not affect your rights under the GPL above.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#include "manifold/manifold.h"
#include "manifold/cross_section.h"

#include "project_damage_core.h"
#include "py_random.h"
#include "project_damage_internal.h"
#include "project_damage_bvh.h"
#include "surface_guidance.h"
#include "project_damage_deposits.h"
#include "coating_peeling.h"
#include "one_mesh.h"
#include "heat_damage.h"
#include "rust.h"
#include "damage_debris.h"
#include "damage_shading.h"
#include "break_texture.h"
#include "fabric_flat.h"
#include "fabric_solid.h"
#include "hang_hole.h"
#include "wall_hole.h"
#include "rb_fracture.h"
#include "cell_fracture.h"
#include "fabric_tears.h"
#include "concrete_damage.h"
#include "concrete_hit.h"
#include "plaster_displace.h"
#include "concrete_pits.h"
#include "metal_tear.h"
#include "clay.h"
#include "edge_bevel.h"
#include "damaged_bevel.h"
#include "scratches_draw.h"
#include "metal_deform.h"
#include "wood_damage.h"
#include "lath_damage.h"
#include "wallpaper.h"
#include "bullet_impacts.h"
#include "building_destruction.h"
#include "ground_cracks.h"
#include "brick_damage.h"
#include "broken_glass.h"
#include "mesh_edit.h"
#include "ground_fissure.h"
#include "rusty_holes.h"
#include "voxel_erosion.h"
#include "project_damage_adjacency.h"
#include "project_damage_noise.h"
#include "obreaker.h"
#include "break_rebar.h"
#include "break_chips.h"
#include "break_framing.h"
#include "oproject_damage.h"

#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

using namespace project_damage;

namespace
{
EVec PolyNormal(const EVec* pts, const ProjectDamagePoly& cp)
{
	Int32 idx[4];
	int k;
	if (cp.c == cp.d) { idx[0] = cp.a; idx[1] = cp.b; idx[2] = cp.c; k = 3; }
	else              { idx[0] = cp.a; idx[1] = cp.b; idx[2] = cp.c; idx[3] = cp.d; k = 4; }
	EVec n(0.0);
	for (int i = 0; i < k; i++)
	{
		const EVec& cur = pts[idx[i]];
		const EVec& nxt = pts[idx[(i + 1) % k]];
		n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
		n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
		n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
	}
	const double L = m::la::length(n);
	if (L > 0)
		n = n * (1.0 / L);
	return n;
}

std::vector<EVec> PolyNormals(const EVec* pts, const ProjectDamagePoly* polys, Int32 vcnt)
{
	std::vector<EVec> normals((size_t)vcnt);
	ProjectDamageParallelFor(0, (int64_t)vcnt, [&](int64_t i)
	{
		normals[(size_t)i] = PolyNormal(pts, polys[i]);
	});
	return normals;
}

double TurnAngle(const EVec* pts, Int32 a, Int32 b, Int32 c)
{
	EVec u = pts[b] - pts[a];
	EVec v = pts[c] - pts[b];
	double lu = m::la::length(u), lv = m::la::length(v);
	if (lu == 0.0 || lv == 0.0)
		return m::kPi;
	double d = m::la::dot(u, v) / (lu * lv);
	if (d > 1.0) d = 1.0;
	if (d < -1.0) d = -1.0;
	return std::acos(d);
}

void DetectExposed(MeshView in, const std::vector<EVec>& normals,
	Float thresholdDeg, std::vector<ExposedEdge>& exposed,
	[[maybe_unused]] const ProjectDamageHost& host, const std::vector<Bool>* skipPoly = nullptr)
{
	const Int32 vcnt = in.npolys;
	const EVec* pts = in.pts;
	const ProjectDamagePoly* polys = in.polys;

	ProjectDamageAdjacency nb;
	if (!nb.Build(in))
		return;

	const double thr = thresholdDeg * m::kPi / 180.0;

	for (Int32 pi = 0; pi < vcnt; pi++)
	{
		const ProjectDamageAdjInfo* info = nb.GetInfo(pi);
		const ProjectDamagePoly& cp = polys[pi];
		const Int32 verts[4] = { cp.a, cp.b, cp.c, cp.d };
		for (int side = 0; side < 4; side++)
		{
			if (info->mark[side])
				continue;
			const Int32 pa = verts[side], pb = verts[(side + 1) & 3];
			if (pa == pb)
				continue;

			const Int32 f0 = pi, f1 = info->face[side];
			if (f1 < 0)
				continue;
			if (skipPoly != nullptr && (*skipPoly)[(size_t)f0] && (*skipPoly)[(size_t)f1])
				continue;

			const EVec& n0 = normals[f0];
			const EVec& n1 = normals[f1];
			double dot = m::la::dot(n0, n1);
			if (dot > 1.0) dot = 1.0;
			if (dot < -1.0) dot = -1.0;
			const double angle = std::acos(dot);
			if (angle <= FLAT_EPS)
				continue;

			const ProjectDamagePoly& cp1 = polys[f1];
			const Int32 q4[4] = { cp1.a, cp1.b, cp1.c, cp1.d };
			Int32 far = pa;
			for (int qi = 0; qi < 4; qi++)
			{
				const Int32 q = q4[qi];
				if (q != pa && q != pb) { far = q; break; }
			}
			const Bool convex = m::la::dot(n0, pts[far] - pts[pa]) < 0.0;

			if (angle > thr)
			{
				ExposedEdge e;
				e.p0 = pa;
				e.p1 = pb;
				e.convex = convex;
				e.reprs.push_back({ pi, side });
				const Int32 q4b[4] = { cp1.a, cp1.b, cp1.c, cp1.d };
				for (int s2 = 0; s2 < 4; s2++)
				{
					const Int32 qa = q4b[s2], qb = q4b[(s2 + 1) & 3];
					if (qa == qb)
						continue;
					if ((qa == pa && qb == pb) || (qa == pb && qb == pa))
					{
						e.reprs.push_back({ f1, s2 });
						break;
					}
				}
				exposed.push_back(std::move(e));
			}
		}
	}
}

void DetectBreakRim(const ProjectDamagePoly* tris, Int32 nt, Int32 nv,
	const std::vector<Bool>& breakFace,
	const EVec* pts, const std::vector<EVec>& normals,
	Float thresholdDeg, std::vector<ExposedEdge>& exposed)
{
	if (nt <= 0 || nv <= 0)
		return;

	std::vector<Bool> touched((size_t)nv, false);
	for (Int32 t = 0; t < nt; t++)
	{
		if (!breakFace[(size_t)t])
			continue;
		touched[(size_t)tris[t].a] = true;
		touched[(size_t)tris[t].b] = true;
		touched[(size_t)tris[t].c] = true;
	}

	std::unordered_map<UInt64, std::pair<Int32, Int32>> cand;
	const double thr = thresholdDeg * m::kPi / 180.0;
	static const int kSide[3] = { 0, 1, 3 };
	auto key = [](Int32 a, Int32 b) {
		return a < b ? ((UInt64)(UInt32)a << 32) | (UInt32)b
					 : ((UInt64)(UInt32)b << 32) | (UInt32)a;
	};

	for (int pass = 0; pass < 2; pass++)
	{
		for (Int32 t = 0; t < nt; t++)
		{
			const Int32 v[3] = { tris[t].a, tris[t].b, tris[t].c };
			if (!touched[(size_t)v[0]] && !touched[(size_t)v[1]] && !touched[(size_t)v[2]])
				continue;
			for (int k = 0; k < 3; k++)
			{
				const Int32 pa = v[k], pb = v[(k + 1) % 3];
				if (pa == pb || !touched[(size_t)pa] || !touched[(size_t)pb])
					continue;
				const Int32 e0 = 4 * t + kSide[k];
				const UInt64 kk = key(pa, pb);
				if (pass == 0)
				{
					auto ins = cand.emplace(kk, std::make_pair(e0, NOTOK));
					if (!ins.second && ins.first->second.second == NOTOK)
						ins.first->second.second = e0;
					continue;
				}
				auto it = cand.find(kk);
				if (it == cand.end() || it->second.first != e0)
					continue;
				if (it->second.second == NOTOK)
					continue;
				const Int32 f1 = it->second.second / 4, s1 = it->second.second % 4;

				const Bool b0 = breakFace[(size_t)t], b1 = breakFace[(size_t)f1];
				if (b0 == b1)
					continue;

				const EVec& n0 = normals[(size_t)t];
				const EVec& n1 = normals[(size_t)f1];
				double dot = m::la::dot(n0, n1);
				if (dot > 1.0) dot = 1.0;
				if (dot < -1.0) dot = -1.0;
				const double angle = std::acos(dot);
				if (angle <= FLAT_EPS || angle <= thr)
					continue;

				Int32 far = pa;
				const Int32 fv[3] = { tris[f1].a, tris[f1].b, tris[f1].c };
				for (int qi = 0; qi < 3; qi++)
				{
					const Int32 q = fv[qi];
					if (q != pa && q != pb) { far = q; break; }
				}
				ExposedEdge e;
				e.p0 = pa;
				e.p1 = pb;
				e.convex = m::la::dot(n0, pts[far] - pts[pa]) < 0.0;
				e.reprs.push_back({ t, kSide[k] });
				e.reprs.push_back({ f1, s1 });
				exposed.push_back(std::move(e));
			}
		}
	}
}

void GroupObjectEdges(std::vector<ExposedEdge>& exposed, const EVec* pts,
	Float thresholdDeg, std::vector<Chain>& chains)
{
	const double turnCeiling = thresholdDeg * m::kPi / 180.0 + FLAT_EPS;

	std::unordered_map<Int32, std::vector<ExposedEdge*>> inc;
	for (auto& e : exposed)
	{
		inc[e.p0].push_back(&e);
		inc[e.p1].push_back(&e);
	}
	auto other = [](ExposedEdge* e, Int32 p) { return e->p0 == p ? e->p1 : e->p0; };

	std::unordered_set<ExposedEdge*> assigned;

	auto extend = [&](std::vector<Int32>& chainPts, std::vector<ExposedEdge*>& chainEdges,
		Int32 fromPt, Int32 curPt, bool append)
	{
		while (true)
		{
			ExposedEdge* best = nullptr;
			double bestTurn = turnCeiling;
			Int32 bestNext = -1;
			auto itInc = inc.find(curPt);
			if (itInc != inc.end())
			{
				for (ExposedEdge* e : itInc->second)
				{
					if (assigned.count(e))
						continue;
					const Int32 nxt = other(e, curPt);
					if (nxt == fromPt)
						continue;
					const double t = TurnAngle(pts, fromPt, curPt, nxt);
					if (t < bestTurn) { best = e; bestTurn = t; bestNext = nxt; }
				}
			}
			if (best == nullptr)
				return;
			assigned.insert(best);
			if (append)
			{
				chainEdges.push_back(best);
				chainPts.push_back(bestNext);
			}
			else
			{
				chainEdges.insert(chainEdges.begin(), best);
				chainPts.insert(chainPts.begin(), bestNext);
			}
			fromPt = curPt;
			curPt = bestNext;
		}
	};

	for (auto& seed : exposed)
	{
		if (assigned.count(&seed))
			continue;
		assigned.insert(&seed);
		const Int32 pa = seed.p0, pb = seed.p1;
		Chain ch;
		ch.points = { pa, pb };
		ch.edges = { &seed };
		extend(ch.points, ch.edges, pa, pb, true);
		extend(ch.points, ch.edges, pb, pa, false);
		double length = 0.0;
		for (ExposedEdge* e : ch.edges)
			length += m::la::length(pts[e->p1] - pts[e->p0]);
		ch.length = length;
		chains.push_back(std::move(ch));
	}

	std::unordered_map<Int32, int> concaveInc;
	for (const ExposedEdge& e : exposed)
	{
		if (!e.convex)
		{
			concaveInc[e.p0]++;
			concaveInc[e.p1]++;
		}
	}
	if (!concaveInc.empty())
	{
		auto blocked = [&](Int32 v, const ExposedEdge* own)
		{
			auto it = concaveInc.find(v);
			if (it == concaveInc.end())
				return false;
			return it->second - (own->convex ? 0 : 1) > 0;
		};
		for (Chain& ch : chains)
		{
			if (ch.points.front() == ch.points.back())
				continue;
			ch.blockedStart = blocked(ch.points.front(), ch.edges.front());
			ch.blockedEnd   = blocked(ch.points.back(),  ch.edges.back());
		}
	}
}

m::vec3 PerpOnto(const m::vec3& v, const m::vec3& axis)
{
	const m::vec3 r = v - axis * m::la::dot(v, axis);
	const double l = m::la::length(r);
	return l > 1e-12 ? r / l : v;
}

m::vec3 PerpFrame(const m::vec3& axis)
{
	const m::vec3 up = std::fabs(axis.y) < 0.9 ? m::vec3(0.0, 1.0, 0.0) : m::vec3(1.0, 0.0, 0.0);
	m::vec3 t = m::la::cross(up, axis);
	const double tl = m::la::length(t);
	return t / (tl != 0.0 ? tl : 1.0);
}
}

namespace project_damage {
m::SimplePolygon MakeSpallOutline(double su, double sv, PyRandom& rng, int n)
{
	const double a2 = rng.Uniform(0.04, 0.12), p2 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double a3 = rng.Uniform(0.03, 0.10), p3 = rng.Uniform(0.0, 2.0 * m::kPi);
	const int lobes = 3 + (int)(rng.Random() * 4.0);
	const double lobePh = rng.Uniform(0.0, m::kPi);
	const double scallop = rng.Uniform(0.05, 0.16);
	const double asym = rng.Uniform(-0.2, 0.2);
	const uint64_t grain = (uint64_t)(rng.Random() * 4294967296.0);
	const double norm = 1.0 / (1.0 - 0.4 * scallop);

	m::SimplePolygon pts;
	pts.reserve(n);
	for (int i = 0; i < n; i++)
	{
		const double a = 2.0 * m::kPi * i / n;
		double r = 1.0 + a2 * std::cos(2.0 * a + p2) + a3 * std::cos(3.0 * a + p3);
		const double lobe = std::pow(std::fabs(std::sin(0.5 * lobes * a + lobePh)), 0.55);
		r *= (1.0 - scallop + scallop * lobe) * norm;
		r *= 1.0 + 0.09 * (Grain01(grain, (uint64_t)i) - 0.5);
		if (r < 0.55) r = 0.55;
		const double u = std::cos(a) * su * r;
		const double v = std::sin(a) * sv * r + asym * sv * std::fabs(std::cos(a));
		pts.push_back(m::vec2(u, v));
	}
	double area = 0.0;
	for (int i = 0; i < n; i++)
	{
		const m::vec2& p = pts[i];
		const m::vec2& q = pts[(i + 1) % n];
		area += p.x * q.y - q.x * p.y;
	}
	if (area < 0.0)
		std::reverse(pts.begin(), pts.end());
	return pts;
}

static m::Manifold BowlCutter(const SpallSpec& s)
{
	const m::SimplePolygon& outline = s.outline;
	const int n = (int)outline.size();
	if (n < 3)
		return m::Manifold();
	const int nr = MaxI(3, s.rings);
	const double depth = s.depth;
	const double RIM_SOFT_DROP = 0.5;
	const double RIM_SOFT_REACH = 0.5;
	const double Rz = s.rimSoft * RIM_SOFT_DROP * depth;
	const double f = s.rimSoft * RIM_SOFT_REACH;
	const int nf = (Rz > 1e-9 && f > 1e-9) ? MaxI(2, nr / 2) : 0;
	const double outer = 1.0 + f;

	const double offLen = std::hypot(s.offX / MaxD(s.su, 1e-12), s.offY / MaxD(s.su, 1e-12));
	const double odx = offLen > 1e-12 ? s.offX / std::hypot(s.offX, s.offY) : 0.0;
	const double ody = offLen > 1e-12 ? s.offY / std::hypot(s.offX, s.offY) : 0.0;
	const double lean = 0.45 * (offLen < 1.0 ? offLen : 1.0);

	std::vector<m::vec3> verts;
	std::vector<uint32_t> tris;
	verts.reserve((size_t)n * (size_t)(nr + nf + 1) + 2);
	tris.reserve(((size_t)4 * n + (size_t)2 * n * (nr + nf)) * 3);

	for (int i = 0; i < n; i++)
		verts.push_back(m::vec3(outline[i].x * outer, outline[i].y * outer, 0.0));
	for (int j = 0; j < nf; j++)
	{
		const double th = (m::kPi * 0.5) * (double)j / (double)nf;
		const double sc = outer - f * std::sin(th);
		const double z = MARGIN + Rz - Rz * std::cos(th);
		for (int i = 0; i < n; i++)
			verts.push_back(m::vec3(outline[i].x * sc, outline[i].y * sc, z));
	}
	std::vector<double> prevS((size_t)n, 1.0 + 1e-3);
	double prevG = -1.0;
	const EVec no(Grain01(s.grain, 11) * 200.0, Grain01(s.grain, 12) * 200.0, Grain01(s.grain, 13) * 200.0);
	std::vector<double> ca((size_t)n), sa((size_t)n);
	for (int i = 0; i < n; i++)
	{
		const double a = std::atan2(outline[i].y, outline[i].x);
		ca[(size_t)i] = std::cos(a);
		sa[(size_t)i] = std::sin(a);
	}
	auto field = [&](int i, double u, double salt)
	{
		const EVec q(ca[(size_t)i] * 1.4 + no.x + salt, sa[(size_t)i] * 1.4 + no.y, u * 4.2 + no.z);
		const double n1 = ProjectDamageNoise(q) - 0.5;
		const double n2 = ProjectDamageNoise(q * 2.3 + EVec(salt, 0.0, 0.0)) - 0.5;
		return (n1 + 0.45 * n2) * 2.2;
	};
	for (int j = 0; j < nr; j++)
	{
		const double u = (double)j / (double)nr;
		double g = 1.0 - std::pow(1.0 - u, s.steep);
		if (s.profile > 0.0)
		{
			// shell-like scar: a small step where the flake broke off (the
			// rim), then a smooth, ever deeper dip to the centre, with faint
			// concentric ripples
			const double step = 0.16 * std::min(1.0, u / 0.12);
			const double shell = step + (1.0 - 0.16) * std::pow(u, 1.45)
				+ 0.035 * std::sin(7.0 * m::kPi * u + s.ripplePh) * u * (1.0 - u);
			g = g + (shell - g) * std::min(1.0, s.profile);
		}
		g += s.ripple * std::sin(3.0 * m::kPi * u + s.ripplePh) * u * (1.0 - u);
		if (g <= prevG) g = prevG + 1e-3;
		prevG = g;
		const double z = MARGIN + Rz + (depth - Rz) * g;
		const double base = 1.0 - std::pow(u, 1.6);
		const double zb = 4.0 * u * (1.0 - u) * (u < 0.3 ? u / 0.3 : 1.0);
		const double bulge = 4.0 * u * (1.0 - u);
		for (int i = 0; i < n; i++)
		{
			const double fx = field(i, u, 0.0) + 0.08 * (Grain01(s.grain, (uint64_t)i, (uint64_t)j) - 0.5);
			double sc = base * (1.0 + 0.6 * s.facet * bulge * fx);
			if (sc < 0.25 * base) sc = 0.25 * base;
			if (j > 0 && sc > prevS[(size_t)i] * 0.97) sc = prevS[(size_t)i] * 0.97;
			prevS[(size_t)i] = sc;
			const double side = ca[(size_t)i] * odx + sa[(size_t)i] * ody;
			const double fzv = field(i, u, 37.0) + 0.08 * (Grain01(s.grain, (uint64_t)i, (uint64_t)j + 977) - 0.5);
			const double zj = j == 0 ? 0.0 : s.facet * 0.9 * zb * fzv * (depth - Rz);
			const double zl = (depth - Rz) * g * lean * side * zb;
			verts.push_back(m::vec3(outline[i].x * sc, outline[i].y * sc, MaxD(z + zj + zl, MARGIN + Rz + 1e-3)));
		}
	}
	const uint32_t apex = (uint32_t)verts.size();
	verts.push_back(m::vec3(0.0, 0.0, MARGIN + depth));
	const uint32_t top = (uint32_t)verts.size();
	verts.push_back(m::vec3(0.0, 0.0, 0.0));

	auto tri = [&tris](uint32_t a, uint32_t b, uint32_t c)
	{
		tris.push_back(a); tris.push_back(b); tris.push_back(c);
	};
	for (int i = 0; i < n; i++)
		tri(top, (uint32_t)((i + 1) % n), (uint32_t)i);
	const int nAll = nf + nr;
	for (int j = 0; j < nAll; j++)
	{
		const uint32_t a = (uint32_t)(j * n), b = (uint32_t)((j + 1) * n);
		for (int i = 0; i < n; i++)
		{
			const uint32_t i2 = (uint32_t)((i + 1) % n);
			tri(a + (uint32_t)i, a + i2, b + i2);
			tri(a + (uint32_t)i, b + i2, b + (uint32_t)i);
		}
	}
	const uint32_t last = (uint32_t)(nAll * n);
	for (int i = 0; i < n; i++)
		tri(last + (uint32_t)i, last + (uint32_t)((i + 1) % n), apex);

	m::MeshGL64 mesh;
	mesh.numProp = 3;
	mesh.vertProperties.resize(verts.size() * 3);
	for (size_t i = 0; i < verts.size(); i++)
	{
		const m::vec3 w = MulPoint(s.frame, verts[i]);
		mesh.vertProperties[i * 3 + 0] = w.x;
		mesh.vertProperties[i * 3 + 1] = w.y;
		mesh.vertProperties[i * 3 + 2] = w.z;
	}
	mesh.triVerts.assign(tris.begin(), tris.end());
	if (m::la::dot(m::la::cross(s.frame[0], s.frame[1]), s.frame[2]) < 0.0)
		for (size_t t = 0; t < mesh.triVerts.size(); t += 3)
			std::swap(mesh.triVerts[t + 1], mesh.triVerts[t + 2]);
	return m::Manifold(mesh);
}

SpallSpec SpallPlan(const m::vec3& p0, const m::vec3& tangent, const m::vec3& nout,
	double su, double sv, double depth, PyRandom& rng, int domeSeg, int outlineN,
	double rimSoft, double tiltDeg)
{
	SpallSpec s;
	s.outline = MakeSpallOutline(su, sv, rng, outlineN);
	s.su = su;
	s.depth = depth;
	s.rimSoft = rimSoft;
	s.rings = domeSeg;

	m::vec3 ev = m::la::cross(nout, tangent);
	const double evL = m::la::length(ev);
	ev = ev / (evL != 0.0 ? evL : 1.0);
	const double tiltRange = tiltDeg >= 0.0 ? tiltDeg : TILT_DEG;
	const double tilt = rng.Uniform(-tiltRange, tiltRange) * m::kPi / 180.0;
	const double offA = rng.Uniform(0.0, 2.0 * m::kPi);
	const double offM = rng.Uniform(0.1, 1.0);
	s.offX = std::cos(offA) * offM * su;
	s.offY = std::sin(offA) * offM * sv;
	s.steep = rng.Uniform(1.8, 3.0);
	s.ripple = rng.Uniform(-0.3, 0.3);
	s.ripplePh = rng.Uniform(0.0, 2.0 * m::kPi);
	s.facet = rng.Uniform(0.12, 0.28);
	s.grain = (uint64_t)(rng.Random() * 4294967296.0);

	const m::vec3 ev_t = RotAxis(ev, tangent, tilt);
	const m::vec3 z_t = RotAxis(nout * -1.0, tangent, tilt);
	const m::vec3 t = p0 + nout * MARGIN;
	s.frame = m::mat3x4(tangent, ev_t, z_t, t);
	return s;
}

void SpallBuildAll(const std::vector<SpallSpec>& specs, const std::vector<bool>& keep,
	std::vector<m::Manifold>& out)
{
	std::vector<size_t> idx;
	idx.reserve(specs.size());
	for (size_t i = 0; i < specs.size(); i++)
		if (i >= keep.size() || keep[i])
			idx.push_back(i);
	const size_t base = out.size();
	out.resize(base + idx.size());
	ProjectDamageParallelFor(0, (int64_t)idx.size(), [&](int64_t k)
	{
		out[base + (size_t)k] = BowlCutter(specs[idx[(size_t)k]]);
	});
}
}

namespace
{
m::vec3 EdgeOutNormal(const ExposedEdge& e, const std::vector<EVec>& normals)
{
	std::vector<Int32> faces;
	for (auto& r : e.reprs)
		if (std::find(faces.begin(), faces.end(), r.first) == faces.end())
			faces.push_back(r.first);
	std::sort(faces.begin(), faces.end());
	m::vec3 sum(0.0, 0.0, 0.0);
	for (Int32 f : faces)
		sum = sum + normals[f];
	const double L = m::la::length(sum);
	return sum / (L != 0.0 ? L : 1.0);
}

// Wood surface chip: one knife facet. Local frame: x along the cut (grain),
// y across the blade, z the surface normal. Along x the floor is a circular
// arc: the knife enters at x = -L/2 at zero depth, is deepest at x = +L/4 and
// leaves at x = +L/2 still ~0.9 deep - the step where the shaving broke off.
// Across, the floor is flat in the middle and rises to the surface at the
// outline's sides (depth x (1 - (|y| / w(x))^2)), so neighbouring facets meet
// in sharp ridges instead of walls. The outline is a tear-drop superellipse,
// narrow where the knife enters. Built as a height-field solid (floor .. top)
// intersected with the outline prism; `roll` tilts the blade across the cut.
static m::Manifold KnifeCutter(const m::vec3& p, const m::vec3& n, const m::vec3& along,
	double L, double W, double d, double roll, int seg)
{
	using CS = m::CrossSection;
	const double x0 = 0.25 * L, se = 0.5 * L + x0;
	const double R = (se * se + d * d) / (2.0 * d);
	const double top = 0.6 * d + 0.05 * L;
	const double ex = 2.6;   // outline superellipse exponent: flat-ish sides
	auto halfW = [&](double x) { return 0.5 * W * (0.45 + 0.55 * std::min(1.0, std::max(0.0, (x + 0.5 * L) / L))); };
	auto floorZ = [&](double x, double y) {
		const double s2 = std::min(R * R, (x - x0) * (x - x0));
		const double arc = d - (R - std::sqrt(R * R - s2));          // depth along the cut
		const double w = std::max(1e-9, halfW(x));
		const double t = std::min(1.0, std::abs(y) / w);
		return -std::max(0.0, arc) * (1.0 - t * t);
	};
	// height-field solid over the outline's box: bottom = floor, top = `top`
	const int nx = std::max(10, (2 * seg) / 3), ny = std::max(6, seg / 3);
	const double pad = 0.03 * L, xa = -0.5 * L - pad, xb = 0.5 * L + pad, yb = 0.5 * W + pad;
	m::MeshGL hf;
	hf.numProp = 3;
	auto vid = [&](int i, int j, int layer) { return uint32_t(layer * (nx + 1) * (ny + 1) + i * (ny + 1) + j); };
	for (int layer = 0; layer < 2; ++layer)
		for (int i = 0; i <= nx; ++i)
			for (int j = 0; j <= ny; ++j) {
				const double x = xa + (xb - xa) * i / nx, y = -yb + 2.0 * yb * j / ny;
				const double z = layer == 0 ? std::min(floorZ(x, y), top - 1e-4) : top;
				hf.vertProperties.push_back((float)x); hf.vertProperties.push_back((float)y); hf.vertProperties.push_back((float)z);
			}
	auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t dd) {
		hf.triVerts.insert(hf.triVerts.end(), {a, b, c, a, c, dd});
	};
	for (int i = 0; i < nx; ++i)
		for (int j = 0; j < ny; ++j) {
			quad(vid(i, j, 0), vid(i, j + 1, 0), vid(i + 1, j + 1, 0), vid(i + 1, j, 0));   // floor, facing -z
			quad(vid(i, j, 1), vid(i + 1, j, 1), vid(i + 1, j + 1, 1), vid(i, j + 1, 1));   // top, facing +z
		}
	for (int i = 0; i < nx; ++i) {
		quad(vid(i, 0, 0), vid(i + 1, 0, 0), vid(i + 1, 0, 1), vid(i, 0, 1));             // y = -yb
		quad(vid(i, ny, 0), vid(i, ny, 1), vid(i + 1, ny, 1), vid(i + 1, ny, 0));           // y = +yb
	}
	for (int j = 0; j < ny; ++j) {
		quad(vid(0, j, 0), vid(0, j, 1), vid(0, j + 1, 1), vid(0, j + 1, 0));               // x = xa
		quad(vid(nx, j, 0), vid(nx, j + 1, 0), vid(nx, j + 1, 1), vid(nx, j, 1));           // x = xb
	}
	m::Manifold slab(hf);
	if (slab.Status() != m::Manifold::Error::NoError || slab.IsEmpty()) return m::Manifold();
	if (std::abs(roll) > 1e-9) slab = slab.Rotate(roll * 180.0 / m::kPi, 0.0, 0.0);
	m::SimplePolygon out;
	const int no = std::max(16, 2 * seg);
	for (int i = 0; i < no; ++i) {
		const double a = 2.0 * m::kPi * i / no;
		const double c = std::cos(a), sn = std::sin(a);
		const double x = 0.5 * L * (c >= 0 ? 1 : -1) * std::pow(std::abs(c), 2.0 / ex);
		const double y = halfW(x) * (sn >= 0 ? 1 : -1) * std::pow(std::abs(sn), 2.0 / ex);
		out.push_back(m::vec2(x, y));
	}
	m::Manifold prism = m::Manifold::Extrude(CS(out).ToPolygons(), top + 1.3 * d).Translate(m::vec3(0, 0, -1.3 * d));
	m::Manifold cut = slab ^ prism;
	const m::vec3 zn = m::la::normalize(n);
	m::vec3 xv = along - zn * m::la::dot(along, zn);
	xv = m::la::length(xv) > 1e-9 ? m::la::normalize(xv) : m::la::normalize(m::la::cross(std::abs(zn.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(1, 0, 0), zn));
	const m::vec3 yv = m::la::cross(zn, xv);
	return cut.Transform(m::mat3x4(xv, yv, zn, p));
}

m::Manifold OrganicCutter(const m::MeshGL& unit, const m::vec3& p0, const m::vec3& nrm,
	double su, double sv, double depth, double roughOut, double roughIn,
	const EVec& noiseOffset, double spin, bool crater = false)
{
	m::MeshGL mesh = unit;
	const double lobes = crater ? (double)(4 + (int)std::fmod(std::fabs(noiseOffset.x) * 7.0, 4.0)) : 0.0;
	const double lobeAmp = crater ? 0.1 + 0.1 * std::fmod(std::fabs(noiseOffset.y) * 0.37, 1.0) : 0.0;
	const double lean = crater ? std::fmod(std::fabs(noiseOffset.z) * 1.7, 2.0 * m::kPi) : 0.0;
	const Int32 np = (Int32)mesh.numProp;
	const size_t nv = mesh.vertProperties.size() / (size_t)np;
	for (size_t i = 0; i < nv; i++)
	{
		float* v = &mesh.vertProperties[i * (size_t)np];
		m::vec3 d(v[0], v[1], v[2]);
		const double len = m::la::length(d);
		if (len > 0.0)
			d = d / len;
		const double a = std::atan2(d.y, d.x);
		const double ca = std::cos(a), sa = std::sin(a);
		const EVec n1(ca * 1.3 + noiseOffset.x, sa * 1.3 + noiseOffset.y, noiseOffset.z);
		const EVec n2(ca * 3.4 + noiseOffset.z, sa * 3.4 + noiseOffset.x, noiseOffset.y);
		double r = 1.0
			+ roughOut * 0.13 * (ProjectDamageNoise(n1) - 0.5) * 2.0
			+ roughOut * 0.05 * (ProjectDamageNoise(n2) - 0.5) * 2.0;
		if (crater)
		{
			const EVec n3(ca * 7.1 + noiseOffset.y, sa * 7.1 + noiseOffset.z, noiseOffset.x);
			r += roughOut * 0.04 * (ProjectDamageNoise(n3) - 0.5) * 2.0;
			r *= 1.0 - lobeAmp + lobeAmp * std::pow(std::fabs(std::sin(0.5 * lobes * a + noiseOffset.x)), 0.6);
		}
		const double rr = r > 0.4 ? r : 0.4;

		double zScale = 1.0;
		if (roughIn > 0.0)
		{
			const double fade = std::fabs(d.z);
			const EVec m1(d.x * 2.2 + noiseOffset.y, d.y * 2.2 + noiseOffset.z, d.z * 2.2 + noiseOffset.x);
			const EVec m2 = m1 * 2.6;
			zScale += roughIn * fade * (0.30 * (ProjectDamageNoise(m1) - 0.5) * 2.0
									  + 0.12 * (ProjectDamageNoise(m2) - 0.5) * 2.0);
			if (zScale < 0.15) zScale = 0.15;
		}
		if (crater)
		{
			const double side = d.x * std::cos(lean) + d.y * std::sin(lean);
			zScale *= 1.0 + 0.3 * side;
			if (zScale < 0.15) zScale = 0.15;
			const double fz = std::fabs(d.z);
			if (fz > 1e-6)
			{
				const double t = fz * 3.0;
				const double k = std::floor(t);
				const double x = t - k;
				const double x3 = x * x * x, y3 = (1.0 - x) * (1.0 - x) * (1.0 - x);
				const double terr = (k + x3 / (x3 + y3)) / 3.0;
				zScale *= (0.6 * fz + 0.4 * terr) / fz;
			}
		}
		v[0] = (float)(d.x * rr * su);
		v[1] = (float)(d.y * rr * sv);
		v[2] = (float)(d.z * zScale * depth);
	}

	const m::vec3 t = PerpFrame(nrm);
	const m::vec3 tSpun = RotAxis(t, nrm, spin);
	const m::vec3 bSpun = m::la::cross(nrm, tSpun);
	return m::Manifold(mesh).Transform(m::mat3x4(tSpun, bSpun, nrm, p0));
}

double LocalThickness(const ProjectDamageBVH* bvh, const m::vec3& p0, const m::vec3& nout, double rayLen)
{
	const double eps = 0.01;
	const m::vec3 origin = p0 - nout * eps;
	const m::vec3 dir(-nout.x, -nout.y, -nout.z);
	double dist = 0.0;
	if (!bvh->Raycast(origin, dir, rayLen, &dist))
		return 0.0;
	return dist + eps;
}

struct ChipPlacement
{
	Int32 amount = 0;
	Float sizeMin = 0.0, sizeMax = 0.0, depthMin = 0.0, depthMax = 0.0;
	Float offset = 0.0, randomOffset = 0.0, detail = 1.0;
	Float stretch = 1.0, normMaxSize = 0.3;
	Float sizeRef = 0.0;
	Float rimSoft = 0.0;
	Int32 seed = 0;
	const IFieldMask* mask = nullptr;
	Bool  amountPerChain = false;
	Float loopRef = 0.0;
	Float cluster = 0.0, fine = 0.0, profile = 0.0, asym = 0.0, guard = 0.0;
	Bool  normEnableCap = false;
    const std::vector<std::vector<EVec>>* paintPaths = nullptr;
    double paintRadius = 0.0;

    Bool InPaint(const m::vec3& point) const
    {
        if (paintRadius <= 0.0) return true;
        if (paintPaths == nullptr) return false;
        const double r2 = paintRadius * paintRadius;
        for (const auto& path : *paintPaths)
            for (size_t i = 1; i < path.size(); ++i)
            {
                const m::vec3 a(path[i-1].x, path[i-1].y, path[i-1].z);
                const m::vec3 b(path[i].x, path[i].y, path[i].z);
                const auto ab = b - a;
                const double den = m::la::dot(ab, ab);
                const double t = den > 0.0 ? std::max(0.0, std::min(1.0, m::la::dot(point-a, ab)/den)) : 0.0;
                const auto delta = point - (a + ab*t);
                if (m::la::dot(delta, delta) <= r2) return true;
            }
        return false;
    }

    std::vector<std::pair<double,double>> PaintedIntervals(const m::vec3& a, const m::vec3& b) const
    {
        if (paintRadius <= 0.0) return {{0.0,1.0}};
        std::vector<std::pair<double,double>> intervals;
        if (!paintPaths) return intervals;
        const auto direction=b-a;
        for (const auto& path:*paintPaths) for (size_t i=1;i<path.size();++i) {
            const m::vec3 start(path[i-1].x,path[i-1].y,path[i-1].z);
            const m::vec3 end(path[i].x,path[i].y,path[i].z);
            const auto axis=end-start; const double axis2=m::la::dot(axis,axis);
            const double u0=axis2>1e-20?m::la::dot(a-start,axis)/axis2:0.0;
            const double du=axis2>1e-20?m::la::dot(direction,axis)/axis2:0.0;
            std::vector<double> breaks={0.0,1.0};
            if (std::abs(du)>1e-20) for (double boundary:{0.0,1.0}) {
                const double t=(boundary-u0)/du;
                if(t>0.0&&t<1.0)breaks.push_back(t);
            }
            std::sort(breaks.begin(),breaks.end());
            for(size_t j=1;j<breaks.size();++j) {
                double lo=breaks[j-1],hi=breaks[j];
                const double u=u0+du*(lo+hi)*0.5;
                auto delta=a-start; auto velocity=direction;
                if(axis2>1e-20&&u>0.0&&u<1.0) {delta-=axis*u0;velocity-=axis*du;}
                else if(u>=1.0)delta=a-end;
                const double aa=m::la::dot(velocity,velocity);
                const double bb=2.0*m::la::dot(delta,velocity);
                const double cc=m::la::dot(delta,delta)-paintRadius*paintRadius;
                if(aa<1e-20) {if(cc>0.0)continue;}
                else {
                    const double discriminant=bb*bb-4.0*aa*cc;
                    if(discriminant<0.0)continue;
                    const double root=std::sqrt(discriminant);
                    lo=std::max(lo,(-bb-root)/(2.0*aa));hi=std::min(hi,(-bb+root)/(2.0*aa));
                }
                if(hi-lo>1e-10)intervals.emplace_back(lo,hi);
            }
        }
        std::sort(intervals.begin(),intervals.end());
        std::vector<std::pair<double,double>> merged;
        for(const auto& span:intervals) {
            if(merged.empty()||span.first>merged.back().second+1e-10)merged.push_back(span);
            else merged.back().second=std::max(merged.back().second,span.second);
        }
        return merged;
    }

	static ChipPlacement FromParams(const ProjectDamageParams& p)
	{
		ChipPlacement c;
		c.amount = p.amount;
		c.sizeMin = p.sizeMin;   c.sizeMax = p.sizeMax;
		c.sizeRef = p.sizeMax;
		c.depthMin = p.depthMin; c.depthMax = p.depthMax;
		c.offset = p.offset;     c.randomOffset = p.randomOffset;
		c.detail = p.detail;     c.stretch = p.stretch;
		c.normMaxSize = p.normMaxSize;
		c.rimSoft = p.chipRimSoft;
		c.seed = p.seed + p.chipSeed;
		c.mask = p.mask;
		c.cluster = Clamp01(p.chipCluster);
		c.fine = MaxD(0.0, p.chipFine);
		c.profile = Clamp01(p.chipProfile);
		c.asym = Clamp01(p.chipAsym);
		c.guard = MaxD(0.0, p.chipGuard);
		c.normEnableCap = p.normEnable;
		return c;
	}
};

static std::vector<Chain> SplitChainsByAvoid(const std::vector<Chain>& in,
	const std::vector<bool>* avoid, const EVec* pts)
{
	if (avoid == nullptr)
		return in;
	bool any = false;
	for (size_t i = 0; i < avoid->size() && !any; i++)
		any = (*avoid)[i];
	if (!any)
		return in;

	std::vector<Chain> out;
	for (const Chain& c : in)
	{
		std::map<std::pair<Int32, Int32>, ExposedEdge*> emap;
		for (ExposedEdge* e : c.edges)
			emap[EdgeKey(e->p0, e->p1)] = e;
		const std::vector<Int32>& P = c.points;
		const size_t nseg = P.size() > 0 ? P.size() - 1 : 0;
		auto segAvoided = [&](size_t k) {
			const auto it = emap.find(EdgeKey(P[k], P[k + 1]));
			return it != emap.end() && EdgeAvoided(*it->second, avoid);
		};
		size_t i = 0;
		while (i < nseg)
		{
			if (segAvoided(i)) { i++; continue; }
			size_t j = i;
			while (j < nseg && !segAvoided(j))
				j++;
			Chain sub;
			sub.points.assign(P.begin() + (std::ptrdiff_t)i,
				P.begin() + (std::ptrdiff_t)j + 1);
			for (size_t k = i; k < j; k++)
				sub.edges.push_back(emap[EdgeKey(P[k], P[k + 1])]);
			double L = 0.0;
			for (size_t k = 0; k + 1 < sub.points.size(); k++)
				L += m::la::length(pts[sub.points[k + 1]] - pts[sub.points[k]]);
			sub.length = L;
			sub.blockedStart = (i == 0) ? c.blockedStart : true;
			sub.blockedEnd   = (j == nseg) ? c.blockedEnd : true;
			out.push_back(std::move(sub));
			i = j;
		}
	}
	return out;
}

std::vector<m::Manifold> PlaceCutters(const std::vector<Chain>& chains,
	const EVec* pts, const std::vector<EVec>& normals, const ChipPlacement& params,
	const ProjectDamageBVH* thicknessBvh, double thicknessRayLen)
{
	std::vector<m::Manifold> cutters;
	if (chains.empty() || params.amount <= 0)
		return cutters;

	std::map<std::pair<Int32, Int32>, ExposedEdge*> emap = BuildEdgeMap(chains);

	struct Seg { m::vec3 pa, pb; double L; ExposedEdge* e; };
	struct ChainData { std::vector<Seg> segs; double length; bool blockedStart, blockedEnd; };
	std::vector<ChainData> chainData;
	double totalLen = 0.0;
	for (const Chain& ch : chains)
	{
		const std::vector<Int32>& P = ch.points;
		std::vector<Seg> segs;
		double length = 0.0;
		for (size_t i = 0; i + 1 < P.size(); i++)
		{
			const Int32 a = P[i], b = P[i + 1];
			const m::vec3 pa = pts[a], pb = pts[b];
			const double L = m::la::length(pb - pa);
			if (L == 0.0)
				continue;
			auto it = emap.find(EdgeKey(a, b));
            if(params.paintRadius>0.0) {
                for(const auto& span:params.PaintedIntervals(pa,pb)) {
                    const double covered=L*(span.second-span.first);
                    segs.push_back({pa+(pb-pa)*span.first,pa+(pb-pa)*span.second,covered,
                                    it!=emap.end()?it->second:nullptr});
                    length+=covered;
                }
            } else {
                segs.push_back({pa,pb,L,it!=emap.end()?it->second:nullptr});
                length+=L;
            }
		}
		if (!segs.empty())
		{
			chainData.push_back({ std::move(segs), length, ch.blockedStart, ch.blockedEnd });
			totalLen += length;
		}
	}
	if (totalLen == 0.0)
		return cutters;

	PyRandom rng((uint32_t)params.seed);
	// own streams, so the cluster field and the fine chips never shift the
	// main draws (Clustering 0 / Fine 0 place exactly what they did before)
	PyRandom rngFine((uint32_t)(params.seed + 7717));
	const double clusterSalt = Grain01((uint64_t)(uint32_t)params.seed, 991) * 500.0;

	Int32 chipIdx = 0;
	std::vector<SpallSpec> specs;
	std::vector<bool> keep;
	struct Placed { const ChainData* cd; double s, su, sv, depth; };
	std::vector<Placed> placed;
	const double tiltDeg = TILT_DEG * (1.0 + 1.8 * params.asym);
	// a tilted or shell-shaped scar removes less than the round bowl at the
	// same depth; deepen it so the look changes, not the amount of damage
	const double lookDepth = (1.0 + 0.6 * params.asym) * (1.0 + 0.3 * params.profile);

	// Worn patches: chips follow a 1D density along each chain (low-frequency
	// noise, patch length a few chip sizes), so damage gathers in groups with
	// intact runs between. Position s in [0, L) is remapped through the
	// inverse of that density's cumulative.
	auto clusterWarp = [&](const ChainData& cd, size_t chainNo, double sIn) {
		if (params.cluster <= 0.0 || cd.length <= 0.0) return sIn;
		const int K = 96;
		const double patch = MaxD(4.0 * params.sizeMax * params.stretch, 1e-6);
		double cum[K + 1];
		cum[0] = 0.0;
		for (int k = 0; k < K; k++)
		{
			const double x = (k + 0.5) / K * cd.length;
			const double nz = ProjectDamageNoise(EVec(x / patch + clusterSalt, 13.7 * (double)chainNo + 0.5, 3.1))
				+ 0.5 * ProjectDamageNoise(EVec(2.1 * x / patch + clusterSalt, 7.3 * (double)chainNo, 9.7));
			const double d = std::pow(MaxD(0.02, nz / 1.5), 1.0 + 4.0 * params.cluster);
			cum[k + 1] = cum[k] + d;
		}
		const double target = sIn / cd.length * cum[K];
		int k = 0;
		while (k < K - 1 && cum[k + 1] < target) k++;
		const double span = cum[k + 1] - cum[k];
		const double f = span > 1e-12 ? (target - cum[k]) / span : 0.5;
		const double sWarp = (k + f) / K * cd.length;
		return sIn + (sWarp - sIn) * params.cluster;
	};

	size_t chainNo = 0;
    double paintedLength=0.0;
	for (const ChainData& cd : chainData)
	{
		chainNo++;
		double prevS = -1.0, prevSu = 0.0;
		int nChips = params.amountPerChain
			? params.amount
			: PyRound(params.amount * cd.length / totalLen);
        if(params.paintRadius>0.0&&!params.amountPerChain) {
            const int before=(int)std::floor(params.amount*paintedLength/totalLen+1e-8);
            paintedLength+=cd.length;
            nChips=(int)std::floor(params.amount*paintedLength/totalLen+1e-8)-before;
        }
		if (params.amountPerChain && params.loopRef > 0.0 && cd.length < params.loopRef)
			nChips = (int)std::floor(params.amount * cd.length / params.loopRef + rng.Random());
		if (nChips <= 0)
			continue;
		const double step = cd.length / nChips;
		for (int k = 0; k < nChips; k++)
		{
			const double even = (k + 0.5) * step + params.offset * cd.length;
			const double rnd = rng.Random() * cd.length;
			double s = std::fmod(even + (rnd - even) * params.randomOffset, cd.length);
			if (s < 0.0) s += cd.length;
			if (!params.amountPerChain && !cd.blockedStart && !cd.blockedEnd)
			{
				const double toEnds = cd.length * (0.5 - 0.5 * std::cos(m::kPi * s / cd.length));
				s += (toEnds - s) * 0.35;
			}
			s = clusterWarp(cd, chainNo, s);
			const double uCluster = rng.Random();
			const double uSize = rng.Random();
			const double uDepth = rng.Random();
			const double tSize = std::pow(uSize, 1.7);
			double su = params.sizeMin + (params.sizeMax - params.sizeMin) * tSize;
			if (prevS >= 0.0 && uCluster < 0.32)
			{
				const double dir = uCluster < 0.16 ? 1.0 : -1.0;
				s = prevS + dir * (0.75 * prevSu + 0.55 * su) * params.stretch;
				s = std::fmod(s, cd.length);
				if (s < 0.0) s += cd.length;
			}
			if (s > cd.length - 1e-6) s = cd.length - 1e-6;
			prevS = s;
			prevSu = su;
			const double suDrawn = su;
			double tDepth = 0.65 * tSize + 0.35 * uDepth;
			double depth = params.depthMin + (params.depthMax - params.depthMin) * tDepth;
			depth *= lookDepth;
			if (cd.blockedStart || cd.blockedEnd)
			{
				double half = su * params.stretch * SPALL_MAX_RADIUS;
				if (2.0 * half > cd.length)
				{
					half = cd.length * 0.5;
					su = half / (params.stretch * SPALL_MAX_RADIUS);
				}
				if (cd.blockedStart && s < half) s = half;
				if (cd.blockedEnd && s > cd.length - half) s = cd.length - half;
			}
			double acc = 0.0;
			for (const Seg& sg : cd.segs)
			{
				if (acc + sg.L >= s)
				{
					const double lt = (s - acc) / sg.L;
					const m::vec3 p0 = sg.pa + (sg.pb - sg.pa) * lt;
					const m::vec3 tangent = (sg.pb - sg.pa) / sg.L;
					const m::vec3 nout = sg.e != nullptr
						? EdgeOutNormal(*sg.e, normals)
						: m::vec3(0.0, 1.0, 0.0);
					if (thicknessBvh != nullptr)
					{
						const double t = LocalThickness(thicknessBvh, p0, nout, thicknessRayLen);
						if (t > 0.0)
						{
							if (params.normMaxSize > 0.0 && params.normEnableCap)
							{
								const double cap = params.normMaxSize * t;
								if (su > cap) su = cap;
								if (depth > cap) depth = cap;
							}
							if (params.guard > 0.0)
							{
								// thin-wall guard: never through, never wider
								// than the part is thick
								if (depth > params.guard * t) depth = params.guard * t;
								if (su > 2.5 * params.guard * t) su = 2.5 * params.guard * t;
							}
						}
					}
					Bool masked = false;
					double sizeScale = 1.0;
					if (params.mask != nullptr)
					{
						const double w0 = params.mask->WeightIfUsed(p0);
						masked = !params.mask->KeepW(w0, chipIdx, params.seed);
						sizeScale = params.mask->SizeScaleW(w0);
						if (sizeScale <= 1e-3)
						{
							masked = true;
							sizeScale = 1e-3;
						}
					}
					chipIdx++;
                    masked = masked || !params.InPaint(p0);

					const double sv = su * SPALL_ASPECT * sizeScale;
					const double suAlong = su * params.stretch * sizeScale;
					int domeSeg = 0, outlineN = 0;
					CutterSegs(params.detail,
						params.sizeRef > 0.0 ? suDrawn / params.sizeRef : 1.0,
						domeSeg, outlineN);
					specs.push_back(SpallPlan(p0, tangent, nout, suAlong, sv,
						depth * sizeScale, rng, domeSeg, outlineN, params.rimSoft, tiltDeg));
					specs.back().profile = params.profile;
					keep.push_back(!masked);
					if (!masked)
						placed.push_back({ &cd, s, su * sizeScale, sv, depth * sizeScale });
					break;
				}
				acc += sg.L;
			}
		}
	}

	// Fine chips: small flakes fraying the rims of the big chips, close to
	// them along the edge - what makes a worn edge read as broken, not cut.
	if (params.fine > 0.0 && !placed.empty())
	{
		const int nFine = (int)std::floor(params.fine * (double)placed.size() * 1.6 + rngFine.Random());
		for (int k = 0; k < nFine; k++)
		{
			const Placed& par = placed[(size_t)(rngFine.Random() * (double)placed.size()) % placed.size()];
			const ChainData& cd = *par.cd;
			const double side = rngFine.Random() < 0.5 ? -1.0 : 1.0;
			const double fs = rngFine.Uniform(0.18, 0.42);
			double su = par.su * fs;
			double s = par.s + side * par.su * params.stretch * rngFine.Uniform(0.55, 1.6);
			if (s < 0.5 * su || s >= cd.length - 0.5 * su)
				continue;
			const double depth = MaxD(1e-4, par.depth * rngFine.Uniform(0.15, 0.4));
			const double uPick = rngFine.Random();
			double acc = 0.0;
			for (const Seg& sg : cd.segs)
			{
				if (acc + sg.L < s) { acc += sg.L; continue; }
				const double lt = (s - acc) / sg.L;
				const m::vec3 p0 = sg.pa + (sg.pb - sg.pa) * lt;
				const m::vec3 tangent = (sg.pb - sg.pa) / sg.L;
				const m::vec3 nout = sg.e != nullptr ? EdgeOutNormal(*sg.e, normals) : m::vec3(0.0, 1.0, 0.0);
				bool masked = !params.InPaint(p0);
				double ms = 1.0;
				if (params.mask != nullptr)
				{
					const double w0 = params.mask->WeightIfUsed(p0);
					masked = masked || !params.mask->KeepW(w0, chipIdx, params.seed + 7717);
					ms = params.mask->SizeScaleW(w0);
					if (ms <= 1e-3) masked = true;
				}
				chipIdx++;
				if (masked) break;
				int domeSeg = 0, outlineN = 0;
				CutterSegs(params.detail, params.sizeRef > 0.0 ? 2.0 * su / params.sizeRef : 0.5, domeSeg, outlineN);
				const double sv = su * SPALL_ASPECT * (0.8 + 0.4 * uPick) * ms;
				specs.push_back(SpallPlan(p0, tangent, nout, su * params.stretch * ms, sv,
					depth * ms, rngFine, domeSeg, outlineN, params.rimSoft, tiltDeg * 1.3));
				specs.back().profile = params.profile;
				keep.push_back(true);
				break;
			}
		}
	}
	SpallBuildAll(specs, keep, cutters);
	return cutters;
}

struct SurfacePaintFrame
{
	EVec origin, normal, tangent, bitangent;
	std::vector<EVec> xy;
	double xmin = 1e100, ymin = 1e100, xmax = -1e100, ymax = -1e100;
	double weight = 0.0;
};
double PaintClearance(const SurfacePaintFrame& f, double x, double y)
{
	bool inside = false;
	double nearest = 1e100;
	for (size_t i = 0, j = f.xy.size() - 1; i < f.xy.size(); j = i++)
	{
		const EVec a = f.xy[j], b = f.xy[i];
		if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
			inside = !inside;
		const EVec v(x - a.x, y - a.y, 0.0), ab = b - a;
		const double den = m::la::dot(ab, ab);
		const double t = den > 0.0 ? std::max(0.0, std::min(1.0, m::la::dot(v, ab) / den)) : 0.0;
		nearest = std::min(nearest, m::la::length(v - ab * t));
	}
	return inside ? nearest : -1.0;
}

std::vector<m::Manifold> PlaceSurfaceCutters(MeshView in,
	const std::vector<EVec>& normals, const std::vector<ExposedEdge>& exposed,
	const ProjectDamageParams& params, const std::vector<Bool>* skipPoly = nullptr,
	const std::vector<SurfacePaintRegion>* paintRegions = nullptr)
{
	std::vector<m::Manifold> cutters;
	if (params.surfPaintOnly && (!paintRegions || paintRegions->empty())) return cutters;
	if (params.surfAmount <= 0)
		return cutters;

	const Int32 vcnt = in.npolys;
	const EVec* pts = in.pts;
	const ProjectDamagePoly* polys = in.polys;
	if (vcnt <= 0)
		return cutters;

	auto triArea = [](const EVec& a, const EVec& b, const EVec& c) {
		return m::la::length(m::la::cross(b - a, c - a)) * 0.5;
	};

	std::vector<double> cdf((size_t)vcnt);
	std::vector<double> firstShare((size_t)vcnt, 1.0);
	double total = 0.0;
	for (Int32 i = 0; i < vcnt; i++)
	{
		const ProjectDamagePoly& cp = polys[i];
		const Bool skip = skipPoly != nullptr && (*skipPoly)[(size_t)i];
		const double a1 = skip ? 0.0 : triArea(pts[cp.a], pts[cp.b], pts[cp.c]);
		const double a2 = (!skip && cp.c != cp.d) ? triArea(pts[cp.a], pts[cp.c], pts[cp.d]) : 0.0;
		const double a = a1 + a2;
		if (a > 0.0)
			firstShare[(size_t)i] = a1 / a;
		total += a;
		cdf[(size_t)i] = total;
	}
	if (total <= 0.0)
		return cutters;

	std::vector<SurfacePaintFrame> frames;
	ProjectDamageBVH paintBvh;
	double paintTotal = 0.0;
	const bool needBvh = params.surfPaintOnly || params.surfCluster > 0.0 || params.surfGuard > 0.0;
	if (needBvh)
		paintBvh.Build(in);
	PyRandom rngLook(((uint32_t)params.seed + (uint32_t)params.surfSeed + 5200u));
	struct Crater { EVec p; double su; EVec n; };
	std::vector<Crater> craters;
	if (params.surfPaintOnly)
	{
		for (const auto& region : *paintRegions)
		{
			if (region.points.size() < 3) continue;
			SurfacePaintFrame f;
			f.origin = EVec(0.0);
			for (const auto& v : region.points) f.origin = f.origin + v;
			f.origin = f.origin / (double)region.points.size();
			f.normal = region.normal;
			const EVec axis = std::abs(f.normal.z) < 0.9 ? EVec(0, 0, 1) : EVec(0, 1, 0);
			f.tangent = m::la::normalize(m::la::cross(axis, f.normal));
			f.bitangent = m::la::cross(f.normal, f.tangent);
			for (const auto& v : region.points)
			{
				const EVec d = v - f.origin;
				const double x = m::la::dot(d, f.tangent), y = m::la::dot(d, f.bitangent);
				f.xy.push_back(EVec(x, y, 0));
				f.xmin = std::min(f.xmin, x); f.xmax = std::max(f.xmax, x);
				f.ymin = std::min(f.ymin, y); f.ymax = std::max(f.ymax, y);
			}
			const double area = (f.xmax - f.xmin) * (f.ymax - f.ymin);
			if (area <= 1e-9) continue;
			paintTotal += area;
			f.weight = paintTotal;
			frames.push_back(std::move(f));
		}
		if (frames.empty()) return cutters;
	}
	PyRandom rng(((uint32_t)params.seed + (uint32_t)params.surfSeed + 5000u));
	const int seg = MaxI(8, PyRound(SURF_SEG * params.surfDetail));
	const m::MeshGL unit = m::Manifold::Sphere(1.0, seg).GetMeshGL();
	// Wood (knife facets): own stream for the knife's look, so the placement
	// draws - and so the result for Concrete - stay exactly as before
	const bool knife = params.surfType == 1;
	PyRandom rngKnife(((uint32_t)params.seed + (uint32_t)params.surfSeed + 5100u));
	m::vec3 grain(params.surfGrainX, params.surfGrainY, params.surfGrainZ);
	if (m::la::length(grain) < 1e-9) grain = m::vec3(0.0, 0.0, 1.0);
	grain = m::la::normalize(grain);

	const int64_t attempts = params.surfPaintOnly ? (int64_t)params.surfAmount * 50 : params.surfAmount;
	for (int64_t k = 0; k < attempts; k++)
	{
		if (params.surfPaintOnly && cutters.size() >= (size_t)params.surfAmount) break;
		const double pick = rng.Random() * total;
		Int32 pi = (Int32)(std::lower_bound(cdf.begin(), cdf.end(), pick) - cdf.begin());
		if (pi >= vcnt)
			pi = vcnt - 1;
		const ProjectDamagePoly& cp = polys[pi];
		const Bool firstTri = rng.Random() < firstShare[(size_t)pi] || cp.c == cp.d;
		const EVec& A = pts[cp.a];
		const EVec& B = firstTri ? pts[cp.b] : pts[cp.c];
		const EVec& C = firstTri ? pts[cp.c] : pts[cp.d];

		const double r1 = std::sqrt(rng.Random()), r2 = rng.Random();
		EVec p = A * (1.0 - r1) + B * (r1 * (1.0 - r2)) + C * (r1 * r2);
		const SurfacePaintFrame* frame = nullptr;
		double clearance = 1e100;
		if (params.surfPaintOnly)
		{
			const double choice = rng.Random() * paintTotal;
			size_t ri = 0;
			while (ri + 1 < frames.size() && frames[ri].weight < choice) ++ri;
			frame = &frames[ri];
			const double x = rng.Uniform(frame->xmin, frame->xmax);
			const double y = rng.Uniform(frame->ymin, frame->ymax);
			clearance = PaintClearance(*frame, x, y);
			if (clearance <= 1e-3) continue;
			const EVec plane = frame->origin + frame->tangent * x + frame->bitangent * y;
			const double reach = MaxD(params.surfPaintSnap, 1e-3);
			double hit = 0.0;
			if (!paintBvh.Raycast(plane + frame->normal * reach, -frame->normal, reach * 2, &hit, &pi)) continue;
			if (skipPoly && (*skipPoly)[(size_t)pi]) continue;
			if (m::la::dot(normals[pi], frame->normal) < 0.5) continue;
			p = plane + frame->normal * (reach - hit);
		}

		double su = rng.Uniform(params.surfSizeMin, params.surfSizeMax);
		const double aspect = rng.Uniform(0.7, 1.0);
		double depth = rng.Uniform(params.surfDepthMin, params.surfDepthMax);
		// own stream: with every look slider at 0 the placement is unchanged
		const double uSize = rngLook.Random(), uDepth = rngLook.Random();
		const double uClus = rngLook.Random(), uAng = rngLook.Random(), uDist = rngLook.Random();
		if (params.surfSizeBias > 0.0)
		{
			// many small, few big (power law between Min and Max)
			const double ex = 1.0 + 3.0 * Clamp01(params.surfSizeBias);
			su = params.surfSizeMin + (params.surfSizeMax - params.surfSizeMin) * std::pow(uSize, ex);
		}
		if (params.surfDepthFollow > 0.0)
		{
			const double range = params.surfSizeMax - params.surfSizeMin;
			const double tSize = range > 1e-9 ? Clamp01((su - params.surfSizeMin) / range) : 0.5;
			const double follow = params.surfDepthMin + (params.surfDepthMax - params.surfDepthMin)
				* Clamp01(tSize * (0.75 + 0.5 * uDepth));
			depth += (follow - depth) * Clamp01(params.surfDepthFollow);
		}
		// pits form in groups: next to an earlier crater, projected back onto
		// the surface
		if (!frame && params.surfCluster > 0.0 && !craters.empty() && uClus < Clamp01(params.surfCluster))
		{
			const Crater& par = craters[(size_t)(uDist * 9973.0) % craters.size()];
			Int32 pj = -1;
			double hit = 0.0;
			const double ang = 2.0 * m::kPi * uAng;
			const double dist = par.su * (0.9 + 1.1 * uDist);
			// step off the parent in its tangent plane, re-seat on the surface
			const double reach = MaxD(2.0 * dist, 1e-3);
			const EVec nPar = par.n;
			const EVec t1 = m::la::normalize(m::la::cross(std::abs(nPar.z) < 0.9 ? EVec(0, 0, 1) : EVec(0, 1, 0), nPar));
			const EVec b1 = m::la::cross(nPar, t1);
			const EVec q1 = par.p + (t1 * std::cos(ang) + b1 * std::sin(ang)) * dist;
			if (paintBvh.Raycast(q1 + nPar * reach * 0.5, -nPar, reach, &hit, &pj) && pj >= 0
				&& !(skipPoly && (*skipPoly)[(size_t)pj]) && m::la::dot(normals[pj], nPar) > 0.7)
			{
				pi = pj;
				p = q1 + nPar * (reach * 0.5 - hit);
				su *= 0.55 + 0.35 * uSize;
				depth *= 0.7 + 0.3 * uDepth;
			}
		}
		const double drawn = su;
		if (params.surfGuard > 0.0 && !frame)
		{
			double wall = 0.0;
			if (paintBvh.Raycast(p - normals[pi] * 1e-4, -normals[pi], 1e9, &wall))
				depth = std::min(depth, wall * Clamp01(params.surfGuard));
		}
		if (frame)
		{
			const double tilt = std::sqrt(std::max(0.0, 1.0 - std::pow(m::la::dot(normals[pi], frame->normal), 2)));
			su = std::min(su, std::max(0.0, clearance - depth * tilt) /
				(1.0 + 0.18 * std::abs(params.surfNoise)));
			double wall = 0.0;
			if (paintBvh.Raycast(p - normals[pi] * 1e-4, -normals[pi], 1e9, &wall))
				depth = std::min(depth, wall * 0.45);
		}

		double nearest = std::numeric_limits<double>::infinity();
		for (const ExposedEdge& e : exposed)
		{
			const EVec& a = pts[e.p0];
			const EVec& b = pts[e.p1];
			const EVec ab = b - a;
			const double len2 = m::la::dot(ab, ab);
			double t = 0.0;
			if (len2 > 0.0)
			{
				t = m::la::dot(p - a, ab) / len2;
				if (t < 0.0) t = 0.0;
				if (t > 1.0) t = 1.0;
			}
			const double d = m::la::length(p - (a + ab * t));
			if (d < nearest)
				nearest = d;
		}
		if (nearest < su)
			su = nearest * 0.95;
		if (su < MaxD(drawn * 0.25, 1e-3))
			continue;
		const double sv = su * aspect;
		const EVec noiseOffset(rng.Uniform(0.0, 1000.0), rng.Uniform(0.0, 1000.0), rng.Uniform(0.0, 1000.0));
		const double spin = rng.Uniform(0.0, 2.0 * m::kPi);

		double ms = 1.0;
		Bool keepCrater = true;
		if (params.mask != nullptr)
		{
			const double wc = params.mask->WeightIfUsed(p);
			keepCrater = params.mask->KeepW(wc, k, params.seed + 5000);
			ms = params.mask->SizeScaleW(wc);
			if (ms <= 1e-3)
				keepCrater = false;
		}
		if (keepCrater)
		{
			m::Manifold cutter;
			if (knife)
			{
				// along the grain on this face, scattered, mostly one way
				const m::vec3 nn = normals[pi];
				m::vec3 g = grain - nn * m::la::dot(grain, nn);
				if (m::la::length(g) < 0.15)   // end grain: any direction on the face
					g = m::vec3(std::cos(spin), std::sin(spin), 0.3) - nn * m::la::dot(m::vec3(std::cos(spin), std::sin(spin), 0.3), nn);
				if (m::la::length(g) < 1e-9)
					g = m::la::cross(std::abs(nn.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(1, 0, 0), nn);
				g = m::la::normalize(g);
				const double jit = rngKnife.Uniform(-1.0, 1.0) * params.surfKnifeJitter * m::kPi / 180.0;
				g = RotAxis(g, nn, jit);
				if (rngKnife.Random() < params.surfKnifeBack) g = g * -1.0;
				const double roll = rngKnife.Uniform(-6.0, 6.0) * m::kPi / 180.0;
				// a facet is long: the drawn size is its length, ~0.45 of it its width
				const double L = 2.0 * su * ms, W = 0.9 * sv * ms;
				cutter = KnifeCutter(p, nn, g, L, W, std::max(1e-4, depth * ms), roll, seg);
			}
			else
				cutter = OrganicCutter(unit, p, normals[pi], su * ms, sv * ms, depth * ms,
					params.surfNoise, params.surfNoiseIn, noiseOffset, spin, true);
			if (frame)
			{
				const auto cm = cutter.GetMeshGL();
				const double px = m::la::dot(p - frame->origin, frame->tangent);
				const double py = m::la::dot(p - frame->origin, frame->bitangent);
				bool fits = true;
				for (size_t vi = 0; vi < cm.vertProperties.size(); vi += cm.numProp)
				{
					const EVec v(cm.vertProperties[vi], cm.vertProperties[vi + 1], cm.vertProperties[vi + 2]);
					const EVec d = v - frame->origin;
					const double dx = m::la::dot(d, frame->tangent) - px;
					const double dy = m::la::dot(d, frame->bitangent) - py;
					if (std::hypot(dx, dy) > clearance * 0.999) { fits = false; break; }
				}
				if (!fits) continue;
			}
			craters.push_back({ p, su * ms, normals[pi] });
			cutters.push_back(std::move(cutter));
		}
	}
	return cutters;
}

// Closed height-field slab over the square [-R, R]^2 of a local frame
// (xv, yv, zn, origin): the bottom is floorZ(x, y), the top the plane z =
// top. Used by the Broken Corner cutter.
template <typename FloorFn>
static m::Manifold HeightSlab(const m::vec3& xv, const m::vec3& yv, const m::vec3& zn, const m::vec3& o,
	double R, int n, double top, FloorFn floorZ)
{
	m::MeshGL64 hf;
	hf.numProp = 3;
	auto vid = [&](int i, int j, int layer) { return (uint64_t)(layer * (n + 1) * (n + 1) + i * (n + 1) + j); };
	for (int layer = 0; layer < 2; ++layer)
		for (int i = 0; i <= n; ++i)
			for (int j = 0; j <= n; ++j)
			{
				const double x = -R + 2.0 * R * i / n, y = -R + 2.0 * R * j / n;
				const double z = layer == 0 ? std::min(floorZ(x, y), top - 1e-3 * R) : top;
				const m::vec3 w = o + xv * x + yv * y + zn * z;
				hf.vertProperties.push_back(w.x); hf.vertProperties.push_back(w.y); hf.vertProperties.push_back(w.z);
			}
	auto quad = [&](uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
		hf.triVerts.insert(hf.triVerts.end(), { a, b, c, a, c, d });
	};
	for (int i = 0; i < n; ++i)
		for (int j = 0; j < n; ++j)
		{
			quad(vid(i, j, 0), vid(i, j + 1, 0), vid(i + 1, j + 1, 0), vid(i + 1, j, 0));
			quad(vid(i, j, 1), vid(i + 1, j, 1), vid(i + 1, j + 1, 1), vid(i, j + 1, 1));
		}
	for (int i = 0; i < n; ++i)
	{
		quad(vid(i, 0, 0), vid(i + 1, 0, 0), vid(i + 1, 0, 1), vid(i, 0, 1));
		quad(vid(i, n, 0), vid(i, n, 1), vid(i + 1, n, 1), vid(i + 1, n, 0));
	}
	for (int j = 0; j < n; ++j)
	{
		quad(vid(0, j, 0), vid(0, j, 1), vid(0, j + 1, 1), vid(0, j + 1, 0));
		quad(vid(n, j, 0), vid(n, j + 1, 0), vid(n, j + 1, 1), vid(n, j, 1));
	}
	if (m::la::dot(m::la::cross(xv, yv), zn) < 0.0)
		for (size_t t = 0; t < hf.triVerts.size(); t += 3)
			std::swap(hf.triVerts[t + 1], hf.triVerts[t + 2]);
	m::Manifold out(hf);
	if (out.Status() != m::Manifold::Error::NoError || out.IsEmpty())
		return m::Manifold();
	if (out.Volume() < 0.0)
		return m::Manifold();
	return out;
}

// Broken Corner: the corner is knocked off. A fracture face cuts it a
// different distance down each convex edge (the plane's tilt), scooped a
// little into the body, rough and with faint conchoidal ripples; outside
// the broken patch the face rises away so the neighbouring faces stay
// whole. edges: unit directions from the corner into the body, lens their
// lengths. Returns an empty Manifold when the corner is too small.
static m::Manifold BrokenCornerCutter(const m::vec3& p, const std::vector<m::vec3>& edges,
	const std::vector<double>& lens, const m::vec3& outward, double size, double depth,
	double rough, double tilt, double detail, PyRandom& rng)
{
	if (edges.size() < 2 || size <= 0.0)
		return m::Manifold();
	// tilted fracture normal
	const m::vec3 t0 = PerpFrame(outward);
	const m::vec3 b0 = m::la::cross(outward, t0);
	const double ta = rng.Uniform(0.0, 2.0 * m::kPi);
	m::vec3 nrm = outward + (t0 * std::cos(ta) + b0 * std::sin(ta)) * (0.45 * tilt * rng.Uniform(0.4, 1.0));
	nrm = m::la::normalize(nrm);
	// height of the cut above... below the tip, so the cut runs ~size down
	// the edges, never past 45% of an edge
	double mean = 0.0;
	for (const m::vec3& e : edges) mean += MaxD(0.05, -m::la::dot(e, nrm));
	mean /= (double)edges.size();
	double h = size * mean;
	for (size_t i = 0; i < edges.size(); i++)
	{
		const double c = -m::la::dot(edges[i], nrm);
		if (c < 0.05) return m::Manifold();   // an edge runs along the cut: skip this corner
		h = std::min(h, 0.45 * lens[i] * c);
	}
	if (h <= 1e-4)
		return m::Manifold();
	m::vec3 centre(0.0);
	double R0 = 0.0;
	std::vector<m::vec3> foot;
	for (const m::vec3& e : edges)
	{
		const double ti = h / -m::la::dot(e, nrm);
		foot.push_back(p + e * ti);
		centre = centre + foot.back();
	}
	centre = centre / (double)foot.size();
	for (const m::vec3& q : foot) R0 = MaxD(R0, m::la::length(q - centre));
	if (R0 <= 1e-6)
		return m::Manifold();
	const m::vec3 xv = PerpFrame(nrm);
	const m::vec3 yv = m::la::cross(nrm, xv);
	const double scoop = MaxD(0.05 * h, std::min(depth, 1.2 * h));
	const EVec no(rng.Uniform(0.0, 500.0), rng.Uniform(0.0, 500.0), rng.Uniform(0.0, 500.0));
	const double ph = rng.Uniform(0.0, 2.0 * m::kPi);
	// ripple centre: the impact point, off to one side of the patch
	const double ra = rng.Uniform(0.0, 2.0 * m::kPi);
	const double rx = std::cos(ra) * 0.8 * R0, ry = std::sin(ra) * 0.8 * R0;
	const double Rd = 1.08 * R0;
	auto floorZ = [&](double x, double y) {
		const double r = std::hypot(x, y) / Rd;
		const double dip = r < 1.0 ? scoop * std::pow(1.0 - r * r, 1.3) : -scoop * 1.5 * (r * r - 1.0);
		const m::vec3 w = xv * x + yv * y;
		const EVec q = EVec(w.x, w.y, w.z) / MaxD(0.35 * R0, 1e-6) + no;
		const double n1 = ProjectDamageNoise(q) - 0.5;
		const double n2 = ProjectDamageNoise(q * 2.7 + EVec(5.3, 1.1, 7.7)) - 0.5;
		const double fade = r < 1.3 ? 1.0 : 0.0;
		const double rr = std::hypot(x - rx, y - ry) / R0;
		const double ripple = 0.06 * scoop * std::sin(9.0 * rr + ph) * std::exp(-1.2 * rr);
		const double noise = rough * (0.55 * n1 + 0.3 * n2) * (0.5 * h + 0.5 * scoop) * fade;
		return -(dip + noise + ripple * rough);
	};
	const int n = MaxI(14, PyRound(30.0 * MaxD(0.25, std::min(detail, 4.0))));
	const double top = h + MaxD(MARGIN, 0.5 * size);
	return HeightSlab(xv, yv, nrm, centre, 1.6 * R0, n, top, floorZ);
}

static double DistToStrokes(const m::vec3& q, const std::vector<double>& xyz, const std::vector<Int32>& count)
{
	double best = std::numeric_limits<double>::infinity();
	size_t off = 0;
	for (Int32 n : count)
	{
		for (Int32 k = 0; k < n; k++)
		{
			const size_t i = (off + (size_t)k) * 3;
			if (i + 2 >= xyz.size()) return best;
			const m::vec3 a(xyz[i], xyz[i + 1], xyz[i + 2]);
			if (k + 1 < n)
			{
				const m::vec3 b(xyz[i + 3], xyz[i + 4], xyz[i + 5]);
				const m::vec3 ab = b - a;
				const double den = m::la::dot(ab, ab);
				const double t = den > 0.0 ? std::max(0.0, std::min(1.0, m::la::dot(q - a, ab) / den)) : 0.0;
				best = std::min(best, m::la::length(q - (a + ab * t)));
			}
			else
				best = std::min(best, m::la::length(q - a));
		}
		off += (size_t)n;
	}
	return best;
}

// Bead Draw Zones: 1 near a stroke, smoothly 0 at buildPaintRadius.
static double BeadZone(const m::vec3& q, const ProjectDamageParams& params)
{
	if (!params.buildPaintOnly)
		return 1.0;
	const double R = MaxD(params.buildPaintRadius, 1e-6);
	const double u = std::max(0.0, std::min(1.0, (R - DistToStrokes(q, params.buildPaintPts, params.buildPaintCount)) / (0.35 * R)));
	return u * u * (3.0 - 2.0 * u);
}

std::vector<m::Manifold> PlaceCornerCutters(const std::vector<ExposedEdge>& exposed,
	const EVec* pts, const ProjectDamageParams& params,
	const std::vector<bool>* avoidPoly = nullptr)
{
	std::vector<m::Manifold> cutters;
	if (params.cornerProb <= 0.0 || exposed.empty())
		return cutters;
	if (params.cornerPaintOnly && params.cornerPaintCount.empty())
		return cutters;
	PyRandom rngShape((uint32_t)(params.seed + 2702));
	std::vector<m::Manifold> broken;

	struct CornerInc
	{
		std::vector<const ExposedEdge*> convexEdges;
		bool hasConcave = false;
	};
	std::map<Int32, CornerInc> inc;
	for (const ExposedEdge& e : exposed)
	{
		const bool avoided = EdgeAvoided(e, avoidPoly);
		const Int32 ends[2] = { e.p0, e.p1 };
		for (Int32 p : ends)
		{
			CornerInc& slot = inc[p];
			if (e.convex)
			{
				if (!avoided)
					slot.convexEdges.push_back(&e);
			}
			else
				slot.hasConcave = true;
		}
	}

	PyRandom rng((uint32_t)(params.seed + 2));

	Int32 cornerIdx = 0;
	std::vector<SpallSpec> specs;
	std::vector<bool> keep;

	for (auto& kv : inc)
	{
		const Int32 p = kv.first;
		const CornerInc& slot = kv.second;
		if ((Int32)slot.convexEdges.size() < params.cornerMinEdges || slot.hasConcave)
			continue;

		m::vec3 sum(0.0, 0.0, 0.0);
		for (const ExposedEdge* e : slot.convexEdges)
		{
			const Int32 q = (e->p0 == p) ? e->p1 : e->p0;
			const m::vec3 d = pts[q] - pts[p];
			const double L = m::la::length(d);
			if (L > 0.0)
				sum = sum + d / L;
		}
		const double sl = m::la::length(sum);
		if (sl < 1e-6)
			continue;

		if (rng.Random() >= params.cornerProb)
			continue;
		if (params.cornerPaintOnly
			&& DistToStrokes(pts[p], params.cornerPaintPts, params.cornerPaintCount) > MaxD(params.cornerPaintRadius, 1e-6))
			continue;

		const m::vec3 outward = sum / sl * -1.0;
		const m::vec3 tangent = PerpFrame(outward);

		const double su = rng.Uniform(params.cornerSizeMin, params.cornerSizeMax);
		const double sv = su * SPALL_ASPECT;
		const double depth = rng.Uniform(params.cornerDepthMin, params.cornerDepthMax);

		Bool cornerMasked = false;
		double cms = 1.0;
		if (params.mask != nullptr)
		{
			const double wk = params.mask->WeightIfUsed(pts[p]);
			cornerMasked = !params.mask->KeepW(wk, cornerIdx, params.seed + 2);
			cms = params.mask->SizeScaleW(wk);
			if (cms <= 1e-3)
			{
				cornerMasked = true;
				cms = 1e-3;
			}
		}
		cornerIdx++;

		int satellites = (int)(rng.Random() * 3.0);
		if (params.cornerStyle == 1)
		{
			if (!cornerMasked)
			{
				std::vector<m::vec3> dirs;
				std::vector<double> lens;
				for (const ExposedEdge* e : slot.convexEdges)
				{
					const Int32 q = (e->p0 == p) ? e->p1 : e->p0;
					const m::vec3 d = pts[q] - pts[p];
					const double L = m::la::length(d);
					if (L <= 1e-9) continue;
					dirs.push_back(d / L);
					lens.push_back(L);
				}
				m::Manifold c = BrokenCornerCutter(pts[p], dirs, lens, outward, su * cms, depth * cms,
					Clamp01(params.cornerRough) * 1.5, Clamp01(params.cornerTilt), params.cornerDetail, rngShape);
				if (!c.IsEmpty())
					broken.push_back(std::move(c));
			}
			// flakes along the edges next to the break
			satellites = (int)std::floor(Clamp01(params.cornerFlakes) * 4.0 * rngShape.Random() + 0.5 * params.cornerFlakes);
		}
		else
		{
			int domeSeg = 0, outlineN = 0;
			CutterSegs(params.cornerDetail,
				params.cornerSizeMax > 0.0 ? su / params.cornerSizeMax : 1.0,
				domeSeg, outlineN);
			specs.push_back(SpallPlan(pts[p], tangent, outward, su * cms, sv * cms,
				depth * cms, rng, domeSeg, outlineN, params.cornerRimSoft));
			keep.push_back(!cornerMasked);
		}
		for (int k = 0; k < satellites; k++)
		{
			const ExposedEdge* e = slot.convexEdges[(size_t)(rng.Random() * (double)slot.convexEdges.size()) % slot.convexEdges.size()];
			const Int32 q = (e->p0 == p) ? e->p1 : e->p0;
			m::vec3 along = pts[q] - pts[p];
			const double alen = m::la::length(along);
			if (alen < 1e-9)
				continue;
			along = along / alen;
			const bool brokenStyle = params.cornerStyle == 1;
			const double ss = su * (brokenStyle ? rngShape.Uniform(0.18, 0.4) : rng.Uniform(0.35, 0.6));
			const double dist = brokenStyle ? std::min(alen * 0.45, su * rngShape.Uniform(0.75, 1.5))
				: std::min(alen * 0.45, su * rng.Uniform(0.7, 1.2));
			const m::vec3 sOut = PerpOnto(outward, along);
			int sd = 0, sn = 0;
			CutterSegs(params.cornerDetail, params.cornerSizeMax > 0.0 ? ss / params.cornerSizeMax : 1.0, sd, sn);
			specs.push_back(SpallPlan(pts[p] + along * dist, along, sOut, ss * cms, ss * SPALL_ASPECT * cms,
				depth * (brokenStyle ? rngShape.Uniform(0.15, 0.35) : rng.Uniform(0.4, 0.7)) * cms,
				brokenStyle ? rngShape : rng, sd, sn, params.cornerRimSoft, brokenStyle ? 26.0 : -1.0));
			if (brokenStyle) specs.back().profile = 0.7;
			keep.push_back(!cornerMasked);
		}
	}
	SpallBuildAll(specs, keep, cutters);
	for (m::Manifold& c : broken)
		cutters.push_back(std::move(c));
	return cutters;
}

constexpr double BUILD_STEP = 3.0;
constexpr int    BUILD_SEG  = 18;

constexpr int BUILD_SDF_RES = 12;

struct BuildPlane { m::vec3 n; double off; };

std::vector<BuildPlane> GatherPlanes(const m::vec3& c, double r,
	const ProjectDamagePoly* polys, Int32 vcnt, const EVec* pts,
	const std::vector<EVec>& normals)
{
	std::vector<BuildPlane> out;
	for (Int32 i = 0; i < vcnt; i++)
	{
		const ProjectDamagePoly& cp = polys[i];
		EVec lo = pts[cp.a], hi = pts[cp.a];
		const Int32 idx[3] = { cp.b, cp.c, cp.d };
		for (Int32 v : idx)
		{
			const EVec& q = pts[v];
			lo.x = q.x < lo.x ? q.x : lo.x; lo.y = q.y < lo.y ? q.y : lo.y; lo.z = q.z < lo.z ? q.z : lo.z;
			hi.x = q.x > hi.x ? q.x : hi.x; hi.y = q.y > hi.y ? q.y : hi.y; hi.z = q.z > hi.z ? q.z : hi.z;
		}
		const double dx = c.x < lo.x ? lo.x - c.x : (c.x > hi.x ? c.x - hi.x : 0.0);
		const double dy = c.y < lo.y ? lo.y - c.y : (c.y > hi.y ? c.y - hi.y : 0.0);
		const double dz = c.z < lo.z ? lo.z - c.z : (c.z > hi.z ? c.z - hi.z : 0.0);
		if (dx * dx + dy * dy + dz * dz > r * r)
			continue;
		m::vec3 n = normals[(size_t)i];
		const double L = m::la::length(n);
		if (L < 1e-12)
			continue;
		n = n / L;
		out.push_back({ n, m::la::dot(n, pts[cp.a]) });
	}
	return out;
}

double SminP(double a, double b, double k)
{
	double t = 0.5 + 0.5 * (b - a) / k;
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	return b + (a - b) * t - k * t * (1.0 - t);
}

m::Manifold BlendedBlob(const m::vec3& p0, const m::vec3& nrm,
	double su, double sv, double h, double irreg,
	const EVec& noiseOffset, double spin, double blend, double detail,
	const ProjectDamagePoly* polys, Int32 vcnt, const EVec* pts,
	const std::vector<EVec>& normals)
{
	constexpr int TAB = 256;
	double rr[TAB + 1];
	for (int i = 0; i <= TAB; i++)
	{
		const double a = 2.0 * m::kPi * i / TAB - m::kPi;
		const double ca = std::cos(a), sa = std::sin(a);
		const EVec n1(ca * 1.3 + noiseOffset.x, sa * 1.3 + noiseOffset.y, noiseOffset.z);
		const EVec n2(ca * 3.4 + noiseOffset.z, sa * 3.4 + noiseOffset.x, noiseOffset.y);
		const double r = 1.0
			+ irreg * 0.13 * (ProjectDamageNoise(n1) - 0.5) * 2.0
			+ irreg * 0.05 * (ProjectDamageNoise(n2) - 0.5) * 2.0;
		rr[i] = r > 0.4 ? r : 0.4;
	}

	const double maxExt = MaxD(MaxD(su, sv), h);
	const double k = MaxD(blend * 0.5 * maxExt, 1e-3);
	const double el = maxExt / (BUILD_SDF_RES * detail);
	const double eps = el;
	const double half = maxExt * (1.0 + 0.18 * irreg) + k + 3.0 * el;
	const std::vector<BuildPlane> planes = GatherPlanes(p0, half * 1.7321,
		polys, vcnt, pts, normals);

	const m::vec3 t = PerpFrame(nrm);
	const m::vec3 tS = RotAxis(t, nrm, spin);
	const m::vec3 bS = m::la::cross(nrm, tS);

	auto sdf = [&](m::vec3 w) -> double
	{
		const m::vec3 d = w - p0;
		const m::vec3 q(m::la::dot(d, tS), m::la::dot(d, bS), m::la::dot(d, nrm));
		const m::vec3 u(q.x / su, q.y / sv, q.z / h);
		double r = 1.0;
		if (m::la::length(u) > 1e-12)
		{
			const double az = std::atan2(u.y, u.x);
			const double ft = (az + m::kPi) / (2.0 * m::kPi) * TAB;
			const int i0 = (int)ft < TAB ? (int)ft : TAB - 1;
			const double fr = ft - i0;
			r = rr[i0] * (1.0 - fr) + rr[i0 + 1] * fr;
		}
		const m::vec3 rad(r * su, r * sv, h);
		const m::vec3 p1(q.x / rad.x, q.y / rad.y, q.z / rad.z);
		const m::vec3 p2(q.x / (rad.x * rad.x), q.y / (rad.y * rad.y), q.z / (rad.z * rad.z));
		const double k0 = m::la::length(p1);
		const double k1 = m::la::length(p2);
		const double minRad = rad.x < rad.y ? (rad.x < rad.z ? rad.x : rad.z) : (rad.y < rad.z ? rad.y : rad.z);
		const double blob = k1 > 1e-12 ? k0 * (k0 - 1.0) / k1 : -minRad;
		double base = 1e30;
		for (const BuildPlane& pl : planes)
		{
			const double dp = m::la::dot(pl.n, w) - pl.off;
			base = dp < base ? dp : base;
		}
		base += eps;
		const double v = SminP(blob, base, k);
		const double clip = blob - 2.0 * k;
		return -(v > clip ? v : clip);
	};
	const m::vec3 hv(half, half, half);
	m::Manifold solid = m::Manifold::LevelSet(sdf, m::Box(p0 - hv, p0 + hv), el);
	return solid.Simplify(0.25 * el);
}

void BuildupBead(const Chain& ch, const EVec* pts,
	const std::vector<EVec>& normals, const ProjectDamageParams& params,
	PyRandom& rng, std::vector<m::Manifold>& out)
{
	const double ph0 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph1 = rng.Uniform(0.0, 2.0 * m::kPi);

	const std::vector<Int32>& P = ch.points;
	const size_t n = P.size();
	if (n < 2 || ch.edges.empty())
		return;
	const bool loop = P.front() == P.back();

	std::vector<m::vec3> pos(n), bis(n);
	for (size_t i = 0; i < n; i++)
	{
		pos[i] = pts[P[i]];
		const ExposedEdge* ePrev = i > 0 ? ch.edges[i - 1]
			: (loop ? ch.edges.back() : ch.edges.front());
		const ExposedEdge* eNext = i < n - 1 ? ch.edges[i]
			: (loop ? ch.edges.front() : ch.edges.back());
		m::vec3 b = EdgeOutNormal(*ePrev, normals) + EdgeOutNormal(*eNext, normals);
		const double L = m::la::length(b);
		bis[i] = b / (L != 0.0 ? L : 1.0);
	}

	const double step = BUILD_STEP / params.buildDetail;
	std::vector<m::vec3> sp, sb;
	for (size_t i = 0; i + 1 < n; i++)
	{
		const double L = m::la::length(pos[i + 1] - pos[i]);
		const int steps = MaxI(1, (int)std::ceil(L / step));
		for (int k = 0; k < steps; k++)
		{
			const double t = (double)k / steps;
			sp.push_back(pos[i] + (pos[i + 1] - pos[i]) * t);
			m::vec3 b = bis[i] * (1.0 - t) + bis[i + 1] * t;
			const double bl = m::la::length(b);
			sb.push_back(b / (bl != 0.0 ? bl : 1.0));
		}
	}
	sp.push_back(pos.back());
	sb.push_back(bis.back());
	if (sp.size() < 2)
		return;

	double total = 0.0;
	for (size_t i = 0; i + 1 < sp.size(); i++)
		total += m::la::length(sp[i + 1] - sp[i]);
	if (total <= 0.0)
		return;

	const double taperLen = MaxD(2.0 * BUILD_STEP, 0.15 * total);

	const int fan = MaxI(3, (int)std::lround(6.0 * params.buildDetail));
	std::vector<std::vector<m::vec3>> secs;
	secs.reserve(sp.size());
	std::vector<double> zone(sp.size(), 1.0);
	double arc = 0.0;
	for (size_t i = 0; i < sp.size(); i++)
	{
		if (i > 0)
			arc += m::la::length(sp[i] - sp[i - 1]);
		const m::vec3& a = i > 0 ? sp[i - 1] : (loop ? sp[sp.size() - 2] : sp[0]);
		const m::vec3& b2 = i + 1 < sp.size() ? sp[i + 1] : (loop ? sp[1] : sp[sp.size() - 1]);
		m::vec3 t = b2 - a;
		const double tl = m::la::length(t);
		t = t / (tl != 0.0 ? tl : 1.0);
		const m::vec3& nb = sb[i];
		m::vec3 e = m::la::cross(nb, t);
		const double el = m::la::length(e);
		e = e / (el != 0.0 ? el : 1.0);
		const double mod = 1.0 + params.buildIrreg
			* (0.25 * std::sin(2.0 * m::kPi * 2.0 * arc / total + ph0)
			 + 0.15 * std::sin(2.0 * m::kPi * 5.0 * arc / total + ph1));
		double w = MaxD(params.buildWidth * 0.5 * mod, 1e-3);
		double h = MaxD(params.buildHeight * mod, 1e-3);
		if (params.mask != nullptr)
		{
			const double mf = params.mask->FadeScale(sp[i]);
			w = MaxD(w * mf, 1e-3);
			h = MaxD(h * mf, 1e-3);
		}
		if (!loop)
		{
			const double fadeIn = arc / taperLen;
			const double fadeOut = (total - arc) / taperLen;
			double fade = fadeIn < fadeOut ? fadeIn : fadeOut;
			if (fade > 1.0) fade = 1.0;
			if (fade < 0.04) fade = 0.04;
			w *= fade;
			h *= fade;
		}
		zone[i] = BeadZone(sp[i], params);
		if (zone[i] < 1.0)
		{
			w = MaxD(w * MaxD(zone[i], 0.04), 1e-3);
			h = MaxD(h * MaxD(zone[i], 0.04), 1e-3);
		}
		std::vector<m::vec3> sec((size_t)fan + 2);
		for (int k = 0; k <= fan; k++)
		{
			const double ang = m::kPi * k / fan;
			sec[(size_t)k] = sp[i] + e * (w * std::cos(ang)) + nb * (h * std::sin(ang));
		}
		sec[(size_t)fan + 1] = sp[i] - nb * (0.35 * h);
		secs.push_back(std::move(sec));
	}
	for (size_t i = 0; i + 1 < secs.size(); i++)
	{
		if (zone[i] <= 0.0 && zone[i + 1] <= 0.0)
			continue;
		std::vector<m::vec3> hull;
		hull.reserve(2 * ((size_t)fan + 2));
		for (int k = 0; k < fan + 2; k++) hull.push_back(secs[i][(size_t)k]);
		for (int k = 0; k < fan + 2; k++) hull.push_back(secs[i + 1][(size_t)k]);
		out.push_back(m::Manifold::Hull(hull));
	}
}

// The two face normals of an exposed edge (its first two distinct faces).
static bool EdgeFaceNormals(const ExposedEdge& e, const std::vector<EVec>& normals, m::vec3& n1, m::vec3& n2)
{
	Int32 f1 = -1, f2 = -1;
	for (auto& r : e.reprs)
	{
		if (f1 < 0) f1 = r.first;
		else if (r.first != f1) { f2 = r.first; break; }
	}
	if (f1 < 0 || f2 < 0) return false;
	n1 = normals[(size_t)f1];
	n2 = normals[(size_t)f2];
	return m::la::length(n1) > 1e-9 && m::la::length(n2) > 1e-9;
}

// Closed prism between two cross-sections of equal point count (any simple
// polygon): two fan caps from point 0 and the side quads. Outward-facing.
static m::Manifold SectionPrism(const std::vector<m::vec3>& a, const std::vector<m::vec3>& b)
{
	const size_t n = a.size();
	if (n < 3 || b.size() != n) return m::Manifold();
	m::MeshGL64 mesh;
	mesh.numProp = 3;
	for (const auto* sec : { &a, &b })
		for (const m::vec3& v : *sec)
		{
			mesh.vertProperties.push_back(v.x); mesh.vertProperties.push_back(v.y); mesh.vertProperties.push_back(v.z);
		}
	auto& tv = mesh.triVerts;
	for (uint64_t k = 0; k < n; k++)
	{
		const uint64_t k2 = (k + 1) % n;
		tv.insert(tv.end(), { k, n + k, n + k2 });
		tv.insert(tv.end(), { k, n + k2, k2 });
	}
	for (uint64_t k = 1; k + 1 < n; k++)
	{
		tv.insert(tv.end(), { 0, k + 1, k });
		tv.insert(tv.end(), { n, n + k, n + k + 1 });
	}
	m::Manifold out(mesh);
	if (out.Status() != m::Manifold::Error::NoError) return m::Manifold();
	if (out.Volume() < 0.0)
	{
		for (size_t t = 0; t < tv.size(); t += 3) std::swap(tv[t + 1], tv[t + 2]);
		out = m::Manifold(mesh);
		if (out.Status() != m::Manifold::Error::NoError) return m::Manifold();
	}
	return out;
}

// Fillet bead: buildup (dirt, grime, mortar) filling an inside corner as a
// cove. In each cross-section it runs out on face 1 at distance wa and on
// face 2 at wb from the edge, its free surface a quadratic curve whose middle
// stands h off the corner line, embedded a little into both faces.
void BuildupFillet(const Chain& ch, const EVec* pts,
	const std::vector<EVec>& normals, const ProjectDamageParams& params,
	PyRandom& rng, std::vector<m::Manifold>& out)
{
	const std::vector<Int32>& P = ch.points;
	if (P.size() < 2 || ch.edges.empty()) return;
	const bool loop = P.front() == P.back();
	const double ph0 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph1 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph2 = rng.Uniform(0.0, 2.0 * m::kPi);
	const EVec no(rng.Uniform(0.0, 500.0), rng.Uniform(0.0, 500.0), rng.Uniform(0.0, 500.0));
	double total = 0.0;
	for (size_t i = 0; i + 1 < P.size(); i++) total += m::la::length(pts[P[i + 1]] - pts[P[i]]);
	if (total <= 0.0) return;
	const double step = MaxD(BUILD_STEP / params.buildDetail, 0.35 * params.buildWidth / params.buildDetail);
	const double taperLen = MaxD(2.0 * step, 0.15 * total);
	const int K = MaxI(4, (int)std::lround(7.0 * params.buildDetail));
	const double lumps = Clamp01(params.buildLumps);
	double arc0 = 0.0;
	for (size_t i = 0; i + 1 < P.size(); i++)
	{
		const m::vec3 pa = pts[P[i]], pb = pts[P[i + 1]];
		const double L = m::la::length(pb - pa);
		if (L <= 1e-9) continue;
		const m::vec3 t = (pb - pa) / L;
		const ExposedEdge* e = ch.edges[i];
		m::vec3 n1, n2;
		if (e == nullptr || e->convex || !EdgeFaceNormals(*e, normals, n1, n2)) { arc0 += L; continue; }
		m::vec3 d1 = n2 - n1 * m::la::dot(n1, n2) - t * m::la::dot(t, n2);
		m::vec3 d2 = n1 - n2 * m::la::dot(n1, n2) - t * m::la::dot(t, n1);
		if (m::la::length(d1) < 1e-6 || m::la::length(d2) < 1e-6) { arc0 += L; continue; }
		d1 = m::la::normalize(d1); d2 = m::la::normalize(d2);
		const m::vec3 bis = m::la::normalize(d1 + d2);
		const int steps = MaxI(1, (int)std::ceil(L / step));
		// overlap neighbouring edges a little so turns close up
		const double ext = std::min(0.25 * params.buildWidth, 0.3 * L);
		std::vector<std::vector<m::vec3>> secs;
		std::vector<double> zone;
		for (int k = 0; k <= steps; k++)
		{
			double lt = (double)k / steps;
			double sAlong = lt * L;
			if (k == 0) sAlong -= ext;
			if (k == steps) sAlong += ext;
			const m::vec3 c = pa + t * sAlong;
			const double arc = arc0 + std::max(0.0, std::min(L, sAlong));
			const double mod = 1.0 + params.buildIrreg
				* (0.25 * std::sin(2.0 * m::kPi * 2.0 * arc / total + ph0)
				 + 0.15 * std::sin(2.0 * m::kPi * 5.0 * arc / total + ph1));
			const double skew = 1.0 + 0.35 * params.buildIrreg * std::sin(2.0 * m::kPi * 3.0 * arc / total + ph2) / 2.33;
			double w = MaxD(params.buildWidth * MaxD(mod, 0.15), 1e-3);
			double h = MaxD(params.buildHeight * MaxD(mod, 0.15), 1e-3);
			if (params.mask != nullptr)
			{
				const double mf = params.mask->FadeScale(c);
				w = MaxD(w * mf, 1e-3);
				h = MaxD(h * mf, 1e-3);
			}
			if (!loop)
			{
				double fade = std::min(arc / taperLen, (total - arc) / taperLen);
				fade = std::max(0.04, std::min(1.0, fade));
				w *= fade; h *= fade;
			}
			zone.push_back(BeadZone(c, params));
			if (zone.back() < 1.0)
			{
				w = MaxD(w * MaxD(zone.back(), 0.04), 1e-3);
				h = MaxD(h * MaxD(zone.back(), 0.04), 1e-3);
			}
			const double wa = w * std::max(0.3, skew), wb = w * std::max(0.3, 2.0 - skew);
			const double emb = 0.12 * w + 0.02;
			const m::vec3 A = c + d1 * wa, B = c + d2 * wb;
			const m::vec3 M = c + bis * std::min(h, 0.9 * std::min(wa, wb));
			const m::vec3 C = M * 2.0 - (A + B) * 0.5;
			std::vector<m::vec3> sec;
			sec.push_back(c - bis * emb);
			sec.push_back(A - n1 * emb);
			for (int j = 0; j <= K; j++)
			{
				const double u = (double)j / K;
				m::vec3 q = A * ((1 - u) * (1 - u)) + C * (2 * u * (1 - u)) + B * (u * u);
				if (lumps > 0.0 && j > 0 && j < K)
				{
					const m::vec3 dq = (C - A) * (2 * (1 - u)) + (B - C) * (2 * u);
					m::vec3 nn = m::la::cross(dq, t);
					if (m::la::dot(nn, bis) < 0.0) nn = -nn;
					const double ln = m::la::length(nn);
					if (ln > 1e-12)
					{
						const EVec nq = EVec(q.x, q.y, q.z) / MaxD(0.6 * params.buildWidth, 1e-6) + no;
						const double bump = (ProjectDamageNoise(nq) - 0.45) * 0.5 + (ProjectDamageNoise(nq * 2.6) - 0.5) * 0.25;
						q = q + nn / ln * (lumps * bump * MaxD(h, 0.25 * w) * 4.0 * u * (1 - u));
					}
				}
				sec.push_back(q);
			}
			sec.push_back(B - n2 * emb);
			secs.push_back(std::move(sec));
		}
		for (size_t k = 0; k + 1 < secs.size(); k++)
		{
			if (zone[k] <= 0.0 && zone[k + 1] <= 0.0)
				continue;
			m::Manifold pr = SectionPrism(secs[k], secs[k + 1]);
			if (pr.IsEmpty())
			{
				std::vector<m::vec3> hull(secs[k].begin(), secs[k].end());
				hull.insert(hull.end(), secs[k + 1].begin(), secs[k + 1].end());
				pr = m::Manifold::Hull(hull);
			}
			if (!pr.IsEmpty()) out.push_back(std::move(pr));
		}
		arc0 += L;
	}
}

void PlaceBuildupCorners(const std::vector<const Chain*>& concave,
	const std::vector<Chain>& all, const EVec* pts,
	const ProjectDamagePoly* polys, Int32 vcnt,
	const std::vector<EVec>& normals, const ProjectDamageParams& params,
	std::vector<m::Manifold>& out)
{
	struct Junc { int count = 0; bool convexTouched = false;
		m::vec3 dir = m::vec3(0.0, 0.0, 0.0); std::vector<m::vec3> along; };
	std::map<Int32, Junc> junc;
	for (const Chain* c : concave)
	{
		if (c->points.front() == c->points.back())
			continue;
		Junc& a = junc[c->points.front()];
		a.count++; a.dir = a.dir + EdgeOutNormal(*c->edges.front(), normals);
		a.along.push_back(pts[c->points[1]] - pts[c->points.front()]);
		Junc& b = junc[c->points.back()];
		b.count++; b.dir = b.dir + EdgeOutNormal(*c->edges.back(), normals);
		b.along.push_back(pts[c->points[c->points.size() - 2]] - pts[c->points.back()]);
	}
	for (const Chain& c : all)
	{
		for (const ExposedEdge* e : c.edges)
		{
			if (!e->convex)
				continue;
			auto it = junc.find(e->p0);
			if (it != junc.end()) it->second.convexTouched = true;
			it = junc.find(e->p1);
			if (it != junc.end()) it->second.convexTouched = true;
		}
	}
	PyRandom rng((uint32_t)(params.seed + 8002));
	PyRandom rngLump((uint32_t)(params.seed + 8102));
	Int32 pileIdx = 0;
	const double pDetail = params.pileDetail > 0.0 ? params.pileDetail : params.buildDetail;
	const int seg = MaxI(6, (int)std::lround(BUILD_SEG * pDetail));
	const m::MeshGL unit = m::Manifold::Sphere(1.0, seg).GetMeshGL();
	for (auto& kv : junc)
	{
		if (kv.second.count < 2 || kv.second.convexTouched)
			continue;
		const double dl = m::la::length(kv.second.dir);
		if (dl < 1e-6)
			continue;
		if (rng.Random() >= params.buildCornerProb)
			continue;
		const m::vec3 dir = kv.second.dir / dl;
		const double su = params.buildCornerSize * rng.Uniform(0.8, 1.2);
		const double sv = params.buildCornerSize * rng.Uniform(0.8, 1.2);
		const double h = params.buildCornerSize * MaxD(0.1, params.pileHeight);
		const EVec noiseOffset(rng.Uniform(0.0, 1000.0),
			rng.Uniform(0.0, 1000.0), rng.Uniform(0.0, 1000.0));
		const double spin = rng.Uniform(0.0, 2.0 * m::kPi);

		double pms = 1.0;
		if (params.mask != nullptr)
		{
			const double wp = params.mask->WeightIfUsed(pts[kv.first]);
			const Bool keepPile = params.mask->KeepW(wp, pileIdx, params.seed + 8002);
			pms = params.mask->SizeScaleW(wp);
			pileIdx++;
			if (!keepPile || pms <= 1e-3)
				continue;
		}

		auto heap = [&](const m::vec3& at, double a, double b, double hh, const EVec& no, double sp) {
			if (params.buildBlend > 0.0)
				out.push_back(BlendedBlob(at - dir * (0.3 * hh), dir,
					MaxD(a, 1e-3), MaxD(b, 1e-3), MaxD(hh, 1e-3),
					params.buildIrreg, no, sp, params.buildBlend,
					pDetail, polys, vcnt, pts, normals));
			else
				out.push_back(OrganicCutter(unit, at - dir * (0.3 * hh), dir,
					MaxD(a, 1e-3), MaxD(b, 1e-3), MaxD(hh, 1e-3),
					params.buildIrreg, 0.0, no, sp));
		};
		heap(pts[kv.first], su * pms, sv * pms, h * pms, noiseOffset, spin);
		// smaller heaps spilling along the inside edges and around the main one
		const int nLumps = (int)std::floor(Clamp01(params.pileLumps) * 4.0 * rngLump.Uniform(0.6, 1.0) + 0.5);
		for (int k = 0; k < nLumps && !kv.second.along.empty(); k++)
		{
			m::vec3 along = kv.second.along[(size_t)(rngLump.Random() * kv.second.along.size()) % kv.second.along.size()];
			const double al = m::la::length(along);
			if (al < 1e-9) continue;
			along = along / al;
			const double f = rngLump.Uniform(0.35, 0.65);
			const double d = std::min(0.45 * al, params.buildCornerSize * rngLump.Uniform(0.45, 0.95));
			const EVec no2(rngLump.Uniform(0.0, 1000.0), rngLump.Uniform(0.0, 1000.0), rngLump.Uniform(0.0, 1000.0));
			heap(pts[kv.first] + along * d, su * f * pms, sv * f * pms, h * f * rngLump.Uniform(0.7, 1.0) * pms, no2,
				rngLump.Uniform(0.0, 2.0 * m::kPi));
		}
	}
}

std::vector<m::Manifold> PlaceBuildup(const std::vector<Chain>& chains,
	const EVec* pts, const ProjectDamagePoly* polys, Int32 vcnt,
	const std::vector<EVec>& normals, const ProjectDamageParams& params,
	bool wantBead = true, bool wantPiles = true)
{
	std::vector<m::Manifold> out;
	const Bool beadOn = wantBead && params.buildCoverage > 0.0
		&& !(params.buildPaintOnly && params.buildPaintCount.empty());
	const Bool cornerOn = wantPiles && params.buildCornerProb > 0.0;
	if (!beadOn && !cornerOn)
		return out;

	std::vector<const Chain*> concave;
	for (const Chain& c : chains)
	{
		int cc = 0;
		for (ExposedEdge* e : c.edges)
			if (e->convex) cc++;
		if (!((double)cc > (double)c.edges.size() / 2.0))
			concave.push_back(&c);
	}

	if (beadOn)
	{
		std::vector<const Chain*> covered;
		PyRandom rng((uint32_t)(params.seed + 8000));
		for (const Chain* c : concave)
		{
			if (rng.Random() < params.buildCoverage)
				covered.push_back(c);
		}
		for (const Chain* c : covered)
		{
			if (params.buildShape == 1)
				BuildupFillet(*c, pts, normals, params, rng, out);
			else
				BuildupBead(*c, pts, normals, params, rng, out);
		}
	}
	if (cornerOn)
		PlaceBuildupCorners(concave, chains, pts, polys, vcnt, normals, params, out);
	return out;
}

constexpr int kSecondaryClasses = 4;
constexpr int kBreakRimSlot     = 3;

inline int RimSlotOf(Int32 cls)
{
	if (cls >= 0 && cls <= (Int32)DamageClass::Surface)
		return (int)cls;
	if (cls == (Int32)DamageClass::Break)
		return kBreakRimSlot;
	return -1;
}

struct SecondarySpec
{
	Int32 amount;
	Float sizeMin, sizeMax, depthMin, depthMax;
	Float sizeF, depthF;
	Int32 seed;
	Float rimSoft;
};

static void PlaceBreakFaceCraters(const MeshView& midView, const std::vector<EVec>& mnormals,
	const std::vector<ExposedEdge>& midExposed, const std::vector<Bool>& breakFace,
	const ProjectDamageParams& params, std::unordered_map<Int32, Int32>& idToClass,
	std::vector<m::Manifold>& secCutters)
{
	const Int32 midTris = midView.npolys;
	const EVec* mpts = midView.pts;
	const ProjectDamagePoly* mpolys = midView.polys;
	std::vector<Bool> notBreak((size_t)midTris, true);
	double area = 0.0, breakArea = 0.0;
	for (Int32 i = 0; i < midTris; i++)
	{
		const ProjectDamagePoly& cp = mpolys[i];
		const double a = m::la::length(m::la::cross(mpts[cp.b] - mpts[cp.a], mpts[cp.c] - mpts[cp.a])) * 0.5;
		area += a;
		if (breakFace[(size_t)i])
		{
			notBreak[(size_t)i] = false;
			breakArea += a;
		}
	}
	ProjectDamageParams bp = params;
	bp.surfAmount = area > 0.0 ? PyRound(params.surfAmount * breakArea / area) : 0;
	std::vector<m::Manifold> bc = PlaceSurfaceCutters(midView, mnormals, midExposed,
		bp, &notBreak);
	for (m::Manifold& one : bc)
	{
		one = one.AsOriginal();
		idToClass[(Int32)one.OriginalID()] = (Int32)DamageClass::Surface;
		secCutters.push_back(std::move(one));
	}
}

// Cutter ids of the breakers with Smooth Slope in the current build: their
// rim gets no break-rim chips (the chips would cut the steep edge back in).
// Filled by ResolveBreakSolids, read by RunSecondaryPass, same thread.


// Cutter ids of the breakers with Smooth Slope in the current build: their
// rim gets no break-rim chips (the chips would cut the steep edge back in).
// Filled by ResolveBreakSolids, read by RunSecondaryPass, same thread.
static thread_local std::unordered_set<Int32> gSmoothBreakIds;
// Wall Hole frames (rim bricks, rubble) of the current build, handed to the
// result with the rebar output (same thread).
static thread_local WallHoleOut gWallHoleOut;

m::Manifold RunSecondaryPass(m::Manifold result, Int32 baseId,
	std::unordered_map<Int32, Int32>& idToClass, const ProjectDamageParams& params,
	const ProjectDamageHost& host, Bool breakPhase, std::unordered_map<Int32, Int32>& effectOwners)
{
	const Bool hasBreak = !params.breakers.empty();
	const SecondarySpec spec[kSecondaryClasses] = {
		{ breakPhase ? 0 : params.secAmount, params.sizeMin, params.sizeMax,
		  params.depthMin, params.depthMax,
		  params.secSize, params.secDepth, params.seed + params.chipSeed + 1, params.chipRimSoft },
		{ breakPhase ? 0 : params.cornerSecAmount, params.cornerSizeMin, params.cornerSizeMax,
		  params.cornerDepthMin, params.cornerDepthMax,
		  params.cornerSecSize, params.cornerSecDepth, params.seed + 6000, params.cornerRimSoft },
		{ breakPhase || params.surfPaintOnly ? 0 : params.surfSecAmount, params.surfSizeMin, params.surfSizeMax,
		  params.surfDepthMin, params.surfDepthMax,
		  params.surfSecSize, params.surfSecDepth, params.seed + 6001, params.chipRimSoft },
		{ breakPhase && hasBreak ? params.breakRimAmount : 0, params.sizeMin, params.sizeMax,
		  params.depthMin, params.depthMax,
		  params.breakRimSize, params.breakRimDepth, params.seed + 9000, params.chipRimSoft },
	};
	const Bool wantBreakCraters = breakPhase && hasBreak && params.breakSurfDmg
		&& params.surfAmount > 0;
	if (spec[0].amount <= 0 && spec[1].amount <= 0 && spec[2].amount <= 0
		&& spec[3].amount <= 0 && !wantBreakCraters)
		return result;
	if (result.NumTri() == 0)
		return result;

	const m::MeshGL mid = result.GetMeshGL();

	const Int32 midTris = (Int32)(mid.triVerts.size() / 3);
	std::vector<bool> isChip((size_t)midTris, false);
	std::vector<Int32> triClass((size_t)midTris, NOTOK);
	std::vector<char> smoothBreakTri((size_t)midTris, 0);
	const Int32 nMidRun = (Int32)mid.runOriginalID.size();
	if ((Int32)mid.runIndex.size() >= nMidRun + 1)
	{
		for (Int32 r = 0; r < nMidRun; r++)
		{
			if ((Int32)mid.runOriginalID[r] != baseId)
			{
				const Int32 t0 = (Int32)(mid.runIndex[r] / 3);
				const Int32 t1 = (Int32)(mid.runIndex[r + 1] / 3);
				auto itc = idToClass.find((Int32)mid.runOriginalID[r]);
				const Int32 cls = itc != idToClass.end() ? itc->second : NOTOK;
				const bool smooth = gSmoothBreakIds.count((Int32)mid.runOriginalID[r]) != 0;
				for (Int32 t = t0; t < t1 && t < midTris; t++)
				{
					isChip[(size_t)t] = true;
					triClass[(size_t)t] = cls;
					if (smooth) smoothBreakTri[(size_t)t] = 1;
				}
			}
		}
	}

	std::vector<Bool> breakFace;
	if (hasBreak && breakPhase)
	{
		breakFace.assign((size_t)midTris, false);
		for (Int32 t = 0; t < midTris; t++)
			breakFace[(size_t)t] = triClass[(size_t)t] == (Int32)DamageClass::Break;
	}
	const Bool rimOnly = breakPhase && !breakFace.empty() && !wantBreakCraters;

	const Int32 mv = (Int32)mid.NumVert();
	const Int32 np = (Int32)mid.numProp;
	std::vector<Int32> slotOf;
	const Int32 welded = BuildWeldSlots(mid, slotOf);
	const Int32 nv = welded > 0 ? welded : mv;
	auto weldSlot = [&](Int32 v) { return slotOf.empty() ? v : slotOf[(size_t)v]; };
	std::vector<EVec> midPtBuf((size_t)nv, EVec(0.0));
	for (Int32 i = 0; i < mv; i++)
	{
		const Int32 s = weldSlot(i);
		if (s >= 0 && s < nv)
			midPtBuf[(size_t)s] = EVec(mid.vertProperties[(size_t)i * np + 0],
										 mid.vertProperties[(size_t)i * np + 1],
										 mid.vertProperties[(size_t)i * np + 2]);
	}
	std::vector<ProjectDamagePoly> midTriBuf((size_t)midTris);
	for (Int32 i = 0; i < midTris; i++)
		midTriBuf[(size_t)i] = ProjectDamagePoly{ weldSlot((Int32)mid.triVerts[(size_t)i * 3 + 0]),
										weldSlot((Int32)mid.triVerts[(size_t)i * 3 + 1]),
										weldSlot((Int32)mid.triVerts[(size_t)i * 3 + 2]),
										weldSlot((Int32)mid.triVerts[(size_t)i * 3 + 2]) };
	const EVec* mpts = midPtBuf.data();
	const ProjectDamagePoly* mpolys = midTriBuf.data();
	const MeshView midView{ mpts, nv, mpolys, midTris };
	const std::vector<EVec> mnormals = PolyNormals(mpts, mpolys, midTris);

	std::vector<ExposedEdge> midExposed;
	if (rimOnly)
		DetectBreakRim(mpolys, midTris, (Int32)midPtBuf.size(), breakFace, mpts,
			mnormals, params.thresholdDeg, midExposed);
	else
		DetectExposed(midView, mnormals, params.thresholdDeg, midExposed, host,
			breakFace.empty() ? nullptr : &breakFace);

	std::vector<ExposedEdge> rim[kSecondaryClasses];
	for (const ExposedEdge& e : midExposed)
	{
		Bool onChip = false, onBase = false, onSmooth = false;
		Int32 cls = NOTOK;
		for (const auto& r : e.reprs)
		{
			if (r.first >= 0 && r.first < midTris)
			{
				if (smoothBreakTri[(size_t)r.first]) onSmooth = true;
				if (isChip[(size_t)r.first])
				{
					onChip = true;
					if (cls == NOTOK)
						cls = triClass[(size_t)r.first];
				}
				else
					onBase = true;
			}
		}
		const int slot = RimSlotOf(cls);
		if (onSmooth && slot == kBreakRimSlot) continue;   // Smooth Slope: no rim chips
		if (onChip && onBase && e.convex
			&& slot >= 0 && spec[slot].amount > 0)
			rim[slot].push_back(e);
	}

	std::vector<m::Manifold> secCutters;
	for (int cls = 0; cls < kSecondaryClasses; cls++)
	{
		if (spec[cls].amount <= 0 || rim[cls].empty())
			continue;
		std::vector<Chain> rimChains;
		GroupObjectEdges(rim[cls], mpts, 180.0, rimChains);

		ChipPlacement cp;
		cp.amount   = spec[cls].amount;
		cp.sizeMin  = spec[cls].sizeMin * spec[cls].sizeF;
		cp.sizeMax  = spec[cls].sizeMax * spec[cls].sizeF;
		cp.sizeRef  = params.sizeMax;
		cp.depthMin = spec[cls].depthMin * spec[cls].depthF;
		cp.depthMax = spec[cls].depthMax * spec[cls].depthF;
		cp.offset       = params.offset;
		cp.randomOffset = params.randomOffset;
		cp.detail       = params.detail;
		cp.seed         = spec[cls].seed;
		cp.rimSoft      = spec[cls].rimSoft;
		cp.stretch = 1.0;
		cp.amountPerChain = cls != kBreakRimSlot;
		cp.loopRef = m::kPi * (spec[cls].sizeMin + spec[cls].sizeMax) * 0.9;

		std::vector<m::Manifold> c = PlaceCutters(rimChains, mpts, mnormals, cp, nullptr, 0.0);
		for (m::Manifold& one : c) {
            // 31 is the Surface Additional Damage region; keep its chip class.
            if (cls == 2) {
                one=one.AsOriginal();
                effectOwners[(Int32)one.OriginalID()]=31;
            }
            secCutters.push_back(std::move(one));
        }
	}

	if (wantBreakCraters)
		PlaceBreakFaceCraters(midView, mnormals, midExposed, breakFace, params, idToClass, secCutters);

	// Texture breakers: the rim chips and break-face craters cut into the
	// textured break surface too - they get the nearest texture breaker's
	// texture, so the gravel runs on into them.
	if (breakPhase && !secCutters.empty())
	{
		std::vector<const BreakerSpec*> texB;
		for (const BreakerSpec& b : params.breakers)
			if (b.surfMode == 1 && b.texStrength > 0.0 && b.strength > 0.0) texB.push_back(&b);
		if (!texB.empty())
		{
			HostStatus(host, "texturing " + std::to_string(secCutters.size()) + " break chips", 72);
			std::shared_ptr<ProjectDamageBVH> midBvh;
			const auto inside = MakeInsideTest(midView, midBvh);   // the mesh the chips cut into
			std::vector<m::Manifold> done(secCutters.size());
			ProjectDamageParallelFor(0, (int64_t)secCutters.size(), [&](int64_t i)
			{
				const m::Manifold& cu = secCutters[(size_t)i];
				const m::Box bb = cu.BoundingBox();
				const EVec c = (bb.min + bb.max) * 0.5;
				const BreakerSpec* best = texB[0];
				double bd = 1e300;
				for (const BreakerSpec* b : texB)
				{
					const double d = m::la::length(c - EVec(b->rel[3]));
					if (d < bd) { bd = d; best = b; }
				}
				done[(size_t)i] = TextureDisplaceSolid(cu, TexSpecOf(*best, best->strength), nullptr, &inside);
			});
			for (size_t i = 0; i < secCutters.size(); ++i)
			{
				if (done[i].IsEmpty()) continue;
				// keep the chip's damage class under its new original id
				const Int32 oldId = (Int32)secCutters[i].OriginalID();
				m::Manifold d = done[i].AsOriginal();
				auto it = idToClass.find(oldId);
				if (it != idToClass.end()) idToClass[(Int32)d.OriginalID()] = it->second;
				secCutters[i] = std::move(d);
			}
		}
	}

	ProjectDamageParallelFor(0, (int64_t)secCutters.size(), [&secCutters](int64_t i)
	{
		secCutters[(size_t)i].NumTri();
	});
	for (const m::Manifold& cu : secCutters)
		result = result - cu;

	return result;
}

constexpr Float COPLANAR_COS = 0.99996192;

struct TopoBuild
{
	std::vector<ProjectDamagePoly> polys;
	std::vector<Int32>    chipPolys;
	std::vector<Int32>    crackPolys[3];
	std::vector<Int32>    holePolys;
    std::vector<Int32> wallpaperPolys[3];
    std::vector<Int32> wallpaperBasePolys;
    std::vector<Int32> wallpaperFlapPolys;  // hanging flaps only (also in wallpaperPolys[0/1])
    std::vector<Int32> brickPolys[5];
    std::vector<Int32> extraPolys[4];
    std::vector<Int32> effectIds;
	std::vector<Int32>    chipTris;
	std::vector<Int32>    buildPolys;
	std::vector<Int32>    breakPolys;
	std::vector<Int32>    propCorner;
	std::vector<Int32>    geoOf;
	Int32                 weldedCount = 0;
	std::vector<Int32>    basePolyOf;
};

Bool ConvexQuad(const EVec v[4], const EVec& n)
{
	for (int i = 0; i < 4; i++)
	{
		const EVec e0 = v[(i + 1) & 3] - v[i];
		const EVec e1 = v[(i + 2) & 3] - v[(i + 1) & 3];
		if (m::la::dot(m::la::cross(e0, e1), n) < 0.0)
			return false;
	}
	return true;
}

struct TopoPairing
{
	const m::MeshGL& mesh;
	Int32 nt;
	Int32 nv;
	Bool haveUV;
	const std::vector<EVec>& pos;
	std::vector<ProjectDamagePoly>& tris;
	std::vector<ProjectDamagePoly>& triProps;
	const std::vector<Bool>& isChip;
    const std::vector<Int32>& effects;
	std::vector<Int32>& mate;
	std::vector<Bool>& used;
};

static EVec Pos(const TopoPairing& P, Int32 slot)
{
	return P.pos[(size_t)slot];
}

static Int32 PropAt(const TopoPairing& P, Int32 t, Int32 v)
{
	const ProjectDamagePoly& c = P.tris[(size_t)t];
	const ProjectDamagePoly& p = P.triProps[(size_t)t];
	if (c.a == v) return p.a;
	if (c.b == v) return p.b;
	return p.c;
}

static void PairByFaceID(TopoPairing& P, const ProjectDamagePoly* inPolys, Int32 inCnt)
{
	std::vector<Int32> cnt((size_t)inCnt, 0);
	std::vector<Int32> tri0((size_t)inCnt, NOTOK);
	std::vector<Int32> tri1((size_t)inCnt, NOTOK);
	for (Int32 t = 0; t < P.nt; t++)
	{
		if (P.isChip[(size_t)t])
			continue;
		const Int32 f = (Int32)P.mesh.faceID[(size_t)t];
		if (f < 0 || f >= inCnt)
			continue;
		const Int32 c = ++cnt[(size_t)f];
		if (c == 1)
			tri0[(size_t)f] = t;
		else if (c == 2)
			tri1[(size_t)f] = t;
	}
	for (Int32 f = 0; f < inCnt; f++)
	{
		if (cnt[(size_t)f] != 2 || inPolys[f].c == inPolys[f].d)
			continue;
		const Int32 t = tri0[(size_t)f], j = tri1[(size_t)f];
		const ProjectDamagePoly& ct = P.tris[(size_t)t];
		const ProjectDamagePoly& cj = P.tris[(size_t)j];
		const Int32 tv[3] = { ct.a, ct.b, ct.c };
		const Int32 jv[3] = { cj.a, cj.b, cj.c };
		Int32 p = NOTOK, q = NOTOK, rest = NOTOK;
		for (int s = 0; s < 3; s++)
		{
			const Int32 e0 = tv[s], e1 = tv[(s + 1) % 3];
			const Bool in0 = e0 == jv[0] || e0 == jv[1] || e0 == jv[2];
			const Bool in1 = e1 == jv[0] || e1 == jv[1] || e1 == jv[2];
			if (in0 && in1)
			{
				p = e0;
				q = e1;
				rest = tv[(s + 2) % 3];
				break;
			}
		}
		if (p == NOTOK)
			continue;
		Int32 far = NOTOK;
		for (int k = 0; k < 3; k++)
			if (jv[k] != p && jv[k] != q)
				far = jv[k];
		if (far == NOTOK)
			continue;
		if (P.haveUV)
		{
			P.triProps[(size_t)t] = ProjectDamagePoly{ PropAt(P, t, p), PropAt(P, j, far),
											PropAt(P, t, q), PropAt(P, t, rest) };
		}
		P.tris[(size_t)t] = ProjectDamagePoly{ p, far, q, rest };
		P.mate[(size_t)t] = j;
		P.used[(size_t)t] = P.used[(size_t)j] = true;
	}
}

static void PairByCoplanarity(TopoPairing& P)
{
	std::vector<EVec> tn((size_t)P.nt);
	ProjectDamageParallelFor(0, (int64_t)P.nt, [&](int64_t i)
	{
		const ProjectDamagePoly& c = P.tris[(size_t)i];
		EVec n = m::la::cross(Pos(P, c.b) - Pos(P, c.a), Pos(P, c.c) - Pos(P, c.a));
		const double len = m::la::length(n);
		if (len > 0)
			n = n * (1.0 / len);
		tn[(size_t)i] = n;
	});
	ProjectDamageAdjacency nb;
	if (nb.Build(MeshView{ nullptr, P.nv, P.tris.data(), P.nt }))
	{
	const int sides[3] = { 0, 1, 3 };
	for (Int32 t = 0; t < P.nt; t++)
	{
		if (P.used[(size_t)t] || P.isChip[(size_t)t])
			continue;
		const ProjectDamageAdjInfo* info = nb.GetInfo(t);
		const ProjectDamagePoly& ct = P.tris[(size_t)t];
		const Int32 tv[4] = { ct.a, ct.b, ct.c, ct.d };
		const EVec n = tn[(size_t)t];
		for (int si = 0; si < 3; si++)
		{
			const int s = sides[si];
			const Int32 j = info->face[s];
			if (j < 0 || j >= P.nt || j == t || P.used[(size_t)j])
				continue;
			if (P.effects[(size_t)t] != P.effects[(size_t)j] || P.isChip[(size_t)j] || m::la::dot(n, tn[(size_t)j]) < COPLANAR_COS)
				continue;
			const Int32 p = tv[s], q = tv[(s + 1) & 3];
			const ProjectDamagePoly& cj = P.tris[(size_t)j];
			const Int32 tv3[3] = { ct.a, ct.b, ct.c };
			const Int32 jv[3] = { cj.a, cj.b, cj.c };
			Int32 rest = NOTOK, far = NOTOK;
			for (int k = 0; k < 3; k++)
			{
				if (rest == NOTOK && tv3[k] != p && tv3[k] != q) rest = tv3[k];
				if (far == NOTOK && jv[k] != p && jv[k] != q) far = jv[k];
			}
			if (far == NOTOK || rest == NOTOK)
				continue;
			const EVec quad[4] = { Pos(P, p), Pos(P, far), Pos(P, q), Pos(P, rest) };
			if (!ConvexQuad(quad, n))
				continue;
			if (P.haveUV && (PropAt(P, t, p) != PropAt(P, j, p)
						|| PropAt(P, t, q) != PropAt(P, j, q)))
				continue;
			if (P.haveUV)
				P.triProps[(size_t)t] = ProjectDamagePoly{ PropAt(P, t, p), PropAt(P, j, far),
												PropAt(P, t, q), PropAt(P, t, rest) };
			P.tris[(size_t)t] = ProjectDamagePoly{ p, far, q, rest };
			P.mate[(size_t)t] = j;
			P.used[(size_t)t] = P.used[(size_t)j] = true;
			break;
		}
	}
	}
}

void BuildTopology(const m::MeshGL& mesh, Int32 baseId, Int32 topology,
	Bool wantChipTris, const std::unordered_map<Int32, Int32>& idToClass,
	const ProjectDamagePoly* inPolys, Int32 inCnt, Bool baseMap, TopoBuild& tb,
    const std::vector<uint8_t>* papered = nullptr,
    const std::unordered_map<Int32, Int32>* effectOwners = nullptr,
    const std::vector<uint8_t>* plasterFaces = nullptr)
{
	const Int32 nv = (Int32)mesh.NumVert();
	const Int32 nt = (Int32)mesh.NumTri();

	tb.weldedCount = BuildWeldSlots(mesh, tb.geoOf);
	const Bool haveUV = tb.weldedCount > 0;
	auto geo = [&](Int32 v) { return haveUV ? tb.geoOf[(size_t)v] : v; };

	std::vector<Int32> effectOf((size_t)nt, 0);
	std::vector<Bool> isChip((size_t)nt, false);
	std::vector<Bool> noNoise((size_t)nt, false);
	std::vector<Int32> crackLvl((size_t)nt, NOTOK);
	std::vector<Bool> isHole((size_t)nt, false);
    std::vector<Int32> wpSlotOf((size_t)nt, NOTOK);
    std::vector<Bool> isFlap((size_t)nt, false);
    const WallpaperFaceIds wpIds = WallpaperIds();
    std::vector<Int32> brickSlotOf((size_t)nt, NOTOK);
    std::vector<Int32> extraSlotOf((size_t)nt, NOTOK);
	std::vector<Bool> isBuild((size_t)nt, false);
	std::vector<Bool> isBreak((size_t)nt, false);
	const Int32 nRun = (Int32)mesh.runOriginalID.size();
	const Bool merge = topology == PROJECT_DAMAGE_TOPO_QUADS || topology == PROJECT_DAMAGE_TOPO_NGONS;
	const Bool keep = topology == PROJECT_DAMAGE_TOPO_KEEP;
	if ((Int32)mesh.runIndex.size() >= nRun + 1)
	{
		for (Int32 r = 0; r < nRun; r++)
		{
			const Int32 t0 = (Int32)(mesh.runIndex[r] / 3);
			const Int32 t1 = (Int32)(mesh.runIndex[r + 1] / 3);
			const Bool chip = (Int32)mesh.runOriginalID[r] != baseId;
			auto itc = idToClass.find((Int32)mesh.runOriginalID[r]);
			const Int32 cls = itc != idToClass.end() ? itc->second : NOTOK;
			const Int32 lvl = (cls >= (Int32)DamageClass::Fracture1
							&& cls <= (Int32)DamageClass::Fracture3)
				? cls - (Int32)DamageClass::Fracture1 : NOTOK;
			const Bool hole = cls == (Int32)DamageClass::Surface || cls == (Int32)DamageClass::Coating || cls == (Int32)DamageClass::Rust;
			const Bool build = cls == (Int32)DamageClass::Buildup;
			const Bool brk = cls == (Int32)DamageClass::Break;
            const Int32 wpSlot = cls >= (Int32)DamageClass::WallpaperFront && cls <= (Int32)DamageClass::WallpaperWall
                ? cls - (Int32)DamageClass::WallpaperFront : NOTOK;
            const Bool wpFlap = (Int32)mesh.runOriginalID[r] == wpIds.flapFront
                || (Int32)mesh.runOriginalID[r] == wpIds.flapBack;
            const Int32 brickSlot = cls >= (Int32)DamageClass::BrickFront && cls <= (Int32)DamageClass::BrickBehind
                ? cls - (Int32)DamageClass::BrickFront : cls == (Int32)DamageClass::BrickRender ? 4 : NOTOK;
			const Int32 extraSlot = cls >= (Int32)DamageClass::Bevel && cls <= (Int32)DamageClass::ScratchScuff ? cls - (Int32)DamageClass::Bevel : NOTOK;
			const Bool quiet = hole || build || brk || cls >= (Int32)DamageClass::Fracture1;
			for (Int32 t = t0; t < t1 && t < nt; t++)
			{
				isChip[(size_t)t] = chip;
				noNoise[(size_t)t] = quiet;
                if (effectOwners) {
                    auto ef = effectOwners->find((Int32)mesh.runOriginalID[r]);
                    if (ef != effectOwners->end()) effectOf[(size_t)t] = ef->second;
                }
				crackLvl[(size_t)t] = lvl;
                wpSlotOf[(size_t)t] = wpSlot;
                isFlap[(size_t)t] = wpFlap;
                brickSlotOf[(size_t)t] = brickSlot;
                extraSlotOf[(size_t)t] = extraSlot;
				isHole[(size_t)t] = hole;
				isBuild[(size_t)t] = build;
				isBreak[(size_t)t] = brk;
			}
		}
	}

    if (plasterFaces && mesh.faceID.size() == (size_t)nt)
        for (Int32 t = 0; t < nt; ++t)
            if (effectOf[(size_t)t] == 0 && mesh.faceID[(size_t)t] < plasterFaces->size()
                    && (*plasterFaces)[mesh.faceID[(size_t)t]]) effectOf[(size_t)t] = 24;
	std::vector<ProjectDamagePoly> tris((size_t)nt);
	std::vector<ProjectDamagePoly> triProps;
	if (haveUV)
		triProps.resize((size_t)nt);
	for (Int32 t = 0; t < nt; t++)
	{
		const Int32 a = (Int32)mesh.triVerts[t * 3 + 0];
		const Int32 b = (Int32)mesh.triVerts[t * 3 + 1];
		const Int32 c = (Int32)mesh.triVerts[t * 3 + 2];
		if (haveUV)
			triProps[(size_t)t] = ProjectDamagePoly{ a, b, c, c };
		tris[(size_t)t] = ProjectDamagePoly{ geo(a), geo(b), geo(c), geo(c) };
	}
	std::vector<Int32> mate((size_t)nt, NOTOK);
	std::vector<Bool> used((size_t)nt, false);
	std::vector<EVec> weldedPos;
	if (merge)
	{
		const Int32 np = (Int32)mesh.numProp;
		weldedPos.resize((size_t)(haveUV ? tb.weldedCount : nv));
		for (Int32 i = 0; i < nv; i++)
		{
			const Int32 slot = haveUV ? tb.geoOf[(size_t)i] : i;
			if (slot < 0 || (size_t)slot >= weldedPos.size())
				continue;
			weldedPos[(size_t)slot] = EVec(mesh.vertProperties[(size_t)i * np + 0],
				mesh.vertProperties[(size_t)i * np + 1],
				mesh.vertProperties[(size_t)i * np + 2]);
		}
	}
	TopoPairing pairing{ mesh, nt, haveUV ? tb.weldedCount : nv, haveUV, weldedPos,
						tris, triProps, isChip, effectOf, mate, used };
	if (keep && inPolys != nullptr && (Int32)mesh.faceID.size() == nt)
		PairByFaceID(pairing, inPolys, inCnt);
	if (merge)
		PairByCoplanarity(pairing);

	std::vector<Bool> consumed((size_t)nt, false);
	for (Int32 t = 0; t < nt; t++)
		if (mate[(size_t)t] != NOTOK)
			consumed[(size_t)mate[(size_t)t]] = true;

	const Bool wantBase = baseMap && (Int32)mesh.faceID.size() == nt;
	auto baseOf = [&](Int32 t) -> Int32
	{
		if (isChip[(size_t)t])
			return NOTOK;
		const Int32 f = (Int32)mesh.faceID[(size_t)t];
		return (f >= 0 && f < inCnt) ? f : NOTOK;
	};

	tb.polys.reserve((size_t)nt);
	if (haveUV)
		tb.propCorner.reserve((size_t)nt * 4);
	if (wantBase)
		tb.basePolyOf.reserve((size_t)nt);
	for (Int32 t = 0; t < nt; t++)
	{
		if (consumed[(size_t)t])
			continue;
		const Int32 idx = (Int32)tb.polys.size();
		tb.polys.push_back(tris[(size_t)t]);
		if (wantBase)
		{
			const Int32 mt = mate[(size_t)t];
			const Int32 bt = baseOf(t);
			tb.basePolyOf.push_back(mt == NOTOK || baseOf(mt) == bt ? bt : NOTOK);
		}
        // Both triangles of a merged polygon must be papered. This keeps a
        // coplanar merge from colouring an adjacent Avoid/source region.
        auto isPapered = [&](Int32 tri) {
            return papered && !isChip[(size_t)tri] && (Int32)mesh.faceID.size() == nt
                && mesh.faceID[(size_t)tri] < papered->size() && (*papered)[mesh.faceID[(size_t)tri]];
        };
        const bool paperBase = isPapered(t) && (mate[(size_t)t] == NOTOK || isPapered(mate[(size_t)t]));
        tb.effectIds.push_back(paperBase ? 14 : effectOf[(size_t)t]);
		if (haveUV)
		{
			const ProjectDamagePoly& pc = triProps[(size_t)t];
			const Bool quad = tris[(size_t)t].c != tris[(size_t)t].d;
			tb.propCorner.push_back(pc.a);
			tb.propCorner.push_back(pc.b);
			tb.propCorner.push_back(pc.c);
			tb.propCorner.push_back(quad ? pc.d : NOTOK);
		}
        if (wpSlotOf[(size_t)t] != NOTOK)
            tb.wallpaperPolys[(size_t)wpSlotOf[(size_t)t]].push_back(idx);
        if (isFlap[(size_t)t])
            tb.wallpaperFlapPolys.push_back(idx);
        else if (paperBase) {
            tb.wallpaperPolys[0].push_back(idx);
            tb.wallpaperBasePolys.push_back(idx);
        }
        else if (brickSlotOf[(size_t)t] != NOTOK)
            tb.brickPolys[(size_t)brickSlotOf[(size_t)t]].push_back(idx);
        else if (extraSlotOf[(size_t)t] != NOTOK)
            tb.extraPolys[(size_t)extraSlotOf[(size_t)t]].push_back(idx);
        else if (crackLvl[(size_t)t] != NOTOK)
		{
			tb.crackPolys[(size_t)crackLvl[(size_t)t]].push_back(idx);
		}
		else if (isHole[(size_t)t])
		{
			tb.holePolys.push_back(idx);
		}
		else if (isBuild[(size_t)t])
		{
			tb.buildPolys.push_back(idx);
		}
		else if (isBreak[(size_t)t])
		{
			tb.breakPolys.push_back(idx);
		}
		else if (isChip[(size_t)t])
		{
			tb.chipPolys.push_back(idx);
			if (wantChipTris && !noNoise[(size_t)t])
				tb.chipTris.push_back(t);
		}
	}
}

void ApplyNoise(EVec* points, Int32 pointCount, const m::MeshGL& mesh,
	const std::vector<Int32>& chipTris, const ProjectDamageParams& params,
	const std::vector<Int32>& geoOf, Int32 weldedCount)
{
	const Int32 nt = (Int32)mesh.NumTri();
	const Int32 mv = (Int32)mesh.NumVert();
	const Int32 np = (Int32)mesh.numProp;
	const Int32 nv = weldedCount > 0 ? weldedCount : mv;
	auto slot = [&](Int32 v) { return geoOf.empty() ? v : geoOf[(size_t)v]; };
	std::vector<m::vec3> posOf((size_t)nv, m::vec3(0.0, 0.0, 0.0));
	for (Int32 v = 0; v < mv; v++)
	{
		const Int32 s = slot(v);
		if (s >= 0 && s < nv)
			posOf[(size_t)s] = m::vec3(mesh.vertProperties[(size_t)v * np + 0],
									   mesh.vertProperties[(size_t)v * np + 1],
									   mesh.vertProperties[(size_t)v * np + 2]);
	}
	auto pos = [&](Int32 s) { return posOf[(size_t)s]; };

	std::vector<bool> mask((size_t)nt, false);
	for (Int32 t : chipTris)
		mask[t] = true;

	std::vector<bool> inChip((size_t)nv, false), inBase((size_t)nv, false);
	for (Int32 t = 0; t < nt; t++)
	{
		const Int32 a = slot((Int32)mesh.triVerts[t * 3 + 0]);
		const Int32 b = slot((Int32)mesh.triVerts[t * 3 + 1]);
		const Int32 c = slot((Int32)mesh.triVerts[t * 3 + 2]);
		if (mask[t]) { inChip[a] = inChip[b] = inChip[c] = true; }
		else         { inBase[a] = inBase[b] = inBase[c] = true; }
	}
	std::vector<Int32> interior, rim;
	for (Int32 v = 0; v < nv; v++)
	{
		if (inChip[v] && !inBase[v]) interior.push_back(v);
		if (inChip[v] && inBase[v])  rim.push_back(v);
	}
	if (interior.empty())
		return;

	std::vector<m::vec3> vn((size_t)nv, m::vec3(0.0, 0.0, 0.0));
	for (Int32 t = 0; t < nt; t++)
	{
		if (!mask[t])
			continue;
		const Int32 a = slot((Int32)mesh.triVerts[t * 3 + 0]);
		const Int32 b = slot((Int32)mesh.triVerts[t * 3 + 1]);
		const Int32 c = slot((Int32)mesh.triVerts[t * 3 + 2]);
		const m::vec3 fn = m::la::cross(pos(b) - pos(a), pos(c) - pos(a));
		vn[a] = vn[a] + fn;
		vn[b] = vn[b] + fn;
		vn[c] = vn[c] + fn;
	}

	const double sc = MaxD(1e-6, params.noiseScale);
	std::vector<double> w(interior.size(), 1.0);
	if (!rim.empty())
	{
		const double half = 0.5 * sc;
		auto cellOf = [&](double v) { return (int64_t)std::floor(v / half); };
		auto packKey = [](int64_t x, int64_t y, int64_t z) -> int64_t
		{
			return ((x + (1 << 20)) & 0x1FFFFF)
				| (((y + (1 << 20)) & 0x1FFFFF) << 21)
				| (((z + (1 << 20)) & 0x1FFFFF) << 42);
		};
		std::unordered_map<int64_t, std::vector<Int32>> grid;
		grid.reserve(rim.size());
		for (Int32 rv : rim)
		{
			const m::vec3 rp = pos(rv);
			grid[packKey(cellOf(rp.x), cellOf(rp.y), cellOf(rp.z))].push_back(rv);
		}
		for (size_t i = 0; i < interior.size(); i++)
		{
			const m::vec3 iv = pos(interior[i]);
			const int64_t bx = cellOf(iv.x), by = cellOf(iv.y), bz = cellOf(iv.z);
			double dmin = std::numeric_limits<double>::infinity();
			for (int64_t dx = -1; dx <= 1; dx++)
				for (int64_t dy = -1; dy <= 1; dy++)
					for (int64_t dz = -1; dz <= 1; dz++)
					{
						auto it = grid.find(packKey(bx + dx, by + dy, bz + dz));
						if (it == grid.end())
							continue;
						for (Int32 rv : it->second)
						{
							const m::vec3 d = iv - pos(rv);
							const double dd = m::la::dot(d, d);
							if (dd < dmin) dmin = dd;
						}
					}
			if (dmin == std::numeric_limits<double>::infinity())
				continue;
			dmin = std::sqrt(dmin);
			double ww = dmin / half;
			if (ww < 0.0) ww = 0.0;
			if (ww > 1.0) ww = 1.0;
			w[i] = ww * ww * (3.0 - 2.0 * ww);
		}
	}

	const EVec off(params.seed * 37.61, params.seed * 71.23, params.seed * 13.37);
	const double roughness = Clamp01(params.noiseRoughness) * 0.75;
	const double distortion = Clamp01(params.noiseDistortion);
	for (size_t i = 0; i < interior.size(); i++)
	{
		const Int32 idx = interior[i];
		if (idx < 0 || idx >= pointCount)
			continue;
		const m::vec3 p = pos(idx);
		double ln = m::la::length(vn[idx]);
		if (ln == 0.0) ln = 1.0;
		const EVec pv(p.x, p.y, p.z);
		EVec q = pv / sc + off;
		// при нулях остаётся прежний шум, без дополнительных слоёв
		if (distortion > 0.0)
		{
			const EVec warp(
				ProjectDamageNoise(q + EVec(17.31, 5.73, 41.19)) - 0.5,
				ProjectDamageNoise(q + EVec(53.87, 29.43, 11.67)) - 0.5,
				ProjectDamageNoise(q + EVec(7.91, 61.37, 37.13)) - 0.5);
			q += warp * (3.0 * distortion);
		}
		double sample = ProjectDamageNoise(q);
		if (roughness > 0.0)
		{
			double weight = roughness, total = 1.0, frequency = 2.0;
			for (int octave = 0; octave < 3; ++octave)
			{
				sample += weight * ProjectDamageNoise(q * frequency);
				total += weight;
				weight *= roughness;
				frequency *= 2.0;
			}
			sample /= total;
		}
		const double d = (sample - 0.5) * 2.0;
		const m::vec3 n = vn[idx] * (params.noiseStrength * d * w[i] / ln);
		points[idx] = EVec(p.x + n.x, p.y + n.y, p.z + n.z);
	}
}
}

m::Manifold BuildInputManifold(const double* xyz, size_t nv, const int32_t* abcd, size_t np,
	uint32_t originalId, bool stampFaceIds, const double* uvs)
{
	m::MeshGL mesh;
	if (uvs == nullptr)
	{
		mesh.numProp = 3;
		mesh.vertProperties.resize(nv * 3);
		for (size_t i = 0; i < nv * 3; i++)
			mesh.vertProperties[i] = (float)xyz[i];
		for (size_t i = 0; i < np; i++)
		{
			const int32_t* cp = abcd + i * 4;
			mesh.triVerts.push_back((uint32_t)cp[0]);
			mesh.triVerts.push_back((uint32_t)cp[1]);
			mesh.triVerts.push_back((uint32_t)cp[2]);
			if (stampFaceIds)
				mesh.faceID.push_back((uint32_t)i);
			if (cp[2] != cp[3])
			{
				mesh.triVerts.push_back((uint32_t)cp[0]);
				mesh.triVerts.push_back((uint32_t)cp[2]);
				mesh.triVerts.push_back((uint32_t)cp[3]);
				if (stampFaceIds)
					mesh.faceID.push_back((uint32_t)i);
			}
		}
		mesh.runOriginalID.push_back(originalId);
		return m::Manifold(mesh);
	}

	mesh.numProp = 5;
	mesh.vertProperties.reserve(nv * 5);
	std::vector<Int32> headProp(nv, NOTOK);
	std::vector<Int32> nextProp;
	nextProp.reserve(nv);
	auto propFor = [&](Int32 point, float u, float v) -> uint32_t
	{
		for (Int32 p = headProp[(size_t)point]; p != NOTOK; p = nextProp[(size_t)p])
		{
			if (mesh.vertProperties[(size_t)p * 5 + 3] == u
				&& mesh.vertProperties[(size_t)p * 5 + 4] == v)
				return (uint32_t)p;
		}
		const Int32 p = (Int32)(mesh.vertProperties.size() / 5);
		mesh.vertProperties.push_back((float)xyz[(size_t)point * 3 + 0]);
		mesh.vertProperties.push_back((float)xyz[(size_t)point * 3 + 1]);
		mesh.vertProperties.push_back((float)xyz[(size_t)point * 3 + 2]);
		mesh.vertProperties.push_back(u);
		mesh.vertProperties.push_back(v);
		nextProp.push_back(headProp[(size_t)point]);
		headProp[(size_t)point] = p;
		return (uint32_t)p;
	};

	for (size_t i = 0; i < np; i++)
	{
		const int32_t* cp = abcd + i * 4;
		const double* uv = uvs + i * 8;
		const uint32_t a = propFor(cp[0], (float)uv[0], (float)uv[1]);
		const uint32_t b = propFor(cp[1], (float)uv[2], (float)uv[3]);
		const uint32_t c = propFor(cp[2], (float)uv[4], (float)uv[5]);
		mesh.triVerts.push_back(a);
		mesh.triVerts.push_back(b);
		mesh.triVerts.push_back(c);
		if (stampFaceIds)
			mesh.faceID.push_back((uint32_t)i);
		if (cp[2] != cp[3])
		{
			const uint32_t d = propFor(cp[3], (float)uv[6], (float)uv[7]);
			mesh.triVerts.push_back(a);
			mesh.triVerts.push_back(c);
			mesh.triVerts.push_back(d);
			if (stampFaceIds)
				mesh.faceID.push_back((uint32_t)i);
		}
	}
	for (size_t i = 0; i < nv; i++)
	{
		if (headProp[i] == NOTOK)
			propFor((Int32)i, 0.0f, 0.0f);
	}
	for (size_t i = 0; i < nv; i++)
	{
		const Int32 head = headProp[i];
		if (head == NOTOK)
			continue;
		for (Int32 p = nextProp[(size_t)head]; p != NOTOK; p = nextProp[(size_t)p])
		{
			mesh.mergeFromVert.push_back((uint32_t)p);
			mesh.mergeToVert.push_back((uint32_t)head);
		}
	}

	mesh.runOriginalID.push_back(originalId);
	return m::Manifold(mesh);
}

constexpr double TILE_CREASE_DOT = 0.9396926;

static void ApplyTileOffset(EVec* points, Int32 pointCount,
	const std::vector<ProjectDamagePoly>& polys, const std::vector<Int32>& basePolyOf,
	const TopoBuild& tb, const std::vector<bool>* avoidPoly,
	const ProjectDamageParams& params)
{
	const Int32 npoly = (Int32)polys.size();
	const Bool tileOn = !params.fractPaintOnly && params.fractTileOffset > 0.0;
	const Bool shardOn = params.fractShardEnable && params.fractShardAmount > 0.0
		&& !params.fractShardPoints.empty();
	if (!(tileOn || shardOn) || npoly <= 0 || pointCount <= 0)
		return;

	std::vector<Int32> cls((size_t)npoly, 0);
	auto markDamage = [&](const std::vector<Int32>& v)
	{
		for (Int32 i : v)
			if (i >= 0 && i < npoly)
				cls[(size_t)i] = -1;
	};
	markDamage(tb.chipPolys);
	markDamage(tb.holePolys);
    for (int k = 0; k < 3; ++k) markDamage(tb.wallpaperPolys[k]);
    for (int k = 0; k < 5; ++k) markDamage(tb.brickPolys[k]);
    for (int k = 0; k < 4; ++k) markDamage(tb.extraPolys[k]);
    for (Int32 idx : tb.wallpaperBasePolys) cls[(size_t)idx] = 0;
	markDamage(tb.buildPolys);
	markDamage(tb.breakPolys);
	std::vector<Int32> lvlOf((size_t)npoly, NOTOK);
	for (Int32 l = 0; l < 3; l++)
	{
		for (Int32 i : tb.crackPolys[(size_t)l])
		{
			if (i >= 0 && i < npoly)
			{
				cls[(size_t)i] = -1;
				lvlOf[(size_t)i] = l;
			}
		}
	}
	if (avoidPoly != nullptr && (Int32)basePolyOf.size() == npoly)
	{
		for (Int32 i = 0; i < npoly; i++)
		{
			const Int32 b = basePolyOf[(size_t)i];
			if (cls[(size_t)i] == 0 && b >= 0 && (size_t)b < avoidPoly->size()
				&& (*avoidPoly)[(size_t)b])
				cls[(size_t)i] = -2;
		}
	}

	auto faceNormal = [&](const ProjectDamagePoly& p)
	{
		const EVec a = points[p.a], b = points[p.b];
		const EVec c = points[p.c], d = points[p.d];
		return p.c != p.d ? m::la::cross(c - a, d - b)
						  : m::la::cross(b - a, c - a);
	};
	auto cornerCount = [](const ProjectDamagePoly& p) { return p.c != p.d ? 4 : 3; };
	auto cornerAt = [](const ProjectDamagePoly& p, Int32 k)
	{
		return k == 0 ? p.a : (k == 1 ? p.b : (k == 2 ? p.c : p.d));
	};

	std::vector<Int32> parent((size_t)npoly);
	for (Int32 i = 0; i < npoly; i++)
		parent[(size_t)i] = i;
	auto find = [&parent](Int32 x)
	{
		while (parent[(size_t)x] != x)
		{
			parent[(size_t)x] = parent[(size_t)parent[(size_t)x]];
			x = parent[(size_t)x];
		}
		return x;
	};

	std::unordered_map<UInt64, Int32> firstOf;
	firstOf.reserve((size_t)npoly * 2);
	std::vector<std::pair<Int32, Int32>> bounding;
	std::vector<bool> creasePt((size_t)pointCount, false);
	for (Int32 i = 0; i < npoly; i++)
	{
		const ProjectDamagePoly& p = polys[(size_t)i];
		const Int32 n = cornerCount(p);
		for (Int32 k = 0; k < n; k++)
		{
			const Int32 u = cornerAt(p, k);
			const Int32 v = cornerAt(p, k == 0 ? n - 1 : k - 1);
			const UInt64 key = ((UInt64)(UInt32)(u < v ? u : v) << 32)
				| (UInt64)(UInt32)(u < v ? v : u);
			auto it = firstOf.find(key);
			if (it == firstOf.end())
			{
				firstOf.emplace(key, i);
				continue;
			}
			const Int32 j = it->second;
			const Int32 ci = cls[(size_t)i], cj = cls[(size_t)j];
			if (ci == 0 && cj == 0)
			{
				const EVec ni = faceNormal(polys[(size_t)i]);
				const EVec nj = faceNormal(polys[(size_t)j]);
				const double li = m::la::length(ni), lj = m::la::length(nj);
				if (li > 1e-12 && lj > 1e-12)
				{
					if (m::la::dot(ni / li, nj / lj) >= TILE_CREASE_DOT)
					{
						const Int32 ra = find(i), rb = find(j);
						if (ra != rb)
							parent[(size_t)rb] = ra;
					}
					else
					{
						if (u >= 0 && u < pointCount)
							creasePt[(size_t)u] = true;
						if (v >= 0 && v < pointCount)
							creasePt[(size_t)v] = true;
					}
				}
			}
			else if (ci == 0 && cj == -1 && lvlOf[(size_t)j] != NOTOK)
				bounding.emplace_back(i, lvlOf[(size_t)j]);
			else if (cj == 0 && ci == -1 && lvlOf[(size_t)i] != NOTOK)
				bounding.emplace_back(j, lvlOf[(size_t)i]);
		}
	}
	{
		std::vector<Int32> ownerOf((size_t)pointCount, NOTOK);
		for (Int32 i = 0; i < npoly; i++)
		{
			if (cls[(size_t)i] != 0)
				continue;
			const ProjectDamagePoly& p = polys[(size_t)i];
			const Int32 n = cornerCount(p);
			for (Int32 k = 0; k < n; k++)
			{
				const Int32 v = cornerAt(p, k);
				if (v < 0 || v >= pointCount || creasePt[(size_t)v])
					continue;
				Int32& own = ownerOf[(size_t)v];
				if (own == NOTOK)
				{
					own = find(i);
					continue;
				}
				const Int32 ra = find(own), rb = find(i);
				if (ra != rb)
					parent[(size_t)rb] = ra;
				own = ra;
			}
		}
	}

	std::vector<Int32> minLvl((size_t)npoly, NOTOK);
	for (const auto& pr : bounding)
	{
		const Int32 r = find(pr.first);
		const Int32 cur = minLvl[(size_t)r];
		if (cur == NOTOK
			|| params.fract[(size_t)pr.second].depth < params.fract[(size_t)cur].depth)
			minLvl[(size_t)r] = pr.second;
	}

	std::vector<EVec> nsum((size_t)npoly, EVec(0.0));
	for (Int32 i = 0; i < npoly; i++)
		if (cls[(size_t)i] == 0)
			nsum[(size_t)find(i)] += faceNormal(polys[(size_t)i]);

	std::vector<Int32> pcomp((size_t)pointCount, NOTOK);
	std::vector<bool> pconf((size_t)pointCount, false);
	std::vector<bool> frozen((size_t)pointCount, false);
	for (Int32 i = 0; i < npoly; i++)
	{
		const Int32 c = cls[(size_t)i];
		if (c == -1)
			continue;
		const ProjectDamagePoly& p = polys[(size_t)i];
		const Int32 n = cornerCount(p);
		const Int32 r = c == 0 ? find(i) : NOTOK;
		for (Int32 k = 0; k < n; k++)
		{
			const Int32 v = cornerAt(p, k);
			if (v < 0 || v >= pointCount)
				continue;
			if (c == -2)
			{
				frozen[(size_t)v] = true;
				continue;
			}
			if (pconf[(size_t)v])
				continue;
			if (pcomp[(size_t)v] == NOTOK)
				pcomp[(size_t)v] = r;
			else if (pcomp[(size_t)v] != r)
			{
				pconf[(size_t)v] = true;
				pcomp[(size_t)v] = NOTOK;
			}
		}
	}
	std::vector<bool> blocked((size_t)npoly, false);
	for (Int32 i = 0; i < npoly; i++)
	{
		if (cls[(size_t)i] != 0)
			continue;
		const ProjectDamagePoly& p = polys[(size_t)i];
		const Int32 n = cornerCount(p);
		for (Int32 k = 0; k < n; k++)
		{
			const Int32 v = cornerAt(p, k);
			if (v >= 0 && v < pointCount
				&& (frozen[(size_t)v] || pconf[(size_t)v]))
			{
				blocked[(size_t)find(i)] = true;
				break;
			}
		}
	}

	PyRandom rng((uint32_t)(params.seed + 7003 + params.fractTileSeed));
	std::vector<EVec> shift((size_t)npoly, EVec(0.0));
	for (Int32 i = 0; i < npoly; i++)
	{
		if (cls[(size_t)i] != 0 || find(i) != i)
			continue;
		const double u = rng.Uniform(-1.0, 1.0);
		const Int32 l = minLvl[(size_t)i];
		if (l == NOTOK || blocked[(size_t)i])
			continue;
		const double depth = params.fract[(size_t)l].depth * params.fractDepthMult;
		const EVec n = nsum[(size_t)i];
		const double len = m::la::length(n);
		if (tileOn && len > 1e-12)
			shift[(size_t)i] = n * (u * params.fractTileOffset * depth / len);
	}

	// Shard Displace: pieces near the guide points move along their own
	// normal (pushed in or lifted out), fading with distance; each piece
	// moves rigidly (a linear field over it), so the side nearer a point
	// moves more and the piece tilts. The big surrounding surface (largest
	// piece) never moves, and inward moves stop short of the crack floor.
	std::vector<double> shA((size_t)npoly, 0.0);       // move at the centroid
	std::vector<EVec> shG((size_t)npoly, EVec(0.0));   // change per unit offset
	std::vector<EVec> shC((size_t)npoly, EVec(0.0));   // centroid
	std::vector<EVec> shN((size_t)npoly, EVec(0.0));   // unit normal
	if (shardOn)
	{
		std::vector<double> area((size_t)npoly, 0.0);
		for (Int32 i = 0; i < npoly; i++)
		{
			if (cls[(size_t)i] != 0)
				continue;
			const ProjectDamagePoly& p = polys[(size_t)i];
			const double a = 0.5 * m::la::length(faceNormal(p));
			const Int32 n = cornerCount(p);
			EVec c(0.0);
			for (Int32 k = 0; k < n; k++)
				c += points[cornerAt(p, k)];
			const Int32 r = find(i);
			area[(size_t)r] += a;
			shC[(size_t)r] += c * (a / (double)n);
		}
		Int32 biggest = NOTOK;
		for (Int32 i = 0; i < npoly; i++)
			if (cls[(size_t)i] == 0 && find(i) == i
				&& (biggest == NOTOK || area[(size_t)i] > area[(size_t)biggest]))
				biggest = i;
		const double falloff = MaxD(0.05, params.fractShardFalloff);
		auto weightAt = [&](const EVec& x)
		{
			double w = 0.0;
			for (const FractProximity& fp : params.fractShardPoints)
			{
				const double R = fp.radius > 0.0 ? fp.radius : params.fractShardRadius;
				if (!(R > 0.0))
					continue;
				const double d = m::la::length(x - EVec(fp.x, fp.y, fp.z));
				const double t = 1.0 - d / R;
				if (t > 0.0)
					w = MaxD(w, std::pow(t, falloff));
			}
			return w;
		};
		PyRandom rs((uint32_t)(params.seed + 7301 + params.fractShardSeed));
		const double tilt = Clamp01(params.fractShardTilt);
		const double rnd = Clamp01(params.fractShardRandom);
		for (Int32 i = 0; i < npoly; i++)
		{
			if (cls[(size_t)i] != 0 || find(i) != i)
				continue;
			const double uDir = rs.Random(), uAmt = rs.Random();
			const Int32 l = minLvl[(size_t)i];
			if (i == biggest || l == NOTOK || blocked[(size_t)i] || !(area[(size_t)i] > 0.0))
				continue;
			const EVec nn = nsum[(size_t)i];
			const double nl = m::la::length(nn);
			if (nl < 1e-12)
				continue;
			const EVec n = nn / nl;
			const EVec c = shC[(size_t)i] / area[(size_t)i];
			const double w = weightAt(c);
			// a piece reaching into the radius still moves a bit at its far side
			double sign = params.fractShardDir == 1 ? 1.0 : -1.0;
			if (params.fractShardDir == 2)
				sign = uDir < 0.5 ? -1.0 : 1.0;
			const double amt = sign * params.fractShardAmount * (1.0 - rnd * uAmt);
			// gradient of the weight in the piece's plane (central differences)
			EVec g(0.0);
			if (tilt > 0.0)
			{
				const double h = MaxD(1e-3, 0.25 * std::sqrt(area[(size_t)i]));
				EVec t1 = m::la::cross(n, std::abs(n.z) < 0.9 ? EVec(0, 0, 1) : EVec(1, 0, 0));
				t1 = t1 / MaxD(m::la::length(t1), 1e-12);
				const EVec t2 = m::la::cross(n, t1);
				const double g1 = (weightAt(c + t1 * h) - weightAt(c - t1 * h)) / (2.0 * h);
				const double g2 = (weightAt(c + t2 * h) - weightAt(c - t2 * h)) / (2.0 * h);
				g = (t1 * g1 + t2 * g2) * (amt * tilt);
			}
			if (w <= 0.0 && m::la::length(g) <= 0.0)
				continue;
			shA[(size_t)i] = amt * w;
			shG[(size_t)i] = g;
			shC[(size_t)i] = c;
			shN[(size_t)i] = n;
			// keep a pushed-in piece above the crack floor
			const double floorD = 0.9 * params.fract[(size_t)l].depth * params.fractDepthMult;
			if (shA[(size_t)i] < -floorD)
				shA[(size_t)i] = -floorD;
		}
		for (Int32 i = 0; i < npoly; i++)
			shC[(size_t)i] = find(i) == i ? shC[(size_t)i] : EVec(0.0);
	}

	for (Int32 v = 0; v < pointCount; v++)
	{
		const Int32 r = pcomp[(size_t)v];
		if (r < 0 || frozen[(size_t)v])
			continue;
		EVec mv = shift[(size_t)r];
		if (shardOn && (shA[(size_t)r] != 0.0 || m::la::length(shG[(size_t)r]) > 0.0))
		{
			double a = shA[(size_t)r] + m::la::dot(shG[(size_t)r], points[v] - shC[(size_t)r]);
			const Int32 l = minLvl[(size_t)r];
			const double floorD = 0.9 * params.fract[(size_t)l].depth * params.fractDepthMult;
			// the tilt never flips the move or digs below the crack floor
			if (shA[(size_t)r] < 0.0) a = std::min(0.0, std::max(a, -floorD));
			else if (shA[(size_t)r] > 0.0) a = std::max(0.0, std::min(a, 2.0 * shA[(size_t)r] + 1e-9));
			else a = params.fractShardDir == 1 ? std::max(0.0, a) : std::max(-floorD, std::min(0.0, a));
			mv += shN[(size_t)r] * a;
		}
		points[v] += mv;
	}
}

std::vector<Int32> DamageCacheIncrementalBuilt(ProjectDamageDamageCache* c) {return c->incrementalBuilt;}
std::vector<Int32> DamageCacheIncrementalReused(ProjectDamageDamageCache* c) {return c->incrementalReused;}
// Cached solids retain exact geometry; the final shared Boolean is still evaluated.
// These entries contain no Blender objects, pointers into inputs, or random state.
struct IncrementalDamageStage {
    UInt64 key=0;
    m::Manifold solid;bool applied=false;std::string note;Int32 baseId=0;
    DebrisMesh fragments;std::vector<CellPieceInfo> cellInfo;std::vector<RBPieceInfo> rbInfo;
    std::vector<uint8_t> faceMask;std::shared_ptr<const MetalDeformMarks> metalMarks;
    std::vector<m::Manifold> cuts, adds, extra, extra2, wpCuts, wpFlaps;
    std::vector<Int32> classes, addEffects, cutEffects;
    std::vector<m::Box> zones;
    EdgeWearField wear;
    std::vector<ScratchLine> scratches;
    WallHoleOut wall;
};
struct IncrementalDamageCache {
    UInt64 geometryKey=0;std::vector<EVec> normals;std::vector<ExposedEdge> exposed;
    std::unordered_map<Int32,IncrementalDamageStage> stages;
};

template<class Fn>
static bool CachedBaseStage(ProjectDamageDamageCache* cache,const ProjectDamageParams& params,Int32 id,
    m::Manifold& solid,Int32 baseId,Fn compute,std::vector<uint8_t>* faces=nullptr,
    std::shared_ptr<const MetalDeformMarks>* marks=nullptr,std::string* note=nullptr)
{
    const auto key=id<(Int32)params.incrementalKeys.size()?params.incrementalKeys[id]:0;
    if(!cache || !key) return compute();
    if(!cache->incremental) cache->incremental=std::make_shared<IncrementalDamageCache>();
    auto& entries=cache->incremental->stages;auto found=entries.find(id);
    if(found!=entries.end() && found->second.key==key) {
        const auto& e=found->second;solid=e.solid;
        if(e.applied && e.baseId!=baseId && solid.NumTri()) {
            auto mesh=solid.GetMeshGL64();
            for(auto& run:mesh.runOriginalID)if(run==(uint32_t)e.baseId)run=(uint32_t)baseId;
            solid=m::Manifold(mesh);
        }
        if(faces)*faces=e.faceMask;if(marks)*marks=e.metalMarks;if(note)*note=e.note;
        cache->incrementalReused.push_back(id);return e.applied;
    }
    const bool applied=compute();IncrementalDamageStage e;e.key=key;e.solid=solid;e.applied=applied;e.baseId=baseId;
    if(faces)e.faceMask=*faces;if(marks)e.metalMarks=*marks;if(note)e.note=*note;
    entries[id]=std::move(e);cache->incrementalBuilt.push_back(id);return applied;
}

static void PlaceDamage(const CoreInput& in, const ProjectDamageParams& params, const ProjectDamageHost& host,
	std::vector<m::Manifold>& cutters, std::vector<Int32>& cutterClass,
	std::vector<Int32>& cutterId, std::vector<m::Manifold>& buildup,
	std::vector<Int32>& buildId, std::vector<m::Manifold>& beads, std::vector<m::Manifold>& cornerPiles, const HeatField& heat, const RustField& rust, EdgeWearField& wearField, std::vector<ScratchLine>& scratchLines, std::vector<m::Manifold>& lathPlanks, std::vector<m::Manifold>& concreteBars, std::vector<m::Manifold>& separateBricks, std::vector<m::Manifold>& brickMortar, std::vector<m::Box>& brickZones, std::unordered_map<Int32, Int32>& effectOwners, ProjectDamageDamageCache* cache)
{
    std::vector<Int32> cutEffects, addEffects;
    std::vector<m::Manifold> wpCuts, wpFlaps;
    // Back-surface texture (Concrete-style) for the area behind the laths:
    // only cutter vertices inside the object move, so nothing dents around it
    std::shared_ptr<ProjectDamageBVH> backBvh;
    std::function<bool(const EVec&)> backInside;
    auto textureBack = [&](std::vector<m::Manifold>& cuts, int32_t mode, const std::string& path, double scale,
        double depth, double ox, double oy, double oz, double edge, const char* what) {
        if (mode != 1 || !(depth > 0.0) || cuts.empty()) return;
        if (!backInside) backInside = MakeInsideTest(in.view, backBvh);
        TexSpec t;
        t.path = path; t.scale = scale; t.depth = depth; t.offX = ox; t.offY = oy; t.offZ = oz; t.edge = edge;
        HostStatus(host, std::string("texturing ") + what, 40);
        std::vector<std::string> logs(cuts.size());
        ProjectDamageParallelFor(0, (int64_t)cuts.size(), [&](int64_t i) {
            cuts[(size_t)i] = TextureDisplaceSolid(cuts[(size_t)i], t, &logs[(size_t)i], &backInside);
        });
        for (const std::string& msg : logs) if (!msg.empty()) { HostLog(host, msg); break; }
    };

    auto stage = [&](Int32 id, auto compute) {
        const UInt64 key = id < (Int32)params.incrementalKeys.size() ? params.incrementalKeys[id] : 0;
        if (!cache || !key) { compute(); return; }
        if (!cache->incremental) cache->incremental=std::make_shared<IncrementalDamageCache>();
        auto& entries=cache->incremental->stages;
        auto found=entries.find(id);
        if(found!=entries.end() && found->second.key==key) {
            const auto& e=found->second;
            cutters.insert(cutters.end(),e.cuts.begin(),e.cuts.end());
            cutterClass.insert(cutterClass.end(),e.classes.begin(),e.classes.end());
            buildup.insert(buildup.end(),e.adds.begin(),e.adds.end());
            addEffects.insert(addEffects.end(),e.addEffects.begin(),e.addEffects.end());
            cutEffects.insert(cutEffects.end(),e.cutEffects.begin(),e.cutEffects.end());
            if(id==10) concreteBars=e.extra;
            if(id==13) lathPlanks=e.extra;
            if(id==18) { separateBricks=e.extra;brickMortar=e.extra2;brickZones=e.zones; }
            if(id==21) wearField=e.wear;
            if(id==22) scratchLines=e.scratches;
            if(id==26) gWallHoleOut=e.wall;
            if(id==14) { wpCuts=e.wpCuts;wpFlaps=e.wpFlaps; }
            cache->incrementalReused.push_back(id);
            return;
        }
        const auto nc=cutters.size(),na=buildup.size(),ne=addEffects.size(),nce=cutEffects.size();
        compute();
        IncrementalDamageStage e;e.key=key;
        e.cuts.assign(cutters.begin()+nc,cutters.end());
        e.classes.assign(cutterClass.begin()+nc,cutterClass.end());
        e.adds.assign(buildup.begin()+na,buildup.end());
        e.addEffects.assign(addEffects.begin()+ne,addEffects.end());
        e.cutEffects.assign(cutEffects.begin()+nce,cutEffects.end());
        if(id==10) e.extra=concreteBars;
        if(id==13) e.extra=lathPlanks;
        if(id==18) { e.extra=separateBricks;e.extra2=brickMortar;e.zones=brickZones; }
        if(id==21) e.wear=wearField;
        if(id==22) e.scratches=scratchLines;
        if(id==26) e.wall=gWallHoleOut;
        if(id==14) { e.wpCuts=wpCuts;e.wpFlaps=wpFlaps; }
        entries[id]=std::move(e);cache->incrementalBuilt.push_back(id);
    };
    auto stamp = [&](Int32 effect) {
        cutEffects.resize(cutters.size(), effect);
        addEffects.resize(buildup.size(), effect);
    };
	const Int32 vcnt = in.view.npolys;
	const EVec* pts = in.view.pts;
	const ProjectDamagePoly* polys = in.view.polys;
    std::vector<EVec> normals;std::vector<ExposedEdge> exposed;
    const UInt64 geometryKey=params.incrementalKeys.empty()?0:params.incrementalKeys[0];
    if(cache && geometryKey && cache->incremental && cache->incremental->geometryKey==geometryKey) {
        normals=cache->incremental->normals;exposed=cache->incremental->exposed;
    } else {
        normals=PolyNormals(pts,polys,vcnt);
        for(Int32 i=0;i<vcnt;++i)normals[i]=GuidedNormal(in.view,i,normals[i]);
        HostStatus(host,"detecting edges",15);
        DetectExposed(in.view,normals,params.thresholdDeg,exposed,host);
        if(cache && geometryKey) {
            if(!cache->incremental)cache->incremental=std::make_shared<IncrementalDamageCache>();
            cache->incremental->geometryKey=geometryKey;cache->incremental->normals=normals;cache->incremental->exposed=exposed;
        }
    }
	std::vector<Chain> chains;
	GroupObjectEdges(exposed, pts, params.thresholdDeg, chains);

	const std::vector<Chain> chainsAll = chains;

	{
		std::vector<Chain> kept;
		for (Chain& c : chains)
		{
			int cc = 0;
			for (ExposedEdge* e : c.edges)
				if (e->convex) cc++;
			if ((double)cc > (double)c.edges.size() / 2.0)
				kept.push_back(std::move(c));
		}
		chains.swap(kept);
	}

	if (!params.axisX || !params.axisY || !params.axisZ)
	{
		std::vector<Chain> kept;
		for (Chain& c : chains)
		{
			m::vec3 w(0.0, 0.0, 0.0);
			const std::vector<Int32>& P = c.points;
			for (size_t i = 0; i + 1 < P.size(); i++)
			{
				const m::vec3 d = pts[P[i + 1]] - pts[P[i]];
				w = w + m::vec3(std::abs(d.x), std::abs(d.y), std::abs(d.z));
			}
			int dom = 0;
			double best = w.x;
			if (w.y > best) { dom = 1; best = w.y; }
			if (w.z > best) { dom = 2; }
			const Bool on = dom == 0 ? params.axisX : (dom == 1 ? params.axisY : params.axisZ);
			if (on)
				kept.push_back(std::move(c));
		}
		chains.swap(kept);
	}

	if (params.avoidChips)
		chains = SplitChainsByAvoid(chains, in.avoidPoly, pts);

    stage(1,[&]() {
	HostStatus(host, "placing chips (" + std::to_string(chains.size()) + " chains)", 30);
	ProjectDamageBVH thicknessBvh;
	const ProjectDamageBVH* bvhUse = nullptr;
	double thicknessRayLen = 0.0;
	if (params.normEnable || (params.chipGuard > 0.0 && params.amount > 0))
	{
		thicknessBvh.Build(in.view);
		bvhUse = &thicknessBvh;
		thicknessRayLen = 2.0 * m::la::length(in.bbRad) + 100.0;
	}
    auto chipPlacement = ChipPlacement::FromParams(params);
    chipPlacement.paintPaths = in.chipPaint;
    chipPlacement.paintRadius = in.chipPaintRadius;
	cutters = PlaceCutters(chains, pts, normals,
		chipPlacement, bvhUse, thicknessRayLen);
	cutterClass.assign((size_t)cutters.size(), (Int32)DamageClass::Edge);
    });
    stamp(1);
    stage(2,[&]() {
	{
		std::vector<m::Manifold> cornerCutters = PlaceCornerCutters(exposed, pts, params,
			params.avoidCorner ? in.avoidPoly : nullptr);
		for (m::Manifold& c : cornerCutters)
		{
			cutters.push_back(std::move(c));
			cutterClass.push_back((Int32)DamageClass::Corner);
		}
	}
    });
    stamp(2);
    stage(3,[&]() {
	{
		std::vector<m::Manifold> surfCutters = PlaceSurfaceCutters(in.view, normals, exposed,
			params, params.avoidSurf ? in.avoidPoly : nullptr, in.surfacePaint);
		for (m::Manifold& c : surfCutters)
		{
			cutters.push_back(std::move(c));
			cutterClass.push_back((Int32)DamageClass::Surface);
		}
	}
    });
    stamp(3);
    stage(4,[&]() {
        auto place = [&](const ProjectDamageParams& layer,
                         const std::vector<std::vector<EVec>>* guides,
                         const std::vector<Int32>* levels, Int32 effect) {
            std::vector<m::Manifold> cracks, spalls;
            std::vector<Int32> crackLevels;
            PlaceFracture(in.view,pts,normals,chainsAll,layer,host,
                layer.avoidFract ? in.avoidPoly : nullptr,guides,cracks,crackLevels,spalls,levels);
            for(size_t i=0;i<cracks.size();++i) {
                cutters.push_back(std::move(cracks[i]));
                cutterClass.push_back((Int32)DamageClass::Fracture1+crackLevels[i]);
            }
            for(auto& cut:spalls) {
                cutters.push_back(std::move(cut));
                cutterClass.push_back((Int32)DamageClass::FractureChip);
            }
            cutEffects.resize(cutters.size(),effect);
        };
        if(params.fractureLayers.empty()) place(params,in.guideSplines,in.guideLevels,4);
        else for(const auto& saved:params.fractureLayers) {
            auto layer=saved;layer.mask=params.mask;
            std::vector<std::vector<EVec>> guides;
            size_t offset=0;
            for(Int32 count:layer.fractureGuideCounts) {
                if(count<2 || offset+size_t(count)*3>layer.fractureGuidePoints.size()) break;
                std::vector<EVec> path;path.reserve(count);
                for(Int32 i=0;i<count;++i,offset+=3)
                    path.emplace_back(layer.fractureGuidePoints[offset],layer.fractureGuidePoints[offset+1],layer.fractureGuidePoints[offset+2]);
                guides.push_back(std::move(path));
            }
            place(layer,&guides,&layer.fractureGuideLevels,layer.fractureEffectId);
        }
    });
    stamp(4);
	const std::vector<Chain> buildChains = params.avoidBuild
		? SplitChainsByAvoid(chainsAll, in.avoidPoly, pts)
		: chainsAll;
    stage(5,[&]() {

	// Bead (5), Corner Piles (28) and Deposits (29) are three generators
	// since 2026-10-26: own effect stamps, so each gets its own group.
	buildup = PlaceBuildup(buildChains, pts, polys, vcnt, normals, params, true, false);
    });
    stamp(5);
    stage(28,[&]() {
	{
		std::vector<m::Manifold> piles = PlaceBuildup(buildChains, pts, polys, vcnt, normals, params, false, true);
		for (auto& solid : piles) buildup.push_back(std::move(solid));
	}
    });
    stamp(28);
    stage(29,[&]() {
	if (params.depositAmount > 0 && !params.depositSeparate)
	{
		auto deposits = PlaceSurfaceDeposits(in, params, normals);
		for (auto& solid : deposits) buildup.push_back(std::move(solid));
	}
    });
    stamp(29);
    stage(6,[&]() {
	if (heat.valid) {
		std::vector<m::Manifold> heatCuts, heatBlisters;
		PlaceHeatDamage(in, params, normals, heat, heatCuts, heatBlisters);
		for (auto& c : heatCuts) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Surface); }
		for (auto& b : heatBlisters) buildup.push_back(std::move(b));
	}
    });
    stamp(6);
    stage(7,[&]() {
	if (rust.valid)
	{
		std::vector<m::Manifold> rustCuts, rustAdds;
		PlaceRust(in, params, rust, rustCuts, rustAdds);
		if (params.rustPaint) BuildRustPaint(in, params, rust, params.coatingThickness, rustCuts, rustAdds);
        KeepOneMesh(in, rustCuts, rustAdds, 0.05 * params.coatingThickness);
		for (auto& cut : rustCuts) { cutters.push_back(std::move(cut)); cutterClass.push_back((Int32)DamageClass::Rust); }
		for (auto& add : rustAdds) buildup.push_back(std::move(add));
	}
    });
    stamp(7);
    stage(8,[&]() {
	if (params.coatingEnable)
	{
		std::vector<m::Manifold> cuts, flakes;
		PlaceCoatingPeeling(in, params, normals, cuts, flakes);
		for (auto& cut : cuts) { cutters.push_back(std::move(cut)); cutterClass.push_back((Int32)DamageClass::Coating); }
		for (auto& flake : flakes) buildup.push_back(std::move(flake));
	}
    });
    stamp(8);
    stage(26,[&]() {
	if (params.wallHole.enable)
	{
		std::vector<m::Manifold> holes;
		std::vector<Int32> cls;
		gWallHoleOut = WallHoleOut();
		PlaceWallHoles(in, params.wallHole, params.seed, holes, cls, gWallHoleOut, host);
		for (size_t i = 0; i < holes.size(); ++i) { cutters.push_back(std::move(holes[i])); cutterClass.push_back(cls[i]); }
	}
	if (params.hang.enable && params.hang.mode == 0)
	{
		std::vector<m::Manifold> holes;
		PlaceHangHoleCutters(in, params.hang, params.seed, holes, host);
		for (auto& h : holes) { cutters.push_back(std::move(h)); cutterClass.push_back((Int32)DamageClass::Break); }
	}
    });
    stamp(26);
    stage(9,[&]() {
	if (params.fabric.enable)
	{
		std::vector<m::Manifold> cuts, threads;
		CoreInput fabricIn = in;
		if (in.fabricAvoidPoly) fabricIn.avoidPoly = in.fabricAvoidPoly;
		FabricTearPaint drawn;
        drawn.holes = in.fabricHoles; drawn.edges = in.fabricEdges; drawn.reach = params.surfPaintSnap;
        if (params.fabric.solid) {
            if (!params.fabric.paintOnly)
                PlaceFabricSolidTears(fabricIn, params.fabric, params.seed, cuts, threads, host);
            // Reuse the closed-contour cutter and thread builder for drawings;
            // disable its procedural generators to keep the Solidify recipe.
            FabricTearParams paintParams=params.fabric;
            paintParams.holesEnable=false;paintParams.edgeEnable=false;paintParams.hangEnable=false;
            paintParams.paintOnly=true;
            PlaceFabricTears(fabricIn, paintParams, params.seed, params.mask, cuts, threads, in.fabricWeights, &drawn);
        }
        else
            PlaceFabricTears(fabricIn, params.fabric, params.seed, params.mask, cuts, threads, in.fabricWeights, &drawn);
		for (auto& cut : cuts) { cutters.push_back(std::move(cut)); cutterClass.push_back((Int32)DamageClass::Surface); }
		for (auto& thread : threads) buildup.push_back(std::move(thread));
	}
    });
    stamp(9);
    stage(10,[&]() {
	if (params.concrete.enable)
	{
		std::vector<m::Manifold> cuts, cracks, bars;
		PlaceConcreteDamage(in, params.concrete, params.seed, params.mask, in.concreteStrokes, cuts, cracks, bars);
		const ConcreteDamageParams& cp = params.concrete;
		if (cp.surfMode == 1 && cp.texStrength > 0.0 && !cuts.empty())
		{
			// Texture spall surface: the procedural spall (terraces, roughness)
			// is the bottom layer, the image displacement goes on top of it
			TexSpec t;
			t.path = cp.texPath; t.scale = cp.texScale; t.depth = cp.texStrength;
			t.offX = cp.texOffX; t.offY = cp.texOffY; t.offZ = cp.texOffZ; t.edge = cp.texEdge;
			HostStatus(host, "texturing " + std::to_string(cuts.size()) + " concrete spalls", 40);
			std::shared_ptr<ProjectDamageBVH> objBvh;
			const auto inside = MakeInsideTest(in.view, objBvh);
			std::vector<std::string> logs(cuts.size());
			ProjectDamageParallelFor(0, (int64_t)cuts.size(), [&](int64_t i) {
				cuts[(size_t)i] = TextureDisplaceSolid(cuts[(size_t)i], t, &logs[(size_t)i], &inside);
			});
			for (const std::string& msg : logs) if (!msg.empty()) { HostLog(host, msg); break; }
		}
		for (auto& cut : cuts) { cutters.push_back(std::move(cut)); cutterClass.push_back((Int32)DamageClass::Surface); }
		for (auto& crack : cracks) { cutters.push_back(std::move(crack)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
		for (auto& bar : bars) concreteBars.push_back(std::move(bar));
	}
    });
    stamp(10);
    stage(11,[&]() {
    if (params.hit.enable && !params.hit.empties.empty()) {
        std::vector<m::Manifold> cuts, pushed, cracks;
        if (params.hit.through) {
            // Concrete Hit Through: `pushed` carries the chunk gaps here; they
            // take the crack material, like Fracture Through's cell gaps
            PlaceConcreteHitThrough(in, params.hit, params.seed, params.mask, cuts, pushed, cracks);
            for (auto& c : cuts) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Surface); }
            for (auto& c : pushed) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
            for (auto& c : cracks) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        } else {
        PlaceConcreteHit(in, params.hit, params.seed, params.mask, cuts, pushed, cracks);
        for (auto& c : cuts) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& c : pushed) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture2); }
        for (auto& c : cracks) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        }
    }
    });
    stamp(11);
    stage(12,[&]() {
    if (params.wood.enable) {
        std::vector<m::Manifold> cuts, cracks, slivers;
        PlaceWoodDamage(in, params.wood, params.seed, params.mask, in.woodPaint, cuts, cracks, slivers);
        for (auto& c : cuts) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& c : cracks) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& b : slivers) buildup.push_back(std::move(b));
    }
    });
    stamp(12);
    stage(13,[&]() {

    if (params.lath.enable) {
        LathDamageOut ld;
        PlaceLathDamage(in, params.lath, params.seed, params.mask, in.lathPaint, ld);
        auto& cuts=ld.cutters; auto& cracks=ld.cracks;
        const LathDamageParams& lp = params.lath;
        textureBack(cuts, lp.surfMode, lp.texPath, lp.texScale, lp.texStrength, lp.texOffX, lp.texOffY, lp.texOffZ, lp.texEdge, "the area behind the laths");
        if (lp.surfMode == 0 && lp.noiseStrength > 0.0 && !cuts.empty()) {
            // Procedural back surface (surface_noise): the Texture displacement with noise for the image
            if (!backInside) backInside = MakeInsideTest(in.view, backBvh);
            TexSpec t;
            t.proc = true; t.scale = lp.noiseScale; t.depth = lp.noiseStrength; t.edge = lp.texEdge;
            t.detail = lp.noiseDetail; t.roughness = lp.noiseRoughness; t.distortion = lp.noiseDistortion;
            HostStatus(host, "roughening the area behind the laths", 40);
            std::vector<std::string> logs(cuts.size());
            ProjectDamageParallelFor(0, (int64_t)cuts.size(), [&](int64_t i) {
                cuts[(size_t)i] = TextureDisplaceSolid(cuts[(size_t)i], t, &logs[(size_t)i], &backInside);
            });
            for (const std::string& msg : logs) if (!msg.empty()) { HostLog(host, msg); break; }
        }
        lathPlanks=std::move(ld.planks);
        for (auto& c : cuts) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& c : cracks) { cutters.push_back(std::move(c)); cutterClass.push_back((Int32)DamageClass::Fracture1); }

    }
    });
    stamp(13);
    stage(14,[&]() {
    if (params.wallpaper.enable) {
        WallpaperParams wp = params.wallpaper;
        wp.paintReach = params.surfPaintSnap;
        PlaceWallpaper(in, wp, params.seed, params.mask, normals, wpCuts, wpFlaps, in.wallpaperPaint);
    }
    });
    stamp(14);
    stage(15,[&]() {
    if (params.bullet.enable) {
        std::vector<m::Manifold> a, b, c, adds;
        PlaceBulletImpacts(in, params.bullet, params.seed, params.mask, in.bulletPaint, a, b, c, adds);
        for (auto& x : a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& x : b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& x : c) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture2); }
        for (auto& x : adds) buildup.push_back(std::move(x));
    }
    });
    stamp(15);
    stage(16,[&]() {
    if (params.blast.enable) {
        std::vector<m::Manifold> a, b, c, adds;
        PlaceBuildingDestruction(in, params.blast, params.seed, params.mask, a, c, b);
        for (auto& x : a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& x : b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& x : c) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture2); }
        for (auto& x : adds) buildup.push_back(std::move(x));
    }
    });
    stamp(16);
    stage(17,[&]() {
    if (params.ground.enable) {
        std::vector<m::Manifold> a, b, c, adds;
        int32_t rubbleCount = 0;
        PlaceGroundCracks(in, params.ground, params.seed, params.mask, in.groundPaint, a, b, adds, &rubbleCount);
        for (auto& x : a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& x : b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        if (params.ground.generatedGeometry) for (size_t i = 0; i < adds.size(); ++i) {
            buildup.push_back(std::move(adds[i]));
            // Internal provenance 30 distinguishes rubble from heaved slabs.
            // Python exports it as Ground (17), plus a rubble polygon mask.
            addEffects.push_back(i < size_t(rubbleCount) ? 30 : 17);
        }
    }
    });
    stamp(17);
    stage(18,[&]() {
    if (params.brick.enable) {
        HostStatus(host, "building brick damage cut volumes", 35);
        BrickDamageOut bd;
        PlaceBrickDamage(in, params.brick, params.seed, params.mask, in.brickPaint, bd);
        auto push = [&](std::vector<m::Manifold>& solids, DamageClass cls) {
            for (auto& x : solids) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)cls); }
        };
        push(bd.cutters, DamageClass::BrickFront);
        push(bd.breaks, DamageClass::BrickBreak);
        push(bd.seams, DamageClass::BrickSeam);
        push(bd.behind, DamageClass::BrickBehind);
        push(bd.render, DamageClass::BrickRender);
        push(bd.cracks, DamageClass::Fracture1);
        if(params.brick.brickMode==1 && params.brick.style!=2) {separateBricks=std::move(bd.bricks);brickMortar=std::move(bd.mortar);brickZones=std::move(bd.zones);}
        else for (auto& x : bd.bricks) buildup.push_back(std::move(x));
    }
    });
    stamp(18);
    stage(19,[&]() {
    if (params.glass.enable) {
        std::vector<m::Manifold> a, b, c, adds;
        BrokenGlassPaint drawn; drawn.holes = in.glassHolePaint; drawn.impacts = in.glassImpactPaint; drawn.reach = params.surfPaintSnap;
        std::vector<m::Manifold> glassGaps;
        if (params.glass.through)
            PlaceBrokenGlassThrough(in, params.glass, params.seed, params.mask, a, b, c, glassGaps, &drawn);
        else
            PlaceBrokenGlass(in, params.glass, params.seed, params.mask, a, b, c, &drawn);
        // Broken Glass Through: the shard gaps take the crack material
        for (auto& x : glassGaps) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& x : a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& x : b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& x : c) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::FractureChip); }
        for (auto& x : adds) buildup.push_back(std::move(x));
    }
    });
    stamp(19);
    stage(20,[&]() {
    if (params.fissure.enable) {
        std::vector<m::Manifold> a, b, c, adds;
        PlaceGroundFissures(in, params.fissure, params.seed, params.mask, in.fissurePaint, a, b, adds);
        for (auto& x : a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
        for (auto& x : b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Fracture1); }
        for (auto& x : adds) buildup.push_back(std::move(x));
    }
    });
    stamp(20);
    stage(21,[&]() {
    if (params.bevel.enable) {
        std::vector<m::Manifold> a,b;
        PlaceEdgeBevel(in, params.bevel, params.seed, params.mask, in.bevelPaint, a,b,&wearField);
        for(auto& x:a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Bevel); }
        for(auto& x:b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::EdgeWear); }
    }
    if (params.damagedBevel.enable) {   // damaged_bevel
        std::vector<m::Manifold> a,b;
        PlaceDamagedBevel(in, params.damagedBevel, params.seed, params.mask, in.damagedBevelPaint, &a, &b, &wearField, nullptr);
        for(auto& x:a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Bevel); }
        for(auto& x:b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::EdgeWear); }
    }
    });
    stamp(21);
    stage(22,[&]() {
    if (params.scratch.enable) {
        std::vector<m::Manifold> a,b;
        PlaceScratches(in, params.scratch, params.seed, params.mask, in.scratchStrokes,a,b,&scratchLines);
        for(auto& x:a) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::ScratchGroove); }
        for(auto& x:b) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::ScratchScuff); }
    }
    });
    stamp(22);
    stage(27,[&]() {
    if (params.rusty.enable) {
        std::vector<m::Manifold> holes;
        PlaceRustyHoles(in, params.rusty, params.seed, params.mask, in.rustyPaint, holes);
        for (auto& x : holes) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Surface); }
    }
    });
    stamp(27);
    stage(32,[&]() {
    if (params.erosion.enable) {
        std::vector<m::Manifold> eroded;
        std::string note;
        PlaceVoxelErosion(in, params.erosion, params.seed, params.mask, eroded, &note);
        if (!note.empty()) HostLog(host, note);
        for (auto& x : eroded) { cutters.push_back(std::move(x)); cutterClass.push_back((Int32)DamageClass::Coating); }
    }
    });
    stamp(32);
    stage(34,[&]() {
    if(params.metalTear.enable) {
        std::vector<m::Manifold> slits;
        PlaceMetalTearCuts(in,params.metalTear,params.seed,slits);
        for(auto& x:slits) {cutters.push_back(std::move(x));cutterClass.push_back((Int32)DamageClass::Coating);}
    }
    });
    stamp(34);
    std::vector<m::Manifold> mergedAdds;
    std::vector<Int32> mergedEffects;
    for(size_t i=0;i<buildup.size();++i) {
        if(addEffects[i]==5) beads.push_back(std::move(buildup[i]));
        else if(addEffects[i]==28) cornerPiles.push_back(std::move(buildup[i]));
        else {mergedAdds.push_back(std::move(buildup[i]));mergedEffects.push_back(addEffects[i]);}
    }
    buildup=std::move(mergedAdds);addEffects=std::move(mergedEffects);
	cutterId.assign(cutters.size(), NOTOK);
	ProjectDamageParallelFor(0, (int64_t)cutters.size(), [&cutters, &cutterId](int64_t i)
	{
		cutters[(size_t)i] = cutters[(size_t)i].AsOriginal();
		cutterId[(size_t)i] = (Int32)cutters[(size_t)i].OriginalID();
	});
	if (!buildup.empty())
	{
		buildId.assign(buildup.size(), NOTOK);
		ProjectDamageParallelFor(0, (int64_t)buildup.size(), [&buildup, &buildId](int64_t i)
		{
			buildup[(size_t)i] = buildup[(size_t)i].AsOriginal();
			buildId[(size_t)i] = (Int32)buildup[(size_t)i].OriginalID();
		});
	}
    for (size_t i = 0; i < cutterId.size(); ++i) effectOwners[cutterId[i]] = cutEffects[i];
    for (size_t i = 0; i < buildId.size(); ++i) effectOwners[buildId[i]] = addEffects[i];
    // Wallpaper contains multiple material provenance runs; AsOriginal would
    // destroy them. Append after generic stamping instead.
    if (params.wallpaper.enable) {
        const WallpaperFaceIds ids = WallpaperIds();
        for (auto& x : wpCuts) {
            cutters.push_back(std::move(x));
            cutterClass.push_back((Int32)DamageClass::WallpaperWall);
            cutterId.push_back(ids.wall);
        }
        for (auto& x : wpFlaps) {
            buildup.push_back(std::move(x));
            buildId.push_back(ids.back);
        }
    }
}

static void ResolveBreakSolids(const CoreInput& in, const ProjectDamageParams& params, const ProjectDamageHost& host,
	ProjectDamageDamageCache* cache, std::unordered_map<Int32, Int32>& idToClass,
	std::vector<m::Manifold>& breakSolids, std::vector<m::Manifold>& rebarSolids)
{
	HostStatus(host, "breaking " + std::to_string(params.breakers.size())
		+ " breakers", 50);
	const EVec bbMin = in.bbMp - in.bbRad;
	const EVec bbMax = in.bbMp + in.bbRad;
	const Bool sameBox = cache != nullptr
		&& SameEVec(cache->breakBbMin, bbMin) && SameEVec(cache->breakBbMax, bbMax);
	std::vector<m::Manifold> builtCut;
	std::vector<m::Manifold> builtLocal;
	ProjectDamageBVH texBvh;      // texture breakers: inside test of the object
	bool texBvhReady = false;
	builtCut.reserve(params.breakers.size());
	builtLocal.reserve(params.breakers.size());
	for (size_t i = 0; i < params.breakers.size(); i++)
	{
		const BreakerSpec& s = params.breakers[i];
		m::Manifold cut;
		m::Manifold local;
		if (sameBox && i < cache->breakSpec.size()
			&& SameBreaker(cache->breakSpec[i], s))
		{
			cut = cache->breakCutter[i];
			if (i < cache->breakLocal.size())
				local = cache->breakLocal[i];
		}
		else if (s.mode == BREAKER_MODE_CUSTOM
			&& s.surfMode == 0
			&& cache != nullptr && i < cache->breakSpec.size()
			&& i < cache->breakLocal.size()
			&& cache->breakLocal[i].NumTri() != 0
			&& SameCustomLocal(cache->breakSpec[i], s))
		{
			local = cache->breakLocal[i];
			cut = PlaceCustom(local, s.rel);
			if (cut.NumTri() != 0)
				cut = cut.AsOriginal();
		}
		else
		{
			if (s.mode == BREAKER_MODE_CUSTOM && !CustomIsSheet(s))
			{
				local = BuildCustomLocal(s, s.strength * (s.surfMode == 1 ? s.texMacro : 1.0));
				cut = PlaceCustom(local, s.rel);
			}
			else if (s.surfMode == 1 && s.texStrength > 0.0 && (s.mode == BREAKER_MODE_PLANE || s.mode == BREAKER_MODE_SPHERE))
			{
				// EXPERIMENTAL texture surface: refined where the object is,
				// so it needs an inside test of the object (ray parity, BVH)
				if (!texBvhReady) { texBvh.Build(in.view); texBvhReady = true; }
				const EVec skew = m::la::normalize(EVec(0.5773, 0.6123, 0.5401));
				const double eps = 1e-7 * m::la::length(bbMax - bbMin) + 1e-9;
				auto inside = [&](const EVec& p) {
					EVec o = p;
					int hits = 0;
					for (int guard = 0; guard < 256; ++guard)
					{
						double d = 0.0;
						int32_t poly = -1;
						if (!texBvh.Raycast(o, skew, 1e30, &d, &poly)) break;
						++hits;
						o = o + skew * (d + eps);
					}
					return (hits & 1) == 1;
				};
				std::string msg;
				cut = BuildTextureBreakCutter(s, s.strength, bbMin, bbMax, inside, &msg);
				if (!msg.empty()) HostLog(host, msg);
				if (cut.NumTri() == 0) cut = BuildBreakCutter(s, s.strength, bbMin, bbMax);
			}
			else
			{
				cut = BuildBreakCutter(s, s.strength * (s.surfMode == 1 ? s.texMacro : 1.0), bbMin, bbMax);
			}
			if (s.surfMode == 1 && (s.mode == BREAKER_MODE_CUSTOM || s.mode == BREAKER_MODE_STRIP)) {
				std::string message;
				cut = TextureDisplaceCutter(cut, s, s.strength, &message);
				if (!message.empty()) HostLog(host,message);
			}
			if (s.smoothSlope && cut.NumTri() != 0)
			{
				// Smooth Slope: flare the opening into the surface; the band B
				// and the flare W follow the breaker size
				const double size = s.mode == BREAKER_MODE_SPHERE ? s.radius : 0.5 * std::min(s.length, s.width);
				const double B = std::max(1e-6, std::clamp((double)s.smoothWidth, 0.02, 1.0) * size);
				std::string msg;
				cut = FlareBreakCutter(cut, in.view, B, B, &msg);
				if (!msg.empty()) HostLog(host, msg);
			}
			if (cut.NumTri() != 0)
				cut = cut.AsOriginal();
		}
		builtLocal.push_back(local);
		builtCut.push_back(cut);
		if (cut.NumTri() == 0)
		{
			HostLog(host, "[ProjectDamage] a linked Breaker produced no cutter solid - skipped");
			continue;
		}
		idToClass[(Int32)cut.OriginalID()] = (Int32)DamageClass::Break;
		if (s.smoothSlope) gSmoothBreakIds.insert((Int32)cut.OriginalID());
		if(s.rebar) rebarSolids.push_back(cut);
        breakSolids.push_back(std::move(cut));
	}
	if (cache != nullptr)
	{
		cache->breakSpec = params.breakers;
		for (BreakerSpec& s : cache->breakSpec)
			s.custom = nullptr;
		cache->breakCutter = std::move(builtCut);
		cache->breakLocal = std::move(builtLocal);
		cache->breakBbMin = bbMin;
		cache->breakBbMax = bbMax;
	}
}

static m::Manifold BuildOrServeDamagedMesh(const m::Manifold& box,
	const std::vector<m::Manifold>& cutters, const std::vector<m::Manifold>& buildup,
	const ProjectDamageParams& params, const ProjectDamageHost& host, ProjectDamageDamageCache* cache,
	UInt64 cacheFingerprint, std::unordered_map<Int32, Int32>& idToClass, Int32& baseId, std::unordered_map<Int32, Int32>& effectOwners)
{
	m::Manifold result;
	if (cache != nullptr && cache->hasDamaged && cache->fp == cacheFingerprint)
	{
		result = cache->damaged;
		baseId = cache->damagedBaseId;
	}
	else
	{
		result = box;
		for (const m::Manifold& cu : cutters)
			result = result - cu;
		if (project_damage::ProfileEnabled()) {
			result.NumTri();
			HostStatus(host, "secondary pass (chips, rims)", 60);
		}
		result = RunSecondaryPass(std::move(result), baseId, idToClass, params,
			host, false, effectOwners);
		for (const m::Manifold& b : buildup)
			result = result + b;
		if (cache != nullptr && !params.breakers.empty())
		{
			result.NumTri();
			result = m::Manifold(result.GetMeshGL64());
			cache->damaged = result;
			cache->damagedBaseId = baseId;
			cache->hasDamaged = true;
            cache->effectOwners=effectOwners;
		}
	}
	return result;
}

static Bool PrepareDrawMask(const CoreInput& inRaw, const ProjectDamageParams& paramsRaw,
    DrawStrokeMask& mask, CoreInput& in, ProjectDamageParams& params, std::vector<double>& heat)
{
    if (inRaw.drawMask == nullptr || !mask.Build(*inRaw.drawMask, paramsRaw.mask))
        return false;
    in = inRaw;
    params = paramsRaw;
    params.mask = &mask;
    if (inRaw.drawMask->heat && params.heatEnable) {
        const size_t n = (size_t)in.view.npts;
        heat.assign(n, 1.0);
        if (inRaw.heatWeights != nullptr && inRaw.heatWeights->size() == n)
            heat = *inRaw.heatWeights;
        for (size_t i = 0; i < n; ++i)
            heat[i] *= mask.DrawWeight(in.view.pts[i]);
        in.heatWeights = &heat;
    }
    return true;
}

static void AppendGlassCell(m::MeshGL& mesh, const m::MeshGL& cell,
    uint32_t originalId, uint32_t baseId, uint32_t cutId)
{
    // Keep cell topology separate, including on planes with coincident edges.
    const uint32_t vertOffset = mesh.NumVert(), indexOffset = uint32_t(mesh.triVerts.size());
    for (size_t v = 0; v < cell.NumVert(); ++v) {
        const auto start = cell.vertProperties.begin() + v * cell.numProp;
        const size_t stride = std::min(mesh.numProp, cell.numProp);
        mesh.vertProperties.insert(mesh.vertProperties.end(), start, start + stride);
        mesh.vertProperties.insert(mesh.vertProperties.end(), mesh.numProp - stride, 0.0f);
    }
    for (const auto i : cell.triVerts) mesh.triVerts.push_back(vertOffset + i);
    for (const auto i : cell.mergeFromVert) mesh.mergeFromVert.push_back(vertOffset + i);
    for (const auto i : cell.mergeToVert) mesh.mergeToVert.push_back(vertOffset + i);
    if (mesh.runIndex.empty()) mesh.runIndex.push_back(indexOffset);
    for (size_t r = 0; r < cell.runOriginalID.size(); ++r) {
        mesh.runOriginalID.push_back(cell.runOriginalID[r] == originalId ? baseId : cutId);
        mesh.runIndex.push_back(indexOffset + cell.runIndex[r + 1]);
    }
    mesh.faceID.resize(indexOffset / 3, uint32_t(-1));
    mesh.faceID.insert(mesh.faceID.end(), cell.faceID.begin(), cell.faceID.end());
    mesh.faceID.resize(mesh.NumTri(), uint32_t(-1));
    mesh.runTransform.clear();
    mesh.runFlags.clear();
    mesh.halfedgeTangent.clear();
}

static size_t GlassFillRegionAt(const std::vector<m::Manifold>& regions, double dust, EVec& origin)
{
    size_t nearest = regions.size();
    double best = std::numeric_limits<double>::infinity();
    EVec projected = origin;
    const pdgen::V3 point(origin.x, origin.y, origin.z);
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        if (region.Volume() <= dust) continue;
        const auto bounds = region.BoundingBox();
        if (bounds.Contains(origin)) {
            const double reach = 2.0 * std::max(1.0, m::la::length(bounds.Size()));
            const auto hits = region.RayCast(origin, origin + EVec(1.0, 0.371, 0.623) * reach);
            if (hits.size() % 2 == 1) return i;
        }
        const EVec boxPoint = m::la::max(bounds.min, m::la::min(bounds.max, origin));
        if (m::la::dot(boxPoint-origin, boxPoint-origin) >= best) continue;
        const auto solid = region.GetMeshGL();
        auto pointAt = [&](uint32_t index) {
            const size_t j = size_t(index) * solid.numProp;
            return pdgen::V3(solid.vertProperties[j], solid.vertProperties[j+1], solid.vertProperties[j+2]);
        };
        for (size_t t = 0; t < solid.triVerts.size(); t += 3) {
            const auto q = pdgen::ClosestOnTriangle(point, pointAt(solid.triVerts[t]),
                pointAt(solid.triVerts[t+1]), pointAt(solid.triVerts[t+2]));
            const auto offset = q - point;
            const double dist = pdgen::Dot(offset, offset);
            if (dist >= best || !std::isfinite(dist)) continue;
            best = dist;
            projected = EVec(q.x,q.y,q.z);
            nearest = i;
        }
    }
    origin = projected;
    return nearest;
}

static void AppendGlassFill(m::MeshGL& mesh, const m::Manifold& original, const m::Manifold& result,
    uint32_t originalId, uint32_t baseId, uint32_t cutId, const std::vector<m::Manifold>& openings,
    const BrokenGlassParams& glass, Int32 mainSeed, const ProjectDamageHost& host)
{
    if (openings.empty()) return;
    // The hole cutters delimit the difference, excluding crack grooves,
    // exterior pieces and any unrelated damage elsewhere on the source.
    const m::Manifold opening = m::Manifold::BatchBoolean(openings, m::OpType::Add);
    const m::Manifold removed = (original ^ opening) - result;
    if (removed.Status() != m::Manifold::Error::NoError || removed.IsEmpty()) return;

    HostStatus(host, "cell fracture of glass hole fill", 89);
    const double dust = std::max(1e-12, 1e-8 * removed.Volume());
    const auto fills = removed.Decompose();
    EVec origin(glass.fillOriginX, glass.fillOriginY, glass.fillOriginZ);
    const size_t targeted = glass.fillOriginEnabled ? GlassFillRegionAt(fills, dust, origin) : fills.size();
    size_t regions = 0, count = 0;
    for (size_t i = 0; i < fills.size(); ++i) {
        const auto& region = fills[i];
        if (region.Volume() <= dust) continue;
        const m::MeshGL solid = region.GetMeshGL();
        std::vector<Int32> slots;
        const Int32 welded = BuildWeldSlots(solid, slots);
        std::vector<EVec> points(welded > 0 ? size_t(welded) : solid.NumVert());
        for (size_t v = 0; v < solid.NumVert(); ++v) {
            const size_t i = v * solid.numProp;
            points[slots.empty() ? v : size_t(slots[v])] =
                EVec(solid.vertProperties[i], solid.vertProperties[i+1], solid.vertProperties[i+2]);
        }
        auto vertex = [&](uint32_t i) { return slots.empty() ? Int32(i) : slots[i]; };
        std::vector<ProjectDamagePoly> polys;
        polys.reserve(solid.NumTri());
        for (size_t t = 0; t < solid.triVerts.size(); t += 3) {
            const Int32 c = vertex(solid.triVerts[t+2]);
            polys.push_back({vertex(solid.triVerts[t]), vertex(solid.triVerts[t+1]), c, c});
        }
        CoreInput local;
        local.view = {points.data(), Int32(points.size()), polys.data(), Int32(polys.size())};
        local.box = &region;
        local.baseId = Int32(originalId);
        CellFractureParams fracture;
        fracture.enable = true;
        fracture.material = 1; // Glass
        fracture.pieces = std::clamp(glass.fillCount, 2, 500);
        fracture.seed = glass.seed;
        const auto bounds = region.BoundingBox();
        const EVec size = bounds.max - bounds.min;
        const EVec centre = i == targeted ? origin : bounds.Center();
        const double extent = std::max({size.x, size.y, size.z});
        const double density = std::clamp(glass.fillCenter, 0.0, 1.0);
        fracture.gap = glass.throughGap > 0.0 ? glass.throughGap : 0.02 * glass.crackWidth;
        fracture.glassRingGrowth = 1.2 + 1.2 * density;
        fracture.proxDensity = 1.0 + 12.0 * density;
        fracture.proximity.push_back({centre.x, centre.y, centre.z, 0.35 * extent, 1.0});
        std::vector<m::Manifold> pieces;
        std::vector<CellPieceInfo> info;
        BuildCellFracture(local, region, originalId, fracture, mainSeed, nullptr, pieces, info);
        if (pieces.empty()) {
            HostLog(host, "[ProjectDamage] Cell Fracture could not split a glass fill region; kept its original volume");
            pieces.push_back(region);
        }
        ++regions;
        for (const auto& piece : pieces) {
            if (piece.Status() != m::Manifold::Error::NoError || piece.Volume() <= dust) continue;
            AppendGlassCell(mesh, piece.GetMeshGL(), originalId, baseId, cutId);
            ++count;
        }
    }
    HostLog(host, "[ProjectDamage] Glass Fill Hole: Cell Fracture produced " + std::to_string(count) +
        " shards in " + std::to_string(regions) + " restored regions");
}

// основной вход: собираем геометрию по переданным настройкам
CoreResult BuildWornCore(const CoreInput& inRaw, const ProjectDamageParams& paramsRaw,
	const ProjectDamageHost& host, ProjectDamageDamageCache* cache, UInt64 cacheFingerprint)
{
    project_damage::CoreProfileScope profileScope(paramsRaw.profileCore);
    if(cache) {
        cache->incrementalBuilt.clear();cache->incrementalReused.clear();
        if(paramsRaw.incrementalKeys.empty()) cache->incremental.reset();
    }
    DrawStrokeMask drawMask;
    CoreInput drawIn;
    ProjectDamageParams drawParams;
    std::vector<double> drawHeat;
    const Bool drawOn = PrepareDrawMask(inRaw, paramsRaw, drawMask, drawIn, drawParams, drawHeat);
    CoreInput in = drawOn ? drawIn : inRaw;
    const ProjectDamageParams& params = drawOn ? drawParams : paramsRaw;
    SurfaceGuidance surfaceGuide;
    if (params.surfaceGuide) surfaceGuide.Bind(in.view);
	CoreResult out;
	HeatField heat;
	RustField rust;
	auto passthrough = [&]() -> CoreResult
	{
		if (params.deform.enable)
		{
			// impact deformation only: the dented input itself is the result
			// (same topology; source polygons and corner UVs carried over)
			out.points.assign(in.view.pts, in.view.pts + in.view.npts);
			out.polys.assign(in.view.polys, in.view.polys + in.view.npolys);
			out.basePolyOf.resize((size_t)in.view.npolys);
			for (Int32 i = 0; i < in.view.npolys; ++i) out.basePolyOf[(size_t)i] = i;
			const bool uv = in.cornerUvs != nullptr && in.cornerUvs->size() >= (size_t)in.view.npolys * 8;
			m::MeshGL mesh;
			mesh.numProp = 5;
			for (Int32 i = 0; i < in.view.npolys; ++i)
			{
				const ProjectDamagePoly& q = in.view.polys[i];
				const Int32 ids[4] = { q.a, q.b, q.c, q.d };
				for (int k = 0; k < 4; ++k)
				{
					if (k == 3 && q.c == q.d) { out.propCorner.push_back(NOTOK); continue; }
					const EVec& p3 = in.view.pts[ids[k]];
					const double u = uv ? (*in.cornerUvs)[(size_t)i * 8 + (size_t)k * 2] : 0.0;
					const double v = uv ? (*in.cornerUvs)[(size_t)i * 8 + (size_t)k * 2 + 1] : 0.0;
					out.propCorner.push_back((Int32)mesh.NumVert());
					mesh.vertProperties.insert(mesh.vertProperties.end(), { (float)p3.x, (float)p3.y, (float)p3.z, (float)u, (float)v });
				}
			}
			out.mesh = std::move(mesh);
			out.inputValid = true;
			return out;
		}
		out.passthrough = true;
		if (rust.valid)
		{
			std::vector<EVec> source(in.view.pts, in.view.pts + in.view.npts);
			EvaluateRustAttributes(in, params, rust, source, out.rustPoint, out.rustStreakPoint);
		}
		if (heat.valid) {
			std::vector<EVec> source(in.view.pts, in.view.pts + in.view.npts);
			EvaluateHeatAttributes(in, params, heat, source, out.heatExposure, out.heatCharAmount, out.heatCharDepth);
		}
		return out;
	};
	HostStatus(host, "reading mesh (" + std::to_string(in.view.npolys) + " polys)", 5);
	Int32 baseId = in.baseId;
	gSmoothBreakIds.clear();
	gWallHoleOut = WallHoleOut();
	// Fabric works on the surface itself (fabric_flat.cpp) - no closed solid
	// needed; the old solid (Boolean) tears are retired
	if (params.fabric.enable && !params.fabric.solid)
	{
		if (BuildFlatFabric(in, params, host, out)) return out;
		return passthrough();
	}
	const m::Manifold& box = *in.box;
	if (box.Status() != m::Manifold::Error::NoError || box.NumTri() == 0)
	{
        if(params.buildCoverage>0 || params.buildCornerProb>0 || params.depositAmount>0) {
            auto normals=PolyNormals(in.view.pts,in.view.polys,in.view.npolys);
            std::vector<ExposedEdge> exposed;
            DetectExposed(in.view,normals,params.thresholdDeg,exposed,host);
            std::vector<Chain> chains;GroupObjectEdges(exposed,in.view.pts,params.thresholdDeg,chains);
            if(params.avoidBuild) chains=SplitChainsByAvoid(chains,in.avoidPoly,in.view.pts);
            auto append=[](const std::vector<m::Manifold>& solids,auto& points,auto& polys) {
                for(const auto& solid:solids) {
                    if(solid.Status()!=m::Manifold::Error::NoError)continue;
                    const auto mesh=solid.GetMeshGL();const Int32 offset=Int32(points.size());
                    for(size_t i=0;i<mesh.NumVert();++i)points.emplace_back(mesh.vertProperties[i*mesh.numProp],mesh.vertProperties[i*mesh.numProp+1],mesh.vertProperties[i*mesh.numProp+2]);
                    for(size_t i=0;i<mesh.triVerts.size();i+=3)polys.push_back({offset+Int32(mesh.triVerts[i]),offset+Int32(mesh.triVerts[i+1]),offset+Int32(mesh.triVerts[i+2]),offset+Int32(mesh.triVerts[i+2])});
                }
            };
            append(PlaceBuildup(chains,in.view.pts,in.view.polys,in.view.npolys,normals,params,true,false),out.beadPoints,out.beadPolys);
            append(PlaceBuildup(chains,in.view.pts,in.view.polys,in.view.npolys,normals,params,false,true),out.pilePoints,out.pilePolys);
            if(params.depositSeparate)append(PlaceSurfaceDeposits(in,params,normals),out.depositPoints,out.depositPolys);
        }
        HostLog(host, "[ProjectDamage] open input: surface additions are separate; volume cuts require a closed mesh");
		return passthrough();
	}
    m::Manifold plastered;
    std::vector<uint8_t> plasterFaces;
    const bool plasterOn = CachedBaseStage(cache,params,35,plastered,baseId,[&]{return params.plaster.enable && ApplyPlasterDisplacement(in, params.plaster, params.seed, params.mask, plastered, &plasterFaces, params.plasterZone);},&plasterFaces);
    const m::Manifold& basePlaster = plasterOn ? plastered : box;
    // Concrete Pitting: pores, dents and flaked skin displaced into the
    // (plastered) base, before anything is cut
    m::Manifold pitted;
    std::string pitsLog;
    const bool pitsOn = CachedBaseStage(cache,params,36,pitted,baseId,[&]{return params.pits.enable && ApplyConcretePits(in, params.pits, params.seed, params.mask, basePlaster, pitted, &pitsLog);},nullptr,nullptr,&pitsLog);
    if (!pitsLog.empty()) HostLog(host, pitsLog);
    const m::Manifold& base0 = pitsOn ? pitted : basePlaster;
    m::Manifold dented;
    std::shared_ptr<const MetalDeformMarks> metalMarks;
    const bool metalOn = CachedBaseStage(cache,params,37,dented,baseId,[&]{return params.metal.enable && ApplyMetalDeform(in, params.metal, params.seed, params.mask, base0, dented, metalMarks);},nullptr,&metalMarks);
    const m::Manifold& preTear = metalOn ? dented : base0;
    m::Manifold torn;
    std::string tearLog;
    const bool tearOn = CachedBaseStage(cache,params,38,torn,baseId,[&]{return params.metalTear.enable && ApplyMetalTearDent(in,params.metalTear,params.seed,preTear,torn,&tearLog);},nullptr,nullptr,&tearLog);
    if (!tearLog.empty()) HostLog(host,tearLog);
    const m::Manifold& preClay = tearOn ? torn : preTear;
    m::Manifold clayed; std::string clayLog;
    const bool clayOn=CachedBaseStage(cache,params,39,clayed,baseId,[&]{return params.clay.enable && ApplyClay(in,params.clay,params.seed,params.mask,preClay,clayed,&clayLog);},nullptr,nullptr,&clayLog);
    if(!clayLog.empty()) HostLog(host,clayLog);
    const m::Manifold& unmerged=clayOn ? clayed : preClay;
    m::Manifold mergedShells;
    if(!params.breakers.empty() && !params.preserveSurfaceShells)
        mergedShells=MergeOverlappingShells(unmerged);
    const m::Manifold& base=mergedShells.NumTri()!=0?mergedShells:unmerged;
    EdgeWearField wearField;
    std::vector<ScratchLine> scratchLines;
    std::vector<m::Manifold> lathPlanks, concreteBars, separateBricks, brickMortar;
    std::vector<m::Box> brickZones;

	out.inputValid = true;
	heat = ComputeHeatField(in, params);
	rust = ComputeRustField(in, params);
	if (rust.valid) out.rustSource.assign(rust.R.begin(), rust.R.end());
	std::unordered_map<Int32, Int32> idToClass;
    std::unordered_map<Int32, Int32> effectOwners;

	std::vector<m::Manifold> cutters;
	std::vector<Int32> cutterClass;
	std::vector<Int32> cutterId;
	std::vector<m::Manifold> buildup;
	std::vector<Int32> buildId;

	std::vector<m::Manifold> separateBeads, separatePiles;
	const Bool cacheLive = params.buildCoverage<=0 && params.buildCornerProb<=0 && cache != nullptr && params.incrementalKeys.empty() && !params.breakers.empty();
	if (cache != nullptr && !cacheLive)
		DamageCacheClear(cache);
	const Bool placed = cacheLive
		&& DamageCacheTake(cache, cacheFingerprint, cutters, cutterClass, cutterId,
			buildup, buildId);
	if (!placed)
	{
		PlaceDamage(in, params, host, cutters, cutterClass, cutterId, buildup, buildId, separateBeads, separatePiles, heat, rust, wearField, scratchLines, lathPlanks, concreteBars, separateBricks, brickMortar, brickZones, effectOwners, cache);
		if (cacheLive)
			DamageCacheStore(cache, cacheFingerprint, cutters, cutterClass, cutterId,
				buildup, buildId);
	}
    if (cache && !placed) { cache->effectOwners = effectOwners; cache->effectsReady = true; }
    if (cache && placed) effectOwners = cache->effectOwners;
	if (placed) {
        if(params.concrete.enable && params.concrete.rebarEnable) {
            std::vector<m::Manifold> cuts, cracks;
            PlaceConcreteDamage(in, params.concrete, params.seed, params.mask, in.concreteStrokes, cuts, cracks, concreteBars);
        }
        if(params.brick.enable && params.brick.brickMode==1 && params.brick.style!=2) {
            BrickDamageOut bd;
            PlaceBrickDamage(in,params.brick,params.seed,params.mask,in.brickPaint,bd);
            separateBricks=std::move(bd.bricks);brickMortar=std::move(bd.mortar);brickZones=std::move(bd.zones);
        }
        if(params.lath.enable) {
            LathDamageOut ld;
            PlaceLathDamage(in,params.lath,params.seed,params.mask,in.lathPaint,ld);
            lathPlanks=std::move(ld.planks);
        }
        std::vector<m::Manifold> a,b;
        if(params.bevel.enable) PlaceEdgeBevel(in,params.bevel,params.seed,params.mask,in.bevelPaint,a,b,&wearField);
        if(params.damagedBevel.enable) PlaceDamagedBevel(in,params.damagedBevel,params.seed,params.mask,in.damagedBevelPaint,nullptr,nullptr,&wearField,nullptr);   // damaged_bevel: cache hit, chamfer field only
        a.clear(); b.clear();
        if(params.scratch.enable) PlaceScratches(in,params.scratch,params.seed,params.mask,in.scratchStrokes,a,b,&scratchLines);
    }
    for (size_t i = 0; i < cutterId.size(); i++)
		idToClass[cutterId[i]] = cutterClass[i];
	for (size_t i = 0; i < buildId.size(); i++)
		idToClass[buildId[i]] = (Int32)DamageClass::Buildup;
    if (params.wallpaper.enable) {
        const WallpaperFaceIds ids = WallpaperIds();
        idToClass[ids.front] = (Int32)DamageClass::WallpaperFront;
        idToClass[ids.back] = (Int32)DamageClass::WallpaperBack;
        idToClass[ids.wall] = (Int32)DamageClass::WallpaperWall;
        // Hanging flaps: same material classes, own ids for wallpaperFlapPolys.
        idToClass[ids.flapFront] = (Int32)DamageClass::WallpaperFront;
        idToClass[ids.flapBack] = (Int32)DamageClass::WallpaperBack;
    }

	std::vector<m::Manifold> breakSolids, rebarSolids;
	if (!params.breakers.empty())
		ResolveBreakSolids(in, params, host, cache, idToClass, breakSolids, rebarSolids);
    // Auto Setup: reuse the already generated cut volumes, including damage
    // from other assets. This list is used only for reinforcement placement;
    // the damaged mesh and its materials follow the original Boolean path.
    if (params.breakRebar.enable && params.breakRebar.existingDamage) {
        rebarSolids = breakSolids;
        rebarSolids.insert(rebarSolids.end(), cutters.begin(), cutters.end());
    }
    // Separate brick layer: preserve islands; Boolean cutters affect wood and bricks alike.
    if (!separateBricks.empty() || !brickMortar.empty())
        HostStatus(host, "collecting separate bricks and mortar", 39);
    for(auto& brick:separateBricks) {
        for(const auto& cut:breakSolids) brick=brick-cut;
        if(brick.Status()!=m::Manifold::Error::NoError) throw std::runtime_error("separate brick Boolean failed");
    }
    BrickDamageMesh brickMesh;
    for(auto& mortar:brickMortar) {
        for(const auto& cut:breakSolids) mortar=mortar-cut;
        if(mortar.Status()!=m::Manifold::Error::NoError) throw std::runtime_error("brick mortar Boolean failed");
    }
    BrickDamageBricksMesh(separateBricks,brickMesh,&brickMortar);
    out.brickMeshPoints=std::move(brickMesh.points);out.brickMeshPolys=std::move(brickMesh.polys);
    out.brickPiece=std::move(brickMesh.polyBrick);out.brickCount=brickMesh.bricks;
    // Preserve one closed island per plank piece; never union the wood into plaster.
    for(size_t piece=0;piece<lathPlanks.size();++piece) {
        auto plank=lathPlanks[piece];
        for(const auto& cut:breakSolids) plank=plank-cut;
        if(plank.Status()!=m::Manifold::Error::NoError) throw std::runtime_error("lath plank Boolean failed");
        auto pm=plank.GetMeshGL();const Int32 offset=(Int32)out.lathPoints.size();
        for(size_t v=0;v<pm.NumVert();++v) out.lathPoints.emplace_back(pm.vertProperties[v*pm.numProp],pm.vertProperties[v*pm.numProp+1],pm.vertProperties[v*pm.numProp+2]);
        for(size_t t=0;t<pm.triVerts.size();t+=3) {
            Int32 a=offset+pm.triVerts[t],b=offset+pm.triVerts[t+1],c=offset+pm.triVerts[t+2];
            out.lathPolys.push_back({a,b,c,c});out.lathPiece.push_back((Int32)piece);
        }
    }
    // Concrete reinforcement is independent output: retain complete closed bars,
    // including embedded ends, rather than unioning them into the damaged wall.
    for(const auto& bar:concreteBars) {
        auto mg=bar.GetMeshGL();const Int32 off=(Int32)out.concreteRebarPoints.size();
        for(size_t v=0;v<mg.NumVert();++v)
            out.concreteRebarPoints.emplace_back(mg.vertProperties[v*mg.numProp],mg.vertProperties[v*mg.numProp+1],mg.vertProperties[v*mg.numProp+2]);
        for(size_t t=0;t<mg.triVerts.size();t+=3) {
            Int32 a=off+mg.triVerts[t],b=off+mg.triVerts[t+1],c=off+mg.triVerts[t+2];
            out.concreteRebarPolys.push_back({a,b,c,c});
        }
    }
    // Wood Framing fill: built with the hollowing (below), placed by buildRebar
    const bool framing = params.breakRebar.enable && params.breakRebar.material == BREAK_FILL_FRAMING;
    const bool chips = params.breakRebar.enable && params.breakRebar.material == BREAK_FILL_CHIPS;
    BreakRebarMesh framingMesh;
    bool framingBuilt = false;
    auto buildRebar = [&](const m::Manifold& left) {
      if(params.breakRebar.enable && params.breakRebar.generatedGeometry && (!rebarSolids.empty() || !params.breakRebar.strokeCount.empty() || params.breakRebar.showAll)) {
        BreakRebarMesh rb;
        if (framing) {
          if (!framingBuilt) BuildBreakFraming(base, rebarSolids, params.breakRebar, params.seed, framingMesh, nullptr);
          rb = std::move(framingMesh);
          out.rebarGrain = std::move(rb.grain);
        } else if (chips) {
          if (!framingBuilt) BuildBreakChips(base, rebarSolids, params.breakRebar, params.seed, framingMesh, nullptr);
          rb = std::move(framingMesh);
          out.rebarGrain = std::move(rb.grain);
        } else
        {
          auto settings=params.breakRebar;if(settings.material==BREAK_FILL_LATHS)settings.material=BREAK_FILL_PLANKS;
          BuildBreakRebar(base,rebarSolids,settings,params.seed,rb,&left,in.view.guideField);
        }
        out.rebarPoints=std::move(rb.points);out.rebarPolys=std::move(rb.polys);
        out.rebarPolyBar=std::move(rb.polyBar);out.rebarPolyPart=std::move(rb.polyPart);out.rebarExposure=std::move(rb.exposure);out.rebarBars=rb.bars;
        out.rebarBrickFrame=std::move(rb.brickFrame);out.rebarBrickKey=std::move(rb.brickKey);out.rebarBrickCut=std::move(rb.brickCut);
      }
      out.wallHoleRimFrame = gWallHoleOut.rimFrame; out.wallHoleRimCut = gWallHoleOut.rimCut; out.wallHoleRimKey = gWallHoleOut.rimKey;
      out.wallHoleRubbleFrame = gWallHoleOut.rubbleFrame; out.wallHoleRubbleKey = gWallHoleOut.rubbleKey;
      // hanging bars and cables of the hang holes, with the break rebar
      if (params.hang.enable) {
        std::vector<m::Manifold> tubes;
        std::vector<Int32> tubeKinds;
        BuildHangHoleTubes(in, params.hang, params.seed, base, breakSolids, tubes, &tubeKinds);
        out.rebarPolyPart.resize(out.rebarPolys.size(), 0);
        for (size_t tubeIndex=0; tubeIndex<tubes.size(); ++tubeIndex) {
          const m::Manifold& tb=tubes[tubeIndex];
          const m::MeshGL mg = tb.GetMeshGL();
          const Int32 off = (Int32)out.rebarPoints.size();
          for (size_t v = 0; v < mg.NumVert(); ++v) {
            out.rebarPoints.emplace_back(mg.vertProperties[v*mg.numProp], mg.vertProperties[v*mg.numProp+1], mg.vertProperties[v*mg.numProp+2]);
            out.rebarExposure.push_back(1.0f);
          }
          for (size_t t = 0; t < mg.triVerts.size(); t += 3) {
            const Int32 a = off + (Int32)mg.triVerts[t], b = off + (Int32)mg.triVerts[t+1], c = off + (Int32)mg.triVerts[t+2];
            out.rebarPolys.push_back({a, b, c, c});
            out.rebarPolyBar.push_back(out.rebarBars);
            out.rebarPolyPart.push_back(tubeKinds[tubeIndex] == 1 ? 2 : 0);
          }
          out.rebarBars++;
        }
        if (!out.rebarPolyPart.empty()) out.rebarPolyPart.resize(out.rebarPolys.size(), 0);
        if (!out.rebarGrain.empty()) out.rebarGrain.resize(out.rebarPoints.size() * 3, 0.0f);
      }
    };
    auto exportAdditions = [&](const std::vector<m::Manifold>& solids, auto& points, auto& polygons) {
        for(auto solid:solids) {
            for(const auto& cutter:breakSolids) solid=solid-cutter;
            if(solid.Status()!=m::Manifold::Error::NoError) continue;
            const auto mesh=solid.GetMeshGL();const Int32 offset=Int32(points.size());
            for(size_t j=0;j<mesh.NumVert();++j) points.emplace_back(mesh.vertProperties[j*mesh.numProp],mesh.vertProperties[j*mesh.numProp+1],mesh.vertProperties[j*mesh.numProp+2]);
            for(size_t j=0;j<mesh.triVerts.size();j+=3) polygons.push_back({offset+Int32(mesh.triVerts[j]),offset+Int32(mesh.triVerts[j+1]),offset+Int32(mesh.triVerts[j+2]),offset+Int32(mesh.triVerts[j+2])});
        }
    };
    exportAdditions(separateBeads,out.beadPoints,out.beadPolys);
    exportAdditions(separatePiles,out.pilePoints,out.pilePolys);
	if (params.depositAmount > 0 && params.depositSeparate)
	{
		auto normals = PolyNormals(in.view.pts, in.view.polys, in.view.npolys);
		for(Int32 i=0;i<in.view.npolys;++i) normals[i]=GuidedNormal(in.view,i,normals[i]);
		auto solids = PlaceSurfaceDeposits(in, params, normals);
		m::Manifold deposits;
		for (const auto& s : solids) deposits = deposits + s;
		for (const auto& cutter : breakSolids) deposits = deposits - cutter;
		if (deposits.Status() != m::Manifold::Error::NoError)
			throw std::runtime_error("deposits: separate output Boolean failed");
		const auto dm = deposits.GetMeshGL();
		for (size_t j = 0; j < dm.NumVert(); ++j)
			out.depositPoints.emplace_back(dm.vertProperties[j*dm.numProp],
				dm.vertProperties[j*dm.numProp+1],dm.vertProperties[j*dm.numProp+2]);
		for (size_t j = 0; j < dm.triVerts.size(); j+=3)
			out.depositPolys.push_back({Int32(dm.triVerts[j]),Int32(dm.triVerts[j+1]),
				Int32(dm.triVerts[j+2]),Int32(dm.triVerts[j+2])});
	}
	if (cutterId.empty() && buildId.empty() && breakSolids.empty() && !plasterOn && !pitsOn && !metalOn && !clayOn && !tearOn && !params.wallpaper.enable
		&& !params.rbf.enable && !params.cellf.enable)   // Rigid Body / Cell Fracture work on the source itself
	{
        buildRebar(base);
		return passthrough();
    }

	HostStatus(host, "cutting " + std::to_string(cutterId.size() + breakSolids.size())
		+ " cutters", 60);
	m::Manifold result = BuildOrServeDamagedMesh(base, cutters, buildup, params, host, cache,
		cacheFingerprint, idToClass, baseId, effectOwners);
	for (const m::Manifold& cu : breakSolids)
		result = result - cu;

	if (!breakSolids.empty())
		result = RunSecondaryPass(std::move(result), baseId, idToClass, params,
			host, true, effectOwners);

	const bool fractCut = std::find(cutterClass.begin(), cutterClass.end(), (Int32)DamageClass::Fracture1) != cutterClass.end()
        || std::find(cutterClass.begin(), cutterClass.end(), (Int32)DamageClass::Fracture2) != cutterClass.end()
        || std::find(cutterClass.begin(), cutterClass.end(), (Int32)DamageClass::Fracture3) != cutterClass.end();
    // Fracture Physics keeps its chunks like Through (same Min Piece)
    bool fractThrough = (params.fractThrough || params.fractPhysics) && fractCut;
    bool fractureCleanup = false;
    for(const auto& layer:params.fractureLayers) {
        fractThrough |= (layer.fractThrough || layer.fractPhysics) && fractCut;
        fractureCleanup |= !layer.fractPaintOnly && layer.fractRecipe>=2 &&
            (layer.fract[0].amount>0 || layer.fract[1].amount>0 || layer.fract[2].amount>0 ||
             layer.fractType==1 || layer.fractType==2 || !layer.fractProximity.empty());
    }
    // Through variants keep their pieces: the cleanup threshold is their own
    // Min Piece (smallest of the active ones), not the 2% that only removes
    // films and fallen crumbs.
    const bool glassThrough = params.glass.enable && params.glass.through;
    const bool hitThrough = params.hit.enable && params.hit.through;
    double throughKeep = 1e300;
    if (fractThrough) throughKeep = std::min(throughKeep, MaxD(0.0, params.fractThroughMinPiece));
    for(const auto& layer:params.fractureLayers)
        if(layer.fractThrough || layer.fractPhysics) throughKeep=std::min(throughKeep,MaxD(0.,layer.fractThroughMinPiece));
    if (glassThrough) throughKeep = std::min(throughKeep, MaxD(0.0, params.glass.throughMinPiece));
    if (hitThrough) throughKeep = std::min(throughKeep, MaxD(0.0, params.hit.throughMinPiece));
	if (fractureCleanup || fractThrough || hitThrough || params.glass.enable || params.rusty.enable || (rust.valid && params.rustHoleStart < 1.4 && std::find(cutterClass.begin(), cutterClass.end(), (Int32)DamageClass::Rust) != cutterClass.end()) || (!params.fractPaintOnly && params.fractRecipe >= 2
		&& (params.fract[0].amount > 0 || params.fract[1].amount > 0 || params.fract[2].amount > 0
			|| params.fractType == 1 || params.fractType == 2
			|| ((params.fractProxMode == 2 || params.fractProxMode == 3) && !params.fractProximity.empty()))))
	{
		const int dropped = DropFallenFragments(result, base, throughKeep < 1e299 ? throughKeep : 0.02);
		if (dropped > 0)
			HostLog(host, "[ProjectDamage] cleanup: " + std::to_string(dropped) + " fallen fragments removed");
	}
    // Damage Debris: source - result, after the cleanup (fallen fragments
    // become debris too) and before any remesh or added rebar.
    if (params.debris.enable) {
        HostStatus(host, "damage debris", 85);
        std::vector<m::Manifold> chunks;
        ExtractDamageDebris(base, result, params.debris, params.seed, chunks);
        DebrisMesh dmesh;
        BuildDebrisMesh(chunks, (uint32_t)baseId, in.view, params.debris, dmesh);
        out.debrisPoints = std::move(dmesh.points);
        out.debrisPolys = std::move(dmesh.polys);
        out.debrisPiece = std::move(dmesh.piece);
        out.debrisBasePoly = std::move(dmesh.basePoly);
        out.debrisUvs = std::move(dmesh.uvs);
        out.debrisNormals = std::move(dmesh.normals);
        out.debrisSourceUvs = dmesh.sourceUvs;
        if (!chunks.empty())
            HostLog(host, "[ProjectDamage] debris: " + std::to_string(chunks.size()) + " chunks");
    }
    // Rigid Body Fracture: the (undamaged) source cut into convex pieces with
    // the source's materials, UVs and smooth normals on their outer faces.
    if (params.rbf.enable) {
        HostStatus(host, "rigid body fracture", 88);
        std::vector<m::Manifold> rbPieces;
        std::vector<RBPieceInfo> rbInfo;
        BuildRigidBodyFracture(in, base, (uint32_t)baseId, params.rbf, params.seed,
            in.rbfCutStrokes, in.rbfImpactStrokes, rbPieces, rbInfo);
        DamageDebrisParams look;
        look.innerUvScale = params.rbf.innerUvScale;
        look.smoothAngle = params.rbf.smoothAngle;
        DebrisMesh rm;
        BuildDebrisMesh(rbPieces, (uint32_t)baseId, in.view, look, rm);
        out.rbfPoints = std::move(rm.points); out.rbfPolys = std::move(rm.polys);
        out.rbfPiece = std::move(rm.piece); out.rbfBasePoly = std::move(rm.basePoly);
        out.rbfUvs = std::move(rm.uvs); out.rbfNormals = std::move(rm.normals);
        for (const RBPieceInfo& r : rbInfo) {
            out.rbfPieceVolume.push_back(r.volume);
            out.rbfPieceCenter.push_back(r.center.x); out.rbfPieceCenter.push_back(r.center.y); out.rbfPieceCenter.push_back(r.center.z);
        }
        HostLog(host, "[ProjectDamage] rigid body fracture: " + std::to_string(rbPieces.size()) + " pieces");
    }
    // Cell Fracture: the (undamaged) source split into solid pieces with rough
    // material break faces; outer faces keep the source's materials and UVs.
    if (params.cellf.enable) {
        HostStatus(host, "cell fracture", 89);
        std::vector<m::Manifold> cfPieces;
        std::vector<CellPieceInfo> cfInfo;
        std::string cfLog;
        DebrisMesh cm;
        const UInt64 cellKey=params.incrementalKeys.size()>40?params.incrementalKeys[40]:0;
        if(cache && cellKey && cache->incremental && cache->incremental->stages.count(40)
             && cache->incremental->stages[40].key==cellKey) {
            const auto& e=cache->incremental->stages[40];cm=e.fragments;cfInfo=e.cellInfo;cfLog=e.note;
            cache->incrementalReused.push_back(40);
        } else {
        BuildCellFracture(in, base, (uint32_t)baseId, params.cellf, params.seed,
            in.cellfStrokes, cfPieces, cfInfo, &cfLog);
        DamageDebrisParams look;
        look.innerUvScale = params.cellf.innerUvScale;
        look.smoothAngle = params.cellf.smoothAngle;
        BuildDebrisMesh(cfPieces, (uint32_t)baseId, in.view, look, cm);
            if(cache && cellKey) {
                if(!cache->incremental)cache->incremental=std::make_shared<IncrementalDamageCache>();
                IncrementalDamageStage e;e.key=cellKey;e.fragments=cm;e.cellInfo=cfInfo;e.note=cfLog;
                cache->incremental->stages[40]=std::move(e);cache->incrementalBuilt.push_back(40);
            }
        }
        out.cellfPoints = std::move(cm.points); out.cellfPolys = std::move(cm.polys);
        out.cellfPiece = std::move(cm.piece); out.cellfBasePoly = std::move(cm.basePoly);
        out.cellfUvs = std::move(cm.uvs); out.cellfNormals = std::move(cm.normals);
        for (const CellPieceInfo& r : cfInfo) {
            out.cellfPieceVolume.push_back(r.volume);
            out.cellfPieceCenter.push_back(r.center.x); out.cellfPieceCenter.push_back(r.center.y); out.cellfPieceCenter.push_back(r.center.z);
        }
        HostLog(host, "[ProjectDamage] " + cfLog);
    }
    // Brick back surface texture: on the FINISHED faces the "behind" cutters
    // left (the back leaf / collar / sockets seen through the knocked-out
    // bricks) - their rim stays put, so the mesh stays closed
    if (params.brick.enable && ((params.brick.surfMode == 1 && params.brick.texStrength > 0.0) || (params.brick.surfMode == 0 && params.brick.noiseStrength > 0.0)))
    {
        std::vector<Int32> backIds;
        for (size_t i = 0; i < cutterId.size() && i < cutterClass.size(); ++i)
            if (cutterClass[i] == (Int32)DamageClass::BrickBehind)
                backIds.push_back(cutterId[i]);
        if (!backIds.empty())
        {
            const BrickDamageParams& bp = params.brick;
            TexSpec t;
            t.path = bp.texPath; t.scale = bp.texScale; t.depth = bp.texStrength;
            t.offX = bp.texOffX; t.offY = bp.texOffY; t.offZ = bp.texOffZ; t.edge = bp.texEdge;
            if (bp.surfMode == 0) {   // Procedural back surface (surface_noise): noise for the image
            	t.proc = true; t.scale = bp.noiseScale; t.depth = bp.noiseStrength; t.offX = t.offY = t.offZ = 0.0;
            	t.detail = bp.noiseDetail; t.roughness = bp.noiseRoughness; t.distortion = bp.noiseDistortion;
            }
            HostStatus(host, "texturing the area behind the bricks", 86);
            std::string msg;
            result = TextureDisplaceClassFaces(result, backIds, t, &msg);
            if (!msg.empty()) HostLog(host, msg);
        }
    }
    if(params.brick.enable && params.brick.remesh && !brickZones.empty()) {
        HostStatus(host, "remeshing brick damage zones", 86);
        result=BrickDamageRemesh(result,brickZones,params.brick.remeshSize*params.brick.brickScale);
    }
    // Wood Framing: the wall is a drywall shell around a stud frame - hollow
    // it behind the skin around the breaks and tear the face paper off around
    // each hole, so the frame (built here, placed by buildRebar) shows
    if (framing && !params.breakRebar.existingDamage && !rebarSolids.empty()) {
        HostStatus(host, "framing", 87);
        m::Manifold carve;
        BuildBreakFraming(base, rebarSolids, params.breakRebar, params.seed, framingMesh, &carve);
        framingBuilt = true;
        if (carve.NumTri() != 0) {
            carve = carve.AsOriginal();
            idToClass[carve.OriginalID()] = (Int32)DamageClass::Break;
            m::Manifold hollow = result - carve;
            if (hollow.Status() == m::Manifold::Error::NoError) {
                // skin crumbs only the solid behind them held
                const int dropped = DropFallenFragments(hollow, base, 0.0005);
                if (dropped > 0)
                    HostLog(host, "[ProjectDamage] framing: " + std::to_string(dropped) + " loose skin pieces removed");
                result = std::move(hollow);
            }
        }
    }
    // Wooden Chips: tear the plywood plies around the breaks; the splinters
    // (built here, placed by buildRebar) stay attached at the ends of the tears
    if (chips && !params.breakRebar.existingDamage && !rebarSolids.empty()) {
        HostStatus(host, "tearing plies", 87);
        m::Manifold carve;
        BuildBreakChips(base, rebarSolids, params.breakRebar, params.seed, framingMesh, &carve);
        framingBuilt = true;
        if (carve.NumTri() != 0) {
            carve = carve.AsOriginal();
            idToClass[carve.OriginalID()] = (Int32)DamageClass::Break;
            m::Manifold torn = result - carve;
            if (torn.Status() == m::Manifold::Error::NoError) {
                const int dropped = DropFallenFragments(torn, base, 0.0005);
                if (dropped > 0)
                    HostLog(host, "[ProjectDamage] chips: " + std::to_string(dropped) + " loose pieces removed");
                result = std::move(torn);
            }
        }
    }
    buildRebar(result);
    if(params.fractPaintOnly || std::any_of(params.fractureLayers.begin(),params.fractureLayers.end(),
        [](const ProjectDamageParams& layer){return layer.fractPaintOnly;}))
    {
        const auto box=result.BoundingBox();
        const double magnitude=std::max({std::abs(box.min.x),std::abs(box.min.y),std::abs(box.min.z),
            std::abs(box.max.x),std::abs(box.max.y),std::abs(box.max.z),1.});
        const double tolerance=magnitude*std::numeric_limits<float>::epsilon()*32.;
        auto clean=result.Simplify(tolerance);
        for(int pass=0;pass<8;++pass)
        {
            auto rounded=clean.GetMeshGL();
            for(size_t i=0;i<rounded.vertProperties.size();i+=rounded.numProp)
                for(size_t axis=0;axis<3;++axis)
                    rounded.vertProperties[i+axis]=float(std::round(double(rounded.vertProperties[i+axis])/tolerance)*tolerance);
            rounded.tolerance=tolerance;
            m::Manifold repaired(rounded);
            if(repaired.Status()!=m::Manifold::Error::NoError||repaired.IsEmpty())break;
            clean=repaired.Simplify(tolerance);
        }
        if(clean.Status()==m::Manifold::Error::NoError&&!clean.IsEmpty())result=std::move(clean);
    }
	m::MeshGL mesh = result.GetMeshGL();
	if (params.rusty.enable) {
		std::unordered_map<Int32, m::vec3> rustyAxes;
		RustyCutAxes(cutters, cutterId, effectOwners, 27, rustyAxes);
		RustyOpenFloors(mesh, rustyAxes);
	}
    if (params.glass.enable && params.glass.fillHole) {
        std::vector<m::Manifold> openings;
        Int32 cutId = NOTOK;
        for (size_t i = 0; i < cutters.size(); ++i) {
            const auto owner = effectOwners.find(cutterId[i]);
            if (owner == effectOwners.end() || owner->second != 19 ||
                cutterClass[i] != (Int32)DamageClass::Surface) continue;
            openings.push_back(cutters[i]);
            cutId = cutterId[i];
        }
        if (cutId != NOTOK)
            AppendGlassFill(mesh, box, result, uint32_t(in.baseId), uint32_t(baseId), uint32_t(cutId),
                openings, params.glass, params.seed, host);
    }
	HostStatus(host, "building mesh (" + std::to_string(mesh.NumTri()) + " tris)", 90);
	TopoBuild tb;
    const std::vector<uint8_t> papered = WallpaperPaperedPolys(in, params.wallpaper);
    for (const auto& entry : idToClass) if (!effectOwners.count(entry.first)) {
        const auto cls = (DamageClass)entry.second;
        effectOwners[entry.first] = cls == DamageClass::Corner ? 2 :
            cls == DamageClass::Edge ? 1 : cls == DamageClass::Break ? 23 :
            cls == DamageClass::Buildup ? 5 :
            cls >= DamageClass::Fracture1 && cls <= DamageClass::FractureChip ? 4 :
            cls >= DamageClass::WallpaperFront && cls <= DamageClass::WallpaperWall ? 14 : 3;
    }
	BuildTopology(mesh, baseId, params.topology, params.noiseStrength > 0.0, idToClass,
		in.view.polys, in.view.npolys, params.baseMap, tb, papered.empty() ? nullptr : &papered, &effectOwners, &plasterFaces);

	if (tb.polys.empty())
		return out;

	const Int32 mv = (Int32)mesh.NumVert();
	const Int32 nv = tb.weldedCount > 0 ? tb.weldedCount : mv;
	const Int32 np = (Int32)mesh.numProp;
	out.points.assign((size_t)nv, EVec(0.0));
	for (Int32 i = 0; i < mv; i++)
	{
		const Int32 slot = tb.geoOf.empty() ? i : tb.geoOf[(size_t)i];
		if (slot >= 0 && slot < nv)
			out.points[(size_t)slot] = EVec(mesh.vertProperties[i * np + 0],
											mesh.vertProperties[i * np + 1],
											mesh.vertProperties[i * np + 2]);
	}
	if(metalOn) EvaluateMetalDeformAttributes(metalMarks.get(),out.points,out.metalDent,out.metalCrease,out.metalContact);
    if(params.bevel.enable) out.edgeWear = EdgeWearMask(wearField,out.points.data(),out.points.size());
    if(params.damagedBevel.enable) {   // damaged_bevel
        if(!params.bevel.enable) out.edgeWear = EdgeWearMask(wearField,out.points.data(),out.points.size());
        EdgeWearField dbField;
        PlaceDamagedBevel(in,params.damagedBevel,params.seed,params.mask,in.damagedBevelPaint,nullptr,nullptr,nullptr,&dbField);
        out.bevelDamage = EdgeWearMask(dbField,out.points.data(),out.points.size());
        if(params.damagedBevel.exportSegments) DamagedBevelSegments(in,params.damagedBevel,params.seed,params.mask,in.damagedBevelPaint,out.bevelSegPts,out.bevelSegTris);   // damaged_bevel: Fill Segments
    }
    if(params.scratch.enable) {
        std::vector<double> weights;
        ScratchMaskWeights(scratchLines,out.points.data(),(int32_t)out.points.size(),([](ScratchDrawParams p){ ScratchDrawLook(p,p.style); return p.maskFalloff; })(params.scratch),weights);
        out.scratchMask.assign(weights.begin(),weights.end());
    }
    out.polys = std::move(tb.polys);
	if (np >= 5 && tb.propCorner.size() == out.polys.size() * 4)
		out.propCorner = std::move(tb.propCorner);
	if (tb.basePolyOf.size() == out.polys.size())
		out.basePolyOf = std::move(tb.basePolyOf);

	if (params.noiseStrength > 0.0 && !tb.chipTris.empty())
		ApplyNoise(out.points.data(), nv, mesh, tb.chipTris, params, tb.geoOf,
			tb.weldedCount);

	ApplyTileOffset(out.points.data(), nv, out.polys, out.basePolyOf, tb,
		params.avoidFract ? in.avoidPoly : nullptr, params);

	for (Int32 l = 0; l < 3; l++)
		out.crackPolys[l] = std::move(tb.crackPolys[(size_t)l]);
	out.holePolys = std::move(tb.holePolys);
    for (int k = 0; k < 3; ++k) out.wallpaperPolys[k] = std::move(tb.wallpaperPolys[k]);
    out.wallpaperBasePolys = std::move(tb.wallpaperBasePolys);
    out.wallpaperFlapPolys = std::move(tb.wallpaperFlapPolys);
    for (int k = 0; k < 5; ++k) out.brickPolys[k] = std::move(tb.brickPolys[k]);
    for (int k = 0; k < 4; ++k) out.extraPolys[k] = std::move(tb.extraPolys[k]);
	out.buildPolys = std::move(tb.buildPolys);
	out.breakPolys = std::move(tb.breakPolys);
	out.chipPolys = std::move(tb.chipPolys);
    out.effectIds = std::move(tb.effectIds);
	if (params.damageShading && !out.polys.empty())
	{
		// smoothing groups: 1 = every Boolean-cut / added damage surface,
		// 2 = wallpaper sheets, 0 = the original surface (source normals)
		std::vector<unsigned char> group(out.polys.size(), 0);
		auto mark = [&](const std::vector<Int32>& list, unsigned char g) {
			for (Int32 i : list) if (i >= 0 && (size_t)i < group.size()) group[(size_t)i] = g;
		};
		mark(out.chipPolys, 1);
		for (int k = 0; k < 3; ++k) mark(out.crackPolys[k], 1);
		mark(out.holePolys, 1);
		mark(out.buildPolys, 1);
		mark(out.breakPolys, 1);
		for (int k = 0; k < 5; ++k) mark(out.brickPolys[k], 1);
		for (int k = 0; k < 4; ++k) mark(out.extraPolys[k], 1);
		for (int k = 0; k < 3; ++k) mark(out.wallpaperPolys[k], 2);
		// Wood knife facets: keep the ridges between facets crisp
		const double dmgAngle = params.surfType == 1 && params.surfAmount > 0 ? std::min((double)params.damageSmoothAngle, 25.0)
			: (double)params.damageSmoothAngle;
		ComputeDamageShading(out.points, out.polys, group, dmgAngle, params.baseSmoothAngle,
			out.cornerNormals, &in.view, in.srcCornerNormals, &out.basePolyOf);
	}
	EvaluateHeatAttributes(in, params, heat, out.points, out.heatExposure, out.heatCharAmount, out.heatCharDepth);
	if (rust.valid)
		EvaluateRustAttributes(in, params, rust, out.points, out.rustPoint, out.rustStreakPoint);
	out.mesh = std::move(mesh);
	return out;
}

PROJECT_DAMAGE_NS_MACRO_END
