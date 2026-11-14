// общие объявления: общий вход расчёта отслоения покрытия
// SPDX-License-Identifier: GPL-3.0-or-later
// Connected Peeling: host-neutral geometric planning. Blender owns attribute IO.
#pragma once
#include <array>
#include <vector>
#include <cstdint>

namespace pd_peeling {
using V=std::array<double,3>;
using Edge=std::array<int32_t,2>;
using Tri=std::array<int32_t,3>;
using Patch=std::array<double,7>; // centre, normal, radius
struct Plan {
 std::vector<int32_t> hole,band,border;
 std::vector<double> distance,rimDistance,rootDistance,scale;
};
std::vector<int32_t> RefineEdges(const std::vector<V>& points,const std::vector<V>& normals,
 const std::vector<Edge>& edges,const std::vector<int32_t>& eligible,const std::vector<V>& seeds,
 const std::vector<Patch>& patches,double step,double bend,double probe,bool adaptive,bool micro);
Plan PlanAttached(const std::vector<V>& points,const std::vector<Tri>& triangles,
 const std::vector<int32_t>& initialHole,const std::vector<double>& vertexScale,double bend);
std::vector<double> DrawingValues(const std::vector<V>& points,const std::vector<V>& normals,
 const std::vector<std::array<double,14>>& frames,const std::vector<std::array<double,2>>& polygons,
 const std::vector<int32_t>& offsets,double size,double radius);
struct Offsets {std::vector<V> top,floor,back;};
Offsets Deform(const std::vector<V>& points,const std::vector<V>& normals,const Plan& plan,
 const std::vector<double>& variationNoise,const std::vector<double>& localDepth,
 double lift,double bend,double thickness,double variation);

// ---- Curl kernels (Surface Peel v2). The v1 functions above stay unchanged
// for compatibility; peeling.py uses these when the module exports them.
//
// PlanCurl: like PlanAttached, but every lifted vertex is tied to ONE rim
// vertex (geodesic nearest, Dijkstra over coating faces), so all per-rim
// properties (width, lift, curl direction) are coherent across the band.
//  rimWidth: per-vertex width factor, read at rim vertices (tongues > 1).
//  guard:    per-vertex distance to protected structure (sharp edges, other
//            effects, masked coating); the bend fades out before it. inf = none.
//  locked:   per-face, 1 = never part of the band (other effects, masked).
struct CurlPlan {
 std::vector<int32_t> hole,band,border,rim;
 std::vector<double> distance,rimDistance,rootDistance,scale,width;
 std::vector<V> direction; // root -> rim tangent direction, length = coherence (0..1)
};
CurlPlan PlanCurl(const std::vector<V>& points,const std::vector<V>& normals,const std::vector<Tri>& triangles,
 const std::vector<int32_t>& initialHole,const std::vector<int32_t>& locked,const std::vector<double>& vertexScale,
 const std::vector<double>& rimWidth,const std::vector<double>& guard,double bend,const struct CurlOptions* options=nullptr);
// Thin cracks: a slit narrower than the bend would otherwise shrink its flaps,
// and a plate enclosed by slits would be absorbed into the hole.
struct CurlOptions {
 std::vector<uint8_t> fixed_scale; // per vertex, 1 = keep width and lift of this rim whatever the opening's size
 double absorb=.6;                 // coating closer than absorb*bend to a rim everywhere is absorbed
};
struct CurlSettings {
 double lift=0,bend=1,thickness=0;
 double curl=.5;      // 0 stiff lifted plates (uniform bend) .. 1 edges rolled up near the lip
 double droop=0;      // 0..1: flaps hanging from above lift more, flaps below lie flatter
 double rootBlend=.3; // share of the band over which the hidden underside meets the floor
 V gravity{0,0,-1};
};
// rimAmp: per-vertex lift amplitude, read at rim vertices (0..~2).
Offsets DeformCurl(const std::vector<V>& points,const std::vector<V>& normals,const CurlPlan& plan,
 const std::vector<double>& rimAmp,const std::vector<double>& localDepth,const CurlSettings& settings);
// RefineAdaptive: edges to split, most important first. values = field per
// point in Patch Size units (NaN = unknown: use the region gate only).
// Near the contour edges go to step*fine, in the bending reach to step, in
// the exposed interior (flat floor) only to 3*step; curved surface refines more.
std::vector<int32_t> RefineAdaptive(const std::vector<V>& points,const std::vector<V>& normals,
 const std::vector<Edge>& edges,const std::vector<int32_t>& eligible,const std::vector<double>& values,
 const std::vector<V>& seeds,double step,double bend,double reach,double probe,double size,double fine,bool adaptive);
// DrawingValuesSurface: as DrawingValues, but the distance is measured in 3D
// to the drawing snapped onto the surface (polyline3: one point per 2D point),
// so the rim follows the drawing on curved walls. The inside test stays planar.
std::vector<double> DrawingValuesSurface(const std::vector<V>& points,const std::vector<V>& normals,
 const std::vector<std::array<double,14>>& frames,const std::vector<std::array<double,2>>& polygons,
 const std::vector<V>& polyline3,const std::vector<int32_t>& offsets,double size,double radius);
}
