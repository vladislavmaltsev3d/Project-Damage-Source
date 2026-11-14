// общие объявления: быстрый поиск ближайшей поверхности и пересечений луча
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

#pragma once

#include "project_damage_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

struct ProjectDamageBvhTree
{
	struct AABB
	{
		EVec lo{  std::numeric_limits<double>::max(),  std::numeric_limits<double>::max(),  std::numeric_limits<double>::max() };
		EVec hi{ -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max() };
		void Grow(const EVec& p)
		{
			lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
			hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
		}
		void Grow(const AABB& o) { Grow(o.lo); Grow(o.hi); }
		EVec Centroid() const { return (lo + hi) * 0.5; }
	};

	struct Node
	{
		AABB box;
		int32_t left = -1, right = -1;
		int32_t start = 0, count = 0;
		bool IsLeaf() const { return left < 0 && right < 0; }
	};

	static constexpr int32_t kLeafSize = 4;

	std::vector<int32_t> order;
	std::vector<Node>    nodes;

	bool Empty() const { return nodes.empty(); }

	void Build(const std::vector<AABB>& triBox)
	{
		order.clear();
		nodes.clear();
		if (triBox.empty())
			return;
		order.resize(triBox.size());
		for (size_t i = 0; i < triBox.size(); i++)
			order[i] = (int32_t)i;
		nodes.reserve(triBox.size());
		BuildRange(triBox, 0, (int32_t)triBox.size());
	}

private:
	int32_t BuildRange(const std::vector<AABB>& triBox, int32_t start, int32_t count)
	{
		const int32_t nodeIdx = (int32_t)nodes.size();
		nodes.emplace_back();

		AABB box;
		for (int32_t i = start; i < start + count; i++)
			box.Grow(triBox[(size_t)order[(size_t)i]]);

		if (count <= kLeafSize)
		{
			nodes[(size_t)nodeIdx].box = box;
			nodes[(size_t)nodeIdx].start = start;
			nodes[(size_t)nodeIdx].count = count;
			return nodeIdx;
		}

		AABB cbox;
		for (int32_t i = start; i < start + count; i++)
			cbox.Grow(triBox[(size_t)order[(size_t)i]].Centroid());
		const EVec ext = cbox.hi - cbox.lo;
		int axis = 0;
		if (ext.y > ext.x && ext.y >= ext.z) axis = 1;
		else if (ext.z > ext.x && ext.z >= ext.y) axis = 2;

		auto axisVal = [axis](const EVec& v) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); };
		const int32_t mid = start + count / 2;
		std::nth_element(order.begin() + start, order.begin() + mid, order.begin() + start + count,
			[&](int32_t a, int32_t b) { return axisVal(triBox[(size_t)a].Centroid()) < axisVal(triBox[(size_t)b].Centroid()); });

		const int32_t left = BuildRange(triBox, start, mid - start);
		const int32_t right = BuildRange(triBox, mid, start + count - mid);
		nodes[(size_t)nodeIdx].box = box;
		nodes[(size_t)nodeIdx].left = left;
		nodes[(size_t)nodeIdx].right = right;
		return nodeIdx;
	}
};

class ProjectDamageBVH
{
public:
	void Build(const MeshView& mesh)
	{
		tris_.clear();
		tree_ = ProjectDamageBvhTree();
		tris_.reserve((size_t)mesh.npolys * 2);
		for (int32_t i = 0; i < mesh.npolys; i++)
		{
			const ProjectDamagePoly& p = mesh.polys[i];
			tris_.push_back({ mesh.pts[p.a], mesh.pts[p.b], mesh.pts[p.c], i });
			if (p.c != p.d)
				tris_.push_back({ mesh.pts[p.a], mesh.pts[p.c], mesh.pts[p.d], i });
		}
		if (tris_.empty())
			return;
		std::vector<AABB> triBox(tris_.size());
		for (size_t i = 0; i < tris_.size(); i++)
		{
			AABB b;
			b.Grow(tris_[i].v0); b.Grow(tris_[i].v1); b.Grow(tris_[i].v2);
			triBox[i] = b;
		}
		tree_.Build(triBox);
	}

	bool Raycast(const EVec& org, const EVec& dir, double maxDist, double* hitDist, int32_t* hitPoly = nullptr) const
	{
		if (tree_.Empty())
			return false;
		const EVec invDir(
			dir.x != 0.0 ? 1.0 / dir.x : std::numeric_limits<double>::max(),
			dir.y != 0.0 ? 1.0 / dir.y : std::numeric_limits<double>::max(),
			dir.z != 0.0 ? 1.0 / dir.z : std::numeric_limits<double>::max());

		double best = maxDist;
		bool hit = false;
		int32_t stack[128];
		int32_t sp = 0;
		stack[sp++] = 0;
		while (sp > 0)
		{
			const Node& n = tree_.nodes[(size_t)stack[--sp]];
			if (!SlabTest(n.box, org, invDir, best))
				continue;
			if (n.IsLeaf())
			{
				for (int32_t i = n.start; i < n.start + n.count; i++)
				{
					double t;
					if (RayTri(org, dir, tris_[(size_t)tree_.order[(size_t)i]], best, t))
					{
						best = t;
						if (hitPoly) *hitPoly = tris_[(size_t)tree_.order[(size_t)i]].source;
						hit = true;
					}
				}
			}
			else
			{
				stack[sp++] = n.left;
				stack[sp++] = n.right;
			}
		}
		if (hit && hitDist != nullptr)
			*hitDist = best;
		return hit;
	}

private:
	struct Tri { EVec v0, v1, v2; int32_t source; };
	using AABB = ProjectDamageBvhTree::AABB;
	using Node = ProjectDamageBvhTree::Node;

	std::vector<Tri> tris_;
	ProjectDamageBvhTree      tree_;

	static bool SlabTest(const AABB& box, const EVec& org, const EVec& invDir, double maxDist)
	{
		double t0 = 0.0, t1 = maxDist;
		double tmin = (box.lo.x - org.x) * invDir.x, tmax = (box.hi.x - org.x) * invDir.x;
		if (tmin > tmax) std::swap(tmin, tmax);
		t0 = std::max(t0, tmin); t1 = std::min(t1, tmax);
		if (t0 > t1) return false;
		tmin = (box.lo.y - org.y) * invDir.y; tmax = (box.hi.y - org.y) * invDir.y;
		if (tmin > tmax) std::swap(tmin, tmax);
		t0 = std::max(t0, tmin); t1 = std::min(t1, tmax);
		if (t0 > t1) return false;
		tmin = (box.lo.z - org.z) * invDir.z; tmax = (box.hi.z - org.z) * invDir.z;
		if (tmin > tmax) std::swap(tmin, tmax);
		t0 = std::max(t0, tmin); t1 = std::min(t1, tmax);
		if (t0 > t1) return false;
		return true;
	}

	static bool RayTri(const EVec& org, const EVec& dir, const Tri& tri, double maxDist, double& t)
	{
		const EVec e1 = tri.v1 - tri.v0;
		const EVec e2 = tri.v2 - tri.v0;
		const EVec pvec = m::la::cross(dir, e2);
		const double det = m::la::dot(e1, pvec);
		if (std::fabs(det) < 1e-12)
			return false;
		const double invDet = 1.0 / det;
		const EVec tvec = org - tri.v0;
		const double u = m::la::dot(tvec, pvec) * invDet;
		if (u < 0.0 || u > 1.0)
			return false;
		const EVec qvec = m::la::cross(tvec, e1);
		const double v = m::la::dot(dir, qvec) * invDet;
		if (v < 0.0 || u + v > 1.0)
			return false;
		const double dist = m::la::dot(e2, qvec) * invDet;
		if (dist <= 0.0 || dist > maxDist)
			return false;
		t = dist;
		return true;
	}
};
