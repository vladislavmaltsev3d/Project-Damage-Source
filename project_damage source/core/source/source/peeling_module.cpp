// подключаем расчёт Surface Peel к Python
// SPDX-License-Identifier: GPL-3.0-or-later
// Separate module permits loading Peeling into an already running Blender
// without replacing the native module used by existing damage sessions.
#include "peeling_py.h"
#include "inner_matched_py.h"
#include "peeling_full.h"
#include <nanobind/stl/vector.h>

namespace {
namespace nb=nanobind;
using namespace pd_peeling_bind;
using FA=nb::ndarray<const float,nb::c_contig,nb::device::cpu>;
template<typename T,class A> std::vector<T> flat(A a){return std::vector<T>(a.data(),a.data()+a.size());}
pd_peeling_full::F3 f3(nb::handle h){nb::sequence s=nb::cast<nb::sequence>(h);return {nb::cast<float>(s[0]),nb::cast<float>(s[1]),nb::cast<float>(s[2])};}
pd_peeling_full::PassParams readPass(nb::dict d){
 pd_peeling_full::PassParams p;
 auto num=[&](const char*k){return nb::cast<double>(d[k]);};auto flag=[&](const char*k){return nb::cast<bool>(d[k]);};auto whole=[&](const char*k){return nb::cast<int>(d[k]);};
 p.size=num("size");p.detail=num("detail");p.step=num("step");p.bend=num("bend");p.depth=num("depth");p.thickness=num("thickness");p.lift=num("lift");
 p.variation=num("variation");p.curl=num("curl");p.tongues=num("tongues");p.tears=num("tears");p.droop=num("droop");p.small_amount=num("small_amount");
 p.ragged=num("ragged");p.threshold=num("threshold");p.plates=num("plates");p.rough=num("rough");
 p.offset=f3(d["offset"]);
 {nb::sequence o=nb::cast<nb::sequence>(d["O"]);for(size_t i=0;i<12;++i)p.O[i]=f3(o[i]);}
 {nb::sequence o=nb::cast<nb::sequence>(d["rim_offset"]);for(size_t i=0;i<3;++i)p.rim_offset[i]=f3(o[i]);}
 p.floor_mat=whole("floor_mat");p.back_mat=whole("back_mat");p.bridge_mat=whole("bridge_mat");p.crack_mat=whole("crack_mat");
 p.mark_paper=flag("mark_paper");p.edge_all=flag("edge_all");p.paper_only=flag("paper_only");p.use_drawing=flag("use_drawing");p.use_group=flag("use_group");p.wrap=flag("wrap");p.damage=num("damage");
 return p;
}
void BindPeelingFull(nb::module_&mod){
 mod.def("peeling_generate",[](FA points,I polyOffsets,I cornerVert,I triCorners,I triPoly,I material,I smooth,I stamp,I edges,I edgeSharp,
                               FA groupWeight,FA guided,FA gravity,nb::dict main,nb::object paper,nb::object drawing,bool adaptive,int seed,int budget,nb::object guide){
  pd_peeling_full::Input in;
  in.points=flat<float>(points);in.poly_offsets=flat<int32_t>(polyOffsets);in.corner_vert=flat<int32_t>(cornerVert);in.tri_corners=flat<int32_t>(triCorners);in.tri_poly=flat<int32_t>(triPoly);
  in.poly_material=flat<int32_t>(material);in.poly_smooth=flat<int32_t>(smooth);in.poly_stamp=flat<int32_t>(stamp);in.edges=flat<int32_t>(edges);in.edge_sharp=flat<int32_t>(edgeSharp);
  in.group_weight=flat<float>(groupWeight);in.guided_face_normals=flat<float>(guided);in.gravity=flat<float>(gravity);
  const size_t nv=in.points.size()/3;const size_t nc=in.corner_vert.size();
  if(in.points.size()%3||in.gravity.size()!=3||in.poly_offsets.empty()||in.tri_corners.size()!=in.tri_poly.size()*3||in.edges.size()%2||in.edge_sharp.size()*2!=in.edges.size())throw std::invalid_argument("Peeling: invalid mesh arrays");
  const size_t np=in.poly_offsets.size()-1;
  if(in.poly_material.size()!=np||in.poly_smooth.size()!=np||in.poly_stamp.size()!=np||(!in.group_weight.empty()&&in.group_weight.size()!=nv)||size_t(in.poly_offsets.back())!=nc||in.poly_offsets.front()!=0)throw std::invalid_argument("Peeling: invalid mesh arrays");
  for(int32_t v:in.corner_vert)if(v<0||size_t(v)>=nv)throw std::invalid_argument("Peeling: invalid vertex index");
  for(int32_t c:in.tri_corners)if(c<0||size_t(c)>=nc)throw std::invalid_argument("Peeling: invalid corner index");
  for(int32_t p:in.tri_poly)if(p<0||size_t(p)>=np)throw std::invalid_argument("Peeling: invalid polygon index");
  for(int32_t v:in.edges)if(v<0||size_t(v)>=nv)throw std::invalid_argument("Peeling: invalid edge index");
  for(size_t i=0;i<np;++i)if(in.poly_offsets[i]>in.poly_offsets[i+1]||in.poly_offsets[i+1]-in.poly_offsets[i]<3)throw std::invalid_argument("Peeling: invalid polygon");
  in.main=readPass(main);in.adaptive=adaptive;in.seed=seed;in.budget=budget;
  if(!paper.is_none()){in.wallpaper=true;in.paper=readPass(nb::cast<nb::dict>(paper));}
  if(!drawing.is_none()){
   nb::dict d=nb::cast<nb::dict>(drawing);in.has_drawing=true;
   in.drawing.points=flat<double>(nb::cast<D>(d["points"]));in.drawing.offsets=flat<int32_t>(nb::cast<I>(d["offsets"]));
   in.drawing.areas=nb::cast<bool>(d["areas"]);in.drawing.close_strokes=nb::cast<bool>(d["close_strokes"]);in.drawing.radius=nb::cast<double>(d["radius"]);in.drawing.snap=nb::cast<double>(d["snap"]);
   if(in.drawing.points.size()%3||in.drawing.offsets.empty()||in.drawing.offsets.front()!=0||size_t(in.drawing.offsets.back())*3!=in.drawing.points.size())throw std::invalid_argument("Peeling: invalid drawing arrays");
  }
  if(!guide.is_none())in.guide=[guide](const std::vector<double>&p,const std::vector<int32_t>&q){
   nb::gil_scoped_acquire lock;std::vector<double> pc=p;std::vector<int32_t> qc=q;
   nb::object r=guide(array(std::move(pc),3),array(std::move(qc),4));auto a=nb::cast<D>(r);
   return std::vector<double>(a.data(),a.data()+a.size());
  };
  pd_peeling_full::Output out;
  {nb::gil_scoped_release release;out=pd_peeling_full::Generate(in);}
  in.guide=nullptr;
  nb::dict r;r["faces"]=out.faces;r["limited"]=out.limited;r["contour_edges"]=out.contour_edges;r["small_patches"]=out.small_patches;r["tears"]=out.tears;
  r["drawing_empty"]=out.drawing_empty;if(out.has_paper_stats)r["paper_contour_edges"]=out.paper_contour_edges;
  if(out.faces){
   r["points"]=array(std::move(out.points),3);r["vert_src"]=array(std::move(out.vert_src),3);r["vert_w"]=array(std::move(out.vert_w),3);
   r["poly_offsets"]=array(std::move(out.poly_offsets));r["corner_vert"]=array(std::move(out.corner_vert));r["corner_edge"]=array(std::move(out.corner_edge));
   r["corner_src"]=array(std::move(out.corner_src),3);r["corner_w"]=array(std::move(out.corner_w),3);
   r["face_src"]=array(std::move(out.face_src));r["material"]=array(std::move(out.material));r["smooth"]=array(std::move(out.smooth));r["stamp"]=array(std::move(out.stamp));
   r["edges"]=array(std::move(out.edges),2);r["edge_src"]=array(std::move(out.edge_src));r["edge_sharp"]=array(std::move(out.edge_sharp));
  }
  return r;
 },nb::arg("points"),nb::arg("poly_offsets"),nb::arg("corner_vert"),nb::arg("tri_corners"),nb::arg("tri_poly"),nb::arg("material"),nb::arg("smooth"),nb::arg("stamp"),nb::arg("edges"),nb::arg("edge_sharp"),
    nb::arg("group_weight"),nb::arg("guided_face_normals"),nb::arg("gravity"),nb::arg("main"),nb::arg("paper").none(),nb::arg("drawing").none(),nb::arg("adaptive"),nb::arg("seed"),nb::arg("budget"),nb::arg("guide").none());
 mod.def("peeling_restore_normals",[](FA oldPoints,I oldTris,FA oldNormals,FA points,I polyOffsets,I cornerVert,I stamp,FA current){
  auto a=flat<float>(oldPoints);auto b=flat<int32_t>(oldTris);auto c=flat<float>(oldNormals);auto d=flat<float>(points);auto e=flat<int32_t>(polyOffsets);auto f=flat<int32_t>(cornerVert);auto g=flat<int32_t>(stamp);auto h=flat<float>(current);
  if(b.size()*3!=c.size()||h.size()!=f.size()*3||e.empty()||e.size()!=g.size()+1)throw std::invalid_argument("Peeling: invalid normal arrays");
  for(int32_t i:b)if(i<0||size_t(i)*3>=a.size())throw std::invalid_argument("Peeling: invalid triangle index");
  for(int32_t i:f)if(i<0||size_t(i)*3>=d.size())throw std::invalid_argument("Peeling: invalid vertex index");
  std::vector<float> r;{nb::gil_scoped_release release;r=pd_peeling_full::RestoreNormals(a,b,c,d,e,f,g,h);}return array(std::move(r),3);
 });
 // Pins of the procedural kernels against mathutils.noise, used by the tests.
 mod.def("peeling_noise",[](FA pts){std::vector<double> r(pts.size()/3);for(size_t i=0;i<r.size();++i)r[i]=pd_peeling_full::Noise(pts.data()[3*i],pts.data()[3*i+1],pts.data()[3*i+2]);return array(std::move(r));});
 mod.def("peeling_voronoi",[](FA pts){size_t n=pts.size()/3;std::vector<float> r(n*16);for(size_t i=0;i<n;++i)pd_peeling_full::Voronoi(pts.data()[3*i],pts.data()[3*i+1],pts.data()[3*i+2],&r[16*i],&r[16*i+4]);return array(std::move(r),16);});
 mod.def("peeling_hash",[](std::vector<int64_t> v){return pd_peeling_full::HashUnit(v);});
 mod.attr("peeling_full_native")=1;
}
}
NB_MODULE(project_damage_peeling,mod){BindPeeling(mod);BindInnerMatched(mod);BindPeelingFull(mod);}
