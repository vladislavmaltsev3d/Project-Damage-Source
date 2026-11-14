// общие объявления: строим связанные края покрытия и открывшийся внутренний слой
// SPDX-License-Identifier: GPL-3.0-or-later
// Complete native Peeling build. Blender stays responsible only for attribute
// IO and materials: the host hands over plain arrays and receives a finished
// polygon mesh plus the provenance needed to interpolate its custom data.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pd_peeling_full {
using F3=std::array<float,3>;

struct PassParams {
 double size=1,detail=12,step=1,bend=1,depth=1,thickness=0,lift=0,variation=0,curl=.5,tongues=0,tears=0,droop=0,small_amount=0;
 double ragged=0,threshold=0,plates=0,rough=0;
 F3 offset{0,0,0};
 std::array<F3,12> O{};      // field offsets
 std::array<F3,3> rim_offset{};
 int floor_mat=0,back_mat=-1,bridge_mat=0,crack_mat=-1;
 bool mark_paper=false,edge_all=false,paper_only=false,use_drawing=false,use_group=false;
 // Peeling crosses sharp edges and corners instead of stopping before them.
 bool wrap=false;
 // 0 crack network with lifted plate edges and no paint loss .. 1 full peeling.
 double damage=1;
 double contour_step=0,bend_step=0,reach=0,crack_width=0,relief_step=0; // derived from damage
};

struct DrawingInput {
 std::vector<double> points;       // concatenated xyz of every drawn path
 std::vector<int32_t> offsets;     // path starts, size paths+1
 bool areas=true,close_strokes=true;
 double radius=0,snap=0;
};

struct Input {
 std::vector<float> points;        // Nx3
 std::vector<int32_t> poly_offsets,corner_vert;
 std::vector<int32_t> tri_corners; // Tx3 original corner indices
 std::vector<int32_t> tri_poly;    // T
 std::vector<int32_t> poly_material,poly_smooth,poly_stamp;
 std::vector<int32_t> edges;       // Ex2
 std::vector<int32_t> edge_sharp;  // E
 std::vector<float> group_weight;  // N, or empty
 std::vector<float> guided_face_normals; // Px3, or empty
 std::vector<float> gravity;       // 3
 DrawingInput drawing;
 PassParams main,paper;
 bool has_drawing=false,wallpaper=false,adaptive=true;
 int seed=0,budget=300000;
 // Optional Surface Guide hook (points Nx3, quads Qx4) -> vertex normals Nx3.
 // Called with the interpreter lock held, only when the preference is on.
 std::function<std::vector<double>(const std::vector<double>&,const std::vector<int32_t>&)> guide;
};

struct Output {
 int faces=0,contour_edges=0,small_patches=0,tears=0,paper_contour_edges=0;
 bool limited=false,drawing_empty=false,has_paper_stats=false;
 std::vector<float> points;
 std::vector<int32_t> vert_src;   // Nx3, -1 unused
 std::vector<float> vert_w;       // Nx3
 std::vector<int32_t> poly_offsets,corner_vert,corner_edge;
 std::vector<int32_t> corner_src; // Mx3 original corners
 std::vector<float> corner_w;     // Mx3
 std::vector<int32_t> face_src,material,smooth,stamp;
 std::vector<int32_t> edges,edge_src,edge_sharp;
};

Output Generate(const Input& input);

// Custom-normal restoration outside the peeled region.
std::vector<float> RestoreNormals(const std::vector<float>& old_points,const std::vector<int32_t>& old_tris,
 const std::vector<float>& old_tri_normals,const std::vector<float>& points,const std::vector<int32_t>& poly_offsets,
 const std::vector<int32_t>& corner_vert,const std::vector<int32_t>& stamp,const std::vector<float>& current);

// Exposed for the tests that pin these against mathutils.noise.
double Noise(float x,float y,float z);
void Voronoi(float x,float y,float z,float da[4],float pa[12]);
double HashUnit(const std::vector<int64_t>& values);
}
