// общие объявления: проверяем, какие участки объёма затронуты повреждением
#pragma once
#include "damage_point.h"
#include <memory>
#include <stdexcept>

PROJECT_DAMAGE_NS_MACRO_BEGIN
struct DamageObjectVolume {
    manifold::Manifold solid;
    manifold::Box bounds;
    std::vector<EVec> probes;
    EVec center;
    double extent=1.0;
    explicit DamageObjectVolume(const SurfaceDamagePoint& data) {
        manifold::MeshGL64 mesh;mesh.numProp=3;mesh.vertProperties=data.mesh_points;
        for(auto index:data.mesh_triangles)mesh.triVerts.push_back(uint64_t(index));
        solid=manifold::Manifold(mesh);
        if(solid.Status()!=manifold::Manifold::Error::NoError || solid.IsEmpty())throw std::runtime_error("Damage object must be a closed mesh with consistent normals");
        if(solid.Volume()<0) {
            for(size_t i=0;i<mesh.triVerts.size();i+=3)std::swap(mesh.triVerts[i+1],mesh.triVerts[i+2]);
            solid=manifold::Manifold(mesh);
        }
        bounds=solid.BoundingBox();center=(bounds.min+bounds.max)*0.5;
        extent=std::max(1e-4,manifold::la::length(bounds.max-bounds.min));
        probes.push_back(center);
        const size_t count=data.mesh_triangles.size()/3,stride=std::max(size_t(1),count/24);
        for(size_t face=0;face<count;face+=stride) {
            EVec position(0.0);
            for(int j=0;j<3;++j) {
                size_t i=size_t(data.mesh_triangles[face*3+j])*3;
                position+=EVec(data.mesh_points[i],data.mesh_points[i+1],data.mesh_points[i+2]);
            }
            probes.push_back(position/3.0);
        }
    }
    bool Hits(const EVec& low,const EVec& high) const {
        return !(high.x<bounds.min.x||low.x>bounds.max.x||high.y<bounds.min.y||low.y>bounds.max.y||high.z<bounds.min.z||low.z>bounds.max.z);
    }
    bool Inside(const EVec& point) const {
        const EVec direction(0.613,0.371,0.699);
        auto hits=solid.RayCast(point,point+direction*(4.0*extent));
        if(hits.empty())return false;
        auto first=std::min_element(hits.begin(),hits.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
        return manifold::la::dot(first->normal,direction)>0.0;
    }
    double Depth(const EVec& point,const EVec& normal) const {
        const double epsilon=std::max(1e-7,extent*1e-7);
        if(!Inside(point-normal*epsilon))return 0.0;
        const EVec start=point+normal*epsilon,direction=-normal;
        const double span=4.0*extent+2.0*epsilon;
        auto hits=solid.RayCast(start,start+direction*span);
        double depth=1e300;
        for(const auto& hit:hits)if(manifold::la::dot(hit.normal,direction)>1e-6)
            depth=std::min(depth,hit.distance*span-epsilon);
        return depth<1e299?std::max(0.0,depth):0.0;
    }
    bool Exposes(const manifold::Manifold& mesh) const {
        if(mesh.IsEmpty())return false;
        const auto b=mesh.BoundingBox();if(!Hits(b.min,b.max))return false;
        auto overlap=mesh^solid;
        return overlap.Status()==manifold::Manifold::Error::NoError&&!overlap.IsEmpty();
    }
};

struct ObjectDamageField {std::vector<double> depth,distance;};
template<class Grid,class Patch> ObjectDamageField SampleDamageObject(const Grid& grid,const Patch& patch,
    double u0,double v0,double du,double dv,const std::vector<double>* us=nullptr,const std::vector<double>* vs=nullptr) {
    ObjectDamageField out;if(!patch.volume)return out;
    const size_t count=grid.h.size();out.depth.assign(count,0.0);out.distance.assign(count,1e30);
    std::vector<char> inside(count,0);
    for(int i=0;i<=grid.nu;++i)for(int j=0;j<=grid.nv;++j) {
        const auto k=grid.I(i,j);if(!grid.valid[k])continue;
        const double u=us?(*us)[size_t(i)]:u0+i*du,v=vs?(*vs)[size_t(j)]:v0+j*dv;
        const EVec surface=patch.o+patch.t*u+patch.b*v+patch.n*grid.h[k];
        out.depth[k]=patch.volume->Depth(surface,patch.n);inside[k]=out.depth[k]>1e-7;
    }
    const double step=std::min(du,dv),diagonal=std::hypot(du,dv);
    for(int i=0;i<=grid.nu;++i)for(int j=0;j<=grid.nv;++j) {
        const auto k=grid.I(i,j);
        for(int di=-1;di<=1;++di)for(int dj=-1;dj<=1;++dj) {
            if(std::abs(di)+std::abs(dj)!=1)continue;
            int a=i+di,b=j+dj;
            if(a>=0&&a<=grid.nu&&b>=0&&b<=grid.nv&&inside[k]!=inside[grid.I(a,b)])out.distance[k]=0.5*step;
        }
    }
    for(int pass=0;pass<2;++pass) {
        for(int ii=0;ii<=grid.nu;++ii)for(int jj=0;jj<=grid.nv;++jj) {
            int i=pass?grid.nu-ii:ii,j=pass?grid.nv-jj:jj;
            const auto k=grid.I(i,j);
            for(int di=-1;di<=1;++di)for(int dj=-1;dj<=1;++dj) {
                if(!di&&!dj)continue;
                int a=i+di,b=j+dj;if(a<0||a>grid.nu||b<0||b>grid.nv)continue;
                const auto q=grid.I(a,b);if(inside[k]!=inside[q])continue;
                out.distance[k]=std::min(out.distance[k],out.distance[q]+(di&&dj?diagonal:di?du:dv));
            }
        }
    }
    for(size_t k=0;k<count;++k)out.distance[k]=(inside[k]?1.0:-1.0)*std::min(out.distance[k],patch.volume->extent);
    return out;
}

template<class Surf,class Patch,class Frame> void AppendDamageObjectPatches(const Surf& surface,
    const std::shared_ptr<DamageObjectVolume>& volume,double depth,std::vector<Patch>& patches,Frame frame) {
    const auto& mesh=*surface.v;
    const double radius=1.5*volume->extent;
    std::vector<Patch> added;
    for(int32_t i=0;i<mesh.npolys;++i) {
        if(surface.Avoided(i))continue;
        const auto& f=mesh.polys[i];
        EVec low=manifold::la::min(mesh.pts[f.a],manifold::la::min(mesh.pts[f.b],mesh.pts[f.c]));
        EVec high=manifold::la::max(mesh.pts[f.a],manifold::la::max(mesh.pts[f.b],mesh.pts[f.c]));
        if(f.c!=f.d){low=manifold::la::min(low,mesh.pts[f.d]);high=manifold::la::max(high,mesh.pts[f.d]);}
        if(!volume->Hits(low,high))continue;
        const EVec normal=surface.polyN[size_t(i)];
        bool covered=false;
        for(const auto& old:added)if(manifold::la::dot(old.n,normal)>0.94 && std::abs(manifold::la::dot(mesh.pts[f.a]-old.o,normal))<0.03*volume->extent){covered=true;break;}
        if(covered)continue;
        EVec origin;bool found=false;
        for(const auto& probe:volume->probes) {
            EVec candidate=probe-normal*manifold::la::dot(probe-mesh.pts[f.a],normal);
            double distance=0.;int32_t face=-1;
            if(!surface.Ray(candidate+normal*volume->extent,-normal,2.0*volume->extent,distance,face))continue;
            if(manifold::la::dot(surface.polyN[size_t(face)],normal)<0.7)continue;
            candidate+=normal*(volume->extent-distance);
            if(!volume->Inside(candidate))continue;
            origin=candidate;found=true;break;
        }
        if(!found)continue;
        Patch patch;patch.volume=volume;patch.id=patches.size()+added.size()+1;
        patch.o=origin;patch.n=normal;frame(normal,patch.t,patch.b);
        patch.u0=patch.v0=1e300;patch.u1=patch.v1=-1e300;
        for(int corner=0;corner<8;++corner) {
            EVec vertex((corner&1)?volume->bounds.max.x:volume->bounds.min.x,
                        (corner&2)?volume->bounds.max.y:volume->bounds.min.y,
                        (corner&4)?volume->bounds.max.z:volume->bounds.min.z);
            const EVec delta=vertex-origin;const double u=manifold::la::dot(delta,patch.t),v=manifold::la::dot(delta,patch.b);
            patch.u0=std::min(patch.u0,u);patch.u1=std::max(patch.u1,u);patch.v0=std::min(patch.v0,v);patch.v1=std::max(patch.v1,v);
        }
        patch.R=patch.Rn=0.5*std::min(patch.u1-patch.u0,patch.v1-patch.v0);patch.D=depth;
        patch.blobs.push_back({0.0,0.0,radius});patch.cracks=0;patch.hole=false;patch.off=EVec(17.1,43.2,9.3);
        added.push_back(std::move(patch));
    }
    for(auto& patch:added)patches.push_back(std::move(patch));
}
inline void FilterDamageObjectGeometry(std::vector<manifold::Manifold>& meshes,const DamageObjectVolume& volume) {
    meshes.erase(std::remove_if(meshes.begin(),meshes.end(),[&](const auto& mesh){return !volume.Exposes(mesh);}),meshes.end());
}
PROJECT_DAMAGE_NS_MACRO_END
