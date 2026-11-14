// дополнительные разрезы готовых осколков по нарисованным линиям
// SPDX-License-Identifier: GPL-3.0-or-later
// Finite, projected knife volumes cut existing cells; no seed-density approximation.
#include "peeling_py.h"
#include <nanobind/stl/vector.h>
#include "manifold/manifold.h"
#include <map>
#include <numeric>
#include <optional>
#include <nanobind/stl/optional.h>
namespace pc {
namespace m=manifold;using namespace pd_peeling_bind;
struct Out {std::vector<double> points,uv,normals,centers,volumes;std::vector<int32_t> polys,piece,base;int cuts=0,invalid=0;};
inline void appendOriginal(Out&o,const std::vector<int>&faces,D p,I f,I bp,D uv,D no,double volume,const m::vec3&center){
 const int id=o.volumes.size();std::map<int,int> ids;
 for(int face:faces){for(int j=0;j<4;++j){int v=f.data()[face*4+j];if(!ids.count(v)){ids[v]=o.points.size()/3;o.points.insert(o.points.end(),p.data()+3*v,p.data()+3*v+3);}o.polys.push_back(ids[v]);}
 o.piece.push_back(id);o.base.push_back(bp.data()[face]);o.uv.insert(o.uv.end(),uv.data()+face*8,uv.data()+face*8+8);o.normals.insert(o.normals.end(),no.data()+face*12,no.data()+face*12+12);}
 o.volumes.push_back(volume);o.centers.insert(o.centers.end(),{center.x,center.y,center.z});
}
inline void append(Out&o,const m::Manifold&part,uint32_t sourceID,I bp,double uvScale){
 auto mesh=part.GetMeshGL64();const int id=o.volumes.size(),nv=mesh.NumVert();std::vector<int> parent(nv);std::iota(parent.begin(),parent.end(),0);
 auto root=[&](int x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
 for(size_t i=0;i<mesh.mergeFromVert.size();++i)parent[root(mesh.mergeFromVert[i])]=root(mesh.mergeToVert[i]);
 std::map<int,int> ids;std::vector<int> map(nv);m::vec3 sum(0.0);int n=0;
 for(int v=0;v<nv;++v){int r=root(v);if(!ids.count(r)){ids[r]=o.points.size()/3;auto*p=&mesh.vertProperties[size_t(r)*mesh.numProp];o.points.insert(o.points.end(),p,p+3);sum+=m::vec3(p[0],p[1],p[2]);++n;}map[v]=ids[r];}
 size_t run=0;
 for(size_t t=0;t<mesh.NumTri();++t){while(run+1<mesh.runOriginalID.size()&&3*t>=mesh.runIndex[run+1])++run;
 bool original=run<mesh.runOriginalID.size()&&mesh.runOriginalID[run]==sourceID;
 int fid=mesh.faceID[t];original=original&&fid>=0&&size_t(fid)<bp.shape(0);
 m::vec3 tri[3];for(int j=0;j<3;++j){auto v=mesh.triVerts[3*t+j];auto*p=&mesh.vertProperties[size_t(v)*mesh.numProp];tri[j]=m::vec3(p[0],p[1],p[2]);o.polys.push_back(map[v]);}o.polys.push_back(o.polys.back());
 m::vec3 normal=m::la::cross(tri[1]-tri[0],tri[2]-tri[0]);double len=m::la::length(normal);normal=len>0?normal/len:m::vec3(0,0,1);
 int axis=0;for(int j=1;j<3;++j)if(std::abs(normal[j])>std::abs(normal[axis]))axis=j;
 for(int j=0;j<4;++j){int k=std::min(j,2);auto v=mesh.triVerts[3*t+k];auto*p=&mesh.vertProperties[size_t(v)*mesh.numProp];
 if(original&&mesh.numProp>=8){o.uv.insert(o.uv.end(),{p[3],p[4]});m::vec3 n(p[5],p[6],p[7]);double l=m::la::length(n);n=l>1e-15?n/l:normal;o.normals.insert(o.normals.end(),{n.x,n.y,n.z});}
 else{o.uv.insert(o.uv.end(),{tri[k][(axis+1)%3]/uvScale,tri[k][(axis+2)%3]/uvScale});o.normals.insert(o.normals.end(),{normal.x,normal.y,normal.z});}}
 o.piece.push_back(id);o.base.push_back(original?bp.data()[fid]:-1);
 }
 m::vec3 centroid(0.0);double signedVolume=0.0;
 for(size_t t=0;t<mesh.NumTri();++t){m::vec3 v[3];for(int j=0;j<3;++j){auto*p=&mesh.vertProperties[size_t(mesh.triVerts[t*3+j])*mesh.numProp];v[j]=m::vec3(p[0],p[1],p[2]);}double vol=m::la::dot(v[0],m::la::cross(v[1],v[2]))/6.0;signedVolume+=vol;centroid+=(v[0]+v[1]+v[2])*(vol*.25);}
 auto c=std::abs(signedVolume)>1e-20?centroid/signedVolume:sum/double(std::max(n,1));o.centers.insert(o.centers.end(),{c.x,c.y,c.z});o.volumes.push_back(std::abs(part.Volume()));
}
inline Out cut(D p,I f,I ids,I bp,D uv,D no,const std::vector<D>&cp,const std::vector<I>&ct,
 const std::vector<I>&targets,double uvScale,const std::optional<D>& previousCenters,
 const std::optional<D>& previousVolumes){
 if(previousCenters.has_value()!=previousVolumes.has_value() ||
    (previousCenters && (previousCenters->ndim()!=2 || previousCenters->shape(1)!=3 ||
    previousVolumes->ndim()!=1 || previousCenters->shape(0)!=previousVolumes->shape(0))))
     throw std::invalid_argument("Cell cuts: inconsistent original piece metadata");
 if(p.ndim()!=2||p.shape(1)!=3||f.ndim()!=2||f.shape(1)!=4||ids.size()!=f.shape(0)||bp.size()!=ids.size()||uv.size()!=ids.size()*8||no.size()!=ids.size()*12||cp.size()!=ct.size()||cp.size()!=targets.size())throw std::invalid_argument("Cell cuts: inconsistent mesh arrays");
 std::vector<m::Manifold> cutters;for(size_t i=0;i<cp.size();++i){m::MeshGL64 mesh;mesh.numProp=3;mesh.vertProperties.assign(cp[i].data(),cp[i].data()+cp[i].size());mesh.triVerts.assign(ct[i].data(),ct[i].data()+ct[i].size());m::Manifold c(mesh);if(c.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Cell cuts: invalid closed knife volume");cutters.push_back(c);}
 std::map<int,std::vector<int>> byPiece;for(size_t i=0;i<ids.size();++i)byPiece[ids.data()[i]].push_back(i);Out out;
 for(auto&entry:byPiece){int piece=entry.first;auto&faces=entry.second;std::vector<m::Manifold> selected;
 for(size_t i=0;i<cutters.size();++i){bool use=false;for(size_t j=0;j<targets[i].size();++j)if(targets[i].data()[j]<0||targets[i].data()[j]==piece){use=true;break;}if(use)selected.push_back(cutters[i]);}
 const bool hasOriginal=previousCenters && piece>=0 && size_t(piece)<previousCenters->shape(0);
 m::vec3 centre(0.0);
 double originalVolume=0.0;
 if(hasOriginal){
  const double* c=previousCenters->data()+3*piece;
  centre={c[0],c[1],c[2]};
  originalVolume=previousVolumes->data()[piece];
  if(selected.empty()){
   appendOriginal(out,faces,p,f,bp,uv,no,originalVolume,centre);
   continue;
  }
 }
 m::MeshGL64 mesh;mesh.numProp=8;uint32_t sourceID=m::Manifold::ReserveIDs(1);std::map<int,int> first;
 for(int face:faces){int count=f.data()[4*face+2]==f.data()[4*face+3]?3:4;int corners[6]={0,1,2,0,2,3};for(int k=0;k<(count==3?3:6);++k){int j=corners[k],v=f.data()[4*face+j];if(v<0||size_t(v)>=p.shape(0))throw std::invalid_argument("Cell cuts: vertex outside mesh");int idx=mesh.NumVert();auto*q=p.data()+3*v;auto*u=uv.data()+8*face+2*j;auto*n=no.data()+12*face+3*j;mesh.vertProperties.insert(mesh.vertProperties.end(),{q[0],q[1],q[2],u[0],u[1],n[0],n[1],n[2]});mesh.triVerts.push_back(idx);if(first.count(v)){mesh.mergeFromVert.push_back(idx);mesh.mergeToVert.push_back(first[v]);}else first[v]=idx;if(k%3==0)mesh.faceID.push_back(face);}}
 mesh.runIndex={0,uint32_t(mesh.triVerts.size())};mesh.runOriginalID={sourceID};m::Manifold solid(mesh);
                                                                          
                                                                           
 if(solid.Status()!=m::Manifold::Error::NoError){mesh.Merge();solid=m::Manifold(mesh);}
 const bool closed=solid.Status()==m::Manifold::Error::NoError && !solid.IsEmpty();
 if(!hasOriginal){
  if(closed){
   const auto bounds=solid.BoundingBox();
   centre=(bounds.min+bounds.max)*.5;
   originalVolume=std::abs(solid.Volume());
  } else if(!first.empty()){
   for(const auto& entry:first){
    const double* q=p.data()+3*entry.first;
    centre+=m::vec3(q[0],q[1],q[2]);
   }
   centre/=double(first.size());
  }
 }
 if(selected.empty()||!closed){
  if(!selected.empty())++out.invalid;
  appendOriginal(out,faces,p,f,bp,uv,no,originalVolume,centre);
  continue;
 }
 auto cutter=m::Manifold::BatchBoolean(selected,m::OpType::Add);auto result=solid-cutter;
                                                                           
                                                                   
 if(cutter.Status()!=m::Manifold::Error::NoError || result.Status()!=m::Manifold::Error::NoError){
   result=solid;
   for(const auto& knife:selected){auto next=result-knife;if(next.Status()==m::Manifold::Error::NoError&&!next.IsEmpty())result=next;}
 }
 if(result.Status()!=m::Manifold::Error::NoError||result.IsEmpty()){++out.invalid;appendOriginal(out,faces,p,f,bp,uv,no,originalVolume,centre);continue;}
 double volume=std::abs(solid.Volume());if(std::abs(volume-std::abs(result.Volume()))<std::max(1e-22,volume*1e-12)){appendOriginal(out,faces,p,f,bp,uv,no,originalVolume,centre);continue;}
                                                                       
                                                                          
 auto parts=result.Decompose();size_t significant=0;
 for(const auto&part:parts)if(std::abs(part.Volume())>volume*1e-8)++significant;
 if(significant<2){appendOriginal(out,faces,p,f,bp,uv,no,originalVolume,centre);continue;}
 ++out.cuts;for(auto&part:parts)if(std::abs(part.Volume())>volume*1e-12)append(out,part,sourceID,bp,std::max(uvScale,1e-12));
 }
 return out;
}
}
#include "inner_cutter_bool.h"
#include <tbb/global_control.h>
#include <cstdlib>
NB_MODULE(project_damage_cellcuts,mod){
                                                                                    
    const char* background = std::getenv("PD_PRIVATE_WORKER");
    if (background && background[0]=='1') {
        static tbb::global_control worker_limit(tbb::global_control::max_allowed_parallelism, 1);
    }

 pd_inner_cut::bind(mod);
 using namespace pc;mod.def("slice_cells",[](D p,I f,I ids,I bp,D uv,D no,std::vector<D> cp,std::vector<I> ct,std::vector<I> targets,double uvScale,
    std::optional<D> centres,std::optional<D> volumes){
 Out o;{nanobind::gil_scoped_release release;o=cut(p,f,ids,bp,uv,no,cp,ct,targets,uvScale,centres,volumes);}
 nanobind::dict d;d["cellf_points"]=array(std::move(o.points),3);d["cellf_polys"]=array(std::move(o.polys),4);d["cellf_piece"]=array(std::move(o.piece));d["cellf_base_poly"]=array(std::move(o.base));
 d["cellf_uvs"]=array(std::move(o.uv),8);d["cellf_normals"]=array(std::move(o.normals),12);d["cellf_piece_center"]=array(std::move(o.centers),3);d["cellf_piece_volume"]=array(std::move(o.volumes));d["cut_cells"]=o.cuts;d["invalid_cells"]=o.invalid;return d;
 }, nanobind::arg("points"),nanobind::arg("faces"),nanobind::arg("piece_ids"),nanobind::arg("base_faces"),
    nanobind::arg("uv"),nanobind::arg("normals"),nanobind::arg("cutter_points"),nanobind::arg("cutter_faces"),
    nanobind::arg("targets"),nanobind::arg("uv_scale"),
    nanobind::arg("previous_centres")=nanobind::none(),nanobind::arg("previous_volumes")=nanobind::none());
}
