// общие объявления: данные точки, в которой должно появиться повреждение
#pragma once
#include <vector>
#include <cstdint>
struct SurfaceDamagePoint {
    std::vector<double> mesh_points;
    std::vector<int32_t> mesh_triangles;
    double x=0.0,y=0.0,z=0.0,nx=0.0,ny=0.0,nz=1.0,radius=1.0;
};
