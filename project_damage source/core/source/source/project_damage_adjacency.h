// общие объявления: находим соседние грани и рёбра меша
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

#include <cstdint>
#include <vector>

struct ProjectDamageAdjInfo
{
	int32_t face[4];
	bool    mark[4];
};

class ProjectDamageAdjacency
{
public:
	bool Build(const MeshView& mesh)
	{
		info_.clear();
		if (mesh.polys == nullptr || mesh.npolys <= 0 || mesh.npts <= 0)
			return false;
		const int32_t n = mesh.npolys;
		const int32_t nPts = mesh.npts;
		info_.assign((size_t)n, ProjectDamageAdjInfo{ { -1, -1, -1, -1 }, { false, false, false, false } });

		std::vector<int32_t> start((size_t)nPts + 1, 0);
		for (int32_t p = 0; p < n; p++)
		{
			const ProjectDamagePoly& cp = mesh.polys[p];
			const int32_t v[4] = { cp.a, cp.b, cp.c, cp.d };
			for (int s = 0; s < 4; s++)
			{
				const int32_t a = v[s], b = v[(s + 1) & 3];
				if (!RealSide(a, b, nPts))
					continue;
				start[(size_t)(a < b ? a : b) + 1]++;
			}
		}
		for (int32_t i = 1; i <= nPts; i++)
			start[(size_t)i] += start[(size_t)i - 1];

		const int32_t total = start[(size_t)nPts];
		std::vector<int32_t> other((size_t)total);
		std::vector<int32_t> owner((size_t)total);
		std::vector<int32_t> cursor(start.begin(), start.begin() + nPts);
		for (int32_t p = 0; p < n; p++)
		{
			const ProjectDamagePoly& cp = mesh.polys[p];
			const int32_t v[4] = { cp.a, cp.b, cp.c, cp.d };
			for (int s = 0; s < 4; s++)
			{
				const int32_t a = v[s], b = v[(s + 1) & 3];
				if (!RealSide(a, b, nPts))
					continue;
				const int32_t lo = a < b ? a : b, hi = a < b ? b : a;
				const int32_t k = cursor[(size_t)lo]++;
				other[(size_t)k] = hi;
				owner[(size_t)k] = 4 * p + s;
			}
		}

		for (int32_t v = 0; v < nPts; v++)
		{
			const int32_t b0 = start[(size_t)v], b1 = start[(size_t)v + 1];
			for (int32_t i = b0; i < b1; i++)
			{
				const int32_t pi = owner[(size_t)i] / 4, si = owner[(size_t)i] % 4;
				if (info_[(size_t)pi].mark[si])
					continue;
				const int32_t w = other[(size_t)i];
				for (int32_t j = i + 1; j < b1; j++)
				{
					if (other[(size_t)j] != w)
						continue;
					const int32_t pj = owner[(size_t)j] / 4, sj = owner[(size_t)j] % 4;
					info_[(size_t)pj].face[sj] = pi;
					info_[(size_t)pj].mark[sj] = true;
					if (info_[(size_t)pi].face[si] < 0)
						info_[(size_t)pi].face[si] = pj;
				}
			}
		}
		return true;
	}

	const ProjectDamageAdjInfo* GetInfo(int32_t poly) const { return &info_[(size_t)poly]; }

private:
	static bool RealSide(int32_t a, int32_t b, int32_t nPts)
	{
		return a != b && a >= 0 && a < nPts && b >= 0 && b < nPts;
	}

	std::vector<ProjectDamageAdjInfo> info_;
};
