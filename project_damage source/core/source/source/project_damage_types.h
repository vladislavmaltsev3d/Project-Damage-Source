// общие объявления: общие типы настроек и геометрии повреждений
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

#include "manifold/manifold.h"

#include <cmath>

namespace m = manifold;

struct ProjectDamagePoly { int32_t a, b, c, d; };
using EVec = m::vec3;
using EMat = m::mat3x4;

struct SurfaceDirectionField {
	virtual EVec Near(const EVec& point, const EVec& exact) const = 0;
	virtual ~SurfaceDirectionField() = default;
};

struct MeshView {
	const EVec*     pts;    int32_t npts;
	const ProjectDamagePoly* polys;  int32_t npolys;
	                                                                       
	const EVec* guideNormals = nullptr;
	const EVec* guideVertexNormals = nullptr;
	const EVec* guidePoints = nullptr;
	const SurfaceDirectionField* guideField = nullptr;
};

inline EVec GuidedNormal(const MeshView& view, int32_t poly, const EVec& exact)
{
	return view.guideNormals && poly >= 0 && poly < view.npolys ? view.guideNormals[poly] : exact;
}

inline EVec GuidedVertexNormal(const MeshView& view, int32_t vertex, const EVec& exact)
{
	return view.guideVertexNormals && vertex >= 0 && vertex < view.npts ? view.guideVertexNormals[vertex] : exact;
}

inline EVec GuidedPointNormal(const MeshView& view, const EVec& point, const EVec& exact)
{
	return view.guideField ? view.guideField->Near(point, exact) : exact;
}

inline EVec MulPoint(const EMat& m, const EVec& p)
{
	return m[3] + m[0] * p.x + m[1] * p.y + m[2] * p.z;
}

inline EMat InverseAffine(const EMat& m)
{
	const EVec& v1 = m[0];
	const EVec& v2 = m[1];
	const EVec& v3 = m[2];
	const EVec& off = m[3];
	double det = v1.x * (v2.y * v3.z - v3.y * v2.z) + v2.x * (v3.y * v1.z - v1.y * v3.z)
		+ v3.x * (v1.y * v2.z - v2.y * v1.z);
	if (det == 0.0)
		return EMat(m::la::identity);
	det = 1.0 / det;
	const EVec nOff((v2.x * (off.y * v3.z - v3.y * off.z) +
					 v3.x * (off.z * v2.y - off.y * v2.z) +
					 off.x * (v3.y * v2.z - v2.y * v3.z)) * det,
					(v3.x * (off.y * v1.z - v1.y * off.z) +
					 off.x * (v1.y * v3.z - v3.y * v1.z) +
					 v1.x * (v3.y * off.z - off.y * v3.z)) * det,
					(off.x * (v2.y * v1.z - v1.y * v2.z) +
					 v1.x * (v2.z * off.y - v2.y * off.z) +
					 v2.x * (off.z * v1.y - off.y * v1.z)) * det);
	const EVec n1((v2.y * v3.z - v3.y * v2.z) * det,
				  (v3.y * v1.z - v1.y * v3.z) * det,
				  (v1.y * v2.z - v2.y * v1.z) * det);
	const EVec n2((v2.z * v3.x - v3.z * v2.x) * det,
				  (v3.z * v1.x - v1.z * v3.x) * det,
				  (v1.z * v2.x - v2.z * v1.x) * det);
	const EVec n3((v2.x * v3.y - v3.x * v2.y) * det,
				  (v3.x * v1.y - v1.x * v3.y) * det,
				  (v1.x * v2.y - v2.x * v1.y) * det);
	return EMat(n1, n2, n3, nOff);
}

inline bool SameEVec(const EVec& a, const EVec& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool SameEMat(const EMat& a, const EMat& b)
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 3; r++)
			if (a[c][r] != b[c][r])
				return false;
	return true;
}

constexpr double PROJECT_DAMAGE_SNAP_AXIS   = 1e-9;
constexpr double PROJECT_DAMAGE_SNAP_LENGTH = 1e-6;

inline double SnapTo(double v, double q)
{
	const double r = std::floor(v / q + 0.5) * q;
	return r == 0.0 ? 0.0 : r;
}

inline EVec SnapLength(const EVec& v)
{
	return EVec(SnapTo(v.x, PROJECT_DAMAGE_SNAP_LENGTH),
		SnapTo(v.y, PROJECT_DAMAGE_SNAP_LENGTH),
		SnapTo(v.z, PROJECT_DAMAGE_SNAP_LENGTH));
}

inline EMat SnapRel(const EMat& m)
{
	auto axis = [](const EVec& v) {
		return EVec(SnapTo(v.x, PROJECT_DAMAGE_SNAP_AXIS),
			SnapTo(v.y, PROJECT_DAMAGE_SNAP_AXIS),
			SnapTo(v.z, PROJECT_DAMAGE_SNAP_AXIS));
	};
	return EMat(axis(m[0]), axis(m[1]), axis(m[2]), SnapLength(m[3]));
}
