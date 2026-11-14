// общие объявления: обмен геометрией и настройками Surface Peel с Python
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "peeling.h"
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <stdexcept>

namespace pd_peeling_bind {
namespace nb=nanobind;
using D=nb::ndarray<const double,nb::c_contig,nb::device::cpu>;
using I=nb::ndarray<const int32_t,nb::c_contig,nb::device::cpu>;
template<typename T,size_t N,class A> std::vector<std::array<T,N>> matrix(A a){
 if(a.ndim()!=2||a.shape(1)!=N)throw std::invalid_argument("Peeling: incorrect matrix shape");
 std::vector<std::array<T,N>> out(a.shape(0));for(size_t i=0;i<out.size();++i)for(size_t j=0;j<N;++j)out[i][j]=a.data()[i*N+j];return out;
}
template<typename T,class A> std::vector<T> vector(A a){if(a.ndim()!=1)throw std::invalid_argument("Peeling: expected vector");return {a.data(),a.data()+a.shape(0)};}
template<typename T> nb::ndarray<nb::numpy,T> array(std::vector<T>&&v,size_t cols=0){
 auto*p=new std::vector<T>(std::move(v));if(p->empty())p->reserve(1);nb::capsule owner(p,[](void*x)noexcept{delete static_cast<std::vector<T>*>(x);});
 if(cols)return {p->data(),{p->size()/cols,cols},owner};return {p->data(),{p->size()},owner};
}
template<size_t N> std::vector<double> flatten(const std::vector<std::array<double,N>>&v){std::vector<double> out;out.reserve(v.size()*N);for(const auto&a:v)out.insert(out.end(),a.begin(),a.end());return out;}
}

inline void BindPeeling(nanobind::module_&mod){
 namespace nb=nanobind;using namespace pd_peeling_bind;
 mod.def("peeling_refine_edges",[](D p,D n,I e,I ok,D seeds,D patches,double step,double bend,double probe,bool adaptive,bool micro){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n),seedPoints=matrix<double,3>(seeds);auto edges=matrix<int32_t,2>(e);auto eligible=vector<int32_t>(ok);auto holes=matrix<double,7>(patches);
  std::vector<int32_t> result;{nb::gil_scoped_release release;result=pd_peeling::RefineEdges(points,normals,edges,eligible,seedPoints,holes,step,bend,probe,adaptive,micro);}
  return array(std::move(result));
 });
 mod.def("peeling_plan_attached",[](D p,I t,I h,D scales,double bend){
  auto points=matrix<double,3>(p);auto triangles=matrix<int32_t,3>(t);auto holes=vector<int32_t>(h);auto scale=vector<double>(scales);pd_peeling::Plan plan;
  {nb::gil_scoped_release release;plan=pd_peeling::PlanAttached(points,triangles,holes,scale,bend);}
  nb::dict out;out["hole"]=array(std::move(plan.hole));out["band"]=array(std::move(plan.band));out["border"]=array(std::move(plan.border));out["distance"]=array(std::move(plan.distance));out["rim_distance"]=array(std::move(plan.rimDistance));out["root_distance"]=array(std::move(plan.rootDistance));out["scale"]=array(std::move(plan.scale));return out;
 });
 mod.def("peeling_drawing_values",[](D p,D n,D f,D xy,I offsets,double size,double radius){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n);auto frames=matrix<double,14>(f);auto polygons=matrix<double,2>(xy);auto index=vector<int32_t>(offsets);std::vector<double> result;
  {nb::gil_scoped_release release;result=pd_peeling::DrawingValues(points,normals,frames,polygons,index,size,radius);}return array(std::move(result));
 });
 mod.def("peeling_deform",[](D p,D n,D dist,D rim,D root,D scale,D noise,D depth,double lift,double bend,double thickness,double variation){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n);pd_peeling::Plan plan;plan.distance=vector<double>(dist);plan.rimDistance=vector<double>(rim);plan.rootDistance=vector<double>(root);plan.scale=vector<double>(scale);
  auto noiseValues=vector<double>(noise),depths=vector<double>(depth);pd_peeling::Offsets out;
  {nb::gil_scoped_release release;out=pd_peeling::Deform(points,normals,plan,noiseValues,depths,lift,bend,thickness,variation);}
  nb::dict result;result["top"]=array(flatten(out.top),3);result["floor"]=array(flatten(out.floor),3);result["back"]=array(flatten(out.back),3);return result;
 });
 // ---- Surface Peel v2 (curl). Added functions; the v1 API above is unchanged.
 mod.def("peeling_plan_curl",[](D p,D n,I t,I h,I locked,D scales,D widths,D guard,double bend){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n);auto triangles=matrix<int32_t,3>(t);auto holes=vector<int32_t>(h),lock=vector<int32_t>(locked);
  auto scale=vector<double>(scales),width=vector<double>(widths),guards=vector<double>(guard);pd_peeling::CurlPlan plan;
  {nb::gil_scoped_release release;plan=pd_peeling::PlanCurl(points,normals,triangles,holes,lock,scale,width,guards,bend);}
  nb::dict out;out["hole"]=array(std::move(plan.hole));out["band"]=array(std::move(plan.band));out["border"]=array(std::move(plan.border));
  out["rim"]=array(std::move(plan.rim));out["distance"]=array(std::move(plan.distance));out["rim_distance"]=array(std::move(plan.rimDistance));
  out["root_distance"]=array(std::move(plan.rootDistance));out["scale"]=array(std::move(plan.scale));out["width"]=array(std::move(plan.width));
  out["direction"]=array(flatten(plan.direction),3);return out;
 });
 mod.def("peeling_deform_curl",[](D p,D n,D dist,D width,I rim,I border,D direction,D scale,D amp,D depth,
                                   double lift,double bend,double thickness,double curl,double droop,double rootBlend,D gravity){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n);pd_peeling::CurlPlan plan;
  plan.distance=vector<double>(dist);plan.width=vector<double>(width);plan.rim=vector<int32_t>(rim);plan.border=vector<int32_t>(border);
  plan.direction=matrix<double,3>(direction);plan.scale=vector<double>(scale);
  auto amps=vector<double>(amp),depths=vector<double>(depth);auto g=vector<double>(gravity);
  if(g.size()!=3)throw std::invalid_argument("Peeling: gravity must have 3 values");
  for(int32_t r:plan.rim)if(r>=int32_t(points.size()))throw std::invalid_argument("Peeling: invalid rim index");
  pd_peeling::CurlSettings s;s.lift=lift;s.bend=bend;s.thickness=thickness;s.curl=curl;s.droop=droop;s.rootBlend=rootBlend;s.gravity={g[0],g[1],g[2]};
  pd_peeling::Offsets out;
  {nb::gil_scoped_release release;out=pd_peeling::DeformCurl(points,normals,plan,amps,depths,s);}
  nb::dict result;result["top"]=array(flatten(out.top),3);result["floor"]=array(flatten(out.floor),3);result["back"]=array(flatten(out.back),3);return result;
 });
 mod.def("peeling_refine_adaptive",[](D p,D n,I e,I ok,D values,D seeds,double step,double bend,double reach,double probe,double size,double fine,bool adaptive){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n),seedPoints=matrix<double,3>(seeds);auto edges=matrix<int32_t,2>(e);auto eligible=vector<int32_t>(ok);auto field=vector<double>(values);
  std::vector<int32_t> result;{nb::gil_scoped_release release;result=pd_peeling::RefineAdaptive(points,normals,edges,eligible,field,seedPoints,step,bend,reach,probe,size,fine,adaptive);}
  return array(std::move(result));
 });
 mod.def("peeling_drawing_values_surface",[](D p,D n,D f,D xy,D line,I offsets,double size,double radius){
  auto points=matrix<double,3>(p),normals=matrix<double,3>(n),polyline=matrix<double,3>(line);auto frames=matrix<double,14>(f);auto polygons=matrix<double,2>(xy);auto index=vector<int32_t>(offsets);std::vector<double> result;
  {nb::gil_scoped_release release;result=pd_peeling::DrawingValuesSurface(points,normals,frames,polygons,polyline,index,size,radius);}return array(std::move(result));
 });
 mod.attr("connected_peeling_native")=true;
 mod.attr("peeling_curl_native")=2;
}
