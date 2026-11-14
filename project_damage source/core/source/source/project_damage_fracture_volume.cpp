// разделяем объём по сети трещин
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
// Modified 2026-10-15: cutter rework; painted descendants rooted on the previous generation.
// Modified 2026-10-17: Through Fractures (pieces become islands).
// Modified 2026-10-17: cracks stop at folds (bevels, steps), cutters no longer bridge folds.
// Modified 2026-10-17: Through Fractures close the crack network and split the solid into one
//   cell per crack-bounded region; per-level crack Length; faster DropFallenFragments.

#include "manifold/manifold.h"

#include "project_damage_internal.h"
#include "project_damage_bvh.h"

#include <vector>
#include <unordered_map>
#include <map>
#include <utility>
#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <functional>
#include <atomic>
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN

using namespace project_damage;

namespace
{
constexpr double FR_REF_SQRT_AREA  = 518.0;
constexpr double FR_STEP           = 5.0;
constexpr double FR_SIGMA_DEG      = 5.5;
constexpr double FR_REVERT         = 0.3;
constexpr double FR_KINK_P         = 0.045;
constexpr double FR_KINK_DEG_MIN   = 9.0;
constexpr double FR_KINK_DEG_MAX   = 26.0;
constexpr double FR_START_JIT_DEG  = 28.0;
constexpr double FR_CURVE_TOT_DEG  = 30.0;
constexpr double FR_ATTRACT_R      = 18.0;
constexpr double FR_ATTRACT_W      = 0.55;
constexpr double FR_SNAP_R         = 4.0;
constexpr double FR_CLOSE_R        = 22.0;
constexpr double FR_NORMAL_DOT    = 0.35;
constexpr double FR_CORNER_SCALE   = 28.0;
constexpr double FR_EDGE_SAMPLE    = 8.0;
constexpr double FR_MARGIN         = 4.0;
constexpr double FR_CUT_STEP       = 3.5;
constexpr int    FR_CHAIKIN        = 2;
constexpr double FR_WAVE1          = 6.5;
constexpr double FR_WAVE2          = 2.7;
constexpr double FR_TAPER_FRAC     = 0.18;
constexpr double FR_SPALL_RUN_MIN  = 2.0;
constexpr double FR_SPALL_RUN_MAX  = 4.0;
constexpr double FR_SPALL_ASP_MIN  = 0.35;
constexpr double FR_SPALL_ASP_MAX  = 0.6;
constexpr double FR_SPALL_OFF_MIN  = 0.15;
constexpr double FR_SPALL_OFF_MAX  = 0.55;
constexpr double FR_SPALL_EDGE_BOOST = 0.6;
constexpr double FR_SHATTER_BIAS            = 0.65;
constexpr Int32  FR_ATTEMPT_BUDGET_SHATTER  = 40;
constexpr Int32  FR_ATTEMPT_BUDGET_NORMAL   = 14;
constexpr double FR_FOLD_COS                = 0.819;                                                   
constexpr double FR_FOLD_TOTAL_COS          = 0.342;                                                  
constexpr double FR_CUT_FOLD_COS            = 0.866;                                                          

inline double FrNetScale(double area)
{
	double s = std::sqrt(MaxD(area, 1.0)) / FR_REF_SQRT_AREA;
	if (s < 0.25) s = 0.25;
	if (s > 8.0)  s = 8.0;
	return s;
}

double FrGauss(PyRandom& rng, double sigma)
{
	const double u1 = MaxD(1e-12, rng.Random());
	const double u2 = rng.Random();
	return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * m::kPi * u2) * sigma;
}

inline int FrRandRange(PyRandom& rng, int n)
{
	if (n <= 0)
		return 0;
	int k = (int)(rng.Random() * n);
	return k >= n ? n - 1 : k;
}

inline m::vec3 FrNorm(const m::vec3& v)
{
	const double L = m::la::length(v);
	return L > 1e-12 ? v / L : v;
}

struct FrTriMesh
{
	struct Tri { Int32 v[3]; Int32 nbr[3]; Int32 src; };
	std::vector<m::vec3> verts;
	std::vector<Tri> tris;
	std::vector<m::vec3> nrm;
	std::vector<Int32> srcStart;
	double area = 0.0;
};

void FrBuildTriMesh(MeshView meshIn, FrTriMesh& mesh)
{
	const Int32 pcnt = meshIn.npts;
	const Int32 vcnt = meshIn.npolys;
	const EVec* pts = meshIn.pts;
	const ProjectDamagePoly* polys = meshIn.polys;
	mesh.verts.resize((size_t)pcnt);
	for (Int32 i = 0; i < pcnt; i++)
		mesh.verts[(size_t)i] = pts[i];
	mesh.tris.reserve((size_t)vcnt * 2);
	mesh.srcStart.resize((size_t)vcnt + 1, 0);
	for (Int32 i = 0; i < vcnt; i++)
	{
		mesh.srcStart[(size_t)i] = (Int32)mesh.tris.size();
		const ProjectDamagePoly& cp = polys[i];
		FrTriMesh::Tri t1{ { cp.a, cp.b, cp.c }, { -1, -1, -1 }, i };
		mesh.tris.push_back(t1);
		if (cp.c != cp.d)
		{
			FrTriMesh::Tri t2{ { cp.a, cp.c, cp.d }, { -1, -1, -1 }, i };
			mesh.tris.push_back(t2);
		}
	}
	mesh.srcStart[(size_t)vcnt] = (Int32)mesh.tris.size();
	auto ekey = [](Int32 a, Int32 b) {
		return a < b ? ((UInt64)(UInt32)a << 32) | (UInt32)b
					 : ((UInt64)(UInt32)b << 32) | (UInt32)a;
	};
	std::unordered_map<UInt64, std::pair<Int32, Int32>> emap;
	emap.reserve(mesh.tris.size() * 2);
	for (size_t t = 0; t < mesh.tris.size(); t++)
		for (int e = 0; e < 3; e++)
		{
			const UInt64 k = ekey(mesh.tris[t].v[e], mesh.tris[t].v[(e + 1) % 3]);
			auto ins = emap.emplace(k, std::make_pair((Int32)t, (Int32)NOTOK));
			if (!ins.second && ins.first->second.second == NOTOK)
				ins.first->second.second = (Int32)t;
		}
	for (size_t t = 0; t < mesh.tris.size(); t++)
	{
		for (int e = 0; e < 3; e++)
		{
			auto it = emap.find(ekey(mesh.tris[t].v[e], mesh.tris[t].v[(e + 1) % 3]));
			if (it == emap.end())
				continue;
			const Int32 f = it->second.first, s = it->second.second;
			mesh.tris[t].nbr[e] = f != (Int32)t ? f : s;
		}
	}
	mesh.nrm.resize(mesh.tris.size());
	for (size_t t = 0; t < mesh.tris.size(); t++)
	{
		const FrTriMesh::Tri& tr = mesh.tris[t];
		const m::vec3 n = m::la::cross(mesh.verts[tr.v[1]] - mesh.verts[tr.v[0]],
									   mesh.verts[tr.v[2]] - mesh.verts[tr.v[0]]);
		mesh.area += 0.5 * m::la::length(n);
		mesh.nrm[t] = FrNorm(n);
	}
}

Int32 FrTriOfFacePoint(const FrTriMesh& mesh, Int32 face, const m::vec3& p)
{
	if (face < 0 || (size_t)face + 1 >= mesh.srcStart.size())
		return 0;
	const Int32 first = mesh.srcStart[(size_t)face];
	const Int32 end = mesh.srcStart[(size_t)face + 1];
	if (first >= end)
		return 0;
	Int32 best = first;
	double bestD = std::numeric_limits<double>::infinity();
	for (Int32 ti = first; ti < end; ti++)
	{
		const FrTriMesh::Tri& tr = mesh.tris[(size_t)ti];
		const m::vec3& a = mesh.verts[tr.v[0]];
		const m::vec3& b = mesh.verts[tr.v[1]];
		const m::vec3& c = mesh.verts[tr.v[2]];
		const m::vec3& n = mesh.nrm[(size_t)ti];
		const bool inside =
			m::la::dot(m::la::cross(b - a, p - a), n) >= -1e-6 &&
			m::la::dot(m::la::cross(c - b, p - b), n) >= -1e-6 &&
			m::la::dot(m::la::cross(a - c, p - c), n) >= -1e-6;
		if (inside)
			return ti;
		const double d = m::la::length(p - (a + b + c) / 3.0);
		if (d < bestD) { bestD = d; best = ti; }
	}
	return best;
}

bool FrMarch(const FrTriMesh& mesh, Int32& tri, m::vec3& pos, m::vec3& dirn, double dist)
{
	int guard = 0;
	while (dist > 1e-7 && guard < 400)
	{
		guard++;
		const m::vec3 n = mesh.nrm[(size_t)tri];
		m::vec3 d = FrNorm(dirn - n * m::la::dot(dirn, n));
		const FrTriMesh::Tri& tr = mesh.tris[(size_t)tri];
		double tBest = std::numeric_limits<double>::infinity();
		int eBest = -1;
		for (int e = 0; e < 3; e++)
		{
			const m::vec3& A = mesh.verts[tr.v[e]];
			const m::vec3& B = mesh.verts[tr.v[(e + 1) % 3]];
			const m::vec3 AB = B - A;
			const double denom = m::la::dot(m::la::cross(d, AB), n);
			if (std::fabs(denom) < 1e-12)
				continue;
			const double t = m::la::dot(m::la::cross(A - pos, AB), n) / denom;
			const double s = m::la::dot(m::la::cross(A - pos, d), n) / denom;
			if (t > 1e-6 && s >= -1e-4 && s <= 1.0 + 1e-4 && t < tBest)
			{
				tBest = t;
				eBest = e;
			}
		}
		if (eBest < 0)
		{
			const m::vec3 cen = (mesh.verts[tr.v[0]] + mesh.verts[tr.v[1]] + mesh.verts[tr.v[2]]) / 3.0;
			pos = pos + (cen - pos) * 0.02;
			dirn = d;
			continue;
		}
		if (tBest >= dist)
		{
			pos = pos + d * dist;
			dirn = d;
			return true;
		}
		pos = pos + d * tBest;
		dist -= tBest;
		const Int32 nb = tr.nbr[eBest];
		if (nb < 0)
			return false;
		const m::vec3& A = mesh.verts[tr.v[eBest]];
		const m::vec3& B = mesh.verts[tr.v[(eBest + 1) % 3]];
		const m::vec3 axis = FrNorm(B - A);
		const m::vec3 n2 = mesh.nrm[(size_t)nb];
		const double ang = std::atan2(m::la::dot(m::la::cross(n, n2), axis), m::la::dot(n, n2));
		d = RotAxis(d, axis, ang);
		tri = nb;
		pos = pos + d * 1e-5;
		dirn = d;
	}
	return true;
}

                                                                                
                                                                                 
                                                     
bool FrMarchFlat(const FrTriMesh& mesh, Int32& tri, m::vec3& pos, m::vec3& dirn,
	double dist, const m::vec3& n0, double sub)
{
	m::vec3 nPrev = mesh.nrm[(size_t)tri];
	while (dist > 1e-9)
	{
		const double h = std::min(dist, sub);
		Int32 t2 = tri;
		m::vec3 p2 = pos, d2 = dirn;
		if (!FrMarch(mesh, t2, p2, d2, h))
			return false;
		const m::vec3& n2 = mesh.nrm[(size_t)t2];
		if (m::la::dot(n2, nPrev) < FR_FOLD_COS || m::la::dot(n2, n0) < FR_FOLD_TOTAL_COS)
			return false;
		nPrev = n2;
		tri = t2;
		pos = p2;
		dirn = d2;
		dist -= h;
	}
	return true;
}

struct FrEntry
{
	m::vec3 pos, nrm, tan;
	Int32 crack = -1, gen = 0, tri = -1, idx = 0;
};

struct FrCrack
{
	std::vector<m::vec3> pts, nrms;
	std::vector<Int32> tris;
	Int32 gen = 1;
	bool hit = false;
	bool guided = false;
	bool isBranch = false;                                                      
	bool startFree = false;
	double widthOverride = -1.0, depthOverride = -1.0, reliefScale = 1.0;
	bool noSpall = false;
	bool endEdge = false, startEdge = false;                                                 
	Int32 hitCrack = -1, bornOn = -1;
	std::vector<m::vec3> fP, fN;
};

struct FrHash
{
	double cell = FR_ATTRACT_R;
	std::map<std::tuple<Int32, Int32, Int32>, std::vector<FrEntry>> d;

	std::tuple<Int32, Int32, Int32> Key(const m::vec3& p) const
	{
		return { (Int32)std::floor(p.x / cell), (Int32)std::floor(p.y / cell),
				 (Int32)std::floor(p.z / cell) };
	}
	void Add(const FrEntry& e) { d[Key(e.pos)].push_back(e); }

	template <typename SKIP>
	bool Nearest(const m::vec3& pos, double radius, const m::vec3* normal,
		SKIP skip, FrEntry& out, double& outD) const
	{
		const Int32 r = (Int32)(radius / cell) + 1;
		const auto k0 = Key(pos);
		bool found = false;
		double best = radius;
		for (Int32 dx = -r; dx <= r; dx++)
			for (Int32 dy = -r; dy <= r; dy++)
				for (Int32 dz = -r; dz <= r; dz++)
				{
					auto it = d.find({ std::get<0>(k0) + dx, std::get<1>(k0) + dy,
									   std::get<2>(k0) + dz });
					if (it == d.end())
						continue;
					for (const FrEntry& e : it->second)
					{
						if (skip(e))
							continue;
						if (normal != nullptr && m::la::dot(e.nrm, *normal) < FR_NORMAL_DOT)
							continue;
						const double dd = m::la::length(e.pos - pos);
						if (dd < best)
						{
							best = dd;
							out = e;
							found = true;
						}
					}
				}
		outD = best;
		return found;
	}
};

m::vec3 FrClosestOnSeg(const m::vec3& p, const m::vec3& a, const m::vec3& b)
{
	const m::vec3 ab = b - a;
	const double L2 = m::la::dot(ab, ab);
	if (L2 < 1e-12)
		return a;
	double t = m::la::dot(p - a, ab) / L2;
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return a + ab * t;
}

double FrFreeRun(const FrHash& skel, const FrHash& edgeHash, const m::vec3& pos0,
	const m::vec3& dir0, Int32 parentId, const m::vec3& n0, double ns, double cap,
	double stepOverride = -1.0)
{
	const double step = stepOverride > 0.0 ? stepOverride : FR_STEP * ns;
	const double near_ = FR_SNAP_R * ns * 1.6;
	const double blind = 3.0 * FR_SNAP_R * ns;
	const double parentBlind = 2.0 * FR_ATTRACT_R * ns;
	FrEntry q;
	double qd = 0.0;
	for (double s = step; s < cap; s += step)
	{
		const m::vec3 p = pos0 + dir0 * s;
		if (skel.Nearest(p, near_, &n0, [&](const FrEntry& e) {
				return e.crack == parentId && m::la::length(e.pos - pos0) < parentBlind;
			}, q, qd))
			return s;
		if (s > blind && edgeHash.Nearest(p, near_, nullptr,
				[](const FrEntry&) { return false; }, q, qd))
			return s;
	}
	return cap;
}

m::vec3 FrJunctionPoint(const m::vec3& pos, const FrEntry& ent,
	const std::vector<FrCrack>& cracks)
{
	const FrCrack& oc = cracks[(size_t)ent.crack];
	m::vec3 best = ent.pos;
	double bd = m::la::length(ent.pos - pos);
	for (Int32 j = ent.idx - 1; j <= ent.idx; j++)
	{
		if (j >= 0 && j + 1 < (Int32)oc.pts.size())
		{
			const m::vec3 cp = FrClosestOnSeg(pos, oc.pts[(size_t)j], oc.pts[(size_t)j + 1]);
			const double dd = m::la::length(cp - pos);
			if (dd < bd) { bd = dd; best = cp; }
		}
	}
	return best;
}

void FrGrowCrack(const FrTriMesh& mesh, const FrHash& skel, const FrHash& edgeHash,
	const std::vector<FrCrack>& cracks, const std::vector<bool>* avoidTri,
	Int32 tri, m::vec3 pos, m::vec3 dirn,
	Int32 crackId, Int32 parentId, double maxLen, double curveLen, double sigma,
	double ns, PyRandom& rng, FrCrack& out, double stepOverride = -1.0,
	double foldStopCos = -2.0, const m::vec3* grain = nullptr, double grainPull = 0.0)
{
	out.pts = { pos };
	out.nrms = { mesh.nrm[(size_t)tri] };
	out.tris = { tri };
	out.hit = false;
	out.endEdge = false;
	out.hitCrack = -1;
	const m::vec3 startPos = pos;
	const double step = stepOverride > 0.0 ? stepOverride : FR_STEP * ns;
	double length = 0.0, dev = 0.0;
	const double curve = (FrGauss(rng, FR_CURVE_TOT_DEG) * m::kPi / 180.0)
		* step / MaxD(curveLen, step);
	const double snapR = FR_SNAP_R * ns, attractR = FR_ATTRACT_R * ns;
	const double blind = 3.0 * snapR;
	const double parentBlind = 2.0 * attractR;

	auto skip = [&](const FrEntry& e)
	{
		if (e.crack == crackId)
			return true;
		return e.crack == parentId && m::la::length(e.pos - startPos) < parentBlind;
	};

	while (length < maxLen)
	{
		const m::vec3 n = mesh.nrm[(size_t)tri];
		FrEntry q;
		double qd = 0.0;
		if (skel.Nearest(pos, attractR, &n, skip, q, qd))
		{
			if (qd < snapR)
			{
				out.pts.push_back(FrJunctionPoint(pos, q, cracks));
				out.nrms.push_back(n);
				out.tris.push_back(tri);
				out.hit = true;
				out.hitCrack = q.crack;
				break;
			}
			const m::vec3 to = q.pos - pos;
			if (m::la::dot(to, dirn) > 0.0)
			{
				m::vec3 perp = to - q.tan * m::la::dot(to, q.tan);
				perp = perp - n * m::la::dot(perp, n);
				if (m::la::length(perp) > 1e-9)
				{
					const double w = FR_ATTRACT_W * (1.0 - qd / attractR);
					dirn = FrNorm(dirn * (1.0 - w) + FrNorm(perp) * w);
				}
			}
		}
		{
			                                                                   
			                                                                 
			FrEntry qe;
			double qed = 0.0;
			if (m::la::length(pos - startPos) > blind
				&& edgeHash.Nearest(pos, attractR, nullptr, [](const FrEntry&) { return false; }, qe, qed)
				&& std::fabs(m::la::dot(dirn, qe.tan)) > 0.5)
			{
				m::vec3 perp = (qe.pos - pos) - qe.tan * m::la::dot(qe.pos - pos, qe.tan);
				perp = perp - n * m::la::dot(perp, n);
				if (m::la::length(perp) > 1e-9)
				{
					const double w = FR_ATTRACT_W * (1.0 - qed / attractR);
					dirn = FrNorm(dirn * (1.0 - w) + FrNorm(perp) * w);
				}
			}
		}
		if (m::la::length(pos - startPos) > blind)
		{
			FrEntry qe;
			double qed = 0.0;
			if (edgeHash.Nearest(pos, snapR, nullptr,
					[](const FrEntry&) { return false; }, qe, qed))
			{
				out.pts.push_back(qe.pos + dirn * 1.5 * ns);
				out.nrms.push_back(n);
				out.tris.push_back(tri);
				out.hit = true;
				out.endEdge = true;
				break;
			}
		}
		const double newDev = (dev + FrGauss(rng, sigma)) * (1.0 - FR_REVERT);
		double turn = newDev - dev + curve;
		dev = newDev;
		if (rng.Random() < FR_KINK_P)
		{
			const double kink = rng.Uniform(FR_KINK_DEG_MIN, FR_KINK_DEG_MAX) * m::kPi / 180.0;
			turn += rng.Random() < 0.5 ? -kink : kink;
		}
		dirn = RotAxis(dirn, n, turn);
		if (grain != nullptr && grainPull > 0.0)
		{
			                                                               
			                                                               
			                                                             
			m::vec3 gt = *grain - n * m::la::dot(*grain, n);
			const double gl = m::la::length(gt);
			if (gl > 0.2)
			{
				gt = gt / gl;
				if (m::la::dot(gt, dirn) < 0.0) gt = gt * -1.0;
				const double w = grainPull * std::min(1.0, (gl - 0.2) / 0.5);
				dirn = FrNorm(dirn * (1.0 - w) + gt * w);
			}
		}
		const m::vec3 prevPos = pos, prevDir = dirn;
		const Int32 prevTri = tri;
		if (!FrMarch(mesh, tri, pos, dirn, step))
			break;
		const m::vec3& nNow = mesh.nrm[(size_t)tri];
		const double totalCos = foldStopCos > -1.5 ? foldStopCos : FR_FOLD_TOTAL_COS;
		if (m::la::dot(nNow, out.nrms.back()) < FR_FOLD_COS || m::la::dot(nNow, out.nrms.front()) < totalCos)
		{
			                                                                      
			                                                  
			Int32 ft = prevTri;
			m::vec3 fp = prevPos, fd = prevDir;
			FrMarchFlat(mesh, ft, fp, fd, step, out.nrms.front(), 0.2 * step);
			out.pts.push_back(fp + fd * 0.5 * ns);
			out.nrms.push_back(mesh.nrm[(size_t)ft]);
			out.tris.push_back(ft);
			out.hit = true;
			out.endEdge = true;
			break;
		}
		if (avoidTri != nullptr && (*avoidTri)[(size_t)tri])
		{
			out.hit = true;
			break;
		}
		out.pts.push_back(pos);
		out.nrms.push_back(mesh.nrm[(size_t)tri]);
		out.tris.push_back(tri);
		length += step;
	}
}

Int32 FrNearestTri(const FrTriMesh& mesh, const m::vec3& p, m::vec3& outPos)
{
	Int32 best = -1;
	double bestD = std::numeric_limits<double>::infinity();
	for (size_t t = 0; t < mesh.tris.size(); t++)
	{
		const FrTriMesh::Tri& tr = mesh.tris[t];
		const m::vec3& a = mesh.verts[(size_t)tr.v[0]];
		const m::vec3& b = mesh.verts[(size_t)tr.v[1]];
		const m::vec3& c = mesh.verts[(size_t)tr.v[2]];
		const m::vec3& n = mesh.nrm[t];
		m::vec3 q = p - n * m::la::dot(p - a, n);
		const bool inside =
			m::la::dot(m::la::cross(b - a, q - a), n) >= 0.0 &&
			m::la::dot(m::la::cross(c - b, q - b), n) >= 0.0 &&
			m::la::dot(m::la::cross(a - c, q - c), n) >= 0.0;
		if (!inside)
		{
			const m::vec3 e0 = FrClosestOnSeg(p, a, b);
			const m::vec3 e1 = FrClosestOnSeg(p, b, c);
			const m::vec3 e2 = FrClosestOnSeg(p, c, a);
			q = e0;
			if (m::la::length(e1 - p) < m::la::length(q - p)) q = e1;
			if (m::la::length(e2 - p) < m::la::length(q - p)) q = e2;
		}
		const double d = m::la::length(q - p);
		if (d < bestD) { bestD = d; best = (Int32)t; outPos = q; }
	}
	return best;
}

bool FrGuidedCrack(const FrTriMesh& mesh, const std::vector<EVec>& poly,
	double step, FrCrack& out)
{
	if (poly.size() < 2 || mesh.tris.empty() || step <= 1e-9)
		return false;
	std::vector<m::vec3> S;
	S.push_back(poly[0]);
	for (size_t i = 1; i < poly.size(); i++)
	{
		const m::vec3 a = S.back();
		const m::vec3 b = poly[i];
		const double L = m::la::length(b - a);
		if (L < 1e-9)
			continue;
		const int n = (int)std::ceil(L / step);
		for (int k = 1; k <= n; k++)
			S.push_back(a + (b - a) * ((double)k / (double)n));
	}
	if (S.size() < 2)
		return false;
	m::vec3 pos;
	Int32 tri = FrNearestTri(mesh, S[0], pos);
	if (tri < 0)
		return false;
	out.pts.push_back(pos);
	out.nrms.push_back(mesh.nrm[(size_t)tri]);
	out.tris.push_back(tri);
	for (size_t i = 1; i < S.size(); i++)
	{
		const m::vec3 n = mesh.nrm[(size_t)tri];
		m::vec3 d = S[i] - pos;
		d = d - n * m::la::dot(d, n);
		const double dist = m::la::length(d);
		if (dist < 1e-9)
			continue;
		m::vec3 dirn = d / dist;
		if (!FrMarch(mesh, tri, pos, dirn, dist))
			break;
		out.pts.push_back(pos);
		out.nrms.push_back(mesh.nrm[(size_t)tri]);
		out.tris.push_back(tri);
	}
	out.guided = true;
	return out.pts.size() >= 4;
}

void FrRegisterCrack(FrHash& skel, std::vector<FrEntry>& allPoints,
	const FrCrack& c, Int32 crackId)
{
	for (size_t i = 0; i < c.pts.size(); i++)
	{
		const m::vec3& a = c.pts[i > 0 ? i - 1 : 0];
		const m::vec3& b = c.pts[i + 1 < c.pts.size() ? i + 1 : c.pts.size() - 1];
		FrEntry e;
		e.pos = c.pts[i];
		e.nrm = c.nrms[i];
		e.tan = FrNorm(b - a);
		e.crack = crackId;
		e.gen = c.gen;
		e.tri = c.tris[i];
		e.idx = (Int32)i;
		skel.Add(e);
		allPoints.push_back(e);
	}
}

void FrCloseFreeEnds(const FrTriMesh& mesh, std::vector<FrCrack>& cracks, const FrHash& skel,
	const std::vector<m::vec3>& edgePts, double ns)
{
	const double closeR = FR_CLOSE_R * ns;
	                                                                      
	                                                                         
	                                             
	auto flatTo = [&](const FrCrack& c, const m::vec3& target)
	{
		const m::vec3 end = c.pts.back();
		const double L = m::la::length(target - end);
		if (L < 1e-9)
			return true;
		Int32 tri = c.tris.back();
		m::vec3 pos = end, dirn = (target - end) / L;
		const double reach = MaxD(L - 1.0 * ns, 0.0);
		if (!FrMarchFlat(mesh, tri, pos, dirn, reach, c.nrms.back(), FR_STEP * ns * 0.25))
			return false;
		return m::la::length(pos - target) < MaxD(1.5 * ns, 0.2 * L);
	};
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		FrCrack& c = cracks[ci];
		if (c.hit || c.guided || c.pts.size() < 2)
			continue;
		const m::vec3 end = c.pts.back();
		const m::vec3 dEnd = FrNorm(end - c.pts[c.pts.size() - 2]);
		const m::vec3 nEnd = c.nrms.back();
		FrEntry q;
		double qd = 0.0;
		if (skel.Nearest(end, closeR, &nEnd,
				[&](const FrEntry& e) { return e.crack == (Int32)ci; }, q, qd))
		{
			const m::vec3 target = FrJunctionPoint(end, q, cracks);
			const m::vec3 dirn = FrNorm(target - end);
			if (m::la::dot(dirn, dEnd) > -0.3 && m::la::dot(q.nrm, nEnd) > FR_FOLD_COS && flatTo(c, target))
			{
				c.pts.push_back(target);
				c.nrms.push_back(nEnd);
				c.tris.push_back(c.tris.back());
				c.hit = true;
				c.hitCrack = q.crack;
			}
			continue;
		}
		double bestD = std::numeric_limits<double>::infinity();
		size_t bestI = 0;
		for (size_t i = 0; i < edgePts.size(); i++)
		{
			const double dd = m::la::length(edgePts[i] - end);
			if (dd < bestD) { bestD = dd; bestI = i; }
		}
		if (bestD < closeR)
		{
			const m::vec3 dirn = FrNorm(edgePts[bestI] - end);
			if (m::la::dot(dirn, dEnd) > -0.3 && flatTo(c, edgePts[bestI]))
			{
				c.pts.push_back(edgePts[bestI] + dirn * 1.5 * ns);
				c.nrms.push_back(nEnd);
				c.tris.push_back(c.tris.back());
				c.hit = true;
				c.endEdge = true;
			}
		}
	}
}

void FrChaikin(std::vector<m::vec3>& P, std::vector<m::vec3>& N, int iters)
{
	for (int it = 0; it < iters; it++)
	{
		if (P.size() < 3)
			return;
		std::vector<m::vec3> Q, M;
		Q.reserve(P.size() * 2);
		M.reserve(P.size() * 2);
		Q.push_back(P[0]);
		M.push_back(N[0]);
		for (size_t i = 0; i + 1 < P.size(); i++)
		{
			Q.push_back(P[i] * 0.75 + P[i + 1] * 0.25);
			M.push_back(FrNorm(N[i] * 0.75 + N[i + 1] * 0.25));
			Q.push_back(P[i] * 0.25 + P[i + 1] * 0.75);
			M.push_back(FrNorm(N[i] * 0.25 + N[i + 1] * 0.75));
		}
		Q.push_back(P.back());
		M.push_back(N.back());
		P.swap(Q);
		N.swap(M);
	}
}

void FrResample(std::vector<m::vec3>& P, std::vector<m::vec3>& N, double spacing)
{
	if (P.size() < 2)
		return;
	std::vector<m::vec3> Q = { P[0] }, M = { N[0] };
	double acc = 0.0;
	for (size_t i = 1; i < P.size(); i++)
	{
		const double L = m::la::length(P[i] - P[i - 1]);
		if (L < 1e-9)
			continue;
		acc += L;
		if (acc >= spacing)
		{
			Q.push_back(P[i]);
			M.push_back(N[i]);
			acc = 0.0;
		}
	}
	if (m::la::length(Q.back() - P.back()) > 1e-9)
	{
		Q.push_back(P.back());
		M.push_back(N.back());
	}
	P.swap(Q);
	N.swap(M);
}

void FrFinalSpine(FrCrack& c, double width, double dispStrength, double dispScale,
	double detail, double ns, PyRandom& rng)
{
	c.fP = c.pts;
	c.fN = c.nrms;
	FrChaikin(c.fP, c.fN, FR_CHAIKIN);
	FrResample(c.fP, c.fN, FR_CUT_STEP * ns / detail);
	if (c.fP.size() < 3)
		return;
	double total = 0.0;
	for (size_t i = 0; i + 1 < c.fP.size(); i++)
		total += m::la::length(c.fP[i + 1] - c.fP[i]);
	const double amp0 = (0.22 * width + 0.04 * ns) * dispStrength;
	const double w1 = FR_WAVE1 * ns * dispScale, w2 = FR_WAVE2 * ns * dispScale;
	const double ph0 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph1 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double fadeLen = 6.0 * ns;
	double arc = 0.0;
	for (size_t i = 1; i + 1 < c.fP.size(); i++)
	{
		arc += m::la::length(c.fP[i] - c.fP[i - 1]);
		const m::vec3 t = FrNorm(c.fP[i + 1] - c.fP[i - 1]);
		const m::vec3 bn = FrNorm(m::la::cross(c.fN[i], t));
		double fade = (arc / fadeLen) < 1.0 ? arc / fadeLen : 1.0;
		const double fade2 = ((total - arc) / fadeLen) < 1.0 ? (total - arc) / fadeLen : 1.0;
		fade *= MaxD(fade2, 0.0);
		const double off = 0.6 * std::sin(2.0 * m::kPi * arc / w1 + ph0)
						 + 0.4 * std::sin(2.0 * m::kPi * arc / w2 + ph1);
		c.fP[i] = c.fP[i] + bn * (amp0 * off * MaxD(fade, 0.0));
	}
}

m::vec3 FrClosestOnPolyline(const m::vec3& p, const std::vector<m::vec3>& P)
{
	m::vec3 best = P[0];
	double bd = std::numeric_limits<double>::infinity();
	for (size_t j = 0; j + 1 < P.size(); j++)
	{
		const m::vec3 cp = FrClosestOnSeg(p, P[j], P[j + 1]);
		const double dd = m::la::length(cp - p);
		if (dd < bd) { bd = dd; best = cp; }
	}
	return best;
}

void FrSnapSpines(std::vector<FrCrack>& cracks)
{
	for (FrCrack& c : cracks)
	{
		if (c.fP.size() < 2)
			continue;
		if (c.bornOn >= 0 && cracks[(size_t)c.bornOn].fP.size() >= 2)
			c.fP.front() = FrClosestOnPolyline(c.fP.front(), cracks[(size_t)c.bornOn].fP);
		if (c.hitCrack >= 0 && cracks[(size_t)c.hitCrack].fP.size() >= 2)
			c.fP.back() = FrClosestOnPolyline(c.fP.back(), cracks[(size_t)c.hitCrack].fP);
	}
}

double FrWallThickness(const ProjectDamageBVH& bvh, const m::vec3& p, const m::vec3& n, double ns)
{
	const double lift = 2.0 * ns;
	double d1 = 0.0;
	if (!bvh.Raycast(p + n * lift, -n, 4.0 * lift, &d1))
		return std::numeric_limits<double>::infinity();
	const m::vec3 front = p + n * (lift - d1);
	double d2 = 0.0;
	if (!bvh.Raycast(front - n * (1e-4 * ns), -n, 1e9, &d2))
		return std::numeric_limits<double>::infinity();
	return d2;
}

using FrSection = std::array<m::vec3, 5>;

m::vec3 FrSectionCentre(const FrSection& s)
{
	m::vec3 c(0.0);
	for (const m::vec3& v : s) c = c + v;
	return c * 0.2;
}

bool FrSectionsAdvance(const FrSection& a, const FrSection& b)
{
	const m::vec3 ca = FrSectionCentre(a), cb = FrSectionCentre(b);
	const m::vec3 t = cb - ca;
	const double tl = m::la::length(t);
	if (tl < 1e-9)
		return false;
	const m::vec3 dir = t / tl;
	const m::vec3 na = FrNorm(m::la::cross(a[1] - a[3], a[0] - a[2]));
	const m::vec3 nb = FrNorm(m::la::cross(b[1] - b[3], b[0] - b[2]));
	const m::vec3 fa = m::la::dot(na, dir) < 0.0 ? -na : na;
	const m::vec3 fb = m::la::dot(nb, dir) < 0.0 ? -nb : nb;
	for (int k = 0; k < 5; k++)
	{
		if (m::la::dot(b[(size_t)k] - ca, fa) <= 1e-6 * tl) return false;
		if (m::la::dot(a[(size_t)k] - cb, fb) >= -1e-6 * tl) return false;
	}
	return true;
}

m::Manifold FrLoftRun(const std::vector<FrSection>& secs, size_t i0, size_t i1)
{
	const size_t ns = i1 - i0 + 1;
	m::MeshGL64 mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(ns * 15);
	for (size_t i = i0; i <= i1; i++)
		for (const m::vec3& v : secs[i])
		{
			mesh.vertProperties.push_back(v.x);
			mesh.vertProperties.push_back(v.y);
			mesh.vertProperties.push_back(v.z);
		}
	auto& tv = mesh.triVerts;
	tv.reserve((ns - 1) * 30 + 18);
	for (size_t j = 0; j + 1 < ns; j++)
	{
		const uint64_t a = j * 5, b = (j + 1) * 5;
		for (uint64_t k = 0; k < 5; k++)
		{
			const uint64_t k2 = (k + 1) % 5;
			tv.insert(tv.end(), { a + k, b + k, b + k2 });
			tv.insert(tv.end(), { a + k, b + k2, a + k2 });
		}
	}
	const uint64_t e = (ns - 1) * 5;
	for (uint64_t k = 1; k + 1 < 5; k++)
	{
		tv.insert(tv.end(), { 0, k, k + 1 });
		tv.insert(tv.end(), { e, e + k + 1, e + k });
	}
	m::Manifold out(mesh);
	if (out.Status() != m::Manifold::Error::NoError)
		return m::Manifold();
	if (out.Volume() < 0.0)
	{
		for (size_t t = 0; t < tv.size(); t += 3)
			std::swap(tv[t + 1], tv[t + 2]);
		out = m::Manifold(mesh);
	}
	return out.Status() == m::Manifold::Error::NoError ? out : m::Manifold();
}

void FrLoftSections(const std::vector<FrSection>& secs, std::vector<m::Manifold>& cutters,
	const std::vector<bool>* foldAfter = nullptr, const std::vector<FrSection>* shallow = nullptr)
{
	auto hullPair = [&](size_t i)
	{
		std::vector<m::vec3> pts10;
		pts10.reserve(10);
		for (const m::vec3& v : secs[i]) pts10.push_back(v);
		for (const m::vec3& v : secs[i + 1]) pts10.push_back(v);
		cutters.push_back(m::Manifold::Hull(pts10));
	};
	size_t start = 0;
	auto flush = [&](size_t end)
	{
		if (end <= start)
			return;
		if (end == start + 1)
		{
			hullPair(start);
			return;
		}
		m::Manifold run = FrLoftRun(secs, start, end);
		if (run.NumTri() == 0 || run.Volume() <= 0.0)
		{
			for (size_t i = start; i < end; i++)
				hullPair(i);
			return;
		}
		cutters.push_back(std::move(run));
	};
	for (size_t i = 0; i + 1 < secs.size(); i++)
	{
		if (foldAfter != nullptr && (*foldAfter)[i])
		{
			                                                               
			                                                                
			                                                         
			flush(i);
			std::vector<m::vec3> pts10((*shallow)[i].begin(), (*shallow)[i].end());
			pts10.insert(pts10.end(), (*shallow)[i + 1].begin(), (*shallow)[i + 1].end());
			cutters.push_back(m::Manifold::Hull(pts10));
			start = i + 1;
			continue;
		}
		if (FrSectionsAdvance(secs[i], secs[i + 1]))
			continue;
		flush(i);
		hullPair(i);
		start = i + 1;
	}
	flush(secs.size() - 1);
}

void FrBuildCrackCutters(const FrCrack& c, double width, double depth, double detail,
	double ns, PyRandom& rng, std::vector<m::Manifold>& cutters, const IFieldMask* mask,
	int recipe = 1, const ProjectDamageBVH* bvh = nullptr, double thickCap = 0.0)
{
	const std::vector<m::vec3>& P = c.fP;
	const std::vector<m::vec3>& N = c.fN;
	if (P.size() < 2)
		return;
	double total = 0.0;
	for (size_t i = 0; i + 1 < P.size(); i++)
		total += m::la::length(P[i + 1] - P[i]);
	const double ph0 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph1 = rng.Uniform(0.0, 2.0 * m::kPi);
	const double ph2 = rng.Uniform(0.0, 2.0 * m::kPi);
	const uint64_t jag = (uint64_t)(rng.Random() * 4294967296.0);
	const double taperLen = MaxD(4.0 * FR_CUT_STEP * ns / detail, FR_TAPER_FRAC * total);
	const double margin = FR_MARGIN * ns;

	std::vector<std::array<m::vec3, 5>> secs;
	secs.reserve(P.size());
	std::vector<m::vec3> secP, secN;
	std::vector<double> secHw;
	std::vector<double> walls;
	if (recipe >= 2 && bvh != nullptr && thickCap > 0.0)
	{
		walls.resize(P.size());
		for (size_t i = 0; i < P.size(); i++)
			walls[i] = FrWallThickness(*bvh, P[i], FrNorm(N[i]), ns);
		for (int pass = 0; pass < 2; pass++)
		{
			double last = std::numeric_limits<double>::infinity();
			for (size_t k = 0; k < P.size(); k++)
			{
				const size_t i = pass == 0 ? k : P.size() - 1 - k;
				if (std::isfinite(walls[i])) last = walls[i];
				else walls[i] = last;
			}
		}
	}
	double arc = 0.0;
	for (size_t i = 0; i < P.size(); i++)
	{
		if (i > 0)
			arc += m::la::length(P[i] - P[i - 1]);
		const m::vec3& a = P[i > 0 ? i - 1 : 0];
		const m::vec3& b = P[i + 1 < P.size() ? i + 1 : P.size() - 1];
		const m::vec3 t = FrNorm(b - a);
		const m::vec3 n = FrNorm(N[i]);
		const m::vec3 bn = FrNorm(m::la::cross(n, t));
		const double mMod = 1.0 + 0.18 * std::sin(2.0 * m::kPi * 2.0 * arc / total + ph0)
								+ 0.12 * std::sin(2.0 * m::kPi * 5.0 * arc / total + ph1);
		double w = width * mMod;
		double d = depth * (1.0 + 0.25 * std::sin(2.0 * m::kPi * 3.0 * arc / total + ph2));
		if (recipe >= 2)
		{
			d = depth * std::pow(MaxD(mMod, 0.05), 0.9)
				* (1.0 + 0.08 * std::sin(2.0 * m::kPi * 3.0 * arc / total + ph2));
		}
		double mf = 1.0;
		if (mask != nullptr)
		{
			mf = mask->FadeScale(P[i]);
			w *= mf;
			d *= mf;
		}
		if (!c.hit)
		{
			double fade = (total - arc) / taperLen;
			if (fade > 1.0) fade = 1.0;
			w *= MaxD(fade, 0.04);
			d *= MaxD(fade, 0.04);
		}
		if (recipe >= 2 && c.startFree)
		{
			double fade = arc / taperLen;
			if (fade > 1.0) fade = 1.0;
			w *= MaxD(fade, 0.04);
			d *= MaxD(fade, 0.04);
		}
		if (!walls.empty() && thickCap > 0.0)
		{
			if (d > thickCap * walls[i])
				d = thickCap * walls[i];
		}
		const double jw = 1.0 + 0.3 * (Grain01(jag, i) - 0.5);
		const double jd = 1.0 + 0.22 * (Grain01(jag, i, 1) - 0.5);
		const double hw = MaxD(w * 0.5 * jw, 0.02);
		const double lean = 0.55 * (Grain01(jag, i, 2) - 0.5) * hw;
		const m::vec3& p = P[i];
		secs.push_back({ p - bn * hw + n * margin,
						 p + bn * hw + n * margin,
						 p + bn * hw,
						 p - n * (d * jd) + bn * lean,
						 p - bn * hw });
		secP.push_back(p);
		secN.push_back(n);
		secHw.push_back(hw);
	}
	std::vector<bool> foldAfter(secs.size(), false);
	std::vector<FrSection> shallow(secs);
	bool anyFold = false;
	for (size_t i = 0; i < secs.size(); i++)
	{
		const double lim = 1.5 * secHw[i];
		for (m::vec3& v : shallow[i])
		{
			const double below = -m::la::dot(v - secP[i], secN[i]);
			if (below > lim)
				v = v + secN[i] * (below - lim);
		}
		if (i + 1 < secs.size() && m::la::dot(secN[i], secN[i + 1]) < FR_CUT_FOLD_COS)
			foldAfter[i] = anyFold = true;
	}
	if (anyFold)
		FrLoftSections(secs, cutters, &foldAfter, &shallow);
	else
		FrLoftSections(secs, cutters);
}

void FrSampleAtArc(const std::vector<m::vec3>& P, const std::vector<m::vec3>& N,
	double s, m::vec3& pos, m::vec3& tan, m::vec3& nrm)
{
	double acc = 0.0;
	for (size_t i = 0; i + 1 < P.size(); i++)
	{
		const double L = m::la::length(P[i + 1] - P[i]);
		if (acc + L >= s || i + 2 == P.size())
		{
			tan = FrNorm(P[i + 1] - P[i]);
			double lt = s - acc;
			if (lt < 0.0) lt = 0.0;
			if (lt > L) lt = L;
			pos = P[i] + tan * lt;
			nrm = FrNorm(N[i]);
			return;
		}
		acc += L;
	}
	pos = P.back();
	tan = FrNorm(P.back() - P[P.size() - 2]);
	nrm = FrNorm(N.back());
}

void FrSpallOnCrack(const FrCrack& c, double width, double chipPerLen,
	double chipSize, double chipDepth, const std::vector<m::vec3>& edgePts,
	double ns, PyRandom& rng, double detail, double rimSoft,
	const IFieldMask* mask, int32_t maskStream, Int32& chipIdx,
	std::vector<m::Manifold>& cutters, const ProjectDamageBVH* bvh = nullptr, double thickCap = 0.0)
{
	const bool haveSpine = c.fP.size() >= 2;
	const std::vector<m::vec3>& P = haveSpine ? c.fP : c.pts;
	const std::vector<m::vec3>& N = haveSpine ? c.fN : c.nrms;
	if (P.size() < 2)
		return;
	double total = 0.0;
	for (size_t i = 0; i + 1 < P.size(); i++)
		total += m::la::length(P[i + 1] - P[i]);
	std::vector<SpallSpec> specs;
	std::vector<bool> keep;
	const double evExp = total * chipPerLen;
	int nEv = (int)evExp;
	if (rng.Random() < evExp - (double)nEv)
		nEv++;
	for (int k = 0; k < nEv; k++)
	{
		const double sRaw = total * rng.Random();
		m::vec3 pEv, tEv, nEv_;
		FrSampleAtArc(P, N, sRaw, pEv, tEv, nEv_);
		double prox = 0.0;
		if (!edgePts.empty())
		{
			double dMin = std::numeric_limits<double>::infinity();
			for (const m::vec3& e : edgePts)
			{
				const double dd = m::la::length(e - pEv);
				if (dd < dMin) dMin = dd;
			}
			prox = std::exp(-dMin / (FR_CORNER_SCALE * 1.7 * ns));
		}
		const double boost = 1.0 + FR_SPALL_EDGE_BOOST * prox;
		const int run = (int)std::nearbyint(rng.Uniform(FR_SPALL_RUN_MIN, FR_SPALL_RUN_MAX) * boost);
		const double sideSign = rng.Random() < 0.5 ? 1.0 : -1.0;
		const double mEv = chipSize * boost * 2.2;
		if (total <= 2.0 * mEv)
			continue;
		const double s = sRaw < mEv ? mEv : (sRaw > total - mEv ? total - mEv : sRaw);
		for (int j = 0; j < run; j++)
		{
			const double sizeFactor = 0.6 + 0.8 * rng.Random();
			const double size = chipSize * sizeFactor * boost;
			const double mChip = size * 1.15;
			if (total <= 2.0 * mChip)
				continue;
			double sj = s + (j - run * 0.5) * size * 0.85;
			if (sj < mChip) sj = mChip;
			if (sj > total - mChip) sj = total - mChip;
			m::vec3 p0, t, n;
			FrSampleAtArc(P, N, sj, p0, t, n);
			const m::vec3 side = FrNorm(m::la::cross(n, t)) * sideSign;
			const double su = size;
			const double sv = size * rng.Uniform(FR_SPALL_ASP_MIN, FR_SPALL_ASP_MAX);
			double offMag = size * rng.Uniform(FR_SPALL_OFF_MIN, FR_SPALL_OFF_MAX);
			const double offCap = MaxD(0.0, sv - width * 0.7);
			if (offMag > offCap) offMag = offCap;
			p0 = p0 + side * offMag;
			double depth = chipDepth * (0.7 + 0.6 * rng.Random());
			if (bvh != nullptr && thickCap > 0.0)
				depth = std::min(depth, thickCap * FrWallThickness(*bvh, p0, n, ns));
			int domeSeg = 0, outlineN = 0;
			CutterSegs(detail, sizeFactor / 1.4, domeSeg, outlineN);
			bool masked = false;
			double sizeScale = 1.0;
			if (mask != nullptr)
			{
				const double w0 = mask->WeightIfUsed(p0);
				masked = !mask->KeepW(w0, chipIdx, maskStream);
				sizeScale = mask->SizeScaleW(w0);
				if (sizeScale <= 1e-3)
				{
					masked = true;
					sizeScale = 1e-3;
				}
			}
			chipIdx++;
			specs.push_back(SpallPlan(p0, t, n, su * sizeScale, sv * sizeScale,
				depth * sizeScale, rng, domeSeg, outlineN, rimSoft));
			keep.push_back(!masked);
		}
	}
	SpallBuildAll(specs, keep, cutters);
}

                                                                            
                                                                           
                                                                         
                                                                           
                                                                           
void FrGrowBranches(const FrTriMesh& mesh, FrHash& skel, const FrHash& edgeHash,
	std::vector<FrEntry>& allPoints, std::vector<FrCrack>& cracks, const std::vector<bool>* avoidTri,
	Int32 g, const ProjectDamageParams::FractLevel& lvl, double sigma, double ns, uint32_t seed,
	const m::vec3* grain, double grainPull)
{
	const double per = MaxD(0.0, lvl.branches);
	if (per <= 0.0) return;
	PyRandom rng(seed);
	const size_t parents = cracks.size();
	for (size_t ci = 0; ci < parents; ++ci)
	{
		if (cracks[ci].gen != g || cracks[ci].guided || cracks[ci].isBranch) continue;
		const int count = (int)std::floor(per) + (rng.Random() < per - std::floor(per) ? 1 : 0);
		for (int k = 0; k < count; ++k)
		{
			const double at = rng.Uniform(0.15, 0.85);
			const double ang = rng.Uniform(18.0, 40.0) * (rng.Random() < 0.5 ? -1.0 : 1.0) * m::kPi / 180.0;
			const double lenF = rng.Uniform(0.6, 1.25);
			const FrCrack& par = cracks[ci];
			if (par.pts.size() < 4) continue;
			double total = 0.0;
			for (size_t i = 0; i + 1 < par.pts.size(); ++i) total += m::la::length(par.pts[i + 1] - par.pts[i]);
			double acc = 0.0;
			size_t i0 = 0;
			for (; i0 + 1 < par.pts.size(); ++i0)
			{
				const double L = m::la::length(par.pts[i0 + 1] - par.pts[i0]);
				if (acc + L >= at * total) break;
				acc += L;
			}
			if (i0 + 1 >= par.pts.size()) continue;
			const Int32 tri = par.tris[i0];
			if (avoidTri != nullptr && (*avoidTri)[(size_t)tri]) continue;
			const m::vec3 n = par.nrms[i0];
			const m::vec3 tan = FrNorm(par.pts[i0 + 1] - par.pts[i0]);
			const m::vec3 dir0 = RotAxis(tan, n, ang);
			double len = MaxD(0.0, lvl.branchLength) * lenF * total;
			if (lvl.length > 0.0) len = std::min(len, lvl.length);
			if (len < 4.0 * FR_STEP * ns) continue;
			FrCrack b;
			b.gen = g;
			b.bornOn = (Int32)ci;
			b.isBranch = true;
			FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri, par.pts[i0], dir0,
				(Int32)cracks.size(), (Int32)ci, len, 4.0 * len, sigma, ns, rng, b, -1.0, -2.0, grain, grainPull);
			if (b.pts.size() < 4) continue;
			FrRegisterCrack(skel, allPoints, b, (Int32)cracks.size());
			cracks.push_back(std::move(b));
		}
	}
}

void FrGrowInterior(const FrTriMesh& mesh, const FrHash& skel, const FrHash& edgeHash,
	const std::vector<FrCrack>& cracks, const std::vector<bool>* avoidTri,
	Int32 tri, const m::vec3& pos, const m::vec3& dirn, Int32 crackId,
	double maxLen, double curveLen, double sigma, double ns, PyRandom& rng, FrCrack& out)
{
	FrCrack a, b;
	FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri, pos, dirn, crackId, -1,
		maxLen, curveLen, sigma, ns, rng, a);
	FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri, pos, dirn * -1.0, crackId, -1,
		maxLen, curveLen, sigma, ns, rng, b);
	out.pts.assign(b.pts.rbegin(), b.pts.rend());
	out.nrms.assign(b.nrms.rbegin(), b.nrms.rend());
	out.tris.assign(b.tris.rbegin(), b.tris.rend());
	for (size_t i = 1; i < a.pts.size(); i++)
	{
		out.pts.push_back(a.pts[i]);
		out.nrms.push_back(a.nrms[i]);
		out.tris.push_back(a.tris[i]);
	}
	out.hit = a.hit;
	out.hitCrack = a.hitCrack;
	out.endEdge = a.endEdge;
	out.startEdge = b.endEdge;
	out.bornOn = b.hit ? b.hitCrack : -1;
	out.startFree = !b.hit;
}

bool FrPointAtArc(const FrCrack& c, double s, m::vec3& out)
{
	double acc = 0.0;
	for (size_t i = 0; i + 1 < c.pts.size(); i++)
	{
		const double L = m::la::length(c.pts[i + 1] - c.pts[i]);
		if (acc + L >= s)
		{
			const double t = L > 1e-12 ? (s - acc) / L : 0.0;
			out = c.pts[i] + (c.pts[i + 1] - c.pts[i]) * t;
			return true;
		}
		acc += L;
	}
	return false;
}

void FrFrame(const m::vec3& n, m::vec3& t, m::vec3& b)
{
	const m::vec3 axis = std::abs(n.z) < 0.9 ? m::vec3(0, 0, 1) : m::vec3(0, 1, 0);
	t = FrNorm(m::la::cross(axis, n));
	b = m::la::cross(n, t);
}

                                                                              
                     
  
                                                                            
                                                                          
                                                                             
                                                                            
                                                                            
                                                                            
                                                                          
                                                                              
                                                                             
                                                                             
                                                                                 
                                                                              

constexpr double FR_CELL_GEO_W   = 0.25;                                                               
constexpr double FR_CELL_SAMPLES = 6.0e5;                                       
constexpr double FR_CELL_GAP     = 0.3;                                                
constexpr double FR_CELL_SIMPLIFY = 0.08;                                                    
constexpr double FR_CELL_WIDTH_STEP = 1.2;                                                
constexpr double FR_CELL_FINE_SAMPLES = 1.2e7;                                          
constexpr int FR_CELL_BLOCK = 4;
constexpr double FR_FILL_MAX_PIECES = 3000.0;                                           
constexpr double FR_FILL_DEPTH = 0.6;                                                          
constexpr double FR_FILL_ROUGH_AMP = 0.3;                                                    
constexpr double FR_FILL_ROUGH_FREQ = 0.8;                                                                                      
constexpr double FR_CELL_SURF_SEEDS = 2.0e5;                                                

                                                                               
                                                                             
                                                                                 
bool FrPlaneTrace(const FrTriMesh& mesh, const FrHash& skel, const std::vector<FrCrack>& cracks,
	const std::vector<bool>* avoidTri, Int32 tri, m::vec3 pos, const m::vec3& tEnd,
	const m::vec3& nEnd, Int32 selfId, Int32 parentId, double maxLen, double ns, FrCrack& out)
{
	const m::vec3 pn = FrNorm(m::la::cross(tEnd, nEnd));
	if (m::la::length(pn) < 0.5)
		return false;
	const m::vec3 p0 = pos;
	const double step = 0.5 * FR_STEP * ns;
	const double snapR = FR_SNAP_R * ns;
	const double blind = 3.0 * snapR;
	out.pts = { pos };
	out.nrms = { mesh.nrm[(size_t)tri] };
	out.tris = { tri };
	out.hit = false;
	m::vec3 prevD = tEnd;
	double length = 0.0;
	auto skip = [&](const FrEntry& e)
	{
		return e.crack == selfId || (e.crack == parentId && m::la::length(e.pos - p0) < blind);
	};
	while (length < maxLen)
	{
		const m::vec3 n = mesh.nrm[(size_t)tri];
		m::vec3 d = m::la::cross(pn, n);
		if (m::la::length(d) < 0.2)
			return false;                                                         
		d = FrNorm(d);
		if (m::la::dot(d, prevD) < 0.0)
			d = d * -1.0;
		                                                        
		const double off = m::la::dot(pos - p0, pn);
		d = d - pn * std::max(-0.5, std::min(0.5, off / step));
		d = FrNorm(d - n * m::la::dot(d, n));
		if (!FrMarch(mesh, tri, pos, d, step))
			return false;
		prevD = d;
		length += step;
		if (avoidTri != nullptr && (*avoidTri)[(size_t)tri])
			return false;
		const m::vec3& nNow = mesh.nrm[(size_t)tri];
		FrEntry q;
		double qd = 0.0;
		if (length > blind && skel.Nearest(pos, snapR, &nNow, skip, q, qd))
		{
			out.pts.push_back(FrJunctionPoint(pos, q, cracks));
			out.nrms.push_back(nNow);
			out.tris.push_back(tri);
			out.hit = true;
			out.hitCrack = q.crack;
			return out.pts.size() >= 3;
		}
		out.pts.push_back(pos);
		out.nrms.push_back(nNow);
		out.tris.push_back(tri);
	}
	return false;
}

                                                                             
                                                               
void FrCloseForThrough(const FrTriMesh& mesh, FrHash& skel, const FrHash& edgeHash,
	std::vector<FrEntry>& allPoints, std::vector<FrCrack>& cracks,
	const std::vector<bool>* avoidTri, double ns, double sigma, const double levelLength[3], PyRandom& rng)
{
	const double maxLen = 2.0 * std::sqrt(mesh.area);
	auto childOf = [&](const FrCrack& c, Int32 parent)
	{
		FrCrack k;
		k.gen = c.gen;
		k.bornOn = parent;
		k.widthOverride = c.widthOverride;
		k.depthOverride = c.depthOverride;
		k.reliefScale = c.reliefScale;
		k.noSpall = true;
		return k;
	};
	auto add = [&](FrCrack& k)
	{
		FrRegisterCrack(skel, allPoints, k, (Int32)cracks.size());
		cracks.push_back(std::move(k));
	};

	                                                      
	const size_t nBase = cracks.size();
	for (size_t ci = 0; ci < nBase; ci++)
	{
		for (int side = 0; side < 2; side++)
		{
			const FrCrack& c = cracks[ci];
			if (c.guided || c.pts.size() < 3)
				break;
			if (side == 0 ? c.hit : !c.startFree)
				continue;
			const size_t a = side == 0 ? c.pts.size() - 1 : 0;
			const size_t b = side == 0 ? c.pts.size() - 2 : 1;
			const m::vec3 n = c.nrms[a];
			m::vec3 dir = c.pts[a] - c.pts[b];
			dir = FrNorm(dir - n * m::la::dot(dir, n));
			FrCrack k = childOf(c, (Int32)ci);
			                                                               
			                                                  
			const double lvlLen = levelLength[(size_t)std::max(0, std::min(2, c.gen - 1))];
			const double grow = lvlLen > 0.0 ? std::min(maxLen, lvlLen) : maxLen;
			FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, c.tris[a], c.pts[a], dir,
				(Int32)cracks.size(), (Int32)ci, grow, maxLen, 0.6 * sigma, ns, rng, k);
			if (k.pts.size() < 3 || !k.hit || (avoidTri != nullptr && (*avoidTri)[(size_t)k.tris.back()]))
				continue;
			add(k);
		}
	}

	                                                             
	struct End { size_t crack; bool atStart; };
	std::vector<End> ends;
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		const FrCrack& c = cracks[ci];
		if (c.pts.size() < 3 || c.guided)
			continue;
		if (c.endEdge)
			ends.push_back({ ci, false });
		if (c.startEdge)
			ends.push_back({ ci, true });
	}
	std::vector<bool> done(ends.size(), false);
	for (int pass = 0; pass < 2; pass++)
	{
		for (size_t ei = 0; ei < ends.size(); ei++)
		{
			if (done[ei])
				continue;
			const FrCrack& c = cracks[ends[ei].crack];
			                                                                        
			const size_t a = ends[ei].atStart ? 1 : c.pts.size() - 2;
			const size_t b = ends[ei].atStart ? 2 : c.pts.size() - 3;
			const size_t e = ends[ei].atStart ? 0 : c.pts.size() - 1;
			const m::vec3 n = c.nrms[a];
			m::vec3 t = c.pts[e] - c.pts[b];
			t = t - n * m::la::dot(t, n);
			if (m::la::length(t) < 1e-9)
			{
				done[ei] = true;
				continue;
			}
			t = FrNorm(t);
			FrCrack k = childOf(c, (Int32)ends[ei].crack);
			if (!FrPlaneTrace(mesh, skel, cracks, avoidTri, c.tris[a], c.pts[a], t, n,
					(Int32)cracks.size(), (Int32)ends[ei].crack, maxLen, ns, k))
				continue;
			done[ei] = true;
			add(k);
		}
	}
}

                                                                          
                                                                            
                                                                            
                                                                            
                                                                              
                                                                          
                                                                           
                                                                    
                                                                       
                                                                          
                                                                             
                                                                         
                                                                            
                                                                      
                                                                          
void FrLayHelpers(const FrTriMesh& mesh, FrHash& skel, std::vector<FrEntry>& allPoints,
	std::vector<FrCrack>& cracks, const std::vector<m::vec3>& edgePos,
	const std::vector<std::vector<EVec>>& strokes, double ns, double detail, double snap)
{
	const double step = FR_CUT_STEP * ns / std::max(0.05, detail);
	for (const std::vector<EVec>& st : strokes)
	{
		if (st.size() < 2) continue;
		std::vector<EVec> poly(st.begin(), st.end());
		const bool loop = poly.size() > 3 && m::la::length(poly.front() - poly.back()) < snap;
		Int32 startCrack = -1, endCrack = -1;
		bool startHit = false, endHit = false;
		auto pin = [&](EVec& p, Int32& crackId, bool& hit) {
			double best = snap;
			for (const FrEntry& e : allPoints)
			{
				const double d = m::la::length(e.pos - p);
				if (d < best) { best = d; p = e.pos; crackId = e.crack; hit = true; }
			}
			for (const m::vec3& e : edgePos)
			{
				const double d = m::la::length(e - p);
				if (d < best) { best = d; p = e; crackId = -1; hit = true; }
			}
		};
		if (loop)
		{
			poly.back() = poly.front();                      
			startHit = endHit = true;
		}
		else
		{
			pin(poly.front(), startCrack, startHit);
			pin(poly.back(), endCrack, endHit);
		}
		                                                                          
		const EVec mid = poly[poly.size() / 2];
		Int32 gen = 1;
		double bestMid = std::numeric_limits<double>::infinity();
		for (const FrEntry& e : allPoints)
		{
			const double d = m::la::length(e.pos - mid);
			if (d < bestMid && e.crack >= 0 && (size_t)e.crack < cracks.size()) { bestMid = d; gen = cracks[(size_t)e.crack].gen; }
		}
		FrCrack k;
		k.gen = std::max(1, std::min(3, gen));
		if (!FrGuidedCrack(mesh, poly, step, k)) continue;
		k.guided = false;                                                
		k.noSpall = true;
		k.bornOn = startCrack;
		k.startFree = !startHit;
		k.hit = endHit;
		k.hitCrack = endCrack;
		                                                                     
		                                                              
		                                                                   
		                                                                  
		const bool bothOnEdges = !loop && startHit && endHit && startCrack < 0 && endCrack < 0;
		k.startEdge = !loop && !bothOnEdges && startHit && startCrack < 0;
		k.endEdge = !loop && !bothOnEdges && endHit && endCrack < 0;
		const Int32 helperId = (Int32)cracks.size();
		const EVec a = poly.back(), b = poly.front();
		const Int32 helperGen = k.gen;
		FrRegisterCrack(skel, allPoints, k, helperId);
		cracks.push_back(std::move(k));
		if (bothOnEdges && m::la::length(a - b) > 1e-9)
		{
			                                                                  
			std::vector<EVec> path = { a };
			const m::vec3 dirE = FrNorm(b - a);
			const double L = m::la::length(b - a);
			std::vector<std::pair<double, m::vec3>> mids;
			for (const m::vec3& e : edgePos)
			{
				const double t = m::la::dot(e - a, dirE);
				if (t <= 0.0 || t >= L) continue;
				if (m::la::length((e - a) - dirE * t) < 0.25 * L) mids.push_back({t, e});
			}
			std::sort(mids.begin(), mids.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			for (const auto& q : mids) path.push_back(q.second);
			path.push_back(b);
			FrCrack e;
			e.gen = helperGen;
			if (FrGuidedCrack(mesh, path, step, e))
			{
				e.guided = false;
				e.noSpall = true;
				e.bornOn = helperId;
				e.hit = true;
				e.hitCrack = helperId;
				FrRegisterCrack(skel, allPoints, e, (Int32)cracks.size());
				cracks.push_back(std::move(e));
			}
		}
	}
}

void FrCloseIslands(const FrTriMesh& mesh, FrHash& skel, const FrHash& edgeHash,
	const std::vector<m::vec3>& edgePos, std::vector<FrEntry>& allPoints,
	std::vector<FrCrack>& cracks, const std::vector<bool>* avoidTri, double ns, double sigma,
	int passes, double detail, PyRandom& rng)
{
	const double maxLen = 2.0 * std::sqrt(mesh.area);
	const double step = FR_CUT_STEP * ns / std::max(0.05, detail);
	auto childOf = [&](const FrCrack& c, Int32 parent) {
		FrCrack k;
		k.gen = c.gen;
		k.bornOn = parent;
		k.widthOverride = c.widthOverride;
		k.depthOverride = c.depthOverride;
		k.reliefScale = c.reliefScale;
		k.noSpall = true;
		return k;
	};
	auto add = [&](FrCrack& k) {
		FrRegisterCrack(skel, allPoints, k, (Int32)cracks.size());
		cracks.push_back(std::move(k));
	};
	for (int pass = 0; pass < std::max(1, passes); ++pass)
	{
		const double bridgeR = maxLen * 0.08 * (pass + 1);
		int closed = 0, open = 0;
		const size_t n0 = cracks.size();
		for (size_t ci = 0; ci < n0; ++ci)
		{
			for (int side = 0; side < 2; ++side)
			{
				if (cracks[ci].guided || cracks[ci].pts.size() < 3) break;
				const bool isOpen = side == 0 ? !cracks[ci].hit : cracks[ci].startFree;
				if (!isOpen) continue;
				++open;
				const FrCrack& c = cracks[ci];
				const size_t a = side == 0 ? c.pts.size() - 1 : 0;
				const size_t b = side == 0 ? c.pts.size() - 2 : 1;
				const m::vec3 n = c.nrms[a];
				m::vec3 dir = c.pts[a] - c.pts[b];
				dir = FrNorm(dir - n * m::la::dot(dir, n));
				const m::vec3 endPos = c.pts[a];
				const Int32 endTri = c.tris[a];
				                              
				FrCrack k = childOf(c, (Int32)ci);
				FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, endTri, endPos, dir,
					(Int32)cracks.size(), (Int32)ci, maxLen, maxLen, 0.5 * sigma, ns, rng, k);
				bool ok = k.pts.size() >= 3 && k.hit && !(avoidTri != nullptr && (*avoidTri)[(size_t)k.tris.back()]);
				if (!ok)
				{
					                                                            
					m::vec3 target(0.0);
					double best = bridgeR;
					Int32 hitCrack = -1;
					bool found = false;
					for (const FrEntry& e : allPoints)
					{
						if (e.crack == (Int32)ci) continue;
						const double d = m::la::length(e.pos - endPos);
						if (d < best && d > 1e-9) { best = d; target = e.pos; hitCrack = e.crack; found = true; }
					}
					for (const m::vec3& e : edgePos)
					{
						const double d = m::la::length(e - endPos);
						if (d < best && d > 1e-9) { best = d; target = e; hitCrack = -1; found = true; }
					}
					if (found)
					{
						k = childOf(cracks[ci], (Int32)ci);
						ok = FrGuidedCrack(mesh, { endPos, target }, step, k);
						if (ok && avoidTri != nullptr)
							for (Int32 t : k.tris) if ((*avoidTri)[(size_t)t]) { ok = false; break; }
						if (ok)
						{
							k.guided = false;
							k.hit = true;
							k.hitCrack = hitCrack;
							k.endEdge = hitCrack < 0;
						}
					}
				}
				if (!ok) continue;
				add(k);
				FrCrack& cc = cracks[ci];
				if (side == 0) cc.hit = true; else cc.startFree = false;
				++closed;
			}
		}
		if (open == 0 || closed == 0) break;
	}
}

double FrPointTriDist(const m::vec3& p, const m::vec3& a, const m::vec3& b, const m::vec3& c)
{
	                                               
	const m::vec3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) return m::la::length(p - a);
	const m::vec3 bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) return m::la::length(p - b);
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
		return m::la::length(p - (a + ab * (d1 / (d1 - d3))));
	const m::vec3 cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) return m::la::length(p - c);
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
		return m::la::length(p - (a + ac * (d2 / (d2 - d6))));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
		return m::la::length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
	const double den = 1.0 / (va + vb + vc);
	return m::la::length(p - (a + ab * (vb * den) + ac * (vc * den)));
}

                                                                           
                                                                     
struct FrSeedTree
{
	struct Node { m::vec3 lo, hi; double minW; Int32 label; Int32 l, r, b, e; };
	std::vector<m::vec3> p;
	std::vector<double> w;
	std::vector<Int32> lab;
	std::vector<Int32> idx;
	std::vector<Node> nodes;

	Int32 Build(Int32 b, Int32 e, int depth)
	{
		Node nd;
		nd.lo = m::vec3(std::numeric_limits<double>::infinity());
		nd.hi = m::vec3(-std::numeric_limits<double>::infinity());
		nd.minW = std::numeric_limits<double>::infinity();
		nd.label = lab[(size_t)idx[(size_t)b]];
		for (Int32 i = b; i < e; i++)
		{
			const Int32 s = idx[(size_t)i];
			nd.lo = m::la::min(nd.lo, p[(size_t)s]);
			nd.hi = m::la::max(nd.hi, p[(size_t)s]);
			nd.minW = std::min(nd.minW, w[(size_t)s]);
			if (lab[(size_t)s] != nd.label)
				nd.label = -1;
		}
		nd.b = b;
		nd.e = e;
		nd.l = nd.r = -1;
		const Int32 me = (Int32)nodes.size();
		nodes.push_back(nd);
		if (e - b > 8 && depth < 40)
		{
			const m::vec3 ext = nd.hi - nd.lo;
			const int ax = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
			const Int32 mid = (b + e) / 2;
			std::nth_element(idx.begin() + b, idx.begin() + mid, idx.begin() + e,
				[&](Int32 u, Int32 v) { return p[(size_t)u][ax] < p[(size_t)v][ax]; });
			const Int32 l = Build(b, mid, depth + 1);
			const Int32 r = Build(mid, e, depth + 1);
			nodes[(size_t)me].l = l;
			nodes[(size_t)me].r = r;
		}
		return me;
	}
	void Build()
	{
		idx.resize(p.size());
		for (size_t i = 0; i < idx.size(); i++)
			idx[i] = (Int32)i;
		nodes.clear();
		if (!p.empty())
			Build(0, (Int32)p.size(), 0);
	}
	static double BoxDist(const Node& n, const m::vec3& q)
	{
		const m::vec3 d = m::la::max(m::la::max(n.lo - q, q - n.hi), m::vec3(0.0));
		return m::la::length(d);
	}
	                                                                          
	                                                      
	void Query(Int32 ni, const m::vec3& q, Int32 notLabel, double& best, Int32& bestSeed,
		Int32 onlyLabel = -1) const
	{
		const Node& n = nodes[(size_t)ni];
		if (notLabel >= 0 && n.label == notLabel)
			return;
		if (onlyLabel >= 0 && n.label >= 0 && n.label != onlyLabel)
			return;
		if (BoxDist(n, q) + n.minW >= best)
			return;
		if (n.l < 0)
		{
			for (Int32 i = n.b; i < n.e; i++)
			{
				const Int32 s = idx[(size_t)i];
				if (lab[(size_t)s] == notLabel || (onlyLabel >= 0 && lab[(size_t)s] != onlyLabel))
					continue;
				const double d = m::la::length(p[(size_t)s] - q) + w[(size_t)s];
				if (d < best)
				{
					best = d;
					bestSeed = s;
				}
			}
			return;
		}
		const Node& L = nodes[(size_t)n.l];
		const Node& R = nodes[(size_t)n.r];
		if (BoxDist(L, q) + L.minW <= BoxDist(R, q) + R.minW)
		{
			Query(n.l, q, notLabel, best, bestSeed, onlyLabel);
			Query(n.r, q, notLabel, best, bestSeed, onlyLabel);
		}
		else
		{
			Query(n.r, q, notLabel, best, bestSeed, onlyLabel);
			Query(n.l, q, notLabel, best, bestSeed, onlyLabel);
		}
	}
};

                                                                            
                                                                           
                                                                          
                                                                  
struct FrFillParams
{
	double size = 0.0;                                                      
	double rough = 0.5;                                       
	const ProjectDamageBVH* bvh = nullptr;
};

                                                       
double FrValueNoise(const m::vec3& p, uint32_t ch)
{
	auto hash = [&](int64_t x, int64_t y, int64_t z)
	{
		uint64_t h = (uint64_t)x * 0x9e3779b97f4a7c15ULL ^ (uint64_t)y * 0xc2b2ae3d27d4eb4fULL
			^ (uint64_t)z * 0x165667b19e3779f9ULL ^ (uint64_t)(ch + 1) * 0xd6e8feb86659fd93ULL;
		h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
		h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
		h ^= h >> 31;
		return (double)(h >> 11) * (2.0 / 9007199254740992.0) - 1.0;
	};
	const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const int64_t ix = (int64_t)fx, iy = (int64_t)fy, iz = (int64_t)fz;
	auto sm = [](double t) { return t * t * (3.0 - 2.0 * t); };
	const double u = sm(p.x - fx), v = sm(p.y - fy), w = sm(p.z - fz);
	double r = 0.0;
	for (int c = 0; c < 8; c++)
	{
		const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
		r += (dx ? u : 1.0 - u) * (dy ? v : 1.0 - v) * (dz ? w : 1.0 - w) * hash(ix + dx, iy + dy, iz + dz);
	}
	return r;
}

double FrFbm(const m::vec3& p, uint32_t ch)
{
	double r = 0.0, a = 0.57, f = 1.0;
	for (int o = 0; o < 3; o++, a *= 0.5, f *= 2.13)
		r += a * FrValueNoise(p * f + m::vec3(17.3 * o, 5.1 * o, 11.7 * o), ch);
	return r;
}

                                                                          
                                                            
m::Manifold FrBuildFragmentCutter(const FrTriMesh& mesh, const std::vector<FrCrack>& cracks,
	const std::vector<double>& crackWidth, const std::vector<bool>* avoidTri, double ns,
	const std::vector<double>* crackDepth = nullptr, double capDepth = 0.0,
	const FrFillParams* fill = nullptr, uint32_t seed = 0)
{
	if (capDepth > 0.0)
		fill = nullptr;
	const size_t nT = mesh.tris.size();
	if (nT == 0 || cracks.empty())
		return m::Manifold();
	std::vector<m::vec3> cen(nT);
	std::vector<double> area(nT);
	std::vector<double> edgeLens;
	edgeLens.reserve(nT);
	m::vec3 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
	for (size_t t = 0; t < nT; t++)
	{
		const FrTriMesh::Tri& tr = mesh.tris[t];
		const m::vec3 &a = mesh.verts[(size_t)tr.v[0]], &b = mesh.verts[(size_t)tr.v[1]], &c = mesh.verts[(size_t)tr.v[2]];
		cen[t] = (a + b + c) / 3.0;
		area[t] = 0.5 * m::la::length(m::la::cross(b - a, c - a));
		edgeLens.push_back(std::max({ m::la::length(b - a), m::la::length(c - b), m::la::length(a - c) }));
		lo = m::la::min(lo, m::la::min(a, m::la::min(b, c)));
		hi = m::la::max(hi, m::la::max(a, m::la::max(b, c)));
	}
	std::nth_element(edgeLens.begin(), edgeLens.begin() + (std::ptrdiff_t)(nT / 2), edgeLens.end());
	const double edgeMed = edgeLens[nT / 2];

	                                                                           
	const double gcell = std::max(2.0 * edgeMed, 1e-6);
	std::unordered_map<int64_t, std::vector<Int32>> grid;
	auto gkey = [&](int64_t x, int64_t y, int64_t z) { return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791); };
	for (size_t t = 0; t < nT; t++)
	{
		const FrTriMesh::Tri& tr = mesh.tris[t];
		m::vec3 tlo = mesh.verts[(size_t)tr.v[0]], thi = tlo;
		for (int k = 1; k < 3; k++)
		{
			tlo = m::la::min(tlo, mesh.verts[(size_t)tr.v[k]]);
			thi = m::la::max(thi, mesh.verts[(size_t)tr.v[k]]);
		}
		for (int64_t x = (int64_t)std::floor(tlo.x / gcell); x <= (int64_t)std::floor(thi.x / gcell); x++)
			for (int64_t y = (int64_t)std::floor(tlo.y / gcell); y <= (int64_t)std::floor(thi.y / gcell); y++)
				for (int64_t z = (int64_t)std::floor(tlo.z / gcell); z <= (int64_t)std::floor(thi.z / gcell); z++)
					grid[gkey(x, y, z)].push_back((Int32)t);
	}
	std::vector<char> band(nT, 0);
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		const FrCrack& c = cracks[ci];
		const std::vector<m::vec3>& P = c.fP.size() >= 2 ? c.fP : c.pts;
		const double r = 0.6 * edgeMed + 0.25 * crackWidth[ci];
		for (size_t i = 0; i + 1 < P.size(); i++)
		{
			const m::vec3 a = P[i], b = P[i + 1];
			const double L = m::la::length(b - a);
			const int nS = std::max(1, (int)std::ceil(L / (0.5 * r)));
			for (int k = 0; k <= nS; k++)
			{
				const m::vec3 q = a + (b - a) * ((double)k / nS);
				for (int64_t x = (int64_t)std::floor((q.x - r) / gcell); x <= (int64_t)std::floor((q.x + r) / gcell); x++)
					for (int64_t y = (int64_t)std::floor((q.y - r) / gcell); y <= (int64_t)std::floor((q.y + r) / gcell); y++)
						for (int64_t z = (int64_t)std::floor((q.z - r) / gcell); z <= (int64_t)std::floor((q.z + r) / gcell); z++)
						{
							auto it = grid.find(gkey(x, y, z));
							if (it == grid.end())
								continue;
							for (Int32 t : it->second)
							{
								if (band[(size_t)t])
									continue;
								const FrTriMesh::Tri& tr = mesh.tris[(size_t)t];
								if (FrPointTriDist(q, mesh.verts[(size_t)tr.v[0]], mesh.verts[(size_t)tr.v[1]],
										mesh.verts[(size_t)tr.v[2]]) < r)
									band[(size_t)t] = 1;
							}
						}
			}
		}
	}

	                                                                             
	std::vector<Int32> region(nT, -1);
	std::vector<double> regArea;
	for (size_t t0 = 0; t0 < nT; t0++)
	{
		if (band[t0] || region[t0] >= 0)
			continue;
		const Int32 id = (Int32)regArea.size();
		double ar = 0.0;
		std::vector<Int32> stack = { (Int32)t0 };
		region[t0] = id;
		while (!stack.empty())
		{
			const Int32 t = stack.back();
			stack.pop_back();
			ar += area[(size_t)t];
			for (int k = 0; k < 3; k++)
			{
				const Int32 nb = mesh.tris[(size_t)t].nbr[k];
				if (nb >= 0 && !band[(size_t)nb] && region[(size_t)nb] < 0)
				{
					region[(size_t)nb] = id;
					stack.push_back(nb);
				}
			}
		}
		regArea.push_back(ar);
	}
	                                                                 
	const double minArea = std::pow(2.0 * FR_STEP * ns, 2.0);
	std::vector<Int32> remap(regArea.size(), -1);
	Int32 nReg = 0;
	for (size_t r = 0; r < regArea.size(); r++)
		if (regArea[r] >= minArea)
			remap[r] = nReg++;
	if (nReg < 2)
		return m::Manifold();
	for (size_t t = 0; t < nT; t++)
		if (region[t] >= 0)
			region[t] = remap[(size_t)region[t]];
	                               
	std::vector<double> regA((size_t)nReg, 0.0);
	for (size_t t = 0; t < nT; t++)
		if (region[t] >= 0) regA[(size_t)region[t]] += area[t];
	const Int32 body = (Int32)(std::max_element(regA.begin(), regA.end()) - regA.begin());

	                                                                             
	std::vector<double> geo(nT, std::numeric_limits<double>::infinity());
	using QE = std::pair<double, Int32>;
	std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
	for (size_t t = 0; t < nT; t++)
	{
		if (band[t])
			continue;
		bool edge = false;
		for (int k = 0; k < 3; k++)
		{
			const Int32 nb = mesh.tris[t].nbr[k];
			if (nb >= 0 && band[(size_t)nb])
				edge = true;
		}
		                                                          
		if (edge || (avoidTri != nullptr && (*avoidTri)[t]))
		{
			geo[t] = 0.0;
			pq.push({ 0.0, (Int32)t });
		}
	}
	while (!pq.empty())
	{
		const QE e = pq.top();
		pq.pop();
		if (e.first > geo[(size_t)e.second])
			continue;
		for (int k = 0; k < 3; k++)
		{
			const Int32 nb = mesh.tris[(size_t)e.second].nbr[k];
			if (nb < 0 || band[(size_t)nb])
				continue;
			const double d = e.first + m::la::length(cen[(size_t)nb] - cen[(size_t)e.second]);
			if (d < geo[(size_t)nb])
			{
				geo[(size_t)nb] = d;
				pq.push({ d, nb });
			}
		}
	}

	                                                                             
	FrSeedTree tree;
	size_t nCand = 0;
	for (size_t t = 0; t < nT; t++)
		if (region[t] >= 0)
			nCand++;
	const size_t stride = std::max<size_t>(1, nCand / 80000);
	                                                                         
	                                                                     
	                                                                          
	                                                                     
	auto denseTri = [&](size_t t)
	{
		if (region[t] < 0)
			return false;
		if (region[t] != body)
			return true;
		const double g = std::isfinite(geo[t]) ? geo[t] : 0.0;
		return g < 2.0 * edgeMed + 2.0 * capDepth;
	};
	double denseArea = 0.0;
	for (size_t t = 0; t < nT; t++)
		if (denseTri(t))
			denseArea += area[t];
	std::vector<double> wsS(crackWidth.begin(), crackWidth.end());
	std::nth_element(wsS.begin(), wsS.begin() + (std::ptrdiff_t)(wsS.size() / 2), wsS.end());
	const double medWS = wsS.empty() ? edgeMed : wsS[wsS.size() / 2];
	const double spacing = std::max(capDepth > 0.0 ? 0.5 * capDepth : 2.0 * FR_CELL_WIDTH_STEP * medWS,
		std::sqrt(denseArea / FR_CELL_SURF_SEEDS));
	std::vector<m::vec3> deepAt;                                                         
	size_t k = 0;
	for (size_t t = 0; t < nT; t++)
	{
		if (region[t] < 0)
			continue;
		const double g = std::isfinite(geo[t]) ? geo[t] : 0.0;
		                                                                        
		                                                                   
		                                                                      
		                           
		                                                                     
		                            
		const double wt = capDepth > 0.0 || fill != nullptr ? 0.0 : FR_CELL_GEO_W * g;
		const FrTriMesh::Tri& tr = mesh.tris[t];
		if (denseTri(t))
		{
			const m::vec3 &a = mesh.verts[(size_t)tr.v[0]], &b = mesh.verts[(size_t)tr.v[1]], &c = mesh.verts[(size_t)tr.v[2]];
			                                                          
			const int ns_ = std::max(1, std::min(64, (int)std::round(std::sqrt(2.0 * area[t]) / spacing)));
			for (int i = 0; i < ns_; i++)
				for (int j = 0; i + j < ns_; j++)
				{
					const m::vec3 q = a + (b - a) * ((i + 1.0 / 3.0) / ns_) + (c - a) * ((j + 1.0 / 3.0) / ns_);
					tree.p.push_back(q);
					tree.w.push_back(wt);
					tree.lab.push_back(region[t]);
					if (capDepth > 0.0)
						deepAt.push_back(q - FrNorm(mesh.nrm[t]) * (2.0 * capDepth));
				}
			continue;
		}
		if (g > 4.0 * edgeMed && (k++ % stride) != 0)
			continue;
		tree.p.push_back(cen[t]);
		tree.w.push_back(wt);
		tree.lab.push_back(region[t]);
	}
	                                                                          
	const double hSeed = std::max(std::cbrt(std::max((hi - lo).x, 1e-6) * std::max((hi - lo).y, 1e-6)
		* std::max((hi - lo).z, 1e-6) / FR_CELL_SAMPLES), 1e-3 * m::la::length(hi - lo));
	                                                                          
	                                                                        
	                                                                       
	                                                                        
	                                                                        
	                                                                         
	                                                                         
	                                           
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		const FrCrack& c = cracks[ci];
		const bool useF = c.fP.size() >= 2 && c.fN.size() == c.fP.size();
		const std::vector<m::vec3>& P = useF ? c.fP : c.pts;
		const std::vector<m::vec3>& N = useF ? c.fN : c.nrms;
		const double r = 0.6 * edgeMed + 0.25 * crackWidth[ci];
		const double reach = r + edgeMed;
		for (size_t i = 0; i + 1 < P.size(); i++)
		{
			const m::vec3 a = P[i], b = P[i + 1];
			const double L = m::la::length(b - a);
			if (L < 1e-9)
				continue;
			const m::vec3 t = (b - a) / L;
			const m::vec3 bn = FrNorm(m::la::cross(FrNorm(N[i]), t));
			const int nS = std::max(1, (int)std::ceil(L / (0.5 * edgeMed)));
			for (int k = 0; k < nS; k++)
			{
				const m::vec3 q0 = a + (b - a) * ((double)k / nS);
				for (int side = -1; side <= 1; side += 2)
				{
					const m::vec3 q = q0 + bn * (side * r);
					Int32 best = -1;
					double bd = reach;
					for (int64_t x = (int64_t)std::floor((q.x - reach) / gcell); x <= (int64_t)std::floor((q.x + reach) / gcell); x++)
						for (int64_t y = (int64_t)std::floor((q.y - reach) / gcell); y <= (int64_t)std::floor((q.y + reach) / gcell); y++)
							for (int64_t z = (int64_t)std::floor((q.z - reach) / gcell); z <= (int64_t)std::floor((q.z + reach) / gcell); z++)
							{
								auto it = grid.find(gkey(x, y, z));
								if (it == grid.end())
									continue;
								for (Int32 tt : it->second)
								{
									if (region[(size_t)tt] < 0)
										continue;
									const FrTriMesh::Tri& tr = mesh.tris[(size_t)tt];
									const double d = FrPointTriDist(q, mesh.verts[(size_t)tr.v[0]],
										mesh.verts[(size_t)tr.v[1]], mesh.verts[(size_t)tr.v[2]]);
									if (d < bd)
									{
										bd = d;
										best = tt;
									}
								}
							}
					if (best < 0)
						continue;
					tree.p.push_back(q);
					tree.w.push_back(0.0);
					tree.lab.push_back(region[(size_t)best]);
					double dep = crackDepth != nullptr && ci < crackDepth->size() ? (*crackDepth)[ci] : 0.0;
					                                                                  
					                                                           
					                                                                  
					if (capDepth > 0.0) dep = 0.0;
					if (dep > 0.0)
					{
						const m::vec3 inward = FrNorm(N[i]) * -1.0;
						const double stepIn = std::max(hSeed, dep / 48.0);
						for (double d = stepIn; d <= dep + 1e-9; d += stepIn)
						{
							tree.p.push_back(q + inward * d);
							tree.w.push_back(0.0);
							tree.lab.push_back(region[(size_t)best]);
						}
					}
				}
			}
		}
	}
	if (capDepth > 0.0)
	{
		                                                                        
		                                                                     
		                                                                        
		const size_t strideC = std::max<size_t>(1, nT / 40000);
		for (size_t t = 0; t < nT; t += strideC)
		{
			tree.p.push_back(cen[t] - FrNorm(mesh.nrm[t]) * (2.0 * capDepth));
			tree.w.push_back(0.0);
			tree.lab.push_back(body);
		}
		for (const m::vec3& q : deepAt)
		{
			tree.p.push_back(q);
			tree.w.push_back(0.0);
			tree.lab.push_back(body);
		}
	}
	                                                                               
	FrSeedTree surf;
	double fillSize = 0.0, warpAmp = 0.0, warpFade0 = 0.0, warpFade1 = 1.0;
	if (fill != nullptr)
	{
		surf.p = cen;
		surf.w.assign(nT, 0.0);
		surf.lab.assign(nT, 0);
		surf.Build();
		                                                         
		std::vector<double> sz;
		for (Int32 R = 0; R < nReg; R++)
			if (R != body && regA[(size_t)R] > 0.0)
				sz.push_back(std::sqrt(regA[(size_t)R]));
		std::sort(sz.begin(), sz.end());
		const m::vec3 ext = hi - lo;
		const double boxV = std::max(ext.x, 1e-6) * std::max(ext.y, 1e-6) * std::max(ext.z, 1e-6);
		fillSize = fill->size > 0.0 ? fill->size : (sz.empty() ? std::cbrt(boxV / 64.0) : sz[sz.size() / 2]);
		fillSize = std::max({ fillSize, std::cbrt(boxV / FR_FILL_MAX_PIECES), 1e-3 * m::la::length(ext) });
		                                                                       
		std::vector<double> ds;
		if (crackDepth != nullptr)
			ds.assign(crackDepth->begin(), crackDepth->end());
		std::sort(ds.begin(), ds.end());
		warpFade0 = ds.empty() ? 0.0 : ds[ds.size() / 2];
		warpFade1 = std::max(0.35 * fillSize, 1e-6);
		warpAmp = std::max(0.0, std::min(1.0, fill->rough)) * FR_FILL_ROUGH_AMP * fillSize;
		if (fill->bvh != nullptr)
		{
			auto rnd = [&](uint64_t k, uint64_t c)
			{
				uint64_t z = ((uint64_t)seed + 0x46494c4cULL) ^ (k * 0x9e3779b97f4a7c15ULL) ^ ((c + 1) * 0xd1b54a32d192ed03ULL);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
				z ^= z >> 31;
				return (double)(z >> 11) * (1.0 / 9007199254740992.0);
			};
			const EVec dir = FrNorm(m::vec3(0.5773, 0.6123, 0.5401));
			const double eps = 1e-7 * m::la::length(ext) + 1e-9;
			auto inside = [&](const m::vec3& p)
			{
				m::vec3 o = p;
				int hits = 0;
				for (int guard = 0; guard < 256; ++guard)
				{
					double d = 0.0;
					if (!fill->bvh->Raycast(o, dir, 1e30, &d))
						break;
					++hits;
					o = o + dir * (d + eps);
				}
				return (hits & 1) == 1;
			};
			const int gx = std::max(1, (int)std::ceil(ext.x / fillSize));
			const int gy = std::max(1, (int)std::ceil(ext.y / fillSize));
			const int gz = std::max(1, (int)std::ceil(ext.z / fillSize));
			const Int32 first = nReg;
			uint64_t k = 0;
			for (int z = 0; z < gz; z++)
				for (int y = 0; y < gy; y++)
					for (int x = 0; x < gx; x++, k++)
					{
						const m::vec3 p = lo + m::vec3(ext.x * (x + 0.2 + 0.6 * rnd(k, 0)) / gx,
							ext.y * (y + 0.2 + 0.6 * rnd(k, 1)) / gy, ext.z * (z + 0.2 + 0.6 * rnd(k, 2)) / gz);
						double d = std::numeric_limits<double>::infinity();
						Int32 s0 = -1;
						surf.Query(0, p, -2, d, s0);
						if (s0 < 0 || d < FR_FILL_DEPTH * fillSize || !inside(p))
							continue;
						tree.p.push_back(p);
						tree.w.push_back(0.0);
						tree.lab.push_back(nReg++);
					}
			regA.resize((size_t)nReg, 0.0);
			(void)first;
		}
	}
	                                                                          
	                                                                             
	auto W = [&](const m::vec3& q) -> m::vec3
	{
		if (warpAmp <= 0.0)
			return q;
		double d = std::numeric_limits<double>::infinity();
		Int32 s0 = -1;
		surf.Query(0, q, -2, d, s0);
		const double t = std::max(0.0, std::min(1.0, (d - warpFade0) / warpFade1));
		if (t <= 0.0)
			return q;
		const double a = warpAmp * t * t * (3.0 - 2.0 * t);
		const m::vec3 u = q / (FR_FILL_ROUGH_FREQ * fillSize);
		return q + m::vec3(FrFbm(u, 1), FrFbm(u, 2), FrFbm(u, 3)) * a;
	};
	tree.Build();
	                                                                          
	                                                                    
	std::vector<FrSeedTree> own((size_t)nReg);
	for (size_t s = 0; s < tree.p.size(); s++)
	{
		FrSeedTree& T = own[(size_t)tree.lab[s]];
		T.p.push_back(tree.p[s]);
		T.w.push_back(tree.w[s]);
		T.lab.push_back(tree.lab[s]);
	}
	ProjectDamageParallelFor(0, nReg, [&](int64_t R) { own[(size_t)R].Build(); });
	auto nearestOf = [&](Int32 R, const m::vec3& q, double& d, m::vec3& at)
	{
		const FrSeedTree& T = own[(size_t)R];
		d = std::numeric_limits<double>::infinity();
		Int32 s = -1;
		if (!T.p.empty())
			T.Query(0, q, -2, d, s);
		if (s >= 0)
			at = T.p[(size_t)s];
		return s >= 0;
	};

	                                                                             
	                                                                           
	                                                                           
	                                                                      
	                                                                           
	                                                                          
	                                                                         
	                     
	const m::vec3 ext0 = hi - lo;
	const double boxVol = std::max(ext0.x, 1e-6) * std::max(ext0.y, 1e-6) * std::max(ext0.z, 1e-6);
	const double hCoarse = std::max(std::cbrt(boxVol / FR_CELL_SAMPLES), 1e-3 * m::la::length(ext0));
	                                                                            
	const m::vec3 orgC = lo - m::vec3(2.0 * hCoarse);
	const int cx = (int)std::ceil((ext0.x + 4.0 * hCoarse) / hCoarse) + 1;
	const int cy = (int)std::ceil((ext0.y + 4.0 * hCoarse) / hCoarse) + 1;
	const int cz = (int)std::ceil((ext0.z + 4.0 * hCoarse) / hCoarse) + 1;
	std::vector<Int32> LC;
	if (capDepth <= 0.0)
	{
		LC.resize((size_t)cx * cy * cz);
		ProjectDamageParallelFor(0, cz, [&](int64_t zi)
		{
			const int z = (int)zi;
			for (int y = 0; y < cy; y++)
				for (int x = 0; x < cx; x++)
				{
					double d1 = std::numeric_limits<double>::infinity();
					Int32 a = -1;
					tree.Query(0, W(orgC + m::vec3(x, y, z) * hCoarse), -2, d1, a);
					LC[((size_t)z * cy + y) * cx + x] = a >= 0 ? tree.lab[(size_t)a] : -1;
				}
		});
	}
	auto coarseHas = [&](const m::vec3& c, double r, Int32 R)
	{
		const m::vec3 u0 = (c - m::vec3(r) - orgC) / hCoarse, u1 = (c + m::vec3(r) - orgC) / hCoarse;
		for (int z = std::max(0, (int)std::floor(u0.z)); z <= std::min(cz - 1, (int)std::ceil(u1.z)); z++)
			for (int y = std::max(0, (int)std::floor(u0.y)); y <= std::min(cy - 1, (int)std::ceil(u1.y)); y++)
				for (int x = std::max(0, (int)std::floor(u0.x)); x <= std::min(cx - 1, (int)std::ceil(u1.x)); x++)
					if (LC[((size_t)z * cy + y) * cx + x] == R)
						return true;
		return false;
	};
	                                                             
	std::vector<m::vec3> bLo((size_t)nReg, m::vec3(std::numeric_limits<double>::infinity()));
	std::vector<m::vec3> bHi((size_t)nReg, m::vec3(-std::numeric_limits<double>::infinity()));
	auto grow = [&](Int32 R, const m::vec3& a)
	{
		bLo[(size_t)R] = m::la::min(bLo[(size_t)R], a);
		bHi[(size_t)R] = m::la::max(bHi[(size_t)R], a);
	};
	for (size_t s = 0; s < tree.p.size(); s++)
		grow(tree.lab[s], tree.p[s]);
	if (!LC.empty())
		for (int z = 0; z < cz; z++)
			for (int y = 0; y < cy; y++)
				for (int x = 0; x < cx; x++)
				{
					const Int32 R = LC[((size_t)z * cy + y) * cx + x];
					if (R >= 0)
						grow(R, orgC + m::vec3(x, y, z) * hCoarse);
				}
	                                                                          
	                                                                       
	const double reach = capDepth > 0.0 ? 2.0 * capDepth : 2.0 * hCoarse;
	const m::vec3 gLo = lo - m::vec3(reach), gHi = hi + m::vec3(reach);
	for (Int32 R = 0; R < nReg; R++)
		if (bLo[(size_t)R].x <= bHi[(size_t)R].x)
		{
			bLo[(size_t)R] = m::la::max(bLo[(size_t)R] - m::vec3(reach), gLo);
			bHi[(size_t)R] = m::la::min(bHi[(size_t)R] + m::vec3(reach), gHi);
		}

	                                                                           
	                                                                         
	                                                                        
	                                         
	std::vector<double> ws(crackWidth.begin(), crackWidth.end());
	std::nth_element(ws.begin(), ws.begin() + (std::ptrdiff_t)(ws.size() / 2), ws.end());
	const double medW = ws.empty() ? hCoarse : ws[ws.size() / 2];
	double h = std::min(hCoarse, FR_CELL_WIDTH_STEP * std::max(medW, 1e-6));
	if (capDepth > 0.0)
		h = std::min(h, capDepth / 3.0);
	else
		h = std::max(h, hCoarse / 3.0);                                              
	const int BS = FR_CELL_BLOCK;
	struct RegionGrid
	{
		int x0 = 0, y0 = 0, z0 = 0, nbx = 0, nby = 0, nbz = 0;
		std::vector<Int32> blk;                                                
		std::vector<Int32> lab;                                               
		std::vector<float> f;                                  
		size_t used = 0;
	};
	std::vector<RegionGrid> grids((size_t)nReg);
	const m::vec3 org = lo;
	auto layout = [&](double hh) -> size_t
	{
		size_t total = 0;
		for (Int32 R = 0; R < nReg; R++)
		{
			RegionGrid& G = grids[(size_t)R];
			G = RegionGrid();
			if (R == body || !(bLo[(size_t)R].x < bHi[(size_t)R].x))
				continue;
			G.x0 = (int)std::floor((bLo[(size_t)R].x - org.x) / hh);
			G.y0 = (int)std::floor((bLo[(size_t)R].y - org.y) / hh);
			G.z0 = (int)std::floor((bLo[(size_t)R].z - org.z) / hh);
			G.nbx = (int)std::ceil((bHi[(size_t)R].x - org.x) / hh - G.x0 + 1) / BS + 1;
			G.nby = (int)std::ceil((bHi[(size_t)R].y - org.y) / hh - G.y0 + 1) / BS + 1;
			G.nbz = (int)std::ceil((bHi[(size_t)R].z - org.z) / hh - G.z0 + 1) / BS + 1;
			const size_t nb = (size_t)G.nbx * G.nby * G.nbz;
			G.blk.assign(nb, -1);
			std::vector<char> use(nb, 0);
			const double half = 0.5 * std::sqrt(3.0) * BS * hh;
			ProjectDamageParallelFor(0, (int64_t)nb, [&](int64_t b)
			{
				const int bx = (int)(b % G.nbx), by = (int)((b / G.nbx) % G.nby), bz = (int)(b / ((int64_t)G.nbx * G.nby));
				const m::vec3 c = org + (m::vec3(G.x0, G.y0, G.z0) + (m::vec3(bx, by, bz) + 0.5) * (double)BS - 0.5) * hh;
				double d;
				m::vec3 at;
				bool near = nearestOf(R, W(c), d, at) && d <= reach + half;
				if (!near && !LC.empty())
					near = coarseHas(c, half + hCoarse, R);
				use[(size_t)b] = near ? 1 : 0;
			});
			for (size_t b = 0; b < nb; b++)
				if (use[b])
					G.blk[b] = (Int32)G.used++;
			total += G.used;
		}
		return total * (size_t)(BS * BS * BS);
	};
	                                                                         
	                                    
	h = std::max(h, hCoarse / 16.0);
	size_t nPts = layout(h);
	for (int it = 0; it < 6 && (double)nPts > FR_CELL_FINE_SAMPLES; it++)
	{
		h *= 1.05 * std::cbrt((double)nPts / FR_CELL_FINE_SAMPLES);
		nPts = layout(h);
	}
	const double cap = 3.0 * h, gap = FR_CELL_GAP * h;

	                                                                      
	                                                                         
	                                                                        
	                                                    
	std::vector<m::Manifold> shells, cores;
	for (Int32 R = 0; R < nReg; R++)
	{
		RegionGrid& G = grids[(size_t)R];
		if (G.used == 0)
			continue;
		const size_t B3 = (size_t)(BS * BS * BS);
		G.lab.assign(G.used * B3, -1);
		std::vector<Int32> near1(G.used * B3, -1);
		std::vector<float> dist1(G.used * B3, 0.0f);
		G.f.assign(G.used * B3, (float)-cap);
		const int px = G.nbx * BS, py = G.nby * BS, pz = G.nbz * BS;
		                                                   
		auto slot = [&](int x, int y, int z) -> int64_t
		{
			if (x < 0 || y < 0 || z < 0 || x >= px || y >= py || z >= pz)
				return -1;
			const Int32 b = G.blk[((size_t)(z / BS) * G.nby + (y / BS)) * G.nbx + (x / BS)];
			if (b < 0)
				return -1;
			return (int64_t)b * (int64_t)B3 + ((z % BS) * BS + (y % BS)) * BS + (x % BS);
		};
		std::vector<int64_t> blocks;
		for (size_t b = 0; b < G.blk.size(); b++)
			if (G.blk[b] >= 0)
				blocks.push_back((int64_t)b);
		auto forPoints = [&](auto&& fn)
		{
			ProjectDamageParallelFor(0, (int64_t)blocks.size(), [&](int64_t k)
			{
				const int64_t b = blocks[(size_t)k];
				const int bx = (int)(b % G.nbx), by = (int)((b / G.nbx) % G.nby), bz = (int)(b / ((int64_t)G.nbx * G.nby));
				for (int z = bz * BS; z < bz * BS + BS; z++)
					for (int y = by * BS; y < by * BS + BS; y++)
						for (int x = bx * BS; x < bx * BS + BS; x++)
							fn(x, y, z, (size_t)slot(x, y, z),
								W(org + m::vec3(G.x0 + x, G.y0 + y, G.z0 + z) * h));
			});
		};
		forPoints([&](int, int, int, size_t i, const m::vec3& q)
		{
			double d1 = std::numeric_limits<double>::infinity();
			Int32 a = -1;
			tree.Query(0, q, -2, d1, a);
			G.lab[i] = a >= 0 ? tree.lab[(size_t)a] : -1;
			near1[i] = a;
			dist1[i] = (float)d1;
		});
		forPoints([&](int x, int y, int z, size_t i, const m::vec3& q)
		{
			if (G.lab[i] == R)
			{
				const double d1 = dist1[i];
				const Int32 a = near1[i];
				double d2 = std::numeric_limits<double>::infinity();
				Int32 b = -1;
				tree.Query(0, q, R, d2, b);
				double rt = 1.0;
				if (a >= 0 && b >= 0)
					rt = std::max(m::la::length(FrNorm(q - tree.p[(size_t)b]) - FrNorm(q - tree.p[(size_t)a])), 0.1);
				G.f[i] = (float)(b >= 0 ? std::min((d2 - d1) / rt, cap) : cap);
				return;
			}
			bool nearR = false;
			for (int dz = -1; dz <= 1 && !nearR; dz++)
				for (int dy = -1; dy <= 1 && !nearR; dy++)
					for (int dx = -1; dx <= 1 && !nearR; dx++)
					{
						const int64_t j = slot(x + dx, y + dy, z + dz);
						if (j >= 0 && G.lab[(size_t)j] == R)
							nearR = true;
					}
			if (!nearR)
				return;
			const double d1 = dist1[i];
			const Int32 a = near1[i];
			double dR;
			m::vec3 atR;
			if (a < 0 || !nearestOf(R, q, dR, atR))
				return;
			                                                                     
			                                             
			double d2 = std::numeric_limits<double>::infinity();
			Int32 b = -1;
			tree.Query(0, q, G.lab[i], d2, b);
			const m::vec3 pb = b >= 0 ? tree.p[(size_t)b] : atR;
			const double rt = std::max(m::la::length(FrNorm(q - pb) - FrNorm(q - tree.p[(size_t)a])), 0.1);
			G.f[i] = (float)(-std::min((dR - d1) / rt, cap));
		});
		                                                                       
		                                                                     
		                                                            
		const uint64_t qx = (uint64_t)px + 2, qy = (uint64_t)py + 2;
		auto pid = [&](int x, int y, int z) { return ((uint64_t)(z + 1) * qy + (uint64_t)(y + 1)) * qx + (uint64_t)(x + 1); };
		auto val = [&](int x, int y, int z) -> double
		{
			const int64_t j = slot(x, y, z);
			return j >= 0 ? (double)G.f[(size_t)j] : -cap;
		};
		                                                                     
		const int sbx = G.nbx + 1, sby = G.nby + 1, sbz = G.nbz + 1;
		std::vector<char> visit((size_t)sbx * sby * sbz, 0);
		for (int64_t b : blocks)
		{
			const int bx = (int)(b % G.nbx), by = (int)((b / G.nbx) % G.nby), bz = (int)(b / ((int64_t)G.nbx * G.nby));
			for (int c = 0; c < 8; c++)
				visit[((size_t)(bz + 1 - ((c >> 2) & 1)) * sby + (by + 1 - ((c >> 1) & 1))) * sbx + (bx + 1 - (c & 1))] = 1;
		}
		std::vector<int64_t> vb;
		for (size_t b = 0; b < visit.size(); b++)
			if (visit[b])
				vb.push_back((int64_t)b);
		auto march = [&](double level) -> m::Manifold
		{
			auto g = [&](int x, int y, int z)
			{
				const double v = val(x, y, z) - level;
				return v == 0.0 ? -1e-12 : v;
			};
			auto vpos = [&](uint64_t key) -> m::vec3
			{
				const uint64_t p = key >> 3;
				const int d = (int)(key & 7);
				const int x = (int)(p % qx) - 1, y = (int)((p / qx) % qy) - 1, z = (int)(p / (qx * qy)) - 1;
				const int x2 = x + (d & 1), y2 = y + ((d >> 1) & 1), z2 = z + ((d >> 2) & 1);
				const double ga = g(x, y, z), gb = g(x2, y2, z2);
				const double t = ga / (ga - gb);
				return org + (m::vec3(G.x0 + x, G.y0 + y, G.z0 + z) + m::vec3(x2 - x, y2 - y, z2 - z) * t) * h;
			};
			std::vector<std::vector<uint64_t>> tris(vb.size());
			static const int perm[6][2] = { { 1, 2 }, { 1, 4 }, { 2, 1 }, { 2, 4 }, { 4, 1 }, { 4, 2 } };
			ProjectDamageParallelFor(0, (int64_t)vb.size(), [&](int64_t k)
			{
				const int64_t b = vb[(size_t)k];
				const int bx = (int)(b % sbx) - 1, by = (int)((b / sbx) % sby) - 1, bz = (int)(b / ((int64_t)sbx * sby)) - 1;
				std::vector<uint64_t>& out = tris[(size_t)k];
				for (int z = bz * BS; z < bz * BS + BS; z++)
					for (int y = by * BS; y < by * BS + BS; y++)
						for (int x = bx * BS; x < bx * BS + BS; x++)
						{
							double gc[8];
							int pos = 0;
							for (int c = 0; c < 8; c++)
							{
								gc[c] = g(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
								pos += gc[c] > 0.0;
							}
							if (pos == 0 || pos == 8)
								continue;
							auto ekey = [&](int ca, int cb)
							{
								const int lo_ = ca & cb, d = ca ^ cb;                                              
								return (pid(x + (lo_ & 1), y + ((lo_ >> 1) & 1), z + ((lo_ >> 2) & 1)) << 3) | (uint64_t)d;
							};
							auto cpos = [&](int c) { return m::vec3(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1)); };
							auto emit = [&](uint64_t a, uint64_t b2, uint64_t c, const m::vec3& dirOut)
							{
								const m::vec3 pa = vpos(a), pb = vpos(b2), pc = vpos(c);
								if (m::la::dot(m::la::cross(pb - pa, pc - pa), dirOut) < 0.0)
									std::swap(b2, c);
								out.push_back(a); out.push_back(b2); out.push_back(c);
							};
							for (const auto& pr : perm)
							{
								const int tv[4] = { 0, pr[0], pr[0] | pr[1], 7 };
								int in[4], on[4], ni = 0, no = 0;
								for (int i = 0; i < 4; i++)
									(gc[tv[i]] > 0.0 ? in[ni++] : on[no++]) = tv[i];
								if (ni == 0 || no == 0)
									continue;
								m::vec3 ci(0.0), co(0.0);
								for (int i = 0; i < ni; i++) ci += cpos(in[i]);
								for (int i = 0; i < no; i++) co += cpos(on[i]);
								const m::vec3 dirOut = co / (double)no - ci / (double)ni;
								if (ni == 1 || no == 1)
								{
									const int s = ni == 1 ? in[0] : on[0];
									const int* o = ni == 1 ? on : in;
									emit(ekey(s, o[0]), ekey(s, o[1]), ekey(s, o[2]), dirOut);
								}
								else
								{
									const uint64_t ac = ekey(in[0], on[0]), ad = ekey(in[0], on[1]);
									const uint64_t bd = ekey(in[1], on[1]), bc = ekey(in[1], on[0]);
									emit(ac, ad, bd, dirOut);
									emit(ac, bd, bc, dirOut);
								}
							}
						}
			});
			m::MeshGL64 mg;
			mg.numProp = 3;
			std::unordered_map<uint64_t, uint64_t> vid;
			for (const std::vector<uint64_t>& part : tris)
				for (uint64_t key : part)
				{
					auto it = vid.find(key);
					if (it == vid.end())
					{
						it = vid.emplace(key, (uint64_t)(mg.vertProperties.size() / 3)).first;
						const m::vec3 p = vpos(key);
						mg.vertProperties.push_back(p.x);
						mg.vertProperties.push_back(p.y);
						mg.vertProperties.push_back(p.z);
					}
					mg.triVerts.push_back(it->second);
				}
			if (mg.triVerts.empty())
				return m::Manifold();
			m::Manifold res(mg);
			return res.Status() == m::Manifold::Error::NoError ? res : m::Manifold();
		};
		                                                                        
		                                                                
		m::Manifold core = march(gap);
		m::Manifold outer = march(-gap);
		const double simp = FR_CELL_SIMPLIFY;
		if (simp > 0.0) core = core.Simplify(simp * h);
		if (simp > 0.0) outer = outer.Simplify(simp * h);
		                                                                    
		                          
		std::vector<m::Manifold> parts = core.Decompose();
		if (parts.size() > 1)
		{
			double big = 0.0;
			for (const m::Manifold& q : parts)
				big = std::max(big, q.Volume());
			std::vector<m::Manifold> keep;
			for (m::Manifold& q : parts)
				if (q.Volume() >= std::max(8.0 * h * h * h, 0.02 * big))
					keep.push_back(std::move(q));
			core = m::Manifold::BatchBoolean(keep, m::OpType::Add);
		}
		G.lab.clear(); G.lab.shrink_to_fit();
		G.f.clear(); G.f.shrink_to_fit();
		if (outer.IsEmpty())
			continue;
		shells.push_back(std::move(outer));
		if (!core.IsEmpty())
			cores.push_back(std::move(core));
	}
	if (shells.empty())
		return m::Manifold();
	return m::Manifold::BatchBoolean(shells, m::OpType::Add) - m::Manifold::BatchBoolean(cores, m::OpType::Add);
}

struct FrImpact { m::vec3 pos, nrm; Int32 tri = -1; double radius = 0.0, strength = 1.0; };
}

namespace project_damage {
static void FrSubdivideLevel(const FrTriMesh& mesh, FrHash& skel, const FrHash& edgeHash,
	const std::vector<bool>* avoidTri, std::vector<FrEntry>& allPoints,
	std::vector<FrCrack>& cracks, Int32 g, Int32 subdivPasses, double ns, double safetyCap,
	const ProjectDamageParams& params, const ProjectDamageHost& host, PyRandom& rngSubdiv)
{
	std::vector<double> baseLens;
	for (const FrCrack& c : cracks)
	{
		if (c.gen != g)
			continue;
		double len = 0.0;
		for (size_t i = 0; i + 1 < c.pts.size(); i++)
			len += m::la::length(c.pts[i + 1] - c.pts[i]);
		baseLens.push_back(len);
	}
	if (!baseLens.empty())
	{
		std::sort(baseLens.begin(), baseLens.end());
		const double freeRunCeiling = 1.5 * baseLens[baseLens.size() / 2];
		const double resultCeiling = 2.0 * freeRunCeiling;
		const double lenCap = params.fract[(size_t)(g - 1)].length > 0.0
			? std::min(safetyCap, params.fract[(size_t)(g - 1)].length) : safetyCap;
		double subdivStep = freeRunCeiling / 8.0;
		if (subdivStep < 0.5 * ns) subdivStep = 0.5 * ns;
		if (subdivStep > FR_STEP * ns) subdivStep = FR_STEP * ns;

		for (int pass = 0; pass < subdivPasses; pass++)
		{
			HostStatus(host, "fracture level " + std::to_string(g)
				+ " subdivide " + std::to_string(pass + 1)
				+ "/" + std::to_string(subdivPasses), 30 + g * 5);
			Int32 levelSoFar = 0;
			for (const FrCrack& c : cracks)
				if (c.gen == g)
					levelSoFar++;
			std::vector<size_t> levelPtIdx;
			for (size_t i = 0; i < allPoints.size(); i++)
				if (allPoints[i].gen == g)
					levelPtIdx.push_back(i);

			const Int32 passTarget = levelSoFar;
			const Int32 passBudget = passTarget * 10;
			Int32 passAccepted = 0, passAttempts = 0;
			while (passAccepted < passTarget && passAttempts < passBudget)
			{
				passAttempts++;
				const FrEntry& ent = allPoints[levelPtIdx[(size_t)FrRandRange(rngSubdiv, (int)levelPtIdx.size())]];
				const m::vec3 n0 = ent.nrm;
				m::vec3 side = FrNorm(m::la::cross(n0, ent.tan));
				if (rngSubdiv.Random() < 0.5)
					side = side * -1.0;
				const m::vec3 pos0 = ent.pos;
				const Int32 tri0 = ent.tri;
				const Int32 parent = ent.crack;
				const m::vec3 dir0 = RotAxis(side, n0,
					rngSubdiv.Uniform(-FR_START_JIT_DEG, FR_START_JIT_DEG) * m::kPi / 180.0);

				const double room = FR_SNAP_R * ns;
				FrEntry qr;
				double qrd = 0.0;
				if (skel.Nearest(pos0, room, &n0,
						[&](const FrEntry& e) { return e.crack == parent; }, qr, qrd))
					continue;

				const double run = FrFreeRun(skel, edgeHash, pos0, dir0, parent, n0, ns, safetyCap, subdivStep);
				if (run > freeRunCeiling)
					continue;

				FrCrack c;
				c.gen = g;
				c.bornOn = parent;
				FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri0, pos0, dir0,
					(Int32)cracks.size(), parent, lenCap, 2.6 * freeRunCeiling,
					FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0,
					ns, rngSubdiv, c, subdivStep);
				if (c.pts.size() < 4 || !c.hit)
					continue;
				double grownLen = 0.0;
				for (size_t i = 0; i + 1 < c.pts.size(); i++)
					grownLen += m::la::length(c.pts[i + 1] - c.pts[i]);
				if (grownLen > resultCeiling)
					continue;
				FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
				cracks.push_back(std::move(c));
				passAccepted++;
			}
			if (passAccepted < passTarget)
				HostLog(host, "[ProjectDamage] Fracture level " + std::to_string(g)
					+ " subdivide pass " + std::to_string(pass + 1) + ": placed "
					+ std::to_string(passAccepted) + " of " + std::to_string(passTarget)
					+ " cracks (out of room)");
		}
	}
}

void PlaceFractureVolume(MeshView meshView, const EVec* pts,
	const std::vector<EVec>& normals, const std::vector<Chain>& chains,
	const ProjectDamageParams& params, const ProjectDamageHost& host,
	const std::vector<bool>* avoidPoly,
	const std::vector<std::vector<EVec>>* guideSplines,
	std::vector<m::Manifold>& crackCutters,
	std::vector<Int32>& crackLevels, std::vector<m::Manifold>& spallCutters,
	const std::vector<Int32>* guideLevels)
{
	const bool haveGuides = guideSplines != nullptr && !guideSplines->empty();
	if (params.fractPaintOnly && !haveGuides) return;
	                                                                       
	                                                                         
	const bool haveRadial = ((params.fractProxMode == 2 || params.fractProxMode == 3) && !params.fractProximity.empty())
		|| ((params.fractThrough || params.fractPhysics) && !params.fractHelperStrokes.empty());
	if (!haveGuides && !haveRadial && params.fract[0].amount <= 0 && params.fract[1].amount <= 0
		&& params.fract[2].amount <= 0)
		return;

	FrTriMesh mesh;
	FrBuildTriMesh(meshView, mesh);
	if (mesh.tris.empty() || mesh.area <= 0.0)
		return;
	std::vector<bool> avoidTriV;
	const std::vector<bool>* avoidTri = nullptr;
	if (avoidPoly != nullptr)
	{
		avoidTriV.resize(mesh.tris.size(), false);
		for (size_t t = 0; t < mesh.tris.size(); t++)
		{
			const Int32 src = mesh.tris[t].src;
			if (src >= 0 && src < (Int32)avoidPoly->size())
				avoidTriV[t] = (*avoidPoly)[(size_t)src];
		}
		avoidTri = &avoidTriV;
	}
	const double ns = FrNetScale(mesh.area);
	                                                                    
	                                                                        
	                                                                     
	if ((params.fractThrough || params.fractPhysics) && params.mask != nullptr && params.mask->Active())
	{
		if (avoidTriV.empty()) avoidTriV.resize(mesh.tris.size(), false);
		bool anyAllowed = false;
		for (size_t t = 0; t < mesh.tris.size(); ++t)
		{
			const auto& tr = mesh.tris[t];
			const EVec center = (mesh.verts[(size_t)tr.v[0]] + mesh.verts[(size_t)tr.v[1]] + mesh.verts[(size_t)tr.v[2]]) / 3.0;
			if (params.mask->FadeScale(center) <= 1e-6) avoidTriV[t] = true;
			anyAllowed = anyAllowed || !avoidTriV[t];
		}
		if (!anyAllowed) return;
		avoidTri = &avoidTriV;
	}
	const ProjectDamagePoly* polysR = meshView.polys;

	struct FrEdgeSample { m::vec3 pos, tan; Int32 faces[2]; };
	std::vector<FrEdgeSample> edgeSamples;
	std::vector<m::vec3> edgePts;
	std::map<std::pair<Int32, Int32>, ExposedEdge*> emap = BuildEdgeMap(chains);
	std::vector<m::vec3> objCorners;
	for (const Chain& ch : chains)
	{
		const std::vector<Int32>& P = ch.points;
		if (P.size() >= 2 && P.front() != P.back())
		{
			objCorners.push_back(pts[P.front()]);
			objCorners.push_back(pts[P.back()]);
		}
		                                                                        
		                                                                       
		                                                                       
		                                                             
		const double tanWin = FR_EDGE_SAMPLE * ns;
		std::vector<double> arcAt(P.size(), 0.0);
		for (size_t i = 1; i < P.size(); i++)
			arcAt[i] = arcAt[i - 1] + m::la::length(pts[P[i]] - pts[P[i - 1]]);
		for (size_t i = 0; i + 1 < P.size(); i++)
		{
			const m::vec3 pa = pts[P[i]], pb = pts[P[i + 1]];
			const double L = m::la::length(pb - pa);
			if (L < 1e-9)
				continue;
			size_t i0 = i, i1 = i + 1;
			while (i0 > 0 && arcAt[i] - arcAt[i0] < tanWin) i0--;
			while (i1 + 1 < P.size() && arcAt[i1] - arcAt[i + 1] < tanWin) i1++;
			const m::vec3 span = pts[P[i1]] - pts[P[i0]];
			const m::vec3 tan = m::la::length(span) > 1e-9 && m::la::dot(span, pb - pa) > 0.0
				? FrNorm(span) : (pb - pa) / L;
			auto it = emap.find(EdgeKey(P[i], P[i + 1]));
			Int32 f0 = -1, f1 = -1;
			if (it != emap.end())
			{
				for (const auto& r : it->second->reprs)
				{
					if (f0 < 0) f0 = r.first;
					else if (r.first != f0 && f1 < 0) f1 = r.first;
				}
			}
			const int nS = MaxI(1, (int)(L / (FR_EDGE_SAMPLE * ns)));
			for (int k = 0; k < nS; k++)
			{
				FrEdgeSample s;
				s.pos = pa + (pb - pa) * ((k + 0.5) / nS);
				s.tan = tan;
				s.faces[0] = f0;
				s.faces[1] = f1;
				edgeSamples.push_back(s);
				edgePts.push_back(s.pos);
			}
		}
	}

	auto faceCenter = [&](Int32 face)
	{
		const ProjectDamagePoly& cp = polysR[face];
		m::vec3 c = pts[cp.a] + pts[cp.b] + pts[cp.c];
		double n = 3.0;
		if (cp.c != cp.d) { c = c + pts[cp.d]; n = 4.0; }
		return c / n;
	};
	auto faceArea = [&](Int32 face)
	{
		const ProjectDamagePoly& cp = polysR[face];
		const m::vec3 a = pts[cp.a], b = pts[cp.b], cc = pts[cp.c];
		double ar = m::la::length(m::la::cross(b - a, cc - a)) * 0.5;
		if (cp.c != cp.d)
		{
			const m::vec3 d = pts[cp.d];
			ar += m::la::length(m::la::cross(cc - a, d - a)) * 0.5;
		}
		return ar;
	};

	PyRandom rngNet((uint32_t)(params.seed + 7000));
	PyRandom rngSubdiv((uint32_t)(params.seed + 7002));
	const int recipe = params.fractRecipe >= 2 ? 2 : 1;
	PyRandom rngLevel[3] = { PyRandom((uint32_t)(params.seed + 7010)),
		PyRandom((uint32_t)(params.seed + 7020)), PyRandom((uint32_t)(params.seed + 7030)) };
	PyRandom rngProx((uint32_t)(params.seed + 7003));
	ProjectDamageBVH bvh;
	                                                                           
	const bool capDepth = recipe >= 2 && params.fractThicknessCap > 0.0 && !params.fractThrough && !params.fractPhysics;
	if (capDepth)
		bvh.Build(meshView);
	FrHash skel;
	skel.cell = FR_ATTRACT_R * ns;
	FrHash edgeHash;
	edgeHash.cell = FR_ATTRACT_R * ns;
	for (const FrEdgeSample& es : edgeSamples)
	{
		FrEntry e;
		e.pos = es.pos;
		e.tan = es.tan;
		edgeHash.Add(e);
	}
	std::vector<FrEntry> allPoints;
	std::vector<FrCrack> cracks;

	if (haveGuides)
	{
		const double guideStep = FR_CUT_STEP * ns / params.fractDetail;
		for (size_t gi = 0; gi < guideSplines->size(); ++gi)
		{
			const auto& guide = (*guideSplines)[gi];
			FrCrack c;
			c.gen = guideLevels ? (*guideLevels)[gi] + 1 : 1;
			double step = guideStep;
			if (params.fractPaintOnly)
			{
				double length = 0.0;
				for (size_t i = 1; i < guide.size(); i++) length += m::la::length(guide[i] - guide[i - 1]);
				step = std::min(step, length / 8.0);
			}
			if (!FrGuidedCrack(mesh, guide, step, c))
				continue;
			if (params.fractPaintOnly)
			{
				const auto& lvl = params.fract[(size_t)(c.gen - 1)];
				const double radius = MaxD(lvl.paintRadius, 0.04);
				bool valid = true;
				for (size_t i = 0; i < c.pts.size(); ++i)
				{
					if ((avoidTri && (*avoidTri)[(size_t)c.tris[i]])
						|| m::la::length(c.pts[i] - FrClosestOnPolyline(c.pts[i], guide)) > radius * 0.5)
						valid = false;
				}
				if (!valid || lvl.width <= 0.0 || lvl.depth <= 0.0) continue;
				c.widthOverride = std::min(lvl.width, radius / 1.3);
				c.noSpall = true;
			}
			FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
			cracks.push_back(std::move(c));
		}
		if (cracks.empty())
			HostLog(host, "[ProjectDamage] Fracture: no guide spline reached the mesh");
	}

	std::vector<FrImpact> impacts;
	                                                                          
	                                                                            
	const Int32 fracType = params.fractPaintOnly ? 0 : std::max(0, std::min(3, params.fractType));
	                                                                         
	                                                                   
	const Int32 proxMode = params.fractPaintOnly ? 0 : params.fractProxMode;
	const bool burst = fracType == 1 || fracType == 2;
	const bool glassStyle = fracType == 2;
	m::vec3 grain(params.fractGrainX, params.fractGrainY, params.fractGrainZ);
	const bool wood = fracType == 3 && m::la::length(grain) > 1e-9;
	if (wood) grain = FrNorm(grain);
	const double grainPull = wood ? std::min(1.0, MaxD(0.0, params.fractGrainPull)) : 0.0;
	const m::vec3* grainP = wood ? &grain : nullptr;
	if (proxMode > 0)
	{
		for (const FractProximity& fp : params.fractProximity)
		{
			FrImpact I;
			I.tri = FrNearestTri(mesh, m::vec3(fp.x, fp.y, fp.z), I.pos);
			if (I.tri < 0)
				continue;
			if (avoidTri != nullptr && (*avoidTri)[(size_t)I.tri])
				continue;
			I.nrm = mesh.nrm[(size_t)I.tri];
			I.radius = fp.radius > 0.0 ? fp.radius : params.fractProxRadius;
			I.strength = MaxD(0.0, fp.strength);
			if (I.radius > 0.0 && I.strength > 0.0)
				impacts.push_back(I);
		}
		if (impacts.empty() && !params.fractProximity.empty())
			HostLog(host, "[ProjectDamage] Fracture: no proximity point reached the mesh");
	}
	const double sigmaBase = FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0;

	if ((proxMode == 2 || proxMode == 3) && !impacts.empty())
	{
		const bool glass = proxMode == 3;
		HostStatus(host, glass ? "fracture glass impacts" : "fracture radial impacts", 30);
		const double guideStep = FR_CUT_STEP * ns / params.fractDetail;
		const double snapR = FR_SNAP_R * ns;
		auto styleCrack = [&](FrCrack& c)
		{
			if (!glass)
				return;
			c.widthOverride = params.fractGlassWidth;
			c.depthOverride = params.fractGlassDepth;
			c.reliefScale = 0.12;
			c.noSpall = true;
		};
		for (const FrImpact& I : impacts)
		{
			const int nSpokes = MaxI(3, glass ? params.fractGlassSpokes : params.fractRadialSpokes);
			const double R = I.radius;
			const double crushR = MaxD(0.0, glass ? params.fractGlassCrush : params.fractRadialCrush) * R;
			const double r0 = MaxD(crushR, 1.3 * snapR * nSpokes / (2.0 * m::kPi));
			m::vec3 t0, b0;
			FrFrame(I.nrm, t0, b0);
			const double phase = rngProx.Uniform(0.0, 2.0 * m::kPi);
			std::vector<FrCrack> spokes;
			std::vector<double> spokeAngle;
			for (int k = 0; k < nSpokes; k++)
			{
				const double th = phase + 2.0 * m::kPi * (k + rngProx.Uniform(-0.3, 0.3)) / nSpokes;
				const double len = R * (glass ? rngProx.Uniform(0.85, 1.35) : rngProx.Uniform(0.75, 1.2));
				Int32 tri = I.tri;
				m::vec3 pos = I.pos;
				m::vec3 dirn = t0 * std::cos(th) + b0 * std::sin(th);
				if (!FrMarch(mesh, tri, pos, dirn, r0))
					continue;
				if (avoidTri != nullptr && (*avoidTri)[(size_t)tri])
					continue;
				FrCrack c;
				c.gen = 1;
				FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri, pos, dirn,
					(Int32)(cracks.size() + spokes.size()), -1, len,
					glass ? 40.0 * len : 4.0 * len,
					(glass ? 0.15 : 0.45) * sigmaBase, ns, rngProx, c, -1.0, 0.5);
				if (c.pts.size() < 4)
					continue;
				c.startFree = true;
				styleCrack(c);
				spokes.push_back(std::move(c));
				spokeAngle.push_back(th);
			}
			std::vector<Int32> spokeId;
			for (FrCrack& c : spokes)
			{
				spokeId.push_back((Int32)cracks.size());
				FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
				cracks.push_back(std::move(c));
			}
			if (glass && params.fractGlassBranch > 0.0)
			{
				const size_t nParents = spokeId.size();
				for (size_t a = 0; a < nParents; a++)
				{
					const FrCrack& par = cracks[(size_t)spokeId[a]];
					double parLen = 0.0;
					for (size_t i = 0; i + 1 < par.pts.size(); i++)
						parLen += m::la::length(par.pts[i + 1] - par.pts[i]);
					for (int fork = 0; fork < 2; fork++)
					{
						const double chance = rngProx.Random();
						const double at = parLen * rngProx.Uniform(0.3, 0.75);
						const double ang = rngProx.Uniform(12.0, 35.0) * (rngProx.Random() < 0.5 ? -1.0 : 1.0);
						const double len = R * rngProx.Uniform(0.35, 0.8);
						if (chance >= MaxD(0.0, std::min(1.0, params.fractGlassBranch)))
							continue;
						double acc = 0.0;
						size_t i0 = 0;
						for (; i0 + 1 < par.pts.size(); i0++)
						{
							const double L = m::la::length(par.pts[i0 + 1] - par.pts[i0]);
							if (acc + L >= at) break;
							acc += L;
						}
						if (i0 + 1 >= par.pts.size())
							continue;
						const m::vec3 n = par.nrms[i0];
						const m::vec3 tan = FrNorm(par.pts[i0 + 1] - par.pts[i0]);
						const m::vec3 dir0 = RotAxis(tan, n, ang * m::kPi / 180.0);
						FrCrack c;
						c.gen = 1;
						c.bornOn = spokeId[a];
						FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, par.tris[i0], par.pts[i0], dir0,
							(Int32)cracks.size(), spokeId[a], len, 40.0 * len, 0.15 * sigmaBase, ns, rngProx, c, -1.0, 0.5);
						if (c.pts.size() < 4)
							continue;
						styleCrack(c);
						FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
						cracks.push_back(std::move(c));
					}
				}
			}
			const int nRings = MaxI(0, glass ? params.fractGlassRings : params.fractRadialRings);
			const double ringFill = glass ? params.fractGlassRingFill : params.fractRadialRingFill;
			const size_t ns_ = spokeId.size();
			for (int ring = 0; ring < nRings && ns_ >= 3; ring++)
			{
				const double u = (ring + 0.5) / nRings;
				const double rr = glass ? R * (0.1 + 0.85 * std::pow(u, 1.6))
										: R * (0.3 + 0.6 * u);
				for (size_t a = 0; a < ns_; a++)
				{
					const size_t b = (a + 1) % ns_;
					const double fill = rngProx.Random();
					const double ja = rngProx.Uniform(-0.08, 0.08), jb = rngProx.Uniform(-0.08, 0.08);
					const double bow = glass ? rngProx.Uniform(-0.01, 0.02) : rngProx.Uniform(0.03, 0.09);
					if (fill > ringFill)
						continue;
					double gap = spokeAngle[b] - spokeAngle[a];
					while (gap < 0.0) gap += 2.0 * m::kPi;
					if (gap > m::kPi)
						continue;
					m::vec3 pa, pb;
					if (!FrPointAtArc(cracks[(size_t)spokeId[a]], rr * (1.0 + ja), pa)
						|| !FrPointAtArc(cracks[(size_t)spokeId[b]], rr * (1.0 + jb), pb))
						continue;
					const m::vec3 mid0 = (pa + pb) * 0.5;
					m::vec3 out = mid0 - I.pos;
					out = FrNorm(out - I.nrm * m::la::dot(out, I.nrm));
					const m::vec3 mid = mid0 + out * (bow * m::la::length(pb - pa));
					FrCrack c;
					c.gen = 2;
					if (!FrGuidedCrack(mesh, { pa, mid, pb }, guideStep, c))
						continue;
					bool blocked = false;
					if (avoidTri != nullptr)
						for (Int32 t : c.tris)
							if ((*avoidTri)[(size_t)t]) { blocked = true; break; }
					if (blocked)
						continue;
					c.bornOn = spokeId[a];
					c.hitCrack = spokeId[b];
					c.hit = true;
					styleCrack(c);
					FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
					cracks.push_back(std::move(c));
				}
			}
			if (crushR > 1e-9)
			{
				const int nPits = 5 + (int)std::lround(4.0 * MaxD(0.0, std::min(1.0, crushR / (12.0 * ns))));
				for (int k = 0; k < nPits; k++)
				{
					const double rad = crushR * std::sqrt(rngProx.Random()) * 0.7;
					const double th = rngProx.Uniform(0.0, 2.0 * m::kPi);
					const double sz = crushR * rngProx.Uniform(0.3, 0.6);
					double dep = sz * rngProx.Uniform(0.5, 0.9) * (glass ? 0.35 : 1.0);
					const m::vec3 cpos = I.pos + (t0 * std::cos(th) + b0 * std::sin(th)) * rad;
					if (capDepth)
						dep = std::min(dep, params.fractThicknessCap * FrWallThickness(bvh, cpos, I.nrm, ns));
					std::vector<m::vec3> hull;
					const int nr = 7;
					for (int j = 0; j < nr; j++)
					{
						const double a2 = 2.0 * m::kPi * j / nr + rngProx.Uniform(-0.25, 0.25);
						const double rj = sz * rngProx.Uniform(0.7, 1.15);
						const m::vec3 o = (t0 * std::cos(a2) + b0 * std::sin(a2)) * rj;
						hull.push_back(cpos + o);
						hull.push_back(cpos + o + I.nrm * (FR_MARGIN * ns));
					}
					hull.push_back(cpos - I.nrm * dep);
					spallCutters.push_back(m::Manifold::Hull(hull));
				}
			}
		}
	}

	                                                                      
	                                                                       
	for (Int32 g = params.fractPaintOnly ? 2 : 1; g <= 3; g++)
	{
		const auto& lvl = params.fract[(size_t)(g - 1)];
		const Int32 target = lvl.amount;
		if (target <= 0)
		{
			                                                               
			                                                      
			if (!params.fractPaintOnly)
				FrGrowBranches(mesh, skel, edgeHash, allPoints, cracks, avoidTri, g, lvl,
					FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0 * (wood ? 0.5 : 1.0),
					ns, (uint32_t)(params.seed + 7040 + g), grainP, grainPull);
			continue;
		}
		if (params.fractPaintOnly && (lvl.width <= 0.0 || lvl.depth <= 0.0))
			continue;
		PyRandom& rngG = recipe >= 2 ? rngLevel[(size_t)(g - 1)] : rngNet;
		HostStatus(host, "fracture level " + std::to_string(g)
			+ " (" + std::to_string(target) + " cracks)", 30 + g * 5);
		const double cellG = std::sqrt(mesh.area / MaxD(1.0, (double)target));
		const double safetyCap = 2.0 * std::sqrt(mesh.area);
		const double bias = lvl.bias;
		const std::vector<m::vec3>* cornerSrc = &objCorners;
		std::vector<m::vec3> lvlCorners;
		if (g > 1)
		{
			for (const FrCrack& c : cracks)
			{
				if (c.gen != g - 1)
					continue;
				lvlCorners.push_back(c.pts.front());
				if (c.hit)
					lvlCorners.push_back(c.pts.back());
			}
			cornerSrc = &lvlCorners;
		}

		struct Cand { bool edge; size_t idx; };
		std::vector<Cand> cand;
		for (size_t i = 0; i < edgeSamples.size(); i++)
			if (!params.fractPaintOnly) cand.push_back({ true, i });
		for (size_t i = 0; i < allPoints.size(); i++)
			if (params.fractPaintOnly ? allPoints[i].gen == g - 1 : allPoints[i].gen < g)
				cand.push_back({ false, i });
		if (cand.empty())
			continue;
		std::vector<size_t> cornerIdx;
		if (bias > 0.0 && !cornerSrc->empty())
		{
			const double reach = 1.5 * FR_CORNER_SCALE * ns;
			for (size_t i = 0; i < cand.size(); i++)
			{
				const m::vec3 p = cand[i].edge ? edgeSamples[cand[i].idx].pos
											   : allPoints[cand[i].idx].pos;
				for (const m::vec3& cc : *cornerSrc)
				{
					if (m::la::length(cc - p) < reach)
					{
						cornerIdx.push_back(i);
						break;
					}
				}
			}
		}

		Int32 accepted = 0, attempts = 0;
		const Int32 budget = target * (lvl.bias >= FR_SHATTER_BIAS ? FR_ATTEMPT_BUDGET_SHATTER : FR_ATTEMPT_BUDGET_NORMAL);
		double proxShare = 0.0;
		if (proxMode == 1 && !impacts.empty())
			proxShare = MaxD(0.0, std::min(1.0, params.fractProxStrength));
		while (accepted < target && attempts < budget)
		{
			attempts++;
			if (proxShare > 0.0 && rngProx.Random() < proxShare)
			{
				double sumS = 0.0;
				for (const FrImpact& I : impacts) sumS += I.strength;
				double pickS = rngProx.Random() * sumS;
				size_t ii = 0;
				while (ii + 1 < impacts.size() && pickS > impacts[ii].strength) { pickS -= impacts[ii].strength; ii++; }
				const FrImpact& I = impacts[ii];
				double rho = std::abs(FrGauss(rngProx, 0.5)) * I.radius;
				if (rho > I.radius) rho = I.radius * rngProx.Random();
				const double th = rngProx.Uniform(0.0, 2.0 * m::kPi);
				const double hd = rngProx.Uniform(0.0, 2.0 * m::kPi);
				m::vec3 t0, b0;
				FrFrame(I.nrm, t0, b0);
				Int32 tri = I.tri;
				m::vec3 pos = I.pos;
				m::vec3 dm = t0 * std::cos(th) + b0 * std::sin(th);
				if (rho > 1e-9 && !FrMarch(mesh, tri, pos, dm, rho))
					continue;
				if (avoidTri != nullptr && (*avoidTri)[(size_t)tri])
					continue;
				const m::vec3 n0 = mesh.nrm[(size_t)tri];
				m::vec3 tt, bb;
				FrFrame(n0, tt, bb);
				const m::vec3 dir0 = tt * std::cos(hd) + bb * std::sin(hd);
				const double roomP = MaxD(1.5 * FR_SNAP_R * ns, 0.12 * I.radius);
				FrEntry qn;
				double qnd = 0.0;
				if (skel.Nearest(pos, roomP, &n0,
						[](const FrEntry&) { return false; }, qn, qnd))
					continue;
				FrCrack c;
				c.gen = g;
				double reach = I.radius * rngProx.Uniform(0.45, 0.95);
				if (lvl.length > 0.0)
					reach = std::min(reach, 0.5 * lvl.length);
				FrGrowInterior(mesh, skel, edgeHash, cracks, avoidTri, tri, pos, dir0,
					(Int32)cracks.size(), reach, 2.6 * reach,
					FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0, ns, rngProx, c);
				if (c.pts.size() < 5)
					continue;
				FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
				cracks.push_back(std::move(c));
				accepted++;
				continue;
			}
			const Cand& pick = (bias > 0.0 && !cornerIdx.empty() && rngG.Random() < bias)
				? cand[cornerIdx[(size_t)FrRandRange(rngG, (int)cornerIdx.size())]]
				: cand[(size_t)FrRandRange(rngG, (int)cand.size())];

			Int32 tri0 = -1, parent = -1;
			m::vec3 pos0, dir0, n0;
			bool fromEdge = pick.edge;
			m::vec3 edgeOrigin;
			if (fromEdge)
			{
				const FrEdgeSample& s = edgeSamples[pick.idx];
				const Int32 nFaces = s.faces[1] >= 0 ? 2 : (s.faces[0] >= 0 ? 1 : 0);
				if (nFaces == 0)
					continue;
				Int32 face = s.faces[0];
				if (nFaces == 2)
				{
					const double a0 = faceArea(s.faces[0]);
					const double a1 = faceArea(s.faces[1]);
					if (rngG.Random() * (a0 + a1) >= a0)
						face = s.faces[1];
				}
				                                                                  
				                                                             
				auto startOn = [&](Int32 f)
				{
					n0 = FrNorm(normals[f]);
					dir0 = FrNorm(m::la::cross(n0, s.tan));
					if (m::la::dot(dir0, faceCenter(f) - s.pos) < 0.0)
						dir0 = dir0 * -1.0;
					pos0 = s.pos + dir0 * (FR_STEP * ns * 0.25);
					tri0 = FrTriOfFacePoint(mesh, f, pos0);
					Int32 t = tri0;
					m::vec3 p = pos0, d = dir0;
					return FrMarchFlat(mesh, t, p, d, 2.0 * FR_STEP * ns, mesh.nrm[(size_t)tri0], 0.5 * FR_STEP * ns);
				};
				if (!startOn(face))
				{
					if (nFaces < 2 || !startOn(face == s.faces[0] ? s.faces[1] : s.faces[0]))
						continue;
				}
				if (avoidTri != nullptr && (*avoidTri)[(size_t)tri0])
					continue;
				edgeOrigin = s.pos;
			}
			else
			{
				const FrEntry& ent = allPoints[pick.idx];
				n0 = ent.nrm;
				m::vec3 side = FrNorm(m::la::cross(n0, ent.tan));
				if (rngG.Random() < 0.5)
					side = side * -1.0;
				pos0 = ent.pos;
				tri0 = ent.tri;
				dir0 = side;
				parent = ent.crack;
			}
			dir0 = RotAxis(dir0, n0,
				rngG.Uniform(-FR_START_JIT_DEG, FR_START_JIT_DEG) * m::kPi / 180.0);
			if (wood)
			{
				                                                                    
				m::vec3 gt = grain - n0 * m::la::dot(grain, n0);
				if (m::la::length(gt) > 0.2)
				{
					gt = FrNorm(gt);
					if (m::la::dot(gt, dir0) < 0.0) gt = gt * -1.0;
					dir0 = FrNorm(dir0 * (1.0 - grainPull) + gt * grainPull);
				}
			}

			double proxC = 0.0;
			if (!cornerSrc->empty())
			{
				double dc = std::numeric_limits<double>::infinity();
				for (const m::vec3& cc : *cornerSrc)
				{
					const double dd = m::la::length(cc - pos0);
					if (dd < dc) dc = dd;
				}
				proxC = std::exp(-dc / (FR_CORNER_SCALE * ns));
			}
			double room = cellG * 0.45 * (1.0 - 0.75 * bias * proxC);
			if (proxShare > 0.0)
			{
				double best = 0.0;
				for (const FrImpact& I : impacts)
				{
					const double dd = m::la::length(I.pos - pos0) / I.radius;
					best = MaxD(best, std::exp(-dd * dd));
				}
				room *= 1.0 - 0.6 * proxShare * best;
			}
			const m::vec3 probe = pos0 + dir0 * (room * 1.2);
			FrEntry q;
			double qd = 0.0;
			if (skel.Nearest(probe, room, &n0,
					[&](const FrEntry& e) { return e.crack == parent; }, q, qd))
				continue;

			const Bool shatter = bias >= FR_SHATTER_BIAS;
			const double probeCap = 2.6 * cellG;
			const double run = shatter
				? FrFreeRun(skel, edgeHash, pos0, dir0, parent, n0, ns, probeCap)
				: 0.0;
			if (shatter)
			{
				const double gate = cellG * (2.6 - 2.2 * bias);
				if (run > gate)
					continue;
			}

			double leash = shatter
				? MaxD(run * 2.5, 8.0 * FR_STEP * ns)
				: safetyCap;
			if (lvl.length > 0.0)
				leash = std::min(leash, lvl.length);
			if (burst)
			{
				                                                          
				                                                            
				                                                           
				                                                         
				const int nSp = std::max(3, std::min(16, glassStyle ? params.fractGlassSpokes / 2 + 1
					: params.fractRadialSpokes));
				const double fan = fromEdge ? 0.8 * m::kPi : 2.0 * m::kPi;
				const double r0 = 2.5 * FR_SNAP_R * ns;
				const double sig = FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0 * (glassStyle ? 0.15 : 0.6);
				std::vector<FrCrack> spokes;
				for (int k = 0; k < nSp; ++k)
				{
					const double th = fromEdge ? -0.5 * fan + fan * (k + 0.5) / nSp : fan * k / nSp;
					m::vec3 d = RotAxis(dir0, n0, th + rngG.Uniform(-0.25, 0.25) * fan / nSp);
					Int32 t = tri0;
					m::vec3 p = pos0;
					if (!FrMarch(mesh, t, p, d, r0)) continue;
					if (avoidTri != nullptr && (*avoidTri)[(size_t)t]) continue;
					FrCrack sc;
					sc.gen = g;
					sc.bornOn = parent;
					sc.startFree = true;
					const double lk = leash * rngG.Uniform(0.55, 1.0);
					FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, t, p, d,
						(Int32)(cracks.size() + spokes.size()), parent, lk, glassStyle ? 40.0 * lk : 2.6 * cellG,
						sig, ns, rngG, sc, -1.0, -2.0, grainP, grainPull);
					if (sc.pts.size() >= 4) spokes.push_back(std::move(sc));
				}
				if (spokes.size() < 2) continue;
				std::vector<Int32> ids;
				for (FrCrack& sc : spokes)
				{
					ids.push_back((Int32)cracks.size());
					FrRegisterCrack(skel, allPoints, sc, (Int32)cracks.size());
					cracks.push_back(std::move(sc));
				}
				if (glassStyle && ids.size() >= 3)
				{
					                                             
					double minLen = std::numeric_limits<double>::infinity();
					for (Int32 id : ids)
					{
						double L = 0.0;
						const auto& P = cracks[(size_t)id].pts;
						for (size_t i = 0; i + 1 < P.size(); ++i) L += m::la::length(P[i + 1] - P[i]);
						minLen = std::min(minLen, L);
					}
					const int nRing = std::max(1, std::min(4, params.fractGlassRings / 2));
					const double step = FR_CUT_STEP * ns / params.fractDetail;
					const size_t last = fromEdge ? ids.size() - 1 : ids.size();
					for (int rI = 0; rI < nRing; ++rI)
					{
						const double rr = minLen * (0.25 + 0.6 * (rI + 0.5) / nRing);
						for (size_t a = 0; a < last; ++a)
						{
							const size_t bI = (a + 1) % ids.size();
							if (rngG.Random() > params.fractGlassRingFill) continue;
							m::vec3 pa, pb;
							if (!FrPointAtArc(cracks[(size_t)ids[a]], rr * rngG.Uniform(0.92, 1.08), pa)
								|| !FrPointAtArc(cracks[(size_t)ids[bI]], rr * rngG.Uniform(0.92, 1.08), pb))
								continue;
							FrCrack rc;
							rc.gen = std::min(3, g + 1);
							if (!FrGuidedCrack(mesh, { pa, pb }, step, rc)) continue;
							bool blocked = false;
							if (avoidTri != nullptr)
								for (Int32 tt : rc.tris)
									if ((*avoidTri)[(size_t)tt]) { blocked = true; break; }
							if (blocked) continue;
							rc.bornOn = ids[a];
							rc.hitCrack = ids[bI];
							rc.hit = true;
							FrRegisterCrack(skel, allPoints, rc, (Int32)cracks.size());
							cracks.push_back(std::move(rc));
						}
					}
				}
				accepted++;
				continue;
			}
			FrCrack c;
			c.gen = g;
			c.bornOn = parent;
			FrGrowCrack(mesh, skel, edgeHash, cracks, avoidTri, tri0, pos0, dir0,
				(Int32)cracks.size(), parent, leash, wood ? 12.0 * cellG : 2.6 * cellG,
				FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0 * (wood ? 0.5 : 1.0),
				ns, rngG, c, -1.0, -2.0, grainP, grainPull);
			if (c.pts.size() < 4)
				continue;
			if (shatter && !c.hit)
				continue;
			if (params.fractPaintOnly) c.noSpall = true;
			if (fromEdge)
			{
				c.pts.insert(c.pts.begin(), edgeOrigin - dir0 * 1.5 * ns);
				c.nrms.insert(c.nrms.begin(), c.nrms.front());
				c.tris.insert(c.tris.begin(), c.tris.front());
				c.startEdge = true;
			}
			FrRegisterCrack(skel, allPoints, c, (Int32)cracks.size());
			cracks.push_back(std::move(c));
			accepted++;
		}
		if (accepted < target)
			HostLog(host, "[ProjectDamage] Fracture level " + std::to_string(g) + ": placed "
				+ std::to_string(accepted) + " of " + std::to_string(target)
				+ " cracks (out of room)");
		FrGrowBranches(mesh, skel, edgeHash, allPoints, cracks, avoidTri, g, lvl,
			FR_SIGMA_DEG * params.fractDispStrength * m::kPi / 180.0 * (wood ? 0.5 : 1.0),
			ns, (uint32_t)(params.seed + 7040 + g), grainP, grainPull);

		if (lvl.subdiv > 0 && !params.fractPaintOnly)
			FrSubdivideLevel(mesh, skel, edgeHash, avoidTri, allPoints, cracks, g, lvl.subdiv,
				ns, safetyCap, params, host, rngSubdiv);
	}
	FrCloseFreeEnds(mesh, cracks, skel, edgePts, ns);
	const bool physics = params.fractPhysics && !params.fractThrough;
	const bool through = params.fractThrough || physics;
	if (through)
	{
		HostStatus(host, "fracture through: closing cracks", 45);
		PyRandom rngThrough((uint32_t)(params.seed + 7040));
		const double levelLength[3] = { params.fract[0].length, params.fract[1].length, params.fract[2].length };
		std::vector<m::vec3> edgePos;
		for (const FrEdgeSample& e : edgeSamples) edgePos.push_back(e.pos);
		if (!params.fractHelperStrokes.empty())
		{
			                                                                    
			                                                                
			                                                          
			HostStatus(host, "fracture: helper strokes", 45);
			const double snap = params.fractHelperSnap > 0.0 ? params.fractHelperSnap
				: std::max(6.0 * FR_SNAP_R * ns, 1.5 * params.fract[0].width);
			FrLayHelpers(mesh, skel, allPoints, cracks, edgePos, params.fractHelperStrokes, ns, params.fractDetail, snap);
		}
		FrCloseForThrough(mesh, skel, edgeHash, allPoints, cracks, avoidTri, ns, sigmaBase, levelLength, rngThrough);
		if (physics && params.fractPhysicsClose > 0)
		{
			HostStatus(host, "fracture physics: closing islands", 46);
			PyRandom rngIsl((uint32_t)(params.seed + 7045));
			FrCloseIslands(mesh, skel, edgeHash, edgePos, allPoints, cracks, avoidTri, ns, sigmaBase,
				params.fractPhysicsClose, params.fractDetail, rngIsl);
		}
	}

	auto crackRng = [&](size_t ci) {
		return PyRandom((uint32_t)(params.seed + 7100 + 7919u * (uint32_t)ci));
	};
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		FrCrack& c = cracks[ci];
		if (params.fractPaintOnly)
		{
			c.fP = c.pts;
			c.fN = c.nrms;
		}
		else if (recipe >= 2 || c.widthOverride > 0.0)
		{
			PyRandom r = crackRng(ci);
			FrFinalSpine(c, c.widthOverride > 0.0 ? c.widthOverride : params.fract[(size_t)(c.gen - 1)].width,
				params.fractDispStrength * c.reliefScale, params.fractDispScale, params.fractDetail, ns, r);
		}
		else
			FrFinalSpine(c, params.fract[(size_t)(c.gen - 1)].width,
				params.fractDispStrength, params.fractDispScale, params.fractDetail,
				ns, rngNet);
	}
	FrSnapSpines(cracks);
	for (size_t ci = 0; ci < cracks.size(); ci++)
	{
		const FrCrack& c = cracks[ci];
		const size_t before = crackCutters.size();
		if (recipe >= 2 || c.widthOverride > 0.0)
		{
			PyRandom r((uint32_t)(params.seed + 7101 + 7919u * (uint32_t)ci));
			FrBuildCrackCutters(c, c.widthOverride > 0.0 ? c.widthOverride : params.fract[(size_t)(c.gen - 1)].width,
				(c.depthOverride > 0.0 ? c.depthOverride : params.fract[(size_t)(c.gen - 1)].depth)
					* params.fractDepthMult,
				params.fractDetail, ns, r, crackCutters, params.mask,
				2, capDepth ? &bvh : nullptr, params.fractThicknessCap);
		}
		else
			FrBuildCrackCutters(c, params.fract[(size_t)(c.gen - 1)].width,
				params.fract[(size_t)(c.gen - 1)].depth * params.fractDepthMult,
				params.fractDetail, ns, rngNet, crackCutters, params.mask);
		for (size_t i = before; i < crackCutters.size(); i++)
			crackLevels.push_back(c.gen - 1);
	}

	if (through)
	{
		HostStatus(host, "fracture through: splitting pieces", 48);
		std::vector<double> widths;
		for (const FrCrack& c : cracks)
			widths.push_back(c.widthOverride > 0.0 ? c.widthOverride : params.fract[(size_t)(c.gen - 1)].width);
		std::vector<double> depths;
		for (const FrCrack& c : cracks)
			depths.push_back((c.depthOverride > 0.0 ? c.depthOverride : params.fract[(size_t)(c.gen - 1)].depth)
				* params.fractDepthMult);
		const double physDepth = !physics ? 0.0
			: (params.fractPhysicsDepth > 0.0 ? params.fractPhysicsDepth : params.fract[0].depth * params.fractDepthMult);
		                                                                
		                                                                      
		                          
		FrFillParams fill;
		ProjectDamageBVH fillBvh;
		const bool doFill = !physics && params.fractThroughFill;
		if (doFill)
		{
			fillBvh.Build(meshView);
			fill.bvh = &fillBvh;
			fill.size = params.fractThroughFillSize;
			fill.rough = params.fractThroughRough;
		}
		m::Manifold gaps = FrBuildFragmentCutter(mesh, cracks, widths, avoidTri, ns, &depths, physDepth,
			doFill ? &fill : nullptr, (uint32_t)params.seed);
		if (!gaps.IsEmpty())
		{
			crackCutters.push_back(std::move(gaps));
			crackLevels.push_back(0);
		}
	}

	PyRandom rngSpall((uint32_t)(params.seed + 7001));
	Int32 spallIdx = 0;
	for (const FrCrack& c : cracks)
	{
		const auto& lvl = params.fract[(size_t)(c.gen - 1)];
		if (!lvl.chips || lvl.chipAmount <= 0.0 || c.noSpall)
			continue;
		FrSpallOnCrack(c, lvl.width, lvl.chipAmount / 100.0, lvl.chipSize,
			lvl.chipDepth, edgePts, ns, rngSpall, params.fractDetail,
			params.fractRimSoft, params.mask, params.seed + 7001, spallIdx,
			spallCutters, capDepth ? &bvh : nullptr, params.fractThicknessCap);
	}
}

                                                                 
                                                                        
                                                                         
int DropFallenFragmentsVolume(m::Manifold& result, const m::Manifold& source, double keepShare)
{
	double smallest = std::numeric_limits<double>::infinity();
	for (const m::Manifold& p : source.Decompose())
	{
		const double v = p.Volume();
		if (v > 0.0 && v < smallest)
			smallest = v;
	}
	if (!std::isfinite(smallest))
		return 0;
	m::MeshGL64 mg = result.GetMeshGL64();
	const size_t nV = mg.NumVert(), nT = mg.NumTri(), np = mg.numProp;
	if (nT == 0)
		return 0;
	std::vector<uint64_t> up(nV);
	for (size_t v = 0; v < nV; v++)
		up[v] = v;
	auto find = [&](uint64_t v)
	{
		while (up[v] != v)
		{
			up[v] = up[up[v]];
			v = up[v];
		}
		return v;
	};
	auto unite = [&](uint64_t a, uint64_t b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			up[std::max(a, b)] = std::min(a, b);
	};
	for (size_t t = 0; t < nT; t++)
	{
		unite(mg.triVerts[3 * t], mg.triVerts[3 * t + 1]);
		unite(mg.triVerts[3 * t], mg.triVerts[3 * t + 2]);
	}
	for (size_t i = 0; i < mg.mergeFromVert.size(); i++)
		unite(mg.mergeFromVert[i], mg.mergeToVert[i]);
	std::unordered_map<uint64_t, double> vol;
	auto pos = [&](uint64_t v) { return m::vec3(mg.vertProperties[v * np], mg.vertProperties[v * np + 1], mg.vertProperties[v * np + 2]); };
	for (size_t t = 0; t < nT; t++)
	{
		const m::vec3 a = pos(mg.triVerts[3 * t]), b = pos(mg.triVerts[3 * t + 1]), c = pos(mg.triVerts[3 * t + 2]);
		vol[find(mg.triVerts[3 * t])] += m::la::dot(a, m::la::cross(b, c)) / 6.0;
	}
	if (vol.size() <= 1)
		return 0;
	const double film = 1e-6 * smallest, fallen = keepShare * smallest;
	int dropped = 0;
	std::unordered_map<uint64_t, bool> drop;
	for (const auto& kv : vol)
	{
		const bool d = std::abs(kv.second) <= film || std::abs(kv.second) < fallen;
		drop[kv.first] = d;
		dropped += d ? 1 : 0;
	}
	if (dropped == 0)
		return 0;
	if (dropped == (int)vol.size())
	{
		result = m::Manifold();
		return dropped;
	}
	                                                                          
	std::vector<int64_t> newIdx(nV, -1);
	m::MeshGL64 out;
	out.numProp = mg.numProp;
	out.runOriginalID = mg.runOriginalID;
	out.runTransform = mg.runTransform;
	out.runFlags = mg.runFlags;
	out.tolerance = mg.tolerance;
	const bool haveFace = mg.faceID.size() == nT;
	const bool haveTan = mg.halfedgeTangent.size() == 12 * nT;
	std::vector<uint64_t> runIndex = mg.runIndex;
	if (runIndex.empty())
		runIndex = { 0, 3 * nT };
	for (size_t r = 0; r + 1 < runIndex.size(); r++)
	{
		out.runIndex.push_back(out.triVerts.size());
		for (uint64_t t = runIndex[r] / 3; t < runIndex[r + 1] / 3; t++)
		{
			if (drop[find(mg.triVerts[3 * t])])
				continue;
			for (int k = 0; k < 3; k++)
			{
				const uint64_t v = mg.triVerts[3 * t + k];
				if (newIdx[v] < 0)
				{
					newIdx[v] = (int64_t)(out.vertProperties.size() / np);
					out.vertProperties.insert(out.vertProperties.end(),
						mg.vertProperties.begin() + (std::ptrdiff_t)(v * np),
						mg.vertProperties.begin() + (std::ptrdiff_t)((v + 1) * np));
				}
				out.triVerts.push_back((uint64_t)newIdx[v]);
			}
			if (haveFace)
				out.faceID.push_back(mg.faceID[t]);
			if (haveTan)
				out.halfedgeTangent.insert(out.halfedgeTangent.end(),
					mg.halfedgeTangent.begin() + (std::ptrdiff_t)(12 * t),
					mg.halfedgeTangent.begin() + (std::ptrdiff_t)(12 * (t + 1)));
		}
	}
	out.runIndex.push_back(out.triVerts.size());
	if (mg.runIndex.empty())
		out.runIndex.clear();
	for (size_t i = 0; i < mg.mergeFromVert.size(); i++)
	{
		const int64_t f = newIdx[mg.mergeFromVert[i]], t = newIdx[mg.mergeToVert[i]];
		if (f >= 0 && t >= 0)
		{
			out.mergeFromVert.push_back((uint64_t)f);
			out.mergeToVert.push_back((uint64_t)t);
		}
	}
	result = m::Manifold(out);
	return dropped;
}
}

PROJECT_DAMAGE_NS_MACRO_END
