// связываем быстрый расчёт геометрии с вызовами из Python
#include "cable_generator.h"
#include "paper_generator.h"
#include "cloth_sheets.h"
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

// Nanobind binding surface for the host-neutral ProjectDamage core: Params (a
// field-for-field binding of ProjectDamageParams, project_damage_core.h), build_worn (the
// standalone equivalent of project_damage_adapter.cpp's BuildWorn - same CoreInput
// construction, minus the C4D-only I/O a PolygonObject would have supplied),
// the session Cache and MeshWeightMask (the module's own damage mask).
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include "impact_deform.h"
#include "ivy_generator.h"
#include "roots_generator.h"
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include "manifold/manifold.h"
#include "oproject_damage.h"                                  
#include "project_damage_bvh.h"                                                                                        
#include "edge_bevel_params.h"
#include "damaged_bevel_params.h"
#include "scratches_draw_params.h"
#include "concrete_pits_py.h"
#include "metal_tear_py.h"
#include "inner_matched_py.h"
#include "peeling_py.h"
#include "clay_py.h"
#include "surface_guidance.h"
#include "voxel_erosion_py.h"                                                                      
#include "lath_damage.h"
#include "brick_damage.h"
#include "break_rebar.h"
#include "foam.h"
#include "weld.h"
#include "grass_scatter.h"
#include "project_damage_core.h"                                                                                
#include "damage_gen_common.h"
#include "gen_metal_tear.h"
#include "wood_split.h"
#include "tile_damage.h"
#include "erosion.h"
#include "blast_crater.h"
#include "damage_shading.h"
                                                                                             
                                                                                  
                                                                                  

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;
namespace m = manifold;

namespace {

using PointsIn = nb::ndarray<double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
                                                                               
                                                                                       
                                                                          
                                                                                  
                                                                            
                                                                            
using PolysIn = nb::ndarray<int32_t, nb::shape<-1, 4>, nb::c_contig, nb::device::cpu>;
using NormalsIn = nb::ndarray<double, nb::shape<-1, 12>, nb::c_contig, nb::device::cpu>;
using GroupsIn = nb::ndarray<uint8_t, nb::shape<-1>, nb::c_contig, nb::device::cpu>;
                                                                       
                                                        
                                                                            
                                                                   
                                                               
                                                                        
                                                           
using UvsIn = nb::ndarray<double, nb::shape<-1, 4, 2>, nb::c_contig, nb::device::cpu>;
                                                                  
                                                 
                                                                              
                                 
using WeightsIn = nb::ndarray<double, nb::shape<-1>, nb::c_contig, nb::device::cpu>;
                                                                                     
                                                                        
                                            
using AvoidIn = nb::ndarray<bool, nb::shape<-1>, nb::c_contig, nb::device::cpu>;

                                                                              
                                                            
                                                                              
                                                                          
                                                                              
                                                                            
                                                                             
                                       
template <typename T>
nb::ndarray<nb::numpy, T> ToNumpy2D(std::vector<T>&& data, size_t cols)
{
	auto* owned = new std::vector<T>(std::move(data));
	if (owned->empty())
		owned->reserve(1);
	nb::capsule owner(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
	return nb::ndarray<nb::numpy, T>(owned->data(), { owned->size() / cols, cols }, owner);
}

                                                                     
template <typename T>
nb::ndarray<nb::numpy, T> ToNumpy3D(std::vector<T>&& data, size_t d1, size_t d2)
{
	auto* owned = new std::vector<T>(std::move(data));
	if (owned->empty())
		owned->reserve(1);
	nb::capsule owner(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
	return nb::ndarray<nb::numpy, T>(owned->data(), { owned->size() / (d1 * d2), d1, d2 }, owner);
}

template <typename T>
nb::ndarray<nb::numpy, T> ToNumpy1D(std::vector<T>&& data)
{
	auto* owned = new std::vector<T>(std::move(data));
	if (owned->empty())
		owned->reserve(1);
	nb::capsule owner(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
	return nb::ndarray<nb::numpy, T>(owned->data(), { owned->size() }, owner);
}

                                                                           
                                                                                       
                                                                           
                                                                             
                                                                             
                                                               

                                                                                        
                                                                            
                                                                             
                                                                             
                                                                                
                                                            
m::Manifold BuildBoxManifold(const double* pts, size_t nv, const std::vector<ProjectDamagePoly>& polys,
	uint32_t originalId, bool stampFaceIds, const double* uvs)
{
	                                                                                
	                                        
	static_assert(sizeof(ProjectDamagePoly) == 4 * sizeof(int32_t), "ProjectDamagePoly must be abcd int32");
	return BuildInputManifold(pts, nv, polys.empty() ? nullptr : &polys[0].a, polys.size(),
		originalId, stampFaceIds, uvs);
}

                                                                                  
                                                                     
                                               
struct CacheGuard
{
	ProjectDamageDamageCache* cache = DamageCacheAlloc();
	~CacheGuard() { DamageCacheFree(cache); }
};

                                                              
                                                                               
                                                                         
                                                                     
  
                                                                  
                                                                           
                                                                            
                                                
class PyDamageCache
{
public:
	PyDamageCache() : cache_(DamageCacheAlloc()) {}
	~PyDamageCache() { DamageCacheFree(cache_); }
	PyDamageCache(const PyDamageCache&) = delete;
	PyDamageCache& operator=(const PyDamageCache&) = delete;
	ProjectDamageDamageCache* get() { return cache_; }
private:
	ProjectDamageDamageCache* cache_;
};

                                                                            
                                                                                        
                                                                        
                                                                        
                                                                          
                                                                    
                                                                                     
                                                 


                                                                                             
                                                                           
                                                                          
                                                                                                    
                                                                            
                                                                          
            
class ClosestPointBVH
{
public:
	void Build(const EVec* pts, const std::vector<ProjectDamagePoly>& polys)
	{
		tris_.clear();
		tree_ = ProjectDamageBvhTree();
		tris_.reserve(polys.size() * 2);
		for (const ProjectDamagePoly& p : polys)
		{
			tris_.push_back({ pts[p.a], pts[p.b], pts[p.c], p.a, p.b, p.c });
			if (p.c != p.d)
				tris_.push_back({ pts[p.a], pts[p.c], pts[p.d], p.a, p.c, p.d });
		}
		if (tris_.empty())
			return;
		std::vector<AABB> box(tris_.size());
		for (size_t i = 0; i < tris_.size(); i++)
		{
			AABB b;
			b.Grow(tris_[i].v0); b.Grow(tris_[i].v1); b.Grow(tris_[i].v2);
			box[i] = b;
		}
		tree_.Build(box);
	}

	                                                                 
	                                                                         
	                                                                     
	                                                                     
	                           
	double ClosestWeight(const EVec& p, const std::vector<double>& w) const
	{
		if (tree_.Empty())
			return 1.0;
		double bestSq = std::numeric_limits<double>::max();
		int32_t bestTri = -1;
		double bu = 0.0, bv = 0.0, bw = 0.0;
		Query(0, p, bestSq, bestTri, bu, bv, bw);
		if (bestTri < 0)
			return 1.0;
		const Tri& t = tris_[(size_t)bestTri];
		return bu * w[(size_t)t.i0] + bv * w[(size_t)t.i1] + bw * w[(size_t)t.i2];
	}

private:
	struct Tri { EVec v0, v1, v2; int32_t i0, i1, i2; };
	using AABB = ProjectDamageBvhTree::AABB;
	using Node = ProjectDamageBvhTree::Node;

	std::vector<Tri> tris_;
	ProjectDamageBvhTree      tree_;                                                 

	static double SqDistPointAABB(const EVec& p, const AABB& b)
	{
		double sq = 0.0;
		if (p.x < b.lo.x)      sq += (b.lo.x - p.x) * (b.lo.x - p.x);
		else if (p.x > b.hi.x) sq += (p.x - b.hi.x) * (p.x - b.hi.x);
		if (p.y < b.lo.y)      sq += (b.lo.y - p.y) * (b.lo.y - p.y);
		else if (p.y > b.hi.y) sq += (p.y - b.hi.y) * (p.y - b.hi.y);
		if (p.z < b.lo.z)      sq += (b.lo.z - p.z) * (b.lo.z - p.z);
		else if (p.z > b.hi.z) sq += (p.z - b.hi.z) * (p.z - b.hi.z);
		return sq;
	}

	                                                                   
	                                                                      
	                                                    
	static EVec ClosestPtTri(const EVec& p, const EVec& a, const EVec& b, const EVec& c,
		double& u, double& v, double& w)
	{
		const EVec ab = b - a, ac = c - a, ap = p - a;
		const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
		if (d1 <= 0.0 && d2 <= 0.0) { u = 1; v = 0; w = 0; return a; }

		const EVec bp = p - b;
		const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3) { u = 0; v = 1; w = 0; return b; }

		const double vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
		{
			const double t = d1 / (d1 - d3);
			u = 1 - t; v = t; w = 0;
			return a + ab * t;
		}

		const EVec cp = p - c;
		const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
		if (d6 >= 0.0 && d5 <= d6) { u = 0; v = 0; w = 1; return c; }

		const double vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
		{
			const double t = d2 / (d2 - d6);
			u = 1 - t; v = 0; w = t;
			return a + ac * t;
		}

		const double va = d3 * d6 - d5 * d4;
		if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
		{
			const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			u = 0; v = 1 - t; w = t;
			return b + (c - b) * t;
		}

		const double denom = 1.0 / (va + vb + vc);
		v = vb * denom; w = vc * denom; u = 1 - v - w;
		return a + ab * v + ac * w;
	}

	void Query(int32_t nodeIdx, const EVec& p, double& bestSq, int32_t& bestTri,
		double& bu, double& bv, double& bw) const
	{
		const Node& n = tree_.nodes[(size_t)nodeIdx];
		if (SqDistPointAABB(p, n.box) >= bestSq)
			return;
		if (n.IsLeaf())
		{
			for (int32_t oi = n.start; oi < n.start + n.count; oi++)
			{
				const int32_t triIdx = tree_.order[(size_t)oi];
				const Tri& t = tris_[(size_t)triIdx];
				double u, v, w;
				const EVec cp = ClosestPtTri(p, t.v0, t.v1, t.v2, u, v, w);
				const EVec d = cp - p;
				const double sq = m::la::dot(d, d);
				if (sq < bestSq) { bestSq = sq; bestTri = triIdx; bu = u; bv = v; bw = w; }
			}
			return;
		}
		const double dl = SqDistPointAABB(p, tree_.nodes[(size_t)n.left].box);
		const double dr = SqDistPointAABB(p, tree_.nodes[(size_t)n.right].box);
		if (dl <= dr) { Query(n.left, p, bestSq, bestTri, bu, bv, bw); Query(n.right, p, bestSq, bestTri, bu, bv, bw); }
		else          { Query(n.right, p, bestSq, bestTri, bu, bv, bw); Query(n.left, p, bestSq, bestTri, bu, bv, bw); }
	}
};

                                                                                   
                                                                        
                                                                                      
                                                                          
                                                                            
                                                                              
                                                
class MeshWeightMask : public IFieldMask
{
public:
	bool Active() const override { return active_; }

	                                                                   
	                                             
	                                                           
	double Weight(const EVec& p) const override
	{
		if (!active_)
			return 1.0;
		return Clamp01(bvh_->ClosestWeight(p, *weights_));
	}

	                                                                                 
	                                                                          
	                                                         
	double Amount() const override { return amount_; }
	double Size() const override { return size_; }

	bool active_ = false;
	double amount_ = 0.0;
	double size_ = 0.0;
	const ClosestPointBVH* bvh_ = nullptr;
	const std::vector<double>* weights_ = nullptr;
};

                                                                               
                                                                               
                                                                            
                          
// возвращаем Python геометрию вместе с данными её поверхности
static nb::dict ResultToDict(const CoreResult& res, bool haveUvsIn)
{
	                                                                      
	nb::dict whd;
	{
		std::vector<double> rf(res.wallHoleRimFrame), rc(res.wallHoleRimCut), bf(res.wallHoleRubbleFrame);
		std::vector<int32_t> rk(res.wallHoleRimKey.begin(), res.wallHoleRimKey.end()), bk(res.wallHoleRubbleKey.begin(), res.wallHoleRubbleKey.end());
		whd["rim_frame"] = ToNumpy2D(std::move(rf), 12);
		whd["rim_cut"] = ToNumpy2D(std::move(rc), 8);
		whd["rim_key"] = ToNumpy1D(std::move(rk));
		whd["rubble_frame"] = ToNumpy2D(std::move(bf), 12);
		whd["rubble_key"] = ToNumpy1D(std::move(bk));
	}

	                                                                                    
	                                                                                  
	                                                                 
	                                                                      
	                                                                                   
	                                                                        
	                                                                         
	std::vector<int32_t> triClass(res.polys.size(), 6);
    for (int k = 0; k < 5; ++k)
        for (Int32 idx : res.brickPolys[k]) triClass[(size_t)idx] = 4;
    for (int k = 0; k < 3; ++k)
        for (Int32 idx : res.wallpaperPolys[k]) triClass[(size_t)idx] = k == 2 ? 4 : 5;
	for (int l = 0; l < 3; l++)
		for (Int32 idx : res.crackPolys[l])
			triClass[(size_t)idx] = 1 + l;
    for(int k=0;k<4;++k) for(Int32 idx:res.extraPolys[k]) triClass[(size_t)idx] = k == 0 ? 6 : k == 1 ? 0 : 4;
    for (Int32 idx : res.wallpaperBasePolys) triClass[(size_t)idx] = 6;
	for (Int32 idx : res.holePolys)  triClass[(size_t)idx] = 4;
	for (Int32 idx : res.buildPolys) triClass[(size_t)idx] = 5;
	for (Int32 idx : res.breakPolys) triClass[(size_t)idx] = 7;
	for (Int32 idx : res.chipPolys)  triClass[(size_t)idx] = 0;

	std::vector<double> ptsFlat(res.points.size() * 3);
	for (size_t i = 0; i < res.points.size(); i++)
	{
		ptsFlat[i * 3 + 0] = res.points[i].x;
		ptsFlat[i * 3 + 1] = res.points[i].y;
		ptsFlat[i * 3 + 2] = res.points[i].z;
	}
	std::vector<int32_t> polysFlat(res.polys.size() * 4);
	for (size_t i = 0; i < res.polys.size(); i++)
	{
		polysFlat[i * 4 + 0] = res.polys[i].a;
		polysFlat[i * 4 + 1] = res.polys[i].b;
		polysFlat[i * 4 + 2] = res.polys[i].c;
		polysFlat[i * 4 + 3] = res.polys[i].d;
	}

	nb::dict d;
	d["wall_hole"] = whd;
	d["points"] = ToNumpy2D(std::move(ptsFlat), 3);
	d["polys"] = ToNumpy2D(std::move(polysFlat), 4);
	std::vector<double> depositsXYZ;
	for (const auto& p : res.depositPoints) {
		depositsXYZ.push_back(p.x); depositsXYZ.push_back(p.y); depositsXYZ.push_back(p.z);
	}
	std::vector<int32_t> depositsABCD;
	for (const auto& p : res.depositPolys) {
		depositsABCD.push_back(p.a); depositsABCD.push_back(p.b);
		depositsABCD.push_back(p.c); depositsABCD.push_back(p.d);
	}
	d["deposits_points"] = ToNumpy2D(std::move(depositsXYZ), 3);
	d["deposits_polys"] = ToNumpy2D(std::move(depositsABCD), 4);
    auto exportAdditive = [&](const char* pointKey,const char* polyKey,const auto& points,const auto& polys) {
        std::vector<double> xyz;std::vector<int32_t> faces;
        for(const auto& p:points) {xyz.push_back(p.x);xyz.push_back(p.y);xyz.push_back(p.z);}
        for(const auto& p:polys) {faces.push_back(p.a);faces.push_back(p.b);faces.push_back(p.c);faces.push_back(p.d);}
        d[pointKey]=ToNumpy2D(std::move(xyz),3);d[polyKey]=ToNumpy2D(std::move(faces),4);
    };
    exportAdditive("beads_points","beads_polys",res.beadPoints,res.beadPolys);
    exportAdditive("piles_points","piles_polys",res.pilePoints,res.pilePolys);
	std::vector<double> coatingXYZ;
	for (const auto& p : res.coatingPoints) {
		coatingXYZ.push_back(p.x); coatingXYZ.push_back(p.y); coatingXYZ.push_back(p.z);
	}
	std::vector<int32_t> coatingABCD;
	for (const auto& p : res.coatingPolys) {
		coatingABCD.push_back(p.a); coatingABCD.push_back(p.b);
		coatingABCD.push_back(p.c); coatingABCD.push_back(p.d);
	}
	std::vector<double> bricksXYZ;
    for(const auto& p:res.brickMeshPoints) {bricksXYZ.push_back(p.x);bricksXYZ.push_back(p.y);bricksXYZ.push_back(p.z);}
    std::vector<int32_t> bricksABCD;
    for(const auto& p:res.brickMeshPolys) {bricksABCD.push_back(p.a);bricksABCD.push_back(p.b);bricksABCD.push_back(p.c);bricksABCD.push_back(p.d);}
    d["bricks_points"]=ToNumpy2D(std::move(bricksXYZ),3);
    d["bricks_polys"]=ToNumpy2D(std::move(bricksABCD),4);
    d["bricks_piece"]=ToNumpy1D(std::vector<int32_t>(res.brickPiece));
    d["bricks_count"]=res.brickCount;
    std::vector<double> lathXYZ;
    for(const auto& p:res.lathPoints) {lathXYZ.push_back(p.x);lathXYZ.push_back(p.y);lathXYZ.push_back(p.z);}
    std::vector<int32_t> lathABCD;
    for(const auto& p:res.lathPolys) {lathABCD.push_back(p.a);lathABCD.push_back(p.b);lathABCD.push_back(p.c);lathABCD.push_back(p.d);}
    d["lath_points"]=ToNumpy2D(std::move(lathXYZ),3);
    d["lath_polys"]=ToNumpy2D(std::move(lathABCD),4);
    d["lath_piece"]=ToNumpy1D(std::vector<int32_t>(res.lathPiece));
    std::vector<double> debrisXYZ;
    for(const auto& p:res.debrisPoints) {debrisXYZ.push_back(p.x);debrisXYZ.push_back(p.y);debrisXYZ.push_back(p.z);}
    std::vector<int32_t> debrisABCD;
    for(const auto& p:res.debrisPolys) {debrisABCD.push_back(p.a);debrisABCD.push_back(p.b);debrisABCD.push_back(p.c);debrisABCD.push_back(p.d);}
    d["debris_points"]=ToNumpy2D(std::move(debrisXYZ),3);
    d["debris_polys"]=ToNumpy2D(std::move(debrisABCD),4);
    d["debris_piece"]=ToNumpy1D(std::vector<int32_t>(res.debrisPiece));
    d["debris_base_poly"]=ToNumpy1D(std::vector<int32_t>(res.debrisBasePoly));
    d["debris_uvs"]=ToNumpy2D(std::vector<double>(res.debrisUvs),8);
    d["debris_normals"]=ToNumpy2D(std::vector<double>(res.debrisNormals),12);
    {
        std::vector<double> xyz; for (const auto& p : res.rbfPoints) { xyz.push_back(p.x); xyz.push_back(p.y); xyz.push_back(p.z); }
        std::vector<int32_t> abcd; for (const auto& p : res.rbfPolys) { abcd.push_back(p.a); abcd.push_back(p.b); abcd.push_back(p.c); abcd.push_back(p.d); }
        d["rbf_points"] = ToNumpy2D(std::move(xyz), 3);
        d["rbf_polys"] = ToNumpy2D(std::move(abcd), 4);
        d["rbf_piece"] = ToNumpy1D(std::vector<int32_t>(res.rbfPiece));
        d["rbf_base_poly"] = ToNumpy1D(std::vector<int32_t>(res.rbfBasePoly));
        d["rbf_uvs"] = ToNumpy2D(std::vector<double>(res.rbfUvs), 8);
        d["rbf_normals"] = ToNumpy2D(std::vector<double>(res.rbfNormals), 12);
        d["rbf_piece_volume"] = ToNumpy1D(std::vector<double>(res.rbfPieceVolume));
        d["rbf_piece_center"] = ToNumpy2D(std::vector<double>(res.rbfPieceCenter), 3);
    }
    {
        std::vector<double> xyz; for (const auto& p : res.cellfPoints) { xyz.push_back(p.x); xyz.push_back(p.y); xyz.push_back(p.z); }
        std::vector<int32_t> abcd; for (const auto& p : res.cellfPolys) { abcd.push_back(p.a); abcd.push_back(p.b); abcd.push_back(p.c); abcd.push_back(p.d); }
        d["cellf_points"] = ToNumpy2D(std::move(xyz), 3);
        d["cellf_polys"] = ToNumpy2D(std::move(abcd), 4);
        d["cellf_piece"] = ToNumpy1D(std::vector<int32_t>(res.cellfPiece));
        d["cellf_base_poly"] = ToNumpy1D(std::vector<int32_t>(res.cellfBasePoly));
        d["cellf_uvs"] = ToNumpy2D(std::vector<double>(res.cellfUvs), 8);
        d["cellf_normals"] = ToNumpy2D(std::vector<double>(res.cellfNormals), 12);
        d["cellf_piece_volume"] = ToNumpy1D(std::vector<double>(res.cellfPieceVolume));
        d["cellf_piece_center"] = ToNumpy2D(std::vector<double>(res.cellfPieceCenter), 3);
    }
    d["debris_source_uvs"]=bool(res.debrisSourceUvs);
    d["brick_render_polys"]=ToNumpy1D(std::vector<int32_t>(res.brickPolys[4]));
    d["coating_points"] = ToNumpy2D(std::move(coatingXYZ), 3);
	d["coating_polys"] = ToNumpy2D(std::move(coatingABCD), 4);
	d["input_valid"] = res.inputValid;
	                                                                           
	                                                                          
	                                                                            
	                                                                             
	                                                                         
	{
		std::vector<int32_t> baseFlat(res.basePolyOf.begin(), res.basePolyOf.end());
		d["base_poly"] = ToNumpy1D(std::move(baseFlat));
	}
	                                                                           
	                                                                     
	                                                                         
	                                                                          
	                                                                  
	                                                                              
	                                                                      
	                                             
	                                                                          
	                                                                         
	                                                                
	                                                                                   
	              
	if (haveUvsIn)
	{
		const size_t np2 = res.polys.size();
		std::vector<double> uvOut(np2 * 8, std::numeric_limits<double>::quiet_NaN());
		if (res.propCorner.size() == np2 * 4)
		{
			const m::MeshGL& mesh = res.mesh;
			const int32_t mv = (int32_t)mesh.NumVert();
			const int32_t np = (int32_t)mesh.numProp;
			for (size_t i = 0; i < np2; i++)
			{
				if (triClass[i] != 6)
					continue;                                           
				for (int k = 0; k < 4; k++)
				{
					const Int32 q = res.propCorner[i * 4 + (size_t)k];
					if (q == NOTOK || q < 0 || q >= mv)
						continue;                                             
					uvOut[i * 8 + (size_t)k * 2 + 0] = mesh.vertProperties[(size_t)q * (size_t)np + 3];
					uvOut[i * 8 + (size_t)k * 2 + 1] = mesh.vertProperties[(size_t)q * (size_t)np + 4];
				}
			}
		}
		                                                                     
		                                                                  
		                                                                   
		d["uvs"] = ToNumpy3D(std::move(uvOut), 4, 2);
	}
	else
	{
		d["uvs"] = nb::none();
	}
	d["rust_source"] = ToNumpy1D(std::vector<float>(res.rustSource));
	d["rust"] = ToNumpy1D(std::vector<float>(res.rustPoint));
	d["rust_streak"] = ToNumpy1D(std::vector<float>(res.rustStreakPoint));
	d["coating_rust"] = ToNumpy1D(std::vector<float>(res.coatingRust));
	d["coating_rust_streak"] = ToNumpy1D(std::vector<float>(res.coatingStreak));
	d["heat_exposure"] = ToNumpy1D(std::vector<float>(res.heatExposure));
	d["heat_char"] = ToNumpy1D(std::vector<float>(res.heatCharAmount));
	d["heat_char_depth"] = ToNumpy1D(std::vector<float>(res.heatCharDepth));
    d["wallpaper_front_polys"] = ToNumpy1D(std::vector<int32_t>(res.wallpaperPolys[0]));
    d["wallpaper_back_polys"] = ToNumpy1D(std::vector<int32_t>(res.wallpaperPolys[1]));
    d["wallpaper_wall_polys"] = ToNumpy1D(std::vector<int32_t>(res.wallpaperPolys[2]));
    d["brick_front_polys"] = ToNumpy1D(std::vector<int32_t>(res.brickPolys[0]));
    d["brick_break_polys"] = ToNumpy1D(std::vector<int32_t>(res.brickPolys[1]));
    d["brick_seam_polys"] = ToNumpy1D(std::vector<int32_t>(res.brickPolys[2]));
    d["brick_behind_polys"] = ToNumpy1D(std::vector<int32_t>(res.brickPolys[3]));
        d["bevel_polys"] = ToNumpy1D(std::vector<int32_t>(res.extraPolys[0]));
    d["wear_polys"] = ToNumpy1D(std::vector<int32_t>(res.extraPolys[1]));
    d["scratch_groove_polys"] = ToNumpy1D(std::vector<int32_t>(res.extraPolys[2]));
    d["scratch_scuff_polys"] = ToNumpy1D(std::vector<int32_t>(res.extraPolys[3]));
    d["edge_wear"] = ToNumpy1D(std::vector<float>(res.edgeWear));
    d["bevel_damage"] = ToNumpy1D(std::vector<float>(res.bevelDamage));
    d["bevel_segments_points"] = ToNumpy2D(std::vector<double>(res.bevelSegPts), 3);                                  
    d["bevel_segments_polys"] = ToNumpy2D(std::vector<int32_t>(res.bevelSegTris), 3);
    d["scratch"] = ToNumpy1D(std::vector<float>(res.scratchMask));
    d["metal_dent"] = ToNumpy1D(std::vector<float>(res.metalDent));
    d["metal_crease"] = ToNumpy1D(std::vector<float>(res.metalCrease));
    d["metal_contact"] = ToNumpy1D(std::vector<float>(res.metalContact));
    d["wallpaper_base_polys"] = ToNumpy1D(std::vector<int32_t>(res.wallpaperBasePolys));
    d["wallpaper_flap_polys"] = ToNumpy1D(std::vector<int32_t>(res.wallpaperFlapPolys));
	d["corner_normals"] = ToNumpy2D(std::vector<double>(res.cornerNormals), 12);
	d["tri_class"] = ToNumpy1D(std::move(triClass));
    std::vector<int32_t> effectIds(res.effectIds.begin(),res.effectIds.end()), groundRubble;
    for (size_t i = 0; i < effectIds.size(); ++i) {
        if (effectIds[i] == 30) { groundRubble.push_back(int32_t(i)); effectIds[i] = 17; }
    }
    d["ground_rubble_polys"] = ToNumpy1D(std::move(groundRubble));
    d["effect_ids"] = ToNumpy1D(std::move(effectIds));
	                                                                        
	                                                                         
	                                                                               
	d["passthrough"] = res.passthrough;
	                                                                         
	                                                               
	std::vector<double> rbPoints; rbPoints.reserve(res.rebarPoints.size()*3);
    for(const auto& p:res.rebarPoints) { rbPoints.push_back(p.x);rbPoints.push_back(p.y);rbPoints.push_back(p.z); }
    std::vector<int32_t> rbPolys; rbPolys.reserve(res.rebarPolys.size()*4);
    for(const auto& p:res.rebarPolys) { rbPolys.push_back(p.a);rbPolys.push_back(p.b);rbPolys.push_back(p.c);rbPolys.push_back(p.d); }
    d["rebar_points"]=ToNumpy2D(std::move(rbPoints),3);
    d["rebar_polys"]=ToNumpy2D(std::move(rbPolys),4);
    d["rebar_bar"]=ToNumpy1D(std::vector<int32_t>(res.rebarPolyBar));
    d["rebar_part"]=ToNumpy1D(std::vector<int32_t>(res.rebarPolyPart));
    d["rebar_grain"]=ToNumpy1D(std::vector<float>(res.rebarGrain));
    d["rebar_brick_frame"]=ToNumpy2D(std::vector<double>(res.rebarBrickFrame),12);
    d["rebar_brick_key"]=ToNumpy1D(std::vector<int32_t>(res.rebarBrickKey));
    d["rebar_brick_cut"]=ToNumpy2D(std::vector<double>(res.rebarBrickCut),8);
    d["rebar_exposure"]=ToNumpy1D(std::vector<float>(res.rebarExposure));
    d["rebar_bars"]=res.rebarBars;
    std::vector<double> crPoints;
    for(const auto& p:res.concreteRebarPoints) { crPoints.push_back(p.x);crPoints.push_back(p.y);crPoints.push_back(p.z); }
    std::vector<int32_t> crPolys;
    for(const auto& p:res.concreteRebarPolys) { crPolys.push_back(p.a);crPolys.push_back(p.b);crPolys.push_back(p.c);crPolys.push_back(p.d); }
    d["concrete_rebar_points"]=ToNumpy2D(std::move(crPoints),3);
    d["concrete_rebar_polys"]=ToNumpy2D(std::move(crPolys),4);

    d["counts"] = std::make_tuple((int64_t)res.points.size(), (int64_t)res.polys.size(),
		(int64_t)res.chipPolys.size());
	return d;
}

// принимаем массивы из Blender и запускаем основной расчёт
nb::dict BuildWornPy(PointsIn points, PolysIn polysIn, const ProjectDamageParams& params,
	PyDamageCache* cache, uint64_t cacheFingerprint, UvsIn uvsIn,
	WeightsIn maskWeightsIn, double maskAmount, double maskSize, AvoidIn avoidPolysIn,
	std::vector<PointsIn> guideSplinesIn, WeightsIn heatWeightsIn, WeightsIn coatingZoneWeightsIn,
	std::vector<Int32> guideLevelsIn, std::vector<PointsIn> surfacePathsIn, PointsIn surfaceNormalsIn, WeightsIn rustWeightsIn,
	WeightsIn fabricWeightsIn, AvoidIn fabricAvoidIn, std::vector<PointsIn> concreteStrokesIn,
	std::vector<PointsIn> coatingPathsIn, PointsIn coatingNormalsIn,
	std::vector<PointsIn> rustPathsIn, PointsIn rustNormalsIn, std::vector<PointsIn> woodStrokesIn, std::vector<PointsIn> lathStrokesIn,
	std::vector<PointsIn> wallpaperPathsIn, PointsIn wallpaperNormalsIn,
	std::vector<PointsIn> fabricHolesIn, PointsIn fabricHoleNormalsIn,
	std::vector<PointsIn> fabricEdgesIn, PointsIn fabricEdgeNormalsIn,
    std::vector<PointsIn> bulletStrokesIn, std::vector<PointsIn> groundStrokesIn, std::vector<PointsIn> brickStrokesIn, std::vector<PointsIn> fissureStrokesIn, std::vector<PointsIn> rustyStrokesIn,
    std::vector<PointsIn> glassHolesIn, PointsIn glassHoleNormalsIn, std::vector<PointsIn> glassImpactsIn, PointsIn glassImpactNormalsIn, std::vector<PointsIn> bevelStrokesIn, std::vector<PointsIn> damagedBevelStrokesIn, std::vector<PointsIn> scratchStrokesIn,
    std::vector<PointsIn> chipStrokesIn, double chipPaintRadius,
    std::vector<PointsIn> drawMaskPathsIn, double drawMaskRadius, double drawMaskSoft,
    bool drawMaskInvert, double drawMaskSize, bool drawMaskHeat,
	std::vector<PointsIn> rbfCutIn, std::vector<PointsIn> rbfImpactIn,
	std::vector<PointsIn> fractHelperIn, std::vector<PointsIn> cellfStrokesIn, NormalsIn srcNormalsIn)
{
	const size_t nv = points.shape(0);
	const size_t nt = polysIn.shape(0);
	if (params.surfType < 0 || params.surfType > 1) throw std::invalid_argument("surf_type must be Concrete (0) or Wood Knife (1)");
	for (double x : {double(params.surfGrainX), double(params.surfGrainY), double(params.surfGrainZ),
	                 double(params.surfKnifeJitter), double(params.surfKnifeBack)})
		if (!std::isfinite(x)) throw std::invalid_argument("Surface Wood Knife settings must be finite");
	if (params.surfKnifeJitter < 0 || params.surfKnifeJitter > 180 || params.surfKnifeBack < 0 || params.surfKnifeBack > 1)
		throw std::invalid_argument("Surface Wood Knife jitter must be 0..180 degrees and reverse share 0..1");
	if (!std::isfinite(params.damageSmoothAngle) || !std::isfinite(params.baseSmoothAngle))
		throw std::invalid_argument("damage shading angles must be finite");
	if (nv == 0 || nt == 0)
		throw std::invalid_argument("build_worn: empty mesh");
	                                                                          
	                                                                         
	                                                                         
	const bool haveUvsIn = uvsIn.is_valid();
	if (haveUvsIn && (size_t)uvsIn.shape(0) != nt)
		throw std::invalid_argument("build_worn: uvs must have one row per polygon");
	                                                                          
	                                                                
	                                 
	const bool haveMaskIn = maskWeightsIn.is_valid();
	if (haveMaskIn && (size_t)maskWeightsIn.shape(0) != nv)
		throw std::invalid_argument("build_worn: mask_weights must have one entry per input point");
	const bool haveAvoidIn = avoidPolysIn.is_valid();
	if (haveAvoidIn && (size_t)avoidPolysIn.shape(0) != nt)
		throw std::invalid_argument("build_worn: avoid_polys must have one entry per input polygon");

	                                                                      
	                                                                        
	                                                                      
	                                                                         
	                                                                         
	                                                                        
	                                                                           
	                                                                           
	                                                                      
	         
	                                                                         
	                                                                         
	                                                                         
	                                                                              
	                                                                           
	                                                                          
	                                                                      
	                                                                                  
	                                                                     
	                                                                         
	                                                                      
	                                                                     
	                                                                          
	                                                                     
	                                                                      
	                                                                        
	                                                         
	                                                                      
	                                                                     
	std::vector<double> pts(points.data(), points.data() + nv * 3);
	ProjectDamageParams prm = params;                                                   
    if (prm.fractPaintOnly)
        for (auto& level : prm.fract)
        {
            if (!std::isfinite(level.length) || !std::isfinite(level.paintLengthMin)
                || !std::isfinite(level.paintDistortion)
                || !std::isfinite(level.paintDistortionScale))
                throw std::invalid_argument("paint fracture length/distortion parameters must be finite");
            level.paintDistortion = level.paintDistortion < 0.0 ? -1.0 : Clamp01(level.paintDistortion);
            level.paintDistortionScale = std::max(0.02, level.paintDistortionScale);
        }
	                                                                         
	                                                                
	std::vector<float> deformAmount;
	                                                                          
	if (prm.deform.enable) ApplyImpactDeform(pts.data(), (int)nv, prm.deform, &deformAmount, polysIn.data(), nt);
	for (const PointsIn& g : fractHelperIn)
	{
		const size_t n = g.shape(0);
		const double* gd = g.data();
		std::vector<EVec> poly;
		for (size_t i = 0; i < n; i++)
		{
			for (int j = 0; j < 3; j++) if (!std::isfinite(gd[i * 3 + j]))
				throw std::invalid_argument("fract_helper_strokes must be finite");
			poly.push_back(EVec(gd[i * 3], gd[i * 3 + 1], gd[i * 3 + 2]));
		}
		if (poly.size() >= 2) prm.fractHelperStrokes.push_back(std::move(poly));
	}
	if (prm.fabric.enable)
	{
        if (!std::isfinite(prm.fabric.distributionScale) || !std::isfinite(prm.fabric.distributionStrength)
            || !std::isfinite(prm.fabric.shapeNoiseScale) || !std::isfinite(prm.fabric.shapeNoiseStrength))
            throw std::invalid_argument("fabric noise parameters must be finite");
		if (!std::isfinite(prm.fabric.thickness)) throw std::invalid_argument("fabric_thickness must be finite");
		if (!std::isfinite(prm.fabric.cutMargin)) throw std::invalid_argument("fabric_cut_margin must be finite");
		if (!std::isfinite(prm.fabric.detail)) throw std::invalid_argument("fabric_detail must be finite");
		if (!std::isfinite(prm.fabric.holeLengthMin)) throw std::invalid_argument("fabric_hole_length_min must be finite");
		if (!std::isfinite(prm.fabric.holeLengthMax)) throw std::invalid_argument("fabric_hole_length_max must be finite");
		if (!std::isfinite(prm.fabric.holeWidthMin)) throw std::invalid_argument("fabric_hole_width_min must be finite");
		if (!std::isfinite(prm.fabric.holeWidthMax)) throw std::invalid_argument("fabric_hole_width_max must be finite");
		if (!std::isfinite(prm.fabric.holeJagged)) throw std::invalid_argument("fabric_hole_jagged must be finite");
		if (!std::isfinite(prm.fabric.tearRip) || !std::isfinite(prm.fabric.tearCorner) || !std::isfinite(prm.fabric.tearWorn)
			|| !std::isfinite(prm.fabric.tearPuncture) || !std::isfinite(prm.fabric.grainSnap) || !std::isfinite(prm.fabric.flapLift))
			throw std::invalid_argument("fabric tear shape values must be finite");
		if (!std::isfinite(prm.fabric.yarnSize) || prm.fabric.yarnSize <= 0.0) throw std::invalid_argument("fabric_yarn_size must be finite and > 0");
		if (!std::isfinite(prm.fabric.holeLobes)) throw std::invalid_argument("fabric_hole_lobes must be finite");
		if (!std::isfinite(prm.fabric.holeMeander)) throw std::invalid_argument("fabric_hole_meander must be finite");
		if (!std::isfinite(prm.fabric.holeBridge)) throw std::invalid_argument("fabric_hole_bridge must be finite");
		if (!std::isfinite(prm.fabric.holeDirX)) throw std::invalid_argument("fabric_hole_dir_x must be finite");
		if (!std::isfinite(prm.fabric.holeDirY)) throw std::invalid_argument("fabric_hole_dir_y must be finite");
		if (!std::isfinite(prm.fabric.holeDirZ)) throw std::invalid_argument("fabric_hole_dir_z must be finite");
		if (!std::isfinite(prm.fabric.holeDirJitter)) throw std::invalid_argument("fabric_hole_dir_jitter must be finite");
		if (!std::isfinite(prm.fabric.edgeSpacingJitter)) throw std::invalid_argument("fabric_edge_spacing_jitter must be finite");
		if (!std::isfinite(prm.fabric.edgeLengthMin)) throw std::invalid_argument("fabric_edge_length_min must be finite");
		if (!std::isfinite(prm.fabric.edgeLengthMax)) throw std::invalid_argument("fabric_edge_length_max must be finite");
		if (!std::isfinite(prm.fabric.edgeWidthMin)) throw std::invalid_argument("fabric_edge_width_min must be finite");
		if (!std::isfinite(prm.fabric.edgeWidthMax)) throw std::invalid_argument("fabric_edge_width_max must be finite");
		if (!std::isfinite(prm.fabric.edgeTaper)) throw std::invalid_argument("fabric_edge_taper must be finite");
		if (!std::isfinite(prm.fabric.edgeJagged)) throw std::invalid_argument("fabric_edge_jagged must be finite");
		if (!std::isfinite(prm.fabric.edgeMeander)) throw std::invalid_argument("fabric_edge_meander must be finite");
		if (!std::isfinite(prm.fabric.edgeAngleJitter)) throw std::invalid_argument("fabric_edge_angle_jitter must be finite");
		if (!std::isfinite(prm.fabric.threadRadius)) throw std::invalid_argument("fabric_thread_radius must be finite");
		if (!std::isfinite(prm.fabric.threadSpacing)) throw std::invalid_argument("fabric_thread_spacing must be finite");
		if (!std::isfinite(prm.fabric.bridgeShare)) throw std::invalid_argument("fabric_bridge_share must be finite");
		if (!std::isfinite(prm.fabric.threadSag)) throw std::invalid_argument("fabric_thread_sag must be finite");
		if (!std::isfinite(prm.fabric.threadSkew)) throw std::invalid_argument("fabric_thread_skew must be finite");
		if (!std::isfinite(prm.fabric.threadWobble)) throw std::invalid_argument("fabric_thread_wobble must be finite");
		if (!std::isfinite(prm.fabric.brokenShare)) throw std::invalid_argument("fabric_broken_share must be finite");
		if (!std::isfinite(prm.fabric.threadDroop)) throw std::invalid_argument("fabric_thread_droop must be finite");
		if (!std::isfinite(prm.fabric.threadCurl)) throw std::invalid_argument("fabric_thread_curl must be finite");
		if (!std::isfinite(prm.fabric.threadTaper)) throw std::invalid_argument("fabric_thread_taper must be finite");
		if (!std::isfinite(prm.fabric.fraySpacing)) throw std::invalid_argument("fabric_fray_spacing must be finite");
		if (!std::isfinite(prm.fabric.frayLengthMin)) throw std::invalid_argument("fabric_fray_length_min must be finite");
		if (!std::isfinite(prm.fabric.frayLengthMax)) throw std::invalid_argument("fabric_fray_length_max must be finite");
		if (!std::isfinite(prm.fabric.hangLengthMin)) throw std::invalid_argument("fabric_hang_length_min must be finite");
		if (!std::isfinite(prm.fabric.hangLengthMax)) throw std::invalid_argument("fabric_hang_length_max must be finite");
		if (!std::isfinite(prm.fabric.gravityX)) throw std::invalid_argument("fabric_gravity_x must be finite");
		if (!std::isfinite(prm.fabric.gravityY)) throw std::invalid_argument("fabric_gravity_y must be finite");
		if (!std::isfinite(prm.fabric.gravityZ)) throw std::invalid_argument("fabric_gravity_z must be finite");
	}
	if (prm.concrete.enable)
	{
		if (!std::isfinite(prm.concrete.detail)) throw std::invalid_argument("concrete_detail must be finite");
		if (!std::isfinite(prm.concrete.sizeMin)) throw std::invalid_argument("concrete_size_min must be finite");
		if (!std::isfinite(prm.concrete.sizeMax)) throw std::invalid_argument("concrete_size_max must be finite");
		if (!std::isfinite(prm.concrete.stretch)) throw std::invalid_argument("concrete_stretch must be finite");
		if (!std::isfinite(prm.concrete.edgeBias)) throw std::invalid_argument("concrete_edge_bias must be finite");
		if (!std::isfinite(prm.concrete.paintRadius)) throw std::invalid_argument("concrete_paint_radius must be finite");
		if (!std::isfinite(prm.concrete.paintSnap)) throw std::invalid_argument("concrete_paint_snap must be finite");
		if (!std::isfinite(prm.concrete.paintClose)) throw std::invalid_argument("concrete_paint_close must be finite");
		if (!std::isfinite(prm.concrete.depthMin)) throw std::invalid_argument("concrete_depth_min must be finite");
		if (!std::isfinite(prm.concrete.depthMax)) throw std::invalid_argument("concrete_depth_max must be finite");
		if (!std::isfinite(prm.concrete.jagged)) throw std::invalid_argument("concrete_jagged must be finite");
		if (!std::isfinite(prm.concrete.outlineScale)) throw std::invalid_argument("concrete_outline_scale must be finite");
		if (!std::isfinite(prm.concrete.terraceSkin)) throw std::invalid_argument("concrete_terrace_skin must be finite");
		if (!std::isfinite(prm.concrete.terraceInset)) throw std::invalid_argument("concrete_terrace_inset must be finite");
		if (!std::isfinite(prm.concrete.wallSoft)) throw std::invalid_argument("concrete_wall_soft must be finite");
		if (!std::isfinite(prm.concrete.roughness)) throw std::invalid_argument("concrete_roughness must be finite");
        if (!std::isfinite(prm.concrete.surfaceDistortion) || !std::isfinite(prm.concrete.surfaceRoughness) || !std::isfinite(prm.concrete.surfaceStrength))
            throw std::invalid_argument("concrete procedural surface controls must be finite");
		if (!std::isfinite(prm.concrete.aggregateSize)) throw std::invalid_argument("concrete_aggregate_size must be finite");
		if (!std::isfinite(prm.concrete.thicknessCap)) throw std::invalid_argument("concrete_thickness_cap must be finite");
		if (!std::isfinite(prm.concrete.holeChance)) throw std::invalid_argument("concrete_hole_chance must be finite");
		if (!std::isfinite(prm.concrete.holeSize)) throw std::invalid_argument("concrete_hole_size must be finite");
		if (!std::isfinite(prm.concrete.holeJagged)) throw std::invalid_argument("concrete_hole_jagged must be finite");
		if (!std::isfinite(prm.concrete.crackLengthMin)) throw std::invalid_argument("concrete_crack_length_min must be finite");
		if (!std::isfinite(prm.concrete.crackLengthMax)) throw std::invalid_argument("concrete_crack_length_max must be finite");
		if (!std::isfinite(prm.concrete.crackWidth)) throw std::invalid_argument("concrete_crack_width must be finite");
		if (!std::isfinite(prm.concrete.crackDepth)) throw std::invalid_argument("concrete_crack_depth must be finite");
		if (!std::isfinite(prm.concrete.crackMeander)) throw std::invalid_argument("concrete_crack_meander must be finite");
		if (!std::isfinite(prm.concrete.crackBranch)) throw std::invalid_argument("concrete_crack_branch must be finite");
		if (!std::isfinite(prm.concrete.crackInterior)) throw std::invalid_argument("concrete_crack_interior must be finite");
		if (!std::isfinite(prm.concrete.rebarAmount)) throw std::invalid_argument("concrete_rebar_amount must be finite");
		if (!std::isfinite(prm.concrete.rebarDensity)) throw std::invalid_argument("concrete_rebar_density must be finite");
		if (!std::isfinite(prm.concrete.rebarRadius)) throw std::invalid_argument("concrete_rebar_radius must be finite");
		if (!std::isfinite(prm.concrete.rebarScale)) throw std::invalid_argument("concrete_rebar_scale must be finite");
		if (!std::isfinite(prm.concrete.rebarSpacingX)) throw std::invalid_argument("concrete_rebar_spacing_x must be finite");
		if (!std::isfinite(prm.concrete.rebarSpacingY)) throw std::invalid_argument("concrete_rebar_spacing_y must be finite");
		if (!std::isfinite(prm.concrete.rebarAngle)) throw std::invalid_argument("concrete_rebar_angle must be finite");
		if (!std::isfinite(prm.concrete.rebarCover)) throw std::invalid_argument("concrete_rebar_cover must be finite");
		if (!std::isfinite(prm.concrete.rebarExpose)) throw std::invalid_argument("concrete_rebar_expose must be finite");
		if (!std::isfinite(prm.concrete.rebarEmbed)) throw std::invalid_argument("concrete_rebar_embed must be finite");
		if (!std::isfinite(prm.concrete.rebarRibs)) throw std::invalid_argument("concrete_rebar_ribs must be finite");
		if (!std::isfinite(prm.concrete.rebarJitter)) throw std::invalid_argument("concrete_rebar_jitter must be finite");
		if (!std::isfinite(prm.concrete.rebarWobble)) throw std::invalid_argument("concrete_rebar_wobble must be finite");
		if (!std::isfinite(prm.concrete.rebarBend)) throw std::invalid_argument("concrete_rebar_bend must be finite");
		if (!std::isfinite(prm.concrete.rebarBroken)) throw std::invalid_argument("concrete_rebar_broken must be finite");
		if (!std::isfinite(prm.concrete.upX)) throw std::invalid_argument("concrete_up_x must be finite");
		if (!std::isfinite(prm.concrete.upY)) throw std::invalid_argument("concrete_up_y must be finite");
		if (!std::isfinite(prm.concrete.upZ)) throw std::invalid_argument("concrete_up_z must be finite");
	}

	                                                                           
	                                               
	const std::vector<double> uvsVec = haveUvsIn
		? std::vector<double>(uvsIn.data(), uvsIn.data() + nt * 8)
		: std::vector<double>();
	                                                              
	                                             
	                                    
	const std::vector<double> maskWeights = haveMaskIn
		? std::vector<double>(maskWeightsIn.data(), maskWeightsIn.data() + nv)
		: std::vector<double>();
	std::vector<bool> avoidVec;
	if (haveAvoidIn)
	{
		avoidVec.resize(nt);
		const bool* ad = avoidPolysIn.data();
		for (size_t i = 0; i < nt; i++)
			avoidVec[i] = ad[i];
	}
	                                                                          
	                                                                          
	                                                                        
	                                                        
	if (prm.hit.enable && !std::isfinite(prm.hit.detail)) throw nb::value_error("hit_detail must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.radiusScale)) throw nb::value_error("hit_radius_scale must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.proximity)) throw nb::value_error("hit_proximity must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.proxShrink)) throw nb::value_error("hit_prox_shrink must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.oblique)) throw nb::value_error("hit_oblique must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.layerThickness)) throw nb::value_error("hit_layer_thickness must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.coreSize)) throw nb::value_error("hit_core_size must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.coreDepth)) throw nb::value_error("hit_core_depth must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.coreJagged)) throw nb::value_error("hit_core_jagged must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.roughness)) throw nb::value_error("hit_roughness must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.aggregateSize)) throw nb::value_error("hit_aggregate_size must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.thicknessCap)) throw nb::value_error("hit_thickness_cap must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.holeSize)) throw nb::value_error("hit_hole_size must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.plateSize)) throw nb::value_error("hit_plate_size must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.plateGrowth)) throw nb::value_error("hit_plate_growth must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.zoneJagged)) throw nb::value_error("hit_zone_jagged must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.shatter)) throw nb::value_error("hit_shatter must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.tilt)) throw nb::value_error("hit_tilt must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.edgeJagged)) throw nb::value_error("hit_edge_jagged must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.chip)) throw nb::value_error("hit_chip must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.crackWidth)) throw nb::value_error("hit_crack_width must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.crackDepth)) throw nb::value_error("hit_crack_depth must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.radialLengthMin)) throw nb::value_error("hit_radial_length_min must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.radialLengthMax)) throw nb::value_error("hit_radial_length_max must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.radialMeander)) throw nb::value_error("hit_radial_meander must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.radialBranch)) throw nb::value_error("hit_radial_branch must be finite");
	if (prm.hit.enable && !std::isfinite(prm.hit.ringFill)) throw nb::value_error("hit_ring_fill must be finite");
    CheckMetalTear(prm.metalTear);
	if (prm.plaster.enable && !std::isfinite(prm.plaster.edgeLength)) throw nb::value_error("plaster_edge_length must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.detail)) throw nb::value_error("plaster_detail must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.strength)) throw nb::value_error("plaster_strength must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.sink)) throw nb::value_error("plaster_sink must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.thicknessCap)) throw nb::value_error("plaster_thickness_cap must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.undulation)) throw nb::value_error("plaster_undulation must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.undulationScale)) throw nb::value_error("plaster_undulation_scale must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.coverage)) throw nb::value_error("plaster_coverage must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.lengthMin)) throw nb::value_error("plaster_length_min must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.lengthMax)) throw nb::value_error("plaster_length_max must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.widthMin)) throw nb::value_error("plaster_width_min must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.widthMax)) throw nb::value_error("plaster_width_max must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.angle)) throw nb::value_error("plaster_angle must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.angleRandom)) throw nb::value_error("plaster_angle_random must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crossChance)) throw nb::value_error("plaster_cross_chance must be finite");
	CheckVoxelErosion(prm.erosion);
    CheckClay(prm.clay);
    if (prm.damagedBevel.enable) {
        const auto& b = prm.damagedBevel;
        for (double v : {b.detail,b.angle,b.paintRadius,b.paintAlign,b.paintCover,b.maxTurn,
             b.width,b.smoothness,b.uneven,b.maxShare,b.damage,b.damageAmount,b.damageFactor,
             b.damageDepth,b.patchLength,b.chipScatter,b.jagged,b.maskWidth,b.maskNoise,
             b.maskScale,b.upX,b.upY,b.upZ,b.placementRandom,b.smoothDamageAmount,b.profileShape})
            if (!std::isfinite(v)) throw nb::value_error("Damaged Edge Bevel parameters must be finite");
    }
	if (prm.plaster.enable && !std::isfinite(prm.plaster.curve)) throw nb::value_error("plaster_curve must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.flatten)) throw nb::value_error("plaster_flatten must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.tilt)) throw nb::value_error("plaster_tilt must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.jagged)) throw nb::value_error("plaster_jagged must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.streaks)) throw nb::value_error("plaster_streaks must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.ridgeHeight)) throw nb::value_error("plaster_ridge_height must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.ridgeWidth)) throw nb::value_error("plaster_ridge_width must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.ridgeSharp)) throw nb::value_error("plaster_ridge_sharp must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.ridgeTrail)) throw nb::value_error("plaster_ridge_trail must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.grain)) throw nb::value_error("plaster_grain must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.grainSize)) throw nb::value_error("plaster_grain_size must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.pits)) throw nb::value_error("plaster_pits must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackAmount)) throw nb::value_error("plaster_crack_amount must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackScale)) throw nb::value_error("plaster_crack_scale must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackWidth)) throw nb::value_error("plaster_crack_width must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackDepth)) throw nb::value_error("plaster_crack_depth must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackMeander)) throw nb::value_error("plaster_crack_meander must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackBreakup)) throw nb::value_error("plaster_crack_breakup must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackStep)) throw nb::value_error("plaster_crack_step must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.crackLip)) throw nb::value_error("plaster_crack_lip must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.upX)) throw nb::value_error("plaster_up_x must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.upY)) throw nb::value_error("plaster_up_y must be finite");
	if (prm.plaster.enable && !std::isfinite(prm.plaster.upZ)) throw nb::value_error("plaster_up_z must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.detail)) throw nb::value_error("wood_detail must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grainX)) throw nb::value_error("wood_grain_x must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grainY)) throw nb::value_error("wood_grain_y must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grainZ)) throw nb::value_error("wood_grain_z must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sizeMin)) throw nb::value_error("wood_size_min must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sizeMax)) throw nb::value_error("wood_size_max must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.stretch)) throw nb::value_error("wood_stretch must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.endBias)) throw nb::value_error("wood_end_bias must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.edgeBias)) throw nb::value_error("wood_edge_bias must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.paintRadius)) throw nb::value_error("wood_paint_radius must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.paintSnap)) throw nb::value_error("wood_paint_snap must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.paintClose)) throw nb::value_error("wood_paint_close must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.depthMin)) throw nb::value_error("wood_depth_min must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.depthMax)) throw nb::value_error("wood_depth_max must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.layerInset)) throw nb::value_error("wood_layer_inset must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.fiberDepthVar)) throw nb::value_error("wood_fiber_depth_var must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.thicknessCap)) throw nb::value_error("wood_thickness_cap must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.jagged)) throw nb::value_error("wood_jagged must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.fiberWidth)) throw nb::value_error("wood_fiber_width must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.fiberJitter)) throw nb::value_error("wood_fiber_jitter must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.splinterLength)) throw nb::value_error("wood_splinter_length must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.splinterSharp)) throw nb::value_error("wood_splinter_sharp must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.splinterChunk)) throw nb::value_error("wood_splinter_chunk must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.splinterRamp)) throw nb::value_error("wood_splinter_ramp must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.floorFibers)) throw nb::value_error("wood_floor_fibers must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grooveSpacing)) throw nb::value_error("wood_groove_spacing must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grooveDepth)) throw nb::value_error("wood_groove_depth must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grooveWave)) throw nb::value_error("wood_groove_wave must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.grooveBreakup)) throw nb::value_error("wood_groove_breakup must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.weatherHalo)) throw nb::value_error("wood_weather_halo must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.breakChance)) throw nb::value_error("wood_break_chance must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.breakSize)) throw nb::value_error("wood_break_size must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.checkLengthMin)) throw nb::value_error("wood_check_length_min must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.checkLengthMax)) throw nb::value_error("wood_check_length_max must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.checkWidth)) throw nb::value_error("wood_check_width must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.checkDepth)) throw nb::value_error("wood_check_depth must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.checkMeander)) throw nb::value_error("wood_check_meander must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.knotSize)) throw nb::value_error("wood_knot_size must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.knotStretch)) throw nb::value_error("wood_knot_stretch must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.knotDepth)) throw nb::value_error("wood_knot_depth must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverAmount)) throw nb::value_error("wood_sliver_amount must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverLength)) throw nb::value_error("wood_sliver_length must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverWidth)) throw nb::value_error("wood_sliver_width must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverThick)) throw nb::value_error("wood_sliver_thick must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverLift)) throw nb::value_error("wood_sliver_lift must be finite");
	if (prm.wood.enable && !std::isfinite(prm.wood.sliverEmbed)) throw nb::value_error("wood_sliver_embed must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.detail)) throw nb::value_error("lath_detail must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.sizeMin)) throw nb::value_error("lath_size_min must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.sizeMax)) throw nb::value_error("lath_size_max must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.stretch)) throw nb::value_error("lath_stretch must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.edgeBias)) throw nb::value_error("lath_edge_bias must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.paintRadius)) throw nb::value_error("lath_paint_radius must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.paintSnap)) throw nb::value_error("lath_paint_snap must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.paintClose)) throw nb::value_error("lath_paint_close must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.plasterMin)) throw nb::value_error("lath_plaster_min must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.plasterMax)) throw nb::value_error("lath_plaster_max must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.jagged)) throw nb::value_error("lath_jagged must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.outlineScale)) throw nb::value_error("lath_outline_scale must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crumble)) throw nb::value_error("lath_crumble must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crumbleSize)) throw nb::value_error("lath_crumble_size must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.terraceSkin)) throw nb::value_error("lath_terrace_skin must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.terraceInset)) throw nb::value_error("lath_terrace_inset must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.wallSoft)) throw nb::value_error("lath_wall_soft must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.roughness)) throw nb::value_error("lath_roughness must be finite");
	if (prm.brick.enable && (!std::isfinite(prm.brick.damageDepth) || !std::isfinite(prm.brick.damageScale) || !std::isfinite(prm.brick.damageNoise) || !std::isfinite(prm.brick.remeshSize))) throw nb::value_error("brick pocket depth, scale and noise must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.thicknessCap)) throw nb::value_error("lath_thickness_cap must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.cavityDepth)) throw nb::value_error("lath_cavity_depth must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.keys)) throw nb::value_error("lath_keys must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.keySize)) throw nb::value_error("lath_key_size must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.holeChance)) throw nb::value_error("lath_hole_chance must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.holeSize)) throw nb::value_error("lath_hole_size must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.holeJagged)) throw nb::value_error("lath_hole_jagged must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.holeLaths)) throw nb::value_error("lath_hole_laths must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackLengthMin)) throw nb::value_error("lath_crack_length_min must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackLengthMax)) throw nb::value_error("lath_crack_length_max must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackWidth)) throw nb::value_error("lath_crack_width must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackDepth)) throw nb::value_error("lath_crack_depth must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackMeander)) throw nb::value_error("lath_crack_meander must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackBranch)) throw nb::value_error("lath_crack_branch must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.crackInterior)) throw nb::value_error("lath_crack_interior must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathAmount)) throw nb::value_error("lath_lath_amount must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathWidth)) throw nb::value_error("lath_lath_width must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathThickness)) throw nb::value_error("lath_lath_thickness must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathGap)) throw nb::value_error("lath_lath_gap must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathScale)) throw nb::value_error("lath_lath_scale must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathAngle)) throw nb::value_error("lath_lath_angle must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathLength)) throw nb::value_error("lath_lath_length must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathEmbed)) throw nb::value_error("lath_lath_embed must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathJitter)) throw nb::value_error("lath_lath_jitter must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathTilt)) throw nb::value_error("lath_lath_tilt must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathWarp)) throw nb::value_error("lath_lath_warp must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathEdge)) throw nb::value_error("lath_lath_edge must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathMissing)) throw nb::value_error("lath_lath_missing must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathBroken)) throw nb::value_error("lath_lath_broken must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathBend)) throw nb::value_error("lath_lath_bend must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathSplinter)) throw nb::value_error("lath_lath_splinter must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.upX)) throw nb::value_error("lath_up_x must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.upY)) throw nb::value_error("lath_up_y must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.upZ)) throw nb::value_error("lath_up_z must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.rivenWave)) throw nb::value_error("lath_riven_wave must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.rivenWaveLength)) throw nb::value_error("lath_riven_wave_length must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.rivenWidthVar)) throw nb::value_error("lath_riven_width_var must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.rivenTwist)) throw nb::value_error("lath_riven_twist must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.rivenRough)) throw nb::value_error("lath_riven_rough must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.studSpacing)) throw nb::value_error("lath_stud_spacing must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.studWidth)) throw nb::value_error("lath_stud_width must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.studDepth)) throw nb::value_error("lath_stud_depth must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.lathHang)) throw nb::value_error("lath_lath_hang must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.nogginSpacing)) throw nb::value_error("lath_noggin_spacing must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.nailAmount)) throw nb::value_error("lath_nail_amount must be finite");
	if (prm.lath.enable && !std::isfinite(prm.lath.nailLength)) throw nb::value_error("lath_nail_length must be finite");
	std::vector<std::vector<EVec>> rbfCutVec;
	for (const PointsIn& g : rbfCutIn)
	{
		const size_t n = g.shape(0);
		const double* gd = g.data();
		std::vector<EVec> poly;
		for (size_t i = 0; i < n; i++)
		{
			for (int j = 0; j < 3; j++) if (!std::isfinite(gd[i * 3 + j]))
				throw std::invalid_argument("rbf strokes must be finite");
			poly.push_back(EVec(gd[i * 3], gd[i * 3 + 1], gd[i * 3 + 2]));
		}
		if (!poly.empty()) rbfCutVec.push_back(std::move(poly));
	}
	std::vector<std::vector<EVec>> cellfStrokeVec;
	for (const PointsIn& g : cellfStrokesIn)
	{
		const size_t n = g.shape(0);
		const double* gd = g.data();
		std::vector<EVec> poly;
		for (size_t i = 0; i < n; i++)
		{
			for (int j = 0; j < 3; j++) if (!std::isfinite(gd[i * 3 + j]))
				throw std::invalid_argument("cell fracture strokes must be finite");
			poly.push_back(EVec(gd[i * 3], gd[i * 3 + 1], gd[i * 3 + 2]));
		}
		if (!poly.empty()) cellfStrokeVec.push_back(std::move(poly));
	}
	if (prm.cellf.enable)
	{
		const CellFractureParams& c = prm.cellf;
		for (double v : {c.gap, c.sizeVariation, c.roughness, c.roughScale, c.detail, c.planarRatio,
			c.proxRadius, c.proxDensity, c.proxFalloff, c.rubble, c.remainderReach, c.strokeSpacing,
            c.guidanceRadius, c.guidanceDepth, c.guidanceFalloff, c.guidanceDistance,
			c.grainX, c.grainY, c.grainZ, c.grainStretch, c.splinter, c.glassRingGrowth, c.glassBranch, c.glassWander, c.glassHole, c.innerUvScale, c.smoothAngle})
			if (!std::isfinite(v)) throw std::invalid_argument("cell fracture parameters must be finite");
		for (const CellProximity& q : c.proximity)
			if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.radius) || !std::isfinite(q.strength))
				throw std::invalid_argument("cell fracture proximity must be finite");
		if (c.material < 0 || c.material > 4) throw std::invalid_argument("cell fracture material must be 0..4");
		if (c.pieces < 1 || c.maxPieces < 2 || c.maxPieces > 5000) throw std::invalid_argument("cell fracture piece counts out of range");
	}
	std::vector<std::vector<EVec>> rbfImpactVec;
	for (const PointsIn& g : rbfImpactIn)
	{
		const size_t n = g.shape(0);
		const double* gd = g.data();
		std::vector<EVec> poly;
		for (size_t i = 0; i < n; i++)
		{
			for (int j = 0; j < 3; j++) if (!std::isfinite(gd[i * 3 + j]))
				throw std::invalid_argument("rbf strokes must be finite");
			poly.push_back(EVec(gd[i * 3], gd[i * 3 + 1], gd[i * 3 + 2]));
		}
		if (!poly.empty()) rbfImpactVec.push_back(std::move(poly));
	}
	std::vector<std::vector<EVec>> guideVec;
	guideVec.reserve(guideSplinesIn.size());
	if (!guideLevelsIn.empty() && guideLevelsIn.size() != guideSplinesIn.size())
		throw std::invalid_argument("guide_levels must match guide_splines");
	for (Int32 level : guideLevelsIn) if (level < 0 || level > 2)
		throw std::invalid_argument("guide_levels must be 0, 1 or 2");
	for (const PointsIn& g : guideSplinesIn)
	{
		const size_t n = g.shape(0);
		if (n < 2)
			throw std::invalid_argument("guide_splines need at least two points");
		const double* gd = g.data();
		std::vector<EVec> poly;
		poly.reserve(n);
		for (size_t i = 0; i < n; i++)
		{
			for (int j = 0; j < 3; j++) if (!std::isfinite(gd[i * 3 + j]))
				throw std::invalid_argument("guide_splines must be finite");
			poly.push_back(EVec(gd[i * 3], gd[i * 3 + 1], gd[i * 3 + 2]));
		}
		guideVec.push_back(std::move(poly));
	}

	                                                                       
	                                                           
	                                                                                             
	                                                                    
	                           
	static_assert(sizeof(EVec) == 3 * sizeof(double), "EVec must be 3 plain doubles");

	                                                                        
	                                                                     
	                                                                                   
	                                  
	std::vector<ProjectDamagePoly> polys(nt);
	const int32_t* polyData = polysIn.data();
	for (size_t i = 0; i < nt; i++)
	{
		const int32_t a = polyData[i * 4 + 0], b = polyData[i * 4 + 1],
			c = polyData[i * 4 + 2], d = polyData[i * 4 + 3];
		if (a < 0 || a >= (int32_t)nv || b < 0 || b >= (int32_t)nv
			|| c < 0 || c >= (int32_t)nv || d < 0 || d >= (int32_t)nv)
			throw std::invalid_argument("build_worn: polygon index out of range");
		polys[i] = ProjectDamagePoly{ a, b, c, d };
	}

	std::vector<double> heatWeights;
	if (heatWeightsIn.is_valid()) {
		if (heatWeightsIn.shape(0) != nv) throw std::invalid_argument("heat_weights: one entry per input vertex required");
		heatWeights.assign(heatWeightsIn.data(), heatWeightsIn.data() + nv);
		for (double w : heatWeights) if (!std::isfinite(w)) throw std::invalid_argument("heat_weights must be finite");
	}
	std::vector<double> coatingZoneWeights;
	if (coatingZoneWeightsIn.is_valid()) {
		if (coatingZoneWeightsIn.shape(0) != nv) throw std::invalid_argument("coating_zone_weights: one entry per vertex required");
		coatingZoneWeights.assign(coatingZoneWeightsIn.data(), coatingZoneWeightsIn.data() + nv);
		for (double w : coatingZoneWeights) if (!std::isfinite(w)) throw std::invalid_argument("coating_zone_weights must be finite");
	}
	std::vector<SurfacePaintRegion> surfaceRegions;
	if (!surfacePathsIn.empty() && (!surfaceNormalsIn.is_valid()
		|| surfaceNormalsIn.shape(0) != surfacePathsIn.size()))
		throw std::invalid_argument("surface_paint_normals must match surface_paint_paths");
	for (size_t ri = 0; ri < surfacePathsIn.size(); ri++)
	{
		const auto& path = surfacePathsIn[ri];
		if (path.shape(0) < 3) throw std::invalid_argument("surface paint loops need three points");
		SurfacePaintRegion region;
		const double* nd = surfaceNormalsIn.data() + ri * 3;
		region.normal = EVec(nd[0], nd[1], nd[2]);
		const double nl = m::la::length(region.normal);
		if (!std::isfinite(nl) || nl < 1e-9) throw std::invalid_argument("invalid surface paint normal");
		region.normal = region.normal / nl;
		for (size_t i = 0; i < path.shape(0); i++)
		{
			const double* v = path.data() + i * 3;
			for (int j = 0; j < 3; j++) if (!std::isfinite(v[j]))
				throw std::invalid_argument("surface paint points must be finite");
			region.points.push_back(EVec(v[0], v[1], v[2]));
		}
		surfaceRegions.push_back(std::move(region));
	}
	std::vector<double> fabricWeights;
	if (fabricWeightsIn.is_valid())
	{
		if (fabricWeightsIn.shape(0) != nv) throw std::invalid_argument("fabric_weights must have one entry per input vertex");
		fabricWeights.assign(fabricWeightsIn.data(), fabricWeightsIn.data() + nv);
		for (double w : fabricWeights) if (!std::isfinite(w)) throw std::invalid_argument("fabric_weights must be finite");
	}
	std::vector<bool> fabricAvoid;
	if (fabricAvoidIn.is_valid())
	{
		if (fabricAvoidIn.shape(0) != nt) throw std::invalid_argument("fabric_avoid_polys must match input polygons");
		for (size_t i = 0; i < nt; i++) fabricAvoid.push_back(fabricAvoidIn.data()[i]);
	}
	std::vector<std::vector<EVec>> concreteStrokes;
	for (const PointsIn& path : concreteStrokesIn)
	{
		if (path.shape(0) < 2) throw std::invalid_argument("concrete_strokes need at least two points");
		std::vector<EVec> poly;
		for (size_t i = 0; i < path.shape(0); i++)
		{
			const double* v = path.data() + i * 3;
			for (int j = 0; j < 3; j++) if (!std::isfinite(v[j])) throw std::invalid_argument("concrete_strokes must be finite");
			poly.emplace_back(v[0], v[1], v[2]);
		}
		concreteStrokes.push_back(std::move(poly));
	}
    if (!std::isfinite(prm.noiseRoughness)) throw nb::value_error("noise_roughness must be finite");
    if (!std::isfinite(prm.noiseDistortion)) throw nb::value_error("noise_distortion must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.detail)) throw nb::value_error("bullet_detail must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.sizeMin)) throw nb::value_error("bullet_size_min must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.sizeMax)) throw nb::value_error("bullet_size_max must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.sizeBias)) throw nb::value_error("bullet_size_bias must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.penetration)) throw nb::value_error("bullet_penetration must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.penetrationVar)) throw nb::value_error("bullet_penetration_var must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.thicknessCap)) throw nb::value_error("bullet_thickness_cap must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.oblique)) throw nb::value_error("bullet_oblique must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.shotX)) throw nb::value_error("bullet_shot_x must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.shotY)) throw nb::value_error("bullet_shot_y must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.shotZ)) throw nb::value_error("bullet_shot_z must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.elongate)) throw nb::value_error("bullet_elongate must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.clusterShare)) throw nb::value_error("bullet_cluster_share must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.clusterSpread)) throw nb::value_error("bullet_cluster_spread must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.minSpacing)) throw nb::value_error("bullet_min_spacing must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.paintSpacing)) throw nb::value_error("bullet_paint_spacing must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.paintSpread)) throw nb::value_error("bullet_paint_spread must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.paintSnap)) throw nb::value_error("bullet_paint_snap must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.paintClose)) throw nb::value_error("bullet_paint_close must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.paintSingle)) throw nb::value_error("bullet_paint_single must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.haloScale)) throw nb::value_error("bullet_halo_scale must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.haloDepth)) throw nb::value_error("bullet_halo_depth must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.haloJagged)) throw nb::value_error("bullet_halo_jagged must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.pitSize)) throw nb::value_error("bullet_pit_size must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.pitSpread)) throw nb::value_error("bullet_pit_spread must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalHoleJagged)) throw nb::value_error("bullet_metal_hole_jagged must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalDish)) throw nb::value_error("bullet_metal_dish must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalDishWidth)) throw nb::value_error("bullet_metal_dish_width must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalLipHeight)) throw nb::value_error("bullet_metal_lip_height must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalLipWidth)) throw nb::value_error("bullet_metal_lip_width must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalPetalTear)) throw nb::value_error("bullet_metal_petal_tear must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalCurl)) throw nb::value_error("bullet_metal_curl must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalExitBurr)) throw nb::value_error("bullet_metal_exit_burr must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.metalDentChance)) throw nb::value_error("bullet_metal_dent_chance must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.grainX)) throw nb::value_error("bullet_grain_x must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.grainY)) throw nb::value_error("bullet_grain_y must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.grainZ)) throw nb::value_error("bullet_grain_z must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodCrush)) throw nb::value_error("bullet_wood_crush must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodSplinter)) throw nb::value_error("bullet_wood_splinter must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodFiberWidth)) throw nb::value_error("bullet_wood_fiber_width must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodExitScale)) throw nb::value_error("bullet_wood_exit_scale must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodExitDepth)) throw nb::value_error("bullet_wood_exit_depth must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodSliverLength)) throw nb::value_error("bullet_wood_sliver_length must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodSplitChance)) throw nb::value_error("bullet_wood_split_chance must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.woodSplitLength)) throw nb::value_error("bullet_wood_split_length must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concSpallChance)) throw nb::value_error("bullet_conc_spall_chance must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concSpallScale)) throw nb::value_error("bullet_conc_spall_scale must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concSpallDepth)) throw nb::value_error("bullet_conc_spall_depth must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concSkinDepth)) throw nb::value_error("bullet_conc_skin_depth must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concBoreDepth)) throw nb::value_error("bullet_conc_bore_depth must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concJagged)) throw nb::value_error("bullet_conc_jagged must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concRoughness)) throw nb::value_error("bullet_conc_roughness must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concAggregate)) throw nb::value_error("bullet_conc_aggregate must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concExitScale)) throw nb::value_error("bullet_conc_exit_scale must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.concCrackLength)) throw nb::value_error("bullet_conc_crack_length must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.crackWidth)) throw nb::value_error("bullet_crack_width must be finite");
    if (prm.bullet.enable && !std::isfinite(prm.bullet.crackDepth)) throw nb::value_error("bullet_crack_depth must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.detail)) throw nb::value_error("blast_detail must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.radiusScale)) throw nb::value_error("blast_radius_scale must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.proximity)) throw nb::value_error("blast_proximity must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.proxShrink)) throw nb::value_error("blast_prox_shrink must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.jagged)) throw nb::value_error("blast_jagged must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.noiseScale)) throw nb::value_error("blast_noise_scale must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.floorHeight)) throw nb::value_error("blast_floor_height must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.floorOffset)) throw nb::value_error("blast_floor_offset must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.slabThickness)) throw nb::value_error("blast_slab_thickness must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.wallDepth)) throw nb::value_error("blast_wall_depth must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.coreSize)) throw nb::value_error("blast_core_size must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.roomSize)) throw nb::value_error("blast_room_size must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.roomFacets)) throw nb::value_error("blast_room_facets must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.partitionSpacing)) throw nb::value_error("blast_partition_spacing must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.partitionThickness)) throw nb::value_error("blast_partition_thickness must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.slabBreak)) throw nb::value_error("blast_slab_break must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.slabDroop)) throw nb::value_error("blast_slab_droop must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.wallSize)) throw nb::value_error("blast_wall_size must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.fringe)) throw nb::value_error("blast_fringe must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.fringeDensity)) throw nb::value_error("blast_fringe_density must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.spallReach)) throw nb::value_error("blast_spall_reach must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.spallCover)) throw nb::value_error("blast_spall_cover must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.brickLength)) throw nb::value_error("blast_brick_length must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.brickHeight)) throw nb::value_error("blast_brick_height must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.jointWidth)) throw nb::value_error("blast_joint_width must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.jointDepth)) throw nb::value_error("blast_joint_depth must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.brickJitter)) throw nb::value_error("blast_brick_jitter must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.plasterDepth)) throw nb::value_error("blast_plaster_depth must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.crackLength)) throw nb::value_error("blast_crack_length must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.crackWidth)) throw nb::value_error("blast_crack_width must be finite");
    if (prm.blast.enable && !std::isfinite(prm.blast.crackDepth)) throw nb::value_error("blast_crack_depth must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.detail)) throw nb::value_error("ground_detail must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.upX)) throw nb::value_error("ground_up_x must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.upY)) throw nb::value_error("ground_up_y must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.upZ)) throw nb::value_error("ground_up_z must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.maxSlope)) throw nb::value_error("ground_max_slope must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.sizeMin)) throw nb::value_error("ground_size_min must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.sizeMax)) throw nb::value_error("ground_size_max must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.areaChance)) throw nb::value_error("ground_area_chance must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.areaSize)) throw nb::value_error("ground_area_size must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.areaStretch)) throw nb::value_error("ground_area_stretch must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.paintSnap)) throw nb::value_error("ground_paint_snap must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.paintClose)) throw nb::value_error("ground_paint_close must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.paintJitter)) throw nb::value_error("ground_paint_jitter must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.paintBranch)) throw nb::value_error("ground_paint_branch must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackLengthMin)) throw nb::value_error("ground_crack_length_min must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackLengthMax)) throw nb::value_error("ground_crack_length_max must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackWidth)) throw nb::value_error("ground_crack_width must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackWidthVar)) throw nb::value_error("ground_crack_width_var must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackTaper)) throw nb::value_error("ground_crack_taper must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackDepth)) throw nb::value_error("ground_crack_depth must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackMeander)) throw nb::value_error("ground_crack_meander must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackKink)) throw nb::value_error("ground_crack_kink must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackBranch)) throw nb::value_error("ground_crack_branch must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackJoin)) throw nb::value_error("ground_crack_join must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.crackChip)) throw nb::value_error("ground_crack_chip must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.areaJagged)) throw nb::value_error("ground_area_jagged must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.rimFade)) throw nb::value_error("ground_rim_fade must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.plateSize)) throw nb::value_error("ground_plate_size must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.plateCenterScale)) throw nb::value_error("ground_plate_center_scale must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.plateJagged)) throw nb::value_error("ground_plate_jagged must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.gapWidth)) throw nb::value_error("ground_gap_width must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.gapDepth)) throw nb::value_error("ground_gap_depth must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.plateSink)) throw nb::value_error("ground_plate_sink must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.plateTilt)) throw nb::value_error("ground_plate_tilt must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.potholeChance)) throw nb::value_error("ground_pothole_chance must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.potholeSize)) throw nb::value_error("ground_pothole_size must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.potholeLoose)) throw nb::value_error("ground_pothole_loose must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.potholeDepth)) throw nb::value_error("ground_pothole_depth must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.roughness)) throw nb::value_error("ground_roughness must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.aggregateSize)) throw nb::value_error("ground_aggregate_size must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.thicknessCap)) throw nb::value_error("ground_thickness_cap must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.rubbleSizeMin)) throw nb::value_error("ground_rubble_size_min must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.rubbleSizeMax)) throw nb::value_error("ground_rubble_size_max must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.heaveChance)) throw nb::value_error("ground_heave_chance must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.heaveHeight)) throw nb::value_error("ground_heave_height must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.heaveWidth)) throw nb::value_error("ground_heave_width must be finite");
    if (prm.ground.enable && !std::isfinite(prm.ground.heaveGap)) throw nb::value_error("ground_heave_gap must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.detail)) throw nb::value_error("brick_detail must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.sizeMin)) throw nb::value_error("brick_size_min must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.sizeMax)) throw nb::value_error("brick_size_max must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.stretch)) throw nb::value_error("brick_stretch must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.edgeBias)) throw nb::value_error("brick_edge_bias must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.paintRadius)) throw nb::value_error("brick_paint_radius must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.paintSnap)) throw nb::value_error("brick_paint_snap must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.paintClose)) throw nb::value_error("brick_paint_close must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.renderMin)) throw nb::value_error("brick_render_min must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.renderMax)) throw nb::value_error("brick_render_max must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.jagged)) throw nb::value_error("brick_jagged must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.outlineScale)) throw nb::value_error("brick_outline_scale must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crumble)) throw nb::value_error("brick_crumble must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crumbleSize)) throw nb::value_error("brick_crumble_size must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.terraceSkin)) throw nb::value_error("brick_terrace_skin must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.terraceInset)) throw nb::value_error("brick_terrace_inset must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.wallSoft)) throw nb::value_error("brick_wall_soft must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.roughness)) throw nb::value_error("brick_roughness must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.renderStick)) throw nb::value_error("brick_render_stick must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.thicknessCap)) throw nb::value_error("brick_thickness_cap must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brickLength)) throw nb::value_error("brick_brick_length must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brickHeight)) throw nb::value_error("brick_brick_height must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brickWidth)) throw nb::value_error("brick_brick_width must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.mortar)) throw nb::value_error("brick_mortar must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.mortarDepth)) throw nb::value_error("brick_mortar_depth must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brickScale)) throw nb::value_error("brick_brick_scale must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.bondOffset)) throw nb::value_error("brick_bond_offset must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.faceJitter)) throw nb::value_error("brick_face_jitter must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.faceTilt)) throw nb::value_error("brick_face_tilt must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brickRough)) throw nb::value_error("brick_brick_rough must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.chip)) throw nb::value_error("brick_chip must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.chipSize)) throw nb::value_error("brick_chip_size must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.jointRough)) throw nb::value_error("brick_joint_rough must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.breakSize)) throw nb::value_error("brick_break_size must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.layerInset)) throw nb::value_error("brick_layer_inset must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.breakJagged)) throw nb::value_error("brick_break_jagged must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.missing)) throw nb::value_error("brick_missing must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.broken)) throw nb::value_error("brick_broken must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.brokenRough)) throw nb::value_error("brick_broken_rough must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.collarFill)) throw nb::value_error("brick_collar_fill must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.loose)) throw nb::value_error("brick_loose must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.looseDepth)) throw nb::value_error("brick_loose_depth must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.looseTilt)) throw nb::value_error("brick_loose_tilt must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.looseOut)) throw nb::value_error("brick_loose_out must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.looseOutDist)) throw nb::value_error("brick_loose_out_dist must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.holeChance)) throw nb::value_error("brick_hole_chance must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.holeSize)) throw nb::value_error("brick_hole_size must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackLengthMin)) throw nb::value_error("brick_crack_length_min must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackLengthMax)) throw nb::value_error("brick_crack_length_max must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackWidth)) throw nb::value_error("brick_crack_width must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackDepth)) throw nb::value_error("brick_crack_depth must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackMeander)) throw nb::value_error("brick_crack_meander must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackBranch)) throw nb::value_error("brick_crack_branch must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.crackInterior)) throw nb::value_error("brick_crack_interior must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.upX)) throw nb::value_error("brick_up_x must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.upY)) throw nb::value_error("brick_up_y must be finite");
    if (prm.brick.enable && !std::isfinite(prm.brick.upZ)) throw nb::value_error("brick_up_z must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.thickness)) throw nb::value_error("glass_thickness must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.skinDepth)) throw nb::value_error("glass_skin_depth must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.cutMargin)) throw nb::value_error("glass_cut_margin must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.detail)) throw nb::value_error("glass_detail must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.holeChance)) throw nb::value_error("glass_hole_chance must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.holeSizeMin)) throw nb::value_error("glass_hole_size_min must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.holeSizeMax)) throw nb::value_error("glass_hole_size_max must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.holeLobes)) throw nb::value_error("glass_hole_lobes must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.impactSpacing)) throw nb::value_error("glass_impact_spacing must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.radialJitter)) throw nb::value_error("glass_radial_jitter must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.spikeChance)) throw nb::value_error("glass_spike_chance must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.spikeLength)) throw nb::value_error("glass_spike_length must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.spikeWidth)) throw nb::value_error("glass_spike_width must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.spikeSplit)) throw nb::value_error("glass_spike_split must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.spikeLean)) throw nb::value_error("glass_spike_lean must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.notchJitter)) throw nb::value_error("glass_notch_jitter must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.blowout)) throw nb::value_error("glass_blowout must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.blowoutReach)) throw nb::value_error("glass_blowout_reach must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.edgeKink)) throw nb::value_error("glass_edge_kink must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.shardCrack)) throw nb::value_error("glass_shard_crack must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackWidth)) throw nb::value_error("glass_crack_width must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackDepth)) throw nb::value_error("glass_crack_depth must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackReach)) throw nb::value_error("glass_crack_reach must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackReachJitter)) throw nb::value_error("glass_crack_reach_jitter must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackMeander)) throw nb::value_error("glass_crack_meander must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackBranch)) throw nb::value_error("glass_crack_branch must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crackTaper)) throw nb::value_error("glass_crack_taper must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.crushSize)) throw nb::value_error("glass_crush_size must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.ringFirst)) throw nb::value_error("glass_ring_first must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.ringGrowth)) throw nb::value_error("glass_ring_growth must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.ringFill)) throw nb::value_error("glass_ring_fill must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.ringJitter)) throw nb::value_error("glass_ring_jitter must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.ringBow)) throw nb::value_error("glass_ring_bow must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.chipChance)) throw nb::value_error("glass_chip_chance must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.chipSpacing)) throw nb::value_error("glass_chip_spacing must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.chipSize)) throw nb::value_error("glass_chip_size must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.chipDepth)) throw nb::value_error("glass_chip_depth must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.paintShardWidth)) throw nb::value_error("glass_paint_shard_width must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.paintPointSize)) throw nb::value_error("glass_paint_point_size must be finite");
    if (prm.glass.enable && !std::isfinite(prm.glass.paintImpactSize)) throw nb::value_error("glass_paint_impact_size must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.detail)) throw nb::value_error("fissure_detail must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.upX)) throw nb::value_error("fissure_up_x must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.upY)) throw nb::value_error("fissure_up_y must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.upZ)) throw nb::value_error("fissure_up_z must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.width)) throw nb::value_error("fissure_width must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.widthRandom)) throw nb::value_error("fissure_width_random must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.depth)) throw nb::value_error("fissure_depth must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.depthRandom)) throw nb::value_error("fissure_depth_random must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.taper)) throw nb::value_error("fissure_taper must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.jagged)) throw nb::value_error("fissure_jagged must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.edgeScale)) throw nb::value_error("fissure_edge_scale must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.wallLean)) throw nb::value_error("fissure_wall_lean must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.wallRough)) throw nb::value_error("fissure_wall_rough must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.floorShape)) throw nb::value_error("fissure_floor_shape must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.thicknessCap)) throw nb::value_error("fissure_thickness_cap must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabLengthMin)) throw nb::value_error("fissure_slab_length_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabLengthMax)) throw nb::value_error("fissure_slab_length_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabWidthMin)) throw nb::value_error("fissure_slab_width_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabWidthMax)) throw nb::value_error("fissure_slab_width_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabRowFalloff)) throw nb::value_error("fissure_slab_row_falloff must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabThickMin)) throw nb::value_error("fissure_slab_thick_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabThickMax)) throw nb::value_error("fissure_slab_thick_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.tiltMin)) throw nb::value_error("fissure_tilt_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.tiltMax)) throw nb::value_error("fissure_tilt_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.sinkChance)) throw nb::value_error("fissure_sink_chance must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.roll)) throw nb::value_error("fissure_roll must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.flatChance)) throw nb::value_error("fissure_flat_chance must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.sideBias)) throw nb::value_error("fissure_side_bias must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabGap)) throw nb::value_error("fissure_slab_gap must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabJagged)) throw nb::value_error("fissure_slab_jagged must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.slabSkew)) throw nb::value_error("fissure_slab_skew must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchDensity)) throw nb::value_error("fissure_branch_density must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchLengthMin)) throw nb::value_error("fissure_branch_length_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchLengthMax)) throw nb::value_error("fissure_branch_length_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchWidth)) throw nb::value_error("fissure_branch_width must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchDepth)) throw nb::value_error("fissure_branch_depth must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchMeander)) throw nb::value_error("fissure_branch_meander must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchAngle)) throw nb::value_error("fissure_branch_angle must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.branchFork)) throw nb::value_error("fissure_branch_fork must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.rubbleDensity)) throw nb::value_error("fissure_rubble_density must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.rubbleSizeMin)) throw nb::value_error("fissure_rubble_size_min must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.rubbleSizeMax)) throw nb::value_error("fissure_rubble_size_max must be finite");
    if (prm.fissure.enable && !std::isfinite(prm.fissure.rubbleWedged)) throw nb::value_error("fissure_rubble_wedged must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.detail)) throw nb::value_error("rusty_detail must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.sizeMin)) throw nb::value_error("rusty_size_min must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.sizeMax)) throw nb::value_error("rusty_size_max must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.sizeBias)) throw nb::value_error("rusty_size_bias must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.minSpacing)) throw nb::value_error("rusty_min_spacing must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.lobeSpread)) throw nb::value_error("rusty_lobe_spread must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.jagged)) throw nb::value_error("rusty_jagged must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.roughness)) throw nb::value_error("rusty_roughness must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.scallop)) throw nb::value_error("rusty_scallop must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.merge)) throw nb::value_error("rusty_merge must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.lace)) throw nb::value_error("rusty_lace must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.pitSize)) throw nb::value_error("rusty_pit_size must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.pitSpread)) throw nb::value_error("rusty_pit_spread must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.crackLength)) throw nb::value_error("rusty_crack_length must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.crackWidth)) throw nb::value_error("rusty_crack_width must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.crackMeander)) throw nb::value_error("rusty_crack_meander must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.crackFork)) throw nb::value_error("rusty_crack_fork must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.crackPits)) throw nb::value_error("rusty_crack_pits must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.flow)) throw nb::value_error("rusty_flow must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.stretch)) throw nb::value_error("rusty_stretch must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.downX)) throw nb::value_error("rusty_down_x must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.downY)) throw nb::value_error("rusty_down_y must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.downZ)) throw nb::value_error("rusty_down_z must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.thickness)) throw nb::value_error("rusty_thickness must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.minBridge)) throw nb::value_error("rusty_min_bridge must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintSize)) throw nb::value_error("rusty_paint_size must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintSpacing)) throw nb::value_error("rusty_paint_spacing must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintGaps)) throw nb::value_error("rusty_paint_gaps must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintSnap)) throw nb::value_error("rusty_paint_snap must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintClose)) throw nb::value_error("rusty_paint_close must be finite");
    if (prm.rusty.enable && !std::isfinite(prm.rusty.paintSingle)) throw nb::value_error("rusty_paint_single must be finite");
    for (const auto& e : prm.blast.empties) { if (!std::isfinite(e.size) || !std::isfinite(e.strength)) throw nb::value_error("blast empty must be finite"); for (double x : e.rel) if (!std::isfinite(x)) throw nb::value_error("blast transform must be finite"); }
	auto readRegions = [](const std::vector<PointsIn>& paths, const PointsIn& normals, size_t minPoints = 3) {
		std::vector<SurfacePaintRegion> regions;
		if (!paths.empty() && (!normals.is_valid() || normals.shape(0) != paths.size()))
			throw std::invalid_argument("paint normals must match contours");
		for (size_t r = 0; r < paths.size(); r++)
		{
			if (paths[r].shape(0) < minPoints) throw std::invalid_argument("paint contour needs three points");
			const double* nd = normals.data() + r * 3;
			SurfacePaintRegion region;
			region.normal = EVec(nd[0], nd[1], nd[2]);
			const double len = m::la::length(region.normal);
			if (!std::isfinite(len) || len < 1e-9) throw std::invalid_argument("invalid contour normal");
			region.normal = region.normal / len;
			for (size_t i = 0; i < paths[r].shape(0); i++)
			{
				const double* v = paths[r].data() + i * 3;
				for (int j = 0; j < 3; j++) if (!std::isfinite(v[j])) throw std::invalid_argument("paint contour must be finite");
				region.points.emplace_back(v[0], v[1], v[2]);
			}
			regions.push_back(std::move(region));
		}
		return regions;
	};
	auto coatingRegions = readRegions(coatingPathsIn, coatingNormalsIn);
	auto rustRegions = readRegions(rustPathsIn, rustNormalsIn);
	if (coatingZoneWeightsIn.is_valid() || rustWeightsIn.is_valid())
		throw std::invalid_argument("Inline Coating/Rust use Grease Pencil contours, not vertex masks");

    auto readStrokes = [](const std::vector<PointsIn>& paths, size_t minPoints = 2) {
        std::vector<std::vector<EVec>> result;
        for (const auto& path : paths) {
            if (path.shape(0) < minPoints) throw nb::value_error("stroke has too few points");
            std::vector<EVec> points;
            for (size_t i = 0; i < path.shape(0); ++i) {
                const double* v = path.data() + i * 3;
                for (int j = 0; j < 3; ++j) if (!std::isfinite(v[j])) throw nb::value_error("stroke must be finite");
                points.emplace_back(v[0], v[1], v[2]);
            }
            result.push_back(std::move(points));
        }
        return result;
    };
    auto woodStrokes = readStrokes(woodStrokesIn);
    auto lathStrokes = readStrokes(lathStrokesIn);
    auto wallpaperRegions = readRegions(wallpaperPathsIn, wallpaperNormalsIn);
    auto fabricHoles = readRegions(fabricHolesIn, fabricHoleNormalsIn);
    auto fabricEdges = readRegions(fabricEdgesIn, fabricEdgeNormalsIn);
    auto bulletStrokes = readStrokes(bulletStrokesIn, 1);
    auto groundStrokes = readStrokes(groundStrokesIn);
    auto brickStrokes = readStrokes(brickStrokesIn);
    auto bevelStrokes = readStrokes(bevelStrokesIn);
    auto damagedBevelStrokes = readStrokes(damagedBevelStrokesIn);
    auto scratchStrokes = readStrokes(scratchStrokesIn);
    if (!std::isfinite(chipPaintRadius) || chipPaintRadius < 0.0)
        throw nb::value_error("chip_paint_radius must be finite and non-negative");
    auto chipStrokes = readStrokes(chipStrokesIn);
    auto fissureStrokes = readStrokes(fissureStrokesIn);
    auto rustyStrokes = readStrokes(rustyStrokesIn, 1);
    auto glassHoles = readRegions(glassHolesIn, glassHoleNormalsIn);
    auto glassImpacts = readRegions(glassImpactsIn, glassImpactNormalsIn, 1);
    DrawMaskInput drawMask;
    if (!std::isfinite(drawMaskRadius) || drawMaskRadius <= 0.0 ||
        !std::isfinite(drawMaskSoft) || drawMaskSoft < 0.0 || drawMaskSoft > 1.0 ||
        !std::isfinite(drawMaskSize) || drawMaskSize < 0.0 || drawMaskSize > 1.0)
        throw nb::value_error("draw mask: radius must be positive and finite; softness/size must be in [0,1]");
    drawMask.paths = readStrokes(drawMaskPathsIn, 1);
    drawMask.radius = drawMaskRadius;
    drawMask.soft = drawMaskSoft;
    drawMask.invert = drawMaskInvert;
    drawMask.size = drawMaskSize;
    drawMask.heat = drawMaskHeat;
	CoreInput in;
	std::vector<double> srcNormalsVec;
	if (srcNormalsIn.is_valid()) {
		if (srcNormalsIn.shape(0) != nt) throw std::invalid_argument("src_corner_normals: expected one row per source polygon");
		srcNormalsVec.assign(srcNormalsIn.data(), srcNormalsIn.data() + nt * 12);
		for (double x : srcNormalsVec) if (!std::isfinite(x)) throw std::invalid_argument("src_corner_normals must be finite");
		in.srcCornerNormals = &srcNormalsVec;
	}
    in.drawMask = drawMask.paths.empty() ? nullptr : &drawMask;
    in.bulletPaint = bulletStrokes.empty() ? nullptr : &bulletStrokes;
    in.groundPaint = groundStrokes.empty() ? nullptr : &groundStrokes;
    in.brickPaint = brickStrokes.empty() ? nullptr : &brickStrokes;
    in.bevelPaint = bevelStrokes.empty() ? nullptr : &bevelStrokes;
    in.damagedBevelPaint = damagedBevelStrokes.empty() ? nullptr : &damagedBevelStrokes;
    in.scratchStrokes = scratchStrokes.empty() ? nullptr : &scratchStrokes;
    in.chipPaint = chipStrokes.empty() ? nullptr : &chipStrokes;
    in.chipPaintRadius = chipPaintRadius;
    in.fissurePaint = fissureStrokes.empty() ? nullptr : &fissureStrokes;
    in.rustyPaint = rustyStrokes.empty() ? nullptr : &rustyStrokes;
    in.glassHolePaint = glassHoles.empty() ? nullptr : &glassHoles;
    in.glassImpactPaint = glassImpacts.empty() ? nullptr : &glassImpacts;
	in.coatingPaint = prm.coatingZoneEnable ? &coatingRegions : nullptr;
	in.rustPaint = prm.rustUsePaint ? &rustRegions : nullptr;
    in.woodPaint = woodStrokes.empty() ? nullptr : &woodStrokes;
    in.lathPaint = lathStrokes.empty() ? nullptr : &lathStrokes;
    in.wallpaperPaint = wallpaperRegions.empty() ? nullptr : &wallpaperRegions;
    in.fabricHoles = fabricHoles.empty() ? nullptr : &fabricHoles;
    in.fabricEdges = fabricEdges.empty() ? nullptr : &fabricEdges;
	in.fabricWeights = fabricWeightsIn.is_valid() ? &fabricWeights : nullptr;
	in.fabricAvoidPoly = fabricAvoidIn.is_valid() ? &fabricAvoid : nullptr;
	in.concreteStrokes = concreteStrokes.empty() ? nullptr : &concreteStrokes;
	                                                      
	std::vector<double> rustVec;
	if (rustWeightsIn.is_valid())
	{
		if ((size_t)rustWeightsIn.shape(0) != nv)
			throw std::invalid_argument("build_worn: rust_weights must have one entry per input point");
		rustVec.assign(rustWeightsIn.data(), rustWeightsIn.data() + nv);
		in.rustWeights = &rustVec;
	}
	for (double w : rustVec) if (!std::isfinite(w)) throw std::invalid_argument("rust_weights must be finite");
	in.guideLevels = guideLevelsIn.empty() ? nullptr : &guideLevelsIn;
	in.surfacePaint = surfaceRegions.empty() ? nullptr : &surfaceRegions;
	if (coatingZoneWeightsIn.is_valid()) in.coatingZoneWeights = &coatingZoneWeights;
	if (heatWeightsIn.is_valid()) in.heatWeights = &heatWeights;
	in.view = MeshView{ reinterpret_cast<const EVec*>(pts.data()), (int32_t)nv,
		polys.data(), (int32_t)nt };
	if (haveUvsIn) in.cornerUvs = &uvsVec;

	                                                                       
	                                                                     
	                                                                         
	                     
	const double* p = pts.data();
	EVec bbMin(p[0], p[1], p[2]), bbMax = bbMin;
	for (size_t i = 0; i < nv; i++)
	{
		bbMin.x = std::min(bbMin.x, p[i * 3 + 0]); bbMax.x = std::max(bbMax.x, p[i * 3 + 0]);
		bbMin.y = std::min(bbMin.y, p[i * 3 + 1]); bbMax.y = std::max(bbMax.y, p[i * 3 + 1]);
		bbMin.z = std::min(bbMin.z, p[i * 3 + 2]); bbMax.z = std::max(bbMax.z, p[i * 3 + 2]);
	}
	in.bbMp = (bbMin + bbMax) * 0.5;
	in.bbRad = (bbMax - bbMin) * 0.5;

	in.baseId = (Int32)m::Manifold::ReserveIDs(1);
	                                                                          
	                                                                            
	                                                                         
	                                                                           
	const bool stampFaceIds = (prm.topology == PROJECT_DAMAGE_TOPO_KEEP) || prm.baseMap;
	const m::Manifold box = BuildBoxManifold(pts.data(), nv, polys, (uint32_t)in.baseId,
		stampFaceIds, haveUvsIn ? uvsVec.data() : nullptr);
	in.box = &box;
	if (haveAvoidIn)
		in.avoidPoly = &avoidVec;
	if (!guideVec.empty())
		in.guideSplines = &guideVec;
		if (!rbfCutVec.empty()) in.rbfCutStrokes = &rbfCutVec;
		if (!rbfImpactVec.empty()) in.rbfImpactStrokes = &rbfImpactVec;
		if (!cellfStrokeVec.empty()) in.cellfStrokes = &cellfStrokeVec;
	                                                                   
	                                        

	                                                                         
	                                                                         
	                                                                     
	                                                                     
	                                                                      
	                                        
	                                                                      
	                                                                       
	ClosestPointBVH maskBvh;
	MeshWeightMask meshMask;
	ClosestPointBVH plasterBvh;
	MeshWeightMask plasterMask;
	if (!prm.plaster.zoneWeights.empty()) {
		if (prm.plaster.zoneWeights.size() != nv)
			throw std::invalid_argument("plaster zone_weights must have one entry per input point");
		for (double w : prm.plaster.zoneWeights)
			if (!std::isfinite(w)) throw std::invalid_argument("plaster zone_weights must be finite");
		plasterBvh.Build(in.view.pts, polys);
		plasterMask.active_ = true; plasterMask.amount_ = 1.0;
		plasterMask.bvh_ = &plasterBvh;
		plasterMask.weights_ = &prm.plaster.zoneWeights;
		prm.plasterZone = &plasterMask;
	}
	                                                                       
	                                                                                 
	                                                                             
	                                                                     
	                                                                          
	                                                                                  
	                                                                          
	                                                                   
	const double clampedMaskAmount = Clamp01(maskAmount);
	const double clampedMaskSize = Clamp01(maskSize);
	if (haveMaskIn && (clampedMaskAmount > 0.0 || clampedMaskSize > 0.0))
	{
		maskBvh.Build(in.view.pts, polys);
		meshMask.active_ = true;
		meshMask.amount_ = clampedMaskAmount;
		meshMask.size_ = clampedMaskSize;
		meshMask.bvh_ = &maskBvh;
		meshMask.weights_ = &maskWeights;
		prm.mask = &meshMask;
	}

	ProjectDamageHost host;                                                                 
	                                                                                   
	                                                                        
	                                      
	                                                                       
	                                 
	CacheGuard guard;
	ProjectDamageDamageCache* const cachePtr = cache ? cache->get() : guard.cache;
	const UInt64 fp = cache ? cacheFingerprint : 0;
	CoreResult res;
	{
		                                                                 
		                                                                               
		                                                     
		nb::gil_scoped_release nogil;
		res = BuildWornCore(in, prm, host, cachePtr, fp);
	}

	nb::dict out = ResultToDict(res, haveUvsIn);
	                                                                          
	                            
	out["deform_amount"] = ToNumpy1D(std::move(deformAmount));
	return out;
}

}             

                                                                           
                                                                                        
                                                                           
                                                                             
           
#ifndef PROJECT_DAMAGE_CORE_VERSION
#define PROJECT_DAMAGE_CORE_VERSION "0.0.0-dev"
#endif

static nb::dict GrowRootsPy(PointsIn points, PolysIn polys, PointsIn seeds, const RootsParams& params, std::vector<PointsIn> guidesIn, bool surfaceGuide)
{
	const size_t nv = points.shape(0), np = polys.shape(0), ns = seeds.shape(0);
	if (nv == 0 || np == 0) throw std::invalid_argument("Roots need a nonempty target mesh");
	const RootsParams prm = params;                                                      
	std::vector<EVec> pts(nv), sd(ns);
	for (size_t i = 0; i < nv; ++i) pts[i] = EVec(points(i, 0), points(i, 1), points(i, 2));
	for (size_t i = 0; i < ns; ++i) sd[i] = EVec(seeds(i, 0), seeds(i, 1), seeds(i, 2));
	std::vector<ProjectDamagePoly> pl(np);
	for (size_t i = 0; i < np; ++i) pl[i] = { polys(i, 0), polys(i, 1), polys(i, 2), polys(i, 3) };
	for (const auto& f : pl) for (Int32 i : {f.a,f.b,f.c,f.d})
		if (i < 0 || (size_t)i >= nv) throw std::invalid_argument("Roots polygon index out of range");
	auto valid = [](const EVec& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
	for (const auto& p : pts) if (!valid(p)) throw std::invalid_argument("Roots target points must be finite");
	for (const auto& p : sd) if (!valid(p)) throw std::invalid_argument("Roots seeds must be finite");
	for (double v : {prm.size,prm.rootLength,prm.lengthRandom,prm.primaryWeight,prm.randomWeight,prm.gravityWeight,
		 prm.spreadWeight,prm.branchingProbability,prm.branchAngle,prm.childLength,prm.rootSize,prm.taper,prm.tipRadius,
		 prm.childRadius,prm.embed,prm.flare,prm.flatten,prm.uvScale,prm.upX,prm.upY,prm.upZ,prm.guideWeight,prm.guideRadius})
		if (!std::isfinite(v)) throw std::invalid_argument("Roots parameters must be finite");
	if (prm.size <= 0.0 || prm.maxNodes < 1 || prm.maxNodes > 200000 || prm.maxSteps < 1 || prm.maxSteps > 20000 ||
		prm.sides < 3 || prm.sides > 16 || prm.rootsPerSeed < 1 || prm.rootsPerSeed > 32 || prm.maxLevels < 0 || prm.maxLevels > 5)
		throw std::invalid_argument("Roots geometry budget out of supported range");
	std::vector<std::vector<EVec>> guides;
	for (const PointsIn& g : guidesIn)
	{
		std::vector<EVec> s(g.shape(0));
		for (size_t i = 0; i < s.size(); ++i) s[i] = EVec(g(i, 0), g(i, 1), g(i, 2));
		for (const auto& p : s) if (!valid(p)) throw std::invalid_argument("Roots guides must be finite");
		if (s.size() >= 2) guides.push_back(std::move(s));
	}
	RootsResult r;
	{
		nb::gil_scoped_release release;
        MeshView view{pts.data(),int32_t(nv),pl.data(),int32_t(np)};
        SurfaceGuidance guide; if(surfaceGuide)guide.Bind(view);
		GrowRoots(view, sd, prm, r, &guides);
	}
	std::vector<double> rp, ruv;
	std::vector<int32_t> rf;
	for (const EVec& q : r.points) rp.insert(rp.end(), { q.x, q.y, q.z });
	for (const auto& u : r.uvVert) ruv.insert(ruv.end(), { u[0], u[1] });
	for (const auto& f : r.polys) rf.insert(rf.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(rp), 3);
	d["uvs"] = ToNumpy2D(std::move(ruv), 2);
	d["polys"] = ToNumpy2D(std::move(rf), 4);
	d["roots"] = r.roots;
	d["nodes"] = r.nodes;
	return d;
}

                                                                             
                                                         
using PaperStrokeIn = nb::ndarray<double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
static nb::dict BuildPaperPyInternal(const PaperGenParams& params, std::vector<PaperStrokeIn> tears, std::vector<int32_t> tearsCyclic,
	std::vector<PaperStrokeIn> holes, std::vector<int32_t> holesCyclic, const PaperGenSurface* surface)
{
	const PaperGenParams prm = params; // No Python-owned parameter reads after GIL release.
	for (double v : { prm.width, prm.height, prm.resolution, prm.thickness, prm.edgeRough, prm.crumpleStrength, prm.crumpleSize,
		 prm.crumpleDetail, prm.crumpleSharpness, prm.waviness, prm.waveSize, prm.foldDepth, prm.ragSize, prm.jagged, prm.fiber,
		 prm.roundness, prm.tearGap, prm.minPiece, prm.edgeCurl, prm.curlWidth, prm.edgeDistMax, prm.holeSizeMin, prm.holeSizeMax,
		 prm.holeIrregular, prm.holeElongation, prm.holeMargin, prm.cornerSizeMin, prm.cornerSizeMax, prm.biteSizeMin,
		 prm.biteSizeMax, prm.biteDepth, prm.strokeHoleWidth, prm.tearSnap, prm.closeShare, prm.tornWidth, prm.tornScale,
		 prm.tongues, prm.tongueStretch, prm.tongueAngle, prm.tongueAngleRandom, prm.tornDetail, prm.flakes, prm.peelWidth })
		if (!std::isfinite(v)) throw std::invalid_argument("Paper parameters must be finite");
	if (prm.width <= 0.0 || prm.height <= 0.0 || prm.resolution <= 0.0 || prm.thickness < 0.0 || prm.crumpleSize <= 0.0 ||
		prm.ragSize <= 0.0 || prm.tornWidth < 0.0 || prm.tornScale <= 0.0 || prm.peelWidth < 0.0 || prm.holeAmount < 0 || prm.holeAmount > 4096 || prm.cornerTears < 0 || prm.edgeBites < 0 ||
		prm.edgeBites > 256 || prm.foldsX < 0 || prm.foldsX > 32 || prm.foldsY < 0 || prm.foldsY > 32 ||
		prm.maxVerts < 1000 || prm.maxVerts > 50000000 || prm.width / prm.resolution > 1e5 || prm.height / prm.resolution > 1e5)
		throw std::invalid_argument("Paper parameters out of supported range");
	PaperGenPaint paint;
	auto read = [](const std::vector<PaperStrokeIn>& in, const std::vector<int32_t>& cyc, std::vector<PaperGenStroke>& outS)
	{
		for (size_t i = 0; i < in.size(); ++i)
		{
			PaperGenStroke s;
			for (size_t j = 0; j < in[i].shape(0); ++j)
			{
				const double x = in[i](j, 0), y = in[i](j, 1), z = in[i](j, 2);
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) throw std::invalid_argument("Paper strokes must be finite");
				s.pts.push_back(EVec(x, y, z));
			}
			s.cyclic = i < cyc.size() && cyc[i] != 0;
			if (s.pts.size() >= 2) outS.push_back(std::move(s));
		}
	};
	read(tears, tearsCyclic, paint.tears);
	read(holes, holesCyclic, paint.holes);
	PaperGenResult r;
	bool ok;
	{
		nb::gil_scoped_release release;
		ok = surface != nullptr ? ApplyPaper(*surface, prm, paint, r) : BuildPaper(prm, paint, r);
	}
	std::vector<double> rp, ruv, ed, td, cr, pe;
	for (const EVec& q : r.points) rp.insert(rp.end(), { q.x, q.y, q.z });
	for (const auto& u : r.loopUv) ruv.insert(ruv.end(), { u[0], u[1] });
	ed.assign(r.edgeDist.begin(), r.edgeDist.end());
	td.assign(r.tearDist.begin(), r.tearDist.end());
	cr.assign(r.crease.begin(), r.crease.end());
	pe.assign(r.peel.begin(), r.peel.end());
	nb::dict d;
	d["ok"] = ok;
	d["points"] = ToNumpy2D(std::move(rp), 3);
	d["loop_uvs"] = ToNumpy2D(std::move(ruv), 2);
	d["face_verts"] = ToNumpy2D(std::move(r.faceVerts), 1);
	d["face_sizes"] = ToNumpy2D(std::move(r.faceSizes), 1);
	d["face_mat"] = ToNumpy2D(std::move(r.faceMat), 1);
	d["edge_dist"] = ToNumpy2D(std::move(ed), 1);
	d["tear_dist"] = ToNumpy2D(std::move(td), 1);
	d["crease"] = ToNumpy2D(std::move(cr), 1);
	d["peel"] = ToNumpy2D(std::move(pe), 1);
	d["pieces"] = r.pieces;
	d["holes"] = r.holes;
	d["tears"] = r.tears;
	d["loops"] = r.loops;
	d["resolution"] = r.resolution;
	d["source_faces"] = ToNumpy2D(std::move(r.sourceFace), 1);
	std::vector<double> bary;
	for (const auto& weights : r.sourceBary) bary.insert(bary.end(), weights.begin(), weights.end());
	d["source_bary"] = ToNumpy2D(std::move(bary), 3);
	return d;
}

static nb::dict BuildPaperPy(const PaperGenParams& params, std::vector<PaperStrokeIn> tears, std::vector<int32_t> tearsCyclic,
	std::vector<PaperStrokeIn> holes, std::vector<int32_t> holesCyclic)
{
	return BuildPaperPyInternal(params, tears, tearsCyclic, holes, holesCyclic, nullptr);
}

using PaperTrisIn = nb::ndarray<int32_t, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
static nb::dict ApplyPaperPy(PointsIn points, PaperTrisIn triangles, const PaperGenParams& params,
	std::vector<PaperStrokeIn> tears, std::vector<int32_t> tearsCyclic,
	std::vector<PaperStrokeIn> holes, std::vector<int32_t> holesCyclic)
{
	PaperGenSurface surface;
	for (size_t i = 0; i < points.shape(0); ++i)
	{
		const EVec q(points(i, 0), points(i, 1), points(i, 2));
		if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z))
			throw std::invalid_argument("Paper mesh positions must be finite");
		surface.points.push_back(q);
	}
	for (size_t i = 0; i < triangles.shape(0); ++i)
	{
		std::array<int32_t, 3> triangle;
		for (int k = 0; k < 3; ++k)
		{
			triangle[k] = triangles(i, k);
			if (triangle[k] < 0 || size_t(triangle[k]) >= surface.points.size())
				throw std::invalid_argument("Paper mesh triangle index outside input mesh");
		}
		surface.triangles.push_back(triangle);
	}
	if (surface.points.empty() || surface.triangles.empty()) throw std::invalid_argument("Paper needs a mesh with faces");
	return BuildPaperPyInternal(params, tears, tearsCyclic, holes, holesCyclic, &surface);
}

using CableCurveIn = nb::ndarray<double, nb::shape<-1, 6>, nb::c_contig, nb::device::cpu>;
using CableTrisIn = nb::ndarray<int32_t, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
using CableZonesIn = nb::ndarray<double, nb::shape<-1, 4>, nb::c_contig, nb::device::cpu>;
static nb::dict BuildCableCurvesPy(std::vector<CableCurveIn> curvesIn, std::vector<int32_t> cyclic, const CableGenParams& params,
	PointsIn spawns, PointsIn colliderPoints, CableTrisIn colliderTris, CableZonesIn zones)
{
	const CableGenParams prm = params;                                                      
	for (double v : {prm.diameter,prm.radius,prm.gap,prm.twist,prm.lengthMin,prm.lengthMax,prm.noise,prm.noiseLength,prm.sag,
		 prm.sagRandom,prm.spread,prm.diameterRandom,prm.fanOut,prm.coils,prm.coilRadius,prm.coilTurns,prm.dangle,prm.dangleLength,prm.gravity,prm.metre,prm.slack,prm.stiffness,prm.friction,prm.floorHeight,prm.strip,prm.splay,
		 prm.fray,prm.damage,prm.damageSize,prm.strands,prm.strandLength,prm.tieSpacing,prm.tieWidth,prm.segment,prm.uvScale,
		 prm.downX,prm.downY,prm.downZ})
		if (!std::isfinite(v)) throw std::invalid_argument("Cable parameters must be finite");
	if (prm.diameter <= 0.0 || prm.segment <= 0.0 || prm.metre <= 0.0 || prm.count < 1 || prm.count > 256 || prm.clumps < 1 ||
		prm.clumps > 64 || prm.sides < 4 || prm.sides > 48 || prm.type < 0 || prm.type > 4 || prm.arrangement < 0 ||
		prm.arrangement > 2 || prm.endStart < 0 || prm.endStart > 3 || prm.endEnd < 0 || prm.endEnd > 3 || prm.pin < 0 ||
		prm.pin > 3 || prm.steps < 1 || prm.steps > 5000 || prm.maxVerts < 1000 || prm.maxVerts > 50000000 ||
		prm.lengthMax > 1e6 || prm.damage > 1e5 || prm.coilTurns > 50.0 || prm.coilRadius > 1e4 || prm.dangleLength > 1e5)
		throw std::invalid_argument("Cable parameters out of supported range");
	std::vector<CableGenCurve> curves;
	for (size_t i = 0; i < curvesIn.size(); ++i)
	{
		const CableCurveIn& a = curvesIn[i];
		CableGenCurve c;
		const size_t n = a.shape(0);
		for (size_t j = 0; j < n; ++j)
		{
			for (int k = 0; k < 6; ++k)
				if (!std::isfinite(a(j, k))) throw std::invalid_argument("Cable curve samples must be finite");
			c.pts.push_back(EVec(a(j, 0), a(j, 1), a(j, 2)));
			c.tilt.push_back(a(j, 3));
			c.radius.push_back(a(j, 4));
			c.anchor.push_back(a(j, 5) > 0.5 ? 1 : 0);
		}
		c.cyclic = i < cyclic.size() && cyclic[i] != 0;
		if (c.pts.size() >= 2) curves.push_back(std::move(c));
	}
	CableGenScene scene;
	auto finite = [](double x, double y, double z) { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); };
	for (size_t i = 0; i < spawns.shape(0); ++i)
	{
		if (!finite(spawns(i, 0), spawns(i, 1), spawns(i, 2))) throw std::invalid_argument("Cable spawn points must be finite");
		scene.spawns.push_back(EVec(spawns(i, 0), spawns(i, 1), spawns(i, 2)));
	}
	const size_t ncp = colliderPoints.shape(0);
	for (size_t i = 0; i < ncp; ++i)
	{
		if (!finite(colliderPoints(i, 0), colliderPoints(i, 1), colliderPoints(i, 2))) throw std::invalid_argument("Collider points must be finite");
		scene.colliderPts.push_back(EVec(colliderPoints(i, 0), colliderPoints(i, 1), colliderPoints(i, 2)));
	}
	for (size_t i = 0; i < colliderTris.shape(0); ++i)
	{
		std::array<int32_t, 3> t{ colliderTris(i, 0), colliderTris(i, 1), colliderTris(i, 2) };
		for (int32_t v : t) if (v < 0 || (size_t)v >= ncp) throw std::invalid_argument("Collider triangle index out of range");
		scene.colliderTris.push_back(t);
	}
	for (size_t i = 0; i < zones.shape(0); ++i)
	{
		if (!finite(zones(i, 0), zones(i, 1), zones(i, 2)) || !std::isfinite(zones(i, 3))) throw std::invalid_argument("Damage zones must be finite");
		scene.zones.push_back({ zones(i, 0), zones(i, 1), zones(i, 2), zones(i, 3) });
	}
	CableGenResult r;
	{
		nb::gil_scoped_release release;
		BuildCableCurves(curves, scene, prm, r);
	}
	std::vector<double> rp, ruv;
	for (const EVec& q : r.points) rp.insert(rp.end(), { q.x, q.y, q.z });
	for (const auto& u : r.loopUv) ruv.insert(ruv.end(), { u[0], u[1] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(rp), 3);
	d["loop_uvs"] = ToNumpy2D(std::move(ruv), 2);
	d["face_verts"] = ToNumpy2D(std::move(r.faceVerts), 1);
	d["face_sizes"] = ToNumpy2D(std::move(r.faceSizes), 1);
	d["face_mat"] = ToNumpy2D(std::move(r.faceMat), 1);
	d["face_cable"] = ToNumpy2D(std::move(r.faceCable), 1);
	d["cables"] = r.cables;
	d["ties"] = r.ties;
	d["clips"] = r.clips;
	d["holes"] = r.holes;
	d["connectors"] = r.connectors;
	d["particles"] = r.particles;
	d["sim_steps"] = r.simSteps;
	return d;
}


static nb::dict ClothSheetsPy(PointsIn points, std::vector<PointsIn> curvesIn, PointsIn colliderTris,
	const pd_cloth::ClothParams& params)
{
	pd_cloth::ClothInput in;
	for (size_t i = 0; i < points.shape(0); ++i) in.points.push_back({ points(i, 0), points(i, 1), points(i, 2) });
	for (const PointsIn& c : curvesIn)
	{
		std::vector<pd_cloth::V3> line;
		for (size_t i = 0; i < c.shape(0); ++i) line.push_back({ c(i, 0), c(i, 1), c(i, 2) });
		if (line.size() >= 2) in.curves.push_back(std::move(line));
	}
	for (size_t i = 0; i < colliderTris.shape(0); ++i) in.colliderTris.push_back({ colliderTris(i, 0), colliderTris(i, 1), colliderTris(i, 2) });
	pd_cloth::ClothResult r;
	{
		nb::gil_scoped_release release;
		pd_cloth::BuildClothSheets(in, params, r);
	}
	std::vector<double> pp, uv(r.uvs);
	std::vector<int32_t> ff;
	std::vector<float> sheet(r.sheet), edge(r.edge);
	for (const auto& q : r.points) pp.insert(pp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.polys) ff.insert(ff.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pp), 3);
	d["polys"] = ToNumpy2D(std::move(ff), 4);
	d["uvs"] = ToNumpy2D(std::move(uv), 2);
	d["sheet"] = ToNumpy1D(std::move(sheet));
	d["edge"] = ToNumpy1D(std::move(edge));
	d["sheets"] = r.sheets;
	return d;
}

static void RegisterClothSheets(nb::module_& mod)
{
	using P = pd_cloth::ClothParams;
	nb::class_<P>(mod, "ClothParams").def(nb::init<>())
		.def_rw("seed", &P::seed).def_rw("mode", &P::mode).def_rw("type", &P::type).def_rw("amount", &P::amount)
		.def_rw("scale", &P::scale).def_rw("radius", &P::radius).def_rw("thickness", &P::thickness)
		.def_rw("gravity", &P::gravity).def_rw("stiffness", &P::stiffness).def_rw("noisiness", &P::noisiness)
		.def_rw("clumps", &P::clumps).def_rw("clump_tightness", &P::clumpTightness).def_rw("cell_size", &P::cellSize)
		.def_rw("steps", &P::steps).def_rw("friction", &P::friction).def_rw("curve_radius", &P::curveRadius)
		.def_rw("pin_spacing", &P::pinSpacing).def_rw("ground_fallback", &P::groundFallback)
		.def_rw("damaged", &P::damaged).def_rw("tear", &P::tear).def_rw("holes", &P::holes)
		.def_rw("up_x", &P::upX).def_rw("up_y", &P::upY).def_rw("up_z", &P::upZ)
		.def_rw("wind", &P::wind).def_rw("wind_x", &P::windX).def_rw("wind_y", &P::windY).def_rw("wind_z", &P::windZ)
		.def_rw("turbulence", &P::turbulence)
		.def_rw("self_collision", &P::selfCollision).def_rw("self_distance", &P::selfDistance);
	mod.def("cloth_sheets", &ClothSheetsPy, nb::arg("points"), nb::arg("curves"), nb::arg("collider_tris"), nb::arg("params"),
		"Torn cloth sheets hanging from / draped over curves or crumpled round points, settled on the colliders.");
}

static nb::dict GrowIvyPy(PointsIn points, PolysIn polys, PointsIn seeds, const IvyParams& params, std::vector<PointsIn> guidesIn, std::vector<PointsIn> curvesIn, bool surfaceGuide)
{
	const size_t nv = points.shape(0), np = polys.shape(0), ns = seeds.shape(0);
    if (nv == 0 || np == 0) throw std::invalid_argument("Ivy needs a nonempty target mesh");
    const IvyParams prm = params;                                                      
	std::vector<EVec> pts(nv), sd(ns);
	for (size_t i = 0; i < nv; ++i) pts[i] = EVec(points(i, 0), points(i, 1), points(i, 2));
	for (size_t i = 0; i < ns; ++i) sd[i] = EVec(seeds(i, 0), seeds(i, 1), seeds(i, 2));
	std::vector<ProjectDamagePoly> pl(np);
	for (size_t i = 0; i < np; ++i) pl[i] = { polys(i, 0), polys(i, 1), polys(i, 2), polys(i, 3) };
    for (const auto& f : pl) for (Int32 i : {f.a,f.b,f.c,f.d})
        if (i < 0 || (size_t)i >= nv) throw std::invalid_argument("Ivy polygon index out of range");
    auto valid = [](const EVec& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    for (const auto& p : pts) if (!valid(p)) throw std::invalid_argument("Ivy target points must be finite");
    for (const auto& p : sd) if (!valid(p)) throw std::invalid_argument("Ivy seeds must be finite");
    for (double v : {prm.size,prm.primaryWeight,prm.randomWeight,prm.gravityWeight,
         prm.adhesionWeight,prm.branchingProbability,prm.maxFloatLength,prm.maxAdhesionDistance,
         prm.upX,prm.upY,prm.upZ,prm.branchSize,prm.uvScale,prm.leafSize,
         prm.leafDensity,prm.leafOffset,prm.guideWeight,prm.guideRadius,prm.leafMaturity,
         prm.stemLength,prm.stemThickness,prm.stemAttach,prm.hangDensity,prm.hangLength,prm.leafTilt})
        if (!std::isfinite(v)) throw std::invalid_argument("Ivy parameters must be finite");
    if (prm.maxNodes < 1 || prm.maxNodes > 50000 || prm.maxSteps < 1 || prm.maxSteps > 10000 ||
        prm.branchSides < 3 || prm.branchSides > 16 || prm.leafDensityMultiplier < 1 || prm.leafDensityMultiplier > 10 ||
        prm.adhesionSmoothing < 0 || prm.adhesionSmoothing > 5 || prm.leafUvDirection < 0 || prm.leafUvDirection > 3)
        throw std::invalid_argument("Ivy geometry budget out of supported range");
	std::vector<std::vector<EVec>> guides;
	for (const PointsIn& g : guidesIn)
	{
		std::vector<EVec> s(g.shape(0));
		for (size_t i = 0; i < s.size(); ++i) s[i] = EVec(g(i, 0), g(i, 1), g(i, 2));
        for (const auto& p : s) if (!valid(p)) throw std::invalid_argument("Ivy guides must be finite");
		if (s.size() >= 2) guides.push_back(std::move(s));
	}
	std::vector<std::vector<EVec>> curves;
	for (const PointsIn& c : curvesIn)
	{
		std::vector<EVec> s(c.shape(0));
		for (size_t i = 0; i < s.size(); ++i) s[i] = EVec(c(i, 0), c(i, 1), c(i, 2));
		for (const auto& p : s) if (!valid(p)) throw std::invalid_argument("Ivy curves must be finite");
		if (s.size() >= 2) curves.push_back(std::move(s));
	}
	IvyResult r;
	{
		nb::gil_scoped_release release;
        MeshView view{pts.data(),int32_t(nv),pl.data(),int32_t(np)};
        SurfaceGuidance guide; if(surfaceGuide)guide.Bind(view);
		GrowIvy(view, sd, prm, r, &guides, &curves);
	}
	std::vector<double> bp, buv, lp;
	std::vector<int32_t> bf, lf;
	for (const EVec& q : r.branchPoints) bp.insert(bp.end(), { q.x, q.y, q.z });
	for (const auto& u : r.branchUvVert) buv.insert(buv.end(), { u[0], u[1] });
	for (const auto& f : r.branchPolys) bf.insert(bf.end(), { f[0], f[1], f[2], f[3] });
	for (const EVec& q : r.leafPoints) lp.insert(lp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.leafPolys) lf.insert(lf.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["branch_points"] = ToNumpy2D(std::move(bp), 3);
	d["branch_uvs"] = ToNumpy2D(std::move(buv), 2);
	d["branch_polys"] = ToNumpy2D(std::move(bf), 4);
	d["leaf_points"] = ToNumpy2D(std::move(lp), 3);
	d["leaf_polys"] = ToNumpy2D(std::move(lf), 4);
	d["leaf_uvs"] = ToNumpy3D(std::move(r.leafUvs), 4, 2);
	d["roots"] = r.roots;
	d["nodes"] = r.nodes;
	return d;
}

                                            
static nb::dict BuildWeldsPy(PointsIn points, PolysIn polys, nb::ndarray<float, nb::ndim<1>, nb::c_contig, nb::device::cpu> weightsIn,
	std::vector<PointsIn> strokesIn, const WeldParams& params, bool surfaceGuide)
{
	const size_t nv = points.shape(0), np = polys.shape(0);
	std::vector<EVec> pts(nv);
	for (size_t i = 0; i < nv; ++i) pts[i] = EVec(points(i, 0), points(i, 1), points(i, 2));
	std::vector<ProjectDamagePoly> pl(np);
	for (size_t i = 0; i < np; ++i) pl[i] = { polys(i, 0), polys(i, 1), polys(i, 2), polys(i, 3) };
	std::vector<float> wts(weightsIn.data(), weightsIn.data() + weightsIn.shape(0));
	std::vector<std::vector<EVec>> strokes;
	for (const PointsIn& g : strokesIn)
	{
		std::vector<EVec> s(g.shape(0));
		for (size_t i = 0; i < s.size(); ++i) s[i] = EVec(g(i, 0), g(i, 1), g(i, 2));
		if (s.size() >= 2) strokes.push_back(std::move(s));
	}
	WeldResult r;
	{
		nb::gil_scoped_release release;
        MeshView view{pts.data(),int32_t(nv),pl.data(),int32_t(np)};
        SurfaceGuidance guide; if(surfaceGuide)guide.Bind(view);
		BuildWelds(view, wts, strokes, params, r);
	}
	std::vector<double> pp, uv(r.uvs);
	std::vector<int32_t> ff;
	std::vector<float> prof(r.profile);
	for (const EVec& q : r.points) pp.insert(pp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.polys) ff.insert(ff.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pp), 3);
	d["polys"] = ToNumpy2D(std::move(ff), 4);
	d["uvs"] = ToNumpy2D(std::move(uv), 2);
	d["profile"] = ToNumpy1D(std::move(prof));
	d["beads"] = r.beads;
	return d;
}


                                                                                    
static nb::dict ScatterGrassPy(PointsIn points, PolysIn polys, std::vector<PointsIn> strokesIn, const GrassParams& params, bool surfaceGuide)
{
	const size_t nv = points.shape(0), np = polys.shape(0);
	std::vector<EVec> pts(nv);
	for (size_t i = 0; i < nv; ++i) pts[i] = EVec(points(i, 0), points(i, 1), points(i, 2));
	std::vector<ProjectDamagePoly> pl(np);
	for (size_t i = 0; i < np; ++i) pl[i] = { polys(i, 0), polys(i, 1), polys(i, 2), polys(i, 3) };
	std::vector<std::vector<EVec>> strokes;
	for (const PointsIn& g : strokesIn)
	{
		std::vector<EVec> s(g.shape(0));
		for (size_t i = 0; i < s.size(); ++i) s[i] = EVec(g(i, 0), g(i, 1), g(i, 2));
		if (s.size() >= 2) strokes.push_back(std::move(s));
	}
	GrassResult r;
	{
		nb::gil_scoped_release release;
        MeshView view{pts.data(),int32_t(nv),pl.data(),int32_t(np)};
        SurfaceGuidance guide; if(surfaceGuide)guide.Bind(view);
		ScatterGrass(view, strokes, params, r);
	}
	std::vector<double> pp, uv(r.uvs);
	std::vector<int32_t> ff;
	std::vector<float> var(r.variation), tip(r.tip);
	for (const EVec& q : r.points) pp.insert(pp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.polys) ff.insert(ff.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pp), 3);
	d["polys"] = ToNumpy2D(std::move(ff), 4);
	d["uvs"] = ToNumpy2D(std::move(uv), 2);
	d["variation"] = ToNumpy1D(std::move(var));
	d["tip"] = ToNumpy1D(std::move(tip));
	d["shards"] = r.shards;
	return d;
}

static nb::dict SprayFoamPy(PointsIn points, PolysIn polys, const FoamParams& params, bool surfaceGuide)
{
	const size_t nv = points.shape(0), np = polys.shape(0);
	std::vector<EVec> pts(nv);
	for (size_t i = 0; i < nv; ++i) pts[i] = EVec(points(i, 0), points(i, 1), points(i, 2));
	std::vector<ProjectDamagePoly> pl(np);
	for (size_t i = 0; i < np; ++i) pl[i] = { polys(i, 0), polys(i, 1), polys(i, 2), polys(i, 3) };
	FoamResult r;
	{
		nb::gil_scoped_release release;
        MeshView view{pts.data(),int32_t(nv),pl.data(),int32_t(np)};
        SurfaceGuidance guide; if(surfaceGuide)guide.Bind(view);
		SprayFoam(view, params, r);
	}
	std::vector<double> pp, uv(r.uvs);
	std::vector<int32_t> ff;
	std::vector<float> var(r.variation);
	std::vector<float> cut(r.cut), cav(r.cavity);
	for (const EVec& q : r.points) pp.insert(pp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.polys) ff.insert(ff.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pp), 3);
	d["polys"] = ToNumpy2D(std::move(ff), 4);
	d["uvs"] = ToNumpy2D(std::move(uv), 2);
	d["variation"] = ToNumpy1D(std::move(var));
	d["cut"] = ToNumpy1D(std::move(cut));
	d["cavity"] = ToNumpy1D(std::move(cav));
	d["blobs"] = r.blobs;
	return d;
}
static nb::dict FoamPathPy(PointsIn centres, PointsIn normals, PointsIn tangents, const FoamParams& params)
{
	auto rows = [](const PointsIn& a) {
		std::vector<EVec> v(a.shape(0));
		for (size_t i = 0; i < v.size(); ++i) v[i] = EVec(a(i, 0), a(i, 1), a(i, 2));
		return v;
	};
	const std::vector<EVec> c = rows(centres), n = rows(normals), t = rows(tangents);
	FoamResult r;
	{
		nb::gil_scoped_release release;
		FoamAlongPath(c, n, t, params, r);
	}
	std::vector<double> pp, uv(r.uvs);
	std::vector<int32_t> ff;
	for (const EVec& q : r.points) pp.insert(pp.end(), { q.x, q.y, q.z });
	for (const auto& f : r.polys) ff.insert(ff.end(), { f[0], f[1], f[2], f[3] });
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pp), 3);
	d["polys"] = ToNumpy2D(std::move(ff), 4);
	d["uvs"] = ToNumpy2D(std::move(uv), 2);
	d["variation"] = ToNumpy1D(std::vector<float>(r.variation));
	d["cut"] = ToNumpy1D(std::vector<float>(r.cut));
	d["cavity"] = ToNumpy1D(std::vector<float>(r.cavity));
	d["blobs"] = r.blobs;
	return d;
}


static nb::dict SurfaceGuideFieldPy(PointsIn points, PolysIn polys)
{
    const size_t nv=points.shape(0), nf=polys.shape(0);
    std::vector<EVec> pts(nv); std::vector<ProjectDamagePoly> faces(nf);
    for(size_t i=0;i<nv;++i) {
        pts[i]=EVec(points(i,0),points(i,1),points(i,2));
        for(int k=0;k<3;++k) if(!std::isfinite(points(i,k))) throw std::invalid_argument("Guide points must be finite");
    }
    for(size_t i=0;i<nf;++i) {
        faces[i]={polys(i,0),polys(i,1),polys(i,2),polys(i,3)};
        for(int k=0;k<4;++k) if(polys(i,k)<0 || size_t(polys(i,k))>=nv) throw std::invalid_argument("Guide polygon index out of range");
    }
    SurfaceGuidance guide;
    { nb::gil_scoped_release release;
      MeshView view{pts.data(),int32_t(nv),faces.data(),int32_t(nf)}; guide.Bind(view); }
    auto array=[](const std::vector<EVec>& values) {
        std::vector<double> flat;flat.reserve(values.size()*3);
        for(const auto& n:values)flat.insert(flat.end(),{n.x,n.y,n.z});
        return ToNumpy2D(std::move(flat),3);
    };
    nb::dict out;
    out["points"]=array(guide.points);out["face_normals"]=array(guide.normals);out["vertex_normals"]=array(guide.vertexNormals);
    return out;
}

#include <tbb/global_control.h>
#include <cstdlib>
// Damage generators: shared binding helpers (paste ONCE into project_damage_core_py.cpp,
// after ToNumpy2D; needs #include "damage_gen_common.h"). Marker: PDGEN_BINDING_COMMON
// PDGEN_BINDING_COMMON
#include <type_traits>
// Settings arrive as a Python dict {name: value}; VisitXxxFields maps the names
// onto the plain-data params struct. Unknown keys raise, so a typo in the
// add-on fails loudly instead of being ignored.
template <class Params, class Visit>
static Params PdGenParamsFromDict(const nb::dict& d, Visit visit, const char* what)
{
	Params p;
	for (auto item : d)
	{
		const std::string key = nb::cast<std::string>(item.first);
		bool found = false;
		visit(p, [&](const char* name, auto& field)
		{
			if (found || key != name) return;
			found = true;
			using T = std::decay_t<decltype(field)>;
			if constexpr (std::is_same_v<T, double>)
			{
				const double v = nb::cast<double>(item.second);
				if (!std::isfinite(v)) throw std::invalid_argument(std::string(what) + ": '" + key + "' must be finite");
				field = v;
			}
			else field = T(nb::cast<int64_t>(item.second));
		});
		if (!found) throw std::invalid_argument(std::string(what) + ": unknown setting '" + key + "'");
	}
	return p;
}

template <class Params, class Visit>
static nb::dict PdGenParamsToDict(Visit visit)
{
	Params p;
	nb::dict d;
	visit(p, [&](const char* name, auto& field) { d[name] = field; });
	return d;
}

// The shared result layout: points (N,3), face_verts, face_sizes, face_mat,
// face_part (-1 when unused), loop_uvs (one per corner), attrs {name: (N,) float32}.
// упаковываем готовый меш для возврата в Blender
static nb::dict PdGenMeshToDict(pdgen::GenMesh&& m)
{
	m.FinishAttrs();
	std::vector<double> pts, uv;
	pts.reserve(m.points.size() * 3);
	for (const auto& q : m.points) pts.insert(pts.end(), { q.x, q.y, q.z });
	uv.reserve(m.loopUv.size() * 2);
	for (const auto& u : m.loopUv) uv.insert(uv.end(), { u[0], u[1] });
	if (m.facePart.empty()) m.facePart.assign(m.faceSizes.size(), -1);
	nb::dict d;
	d["points"] = ToNumpy2D(std::move(pts), 3);
	d["loop_uvs"] = ToNumpy2D(std::move(uv), 2);
	d["face_verts"] = ToNumpy2D(std::move(m.faceVerts), 1);
	d["face_sizes"] = ToNumpy2D(std::move(m.faceSizes), 1);
	d["face_mat"] = ToNumpy2D(std::move(m.faceMat), 1);
	d["face_part"] = ToNumpy2D(std::move(m.facePart), 1);
	nb::dict attrs;
	for (auto& a : m.attrs) attrs[a.name.c_str()] = ToNumpy2D(std::move(a.values), 1);
	d["attrs"] = attrs;
	return d;
}

// Damage generators "on an object": the object's mesh from numpy arrays (paste
// ONCE after the PDGEN_BINDING_COMMON block). Marker: PDGEN_BINDING_MESH_COMMON
// PDGEN_BINDING_MESH_COMMON
using PdGenF64 = nb::ndarray<double, nb::c_contig, nb::device::cpu>;
using PdGenI32 = nb::ndarray<int32_t, nb::c_contig, nb::device::cpu>;
// points (N, 3), face_sizes (F,), face_verts (sum of sizes,), face_mat (F,) or
// empty, loop_uvs (sum of sizes, 2) or empty, face_smooth (F,) or empty.
// собираем входной меш из переданных точек, граней и материалов
static pdgen::MeshInput PdGenMeshInput(PdGenF64 points, PdGenI32 faceSizes, PdGenI32 faceVerts, PdGenI32 faceMat, PdGenF64 loopUvs,
	PdGenI32 faceSmooth, const char* what)
{
	pdgen::MeshInput in;
	if (points.ndim() != 2 || points.shape(1) != 3) throw std::invalid_argument(std::string(what) + ": points must be (N, 3)");
	const size_t np = points.shape(0);
	const double* pp = points.data();
	in.points.reserve(np);
	for (size_t i = 0; i < np; ++i)
	{
		const pdgen::V3 v(pp[3 * i], pp[3 * i + 1], pp[3 * i + 2]);
		if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) throw std::invalid_argument(std::string(what) + ": points must be finite");
		in.points.push_back(v);
	}
	const size_t nf = faceSizes.size(), nl = faceVerts.size();
	size_t total = 0;
	in.faceSizes.assign(faceSizes.data(), faceSizes.data() + nf);
	for (int32_t s : in.faceSizes)
	{
		if (s < 1 || s > 100000) throw std::invalid_argument(std::string(what) + ": bad face size");
		total += size_t(s);
	}
	if (total != nl) throw std::invalid_argument(std::string(what) + ": face_verts does not match face_sizes");
	in.faceVerts.assign(faceVerts.data(), faceVerts.data() + nl);
	for (int32_t v : in.faceVerts)
		if (v < 0 || size_t(v) >= np) throw std::invalid_argument(std::string(what) + ": a face points past the vertices");
	if (faceMat.size() == nf) in.faceMat.assign(faceMat.data(), faceMat.data() + nf);
	if (loopUvs.size() == 2 * nl)
	{
		in.loopUv.resize(nl);
		const double* u = loopUvs.data();
		for (size_t i = 0; i < nl; ++i) in.loopUv[i] = { std::isfinite(u[2 * i]) ? u[2 * i] : 0.0, std::isfinite(u[2 * i + 1]) ? u[2 * i + 1] : 0.0 };
	}
	if (faceSmooth.size() == nf)
	{
		in.faceSmooth.resize(nf);
		for (size_t i = 0; i < nf; ++i) in.faceSmooth[i] = faceSmooth.data()[i] != 0;
	}
	return in;
}

// Blast crater generator (blast_crater.cpp). Paste after the PDGEN_BINDING_COMMON block
// in project_damage_core_py.cpp (needs #include "blast_crater.h").
// PDGEN_BLAST_CRATER_FUNCTION
static nb::dict BuildBlastCraterPy(const nb::dict& settings, nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> craters)
{
	const pdgen::BlastCraterParams p = PdGenParamsFromDict<pdgen::BlastCraterParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitBlastCraterFields(q, f); }, "blast crater");
	if (p.width <= 0 || p.height <= 0 || p.resolution <= 0 || p.width / p.resolution > 2e4 || p.height / p.resolution > 2e4 ||
		p.radius < 0 || p.pavement < 0 || p.pavement > 2 || p.rayCount < 0 || p.rayCount > 64 || p.radialCracks < 0 || p.radialCracks > 64 ||
		p.debris < 0 || p.debris > 20000 || p.maxVerts < 1000 || p.maxVerts > 20000000 || p.plateSize <= 0)
		throw std::invalid_argument("blast crater: settings out of the supported range");
	std::vector<pdgen::CraterMarker> mk;
	if (craters.ndim() == 2 && craters.shape(0) > 0)
	{
		if (craters.shape(1) < 4) throw std::invalid_argument("blast crater: craters must be (N, 4) rows x, y, radius, depth");
		for (size_t i = 0; i < craters.shape(0) && i < 64; ++i)
		{
			pdgen::CraterMarker m{ craters(i, 0), craters(i, 1), craters(i, 2), craters(i, 3) };
			if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.radius) || !std::isfinite(m.depth) || m.radius <= 0)
				throw std::invalid_argument("blast crater: crater rows must be finite with a positive radius");
			mk.push_back(m);
		}
	}
	pdgen::BlastCraterResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildBlastCrater(p, mk, r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["craters"] = r.craters;
	d["plates"] = r.plates;
	d["debris"] = r.debris;
	d["resolution"] = r.resolution;
	return d;
}

// Erosion / weathering generator (erosion.cpp). Paste after the PDGEN_BINDING_COMMON block
// in project_damage_core_py.cpp (needs #include "erosion.h").
// PDGEN_EROSION_FUNCTION
static nb::dict BuildErosionPy(const nb::dict& settings, nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> points,
	nb::ndarray<int32_t, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> tris,
	nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> zones)
{
	const pdgen::ErosionParams p = PdGenParamsFromDict<pdgen::ErosionParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitErosionFields(q, f); }, "erosion");
	if (p.shape < 0 || p.shape > 4 || p.resolution <= 0 || p.maxVoxels < 1000 || p.maxVoxels > 200000000 || p.sizeX <= 0 ||
		p.sizeY <= 0 || p.sizeZ <= 0 || p.sizeX / p.resolution > 1e5 || p.sizeY / p.resolution > 1e5 || p.sizeZ / p.resolution > 1e5 ||
		p.blockWidth <= 0 || p.blockHeight <= 0 || p.edgeLossSize < 0 || p.pitSize < 0 || p.cavitySize < 0 || p.aggregateSize < 0)
		throw std::invalid_argument("erosion: settings out of the supported range");
	std::vector<pdgen::V3> P;
	std::vector<std::array<int32_t, 3>> T;
	if (p.shape == 0)
	{
		if (points.ndim() != 2 || points.shape(1) < 3 || tris.ndim() != 2 || tris.shape(1) < 3)
			throw std::invalid_argument("erosion: the source needs points (N, 3) and triangles (M, 3)");
		P.reserve(points.shape(0));
		for (size_t i = 0; i < points.shape(0); ++i)
		{
			const pdgen::V3 v(points(i, 0), points(i, 1), points(i, 2));
			if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) throw std::invalid_argument("erosion: source points must be finite");
			P.push_back(v);
		}
		T.reserve(tris.shape(0));
		for (size_t i = 0; i < tris.shape(0); ++i)
		{
			const std::array<int32_t, 3> t{ tris(i, 0), tris(i, 1), tris(i, 2) };
			for (int32_t v : t)
				if (v < 0 || size_t(v) >= P.size()) throw std::invalid_argument("erosion: a source triangle points past the vertices");
			T.push_back(t);
		}
	}
	std::vector<pdgen::ErosionMarker> mk;
	if (zones.ndim() == 2 && zones.shape(0) > 0)
	{
		if (zones.shape(1) < 5) throw std::invalid_argument("erosion: zones must be (N, 5) rows x, y, z, radius, strength");
		for (size_t i = 0; i < zones.shape(0) && i < 64; ++i)
		{
			pdgen::ErosionMarker m{ zones(i, 0), zones(i, 1), zones(i, 2), zones(i, 3), zones(i, 4) };
			if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.z) || !std::isfinite(m.radius) || !std::isfinite(m.strength) || m.radius <= 0)
				throw std::invalid_argument("erosion: zone rows must be finite with a positive radius");
			mk.push_back(m);
		}
	}
	pdgen::ErosionResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildErosion(p, P, T, mk, r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["error"] = nb::str(r.error.c_str());
	d["voxels"] = r.voxels;
	d["pits"] = r.pits;
	d["cavities"] = r.cavities;
	d["chunks"] = r.chunks;
	d["stones"] = r.stones;
	d["pieces"] = r.pieces;
	d["resolution"] = r.resolution;
	return d;
}

// Metal tear generator (metal_tear.cpp). Paste after the PDGEN_BINDING_COMMON block
// in project_damage_core_py.cpp (needs #include "metal_tear.h").
// PDGEN_METAL_TEAR_FUNCTION
using PdGenRowsIn = nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;
static nb::dict BuildMetalTearPy(const nb::dict& settings, PdGenRowsIn blasts)
{
	const pdgen::MetalTearParams p = PdGenParamsFromDict<pdgen::MetalTearParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitMetalTearFields(q, f); }, "metal tear");
	if (p.width <= 0 || p.height <= 0 || p.resolution <= 0 || p.thickness < 0 || p.width / p.resolution > 2e4 ||
		p.height / p.resolution > 2e4 || p.blasts < 0 || p.blasts > 64 || p.punctures < 0 || p.punctures > 2048 || p.dents < 0 ||
		p.dents > 512 || p.tears < 0 || p.tears > 256 || p.edgeBites < 0 || p.edgeBites > 64 || p.maxVerts < 1000 || p.maxVerts > 20000000)
		throw std::invalid_argument("metal tear: settings out of the supported range");
	std::vector<pdgen::MetalBlastMarker> mk;
	if (blasts.ndim() == 2 && blasts.shape(0) > 0)
	{
		if (blasts.shape(1) < 5) throw std::invalid_argument("metal tear: blasts must be (N, 5) rows x, y, radius, side, petals");
		for (size_t i = 0; i < blasts.shape(0) && i < 64; ++i)
		{
			pdgen::MetalBlastMarker b;
			b.x = blasts(i, 0); b.y = blasts(i, 1); b.radius = blasts(i, 2);
			b.side = blasts(i, 3) < 0 ? -1 : 1;
			b.petals = int32_t(blasts(i, 4));
			if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.radius) || b.radius <= 0)
				throw std::invalid_argument("metal tear: blast rows must be finite with a positive radius");
			mk.push_back(b);
		}
	}
	pdgen::MetalTearResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildMetalTear(p, mk, r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["blasts"] = r.blasts;
	d["punctures"] = r.punctures;
	d["petals"] = r.petals;
	d["tears"] = r.tears;
	d["dents"] = r.dents;
	d["bites"] = r.bites;
	d["resolution"] = r.resolution;
	return d;
}

// Metal tear on an object (metal_tear.cpp, BuildMetalTearOnMesh). Paste after the
// PDGEN_BINDING_MESH_COMMON block (needs #include "metal_tear.h").
// PDGEN_METAL_TEAR_MESH_FUNCTION
static nb::dict BuildMetalTearMeshPy(const nb::dict& settings, PdGenF64 points, PdGenI32 faceSizes, PdGenI32 faceVerts,
	PdGenI32 faceMat, PdGenF64 loopUvs, PdGenI32 faceSmooth, PdGenF64 blasts, int sourceMaterials, double smoothAngle)
{
	const pdgen::MetalTearParams p = PdGenParamsFromDict<pdgen::MetalTearParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitMetalTearFields(q, f); }, "metal tear");
	if (p.resolution <= 0 || p.thickness < 0 || p.blasts < 0 || p.blasts > 64 || p.punctures < 0 || p.punctures > 1024 || p.dents < 0 ||
		p.dents > 256 || p.tears < 0 || p.tears > 128 || p.maxVerts < 1000 || p.maxVerts > 20000000)
		throw std::invalid_argument("metal tear: settings out of the supported range");
	pdgen::MeshInput in = PdGenMeshInput(points, faceSizes, faceVerts, faceMat, loopUvs, faceSmooth, "metal tear");
	in.smoothAngle = smoothAngle;
	std::vector<pdgen::MetalMeshMarker> mk;
	if (blasts.ndim() == 2 && blasts.shape(0) > 0)
	{
		if (blasts.shape(1) < 6) throw std::invalid_argument("metal tear: blasts must be (N, 6) rows x, y, z, radius, side, petals");
		const double* b = blasts.data();
		const size_t cols = blasts.shape(1);
		for (size_t i = 0; i < blasts.shape(0) && i < 64; ++i)
		{
			pdgen::MetalMeshMarker m;
			m.p = pdgen::V3(b[i * cols], b[i * cols + 1], b[i * cols + 2]);
			m.radius = b[i * cols + 3];
			m.side = b[i * cols + 4] > 0 ? 1 : (b[i * cols + 4] < 0 ? -1 : 0);
			m.petals = int32_t(b[i * cols + 5]);
			if (!std::isfinite(m.p.x) || !std::isfinite(m.p.y) || !std::isfinite(m.p.z) || !std::isfinite(m.radius) || m.radius <= 0)
				throw std::invalid_argument("metal tear: blast rows must be finite with a positive radius");
			mk.push_back(m);
		}
	}
	pdgen::MetalTearResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildMetalTearOnMesh(p, in, mk, std::max(sourceMaterials, 1), r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["blasts"] = r.blasts;
	d["punctures"] = r.punctures;
	d["petals"] = r.petals;
	d["tears"] = r.tears;
	d["dents"] = r.dents;
	d["bites"] = 0;
	d["resolution"] = r.resolution;
	return d;
}

// Broken tile generator (tile_damage.cpp). Paste after the PDGEN_BINDING_COMMON block
// in project_damage_core_py.cpp (needs #include "tile_damage.h").
// PDGEN_TILE_DAMAGE_FUNCTION
static nb::dict BuildTileDamagePy(const nb::dict& settings, nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> impacts)
{
	const pdgen::TileDamageParams p = PdGenParamsFromDict<pdgen::TileDamageParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitTileDamageFields(q, f); }, "broken tiles");
	if (p.width <= 0 || p.height <= 0 || p.tileWidth <= 0 || p.tileHeight <= 0 || p.tileThickness <= 0 || p.resolution <= 0 ||
		p.width / p.tileWidth > 400 || p.height / p.tileHeight > 400 || p.width / p.resolution > 2e4 || p.height / p.resolution > 2e4 ||
		p.impacts < 0 || p.impacts > 64 || p.cracks < 0 || p.cracks > 64 || p.pattern < 0 || p.pattern > 2 || p.maxVerts < 1000 ||
		p.maxVerts > 20000000)
		throw std::invalid_argument("broken tiles: settings out of the supported range");
	std::vector<pdgen::TileImpactMarker> mk;
	if (impacts.ndim() == 2 && impacts.shape(0) > 0)
	{
		if (impacts.shape(1) < 4) throw std::invalid_argument("broken tiles: impacts must be (N, 4) rows x, y, radius, strength");
		for (size_t i = 0; i < impacts.shape(0) && i < 64; ++i)
		{
			pdgen::TileImpactMarker m{ impacts(i, 0), impacts(i, 1), impacts(i, 2), impacts(i, 3) };
			if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.radius) || !std::isfinite(m.strength) || m.radius <= 0)
				throw std::invalid_argument("broken tiles: impact rows must be finite with a positive radius");
			mk.push_back(m);
		}
	}
	pdgen::TileDamageResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildTileDamage(p, mk, r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["tiles"] = r.tiles;
	d["missing"] = r.missing;
	d["cracked"] = r.cracked;
	d["pieces"] = r.pieces;
	d["loose"] = r.loose;
	d["chips"] = r.chips;
	d["resolution"] = r.resolution;
	return d;
}

// Split wood generator (wood_split.cpp). Paste after the PDGEN_BINDING_COMMON block
// in project_damage_core_py.cpp (needs #include "wood_split.h").
// PDGEN_WOOD_SPLIT_FUNCTION
static nb::dict BuildWoodSplitPy(const nb::dict& settings)
{
	const pdgen::WoodSplitParams p = PdGenParamsFromDict<pdgen::WoodSplitParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitWoodSplitFields(q, f); }, "split wood");
	if (p.length <= 0 || p.width <= 0 || p.thickness <= 0 || p.resolution <= 0 || p.layers < 1 || p.layers > 32 ||
		p.length / p.resolution > 1e5 || p.width / p.resolution > 2e4 || p.splits < 0 || p.splits > 16 || p.breakMode < 0 ||
		p.breakMode > 2 || p.maxVerts < 1000 || p.maxVerts > 20000000)
		throw std::invalid_argument("split wood: settings out of the supported range");
	pdgen::WoodSplitResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildWoodSplit(p, r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["pieces"] = r.pieces;
	d["splinters"] = r.splinters;
	d["splits"] = r.splits;
	d["resolution"] = r.resolution;
	return d;
}

// Split wood on an object (wood_split.cpp, BuildWoodSplitOnMesh). Paste after the
// PDGEN_BINDING_MESH_COMMON block (needs #include "wood_split.h").
// PDGEN_WOOD_SPLIT_MESH_FUNCTION
static nb::dict BuildWoodSplitMeshPy(const nb::dict& settings, PdGenF64 points, PdGenI32 faceSizes, PdGenI32 faceVerts,
	PdGenI32 faceMat, PdGenF64 loopUvs, PdGenI32 faceSmooth, int axis, int sourceMaterials, double smoothAngle)
{
	const pdgen::WoodSplitParams p = PdGenParamsFromDict<pdgen::WoodSplitParams>(settings,
		[](auto& q, auto&& f) { pdgen::VisitWoodSplitFields(q, f); }, "split wood");
	if (p.resolution <= 0 || p.layers < 1 || p.layers > 32 || p.maxVerts < 1000 || p.maxVerts > 20000000 || p.breakMode < 0 || p.breakMode > 2)
		throw std::invalid_argument("split wood: settings out of the supported range");
	pdgen::MeshInput in = PdGenMeshInput(points, faceSizes, faceVerts, faceMat, loopUvs, faceSmooth, "split wood");
	in.smoothAngle = smoothAngle;
	pdgen::WoodSplitResult r;
	{
		nb::gil_scoped_release release;
		pdgen::BuildWoodSplitOnMesh(p, in, axis, std::max(sourceMaterials, 1), r);
	}
	nb::dict d = PdGenMeshToDict(std::move(r.mesh));
	d["ok"] = r.ok;
	d["pieces"] = r.pieces;
	d["splinters"] = r.splinters;
	d["splits"] = 0;
	d["resolution"] = r.resolution;
	return d;
}

NB_MODULE(project_damage_core_py, mod)
{
 mod.def("build_rebar_surface_path",[](const BreakRebarParams& p,const std::vector<double>& xyz,
 const std::vector<double>& normal,bool freeStart,bool freeEnd){
  if(xyz.size()!=normal.size() || xyz.size()%3 || xyz.size()>300000)throw nb::value_error("Invalid structural rebar coordinates");
  std::vector<EVec> points,normals;points.reserve(xyz.size()/3);normals.reserve(xyz.size()/3);
  for(size_t i=0;i<xyz.size();i+=3){points.emplace_back(xyz[i],xyz[i+1],xyz[i+2]);normals.emplace_back(normal[i],normal[i+1],normal[i+2]);}
  BreakRebarMesh out;{nb::gil_scoped_release release;BuildRebarSurfacePath(p,points,normals,freeStart,freeEnd,out);}
  std::vector<double> verts;std::vector<int32_t> faces;
  for(const auto& q:out.points){verts.push_back(q.x);verts.push_back(q.y);verts.push_back(q.z);}
  for(const auto& f:out.polys){faces.push_back(f.a);faces.push_back(f.b);faces.push_back(f.c);faces.push_back(f.d);}
  nb::dict result;result["points"]=ToNumpy2D(std::move(verts),3);result["polys"]=ToNumpy2D(std::move(faces),4);return result;
 });

 mod.def("build_brick_surface",[](const BrickDamageParams& p,const std::vector<double>& uv,const std::vector<double>& xyz,const std::vector<double>& normals,
 double u0,double u1,double v0,double v1,int nu,int nv,const std::vector<double>& damage,const std::vector<double>& loose,double offset,double opening){
  BrickSurfaceMesh out;{nb::gil_scoped_release release;out=BuildBrickSurface(p,uv,xyz,normals,u0,u1,v0,v1,nu,nv,damage,loose,offset,opening);}
  std::vector<double> pts,flat;for(const auto& v:out.points){pts.push_back(v.x);pts.push_back(v.y);pts.push_back(v.z);}
  for(const auto& v:out.flat){flat.push_back(v.x);flat.push_back(v.y);flat.push_back(v.z);}
  nb::dict result;result["points"]=ToNumpy2D(std::move(pts),3);result["flat"]=ToNumpy2D(std::move(flat),3);
  result["faces"]=ToNumpy2D(std::move(out.triangles),3);result["materials"]=ToNumpy2D(std::move(out.materials),1);result["pieces"]=ToNumpy2D(std::move(out.pieces),1);return result;
 });

 mod.def("build_lath_surface",[](const LathDamageParams& p,double u0,double u1,double v0,double v1,int nu,int nv,
 const std::vector<double>& height,const std::vector<uint8_t>& valid,const std::vector<uint8_t>& holes,double nailSize){
  std::vector<double> xyz,frames;std::vector<int32_t> faces,material,piece,vertexPiece;std::vector<int32_t> types;
  {
   nb::gil_scoped_release release;
   auto solids=BuildLathSurface(p,u0,u1,v0,v1,nu,nv,height,valid,holes,types,frames,nailSize);
   for(size_t k=0;k<solids.size();++k){
    auto mesh=solids[k].GetMeshGL64();const auto off=int32_t(xyz.size()/3);
    for(size_t i=0;i<mesh.vertProperties.size();i+=mesh.numProp){for(int a=0;a<3;++a)xyz.push_back(mesh.vertProperties[i+a]);vertexPiece.push_back(int32_t(k));}
    for(size_t i=0;i<mesh.triVerts.size();i+=3){for(int a=0;a<3;++a)faces.push_back(off+int32_t(mesh.triVerts[i+a]));material.push_back(types[k]);piece.push_back(int32_t(k));}
   }
  }
  nb::dict result;result["points"]=ToNumpy2D(std::move(xyz),3);result["faces"]=ToNumpy2D(std::move(faces),3);
  result["materials"]=ToNumpy2D(std::move(material),1);result["pieces"]=ToNumpy2D(std::move(piece),1);result["frames"]=ToNumpy2D(std::move(frames),3);result["vertex_pieces"]=ToNumpy2D(std::move(vertexPiece),1);return result;
 },nb::arg("settings"),nb::arg("u0"),nb::arg("u1"),nb::arg("v0"),nb::arg("v1"),nb::arg("nu"),nb::arg("nv"),
 nb::arg("height"),nb::arg("valid"),nb::arg("holes"),nb::arg("nail_size")=1.);

	// PDGEN_WOOD_SPLIT_MESH_REGISTER (paste into the NB_MODULE body)
	mod.def("build_wood_split_mesh", &BuildWoodSplitMeshPy, nb::arg("settings"), nb::arg("points"), nb::arg("face_sizes"),
		nb::arg("face_verts"), nb::arg("face_mat"), nb::arg("loop_uvs"), nb::arg("face_smooth"), nb::arg("axis") = -1,
		nb::arg("source_materials") = 1, nb::arg("smooth_angle") = 40.0,
		"Split wood made in an existing mesh: the object is broken across its grain (axis 0 X, 1 Y, 2 Z, -1 the longest "
		"side) with splintered, interlocking ends; its UVs and materials are kept.");
	// PDGEN_WOOD_SPLIT_REGISTER (paste into the NB_MODULE body)
	mod.def("build_wood_split", &BuildWoodSplitPy, nb::arg("settings"),
		"A board snapped across the grain and split along it, with long interlocking splinters peeling away, "
		"built from fibre layers. settings: dict of WoodSplitParams fields (snake_case).");
	mod.def("wood_split_defaults", []() { return PdGenParamsToDict<pdgen::WoodSplitParams>([](auto& q, auto&& f) { pdgen::VisitWoodSplitFields(q, f); }); },
		"Default settings of build_wood_split as a dict.");
	// PDGEN_TILE_DAMAGE_REGISTER (paste into the NB_MODULE body)
	mod.def("build_tile_damage", &BuildTileDamagePy, nb::arg("settings"), nb::arg("impacts"),
		"A tiled wall or floor, broken: knocked-out tiles (some leave pieces), loose tilted tiles, cracks that follow the grout "
		"and split tiles, chips, crumbled grout, adhesive ridges on the bed. settings: dict of TileDamageParams fields; "
		"impacts: float64 (N, 4) rows x, y, radius, strength in the object's space.");
	mod.def("tile_damage_defaults", []() { return PdGenParamsToDict<pdgen::TileDamageParams>([](auto& q, auto&& f) { pdgen::VisitTileDamageFields(q, f); }); },
		"Default settings of build_tile_damage as a dict.");
	// PDGEN_METAL_TEAR_MESH_REGISTER (paste into the NB_MODULE body)
	mod.def("build_metal_tear_mesh", &BuildMetalTearMeshPy, nb::arg("settings"), nb::arg("points"), nb::arg("face_sizes"),
		nb::arg("face_verts"), nb::arg("face_mat"), nb::arg("loop_uvs"), nb::arg("face_smooth"), nb::arg("blasts"),
		nb::arg("source_materials") = 1, nb::arg("smooth_angle") = 40.0,
		"Torn metal made in an existing mesh: blast holes with curled petals, punctures, tears and dents cut into the "
		"object's own surface (its UVs and materials are kept). blasts: float64 (N, 6) rows x, y, z, radius, side "
		"(0: from the marker's position), petals in the object's space with its scale applied.");
	// PDGEN_METAL_TEAR_REGISTER (paste into the NB_MODULE body)
	mod.def("build_metal_tear", &BuildMetalTearPy, nb::arg("settings"), nb::arg("blasts"),
		"Bent and torn sheet metal built from scratch: blast holes with curled petals, punctures, torn slits, "
		"ragged border, dents, corrugation, bend. settings: dict of MetalTearParams fields (snake_case); "
		"blasts: float64 (N, 5) rows x, y, radius, side, petals in the sheet's object space.");
	mod.def("metal_tear_defaults", []() { return PdGenParamsToDict<pdgen::MetalTearParams>([](auto& q, auto&& f) { pdgen::VisitMetalTearFields(q, f); }); },
		"Default settings of build_metal_tear as a dict.");
	// PDGEN_EROSION_REGISTER (paste into the NB_MODULE body)
	mod.def("build_erosion", &BuildErosionPy, nb::arg("settings"), nb::arg("points"), nb::arg("triangles"), nb::arg("zones"),
		"Stone or concrete worn by weather: rounded edges, chunks lost along the edges, granular surface, bedding grooves, pits, "
		"hollows, exposed aggregate. settings: dict of ErosionParams fields (shape 0 uses the source mesh: points float64 (N, 3), "
		"triangles int32 (M, 3); other shapes ignore them); zones: float64 (N, 5) rows x, y, z, radius, strength of stronger wear.");
	mod.def("erosion_defaults", []() { return PdGenParamsToDict<pdgen::ErosionParams>([](auto& q, auto&& f) { pdgen::VisitErosionFields(q, f); }); },
		"Default settings of build_erosion as a dict.");
	// PDGEN_BLAST_CRATER_REGISTER (paste into the NB_MODULE body)
	mod.def("build_blast_crater", &BuildBlastCraterPy, nb::arg("settings"), nb::arg("craters"),
		"A patch of ground with blast craters: bowl, rim, thrown-out blanket with streaks and clods, pavement torn away and "
		"broken into tilted plates and cracks, debris. settings: dict of BlastCraterParams fields; craters: float64 (N, 4) rows "
		"x, y, radius, depth factor in the object's space (added to the main crater at the origin).");
	mod.def("blast_crater_defaults", []() { return PdGenParamsToDict<pdgen::BlastCraterParams>([](auto& q, auto&& f) { pdgen::VisitBlastCraterFields(q, f); }); },
		"Default settings of build_blast_crater as a dict.");
                                                                                    
    const char* background = std::getenv("PD_PRIVATE_WORKER");
    if (background && background[0]=='1') {
        static tbb::global_control worker_limit(tbb::global_control::max_allowed_parallelism, 1);
    }

    mod.def("surface_guide_field", &SurfaceGuideFieldPy, nb::arg("points"), nb::arg("polys"));
    mod.attr("edge_paint") = true;
    mod.attr("damage_objects") = true;
    mod.attr("organic_damage") = true;
    mod.attr("legacy_boolean_wood") = true;
    mod.attr("central_rebar_layout") = true;
    mod.attr("worn_v2") = true;                                                                 
    mod.attr("draw_mask") = true;
    mod.attr("wallpaper_material_regions") = true;
    mod.attr("wallpaper_parameter_recovery") = true;
    mod.attr("brick_material_regions") = true;
    mod.attr("wallpaper_intact_material") = true;
    mod.attr("wallpaper_flap_polys") = true;                                                            

    nb::class_<BuildingBlastEmpty>(mod, "BuildingBlastEmpty").def(nb::init<>())
        .def_prop_rw("rel", [](const BuildingBlastEmpty& e) { return std::vector<double>(e.rel,e.rel+12); },
            [](BuildingBlastEmpty& e,const std::vector<double>& v) {
                if(v.size()!=12) throw nb::value_error("rel requires 12 finite values");
                for(double x:v) if(!std::isfinite(x)) throw nb::value_error("rel must be finite");
                std::copy(v.begin(),v.end(),e.rel);
            })
        .def_rw("size", &BuildingBlastEmpty::size).def_rw("strength", &BuildingBlastEmpty::strength);
    nb::class_<BulletImpactParams>(mod, "BulletImpactParams").def(nb::init<>())
        .def_rw("enable", &BulletImpactParams::enable)
        .def_rw("seed", &BulletImpactParams::seed)
        .def_rw("mode", &BulletImpactParams::mode)
        .def_rw("material", &BulletImpactParams::material)
        .def_rw("avoid", &BulletImpactParams::avoid)
        .def_rw("detail", &BulletImpactParams::detail)
        .def_rw("size_min", &BulletImpactParams::sizeMin)
        .def_rw("size_max", &BulletImpactParams::sizeMax)
        .def_rw("size_bias", &BulletImpactParams::sizeBias)
        .def_rw("penetration", &BulletImpactParams::penetration)
        .def_rw("penetration_var", &BulletImpactParams::penetrationVar)
        .def_rw("thickness_cap", &BulletImpactParams::thicknessCap)
        .def_rw("shot_mode", &BulletImpactParams::shotMode)
        .def_rw("oblique", &BulletImpactParams::oblique)
        .def_rw("shot_x", &BulletImpactParams::shotX)
        .def_rw("shot_y", &BulletImpactParams::shotY)
        .def_rw("shot_z", &BulletImpactParams::shotZ)
        .def_rw("elongate", &BulletImpactParams::elongate)
        .def_rw("amount", &BulletImpactParams::amount)
        .def_rw("clusters", &BulletImpactParams::clusters)
        .def_rw("cluster_share", &BulletImpactParams::clusterShare)
        .def_rw("cluster_spread", &BulletImpactParams::clusterSpread)
        .def_rw("min_spacing", &BulletImpactParams::minSpacing)
        .def_rw("paint_spacing", &BulletImpactParams::paintSpacing)
        .def_rw("paint_spread", &BulletImpactParams::paintSpread)
        .def_rw("paint_snap", &BulletImpactParams::paintSnap)
        .def_rw("paint_close", &BulletImpactParams::paintClose)
        .def_rw("paint_single", &BulletImpactParams::paintSingle)
        .def_rw("halo_enable", &BulletImpactParams::haloEnable)
        .def_rw("halo_scale", &BulletImpactParams::haloScale)
        .def_rw("halo_depth", &BulletImpactParams::haloDepth)
        .def_rw("halo_jagged", &BulletImpactParams::haloJagged)
        .def_rw("pit_amount", &BulletImpactParams::pitAmount)
        .def_rw("pit_size", &BulletImpactParams::pitSize)
        .def_rw("pit_spread", &BulletImpactParams::pitSpread)
        .def_rw("metal_hole_jagged", &BulletImpactParams::metalHoleJagged)
        .def_rw("metal_dish", &BulletImpactParams::metalDish)
        .def_rw("metal_dish_width", &BulletImpactParams::metalDishWidth)
        .def_rw("metal_lip_height", &BulletImpactParams::metalLipHeight)
        .def_rw("metal_lip_width", &BulletImpactParams::metalLipWidth)
        .def_rw("metal_petals", &BulletImpactParams::metalPetals)
        .def_rw("metal_petal_tear", &BulletImpactParams::metalPetalTear)
        .def_rw("metal_curl", &BulletImpactParams::metalCurl)
        .def_rw("metal_exit_burr", &BulletImpactParams::metalExitBurr)
        .def_rw("metal_dent_chance", &BulletImpactParams::metalDentChance)
        .def_rw("grain_mode", &BulletImpactParams::grainMode)
        .def_rw("grain_x", &BulletImpactParams::grainX)
        .def_rw("grain_y", &BulletImpactParams::grainY)
        .def_rw("grain_z", &BulletImpactParams::grainZ)
        .def_rw("wood_crush", &BulletImpactParams::woodCrush)
        .def_rw("wood_splinter", &BulletImpactParams::woodSplinter)
        .def_rw("wood_fiber_width", &BulletImpactParams::woodFiberWidth)
        .def_rw("wood_exit_scale", &BulletImpactParams::woodExitScale)
        .def_rw("wood_exit_depth", &BulletImpactParams::woodExitDepth)
        .def_rw("wood_slivers", &BulletImpactParams::woodSlivers)
        .def_rw("wood_sliver_length", &BulletImpactParams::woodSliverLength)
        .def_rw("wood_split_chance", &BulletImpactParams::woodSplitChance)
        .def_rw("wood_split_length", &BulletImpactParams::woodSplitLength)
        .def_rw("conc_spall_chance", &BulletImpactParams::concSpallChance)
        .def_rw("conc_spall_scale", &BulletImpactParams::concSpallScale)
        .def_rw("conc_spall_depth", &BulletImpactParams::concSpallDepth)
        .def_rw("conc_skin_depth", &BulletImpactParams::concSkinDepth)
        .def_rw("conc_bore_depth", &BulletImpactParams::concBoreDepth)
        .def_rw("conc_jagged", &BulletImpactParams::concJagged)
        .def_rw("conc_roughness", &BulletImpactParams::concRoughness)
        .def_rw("conc_aggregate", &BulletImpactParams::concAggregate)
        .def_rw("conc_exit_scale", &BulletImpactParams::concExitScale)
        .def_rw("conc_cracks", &BulletImpactParams::concCracks)
        .def_rw("conc_crack_length", &BulletImpactParams::concCrackLength)
        .def_rw("crack_width", &BulletImpactParams::crackWidth)
        .def_rw("crack_depth", &BulletImpactParams::crackDepth)
        ;
    nb::class_<BuildingDestructionParams>(mod, "BuildingDestructionParams").def(nb::init<>())
        .def_rw("enable", &BuildingDestructionParams::enable)
        .def_rw("seed", &BuildingDestructionParams::seed)
        .def_rw("avoid", &BuildingDestructionParams::avoid)
        .def_rw("detail", &BuildingDestructionParams::detail)
        .def_rw("radius_scale", &BuildingDestructionParams::radiusScale)
        .def_rw("proximity", &BuildingDestructionParams::proximity)
        .def_rw("prox_shrink", &BuildingDestructionParams::proxShrink)
        .def_rw("jagged", &BuildingDestructionParams::jagged)
        .def_rw("noise_scale", &BuildingDestructionParams::noiseScale)
        .def_rw("floor_height", &BuildingDestructionParams::floorHeight)
        .def_rw("floor_offset", &BuildingDestructionParams::floorOffset)
        .def_rw("slab_thickness", &BuildingDestructionParams::slabThickness)
        .def_rw("wall_depth", &BuildingDestructionParams::wallDepth)
        .def_rw("core_size", &BuildingDestructionParams::coreSize)
        .def_rw("room_size", &BuildingDestructionParams::roomSize)
        .def_rw("room_facets", &BuildingDestructionParams::roomFacets)
        .def_rw("partition_spacing", &BuildingDestructionParams::partitionSpacing)
        .def_rw("partition_thickness", &BuildingDestructionParams::partitionThickness)
        .def_rw("slab_break", &BuildingDestructionParams::slabBreak)
        .def_rw("slab_droop", &BuildingDestructionParams::slabDroop)
        .def_rw("wall_size", &BuildingDestructionParams::wallSize)
        .def_rw("fringe", &BuildingDestructionParams::fringe)
        .def_rw("fringe_density", &BuildingDestructionParams::fringeDensity)
        .def_rw("spall_reach", &BuildingDestructionParams::spallReach)
        .def_rw("spall_cover", &BuildingDestructionParams::spallCover)
        .def_rw("brick_length", &BuildingDestructionParams::brickLength)
        .def_rw("brick_height", &BuildingDestructionParams::brickHeight)
        .def_rw("joint_width", &BuildingDestructionParams::jointWidth)
        .def_rw("joint_depth", &BuildingDestructionParams::jointDepth)
        .def_rw("brick_jitter", &BuildingDestructionParams::brickJitter)
        .def_rw("plaster_depth", &BuildingDestructionParams::plasterDepth)
        .def_rw("crack_enable", &BuildingDestructionParams::crackEnable)
        .def_rw("crack_count", &BuildingDestructionParams::crackCount)
        .def_rw("crack_length", &BuildingDestructionParams::crackLength)
        .def_rw("crack_width", &BuildingDestructionParams::crackWidth)
        .def_rw("crack_depth", &BuildingDestructionParams::crackDepth)
        .def_rw("empties", &BuildingDestructionParams::empties)
        ;
    nb::class_<GroundCracksParams>(mod, "GroundCracksParams").def(nb::init<>())
        .def_rw("enable", &GroundCracksParams::enable)
        .def_rw("generated_geometry", &GroundCracksParams::generatedGeometry)
        .def_rw("seed", &GroundCracksParams::seed)
        .def_rw("mode", &GroundCracksParams::mode)
        .def_rw("avoid", &GroundCracksParams::avoid)
        .def_rw("detail", &GroundCracksParams::detail)
        .def_rw("up_x", &GroundCracksParams::upX)
        .def_rw("up_y", &GroundCracksParams::upY)
        .def_rw("up_z", &GroundCracksParams::upZ)
        .def_rw("max_slope", &GroundCracksParams::maxSlope)
        .def_rw("amount", &GroundCracksParams::amount)
        .def_rw("size_min", &GroundCracksParams::sizeMin)
        .def_rw("size_max", &GroundCracksParams::sizeMax)
        .def_rw("area_chance", &GroundCracksParams::areaChance)
        .def_rw("area_size", &GroundCracksParams::areaSize)
        .def_rw("area_stretch", &GroundCracksParams::areaStretch)
        .def_rw("paint_snap", &GroundCracksParams::paintSnap)
        .def_rw("paint_close", &GroundCracksParams::paintClose)
        .def_rw("paint_jitter", &GroundCracksParams::paintJitter)
        .def_rw("paint_branch", &GroundCracksParams::paintBranch)
        .def_rw("crack_enable", &GroundCracksParams::crackEnable)
        .def_rw("crack_amount", &GroundCracksParams::crackAmount)
        .def_rw("crack_generations", &GroundCracksParams::crackGenerations)
        .def_rw("crack_length_min", &GroundCracksParams::crackLengthMin)
        .def_rw("crack_length_max", &GroundCracksParams::crackLengthMax)
        .def_rw("crack_width", &GroundCracksParams::crackWidth)
        .def_rw("crack_width_var", &GroundCracksParams::crackWidthVar)
        .def_rw("crack_taper", &GroundCracksParams::crackTaper)
        .def_rw("crack_depth", &GroundCracksParams::crackDepth)
        .def_rw("crack_meander", &GroundCracksParams::crackMeander)
        .def_rw("crack_kink", &GroundCracksParams::crackKink)
        .def_rw("crack_branch", &GroundCracksParams::crackBranch)
        .def_rw("crack_join", &GroundCracksParams::crackJoin)
        .def_rw("crack_chip", &GroundCracksParams::crackChip)
        .def_rw("area_jagged", &GroundCracksParams::areaJagged)
        .def_rw("rim_fade", &GroundCracksParams::rimFade)
        .def_rw("plate_size", &GroundCracksParams::plateSize)
        .def_rw("plate_center_scale", &GroundCracksParams::plateCenterScale)
        .def_rw("plate_jagged", &GroundCracksParams::plateJagged)
        .def_rw("gap_width", &GroundCracksParams::gapWidth)
        .def_rw("gap_depth", &GroundCracksParams::gapDepth)
        .def_rw("plate_sink", &GroundCracksParams::plateSink)
        .def_rw("plate_tilt", &GroundCracksParams::plateTilt)
        .def_rw("pothole_chance", &GroundCracksParams::potholeChance)
        .def_rw("pothole_size", &GroundCracksParams::potholeSize)
        .def_rw("pothole_loose", &GroundCracksParams::potholeLoose)
        .def_rw("pothole_depth", &GroundCracksParams::potholeDepth)
        .def_rw("roughness", &GroundCracksParams::roughness)
        .def_rw("aggregate_size", &GroundCracksParams::aggregateSize)
        .def_rw("thickness_cap", &GroundCracksParams::thicknessCap)
        .def_rw("rubble_enable", &GroundCracksParams::rubbleEnable)
        .def_rw("rubble_amount", &GroundCracksParams::rubbleAmount)
        .def_rw("rubble_size_min", &GroundCracksParams::rubbleSizeMin)
        .def_rw("rubble_size_max", &GroundCracksParams::rubbleSizeMax)
        .def_rw("heave_chance", &GroundCracksParams::heaveChance)
        .def_rw("heave_height", &GroundCracksParams::heaveHeight)
        .def_rw("heave_width", &GroundCracksParams::heaveWidth)
        .def_rw("heave_gap", &GroundCracksParams::heaveGap)
        ;
    
    using TrisIn = nb::ndarray<int32_t, nb::shape<-1,3>,nb::c_contig,nb::device::cpu>;
    nb::class_<MetalDeformImpactor>(mod,"MetalDeformImpactor").def(nb::init<>())
        .def("set_mesh",[](MetalDeformImpactor& im,PointsIn pts,TrisIn tris) {
            for(size_t i=0;i<pts.shape(0)*3;++i) if(!std::isfinite(pts.data()[i])) throw nb::value_error("Non-finite impactor point");
            for(size_t i=0;i<tris.shape(0)*3;++i) if(tris.data()[i]<0 || (size_t)tris.data()[i]>=pts.shape(0)) throw nb::value_error("Impactor index out of range");
            im.pts.assign(pts.data(),pts.data()+pts.shape(0)*3); im.tris.assign(tris.data(),tris.data()+tris.shape(0)*3);
        }).def_rw("strength",&MetalDeformImpactor::strength)
        .def_ro("pts",&MetalDeformImpactor::pts).def_ro("tris",&MetalDeformImpactor::tris);
    nb::class_<MetalCrushEmpty>(mod,"MetalCrushEmpty").def(nb::init<>())
        .def_prop_rw("rel",[](const MetalCrushEmpty& e){return std::vector<double>(e.rel,e.rel+12);},
          [](MetalCrushEmpty& e,const std::vector<double>& v){if(v.size()!=12) throw nb::value_error("rel needs 12 values"); for(double x:v) if(!std::isfinite(x)) throw nb::value_error("Non-finite transform"); std::copy(v.begin(),v.end(),e.rel);})
        .def_rw("size",&MetalCrushEmpty::size).def_rw("strength",&MetalCrushEmpty::strength);
        nb::class_<BreakRebarParams>(mod,"BreakRebarParams").def(nb::init<>())
                .def_rw("mode",&BreakRebarParams::mode)
        .def_rw("fit",&BreakRebarParams::fit)
        .def_rw("deform",&BreakRebarParams::deform)
        .def_rw("reach",&BreakRebarParams::reach)
        .def_rw("core",&BreakRebarParams::core)
        .def_rw("show_all",&BreakRebarParams::showAll)
        .def_rw("existing_damage",&BreakRebarParams::existingDamage)
        .def_rw("stroke_radius",&BreakRebarParams::strokeRadius)
        .def_rw("stroke_embed",&BreakRebarParams::strokeEmbed)
        .def_ro("stroke_pts",&BreakRebarParams::strokePts)
        .def_ro("stroke_count",&BreakRebarParams::strokeCount)
        .def("set_strokes",[](BreakRebarParams& p,const std::vector<double>& xyz,const std::vector<int32_t>& counts) {
            if(xyz.size()>6000000 || xyz.size()%3) throw nb::value_error("Drawn rebar needs complete xyz triples (max 2M points)");
            size_t total=0;
            for(int32_t n:counts) { if(n<2) throw nb::value_error("Each drawn bar needs at least two points"); total+=(size_t)n; }
            if(total!=xyz.size()/3) throw nb::value_error("Drawn bar counts do not match point array");
            for(double v:xyz) if(!std::isfinite(v)) throw nb::value_error("Drawn bar points must be finite");
            p.strokePts=xyz;p.strokeCount=counts;
        })
        .def_rw("enable",&BreakRebarParams::enable)
        .def_rw("generated_geometry", &BreakRebarParams::generatedGeometry)
        .def_rw("experimental_riven", &BreakRebarParams::experimentalRiven)
        .def_rw("stable_volume", &BreakRebarParams::stableVolume)
        .def_rw("volume_axis", &BreakRebarParams::volumeAxis)
        .def_rw("volume_budget", &BreakRebarParams::volumeBudget)
        .def_rw("volume_clearance", &BreakRebarParams::volumeClearance)
        .def_rw("volume_min_length", &BreakRebarParams::volumeMinLength)
        .def_rw("volume_min_section", &BreakRebarParams::volumeMinSection)
        .def_rw("volume_origin_x", &BreakRebarParams::volumeOriginX)
        .def_rw("volume_origin_y", &BreakRebarParams::volumeOriginY)
        .def_rw("volume_origin_z", &BreakRebarParams::volumeOriginZ)
        .def_rw("seed",&BreakRebarParams::seed)
        .def_rw("layout",&BreakRebarParams::layout)
        .def_rw("axis",&BreakRebarParams::axis)
        .def_rw("axis_mask",&BreakRebarParams::axisMask)
        .def_rw("side",&BreakRebarParams::side)
        .def_rw("angle",&BreakRebarParams::angle)
        .def_rw("cover",&BreakRebarParams::cover)
        .def_rw("spacing_u",&BreakRebarParams::spacingU)
        .def_rw("spacing_v",&BreakRebarParams::spacingV)
        .def_rw("bars_u",&BreakRebarParams::barsU)
        .def_rw("bars_v",&BreakRebarParams::barsV)
        .def_rw("outer",&BreakRebarParams::outer)
        .def_rw("amount",&BreakRebarParams::amount)
        .def_rw("jitter",&BreakRebarParams::jitter)
        .def_rw("tie_spacing",&BreakRebarParams::tieSpacing)
        .def_rw("tie_radius",&BreakRebarParams::tieRadius)
        .def_rw("spiral",&BreakRebarParams::spiral)
        .def_rw("type",&BreakRebarParams::type)
        .def_rw("radius",&BreakRebarParams::radius)
        .def_rw("scale",&BreakRebarParams::scale)
        .def_rw("rib_height",&BreakRebarParams::ribHeight)
        .def_rw("rib_pitch",&BreakRebarParams::ribPitch)
        .def_rw("twist_pitch",&BreakRebarParams::twistPitch)
        .def_rw("sides",&BreakRebarParams::sides)
        .def_rw("rebar_resolution", &BreakRebarParams::rebarResolution)
        .def_rw("detail",&BreakRebarParams::detail)
        .def_rw("embed",&BreakRebarParams::embed)
        .def_rw("min_exposed",&BreakRebarParams::minExposed)
        .def_rw("wobble",&BreakRebarParams::wobble)
        .def_rw("bow",&BreakRebarParams::bow)
        .def_rw("bend",&BreakRebarParams::bend)
        .def_rw("broken",&BreakRebarParams::broken)
        .def_rw("trim",&BreakRebarParams::trim)
        .def_rw("neck",&BreakRebarParams::neck)
        .def_rw("drop_loose",&BreakRebarParams::dropLoose)
        .def_rw("material",&BreakRebarParams::material)
        .def_rw("up_x",&BreakRebarParams::upX)
        .def_rw("up_y",&BreakRebarParams::upY)
        .def_rw("up_z",&BreakRebarParams::upZ)
        .def_rw("mortar_joints",&BreakRebarParams::mortarJoints)
        .def_rw("brick_3d",&BreakRebarParams::brick3D)
        .def_rw("mortar_recess",&BreakRebarParams::mortarRecess)
        .def_rw("brick_length",&BreakRebarParams::brickLength)
        .def_rw("brick_height",&BreakRebarParams::brickHeight)
        .def_rw("brick_depth",&BreakRebarParams::brickDepth)
        .def_rw("mortar",&BreakRebarParams::mortar)
        .def_rw("missing",&BreakRebarParams::missing)
        .def_rw("brick_broken",&BreakRebarParams::brickBroken)
        .def_rw("chamfer",&BreakRebarParams::chamfer)
        .def_rw("brick_jitter",&BreakRebarParams::brickJitter)
        .def_rw("plank_width",&BreakRebarParams::plankWidth)
        .def_rw("stud_width",&BreakRebarParams::studWidth)
        .def_rw("stud_depth",&BreakRebarParams::studDepth)
        .def_rw("stud_spacing",&BreakRebarParams::studSpacing)
        .def_rw("joist_depth",&BreakRebarParams::joistDepth)
        .def_rw("noggin_spacing",&BreakRebarParams::nogginSpacing)
        .def_rw("double_plate",&BreakRebarParams::doublePlate)
        .def_rw("skin",&BreakRebarParams::skin)
        .def_rw("cavity_reach",&BreakRebarParams::cavityReach)
        .def_rw("paper_tear",&BreakRebarParams::paperTear)
        .def_rw("paper_depth",&BreakRebarParams::paperDepth)
        .def_rw("nails",&BreakRebarParams::nails)
        .def_rw("nail_length",&BreakRebarParams::nailLength)
        .def_rw("wood_broken",&BreakRebarParams::woodBroken)
        .def_rw("facet_angle",&BreakRebarParams::facetAngle)
        .def_rw("plank_thickness",&BreakRebarParams::plankThickness)
        .def_rw("plank_gap",&BreakRebarParams::plankGap)
        .def_rw("plank_dir",&BreakRebarParams::plankDir)
        .def_rw("splinter",&BreakRebarParams::splinter)
        .def_rw("chip_plies",&BreakRebarParams::chipPlies)
        .def_rw("chip_ply_thickness",&BreakRebarParams::chipPlyThickness)
        .def_rw("chip_ply_angle",&BreakRebarParams::chipPlyAngle)
        .def_rw("chip_length",&BreakRebarParams::chipLength)
        .def_rw("chip_width",&BreakRebarParams::chipWidth)
        .def_rw("chip_sharp",&BreakRebarParams::chipSharp)
        .def_rw("chip_lift",&BreakRebarParams::chipLift)
        .def_rw("chip_amount",&BreakRebarParams::chipAmount)
        .def_rw("chip_crater",&BreakRebarParams::chipCrater)
        .def_rw("chip_face",&BreakRebarParams::chipFace)
        ;
    nb::class_<EdgeBevelParams>(mod,"EdgeBevelParams").def(nb::init<>())
        .def_rw("enable",&EdgeBevelParams::enable)
        .def_rw("seed",&EdgeBevelParams::seed)
        .def_rw("avoid",&EdgeBevelParams::avoid)
        .def_rw("detail",&EdgeBevelParams::detail)
        .def_rw("angle",&EdgeBevelParams::angle)
        .def_rw("drawn_only",&EdgeBevelParams::drawnOnly)
        .def_rw("paint_radius",&EdgeBevelParams::paintRadius)
        .def_rw("paint_align",&EdgeBevelParams::paintAlign)
        .def_rw("paint_cover",&EdgeBevelParams::paintCover)
        .def_rw("paint_partial",&EdgeBevelParams::paintPartial)
        .def_rw("paint_taper",&EdgeBevelParams::paintTaper)
        .def_rw("max_turn",&EdgeBevelParams::maxTurn)
        .def_rw("profile",&EdgeBevelParams::profile)
        .def_rw("roundness",&EdgeBevelParams::roundness)
        .def_rw("fillet_top",&EdgeBevelParams::filletTop)
        .def_rw("fillet_side",&EdgeBevelParams::filletSide)
        .def_rw("size_mode",&EdgeBevelParams::sizeMode)
        .def_rw("size_top",&EdgeBevelParams::sizeTop)
        .def_rw("size_side",&EdgeBevelParams::sizeSide)
        .def_rw("max_share",&EdgeBevelParams::maxShare)
        .def_rw("taper",&EdgeBevelParams::taper)
        .def_rw("taper_flip",&EdgeBevelParams::taperFlip)
        .def_rw("round_corners",&EdgeBevelParams::roundCorners)
        .def_rw("wear_enable",&EdgeBevelParams::wearEnable)
        .def_rw("wear_uneven",&EdgeBevelParams::wearUneven)
        .def_rw("wear_uneven_scale",&EdgeBevelParams::wearUnevenScale)
        .def_rw("wear_rough",&EdgeBevelParams::wearRough)
        .def_rw("wear_chips",&EdgeBevelParams::wearChips)
        .def_rw("wear_chip_min",&EdgeBevelParams::wearChipMin)
        .def_rw("wear_chip_max",&EdgeBevelParams::wearChipMax)
        .def_rw("wear_chip_depth",&EdgeBevelParams::wearChipDepth)
        .def_rw("wear_chip_stretch",&EdgeBevelParams::wearChipStretch)
        .def_rw("wear_big",&EdgeBevelParams::wearBig)
        .def_rw("wear_mask_width",&EdgeBevelParams::wearMaskWidth)
        .def_rw("wear_mask_noise",&EdgeBevelParams::wearMaskNoise)
        .def_rw("wear_mask_scale",&EdgeBevelParams::wearMaskScale)
        .def_rw("up_x",&EdgeBevelParams::upX)
        .def_rw("up_y",&EdgeBevelParams::upY)
        .def_rw("up_z",&EdgeBevelParams::upZ)
        ;
    nb::class_<ScratchDrawParams>(mod,"ScratchDrawParams").def(nb::init<>())
        .def_rw("enable",&ScratchDrawParams::enable)
        .def_rw("seed",&ScratchDrawParams::seed)
        .def_rw("size_scale",&ScratchDrawParams::sizeScale)
        .def_rw("style",&ScratchDrawParams::style)
        .def_rw("mode", &ScratchDrawParams::mode)
        .def_rw("avoid",&ScratchDrawParams::avoid)
        .def_rw("detail",&ScratchDrawParams::detail)
        .def_rw("density",&ScratchDrawParams::density)
        .def_rw("max_scratches",&ScratchDrawParams::maxScratches)
        .def_rw("spread",&ScratchDrawParams::spread)
        .def_rw("falloff",&ScratchDrawParams::falloff)
        .def_rw("angle_jitter",&ScratchDrawParams::angleJitter)
        .def_rw("cross_share",&ScratchDrawParams::crossShare)
        .def_rw("along",&ScratchDrawParams::along)
        .def_rw("paint_snap",&ScratchDrawParams::paintSnap)
        .def_rw("length_min",&ScratchDrawParams::lengthMin)
        .def_rw("length_max",&ScratchDrawParams::lengthMax)
        .def_rw("length_bias",&ScratchDrawParams::lengthBias)
        .def_rw("width",&ScratchDrawParams::width)
        .def_rw("width_var",&ScratchDrawParams::widthVar)
        .def_rw("depth",&ScratchDrawParams::depth)
        .def_rw("depth_var",&ScratchDrawParams::depthVar)
        .def_rw("profile",&ScratchDrawParams::profile)
        .def_rw("taper",&ScratchDrawParams::taper)
        .def_rw("gouge",&ScratchDrawParams::gouge)
        .def_rw("curve",&ScratchDrawParams::curve)
        .def_rw("wobble",&ScratchDrawParams::wobble)
        .def_rw("gaps",&ScratchDrawParams::gaps)
        .def_rw("wrap_angle",&ScratchDrawParams::wrapAngle)
        .def_rw("bundle_chance",&ScratchDrawParams::bundleChance)
        .def_rw("bundle_min",&ScratchDrawParams::bundleMin)
        .def_rw("bundle_max",&ScratchDrawParams::bundleMax)
        .def_rw("bundle_gap",&ScratchDrawParams::bundleGap)
        .def_rw("bundle_thin",&ScratchDrawParams::bundleThin)
        .def_rw("scuff_enable",&ScratchDrawParams::scuffEnable)
        .def_rw("scuff_width",&ScratchDrawParams::scuffWidth)
        .def_rw("scuff_depth",&ScratchDrawParams::scuffDepth)
        .def_rw("mask_falloff",&ScratchDrawParams::maskFalloff)
        ;
    nb::class_<MetalDeformParams>(mod,"MetalDeformParams").def(nb::init<>())
        .def_rw("enable",&MetalDeformParams::enable)
        .def_rw("seed",&MetalDeformParams::seed)
        .def_rw("avoid",&MetalDeformParams::avoid)
        .def_rw("style",&MetalDeformParams::style)
        .def_rw("edge_length",&MetalDeformParams::edgeLength)
        .def_rw("detail",&MetalDeformParams::detail)
        .def_rw("max_tris",&MetalDeformParams::maxTris)
        .def_rw("depth_scale",&MetalDeformParams::depthScale)
        .def_rw("clearance",&MetalDeformParams::clearance)
        .def_rw("proximity",&MetalDeformParams::proximity)
        .def_rw("proximity_depth",&MetalDeformParams::proximityDepth)
        .def_rw("falloff",&MetalDeformParams::falloff)
        .def_rw("falloff_min",&MetalDeformParams::falloffMin)
        .def_rw("radius_max",&MetalDeformParams::radiusMax)
        .def_rw("profile",&MetalDeformParams::profile)
        .def_rw("bulge",&MetalDeformParams::bulge)
        .def_rw("sheet_thickness",&MetalDeformParams::sheetThickness)
        .def_rw("thickness_cap",&MetalDeformParams::thicknessCap)
        .def_rw("crease",&MetalDeformParams::crease)
        .def_rw("crease_spacing",&MetalDeformParams::creaseSpacing)
        .def_rw("crease_length",&MetalDeformParams::creaseLength)
        .def_rw("crease_sharp",&MetalDeformParams::creaseSharp)
        .def_rw("crease_branch",&MetalDeformParams::creaseBranch)
        .def_rw("crumple",&MetalDeformParams::crumple)
        .def_rw("crumple_scale",&MetalDeformParams::crumpleScale)
        .def_rw("roughness",&MetalDeformParams::roughness)
        .def_rw("crush_aim",&MetalDeformParams::crushAim)
        .def_rw("crush_proximity",&MetalDeformParams::crushProximity)
        .def_rw("crush_depth",&MetalDeformParams::crushDepth)
        .def_rw("crush_length",&MetalDeformParams::crushLength)
        .def_rw("crush_hinge",&MetalDeformParams::crushHinge)
        .def_rw("crush_skew",&MetalDeformParams::crushSkew)
        .def_rw("fold_size",&MetalDeformParams::foldSize)
        .def_rw("fold",&MetalDeformParams::fold)
        .def_rw("fold_sharp",&MetalDeformParams::foldSharp)
        .def_rw("fold_wander",&MetalDeformParams::foldWander)
        .def_rw("fold_lift",&MetalDeformParams::foldLift)
        .def_rw("crush_crumple",&MetalDeformParams::crushCrumple)
        .def_rw("crush_facet",&MetalDeformParams::crushFacet)
        .def_rw("impactors",&MetalDeformParams::impactors).def_rw("empties",&MetalDeformParams::empties)
        ;
    mod.def("edge_bevel_preset",&ApplyEdgeBevelPreset);
    nb::class_<DamagedBevelParams>(mod,"DamagedBevelParams").def(nb::init<>())                   
        .def_rw("enable",&DamagedBevelParams::enable)
        .def_rw("seed",&DamagedBevelParams::seed)
        .def_rw("avoid",&DamagedBevelParams::avoid)
        .def_rw("detail",&DamagedBevelParams::detail)
        .def_rw("angle",&DamagedBevelParams::angle)
        .def_rw("drawn_only",&DamagedBevelParams::drawnOnly)
        .def_rw("paint_radius",&DamagedBevelParams::paintRadius)
        .def_rw("paint_align",&DamagedBevelParams::paintAlign)
        .def_rw("paint_cover",&DamagedBevelParams::paintCover)
        .def_rw("max_turn",&DamagedBevelParams::maxTurn)
        .def_rw("width",&DamagedBevelParams::width)
        .def_rw("smoothness",&DamagedBevelParams::smoothness)
        .def_rw("uneven",&DamagedBevelParams::uneven)
        .def_rw("max_share",&DamagedBevelParams::maxShare)
        .def_rw("damage",&DamagedBevelParams::damage)
        .def_rw("damage_amount",&DamagedBevelParams::damageAmount)
        .def_rw("damage_factor",&DamagedBevelParams::damageFactor)
        .def_rw("damage_depth",&DamagedBevelParams::damageDepth)
        .def_rw("patch_length",&DamagedBevelParams::patchLength)
        .def_rw("chip_scatter",&DamagedBevelParams::chipScatter)
        .def_rw("jagged",&DamagedBevelParams::jagged)
        .def_rw("mask_width",&DamagedBevelParams::maskWidth)
        .def_rw("mask_noise",&DamagedBevelParams::maskNoise)
        .def_rw("mask_scale",&DamagedBevelParams::maskScale)
        .def_rw("up_x",&DamagedBevelParams::upX)
        .def_rw("up_y",&DamagedBevelParams::upY)
        .def_rw("up_z",&DamagedBevelParams::upZ)
        .def_rw("placement_random",&DamagedBevelParams::placementRandom)
        .def_rw("smooth_damage",&DamagedBevelParams::smoothDamage)
        .def_rw("smooth_damage_amount",&DamagedBevelParams::smoothDamageAmount)
        .def_rw("export_segments",&DamagedBevelParams::exportSegments)
        .def_rw("profile_shape",&DamagedBevelParams::profileShape)                                  
        ;
    mod.def("damaged_bevel_preset",&ApplyDamagedBevelPreset);
    mod.def("scratch_draw_preset",&ScratchDrawPreset);

    nb::class_<SurfaceDamagePoint>(mod,"SurfaceDamagePoint").def(nb::init<>())
        .def_rw("x",&SurfaceDamagePoint::x).def_rw("y",&SurfaceDamagePoint::y).def_rw("z",&SurfaceDamagePoint::z)
        .def_rw("nx",&SurfaceDamagePoint::nx).def_rw("ny",&SurfaceDamagePoint::ny).def_rw("nz",&SurfaceDamagePoint::nz)
        .def_rw("radius",&SurfaceDamagePoint::radius)
        .def_rw("mesh_points",&SurfaceDamagePoint::mesh_points).def_rw("mesh_triangles",&SurfaceDamagePoint::mesh_triangles);
    nb::class_<BrickDamageParams>(mod, "BrickDamageParams").def(nb::init<>())
        .def_rw("enable", &BrickDamageParams::enable)
        .def_rw("generated_geometry", &BrickDamageParams::generatedGeometry)
        .def_rw("edge_wall_depth", &BrickDamageParams::edgeWallDepth)
        .def_rw("organic", &BrickDamageParams::organic)
        .def_rw("proximity", &BrickDamageParams::proximity)
        .def_rw("seed", &BrickDamageParams::seed)
        .def_rw("mode", &BrickDamageParams::mode)
        .def_rw("avoid", &BrickDamageParams::avoid)
        .def_rw("detail", &BrickDamageParams::detail)
        .def_rw("amount", &BrickDamageParams::amount)
        .def_rw("size_min", &BrickDamageParams::sizeMin)
        .def_rw("size_max", &BrickDamageParams::sizeMax)
        .def_rw("lobes", &BrickDamageParams::lobes)
        .def_rw("stretch", &BrickDamageParams::stretch)
        .def_rw("edge_bias", &BrickDamageParams::edgeBias)
        .def_rw("paint_radius", &BrickDamageParams::paintRadius)
        .def_rw("paint_snap", &BrickDamageParams::paintSnap)
        .def_rw("paint_close", &BrickDamageParams::paintClose)
        .def_rw("render_min", &BrickDamageParams::renderMin)
        .def_rw("render_max", &BrickDamageParams::renderMax)
        .def_rw("jagged", &BrickDamageParams::jagged)
        .def_rw("outline_scale", &BrickDamageParams::outlineScale)
        .def_rw("crumble", &BrickDamageParams::crumble)
        .def_rw("crumble_size", &BrickDamageParams::crumbleSize)
        .def_rw("profile", &BrickDamageParams::profile)
		.def_rw("smooth_width", &BrickDamageParams::smoothWidth)
		.def_rw("terraces", &BrickDamageParams::terraces)
        .def_rw("terrace_skin", &BrickDamageParams::terraceSkin)
        .def_rw("terrace_inset", &BrickDamageParams::terraceInset)
        .def_rw("wall_soft", &BrickDamageParams::wallSoft)
        .def_rw("roughness", &BrickDamageParams::roughness)
        .def_rw("render_stick", &BrickDamageParams::renderStick)
        .def_rw("thickness_cap", &BrickDamageParams::thicknessCap)
        .def_rw("split_materials", &BrickDamageParams::splitMaterials)
        .def_rw("sharp_edge", &BrickDamageParams::sharpEdge)
        .def_rw("behind", &BrickDamageParams::behind)
        .def_rw("damage_depth", &BrickDamageParams::damageDepth)
        .def_rw("damage_noise", &BrickDamageParams::damageNoise)
        .def_rw("brick_mode", &BrickDamageParams::brickMode)
        .def_rw("corners", &BrickDamageParams::corners)
        .def_rw("joint_fill", &BrickDamageParams::jointFill)
        .def_rw("remesh", &BrickDamageParams::remesh)
        .def_rw("remesh_size", &BrickDamageParams::remeshSize)
        .def_rw("damage_scale", &BrickDamageParams::damageScale)
        .def_rw("style", &BrickDamageParams::style)
        .def_rw("bond", &BrickDamageParams::bond)
        .def_rw("brick_length", &BrickDamageParams::brickLength)
        .def_rw("brick_height", &BrickDamageParams::brickHeight)
        .def_rw("brick_width", &BrickDamageParams::brickWidth)
        .def_rw("mortar", &BrickDamageParams::mortar)
        .def_rw("mortar_depth", &BrickDamageParams::mortarDepth)
        .def_rw("brick_scale", &BrickDamageParams::brickScale)
        .def_rw("bond_offset", &BrickDamageParams::bondOffset)
        .def_rw("face_jitter", &BrickDamageParams::faceJitter)
        .def_rw("face_tilt", &BrickDamageParams::faceTilt)
        .def_rw("brick_rough", &BrickDamageParams::brickRough)
        .def_rw("chip", &BrickDamageParams::chip)
        .def_rw("chip_size", &BrickDamageParams::chipSize)
        .def_rw("joint_rough", &BrickDamageParams::jointRough)
        .def_rw("layers", &BrickDamageParams::layers)
        .def_rw("break_size", &BrickDamageParams::breakSize)
        .def_rw("layer_inset", &BrickDamageParams::layerInset)
        .def_rw("break_jagged", &BrickDamageParams::breakJagged)
        .def_rw("missing", &BrickDamageParams::missing)
        .def_rw("broken", &BrickDamageParams::broken)
        .def_rw("broken_rough", &BrickDamageParams::brokenRough)
        .def_rw("collar_fill", &BrickDamageParams::collarFill)
        .def_rw("loose", &BrickDamageParams::loose)
        .def_rw("loose_depth", &BrickDamageParams::looseDepth)
        .def_rw("loose_tilt", &BrickDamageParams::looseTilt)
        .def_rw("loose_out", &BrickDamageParams::looseOut)
        .def_rw("loose_out_dist", &BrickDamageParams::looseOutDist)
        .def_rw("hole_enable", &BrickDamageParams::holeEnable)
        .def_rw("hole_chance", &BrickDamageParams::holeChance)
        .def_rw("hole_size", &BrickDamageParams::holeSize)
        .def_rw("crack_enable", &BrickDamageParams::crackEnable)
        .def_rw("crack_amount", &BrickDamageParams::crackAmount)
        .def_rw("crack_length_min", &BrickDamageParams::crackLengthMin)
        .def_rw("crack_length_max", &BrickDamageParams::crackLengthMax)
        .def_rw("crack_width", &BrickDamageParams::crackWidth)
        .def_rw("crack_depth", &BrickDamageParams::crackDepth)
        .def_rw("crack_meander", &BrickDamageParams::crackMeander)
        .def_rw("crack_branch", &BrickDamageParams::crackBranch)
        .def_rw("crack_interior", &BrickDamageParams::crackInterior)
        .def_rw("up_x", &BrickDamageParams::upX)
		.def_rw("surf_mode", &BrickDamageParams::surfMode)
		.def_rw("tex_path", &BrickDamageParams::texPath)
		.def_rw("tex_scale", &BrickDamageParams::texScale)
		.def_rw("noise_scale", &BrickDamageParams::noiseScale)
		.def_rw("noise_detail", &BrickDamageParams::noiseDetail)
		.def_rw("noise_roughness", &BrickDamageParams::noiseRoughness)
		.def_rw("noise_distortion", &BrickDamageParams::noiseDistortion)
		.def_rw("noise_strength", &BrickDamageParams::noiseStrength)
		.def_rw("tex_strength", &BrickDamageParams::texStrength)
		.def_rw("tex_offset_x", &BrickDamageParams::texOffX)
		.def_rw("tex_offset_y", &BrickDamageParams::texOffY)
		.def_rw("tex_offset_z", &BrickDamageParams::texOffZ)
		.def_rw("tex_edge", &BrickDamageParams::texEdge)
        .def_rw("up_y", &BrickDamageParams::upY)
        .def_rw("up_z", &BrickDamageParams::upZ)
        ;
    nb::class_<BrokenGlassParams>(mod, "BrokenGlassParams").def(nb::init<>())
		.def_rw("through", &BrokenGlassParams::through)
		.def_rw("through_gap", &BrokenGlassParams::throughGap)
		.def_rw("through_rings", &BrokenGlassParams::throughRings)
		.def_rw("through_fall", &BrokenGlassParams::throughFall)
		.def_rw("through_min_piece", &BrokenGlassParams::throughMinPiece)
		.def_rw("fill_hole", &BrokenGlassParams::fillHole)
		.def_rw("fill_count", &BrokenGlassParams::fillCount)
		.def_rw("fill_center", &BrokenGlassParams::fillCenter)
        .def_rw("fill_origin_enabled", &BrokenGlassParams::fillOriginEnabled)
        .def_rw("fill_origin_x", &BrokenGlassParams::fillOriginX)
        .def_rw("fill_origin_y", &BrokenGlassParams::fillOriginY)
        .def_rw("fill_origin_z", &BrokenGlassParams::fillOriginZ)
        .def_rw("enable", &BrokenGlassParams::enable)
        .def_rw("seed", &BrokenGlassParams::seed)
        .def_rw("thickness", &BrokenGlassParams::thickness)
        .def_rw("skin_depth", &BrokenGlassParams::skinDepth)
        .def_rw("thickness_logic", &BrokenGlassParams::thicknessLogic)
        .def_rw("auto_hole", &BrokenGlassParams::autoHole)
        .def_rw("auto_hole_thickness", &BrokenGlassParams::autoHoleThickness)
        .def_rw("cut_margin", &BrokenGlassParams::cutMargin)
        .def_rw("detail", &BrokenGlassParams::detail)
        .def_rw("avoid", &BrokenGlassParams::avoid)
        .def_rw("impact_enable", &BrokenGlassParams::impactEnable)
        .def_rw("impact_amount", &BrokenGlassParams::impactAmount)
        .def_rw("hole_chance", &BrokenGlassParams::holeChance)
        .def_rw("hole_size_min", &BrokenGlassParams::holeSizeMin)
        .def_rw("hole_size_max", &BrokenGlassParams::holeSizeMax)
        .def_rw("hole_lobes", &BrokenGlassParams::holeLobes)
        .def_rw("impact_spacing", &BrokenGlassParams::impactSpacing)
        .def_rw("radial_min", &BrokenGlassParams::radialMin)
        .def_rw("radial_max", &BrokenGlassParams::radialMax)
        .def_rw("radial_jitter", &BrokenGlassParams::radialJitter)
        .def_rw("spike_chance", &BrokenGlassParams::spikeChance)
        .def_rw("spike_length", &BrokenGlassParams::spikeLength)
        .def_rw("spike_width", &BrokenGlassParams::spikeWidth)
        .def_rw("spike_split", &BrokenGlassParams::spikeSplit)
        .def_rw("spike_lean", &BrokenGlassParams::spikeLean)
        .def_rw("notch_jitter", &BrokenGlassParams::notchJitter)
        .def_rw("blowout", &BrokenGlassParams::blowout)
        .def_rw("blowout_reach", &BrokenGlassParams::blowoutReach)
        .def_rw("edge_kink", &BrokenGlassParams::edgeKink)
        .def_rw("shard_crack", &BrokenGlassParams::shardCrack)
        .def_rw("crack_enable", &BrokenGlassParams::crackEnable)
        .def_rw("crack_width", &BrokenGlassParams::crackWidth)
        .def_rw("crack_depth", &BrokenGlassParams::crackDepth)
        .def_rw("crack_both_sides", &BrokenGlassParams::crackBothSides)
        .def_rw("crack_reach", &BrokenGlassParams::crackReach)
        .def_rw("crack_reach_jitter", &BrokenGlassParams::crackReachJitter)
        .def_rw("crack_meander", &BrokenGlassParams::crackMeander)
        .def_rw("crack_branch", &BrokenGlassParams::crackBranch)
        .def_rw("crack_taper", &BrokenGlassParams::crackTaper)
        .def_rw("crush_size", &BrokenGlassParams::crushSize)
        .def_rw("ring_count", &BrokenGlassParams::ringCount)
        .def_rw("ring_first", &BrokenGlassParams::ringFirst)
        .def_rw("ring_growth", &BrokenGlassParams::ringGrowth)
        .def_rw("ring_fill", &BrokenGlassParams::ringFill)
        .def_rw("ring_jitter", &BrokenGlassParams::ringJitter)
        .def_rw("ring_bow", &BrokenGlassParams::ringBow)
        .def_rw("chip_enable", &BrokenGlassParams::chipEnable)
        .def_rw("chip_chance", &BrokenGlassParams::chipChance)
        .def_rw("chip_spacing", &BrokenGlassParams::chipSpacing)
        .def_rw("chip_size", &BrokenGlassParams::chipSize)
        .def_rw("chip_depth", &BrokenGlassParams::chipDepth)
        .def_rw("paint_holes_enable", &BrokenGlassParams::paintHolesEnable)
        .def_rw("paint_impacts_enable", &BrokenGlassParams::paintImpactsEnable)
        .def_rw("paint_only", &BrokenGlassParams::paintOnly)
        .def_rw("paint_shard_width", &BrokenGlassParams::paintShardWidth)
        .def_rw("paint_hole_cracks", &BrokenGlassParams::paintHoleCracks)
        .def_rw("paint_point_size", &BrokenGlassParams::paintPointSize)
        .def_rw("paint_impact_size", &BrokenGlassParams::paintImpactSize)
        .def_rw("paint_point_hole", &BrokenGlassParams::paintPointHole)
        .def_rw("max_cracks", &BrokenGlassParams::maxCracks)
        ;
    nb::class_<RustyHoleParams>(mod, "RustyHoleParams").def(nb::init<>())
        .def_rw("enable", &RustyHoleParams::enable)
        .def_rw("seed", &RustyHoleParams::seed)
        .def_rw("mode", &RustyHoleParams::mode)
        .def_rw("avoid", &RustyHoleParams::avoid)
        .def_rw("detail", &RustyHoleParams::detail)
        .def_rw("amount", &RustyHoleParams::amount)
        .def_rw("size_min", &RustyHoleParams::sizeMin)
        .def_rw("size_max", &RustyHoleParams::sizeMax)
        .def_rw("size_bias", &RustyHoleParams::sizeBias)
        .def_rw("min_spacing", &RustyHoleParams::minSpacing)
        .def_rw("lobes", &RustyHoleParams::lobes)
        .def_rw("lobe_spread", &RustyHoleParams::lobeSpread)
        .def_rw("jagged", &RustyHoleParams::jagged)
        .def_rw("roughness", &RustyHoleParams::roughness)
        .def_rw("scallop", &RustyHoleParams::scallop)
        .def_rw("merge", &RustyHoleParams::merge)
        .def_rw("lace", &RustyHoleParams::lace)
        .def_rw("pit_amount", &RustyHoleParams::pitAmount)
        .def_rw("pit_size", &RustyHoleParams::pitSize)
        .def_rw("pit_spread", &RustyHoleParams::pitSpread)
        .def_rw("crack_amount", &RustyHoleParams::crackAmount)
        .def_rw("crack_length", &RustyHoleParams::crackLength)
        .def_rw("crack_width", &RustyHoleParams::crackWidth)
        .def_rw("crack_meander", &RustyHoleParams::crackMeander)
        .def_rw("crack_fork", &RustyHoleParams::crackFork)
        .def_rw("crack_pits", &RustyHoleParams::crackPits)
        .def_rw("flow", &RustyHoleParams::flow)
        .def_rw("stretch", &RustyHoleParams::stretch)
        .def_rw("down_x", &RustyHoleParams::downX)
        .def_rw("down_y", &RustyHoleParams::downY)
        .def_rw("down_z", &RustyHoleParams::downZ)
        .def_rw("thickness", &RustyHoleParams::thickness)
        .def_rw("min_bridge", &RustyHoleParams::minBridge)
        .def_rw("paint_size", &RustyHoleParams::paintSize)
        .def_rw("paint_spacing", &RustyHoleParams::paintSpacing)
        .def_rw("paint_gaps", &RustyHoleParams::paintGaps)
        .def_rw("paint_crack", &RustyHoleParams::paintCrack)
        .def_rw("paint_snap", &RustyHoleParams::paintSnap)
        .def_rw("paint_close", &RustyHoleParams::paintClose)
        .def_rw("paint_single", &RustyHoleParams::paintSingle)
        ;
    nb::class_<GroundFissureParams>(mod, "GroundFissureParams").def(nb::init<>())
        .def_rw("enable", &GroundFissureParams::enable)
        .def_rw("generated_geometry", &GroundFissureParams::generatedGeometry)
        .def_rw("seed", &GroundFissureParams::seed)
        .def_rw("avoid", &GroundFissureParams::avoid)
        .def_rw("detail", &GroundFissureParams::detail)
        .def_rw("up_x", &GroundFissureParams::upX)
        .def_rw("up_y", &GroundFissureParams::upY)
        .def_rw("up_z", &GroundFissureParams::upZ)
        .def_rw("width", &GroundFissureParams::width)
        .def_rw("width_random", &GroundFissureParams::widthRandom)
        .def_rw("depth", &GroundFissureParams::depth)
        .def_rw("depth_random", &GroundFissureParams::depthRandom)
        .def_rw("taper", &GroundFissureParams::taper)
        .def_rw("jagged", &GroundFissureParams::jagged)
        .def_rw("edge_scale", &GroundFissureParams::edgeScale)
        .def_rw("wall_lean", &GroundFissureParams::wallLean)
        .def_rw("wall_rough", &GroundFissureParams::wallRough)
        .def_rw("floor_shape", &GroundFissureParams::floorShape)
        .def_rw("thickness_cap", &GroundFissureParams::thicknessCap)
        .def_rw("slab_enable", &GroundFissureParams::slabEnable)
        .def_rw("slab_rows", &GroundFissureParams::slabRows)
        .def_rw("slab_length_min", &GroundFissureParams::slabLengthMin)
        .def_rw("slab_length_max", &GroundFissureParams::slabLengthMax)
        .def_rw("slab_width_min", &GroundFissureParams::slabWidthMin)
        .def_rw("slab_width_max", &GroundFissureParams::slabWidthMax)
        .def_rw("slab_row_falloff", &GroundFissureParams::slabRowFalloff)
        .def_rw("slab_thick_min", &GroundFissureParams::slabThickMin)
        .def_rw("slab_thick_max", &GroundFissureParams::slabThickMax)
        .def_rw("tilt_min", &GroundFissureParams::tiltMin)
        .def_rw("tilt_max", &GroundFissureParams::tiltMax)
        .def_rw("sink_chance", &GroundFissureParams::sinkChance)
        .def_rw("roll", &GroundFissureParams::roll)
        .def_rw("flat_chance", &GroundFissureParams::flatChance)
        .def_rw("side_bias", &GroundFissureParams::sideBias)
        .def_rw("slab_gap", &GroundFissureParams::slabGap)
        .def_rw("slab_jagged", &GroundFissureParams::slabJagged)
        .def_rw("slab_skew", &GroundFissureParams::slabSkew)
        .def_rw("branch_enable", &GroundFissureParams::branchEnable)
        .def_rw("branch_density", &GroundFissureParams::branchDensity)
        .def_rw("branch_length_min", &GroundFissureParams::branchLengthMin)
        .def_rw("branch_length_max", &GroundFissureParams::branchLengthMax)
        .def_rw("branch_width", &GroundFissureParams::branchWidth)
        .def_rw("branch_depth", &GroundFissureParams::branchDepth)
        .def_rw("branch_meander", &GroundFissureParams::branchMeander)
        .def_rw("branch_angle", &GroundFissureParams::branchAngle)
        .def_rw("branch_fork", &GroundFissureParams::branchFork)
        .def_rw("rubble_enable", &GroundFissureParams::rubbleEnable)
        .def_rw("rubble_density", &GroundFissureParams::rubbleDensity)
        .def_rw("rubble_size_min", &GroundFissureParams::rubbleSizeMin)
        .def_rw("rubble_size_max", &GroundFissureParams::rubbleSizeMax)
        .def_rw("rubble_wedged", &GroundFissureParams::rubbleWedged)
		.def_rw("realistic", &GroundFissureParams::realistic)
		.def_rw("edge_damage", &GroundFissureParams::edgeDamage)
		.def_rw("face_rough", &GroundFissureParams::faceRough)
		.def_rw("surface_cracks", &GroundFissureParams::surfaceCracks)
		.def_rw("debris", &GroundFissureParams::debris)
		.def_rw("debris_size", &GroundFissureParams::debrisSize)
		.def_rw("split_chance", &GroundFissureParams::splitChance)
        ;
    mod.def("bullet_impact_preset", &BulletImpactPreset);


    nb::class_<ConcreteHitEmpty>(mod, "ConcreteHitEmpty")
        .def(nb::init<>())
        .def_prop_rw("rel", [](const ConcreteHitEmpty& e) { return std::vector<double>(e.rel, e.rel + 12); },
            [](ConcreteHitEmpty& e, const std::vector<double>& v) {
                if (v.size() != 12) throw nb::value_error("rel requires 12 finite values");
                for (double x : v) if (!std::isfinite(x)) throw nb::value_error("rel must be finite");
                std::copy(v.begin(), v.end(), e.rel);
            })
        .def_rw("size", &ConcreteHitEmpty::size)
        .def_rw("strength", &ConcreteHitEmpty::strength);
	nb::class_<FabricTearParams>(mod, "FabricTearParams").def(nb::init<>())
		.def_rw("enable", &FabricTearParams::enable)
		.def_rw("seed", &FabricTearParams::seed)
		.def_rw("flat", &FabricTearParams::flat)
		.def_rw("solid", &FabricTearParams::solid)
		.def_rw("solid_bevel", &FabricTearParams::solidBevel)
		.def_rw("solid_bevel_segments", &FabricTearParams::solidBevelSegments)
		.def_rw("woven_threads", &FabricTearParams::wovenThreads)
		.def_rw("tear_style", &FabricTearParams::tearStyle)
        .def_rw("distribution_scale", &FabricTearParams::distributionScale)
        .def_rw("distribution_strength", &FabricTearParams::distributionStrength)
        .def_rw("shape_noise_scale", &FabricTearParams::shapeNoiseScale)
        .def_rw("shape_noise_strength", &FabricTearParams::shapeNoiseStrength)
		.def_rw("tear_rip", &FabricTearParams::tearRip)
		.def_rw("tear_corner", &FabricTearParams::tearCorner)
		.def_rw("tear_worn", &FabricTearParams::tearWorn)
		.def_rw("tear_puncture", &FabricTearParams::tearPuncture)
		.def_rw("grain_snap", &FabricTearParams::grainSnap)
		.def_rw("yarn_size", &FabricTearParams::yarnSize)
		.def_rw("flap_lift", &FabricTearParams::flapLift)
		.def_rw("layer_depth", &FabricTearParams::layerDepth)
		.def_rw("thickness", &FabricTearParams::thickness)
		.def_rw("skin_depth", &FabricTearParams::skinDepth)
		.def_rw("cut_margin", &FabricTearParams::cutMargin)
		.def_rw("detail", &FabricTearParams::detail)
		.def_rw("avoid", &FabricTearParams::avoid)
		.def_rw("holes_enable", &FabricTearParams::holesEnable)
		.def_rw("hole_amount", &FabricTearParams::holeAmount)
		.def_rw("hole_length_min", &FabricTearParams::holeLengthMin)
		.def_rw("hole_length_max", &FabricTearParams::holeLengthMax)
		.def_rw("hole_width_min", &FabricTearParams::holeWidthMin)
		.def_rw("hole_width_max", &FabricTearParams::holeWidthMax)
		.def_rw("hole_jagged", &FabricTearParams::holeJagged)
		.def_rw("hole_lobes", &FabricTearParams::holeLobes)
		.def_rw("hole_meander", &FabricTearParams::holeMeander)
		.def_rw("hole_split_max", &FabricTearParams::holeSplitMax)
		.def_rw("hole_bridge", &FabricTearParams::holeBridge)
		.def_rw("hole_dir_mode", &FabricTearParams::holeDirMode)
		.def_rw("hole_dir_x", &FabricTearParams::holeDirX)
		.def_rw("hole_dir_y", &FabricTearParams::holeDirY)
		.def_rw("hole_dir_z", &FabricTearParams::holeDirZ)
		.def_rw("hole_dir_jitter", &FabricTearParams::holeDirJitter)
		.def_rw("edge_enable", &FabricTearParams::edgeEnable)
		.def_rw("edge_amount", &FabricTearParams::edgeAmount)
		.def_rw("edge_spacing_jitter", &FabricTearParams::edgeSpacingJitter)
		.def_rw("edge_length_min", &FabricTearParams::edgeLengthMin)
		.def_rw("edge_length_max", &FabricTearParams::edgeLengthMax)
		.def_rw("edge_width_min", &FabricTearParams::edgeWidthMin)
		.def_rw("edge_width_max", &FabricTearParams::edgeWidthMax)
		.def_rw("edge_taper", &FabricTearParams::edgeTaper)
		.def_rw("edge_jagged", &FabricTearParams::edgeJagged)
		.def_rw("edge_meander", &FabricTearParams::edgeMeander)
		.def_rw("edge_angle_jitter", &FabricTearParams::edgeAngleJitter)
		.def_rw("threads_enable", &FabricTearParams::threadsEnable)
		.def_rw("thread_radius", &FabricTearParams::threadRadius)
		.def_rw("thread_sides", &FabricTearParams::threadSides)
		.def_rw("thread_spacing", &FabricTearParams::threadSpacing)
		.def_rw("bridge_enable", &FabricTearParams::bridgeEnable)
		.def_rw("bridge_share", &FabricTearParams::bridgeShare)
		.def_rw("thread_sag", &FabricTearParams::threadSag)
		.def_rw("thread_skew", &FabricTearParams::threadSkew)
		.def_rw("thread_wobble", &FabricTearParams::threadWobble)
		.def_rw("broken_enable", &FabricTearParams::brokenEnable)
		.def_rw("broken_share", &FabricTearParams::brokenShare)
		.def_rw("thread_droop", &FabricTearParams::threadDroop)
		.def_rw("thread_curl", &FabricTearParams::threadCurl)
		.def_rw("thread_taper", &FabricTearParams::threadTaper)
		.def_rw("fray_enable", &FabricTearParams::frayEnable)
		.def_rw("fray_spacing", &FabricTearParams::fraySpacing)
		.def_rw("fray_length_min", &FabricTearParams::frayLengthMin)
		.def_rw("fray_length_max", &FabricTearParams::frayLengthMax)
		.def_rw("hang_enable", &FabricTearParams::hangEnable)
		.def_rw("hang_amount", &FabricTearParams::hangAmount)
		.def_rw("hang_length_min", &FabricTearParams::hangLengthMin)
		.def_rw("hang_length_max", &FabricTearParams::hangLengthMax)
		.def_rw("paint_holes_enable", &FabricTearParams::paintHolesEnable)
		.def_rw("paint_edges_enable", &FabricTearParams::paintEdgesEnable)
		.def_rw("paint_only", &FabricTearParams::paintOnly)
		.def_rw("paint_jagged", &FabricTearParams::paintJagged)
		.def_rw("paint_rag_size", &FabricTearParams::paintRagSize)
		.def_rw("paint_edge_spacing", &FabricTearParams::paintEdgeSpacing)
		.def_rw("paint_edge_length_min", &FabricTearParams::paintEdgeLengthMin)
		.def_rw("paint_edge_length_max", &FabricTearParams::paintEdgeLengthMax)
		.def_rw("paint_edge_width_min", &FabricTearParams::paintEdgeWidthMin)
		.def_rw("paint_edge_width_max", &FabricTearParams::paintEdgeWidthMax)
		.def_rw("paint_hang_spacing", &FabricTearParams::paintHangSpacing)
		.def_rw("gravity_x", &FabricTearParams::gravityX)
		.def_rw("gravity_y", &FabricTearParams::gravityY)
		.def_rw("gravity_z", &FabricTearParams::gravityZ)
		.def_rw("max_threads", &FabricTearParams::maxThreads)
		;
	nb::class_<ConcreteDamageParams>(mod, "ConcreteDamageParams").def(nb::init<>())
		.def_rw("enable", &ConcreteDamageParams::enable)
        .def_rw("generated_geometry", &ConcreteDamageParams::generatedGeometry)
        .def_rw("edge_wall_depth", &ConcreteDamageParams::edgeWallDepth)
        .def_rw("organic", &ConcreteDamageParams::organic)
        .def_rw("proximity", &ConcreteDamageParams::proximity)
		.def_rw("seed", &ConcreteDamageParams::seed)
		.def_rw("mode", &ConcreteDamageParams::mode)
		.def_rw("avoid", &ConcreteDamageParams::avoid)
		.def_rw("detail", &ConcreteDamageParams::detail)
		.def_rw("amount", &ConcreteDamageParams::amount)
		.def_rw("size_min", &ConcreteDamageParams::sizeMin)
		.def_rw("size_max", &ConcreteDamageParams::sizeMax)
		.def_rw("lobes", &ConcreteDamageParams::lobes)
		.def_rw("stretch", &ConcreteDamageParams::stretch)
		.def_rw("edge_bias", &ConcreteDamageParams::edgeBias)
		.def_rw("paint_radius", &ConcreteDamageParams::paintRadius)
		.def_rw("paint_snap", &ConcreteDamageParams::paintSnap)
		.def_rw("paint_close", &ConcreteDamageParams::paintClose)
		.def_rw("depth_min", &ConcreteDamageParams::depthMin)
		.def_rw("depth_max", &ConcreteDamageParams::depthMax)
		.def_rw("jagged", &ConcreteDamageParams::jagged)
		.def_rw("outline_scale", &ConcreteDamageParams::outlineScale)
		.def_rw("terraces", &ConcreteDamageParams::terraces)
		.def_rw("terrace_skin", &ConcreteDamageParams::terraceSkin)
		.def_rw("terrace_inset", &ConcreteDamageParams::terraceInset)
		.def_rw("wall_soft", &ConcreteDamageParams::wallSoft)
		.def_rw("roughness", &ConcreteDamageParams::roughness)
		.def_rw("aggregate_size", &ConcreteDamageParams::aggregateSize)
		.def_rw("surface_detail", &ConcreteDamageParams::surfaceDetail)
		.def_rw("surface_distortion", &ConcreteDamageParams::surfaceDistortion)
		.def_rw("surface_roughness", &ConcreteDamageParams::surfaceRoughness)
		.def_rw("surface_strength", &ConcreteDamageParams::surfaceStrength)
		.def_rw("profile", &ConcreteDamageParams::profile)
		.def_rw("smooth_width", &ConcreteDamageParams::smoothWidth)
        .def_rw("wood_thickness", &ConcreteDamageParams::woodThickness)
        .def_rw("wood_width", &ConcreteDamageParams::woodWidth)
        .def_rw("wood_splinter", &ConcreteDamageParams::woodSplinter)
        .def_rw("wood_angle", &ConcreteDamageParams::woodAngle)
		.def_rw("surf_mode", &ConcreteDamageParams::surfMode)
		.def_rw("tex_path", &ConcreteDamageParams::texPath)
		.def_rw("tex_scale", &ConcreteDamageParams::texScale)
		.def_rw("tex_strength", &ConcreteDamageParams::texStrength)
		.def_rw("tex_offset_x", &ConcreteDamageParams::texOffX)
		.def_rw("tex_offset_y", &ConcreteDamageParams::texOffY)
		.def_rw("tex_offset_z", &ConcreteDamageParams::texOffZ)
		.def_rw("tex_edge", &ConcreteDamageParams::texEdge)
		.def_rw("thickness_cap", &ConcreteDamageParams::thicknessCap)
		.def_rw("hole_enable", &ConcreteDamageParams::holeEnable)
		.def_rw("hole_chance", &ConcreteDamageParams::holeChance)
		.def_rw("hole_size", &ConcreteDamageParams::holeSize)
		.def_rw("hole_jagged", &ConcreteDamageParams::holeJagged)
		.def_rw("crack_enable", &ConcreteDamageParams::crackEnable)
		.def_rw("crack_amount", &ConcreteDamageParams::crackAmount)
		.def_rw("crack_length_min", &ConcreteDamageParams::crackLengthMin)
		.def_rw("crack_length_max", &ConcreteDamageParams::crackLengthMax)
		.def_rw("crack_width", &ConcreteDamageParams::crackWidth)
		.def_rw("crack_depth", &ConcreteDamageParams::crackDepth)
		.def_rw("crack_meander", &ConcreteDamageParams::crackMeander)
		.def_rw("crack_branch", &ConcreteDamageParams::crackBranch)
		.def_rw("crack_interior", &ConcreteDamageParams::crackInterior)
		.def_rw("rebar_enable", &ConcreteDamageParams::rebarEnable)
		.def_rw("rebar_amount", &ConcreteDamageParams::rebarAmount)
		.def_rw("rebar_density", &ConcreteDamageParams::rebarDensity)
		.def_rw("rebar_radius", &ConcreteDamageParams::rebarRadius)
		.def_rw("rebar_scale", &ConcreteDamageParams::rebarScale)
		.def_rw("rebar_spacing_x", &ConcreteDamageParams::rebarSpacingX)
		.def_rw("rebar_spacing_y", &ConcreteDamageParams::rebarSpacingY)
		.def_rw("rebar_bars_x", &ConcreteDamageParams::rebarBarsX)
		.def_rw("rebar_bars_y", &ConcreteDamageParams::rebarBarsY)
		.def_rw("rebar_outer", &ConcreteDamageParams::rebarOuter)
		.def_rw("rebar_angle", &ConcreteDamageParams::rebarAngle)
		.def_rw("rebar_cover", &ConcreteDamageParams::rebarCover)
		.def_rw("rebar_expose", &ConcreteDamageParams::rebarExpose)
		.def_rw("rebar_embed", &ConcreteDamageParams::rebarEmbed)
		.def_rw("rebar_ribs", &ConcreteDamageParams::rebarRibs)
		.def_rw("rebar_sides", &ConcreteDamageParams::rebarSides)
        .def_rw("rebar_resolution", &ConcreteDamageParams::rebarResolution)
		.def_rw("rebar_jitter", &ConcreteDamageParams::rebarJitter)
		.def_rw("rebar_wobble", &ConcreteDamageParams::rebarWobble)
		.def_rw("rebar_bend", &ConcreteDamageParams::rebarBend)
		.def_rw("rebar_broken", &ConcreteDamageParams::rebarBroken)
		.def_rw("up_x", &ConcreteDamageParams::upX)
		.def_rw("up_y", &ConcreteDamageParams::upY)
		.def_rw("up_z", &ConcreteDamageParams::upZ)
		;
	nb::class_<DeformImpact>(mod, "DeformImpact").def(nb::init<>())
		.def_rw("x", &DeformImpact::x).def_rw("y", &DeformImpact::y).def_rw("z", &DeformImpact::z)
		.def_rw("radius", &DeformImpact::radius).def_rw("strength", &DeformImpact::strength)
		.def_rw("own_origin", &DeformImpact::ownOrigin)
		.def_rw("origin_x", &DeformImpact::ox).def_rw("origin_y", &DeformImpact::oy).def_rw("origin_z", &DeformImpact::oz);
	nb::class_<DeformBox>(mod, "DeformBox").def(nb::init<>())
		.def_rw("min_x", &DeformBox::minX).def_rw("min_y", &DeformBox::minY).def_rw("min_z", &DeformBox::minZ)
		.def_rw("max_x", &DeformBox::maxX).def_rw("max_y", &DeformBox::maxY).def_rw("max_z", &DeformBox::maxZ);
	nb::class_<ImpactDeformParams>(mod, "ImpactDeformParams").def(nb::init<>())
		.def_rw("enable", &ImpactDeformParams::enable).def_rw("strength", &ImpactDeformParams::strength)
		.def_rw("max_deform", &ImpactDeformParams::maxDeform).def_rw("total_amplitude", &ImpactDeformParams::totalAmplitude)
		.def_rw("noise_factor", &ImpactDeformParams::noiseFactor)
		.def_rw("noise_scale", &ImpactDeformParams::noiseScale).def_rw("smoothness", &ImpactDeformParams::smoothness)
		.def_rw("protect_weights", &ImpactDeformParams::protectWeights)
		.def_rw("origin_x", &ImpactDeformParams::originX).def_rw("origin_y", &ImpactDeformParams::originY).def_rw("origin_z", &ImpactDeformParams::originZ)
		.def_rw("groups_x", &ImpactDeformParams::groupsX).def_rw("groups_y", &ImpactDeformParams::groupsY).def_rw("groups_z", &ImpactDeformParams::groupsZ)
		.def_rw("curve", &ImpactDeformParams::curve).def_rw("impacts", &ImpactDeformParams::impacts)
		.def_rw("protection", &ImpactDeformParams::protection).def_rw("signature", &ImpactDeformParams::signature);
	nb::class_<WallHoleParams>(mod, "WallHoleParams").def(nb::init<>())
		.def_rw("enable", &WallHoleParams::enable)
		.def_rw("seed", &WallHoleParams::seed)
		.def_rw("sites", &WallHoleParams::sites)
		.def_rw("hole_width", &WallHoleParams::holeWidth)
		.def_rw("hole_height", &WallHoleParams::holeHeight)
		.def_rw("hole_lift", &WallHoleParams::holeLift)
		.def_rw("jagged", &WallHoleParams::jagged)
		.def_rw("edge_ragged", &WallHoleParams::edgeRagged)
		.def_rw("depth_variation", &WallHoleParams::depthVariation)
		.def_rw("brick_length", &WallHoleParams::brickLength)
		.def_rw("brick_height", &WallHoleParams::brickHeight)
		.def_rw("brick_width", &WallHoleParams::brickWidth)
		.def_rw("mortar", &WallHoleParams::mortar)
		.def_rw("bond", &WallHoleParams::bond)
		.def_rw("joint_depth", &WallHoleParams::jointDepth)
		.def_rw("plaster_thickness", &WallHoleParams::plasterThickness)
		.def_rw("plaster_band", &WallHoleParams::plasterBand)
		.def_rw("broken_chance", &WallHoleParams::brokenChance)
		.def_rw("custom_bricks", &WallHoleParams::customBricks)
		.def_rw("rubble_enable", &WallHoleParams::rubbleEnable)
		.def_rw("rubble_share", &WallHoleParams::rubbleShare)
		.def_rw("rubble_spread", &WallHoleParams::rubbleSpread)
		.def_rw("rubble_max", &WallHoleParams::rubbleMax)
		.def_rw("up_x", &WallHoleParams::upX)
		.def_rw("up_y", &WallHoleParams::upY)
		.def_rw("up_z", &WallHoleParams::upZ);
	nb::class_<HangHoleParams>(mod, "HangHoleParams").def(nb::init<>())
		.def_rw("enable", &HangHoleParams::enable)
        .def_rw("edge_wall_depth", &HangHoleParams::edgeWallDepth)
		.def_rw("seed", &HangHoleParams::seed)
		.def_rw("mode", &HangHoleParams::mode)
		.def_rw("avoid", &HangHoleParams::avoid)
		.def_rw("amount", &HangHoleParams::amount)
		.def_rw("thickness", &HangHoleParams::thickness)
		.def_rw("size_min", &HangHoleParams::sizeMin)
		.def_rw("size_max", &HangHoleParams::sizeMax)
		.def_rw("jagged", &HangHoleParams::jagged)
		.def_rw("flare", &HangHoleParams::flare)
		.def_rw("roughness", &HangHoleParams::roughness)
		.def_rw("surf_mode", &HangHoleParams::surfMode)
		.def_rw("tex_path", &HangHoleParams::texPath)
		.def_rw("tex_scale", &HangHoleParams::texScale)
		.def_rw("noise_scale", &HangHoleParams::noiseScale)
		.def_rw("noise_detail", &HangHoleParams::noiseDetail)
		.def_rw("noise_roughness", &HangHoleParams::noiseRoughness)
		.def_rw("noise_distortion", &HangHoleParams::noiseDistortion)
		.def_rw("noise_strength", &HangHoleParams::noiseStrength)
		.def_rw("tex_strength", &HangHoleParams::texStrength)
		.def_rw("tex_offset_x", &HangHoleParams::texOffX)
		.def_rw("tex_offset_y", &HangHoleParams::texOffY)
		.def_rw("tex_offset_z", &HangHoleParams::texOffZ)
		.def_rw("tex_edge", &HangHoleParams::texEdge)
		.def_rw("bars_enable", &HangHoleParams::barsEnable)
		.def_rw("bar_spacing", &HangHoleParams::barSpacing)
		.def_rw("bar_radius", &HangHoleParams::barRadius)
		.def_rw("bar_depth", &HangHoleParams::barDepth)
		.def_rw("bar_broken", &HangHoleParams::barBroken)
		.def_rw("droop", &HangHoleParams::droop)
		.def_rw("bar_bow", &HangHoleParams::barBow)
		.def_rw("cables_enable", &HangHoleParams::cablesEnable)
		.def_rw("cable_amount", &HangHoleParams::cableAmount)
		.def_rw("cable_radius", &HangHoleParams::cableRadius)
		.def_rw("cable_length", &HangHoleParams::cableLength)
		.def_rw("cable_sag", &HangHoleParams::cableSag)
		.def_rw("cable_loops", &HangHoleParams::cableLoops)
		.def_rw("gravity_x", &HangHoleParams::gravityX)
		.def_rw("gravity_y", &HangHoleParams::gravityY)
		.def_rw("gravity_z", &HangHoleParams::gravityZ)
		.def_rw("sides", &HangHoleParams::sides)
        .def_rw("rebar_resolution", &HangHoleParams::rebarResolution);
	nb::class_<CellProximity>(mod, "CellProximity").def(nb::init<>())
		.def_rw("x", &CellProximity::x).def_rw("y", &CellProximity::y).def_rw("z", &CellProximity::z)
		.def_rw("radius", &CellProximity::radius).def_rw("strength", &CellProximity::strength);
	nb::class_<CellFractureParams>(mod, "CellFractureParams").def(nb::init<>())
		.def_rw("enable", &CellFractureParams::enable).def_rw("seed", &CellFractureParams::seed)
		.def_rw("material", &CellFractureParams::material).def_rw("pieces", &CellFractureParams::pieces)
		.def_rw("max_pieces", &CellFractureParams::maxPieces).def_rw("gap", &CellFractureParams::gap)
		.def_rw("size_variation", &CellFractureParams::sizeVariation)
		.def_rw("roughness", &CellFractureParams::roughness).def_rw("rough_scale", &CellFractureParams::roughScale)
		.def_rw("detail", &CellFractureParams::detail).def_rw("planar", &CellFractureParams::planar)
		.def_rw("planar_ratio", &CellFractureParams::planarRatio)
		.def_rw("prox_radius", &CellFractureParams::proxRadius).def_rw("prox_density", &CellFractureParams::proxDensity)
		.def_rw("prox_falloff", &CellFractureParams::proxFalloff).def_rw("rubble", &CellFractureParams::rubble)
		.def_rw("keep_remainder", &CellFractureParams::keepRemainder).def_rw("remainder_reach", &CellFractureParams::remainderReach)
		.def_rw("proximity", &CellFractureParams::proximity)
		.def_rw("stroke_mode", &CellFractureParams::strokeMode).def_rw("stroke_spacing", &CellFractureParams::strokeSpacing)
        .def_rw("guidance_amount", &CellFractureParams::guidanceAmount)
        .def_rw("guidance_radius", &CellFractureParams::guidanceRadius)
        .def_rw("guidance_depth", &CellFractureParams::guidanceDepth)
        .def_rw("guidance_falloff", &CellFractureParams::guidanceFalloff)
        .def_rw("guidance_distance", &CellFractureParams::guidanceDistance)
		.def_rw("grain_x", &CellFractureParams::grainX).def_rw("grain_y", &CellFractureParams::grainY)
		.def_rw("grain_z", &CellFractureParams::grainZ).def_rw("grain_stretch", &CellFractureParams::grainStretch)
		.def_rw("splinter", &CellFractureParams::splinter)
		.def_rw("glass_spokes", &CellFractureParams::glassSpokes).def_rw("glass_ring_growth", &CellFractureParams::glassRingGrowth)
		.def_rw("glass_branch", &CellFractureParams::glassBranch).def_rw("glass_wander", &CellFractureParams::glassWander)
		.def_rw("glass_hole", &CellFractureParams::glassHole)
		.def_rw("inner_uv_scale", &CellFractureParams::innerUvScale).def_rw("smooth_angle", &CellFractureParams::smoothAngle);
	nb::class_<RBImpact>(mod, "RBImpact").def(nb::init<>())
		.def_rw("x", &RBImpact::x).def_rw("y", &RBImpact::y).def_rw("z", &RBImpact::z)
		.def_rw("radius", &RBImpact::radius).def_rw("strength", &RBImpact::strength);
	nb::class_<RBFractureParams>(mod, "RBFractureParams").def(nb::init<>())
		.def_rw("enable", &RBFractureParams::enable).def_rw("seed", &RBFractureParams::seed)
		.def_rw("material", &RBFractureParams::material).def_rw("pieces", &RBFractureParams::pieces)
		.def_rw("max_pieces", &RBFractureParams::maxPieces).def_rw("gap", &RBFractureParams::gap)
		.def_rw("size_variation", &RBFractureParams::sizeVariation).def_rw("min_piece", &RBFractureParams::minPiece)
		.def_rw("cut_spacing", &RBFractureParams::cutSpacing).def_rw("impact_radius", &RBFractureParams::impactRadius)
		.def_rw("impact_density", &RBFractureParams::impactDensity).def_rw("rubble", &RBFractureParams::rubble)
		.def_rw("impacts", &RBFractureParams::impacts)
		.def_rw("grain_x", &RBFractureParams::grainX).def_rw("grain_y", &RBFractureParams::grainY)
		.def_rw("grain_z", &RBFractureParams::grainZ).def_rw("grain_stretch", &RBFractureParams::grainStretch)
		.def_rw("weave_stretch", &RBFractureParams::weaveStretch).def_rw("glass_spokes", &RBFractureParams::glassSpokes)
		.def_rw("glass_ring_growth", &RBFractureParams::glassRingGrowth)
		.def_rw("inner_uv_scale", &RBFractureParams::innerUvScale).def_rw("smooth_angle", &RBFractureParams::smoothAngle);
	nb::class_<DamageDebrisParams>(mod, "DamageDebrisParams").def(nb::init<>())
		.def_rw("enable", &DamageDebrisParams::enable)
		.def_rw("seed", &DamageDebrisParams::seed)
		.def_rw("min_volume", &DamageDebrisParams::minVolume)
		.def_rw("min_thickness", &DamageDebrisParams::minThickness)
		.def_rw("chunk_size", &DamageDebrisParams::chunkSize)
		.def_rw("chunk_jitter", &DamageDebrisParams::chunkJitter)
		.def_rw("max_pieces", &DamageDebrisParams::maxPieces)
		.def_rw("inner_uv_scale", &DamageDebrisParams::innerUvScale)
		.def_rw("smooth_angle", &DamageDebrisParams::smoothAngle);
	nb::class_<ConcreteHitParams>(mod, "ConcreteHitParams").def(nb::init<>())
		.def_rw("enable", &ConcreteHitParams::enable)
		.def_rw("through", &ConcreteHitParams::through)
		.def_rw("through_gap", &ConcreteHitParams::throughGap)
		.def_rw("through_max_wall", &ConcreteHitParams::throughMaxWall)
		.def_rw("chunk_depth", &ConcreteHitParams::chunkDepth)
		.def_rw("through_remove", &ConcreteHitParams::throughRemove)
		.def_rw("through_min_piece", &ConcreteHitParams::throughMinPiece)
		.def_rw("seed", &ConcreteHitParams::seed)
		.def_rw("avoid", &ConcreteHitParams::avoid)
		.def_rw("detail", &ConcreteHitParams::detail)
		.def_rw("aim", &ConcreteHitParams::aim)
		.def_rw("radius_scale", &ConcreteHitParams::radiusScale)
		.def_rw("proximity", &ConcreteHitParams::proximity)
		.def_rw("prox_shrink", &ConcreteHitParams::proxShrink)
		.def_rw("oblique", &ConcreteHitParams::oblique)
		.def_rw("layer_thickness", &ConcreteHitParams::layerThickness)
		.def_rw("core_size", &ConcreteHitParams::coreSize)
		.def_rw("core_depth", &ConcreteHitParams::coreDepth)
		.def_rw("core_jagged", &ConcreteHitParams::coreJagged)
		.def_rw("roughness", &ConcreteHitParams::roughness)
		.def_rw("aggregate_size", &ConcreteHitParams::aggregateSize)
		.def_rw("thickness_cap", &ConcreteHitParams::thicknessCap)
		.def_rw("hole_enable", &ConcreteHitParams::holeEnable)
		.def_rw("hole_size", &ConcreteHitParams::holeSize)
		.def_rw("plate_size", &ConcreteHitParams::plateSize)
		.def_rw("plate_growth", &ConcreteHitParams::plateGrowth)
		.def_rw("zone_jagged", &ConcreteHitParams::zoneJagged)
		.def_rw("shatter", &ConcreteHitParams::shatter)
		.def_rw("tilt", &ConcreteHitParams::tilt)
		.def_rw("edge_jagged", &ConcreteHitParams::edgeJagged)
		.def_rw("chip", &ConcreteHitParams::chip)
		.def_rw("crack_enable", &ConcreteHitParams::crackEnable)
		.def_rw("crack_width", &ConcreteHitParams::crackWidth)
		.def_rw("crack_depth", &ConcreteHitParams::crackDepth)
		.def_rw("radial_count", &ConcreteHitParams::radialCount)
		.def_rw("radial_length_min", &ConcreteHitParams::radialLengthMin)
		.def_rw("radial_length_max", &ConcreteHitParams::radialLengthMax)
		.def_rw("radial_meander", &ConcreteHitParams::radialMeander)
		.def_rw("radial_branch", &ConcreteHitParams::radialBranch)
		.def_rw("ring_count", &ConcreteHitParams::ringCount)
		.def_rw("ring_fill", &ConcreteHitParams::ringFill)
		.def_rw("empties", &ConcreteHitParams::empties)
		;
	BindConcretePits(mod);
    BindMetalTear(mod);
    BindInnerMatched(mod);
    BindPeeling(mod);
    BindClay(mod);
	BindVoxelErosion(mod);
	nb::class_<PlasterDisplaceParams>(mod, "PlasterDisplaceParams").def(nb::init<>())
		.def_rw("zone_weights", &PlasterDisplaceParams::zoneWeights)
		.def_rw("enable", &PlasterDisplaceParams::enable)
		.def_rw("seed", &PlasterDisplaceParams::seed)
		.def_rw("avoid", &PlasterDisplaceParams::avoid)
		.def_rw("edge_length", &PlasterDisplaceParams::edgeLength)
		.def_rw("max_tris", &PlasterDisplaceParams::maxTris)
		.def_rw("detail", &PlasterDisplaceParams::detail)
		.def_rw("strength", &PlasterDisplaceParams::strength)
		.def_rw("sink", &PlasterDisplaceParams::sink)
		.def_rw("thickness_cap", &PlasterDisplaceParams::thicknessCap)
		.def_rw("undulation", &PlasterDisplaceParams::undulation)
		.def_rw("undulation_scale", &PlasterDisplaceParams::undulationScale)
		.def_rw("coats", &PlasterDisplaceParams::coats)
		.def_rw("coverage", &PlasterDisplaceParams::coverage)
		.def_rw("length_min", &PlasterDisplaceParams::lengthMin)
		.def_rw("length_max", &PlasterDisplaceParams::lengthMax)
		.def_rw("width_min", &PlasterDisplaceParams::widthMin)
		.def_rw("width_max", &PlasterDisplaceParams::widthMax)
		.def_rw("angle", &PlasterDisplaceParams::angle)
		.def_rw("angle_random", &PlasterDisplaceParams::angleRandom)
		.def_rw("cross_chance", &PlasterDisplaceParams::crossChance)
		.def_rw("curve", &PlasterDisplaceParams::curve)
		.def_rw("flatten", &PlasterDisplaceParams::flatten)
		.def_rw("tilt", &PlasterDisplaceParams::tilt)
		.def_rw("jagged", &PlasterDisplaceParams::jagged)
		.def_rw("streaks", &PlasterDisplaceParams::streaks)
		.def_rw("ridge_height", &PlasterDisplaceParams::ridgeHeight)
		.def_rw("ridge_width", &PlasterDisplaceParams::ridgeWidth)
		.def_rw("ridge_sharp", &PlasterDisplaceParams::ridgeSharp)
		.def_rw("ridge_trail", &PlasterDisplaceParams::ridgeTrail)
		.def_rw("grain", &PlasterDisplaceParams::grain)
		.def_rw("grain_size", &PlasterDisplaceParams::grainSize)
		.def_rw("pits", &PlasterDisplaceParams::pits)
		.def_rw("crack_enable", &PlasterDisplaceParams::crackEnable)
		.def_rw("crack_amount", &PlasterDisplaceParams::crackAmount)
		.def_rw("crack_scale", &PlasterDisplaceParams::crackScale)
		.def_rw("crack_width", &PlasterDisplaceParams::crackWidth)
		.def_rw("crack_depth", &PlasterDisplaceParams::crackDepth)
		.def_rw("crack_meander", &PlasterDisplaceParams::crackMeander)
		.def_rw("crack_breakup", &PlasterDisplaceParams::crackBreakup)
		.def_rw("crack_step", &PlasterDisplaceParams::crackStep)
		.def_rw("crack_lip", &PlasterDisplaceParams::crackLip)
		.def_rw("up_x", &PlasterDisplaceParams::upX)
		.def_rw("up_y", &PlasterDisplaceParams::upY)
		.def_rw("up_z", &PlasterDisplaceParams::upZ)
		;
	nb::class_<WoodDamageParams>(mod, "WoodDamageParams").def(nb::init<>())
		.def_rw("enable", &WoodDamageParams::enable)
		.def_rw("seed", &WoodDamageParams::seed)
		.def_rw("mode", &WoodDamageParams::mode)
		.def_rw("avoid", &WoodDamageParams::avoid)
		.def_rw("detail", &WoodDamageParams::detail)
		.def_rw("grain_mode", &WoodDamageParams::grainMode)
		.def_rw("grain_x", &WoodDamageParams::grainX)
		.def_rw("grain_y", &WoodDamageParams::grainY)
		.def_rw("grain_z", &WoodDamageParams::grainZ)
		.def_rw("amount", &WoodDamageParams::amount)
		.def_rw("size_min", &WoodDamageParams::sizeMin)
		.def_rw("size_max", &WoodDamageParams::sizeMax)
		.def_rw("lobes", &WoodDamageParams::lobes)
		.def_rw("stretch", &WoodDamageParams::stretch)
		.def_rw("end_bias", &WoodDamageParams::endBias)
		.def_rw("edge_bias", &WoodDamageParams::edgeBias)
		.def_rw("paint_radius", &WoodDamageParams::paintRadius)
		.def_rw("paint_snap", &WoodDamageParams::paintSnap)
		.def_rw("paint_close", &WoodDamageParams::paintClose)
		.def_rw("depth_min", &WoodDamageParams::depthMin)
		.def_rw("depth_max", &WoodDamageParams::depthMax)
		.def_rw("layers", &WoodDamageParams::layers)
		.def_rw("layer_inset", &WoodDamageParams::layerInset)
		.def_rw("fiber_depth_var", &WoodDamageParams::fiberDepthVar)
		.def_rw("thickness_cap", &WoodDamageParams::thicknessCap)
		.def_rw("jagged", &WoodDamageParams::jagged)
		.def_rw("fiber_width", &WoodDamageParams::fiberWidth)
		.def_rw("fiber_jitter", &WoodDamageParams::fiberJitter)
		.def_rw("splinter_length", &WoodDamageParams::splinterLength)
		.def_rw("splinter_sharp", &WoodDamageParams::splinterSharp)
		.def_rw("splinter_chunk", &WoodDamageParams::splinterChunk)
		.def_rw("splinter_ramp", &WoodDamageParams::splinterRamp)
		.def_rw("floor_fibers", &WoodDamageParams::floorFibers)
		.def_rw("groove_enable", &WoodDamageParams::grooveEnable)
		.def_rw("groove_spacing", &WoodDamageParams::grooveSpacing)
		.def_rw("groove_depth", &WoodDamageParams::grooveDepth)
		.def_rw("groove_wave", &WoodDamageParams::grooveWave)
		.def_rw("groove_breakup", &WoodDamageParams::grooveBreakup)
		.def_rw("weather_halo", &WoodDamageParams::weatherHalo)
		.def_rw("break_enable", &WoodDamageParams::breakEnable)
		.def_rw("break_chance", &WoodDamageParams::breakChance)
		.def_rw("break_size", &WoodDamageParams::breakSize)
		.def_rw("check_enable", &WoodDamageParams::checkEnable)
		.def_rw("check_amount", &WoodDamageParams::checkAmount)
		.def_rw("check_global", &WoodDamageParams::checkGlobal)
		.def_rw("check_length_min", &WoodDamageParams::checkLengthMin)
		.def_rw("check_length_max", &WoodDamageParams::checkLengthMax)
		.def_rw("check_width", &WoodDamageParams::checkWidth)
		.def_rw("check_depth", &WoodDamageParams::checkDepth)
		.def_rw("check_meander", &WoodDamageParams::checkMeander)
		.def_rw("knot_amount", &WoodDamageParams::knotAmount)
		.def_rw("knot_size", &WoodDamageParams::knotSize)
		.def_rw("knot_stretch", &WoodDamageParams::knotStretch)
		.def_rw("knot_depth", &WoodDamageParams::knotDepth)
		.def_rw("knot_checks", &WoodDamageParams::knotChecks)
		.def_rw("sliver_enable", &WoodDamageParams::sliverEnable)
		.def_rw("sliver_amount", &WoodDamageParams::sliverAmount)
		.def_rw("sliver_length", &WoodDamageParams::sliverLength)
		.def_rw("sliver_width", &WoodDamageParams::sliverWidth)
		.def_rw("sliver_thick", &WoodDamageParams::sliverThick)
		.def_rw("sliver_lift", &WoodDamageParams::sliverLift)
		.def_rw("sliver_embed", &WoodDamageParams::sliverEmbed)
		;
	nb::class_<LathDamageParams>(mod, "LathDamageParams").def(nb::init<>())
		.def_rw("enable", &LathDamageParams::enable)
        .def_rw("generated_geometry", &LathDamageParams::generatedGeometry)
        .def_rw("edge_wall_depth", &LathDamageParams::edgeWallDepth)
        .def_rw("organic", &LathDamageParams::organic)
        .def_rw("proximity", &LathDamageParams::proximity)
		.def_rw("seed", &LathDamageParams::seed)
		.def_rw("mode", &LathDamageParams::mode)
		.def_rw("avoid", &LathDamageParams::avoid)
		.def_rw("detail", &LathDamageParams::detail)
		.def_rw("amount", &LathDamageParams::amount)
		.def_rw("size_min", &LathDamageParams::sizeMin)
		.def_rw("size_max", &LathDamageParams::sizeMax)
		.def_rw("lobes", &LathDamageParams::lobes)
		.def_rw("stretch", &LathDamageParams::stretch)
		.def_rw("edge_bias", &LathDamageParams::edgeBias)
		.def_rw("paint_radius", &LathDamageParams::paintRadius)
		.def_rw("paint_snap", &LathDamageParams::paintSnap)
		.def_rw("paint_close", &LathDamageParams::paintClose)
		.def_rw("plaster_min", &LathDamageParams::plasterMin)
		.def_rw("plaster_max", &LathDamageParams::plasterMax)
		.def_rw("jagged", &LathDamageParams::jagged)
		.def_rw("outline_scale", &LathDamageParams::outlineScale)
		.def_rw("crumble", &LathDamageParams::crumble)
		.def_rw("crumble_size", &LathDamageParams::crumbleSize)
		.def_rw("profile", &LathDamageParams::profile)
		.def_rw("smooth_width", &LathDamageParams::smoothWidth)
		.def_rw("terraces", &LathDamageParams::terraces)
		.def_rw("terrace_skin", &LathDamageParams::terraceSkin)
		.def_rw("terrace_inset", &LathDamageParams::terraceInset)
		.def_rw("wall_soft", &LathDamageParams::wallSoft)
		.def_rw("roughness", &LathDamageParams::roughness)
		.def_rw("thickness_cap", &LathDamageParams::thicknessCap)
		.def_rw("cavity_depth", &LathDamageParams::cavityDepth)
		.def_rw("keys", &LathDamageParams::keys)
		.def_rw("key_size", &LathDamageParams::keySize)
		.def_rw("hole_enable", &LathDamageParams::holeEnable)
		.def_rw("hole_chance", &LathDamageParams::holeChance)
		.def_rw("hole_size", &LathDamageParams::holeSize)
		.def_rw("hole_jagged", &LathDamageParams::holeJagged)
		.def_rw("hole_laths", &LathDamageParams::holeLaths)
		.def_rw("crack_enable", &LathDamageParams::crackEnable)
		.def_rw("crack_amount", &LathDamageParams::crackAmount)
		.def_rw("crack_length_min", &LathDamageParams::crackLengthMin)
		.def_rw("crack_length_max", &LathDamageParams::crackLengthMax)
		.def_rw("crack_width", &LathDamageParams::crackWidth)
		.def_rw("crack_depth", &LathDamageParams::crackDepth)
		.def_rw("crack_meander", &LathDamageParams::crackMeander)
		.def_rw("crack_branch", &LathDamageParams::crackBranch)
		.def_rw("crack_interior", &LathDamageParams::crackInterior)
		.def_rw("lath_enable", &LathDamageParams::lathEnable)
		.def_rw("lath_amount", &LathDamageParams::lathAmount)
		.def_rw("lath_width", &LathDamageParams::lathWidth)
		.def_rw("lath_thickness", &LathDamageParams::lathThickness)
        .def_rw("cavity_smooth", &LathDamageParams::cavitySmooth)
        .def_rw("lath_optimize", &LathDamageParams::lathOptimize)
        .def_rw("rim_smooth", &LathDamageParams::rimSmooth)
        .def_rw("wall_slope", &LathDamageParams::wallSlope)
        .def_rw("cavity_slope", &LathDamageParams::cavitySlope)
        .def_rw("hole_noise_scale", &LathDamageParams::holeNoiseScale)
        .def_rw("hole_detail", &LathDamageParams::holeDetail)
		.def_rw("lath_gap", &LathDamageParams::lathGap)
		.def_rw("lath_scale", &LathDamageParams::lathScale)
		.def_rw("lath_angle", &LathDamageParams::lathAngle)
		.def_rw("lath_length", &LathDamageParams::lathLength)
		.def_rw("lath_embed", &LathDamageParams::lathEmbed)
		.def_rw("lath_jitter", &LathDamageParams::lathJitter)
		.def_rw("lath_tilt", &LathDamageParams::lathTilt)
		.def_rw("lath_warp", &LathDamageParams::lathWarp)
		.def_rw("lath_edge", &LathDamageParams::lathEdge)
		.def_rw("lath_missing", &LathDamageParams::lathMissing)
		.def_rw("lath_broken", &LathDamageParams::lathBroken)
		.def_rw("lath_bend", &LathDamageParams::lathBend)
		.def_rw("lath_splinter", &LathDamageParams::lathSplinter)
		.def_rw("up_x", &LathDamageParams::upX)
		.def_rw("surf_mode", &LathDamageParams::surfMode)
		.def_rw("tex_path", &LathDamageParams::texPath)
		.def_rw("tex_scale", &LathDamageParams::texScale)
		.def_rw("noise_scale", &LathDamageParams::noiseScale)
		.def_rw("noise_detail", &LathDamageParams::noiseDetail)
		.def_rw("noise_roughness", &LathDamageParams::noiseRoughness)
		.def_rw("noise_distortion", &LathDamageParams::noiseDistortion)
		.def_rw("noise_strength", &LathDamageParams::noiseStrength)
		.def_rw("tex_strength", &LathDamageParams::texStrength)
		.def_rw("tex_offset_x", &LathDamageParams::texOffX)
		.def_rw("tex_offset_y", &LathDamageParams::texOffY)
		.def_rw("tex_offset_z", &LathDamageParams::texOffZ)
		.def_rw("tex_edge", &LathDamageParams::texEdge)
		.def_rw("up_y", &LathDamageParams::upY)
		.def_rw("up_z", &LathDamageParams::upZ)
		                            
		.def_rw("style", &LathDamageParams::style)
		.def_rw("riven_wave", &LathDamageParams::rivenWave)
		.def_rw("riven_wave_length", &LathDamageParams::rivenWaveLength)
		.def_rw("riven_width_var", &LathDamageParams::rivenWidthVar)
		.def_rw("riven_twist", &LathDamageParams::rivenTwist)
		.def_rw("riven_rough", &LathDamageParams::rivenRough)
		.def_rw("stud_enable", &LathDamageParams::studEnable)
		.def_rw("stud_spacing", &LathDamageParams::studSpacing)
		.def_rw("stud_width", &LathDamageParams::studWidth)
		.def_rw("stud_depth", &LathDamageParams::studDepth)
		.def_rw("board_studs", &LathDamageParams::boardStuds)
		.def_rw("joint_bundle", &LathDamageParams::jointBundle)
		.def_rw("lath_hang", &LathDamageParams::lathHang)
		.def_rw("noggin_spacing", &LathDamageParams::nogginSpacing)
		.def_rw("frame_plates", &LathDamageParams::framePlates)
		.def_rw("double_plate", &LathDamageParams::doublePlate)
		.def_rw("nail_amount", &LathDamageParams::nailAmount)
		.def_rw("nail_length", &LathDamageParams::nailLength)
		;
	nb::class_<WallpaperParams>(mod, "WallpaperParams").def(nb::init<>())
		.def_rw("enable", &WallpaperParams::enable)
        .def_rw("manual_seams", &WallpaperParams::manualSeams)
        .def_rw("seam_edges", &WallpaperParams::seamEdges)
		.def_rw("seed", &WallpaperParams::seed)
		.def_rw("thickness", &WallpaperParams::thickness)
		.def_rw("strip_width", &WallpaperParams::stripWidth)
		.def_rw("seam_offset", &WallpaperParams::seamOffset)
		.def_rw("seam_dir_x", &WallpaperParams::seamDirX)
		.def_rw("seam_dir_y", &WallpaperParams::seamDirY)
		.def_rw("seam_dir_z", &WallpaperParams::seamDirZ)
		.def_rw("wall_angle", &WallpaperParams::wallAngle)
		.def_rw("flap_markers", &WallpaperParams::flapMarkers)
		.def_rw("marker_only", &WallpaperParams::markerOnly)
		.def_rw("finder_points", &WallpaperParams::finderPoints)
		.def_rw("finder_enable", &WallpaperParams::finderEnable)
		.def_rw("finder_x", &WallpaperParams::finderX)
		.def_rw("finder_y", &WallpaperParams::finderY)
		.def_rw("finder_z", &WallpaperParams::finderZ)
		.def_rw("detail", &WallpaperParams::detail)
		.def_rw("avoid", &WallpaperParams::avoid)
		.def_rw("seams_enable", &WallpaperParams::seamsEnable)
		.def_rw("seam_width", &WallpaperParams::seamWidth)
		.def_rw("seam_depth", &WallpaperParams::seamDepth)
		.def_rw("seam_open", &WallpaperParams::seamOpen)
		.def_rw("tears_enable", &WallpaperParams::tearsEnable)
		.def_rw("tear_amount", &WallpaperParams::tearAmount)
		.def_rw("tear_length_min", &WallpaperParams::tearLengthMin)
		.def_rw("tear_length_max", &WallpaperParams::tearLengthMax)
		.def_rw("tear_width_min", &WallpaperParams::tearWidthMin)
		.def_rw("tear_width_max", &WallpaperParams::tearWidthMax)
		.def_rw("tear_diagonal", &WallpaperParams::tearDiagonal)
		.def_rw("tear_from_top", &WallpaperParams::tearFromTop)
		.def_rw("tear_side", &WallpaperParams::tearSide)
		.def_rw("tear_diagonal_randomness", &WallpaperParams::tearDiagonalRandomness)
		.def_rw("tear_from_top_randomness", &WallpaperParams::tearFromTopRandomness)
		.def_rw("tear_jagged", &WallpaperParams::tearJagged)
		.def_rw("seam_crack", &WallpaperParams::seamCrack)
		.def_rw("seam_crack_length", &WallpaperParams::seamCrackLength)
		.def_rw("flaps_enable", &WallpaperParams::flapsEnable)
		.def_rw("flap_amount", &WallpaperParams::flapAmount)
		.def_rw("flap_fold", &WallpaperParams::flapFold)
		.def_rw("flap_fold_radius", &WallpaperParams::flapFoldRadius)
		.def_rw("flap_curl", &WallpaperParams::flapCurl)
		.def_rw("flap_curl_focus", &WallpaperParams::flapCurlFocus)
		.def_rw("flap_cone", &WallpaperParams::flapCone)
		.def_rw("flap_wave", &WallpaperParams::flapWave)
		.def_rw("flap_wave_randomness", &WallpaperParams::flapWaveRandomness)
		.def_rw("flap_curl_fold_randomness", &WallpaperParams::flapCurlFoldRandomness)
		.def_rw("flap_ragged", &WallpaperParams::flapRagged)
		.def_rw("lifts_enable", &WallpaperParams::liftsEnable)
		.def_rw("lift_amount", &WallpaperParams::liftAmount)
		.def_rw("lift_length_min", &WallpaperParams::liftLengthMin)
		.def_rw("lift_length_max", &WallpaperParams::liftLengthMax)
		.def_rw("lift_width_min", &WallpaperParams::liftWidthMin)
		.def_rw("lift_width_max", &WallpaperParams::liftWidthMax)
		.def_rw("lift_curl", &WallpaperParams::liftCurl)
		.def_rw("paint_enable", &WallpaperParams::paintEnable)
		.def_rw("paint_reach", &WallpaperParams::paintReach)
		.def_rw("paint_jagged", &WallpaperParams::paintJagged)
		.def_rw("seam_snap", &WallpaperParams::seamSnap)
		;

	mod.attr("__version__") = PROJECT_DAMAGE_CORE_VERSION;

	                                                                       
	                                                                              
	                                                     


	nb::class_<ProjectDamageParams::FractLevel>(mod, "FractLevel")
		.def_rw("branches", &ProjectDamageParams::FractLevel::branches)
		.def_rw("branch_length", &ProjectDamageParams::FractLevel::branchLength)
		.def(nb::init<>())
		.def_rw("amount", &ProjectDamageParams::FractLevel::amount)
		.def_rw("width", &ProjectDamageParams::FractLevel::width)
		.def_rw("depth", &ProjectDamageParams::FractLevel::depth)
		.def_rw("bias", &ProjectDamageParams::FractLevel::bias)
		.def_rw("chips", &ProjectDamageParams::FractLevel::chips)
		.def_rw("chip_amount", &ProjectDamageParams::FractLevel::chipAmount)
		.def_rw("chip_size", &ProjectDamageParams::FractLevel::chipSize)
		.def_rw("chip_depth", &ProjectDamageParams::FractLevel::chipDepth)
		.def_rw("subdiv", &ProjectDamageParams::FractLevel::subdiv)
        .def_rw("length", &ProjectDamageParams::FractLevel::length)
        .def_rw("paint_length_min", &ProjectDamageParams::FractLevel::paintLengthMin)
        .def_rw("paint_distortion", &ProjectDamageParams::FractLevel::paintDistortion)
        .def_rw("paint_distortion_scale", &ProjectDamageParams::FractLevel::paintDistortionScale)
		.def_rw("paint_radius", &ProjectDamageParams::FractLevel::paintRadius);

	                                                                                   
	                                                                        
	                                                                        
	                                                                        
	                                                                          
	                                                                          
	                                                            
	nb::class_<CustomShape>(mod, "CustomShape")
		.def(nb::init<>())
		.def_prop_rw("pts",
			[](const CustomShape& c) {
				std::vector<double> flat(c.pts.size() * 3);
				for (size_t i = 0; i < c.pts.size(); i++)
				{
					flat[i * 3 + 0] = c.pts[i].x;
					flat[i * 3 + 1] = c.pts[i].y;
					flat[i * 3 + 2] = c.pts[i].z;
				}
				return ToNumpy2D(std::move(flat), 3);
			},
			[](CustomShape& c, PointsIn pts) {
				const size_t n = pts.shape(0);
				const double* p = pts.data();
				c.pts.resize(n);
				for (size_t i = 0; i < n; i++)
					c.pts[i] = EVec(p[i * 3 + 0], p[i * 3 + 1], p[i * 3 + 2]);
			},
			                                                                
			                                                                  
			                                                               
			                                                                
			                                                                 
			                                                                
			                                                                   
			                                                                    
			                                                            
			nb::rv_policy::automatic)
		.def_rw("tris", &CustomShape::tris)
		.def_rw("size", &CustomShape::size)
		.def_rw("hash", &CustomShape::hash);

	                                                                                               
	                                                                       
	                            
	                                                                         
	                                                                   
	                                                                      
	                                                                                     
	                                                                                   
	                                                                         
	            
	                                                                          
	                                                                
	                                                                       
	                                                                          
	         
	nb::class_<FractProximity>(mod, "FractProximity")
		.def(nb::init<>())
		.def_rw("x", &FractProximity::x)
		.def_rw("y", &FractProximity::y)
		.def_rw("z", &FractProximity::z)
		.def_rw("radius", &FractProximity::radius)
		.def_rw("strength", &FractProximity::strength);

	nb::class_<BreakerSpec>(mod, "BreakerSpec")
		.def(nb::init<>())
		.def_rw("mode", &BreakerSpec::mode)
		.def_rw("flip", &BreakerSpec::flip)
		.def_rw("seed", &BreakerSpec::seed)
		.def_rw("length", &BreakerSpec::length)
		.def_rw("width", &BreakerSpec::width)
		.def_rw("radius", &BreakerSpec::radius)
		.def_rw("thickness", &BreakerSpec::thickness)
		.def_rw("resolution", &BreakerSpec::resolution)
		.def_rw("strength", &BreakerSpec::strength)
		.def_rw("macro_h", &BreakerSpec::macroH)
		.def_rw("turb_h", &BreakerSpec::turbH)
		.def_rw("fine_h", &BreakerSpec::fineH)
		.def_rw("noise_scale", &BreakerSpec::noiseScale)
		.def_rw("rim_amp", &BreakerSpec::rimAmp)
		.def_rw("rim_scale", &BreakerSpec::rimScale)
		.def_prop_rw("rel",
			[](const BreakerSpec& b) {
				std::vector<double> out(12);
				for (int r = 0; r < 3; r++) out[(size_t)r] = b.rel[3][r];
				for (int c = 0; c < 3; c++)
					for (int r = 0; r < 3; r++)
						out[(size_t)(3 + c * 3 + r)] = b.rel[c][r];
				return out;
			},
			[](BreakerSpec& b, const std::vector<double>& v) {
				if (v.size() != 12)
					throw std::invalid_argument(
						"rel needs exactly 12 doubles, translation-first "
						"(off.x, off.y, off.z, v1.x..z, v2.x..z, v3.x..z)");
				const m::vec3 off(v[0], v[1], v[2]);
				const m::vec3 v1(v[3], v[4], v[5]);
				const m::vec3 v2(v[6], v[7], v[8]);
				const m::vec3 v3(v[9], v[10], v[11]);
				b.rel = EMat(v1, v2, v3, off);
			})
		.def_prop_rw("shape",
			[](const BreakerSpec& b) -> nb::object {
				if (!b.custom)
					return nb::none();
				return nb::cast(*b.custom);
			},
			[](BreakerSpec& b, nb::object obj) {
				if (obj.is_none())
					b.custom = nullptr;
				else
					b.custom = std::make_shared<const CustomShape>(
						nb::cast<const CustomShape&>(obj));
			})
		.def_rw("custom_sheet", &BreakerSpec::customSheet)
        .def_rw("rebar", &BreakerSpec::rebar)
        .def_rw("surf_mode", &BreakerSpec::surfMode)
        .def_rw("tex_path", &BreakerSpec::texPath)
        .def_rw("tex_offset_x", &BreakerSpec::texOffX)
        .def_rw("tex_offset_y", &BreakerSpec::texOffY)
        .def_rw("tex_offset_z", &BreakerSpec::texOffZ)
        .def_rw("tex_scale", &BreakerSpec::texScale)
        .def_rw("tex_strength", &BreakerSpec::texStrength)
        .def_rw("tex_edge", &BreakerSpec::texEdge)
        .def_rw("tex_exit", &BreakerSpec::texExit)
        .def_rw("tex_macro", &BreakerSpec::texMacro)
        .def_rw("smooth_slope", &BreakerSpec::smoothSlope)
        .def_rw("smooth_width", &BreakerSpec::smoothWidth)
        .def_rw("surf_smooth", &BreakerSpec::surfSmooth)
        .def_rw("surf_detail", &BreakerSpec::surfDetail)
        .def_rw("surf_roughness", &BreakerSpec::surfRough)
        .def_rw("surf_crease", &BreakerSpec::surfCrease)
        .def_rw("surf_warp", &BreakerSpec::surfWarp)
        .def_rw("surf_aniso", &BreakerSpec::surfAniso)
        .def_rw("surf_aniso_axis", &BreakerSpec::surfAnisoAxis)
        .def_rw("surf_terrace", &BreakerSpec::surfTerrace)
        .def_rw("macro_scale", &BreakerSpec::macroScale)
        .def_rw("turb_scale", &BreakerSpec::turbScale)
        .def_rw("fine_scale", &BreakerSpec::fineScale)
        .def_rw("custom_hash", &BreakerSpec::customHash);

	                                                                                             
	                                                                     
	                                                                                  
	                                                                         
	                                                        
	                                                                        
	                                                                         
	                                                                     
	                                                                         
	                                                                          
	                                                                        
	                                                                       
	        
	                                                         
	                                                                         
	                   
	                                                                        
	                                                                          
	nb::class_<ProjectDamageParams>(mod, "Params")
		.def(nb::init<>())
        .def_rw("profile_core", &ProjectDamageParams::profileCore)
        .def_rw("incremental_keys", &ProjectDamageParams::incrementalKeys)
        .def_rw("fracture_layers", &ProjectDamageParams::fractureLayers)
        .def_rw("fracture_guide_points", &ProjectDamageParams::fractureGuidePoints)
        .def_rw("fracture_guide_counts", &ProjectDamageParams::fractureGuideCounts)
        .def_rw("fracture_guide_levels", &ProjectDamageParams::fractureGuideLevels)
        .def_rw("fracture_effect_id", &ProjectDamageParams::fractureEffectId)
		.def_rw("amount", &ProjectDamageParams::amount)
		.def_rw("size_min", &ProjectDamageParams::sizeMin)
		.def_rw("size_max", &ProjectDamageParams::sizeMax)
		.def_rw("depth_min", &ProjectDamageParams::depthMin)
		.def_rw("depth_max", &ProjectDamageParams::depthMax)
		.def_rw("chip_rim_soft", &ProjectDamageParams::chipRimSoft)
		.def_rw("offset", &ProjectDamageParams::offset)
		.def_rw("random_offset", &ProjectDamageParams::randomOffset)
		.def_rw("detail", &ProjectDamageParams::detail)
		.def_rw("noise_strength", &ProjectDamageParams::noiseStrength)
		.def_rw("noise_scale", &ProjectDamageParams::noiseScale)
		.def_rw("noise_roughness", &ProjectDamageParams::noiseRoughness)
		.def_rw("noise_distortion", &ProjectDamageParams::noiseDistortion)
		.def_rw("seed", &ProjectDamageParams::seed)
		.def_rw("threshold_deg", &ProjectDamageParams::thresholdDeg)
		.def_rw("axis_x", &ProjectDamageParams::axisX)
		.def_rw("axis_y", &ProjectDamageParams::axisY)
		.def_rw("axis_z", &ProjectDamageParams::axisZ)
		.def_rw("sec_amount", &ProjectDamageParams::secAmount)
		.def_rw("sec_size", &ProjectDamageParams::secSize)
		.def_rw("sec_depth", &ProjectDamageParams::secDepth)
		.def_rw("corner_prob", &ProjectDamageParams::cornerProb)
		.def_rw("corner_min_edges", &ProjectDamageParams::cornerMinEdges)
		.def_rw("corner_size_min", &ProjectDamageParams::cornerSizeMin)
		.def_rw("corner_size_max", &ProjectDamageParams::cornerSizeMax)
		.def_rw("corner_depth_min", &ProjectDamageParams::cornerDepthMin)
		.def_rw("corner_depth_max", &ProjectDamageParams::cornerDepthMax)
		.def_rw("corner_rim_soft", &ProjectDamageParams::cornerRimSoft)
		.def_rw("corner_detail", &ProjectDamageParams::cornerDetail)
		.def_rw("corner_sec_amount", &ProjectDamageParams::cornerSecAmount)
		.def_rw("corner_sec_size", &ProjectDamageParams::cornerSecSize)
		.def_rw("corner_sec_depth", &ProjectDamageParams::cornerSecDepth)
		.def_rw("norm_enable", &ProjectDamageParams::normEnable)
		.def_rw("norm_max_size", &ProjectDamageParams::normMaxSize)
		.def_rw("stretch", &ProjectDamageParams::stretch)
		.def_rw("surf_paint_only", &ProjectDamageParams::surfPaintOnly)
		.def_rw("surf_paint_snap", &ProjectDamageParams::surfPaintSnap)
		.def_rw("fract_paint_only", &ProjectDamageParams::fractPaintOnly)
		.def_rw("surf_amount", &ProjectDamageParams::surfAmount)
		.def_rw("surf_seed", &ProjectDamageParams::surfSeed)
		.def_rw("surf_size_min", &ProjectDamageParams::surfSizeMin)
		.def_rw("surf_size_max", &ProjectDamageParams::surfSizeMax)
		.def_rw("surf_depth_min", &ProjectDamageParams::surfDepthMin)
		.def_rw("surf_depth_max", &ProjectDamageParams::surfDepthMax)
		.def_rw("surf_noise", &ProjectDamageParams::surfNoise)
		.def_rw("surf_noise_in", &ProjectDamageParams::surfNoiseIn)
		.def_rw("surf_detail", &ProjectDamageParams::surfDetail)
		.def_rw("surf_sec_amount", &ProjectDamageParams::surfSecAmount)
		.def_rw("surf_sec_size", &ProjectDamageParams::surfSecSize)
		.def_rw("surf_sec_depth", &ProjectDamageParams::surfSecDepth)
		.def_rw("surf_type", &ProjectDamageParams::surfType)
		.def_rw("surf_grain_x", &ProjectDamageParams::surfGrainX)
		.def_rw("surf_grain_y", &ProjectDamageParams::surfGrainY)
		.def_rw("surf_grain_z", &ProjectDamageParams::surfGrainZ)
		.def_rw("surf_knife_jitter", &ProjectDamageParams::surfKnifeJitter)
		.def_rw("surf_knife_back", &ProjectDamageParams::surfKnifeBack)
		                                                                          
		                                                                    
		                                                               
		                                                                    
		.def_prop_ro("fract1", [](ProjectDamageParams& p) -> ProjectDamageParams::FractLevel& { return p.fract[0]; })
		.def_prop_ro("fract2", [](ProjectDamageParams& p) -> ProjectDamageParams::FractLevel& { return p.fract[1]; })
		.def_prop_ro("fract3", [](ProjectDamageParams& p) -> ProjectDamageParams::FractLevel& { return p.fract[2]; })
		.def_rw("fract_recipe", &ProjectDamageParams::fractRecipe)
		.def_rw("fract_thickness_cap", &ProjectDamageParams::fractThicknessCap)
        .def_rw("fract_through", &ProjectDamageParams::fractThrough)
        .def_rw("fract_through_fill", &ProjectDamageParams::fractThroughFill)
        .def_rw("fract_through_fill_size", &ProjectDamageParams::fractThroughFillSize)
        .def_rw("fract_through_rough", &ProjectDamageParams::fractThroughRough)
        .def_rw("fract_type", &ProjectDamageParams::fractType)
        .def_rw("fract_physics", &ProjectDamageParams::fractPhysics)
        .def_rw("fract_physics_depth", &ProjectDamageParams::fractPhysicsDepth)
        .def_rw("fract_physics_close", &ProjectDamageParams::fractPhysicsClose)
        .def_rw("damage_shading", &ProjectDamageParams::damageShading)
        .def_rw("damage_smooth_angle", &ProjectDamageParams::damageSmoothAngle)
        .def_rw("base_smooth_angle", &ProjectDamageParams::baseSmoothAngle)
        .def_rw("fract_helper_snap", &ProjectDamageParams::fractHelperSnap)
        .def_rw("fract_rounded", &ProjectDamageParams::fractRounded)
        .def_rw("fract_surface_proxy", &ProjectDamageParams::fractSurfaceProxy)
        .def_rw("surface_guide", &ProjectDamageParams::surfaceGuide)
        .def_rw("fract_impacts", &ProjectDamageParams::fractImpacts)
        .def_rw("fract_grain_x", &ProjectDamageParams::fractGrainX)
        .def_rw("fract_grain_y", &ProjectDamageParams::fractGrainY)
        .def_rw("fract_grain_z", &ProjectDamageParams::fractGrainZ)
        .def_rw("fract_grain_pull", &ProjectDamageParams::fractGrainPull)
        .def_rw("fract_through_min_piece", &ProjectDamageParams::fractThroughMinPiece)
		.def_rw("fract_prox_mode", &ProjectDamageParams::fractProxMode)
		.def_rw("fract_prox_radius", &ProjectDamageParams::fractProxRadius)
		.def_rw("fract_prox_strength", &ProjectDamageParams::fractProxStrength)
		.def_rw("fract_radial_spokes", &ProjectDamageParams::fractRadialSpokes)
		.def_rw("fract_radial_rings", &ProjectDamageParams::fractRadialRings)
		.def_rw("fract_radial_ring_fill", &ProjectDamageParams::fractRadialRingFill)
		.def_rw("fract_radial_crush", &ProjectDamageParams::fractRadialCrush)
		.def_rw("fract_proximity", &ProjectDamageParams::fractProximity)
		.def_rw("fract_glass_spokes", &ProjectDamageParams::fractGlassSpokes)
		.def_rw("fract_glass_rings", &ProjectDamageParams::fractGlassRings)
		.def_rw("fract_glass_branch", &ProjectDamageParams::fractGlassBranch)
		.def_rw("fract_glass_ring_fill", &ProjectDamageParams::fractGlassRingFill)
		.def_rw("fract_glass_width", &ProjectDamageParams::fractGlassWidth)
		.def_rw("fract_glass_depth", &ProjectDamageParams::fractGlassDepth)
		.def_rw("fract_glass_crush", &ProjectDamageParams::fractGlassCrush)
		.def_rw("fract_disp_strength", &ProjectDamageParams::fractDispStrength)
		.def_rw("fract_disp_scale", &ProjectDamageParams::fractDispScale)
		.def_rw("fract_detail", &ProjectDamageParams::fractDetail)
		.def_rw("fract_rim_soft", &ProjectDamageParams::fractRimSoft)
		.def_rw("fract_depth_mult", &ProjectDamageParams::fractDepthMult)
		.def_rw("fract_tile_offset", &ProjectDamageParams::fractTileOffset)
		.def_rw("fract_tile_seed", &ProjectDamageParams::fractTileSeed)
		.def_rw("build_coverage", &ProjectDamageParams::buildCoverage)
		.def_rw("build_width", &ProjectDamageParams::buildWidth)
		.def_rw("build_height", &ProjectDamageParams::buildHeight)
		.def_rw("build_irreg", &ProjectDamageParams::buildIrreg)
		.def_rw("build_corner_prob", &ProjectDamageParams::buildCornerProb)
		.def_rw("build_corner_size", &ProjectDamageParams::buildCornerSize)
		.def_rw("build_detail", &ProjectDamageParams::buildDetail)
		.def_rw("build_blend", &ProjectDamageParams::buildBlend)
		.def_rw("deposit_amount", &ProjectDamageParams::depositAmount)
		.def_rw("deposit_style", &ProjectDamageParams::depositStyle)
		.def_rw("deposit_size_min", &ProjectDamageParams::depositSizeMin)
		.def_rw("deposit_size_max", &ProjectDamageParams::depositSizeMax)
		.def_rw("deposit_height", &ProjectDamageParams::depositHeight)
		.def_rw("deposit_gravity", &ProjectDamageParams::depositGravity)
		.def_rw("deposit_gravity_x", &ProjectDamageParams::depositGravityX)
		.def_rw("deposit_gravity_y", &ProjectDamageParams::depositGravityY)
		.def_rw("deposit_gravity_z", &ProjectDamageParams::depositGravityZ)
		.def_rw("deposit_shelter", &ProjectDamageParams::depositShelter)
		.def_rw("deposit_shelter_radius", &ProjectDamageParams::depositShelterRadius)
		.def_rw("deposit_embed", &ProjectDamageParams::depositEmbed)
		.def_rw("deposit_layers", &ProjectDamageParams::depositLayers)
		.def_rw("deposit_porosity", &ProjectDamageParams::depositPorosity)
		.def_rw("deposit_separate", &ProjectDamageParams::depositSeparate)
		                                                                                        
		.def_rw("chip_seed", &ProjectDamageParams::chipSeed)
		.def_rw("chip_cluster", &ProjectDamageParams::chipCluster)
		.def_rw("chip_fine", &ProjectDamageParams::chipFine)
		.def_rw("chip_profile", &ProjectDamageParams::chipProfile)
		.def_rw("chip_asym", &ProjectDamageParams::chipAsym)
		.def_rw("chip_guard", &ProjectDamageParams::chipGuard)
		.def_rw("corner_style", &ProjectDamageParams::cornerStyle)
		.def_rw("corner_rough", &ProjectDamageParams::cornerRough)
		.def_rw("corner_tilt", &ProjectDamageParams::cornerTilt)
		.def_rw("corner_flakes", &ProjectDamageParams::cornerFlakes)
		.def_rw("corner_paint_only", &ProjectDamageParams::cornerPaintOnly)
		.def_rw("corner_paint_radius", &ProjectDamageParams::cornerPaintRadius)
		.def_rw("surf_cluster", &ProjectDamageParams::surfCluster)
		.def_rw("surf_size_bias", &ProjectDamageParams::surfSizeBias)
		.def_rw("surf_depth_follow", &ProjectDamageParams::surfDepthFollow)
		.def_rw("surf_guard", &ProjectDamageParams::surfGuard)
		.def_rw("fract_rim_jag", &ProjectDamageParams::fractRimJag)
		.def_rw("fract_flare", &ProjectDamageParams::fractFlare)
		.def_rw("fract_shard_enable", &ProjectDamageParams::fractShardEnable)
		.def_rw("fract_shard_dir", &ProjectDamageParams::fractShardDir)
		.def_rw("fract_shard_amount", &ProjectDamageParams::fractShardAmount)
		.def_rw("fract_shard_radius", &ProjectDamageParams::fractShardRadius)
		.def_rw("fract_shard_falloff", &ProjectDamageParams::fractShardFalloff)
		.def_rw("fract_shard_tilt", &ProjectDamageParams::fractShardTilt)
		.def_rw("fract_shard_random", &ProjectDamageParams::fractShardRandom)
		.def_rw("fract_shard_seed", &ProjectDamageParams::fractShardSeed)
		.def_rw("fract_shard_points", &ProjectDamageParams::fractShardPoints)
		.def_rw("build_shape", &ProjectDamageParams::buildShape)
		.def_rw("build_lumps", &ProjectDamageParams::buildLumps)
		.def_rw("build_paint_only", &ProjectDamageParams::buildPaintOnly)
		.def_rw("build_paint_radius", &ProjectDamageParams::buildPaintRadius)
		.def_ro("build_paint_pts", &ProjectDamageParams::buildPaintPts)
		.def_ro("build_paint_count", &ProjectDamageParams::buildPaintCount)
		.def_rw("pile_height", &ProjectDamageParams::pileHeight)
		.def_rw("pile_lumps", &ProjectDamageParams::pileLumps)
		.def_rw("pile_detail", &ProjectDamageParams::pileDetail)
		.def_rw("deposit_detail", &ProjectDamageParams::depositDetail)
		.def_ro("corner_paint_pts", &ProjectDamageParams::cornerPaintPts)
		.def_ro("corner_paint_count", &ProjectDamageParams::cornerPaintCount)
		.def("set_corner_paint", [](ProjectDamageParams& p, const std::vector<double>& xyz, const std::vector<int32_t>& counts) {
			size_t total = 0;
			for (int32_t c : counts) { if (c < 0) throw nb::value_error("negative count"); total += (size_t)c; }
			if (xyz.size() != total * 3) throw nb::value_error("xyz size does not match counts");
			p.cornerPaintPts = xyz;
			p.cornerPaintCount.assign(counts.begin(), counts.end());
		}, nb::arg("xyz"), nb::arg("counts"))
		.def("set_build_paint", [](ProjectDamageParams& p, const std::vector<double>& xyz, const std::vector<int32_t>& counts) {
			size_t total = 0;
			for (int32_t c : counts) { if (c < 0) throw nb::value_error("negative count"); total += (size_t)c; }
			if (xyz.size() != total * 3) throw nb::value_error("xyz size does not match counts");
			p.buildPaintPts = xyz;
			p.buildPaintCount.assign(counts.begin(), counts.end());
		}, nb::arg("xyz"), nb::arg("counts"))
		.def_prop_ro("hit", [](ProjectDamageParams& p) -> ConcreteHitParams& { return p.hit; })
		.def_prop_ro("debris", [](ProjectDamageParams& p) -> DamageDebrisParams& { return p.debris; })
		.def_prop_ro("rbf", [](ProjectDamageParams& p) -> RBFractureParams& { return p.rbf; })
		.def_prop_ro("cellf", [](ProjectDamageParams& p) -> CellFractureParams& { return p.cellf; })
		.def_prop_ro("wall_hole", [](ProjectDamageParams& p) -> WallHoleParams& { return p.wallHole; })
		.def_prop_ro("hang", [](ProjectDamageParams& p) -> HangHoleParams& { return p.hang; })
		.def_prop_ro("deform", [](ProjectDamageParams& p) -> ImpactDeformParams& { return p.deform; })
		.def_prop_ro("plaster", [](ProjectDamageParams& p) -> PlasterDisplaceParams& { return p.plaster; })
		.def_prop_ro("erosion", [](ProjectDamageParams& p) -> VoxelErosionParams& { return p.erosion; })
		.def_prop_ro("clay", [](ProjectDamageParams& p) -> ClayParams& { return p.clay; })
        .def_prop_ro("metal_tear", [](ProjectDamageParams& p) -> MetalTearParams& { return p.metalTear; })
        .def_prop_ro("pits", [](ProjectDamageParams& p) -> ConcretePitsParams& { return p.pits; })
		.def_prop_ro("wood", [](ProjectDamageParams& p) -> WoodDamageParams& { return p.wood; })
		.def_prop_ro("lath", [](ProjectDamageParams& p) -> LathDamageParams& { return p.lath; })
		.def_prop_ro("bullet", [](ProjectDamageParams& p) -> BulletImpactParams& { return p.bullet; })
        .def_prop_ro("blast", [](ProjectDamageParams& p) -> BuildingDestructionParams& { return p.blast; })
        .def_prop_ro("ground", [](ProjectDamageParams& p) -> GroundCracksParams& { return p.ground; })
        .def_prop_ro("break_rebar", [](ProjectDamageParams& p) -> BreakRebarParams& { return p.breakRebar; })
        .def_prop_ro("bevel", [](ProjectDamageParams& p) -> EdgeBevelParams& { return p.bevel; })
        .def_prop_ro("damaged_bevel", [](ProjectDamageParams& p) -> DamagedBevelParams& { return p.damagedBevel; })
.def_prop_ro("scratch", [](ProjectDamageParams& p) -> ScratchDrawParams& { return p.scratch; })
.def_prop_ro("metal", [](ProjectDamageParams& p) -> MetalDeformParams& { return p.metal; })
        .def_prop_ro("brick", [](ProjectDamageParams& p) -> BrickDamageParams& { return p.brick; })
        .def_prop_ro("glass", [](ProjectDamageParams& p) -> BrokenGlassParams& { return p.glass; })
        .def_prop_ro("fissure", [](ProjectDamageParams& p) -> GroundFissureParams& { return p.fissure; })
        .def_prop_ro("rusty", [](ProjectDamageParams& p) -> RustyHoleParams& { return p.rusty; })
        .def_prop_ro("wallpaper", [](ProjectDamageParams& p) -> WallpaperParams& { return p.wallpaper; })
		.def_prop_ro("fabric", [](ProjectDamageParams& p) -> FabricTearParams& { return p.fabric; })
		.def_prop_ro("concrete", [](ProjectDamageParams& p) -> ConcreteDamageParams& { return p.concrete; })
		.def_rw("rust_use_paint", &ProjectDamageParams::rustUsePaint)
		.def_rw("rust_enable", &ProjectDamageParams::rustEnable)
		.def_rw("rust_seed", &ProjectDamageParams::rustSeed)
		.def_rw("rust_intensity", &ProjectDamageParams::rustIntensity)
		.def_rw("rust_edge_bias", &ProjectDamageParams::rustEdgeBias)
		.def_rw("rust_bottom_bias", &ProjectDamageParams::rustBottomBias)
		.def_rw("rust_noise", &ProjectDamageParams::rustNoise)
		.def_rw("rust_noise_scale", &ProjectDamageParams::rustNoiseScale)
		.def_rw("rust_detail", &ProjectDamageParams::rustDetail)
		.def_rw("rust_crust_start", &ProjectDamageParams::rustCrustStart)
		.def_rw("rust_crust_height", &ProjectDamageParams::rustCrustHeight)
		.def_rw("rust_crust_rough", &ProjectDamageParams::rustCrustRough)
		.def_rw("rust_pits", &ProjectDamageParams::rustPits)
		.def_rw("rust_pit_start", &ProjectDamageParams::rustPitStart)
		.def_rw("rust_pit_size", &ProjectDamageParams::rustPitSize)
		.def_rw("rust_pit_depth", &ProjectDamageParams::rustPitDepth)
		.def_rw("rust_hole_start", &ProjectDamageParams::rustHoleStart)
		.def_rw("rust_hole_depth", &ProjectDamageParams::rustHoleDepth)
		.def_rw("rust_flakes", &ProjectDamageParams::rustFlakes)
		.def_rw("rust_flake_size", &ProjectDamageParams::rustFlakeSize)
		.def_rw("rust_paint", &ProjectDamageParams::rustPaint)
		.def_rw("rust_paint_start", &ProjectDamageParams::rustPaintStart)
		.def_rw("rust_band", &ProjectDamageParams::rustBand)
		.def_rw("rust_paint_flakes", &ProjectDamageParams::rustPaintFlakes)
		.def_rw("rust_craze", &ProjectDamageParams::rustCraze)
		.def_rw("rust_craze_cell", &ProjectDamageParams::rustCrazeCell)
		.def_rw("rust_blisters", &ProjectDamageParams::rustBlisters)
		.def_rw("rust_blister_size", &ProjectDamageParams::rustBlisterSize)
		.def_rw("rust_streak", &ProjectDamageParams::rustStreak)
		.def_rw("rust_streak_length", &ProjectDamageParams::rustStreakLength)
		.def_rw("rust_gravity_x", &ProjectDamageParams::rustGravityX)
		.def_rw("rust_gravity_y", &ProjectDamageParams::rustGravityY)
		.def_rw("rust_gravity_z", &ProjectDamageParams::rustGravityZ)
		.def_rw("heat_enable", &ProjectDamageParams::heatEnable)
		.def_rw("heat_seed", &ProjectDamageParams::heatSeed)
		.def_rw("heat_intensity", &ProjectDamageParams::heatIntensity)
		.def_rw("heat_diffuse", &ProjectDamageParams::heatDiffuse)
		.def_rw("heat_spread", &ProjectDamageParams::heatSpread)
		.def_rw("heat_duration", &ProjectDamageParams::heatDuration)
		.def_rw("heat_steps", &ProjectDamageParams::heatSteps)
		.def_rw("heat_char_start", &ProjectDamageParams::heatCharStart)
		.def_rw("heat_shrink_depth", &ProjectDamageParams::heatShrinkDepth)
		.def_rw("heat_detail", &ProjectDamageParams::heatDetail)
		.def_rw("heat_block_round", &ProjectDamageParams::heatBlockRound)
		.def_rw("heat_cell_min", &ProjectDamageParams::heatCellMin)
		.def_rw("heat_cell_max", &ProjectDamageParams::heatCellMax)
		.def_rw("heat_crack_cells", &ProjectDamageParams::heatCrackCells)
		.def_rw("heat_crack_width", &ProjectDamageParams::heatCrackWidth)
		.def_rw("heat_crack_depth", &ProjectDamageParams::heatCrackDepth)
		.def_rw("heat_blisters", &ProjectDamageParams::heatBlisters)
		.def_rw("heat_blister_min", &ProjectDamageParams::heatBlisterMin)
		.def_rw("heat_blister_max", &ProjectDamageParams::heatBlisterMax)
		.def_rw("heat_blister_low", &ProjectDamageParams::heatBlisterLow)
		.def_rw("heat_blister_high", &ProjectDamageParams::heatBlisterHigh)
		.def_rw("heat_blister_burst", &ProjectDamageParams::heatBlisterBurst)
		.def_rw("heat_char_layer", &ProjectDamageParams::heatCharLayer)
		.def_rw("coating_zone_enable", &ProjectDamageParams::coatingZoneEnable)
		.def_rw("coating_zone_threshold", &ProjectDamageParams::coatingZoneThreshold)
		.def_rw("coating_zone_smooth", &ProjectDamageParams::coatingZoneSmooth)
		.def_rw("coating_zone_inset", &ProjectDamageParams::coatingZoneInset)
		.def_rw("coating_zone_detail", &ProjectDamageParams::coatingZoneDetail)
		.def_rw("coating_flake_side", &ProjectDamageParams::coatingFlakeSide)
		.def_rw("coating_flake_side_x", &ProjectDamageParams::coatingFlakeSideX)
		.def_rw("coating_flake_side_y", &ProjectDamageParams::coatingFlakeSideY)
		.def_rw("coating_flake_side_z", &ProjectDamageParams::coatingFlakeSideZ)
		.def_rw("coating_enable", &ProjectDamageParams::coatingEnable)
		.def_rw("coating_amount", &ProjectDamageParams::coatingAmount)
		.def_rw("coating_thickness", &ProjectDamageParams::coatingThickness)
		.def_rw("coating_patch_min", &ProjectDamageParams::coatingPatchMin)
		.def_rw("coating_patch_max", &ProjectDamageParams::coatingPatchMax)
		.def_rw("coating_jagged", &ProjectDamageParams::coatingJagged)
		.def_rw("coating_flakes", &ProjectDamageParams::coatingFlakes)
		.def_rw("coating_curl", &ProjectDamageParams::coatingCurl)
		.def_rw("coating_cracks", &ProjectDamageParams::coatingCracks)
		.def_rw("coating_seed", &ProjectDamageParams::coatingSeed)
		.def_rw("coating_satellites", &ProjectDamageParams::coatingSatellites)
		.def_rw("coating_stretch", &ProjectDamageParams::coatingStretch)
		.def_rw("coating_dir_x", &ProjectDamageParams::coatingDirX)
		.def_rw("coating_dir_y", &ProjectDamageParams::coatingDirY)
		.def_rw("coating_dir_z", &ProjectDamageParams::coatingDirZ)
		.def_rw("coating_edge_wrap", &ProjectDamageParams::coatingEdgeWrap)
		.def_rw("coating_facing_only", &ProjectDamageParams::coatingFacingOnly)
		.def_rw("coating_facing_x", &ProjectDamageParams::coatingFacingX)
		.def_rw("coating_facing_y", &ProjectDamageParams::coatingFacingY)
		.def_rw("coating_facing_z", &ProjectDamageParams::coatingFacingZ)
		.def_rw("coating_facing_angle", &ProjectDamageParams::coatingFacingAngle)
		.def_rw("coating_flake_size", &ProjectDamageParams::coatingFlakeSize)
		.def_rw("coating_crack_length", &ProjectDamageParams::coatingCrackLength)
		.def_rw("topology", &ProjectDamageParams::topology)
		.def_rw("base_map", &ProjectDamageParams::baseMap)
        .def_rw("preserve_surface_shells", &ProjectDamageParams::preserveSurfaceShells)
		                                                                          
		                                                                            
		                                     
		                                                                   
		                                                                        
		                                                                    
		                                                           
		                                                                
		.def_prop_rw("breakers",
			[](const ProjectDamageParams& p) { return p.breakers; },
			[](ProjectDamageParams& p, const std::vector<BreakerSpec>& v) { p.breakers = v; })
		.def_rw("break_surf_dmg", &ProjectDamageParams::breakSurfDmg)
		.def_rw("break_rim_amount", &ProjectDamageParams::breakRimAmount)
		.def_rw("break_rim_size", &ProjectDamageParams::breakRimSize)
		.def_rw("break_rim_depth", &ProjectDamageParams::breakRimDepth)
		                                                           
		                                                      
		.def_rw("avoid_chips", &ProjectDamageParams::avoidChips)
		.def_rw("avoid_surf", &ProjectDamageParams::avoidSurf)
		.def_rw("avoid_build", &ProjectDamageParams::avoidBuild)
		.def_rw("avoid_fract", &ProjectDamageParams::avoidFract)
		.def_rw("avoid_corner", &ProjectDamageParams::avoidCorner);

	                                                               
	                                                                         
	                                                                      
	                                                                         
	                                                                         
	                                                                          
	                                                                         
	                                 
	nb::class_<PyDamageCache>(mod, "Cache",
		"Session-persistent damage cache. Pass the same instance to repeated "
		"build_worn() calls (with a matching cache_fingerprint) to skip "
		"re-detecting and re-placing damage cutters when only a breaker "
		"moved. One instance must never be used by two concurrent "
		"build_worn() calls.")
        .def(nb::init<>())
        .def_prop_ro("incremental_built", [](PyDamageCache& c){return DamageCacheIncrementalBuilt(c.get());})
        .def_prop_ro("incremental_reused", [](PyDamageCache& c){return DamageCacheIncrementalReused(c.get());})
                                                                         
		                                                                    
		                                                                   
		                                                                
		                                                                     
		                                                                   
		                                                                    
		                                                                 
		                                                               
		                 
		.def("serialize", [](PyDamageCache& self) {
			const std::vector<uint8_t> buf = DamageCacheSerialize(self.get());
			return nb::bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
		}, "The persisted half of this cache as bytes (empty when there is nothing to store).")
		.def("deserialize", [](PyDamageCache& self, nb::bytes data) {
			return DamageCacheDeserialize(self.get(),
				reinterpret_cast<const uint8_t*>(data.c_str()), data.size()) ? true : false;
		}, nb::arg("data"),
		"Restore the persisted half from bytes serialize() produced. False (and an empty cache) on any implausible input.");

	                                                                        
	                                                                        
	mod.def("compute_damage_shading", [](PointsIn points, PolysIn polys, GroupsIn groups,
		PointsIn srcPoints, PolysIn srcPolys, NormalsIn srcNormals,
		double damageAngle, double baseAngle) {
		if (groups.shape(0) != polys.shape(0) || srcNormals.shape(0) != srcPolys.shape(0))
			throw std::invalid_argument("shading: face/normal row count mismatch");
		if (!std::isfinite(damageAngle) || !std::isfinite(baseAngle))
			throw std::invalid_argument("shading: angles must be finite");
		auto readPoints = [](PointsIn a) {
			std::vector<EVec> v; v.reserve(a.shape(0));
			for (size_t i=0; i<a.shape(0); ++i) {
				for (int k=0;k<3;++k) if (!std::isfinite(a(i,k))) throw std::invalid_argument("shading: non-finite point");
				v.emplace_back(a(i,0),a(i,1),a(i,2));
			}
			return v;
		};
		auto readPolys = [](PolysIn a, size_t nv) {
			std::vector<ProjectDamagePoly> v; v.reserve(a.shape(0));
			for (size_t i=0; i<a.shape(0); ++i) {
				for (int k=0;k<4;++k) if (a(i,k)<0 || size_t(a(i,k))>=nv) throw std::invalid_argument("shading: polygon index out of range");
				v.push_back({a(i,0),a(i,1),a(i,2),a(i,3)});
			}
			return v;
		};
		auto p=readPoints(points), sp=readPoints(srcPoints);
		auto f=readPolys(polys,p.size()), sf=readPolys(srcPolys,sp.size());
		std::vector<unsigned char> g(groups.data(),groups.data()+groups.shape(0));
		std::vector<double> sn(srcNormals.data(),srcNormals.data()+srcNormals.shape(0)*12), result;
		for (double x:sn) if (!std::isfinite(x)) throw std::invalid_argument("shading: non-finite normal");
		MeshView source{sp.data(),(Int32)sp.size(),sf.data(),(Int32)sf.size()};
		{
			nb::gil_scoped_release release;
			ComputeDamageShading(p,f,g,damageAngle,baseAngle,result,&source,&sn,nullptr);
		}
		return ToNumpy2D(std::move(result),12);
	}, nb::arg("points"),nb::arg("polys"),nb::arg("groups"),nb::arg("source_points"),
		nb::arg("source_polys"),nb::arg("source_normals"),nb::arg("damage_angle")=80.,nb::arg("base_angle")=30.);

	nb::class_<RootsParams>(mod, "RootsParams").def(nb::init<>())
		.def_rw("seed", &RootsParams::seed)
		.def_rw("size", &RootsParams::size)
		.def_rw("roots_per_seed", &RootsParams::rootsPerSeed)
		.def_rw("root_length", &RootsParams::rootLength)
		.def_rw("length_random", &RootsParams::lengthRandom)
		.def_rw("primary_weight", &RootsParams::primaryWeight)
		.def_rw("random_weight", &RootsParams::randomWeight)
		.def_rw("gravity_weight", &RootsParams::gravityWeight)
		.def_rw("spread_weight", &RootsParams::spreadWeight)
		.def_rw("branching_probability", &RootsParams::branchingProbability)
		.def_rw("branch_angle", &RootsParams::branchAngle)
		.def_rw("max_levels", &RootsParams::maxLevels)
		.def_rw("child_length", &RootsParams::childLength)
		.def_rw("root_size", &RootsParams::rootSize)
		.def_rw("taper", &RootsParams::taper)
		.def_rw("tip_radius", &RootsParams::tipRadius)
		.def_rw("child_radius", &RootsParams::childRadius)
		.def_rw("embed", &RootsParams::embed)
		.def_rw("flare", &RootsParams::flare)
		.def_rw("flatten", &RootsParams::flatten)
		.def_rw("sides", &RootsParams::sides)
		.def_rw("uv_scale", &RootsParams::uvScale)
		.def_rw("up_x", &RootsParams::upX)
		.def_rw("up_y", &RootsParams::upY)
		.def_rw("up_z", &RootsParams::upZ)
		.def_rw("max_nodes", &RootsParams::maxNodes)
		.def_rw("max_steps", &RootsParams::maxSteps)
		.def_rw("guide_weight", &RootsParams::guideWeight)
		.def_rw("guide_radius", &RootsParams::guideRadius);
	mod.def("grow_roots", &GrowRootsPy, nb::arg("points"), nb::arg("polys"), nb::arg("seeds"), nb::arg("params"), nb::arg("guides") = std::vector<PointsIn>(), nb::arg("surface_guide") = false,
		"Grow roots over the mesh from the seed points: tapering, branching tubes hugging the surface (procedural UVs).");

	RegisterClothSheets(mod);
	nb::class_<PaperGenParams>(mod, "PaperGenParams").def(nb::init<>())
		.def_rw("seed", &PaperGenParams::seed)
		.def_rw("width", &PaperGenParams::width)
		.def_rw("height", &PaperGenParams::height)
		.def_rw("resolution", &PaperGenParams::resolution)
		.def_rw("quads", &PaperGenParams::quads)
		.def_rw("thickness", &PaperGenParams::thickness)
		.def_rw("edge_rough", &PaperGenParams::edgeRough)
		.def_rw("crumple_strength", &PaperGenParams::crumpleStrength)
		.def_rw("crumple_size", &PaperGenParams::crumpleSize)
		.def_rw("crumple_detail", &PaperGenParams::crumpleDetail)
		.def_rw("crumple_sharpness", &PaperGenParams::crumpleSharpness)
		.def_rw("waviness", &PaperGenParams::waviness)
		.def_rw("wave_size", &PaperGenParams::waveSize)
		.def_rw("folds_x", &PaperGenParams::foldsX)
		.def_rw("folds_y", &PaperGenParams::foldsY)
		.def_rw("fold_depth", &PaperGenParams::foldDepth)
		.def_rw("torn_width", &PaperGenParams::tornWidth)
		.def_rw("torn_scale", &PaperGenParams::tornScale)
		.def_rw("tongues", &PaperGenParams::tongues)
		.def_rw("tongue_stretch", &PaperGenParams::tongueStretch)
		.def_rw("tongue_angle", &PaperGenParams::tongueAngle)
		.def_rw("tongue_angle_random", &PaperGenParams::tongueAngleRandom)
		.def_rw("torn_detail", &PaperGenParams::tornDetail)
		.def_rw("flakes", &PaperGenParams::flakes)
		.def_rw("peel_width", &PaperGenParams::peelWidth)
		.def_rw("rag_size", &PaperGenParams::ragSize)
		.def_rw("jagged", &PaperGenParams::jagged)
		.def_rw("fiber", &PaperGenParams::fiber)
		.def_rw("roundness", &PaperGenParams::roundness)
		.def_rw("tear_gap", &PaperGenParams::tearGap)
		.def_rw("keep_pieces", &PaperGenParams::keepPieces)
		.def_rw("min_piece", &PaperGenParams::minPiece)
		.def_rw("edge_curl", &PaperGenParams::edgeCurl)
		.def_rw("curl_width", &PaperGenParams::curlWidth)
		.def_rw("edge_dist_max", &PaperGenParams::edgeDistMax)
		.def_rw("hole_amount", &PaperGenParams::holeAmount)
		.def_rw("hole_size_min", &PaperGenParams::holeSizeMin)
		.def_rw("hole_size_max", &PaperGenParams::holeSizeMax)
		.def_rw("hole_irregular", &PaperGenParams::holeIrregular)
		.def_rw("hole_elongation", &PaperGenParams::holeElongation)
		.def_rw("hole_margin", &PaperGenParams::holeMargin)
		.def_rw("corner_tears", &PaperGenParams::cornerTears)
		.def_rw("corner_size_min", &PaperGenParams::cornerSizeMin)
		.def_rw("corner_size_max", &PaperGenParams::cornerSizeMax)
		.def_rw("edge_bites", &PaperGenParams::edgeBites)
		.def_rw("bite_size_min", &PaperGenParams::biteSizeMin)
		.def_rw("bite_size_max", &PaperGenParams::biteSizeMax)
		.def_rw("bite_depth", &PaperGenParams::biteDepth)
		.def_rw("stroke_hole_width", &PaperGenParams::strokeHoleWidth)
		.def_rw("tear_snap", &PaperGenParams::tearSnap)
		.def_rw("close_share", &PaperGenParams::closeShare)
		.def_rw("paint_only", &PaperGenParams::paintOnly)
		.def_rw("max_verts", &PaperGenParams::maxVerts);
	mod.def("apply_paper", &ApplyPaperPy, nb::arg("points"), nb::arg("triangles"), nb::arg("params"),
		nb::arg("tears"), nb::arg("tears_cyclic"), nb::arg("holes"), nb::arg("holes_cyclic"));
	mod.def("build_paper", &BuildPaperPy, nb::arg("params"), nb::arg("tears"), nb::arg("tears_cyclic"), nb::arg("holes"),
		nb::arg("holes_cyclic"),
		"A sheet of paper built from scratch: crumpled (creases + facets), rounded torn holes, torn corners / border bites, "
		"Grease Pencil tear areas (tears: the stroke picks the area, the torn outline is generated) and holes / gashes "
		"(holes); strokes are float64 (N, 3) arrays in the sheet's object space.");

	nb::class_<CableGenParams>(mod, "CableGenParams").def(nb::init<>())
		.def_rw("seed", &CableGenParams::seed)
		.def_rw("type", &CableGenParams::type)
		.def_rw("diameter", &CableGenParams::diameter)
		.def_rw("count", &CableGenParams::count)
		.def_rw("clumps", &CableGenParams::clumps)
		.def_rw("radius", &CableGenParams::radius)
		.def_rw("arrangement", &CableGenParams::arrangement)
		.def_rw("gap", &CableGenParams::gap)
		.def_rw("twist", &CableGenParams::twist)
		.def_rw("length_min", &CableGenParams::lengthMin)
		.def_rw("length_max", &CableGenParams::lengthMax)
		.def_rw("noise", &CableGenParams::noise)
		.def_rw("noise_length", &CableGenParams::noiseLength)
		.def_rw("sag", &CableGenParams::sag)
		.def_rw("sag_random", &CableGenParams::sagRandom)
		.def_rw("spread", &CableGenParams::spread)
		.def_rw("diameter_random", &CableGenParams::diameterRandom)
		.def_rw("fan_out", &CableGenParams::fanOut)
		.def_rw("coils", &CableGenParams::coils)
		.def_rw("coil_radius", &CableGenParams::coilRadius)
		.def_rw("coil_turns", &CableGenParams::coilTurns)
		.def_rw("dangle", &CableGenParams::dangle)
		.def_rw("dangle_length", &CableGenParams::dangleLength)
		.def_rw("gravity", &CableGenParams::gravity)
		.def_rw("metre", &CableGenParams::metre)
		.def_rw("slack", &CableGenParams::slack)
		.def_rw("stiffness", &CableGenParams::stiffness)
		.def_rw("friction", &CableGenParams::friction)
		.def_rw("pin", &CableGenParams::pin)
		.def_rw("steps", &CableGenParams::steps)
		.def_rw("use_floor", &CableGenParams::useFloor)
		.def_rw("floor_height", &CableGenParams::floorHeight)
		.def_rw("end_start", &CableGenParams::endStart)
		.def_rw("end_end", &CableGenParams::endEnd)
		.def_rw("strip", &CableGenParams::strip)
		.def_rw("splay", &CableGenParams::splay)
		.def_rw("fray", &CableGenParams::fray)
		.def_rw("damage", &CableGenParams::damage)
		.def_rw("damage_size", &CableGenParams::damageSize)
		.def_rw("strands", &CableGenParams::strands)
		.def_rw("strand_length", &CableGenParams::strandLength)
		.def_rw("damage_everywhere", &CableGenParams::damageEverywhere)
		.def_rw("tie_spacing", &CableGenParams::tieSpacing)
		.def_rw("tie_width", &CableGenParams::tieWidth)
		.def_rw("clips", &CableGenParams::clips)
		.def_rw("sides", &CableGenParams::sides)
		.def_rw("segment", &CableGenParams::segment)
		.def_rw("use_radius", &CableGenParams::useRadius)
		.def_rw("use_tilt", &CableGenParams::useTilt)
		.def_rw("uv_scale", &CableGenParams::uvScale)
		.def_rw("down_x", &CableGenParams::downX)
		.def_rw("down_y", &CableGenParams::downY)
		.def_rw("down_z", &CableGenParams::downZ)
		.def_rw("max_verts", &CableGenParams::maxVerts);
	mod.def("build_cable_curves", &BuildCableCurvesPy, nb::arg("curves"), nb::arg("cyclic"), nb::arg("params"),
		nb::arg("spawns"), nb::arg("collider_points"), nb::arg("collider_tris"), nb::arg("zones"),
		"Electrical cables along sampled curve splines (rows x,y,z,tilt,radius,anchor) or around spawn points, optionally "
		"settled by a rope simulation on collider triangles; bundles, clumps, stripped / torn / connector ends, rat-bitten "
		"damage zones, ties, clips.");


	nb::class_<IvyParams>(mod, "IvyParams").def(nb::init<>())
		.def_rw("seed", &IvyParams::seed)
		.def_rw("size", &IvyParams::size)
		.def_rw("primary_weight", &IvyParams::primaryWeight)
		.def_rw("random_weight", &IvyParams::randomWeight)
		.def_rw("gravity_weight", &IvyParams::gravityWeight)
		.def_rw("adhesion_weight", &IvyParams::adhesionWeight)
		.def_rw("branching_probability", &IvyParams::branchingProbability)
		.def_rw("max_float_length", &IvyParams::maxFloatLength)
		.def_rw("max_adhesion_distance", &IvyParams::maxAdhesionDistance)
		.def_rw("up_x", &IvyParams::upX)
		.def_rw("up_y", &IvyParams::upY)
		.def_rw("up_z", &IvyParams::upZ)
		.def_rw("max_nodes", &IvyParams::maxNodes)
		.def_rw("max_steps", &IvyParams::maxSteps)
		.def_rw("branch_size", &IvyParams::branchSize)
		.def_rw("branch_sides", &IvyParams::branchSides)
		.def_rw("uv_scale", &IvyParams::uvScale)
		.def_rw("leaf_size", &IvyParams::leafSize)
		.def_rw("leaf_density", &IvyParams::leafDensity)
		.def_rw("leaf_density_multiplier", &IvyParams::leafDensityMultiplier)
		.def_rw("leaf_offset", &IvyParams::leafOffset)
        .def_rw("adhesion_smoothing", &IvyParams::adhesionSmoothing)
        .def_rw("leaf_maturity", &IvyParams::leafMaturity)
		.def_rw("stem_length", &IvyParams::stemLength)
		.def_rw("stem_thickness", &IvyParams::stemThickness)
		.def_rw("leaf_tilt", &IvyParams::leafTilt)
		.def_rw("stem_attach", &IvyParams::stemAttach)
		.def_rw("leaf_uv_direction", &IvyParams::leafUvDirection)
		.def_rw("hang_density", &IvyParams::hangDensity)
		.def_rw("hang_length", &IvyParams::hangLength)
		.def_rw("guide_weight", &IvyParams::guideWeight)
		.def_rw("guide_radius", &IvyParams::guideRadius);
	mod.def("grow_ivy", &GrowIvyPy, nb::arg("points"), nb::arg("polys"), nb::arg("seeds"), nb::arg("params"), nb::arg("guides") = std::vector<PointsIn>(), nb::arg("curves") = std::vector<PointsIn>(), nb::arg("surface_guide") = false,
		"Grow ivy on the mesh from the seed points and along drawn curves (hanging ivy): branch tubes (procedural UVs), leaf petioles and leaves.");
	nb::class_<WeldParams>(mod, "WeldParams").def(nb::init<>())
		.def_rw("seed", &WeldParams::seed)
		.def_rw("edges_enable", &WeldParams::edgesEnable)
		.def_rw("edge_type", &WeldParams::edgeType)
		.def_rw("angle", &WeldParams::angle)
		.def_rw("mask_threshold", &WeldParams::maskThreshold)
		.def_rw("strokes_enable", &WeldParams::strokesEnable)
		.def_rw("stroke_reach", &WeldParams::strokeReach)
		.def_rw("width", &WeldParams::width)
		.def_rw("height", &WeldParams::height)
		.def_rw("pitch", &WeldParams::pitch)
		.def_rw("ripple_height", &WeldParams::rippleHeight)
		.def_rw("curvature", &WeldParams::curvature)
		.def_rw("wobble", &WeldParams::wobble)
		.def_rw("sides", &WeldParams::sides)
		.def_rw("detail", &WeldParams::detail)
		.def_rw("uv_scale", &WeldParams::uvScale);
	mod.def("build_welds", &BuildWeldsPy, nb::arg("points"), nb::arg("polys"), nb::arg("weights"), nb::arg("strokes"), nb::arg("params"), nb::arg("surface_guide") = false,
		"Weld beads along the masked corner edges and the drawn strokes.");
	nb::class_<GrassParams>(mod, "GrassParams").def(nb::init<>())
		.def_rw("seed", &GrassParams::seed)
		.def_rw("fresh", &GrassParams::fresh)
		.def_rw("up_x", &GrassParams::upX)
		.def_rw("up_y", &GrassParams::upY)
		.def_rw("up_z", &GrassParams::upZ)
		.def_rw("slope_limit", &GrassParams::slopeLimit)
		.def_rw("base_density", &GrassParams::baseDensity)
		.def_rw("edge_density", &GrassParams::edgeDensity)
		.def_rw("edge_radius", &GrassParams::edgeRadius)
		.def_rw("edge_angle", &GrassParams::edgeAngle)
		.def_rw("cavity_density", &GrassParams::cavityDensity)
		.def_rw("cavity_radius", &GrassParams::cavityRadius)
		.def_rw("stroke_mode", &GrassParams::strokeMode)
		.def_rw("stroke_density", &GrassParams::strokeDensity)
		.def_rw("stroke_radius", &GrassParams::strokeRadius)
		.def_rw("max_shards", &GrassParams::maxShards)
		.def_rw("length_min", &GrassParams::lengthMin)
		.def_rw("length_max", &GrassParams::lengthMax)
		.def_rw("width", &GrassParams::width)
		.def_rw("pile_height", &GrassParams::pileHeight)
		.def_rw("segments", &GrassParams::segments)
		.def_rw("shape", &GrassParams::shape)
		.def_rw("stone_min", &GrassParams::stoneMin)
		.def_rw("stone_max", &GrassParams::stoneMax)
		.def_rw("stone_skew", &GrassParams::stoneSkew)
		.def_rw("flatness", &GrassParams::flatness)
		.def_rw("angularity", &GrassParams::angularity)
		.def_rw("embed", &GrassParams::embed)
		.def_rw("sorting", &GrassParams::sorting);
	mod.def("scatter_grass", &ScatterGrassPy, nb::arg("points"), nb::arg("polys"), nb::arg("strokes"), nb::arg("params"), nb::arg("surface_guide") = false,
		"Grass shards on the ground, gathering at edges and in cavities; Grease Pencil overlay / add.");
	nb::class_<FoamParams>(mod, "FoamParams").def(nb::init<>())
		.def_rw("seed", &FoamParams::seed)
		.def_rw("max_gap", &FoamParams::maxGap)
		.def_rw("min_gap", &FoamParams::minGap)
		.def_rw("density", &FoamParams::density)
		.def_rw("spacing", &FoamParams::spacing)
		.def_rw("min_openness", &FoamParams::minOpenness)
		.def_rw("blob_size", &FoamParams::blobSize)
		.def_rw("bulge", &FoamParams::bulge)
		.def_rw("lumpiness", &FoamParams::lumpiness)
		.def_rw("sag", &FoamParams::sag)
		.def_rw("up_x", &FoamParams::upX)
		.def_rw("up_y", &FoamParams::upY)
		.def_rw("up_z", &FoamParams::upZ)
		.def_rw("max_blobs", &FoamParams::maxBlobs)
		.def_rw("style", &FoamParams::style)
		.def_rw("spread", &FoamParams::spread)
		.def_rw("cauliflower", &FoamParams::cauliflower)
		.def_rw("drips", &FoamParams::drips)
		.def_rw("drip_length", &FoamParams::dripLength)
		.def_rw("coverage", &FoamParams::coverage)
		.def_rw("trim", &FoamParams::trim)
		.def_rw("resolution", &FoamParams::resolution)
		.def_rw("max_cells", &FoamParams::maxCells);
	mod.def("spray_foam", &SprayFoamPy, nb::arg("points"), nb::arg("polys"), nb::arg("params"), nb::arg("surface_guide") = false,
		"Expanding foam squeezed out of the tight gaps of the mesh (seams, fractures).");
	mod.def("foam_path", &FoamPathPy, nb::arg("centres"), nb::arg("normals"), nb::arg("tangents"), nb::arg("params"),
		"Expanding foam along a drawn path: points on the surface, surface normals, drawing directions.");
	mod.def("build_worn", &BuildWornPy, nb::arg("points"), nb::arg("polys"), nb::arg("params"),
		nb::arg("cache") = nb::none(), nb::arg("cache_fingerprint") = 0,
		nb::arg("uvs").none() = nb::none(),
		nb::arg("mask_weights").none() = nb::none(),
		nb::arg("mask_amount") = 1.0, nb::arg("mask_size") = 0.0,
		nb::arg("avoid_polys").none() = nb::none(),
		nb::arg("guide_splines") = std::vector<PointsIn>(),
		nb::arg("heat_weights").none() = nb::none(),
		nb::arg("coating_zone_weights").none() = nb::none(),
		nb::arg("guide_levels") = std::vector<Int32>(),
		nb::arg("surface_paint_paths") = std::vector<PointsIn>(),
		nb::arg("surface_paint_normals").none() = nb::none(),
		nb::arg("rust_weights").none() = nb::none(),
		nb::arg("fabric_weights").none() = nb::none(),
		nb::arg("fabric_avoid_polys").none() = nb::none(),
		nb::arg("concrete_strokes") = std::vector<PointsIn>(),
		nb::arg("coating_paint_paths") = std::vector<PointsIn>(),
		nb::arg("coating_paint_normals").none() = nb::none(),
		nb::arg("rust_paint_paths") = std::vector<PointsIn>(),
		nb::arg("rust_paint_normals").none() = nb::none(),
        nb::arg("wood_strokes") = std::vector<PointsIn>(),
        nb::arg("lath_strokes") = std::vector<PointsIn>(),
        nb::arg("wallpaper_paths") = std::vector<PointsIn>(),
        nb::arg("wallpaper_normals").none() = nb::none(),
        nb::arg("fabric_holes") = std::vector<PointsIn>(),
        nb::arg("fabric_hole_normals").none() = nb::none(),
        nb::arg("fabric_edges") = std::vector<PointsIn>(),
        nb::arg("fabric_edge_normals").none() = nb::none(),
        nb::arg("bullet_strokes") = std::vector<PointsIn>(),
        nb::arg("ground_strokes") = std::vector<PointsIn>(),
        nb::arg("brick_strokes") = std::vector<PointsIn>(),
        nb::arg("fissure_strokes") = std::vector<PointsIn>(),
        nb::arg("rusty_strokes") = std::vector<PointsIn>(),
        nb::arg("glass_holes") = std::vector<PointsIn>(),
        nb::arg("glass_hole_normals").none() = nb::none(),
        nb::arg("glass_impacts") = std::vector<PointsIn>(),
        nb::arg("glass_impact_normals").none() = nb::none(),
        nb::arg("bevel_strokes") = std::vector<PointsIn>(),
        nb::arg("damaged_bevel_strokes") = std::vector<PointsIn>(),
        nb::arg("scratch_strokes") = std::vector<PointsIn>(),
        nb::arg("chip_strokes") = std::vector<PointsIn>(),
        nb::arg("chip_paint_radius") = 0.0,
        nb::arg("draw_mask_paths") = std::vector<PointsIn>(),
        nb::arg("draw_mask_radius") = 5.0,
        nb::arg("draw_mask_soft") = 0.5,
        nb::arg("draw_mask_invert") = false,
        nb::arg("draw_mask_size") = 0.0,
        nb::arg("draw_mask_heat") = true,
		nb::arg("rbf_cut_strokes") = std::vector<PointsIn>(),
		nb::arg("rbf_impact_strokes") = std::vector<PointsIn>(),
		nb::arg("fract_helper_strokes") = std::vector<PointsIn>(),
		nb::arg("cellf_strokes") = std::vector<PointsIn>(),
		nb::arg("src_corner_normals").none() = nb::none(),
		"Cut worn-edge damage into a mesh (float64 (N,3) points, int32 (M,4) "
		"polygons - ProjectDamagePoly/CPolygon convention, c==d for a triangle, index "
		"range-checked) using `params` (a Params instance) and return a dict: "
		"points (float64 (K,3)), polys (int32 (K2,4) - ProjectDamagePoly quads, "
		"(a,b,c,c) for a triangle), tri_class (int32 (K2,) per-polygon damage "
		"class in PROJECT_DAMAGE_SEL_COUNT slot order: 0 chips, 1-3 cracks L1-L3, 4 "
		"holes, 5 buildup, 6 base, 7 break), passthrough (bool - true means "
		"the input was not a closed manifold and points/polys/tri_class come "
		"back empty; the caller's OWN input mesh is the result), uvs (None "
		"when the call had no uvs input; otherwise float64 (K2,4,2), NaN on a "
		"triangle's unused 4th corner and on every damage polygon's corners - "
		"the caller must supply its own projection for those, mirroring the "
		"C4D adapter's ApplyDamageUVs cube projection), and counts "
		"(points, polys, chips) matching cpp/tests/_project_damage_test_util.py's "
		"counts() triple. topology == 3 (Keep) stamps per-polygon faceIDs "
		"exactly like the C4D adapter, so Keep-mode parity cases work "
		"standalone too. The GIL is released for the duration of the build "
		"(inputs are copied into C++ storage first), so other Python threads - "
		"the host's UI among them - keep running. `cache` (a Cache instance, "
		"default None) and `cache_fingerprint` (default 0) opt into the core's "
		"session damage cache: pass the same Cache across repeated calls with "
		"the fingerprint of everything except the breaker block to skip "
		"re-detection and re-placement on a hit. cache=None always misses, "
		"matching pre-cache behavior exactly. `uvs` (float64 (M,4,2), one row "
		"per input polygon, corners a/b/c/d - the 4th ignored for a triangle - "
		"default None) mirrors CToManifold's UV branch: a property vert per "
		"distinct (point, u, v), so a UV seam survives the Boolean without "
		"cross-seam averaging. None keeps the build byte-identical to before "
		"this parameter existed. `mask_weights` (float64 (N,), one entry per "
		"input point, default None) is this module's own damage mask: no "
		"Field concept exists on this surface, so the weight is sampled at "
		"the closest point on the input mesh's surface instead - the "
		"documented host difference (docs/superpowers/specs/"
		"2026-08-14-blender-mask-design.md); the SEMANTICS downstream of the "
		"weight (Keep/SizeScale/FadeScale) mirror C4D's FieldMask exactly. "
		"`mask_amount`/`mask_size` (default 1.0/0.0, mirroring "
		"PROJECT_DAMAGE_FIELD_AMOUNT/PROJECT_DAMAGE_FIELD_SIZE's own Init defaults) are the same "
		"two strengths: Amount drives discrete removal (chips/craters/corner "
		"craters/buildup piles), Size shrinks those same elements without "
		"removing them, and the buildup bead and fracture crack networks "
		"fade under the stronger of the two instead of being removed - "
		"linking a mask never MOVES an element, only removes or shrinks it. "
		"mask_weights=None (default) costs nothing and is byte-identical to "
		"no mask; weights with both knobs at zero are the same as no mask. "
		"`avoid_polys` (bool (M,), one entry per input polygon, default "
		"None) is the Global Avoid Selection surface (CoreInput::"
		"avoidPoly): a crack neither nucleates in a flagged polygon's region "
		"nor grows into it. `guide_splines` (a list of float64 (P,3) arrays, "
		"default empty) are the Fracture Guide Splines (CoreInput::"
		"guideSplines): one drawn level-1 crack path per array, in the mesh's "
		"own space. An empty list draws nothing from the RNG and is "
		"byte-identical to the module without the feature.");
}
