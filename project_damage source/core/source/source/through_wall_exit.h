#pragma once

#include "project_damage_bvh.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN

// The first surface hit from inside the wall must be an actual back face.
// Smoothed and interpolated normals are useful for placement, not thickness.
inline bool IsOpposingWallExit(const MeshView& view, int32_t face, double distance, const EVec& outward)
{
    if (!(distance > 0.0) || face < 0 || face >= view.npolys) return false;
    const auto& poly = view.polys[face];
    const EVec a = view.pts[poly.a];
    const EVec b = view.pts[poly.b] - a;
    const EVec c = view.pts[poly.c] - a;
    auto normal = manifold::la::cross(b, c);
    if (poly.c != poly.d)
        normal += manifold::la::cross(c, view.pts[poly.d] - a);
    const double length = manifold::la::length(normal);
    return length > 0.0 && manifold::la::dot(normal, outward) < -0.5 * length;
}

inline double FindWallExit(const ProjectDamageBVH& bvh, const MeshView& view, const EVec& entry,
                           const EVec& outward, double epsilon, double reach)
{
    double distance = 0.0;
    int32_t face = -1;
    if (!bvh.Raycast(entry - outward * epsilon, -outward, reach, &distance, &face) ||
        !IsOpposingWallExit(view, face, distance, outward))
        return -1.0;
    return distance + epsilon;
}

PROJECT_DAMAGE_NS_MACRO_END
