// общие объявления: режем соседние части общими неровными поверхностями
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared noisy-grid cutters, following the Boolean workflow in Destruction Tools.
// Repartition a local union instead of independently displacing fragment normals.
#pragma once
namespace pd_inner_cut {
namespace m=manifold;using namespace pd_peeling_bind;
struct Result {std::vector<double> points,weights;std::vector<int32_t> triangles,owners,faces;int changed=0,skipped=0;};
inline m::Manifold cutter(D p,I t){
 if(p.ndim()!=2||p.shape(1)!=3||t.ndim()!=2||t.shape(1)!=3)throw std::invalid_argument("Noisy Cutters: invalid cutter shape");
 m::MeshGL64 me;me.numProp=3;me.vertProperties.assign(p.data(),p.data()+p.size());me.triVerts.assign(t.data(),t.data()+t.size());
 m::Manifold solid(me);if(solid.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Noisy Cutters: cutter is not a closed solid");return solid;
}
                                                                           
                                                                                 
inline m::Manifold compact(const m::Manifold& solid){
 auto mesh=solid.GetMeshGL64();
 mesh.runOriginalID={m::Manifold::ReserveIDs(1)};mesh.runIndex={0,uint32_t(mesh.triVerts.size())};mesh.runTransform.clear();mesh.runFlags.clear();
 return m::Manifold(mesh);
}
inline Result run(D p,I tris,I owners,const std::vector<D>&hp,const std::vector<I>&ht,const std::vector<D>&bp,const std::vector<I>&bt,I pairs,int budget){
 if(p.ndim()!=2||p.shape(1)!=3||tris.ndim()!=2||tris.shape(1)!=3||owners.size()!=tris.shape(0)||pairs.ndim()!=2||pairs.shape(1)!=2||pairs.shape(0)!=hp.size()||hp.size()!=ht.size()||hp.size()!=bp.size()||hp.size()!=bt.size())throw std::invalid_argument("Noisy Cutters: inconsistent input arrays");
 std::map<int,std::vector<int>> groups;for(size_t f=0;f<owners.size();++f)groups[owners.data()[f]].push_back(f);
 std::map<int,m::Manifold> solids;
 for(auto&[owner,faces]:groups){
  m::MeshGL64 me;me.numProp=8;std::map<int,int> first;
  for(int f:faces){me.faceID.push_back(f);for(int j=0;j<3;++j){int v=tris.data()[3*f+j];if(v<0||size_t(v)>=p.shape(0))throw std::invalid_argument("Noisy Cutters: vertex out of bounds");
   int ix=me.NumVert();me.triVerts.push_back(ix);auto*q=p.data()+3*v;me.vertProperties.insert(me.vertProperties.end(),{q[0],q[1],q[2],j==0?1.:0.,j==1?1.:0.,j==2?1.:0.,double(f),1.});
   if(first.count(v)){me.mergeFromVert.push_back(ix);me.mergeToVert.push_back(first[v]);}else first[v]=ix;
  }}
  me.runOriginalID={m::Manifold::ReserveIDs(1)};me.runIndex={0,uint32_t(me.triVerts.size())};m::Manifold solid(me);
  if(solid.Status()!=m::Manifold::Error::NoError){me.Merge();solid=m::Manifold(me);}
  if(solid.Status()!=m::Manifold::Error::NoError)throw std::runtime_error("Noisy Cutters: a fragment is not a closed manifold");
  solids.emplace(owner,solid);
 }
 Result out;
 for(size_t i=0;i<hp.size();++i){int a=pairs.data()[2*i],b=pairs.data()[2*i+1];if(!solids.count(a)||!solids.count(b)||a==b){++out.skipped;continue;}
  const auto&A=solids.at(a);const auto&B=solids.at(b);auto half=cutter(hp[i],ht[i]),box=cutter(bp[i],bt[i]);
  auto domain=(A+B)^box;auto parts=domain.Split(half);
  auto na=(A-box)+parts.first;auto nb=(B-box)+parts.second;
  double vol=std::abs(A.Volume())+std::abs(B.Volume());
  if(na.Status()!=m::Manifold::Error::NoError||nb.Status()!=m::Manifold::Error::NoError||na.IsEmpty()||nb.IsEmpty()||
     std::abs(na.Volume())<1e-6*std::abs(A.Volume())||std::abs(nb.Volume())<1e-6*std::abs(B.Volume())||
     std::abs(std::abs(na.Volume())+std::abs(nb.Volume())-vol)>std::max(1e-12,vol*1e-6)) {++out.skipped;continue;}
  if(na.NumTri()+nb.NumTri()>budget)throw std::runtime_error("Noisy Cutters: face budget exceeded; lower Resolution");
  solids.at(a)=compact(na);solids.at(b)=compact(nb);++out.changed;
 }
 for(auto&[owner,solid]:solids){auto mesh=solid.GetMeshGL64();int nv=mesh.NumVert();std::vector<int> parent(nv);std::iota(parent.begin(),parent.end(),0);
  auto root=[&](int x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
  for(size_t j=0;j<mesh.mergeFromVert.size();++j)parent[root(mesh.mergeFromVert[j])]=root(mesh.mergeToVert[j]);
  std::map<int,int> remap;std::vector<int> map(nv);
  for(int j=0;j<nv;++j){int r=root(j);if(!remap.count(r)){remap[r]=out.points.size()/3;auto*q=&mesh.vertProperties[size_t(r)*mesh.numProp];out.points.insert(out.points.end(),q,q+3);}map[j]=remap[r];}
  size_t run=0;
  for(size_t f=0;f<mesh.NumTri();++f){while(run+1<mesh.runOriginalID.size()&&3*f>=mesh.runIndex[run+1])++run;
   auto*fp=&mesh.vertProperties[size_t(mesh.triVerts[3*f])*mesh.numProp];
   bool original=mesh.numProp>=8&&fp[7]>.5;
   out.faces.push_back(original?int32_t(std::llround(fp[6])):-1);out.owners.push_back(owner);
   for(int j=0;j<3;++j){int v=mesh.triVerts[3*f+j];out.triangles.push_back(map[v]);auto*q=&mesh.vertProperties[size_t(v)*mesh.numProp];
    for(int k=0;k<3;++k)out.weights.push_back(original&&mesh.numProp>=6?q[3+k]:0.);
   }
  }
 }
 return out;
}
inline void bind(nanobind::module_&mod){mod.def("noisy_boolean_interfaces",[](D p,I t,I o,std::vector<D> hp,std::vector<I> ht,std::vector<D> bp,std::vector<I> bt,I pairs,int budget){
 Result r;{nanobind::gil_scoped_release release;r=run(p,t,o,hp,ht,bp,bt,pairs,budget);}
 nanobind::dict d;d["points"]=array(std::move(r.points),3);d["triangles"]=array(std::move(r.triangles),3);d["owners"]=array(std::move(r.owners));d["faces"]=array(std::move(r.faces));d["weights"]=array(std::move(r.weights),3);d["interfaces"]=r.changed;d["skipped"]=r.skipped;return d;
});}
}
