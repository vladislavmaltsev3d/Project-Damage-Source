// строим связанные края покрытия и открывшийся внутренний слой
// SPDX-License-Identifier: GPL-3.0-or-later
// Attached peeling, complete native build: cut a scalar contour in the existing
// skin, bend its rim as a curled sheet and connect that rim through a thin
// underside to a recessed substrate. Optional nested Wallpaper layer.
//
// Coordinates and procedural inputs are float32, exactly like BMesh/mathutils,
// so the same seed yields the same layout as the former Python implementation.
// Single precision vector arithmetic below is deliberate.
#include "peeling_full.h"
#include "peeling.h"
#include "peeling_noise_tables.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>
#include <tbb/task_group.h>

namespace pd_peeling_full {
namespace {
constexpr int kStamp=33;
// Wrap Across Edges: an edge is only a barrier when it folds almost flat on itself,
// and damage may reach faces up to just past a right angle, never the far side of a wall.
constexpr double kWrapHardEdge=2.6,kWrapGate=-.1;
constexpr double kInf=std::numeric_limits<double>::infinity();
// Stage timings on stderr when PD_PEELING_PROFILE is set; diagnostic only.
struct Profile {
 const bool on=std::getenv("PD_PEELING_PROFILE")!=nullptr;std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
 void mark(const char*name){if(!on)return;const auto now=std::chrono::steady_clock::now();std::fprintf(stderr,"PEEL %-12s %8.1f ms\n",name,std::chrono::duration<double,std::milli>(now-last).count());last=now;}
};
template<class Fn> void pfor(size_t n,Fn&&fn,size_t grain=256){
 if(n<grain){for(size_t i=0;i<n;++i)fn(i);return;}
 tbb::parallel_for(tbb::blocked_range<size_t>(0,n,grain),[&](const tbb::blocked_range<size_t>&r){for(size_t i=r.begin();i!=r.end();++i)fn(i);});
}
inline F3 operator+(const F3&a,const F3&b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline F3 operator-(const F3&a,const F3&b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline F3 operator-(const F3&a){return {-a[0],-a[1],-a[2]};}
inline F3 operator*(const F3&a,float s){return {a[0]*s,a[1]*s,a[2]*s};}
inline F3 mulf(const F3&a,double s){return a*float(s);}
inline F3 divf(const F3&a,double s){return a*(1.0f/float(s));}
inline double dot(const F3&a,const F3&b){return double(a[0])*b[0]+double(a[1])*b[1]+double(a[2])*b[2];}
inline double length(const F3&a){return std::sqrt(dot(a,a));}
inline F3 cross(const F3&a,const F3&b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline F3 normalized(const F3&a){double l=length(a);if(l<=1e-35)return {0,0,0};float s=float(1./l);return a*s;}
inline F3 lerp(const F3&a,const F3&b,float t){float s=1.f-t;return {s*a[0]+t*b[0],s*a[1]+t*b[1],s*a[2]+t*b[2]};}
inline double dist(const F3&a,const F3&b){return length(a-b);}
inline double smooth01(double t){t=std::clamp(t,0.,1.);return t*t*(3.-2.*t);}
F3 orthogonal(const F3&v){
 const float x=std::fabs(v[0]),y=std::fabs(v[1]),z=std::fabs(v[2]);
 const int axis=x>y?(x>z?0:2):(y>z?1:2);
 if(axis==0)return {-v[1]-v[2],v[0],v[0]};
 if(axis==1)return {v[1],-v[0]-v[2],v[1]};
 return {v[2],v[2],-v[0]-v[1]};
}
inline F3 triNormal(const F3&a,const F3&b,const F3&c){return normalized(cross(b-a,c-a));}

// ---- Noise -----------------------------------------------------------------
float perlin(float x,float y,float z){
 using namespace pd_peeling_noise;
 auto setup=[](float val,int&b0,int&b1,float&r0,float&r1){float t=val+10000.0f;b0=int(t)&255;b1=(b0+1)&255;r0=t-std::floor(t);r1=r0-1.0f;};
 int bx0,bx1,by0,by1,bz0,bz1;float rx0,rx1,ry0,ry1,rz0,rz1;
 setup(x,bx0,bx1,rx0,rx1);setup(y,by0,by1,ry0,ry1);setup(z,bz0,bz1,rz0,rz1);
 const int i=kPerlinUb[bx0],j=kPerlinUb[bx1];
 const int b00=kPerlinUb[i+by0],b10=kPerlinUb[j+by0],b01=kPerlinUb[i+by1],b11=kPerlinUb[j+by1];
 auto at=[](int k,float rx,float ry,float rz){const float*q=kPerlinV3+3*k;return rx*q[0]+ry*q[1]+rz*q[2];};
 auto s=[](float t){return t*t*(3.0f-2.0f*t);};
 auto mix=[](float t,float a,float b){return a+t*(b-a);};
 const float sx=s(rx0),sy=s(ry0),sz=s(rz0);
 float u=at(b00+bz0,rx0,ry0,rz0),v=at(b10+bz0,rx1,ry0,rz0);float a=mix(sx,u,v);
 u=at(b01+bz0,rx0,ry1,rz0);v=at(b11+bz0,rx1,ry1,rz0);float b=mix(sx,u,v);
 const float c=mix(sy,a,b);
 u=at(b00+bz1,rx0,ry0,rz1);v=at(b10+bz1,rx1,ry0,rz1);a=mix(sx,u,v);
 u=at(b01+bz1,rx0,ry1,rz1);v=at(b11+bz1,rx1,ry1,rz1);b=mix(sx,u,v);
 const float d=mix(sy,a,b);
 return 1.5f*mix(sz,c,d);
}
inline double N(const F3&q){const float u=0.5f+0.5f*perlin(q[0],q[1],q[2]);return double(2.0f*u-1.0f);}
void voronoi(const F3&q,float da[4],float pa[12]){
 using namespace pd_peeling_noise;
 const int xi=int(std::floor(q[0])),yi=int(std::floor(q[1])),zi=int(std::floor(q[2]));
 da[0]=da[1]=da[2]=da[3]=1e10f;
 for(int xx=xi-1;xx<=xi+1;++xx)for(int yy=yi-1;yy<=yi+1;++yy)for(int zz=zi-1;zz<=zi+1;++zz){
  const float*p=kHashPnt+3*int(kHash[(kHash[(kHash[zz&255]+yy)&255]+xx)&255]);
  const float xd=q[0]-(p[0]+float(xx)),yd=q[1]-(p[1]+float(yy)),zd=q[2]-(p[2]+float(zz));
  const float d=std::sqrt(xd*xd+yd*yd+zd*zd);
  int slot=-1;for(int k=0;k<4;++k)if(d<da[k]){slot=k;break;}
  if(slot<0)continue;
  for(int k=3;k>slot;--k){da[k]=da[k-1];pa[3*k]=pa[3*k-3];pa[3*k+1]=pa[3*k-2];pa[3*k+2]=pa[3*k-1];}
  da[slot]=d;pa[3*slot]=p[0]+float(xx);pa[3*slot+1]=p[1]+float(yy);pa[3*slot+2]=p[2]+float(zz);
 }
}
// BLAKE2b with an 8 byte digest: the stable hash behind the secondary-damage layout.
uint64_t blake2b8(const uint8_t*data,size_t len){
 static const uint64_t iv[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
 static const uint8_t sigma[12][16]={{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},{11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},{7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},{9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},{2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},{12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},{13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},{6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},{10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3}};
 uint64_t h[8];std::memcpy(h,iv,sizeof h);h[0]^=0x01010000ULL^8ULL;
 uint8_t block[128]={0};if(len>128)throw std::logic_error("hash input too long");std::memcpy(block,data,len);
 uint64_t m[16];std::memcpy(m,block,128);uint64_t v[16];
 for(int i=0;i<8;++i){v[i]=h[i];v[i+8]=iv[i];}
 v[12]^=len;v[14]=~v[14];
 auto rot=[](uint64_t x,int n){return (x>>n)|(x<<(64-n));};
 auto G=[&](int a,int b,int c,int d,uint64_t x,uint64_t y){v[a]=v[a]+v[b]+x;v[d]=rot(v[d]^v[a],32);v[c]+=v[d];v[b]=rot(v[b]^v[c],24);v[a]=v[a]+v[b]+y;v[d]=rot(v[d]^v[a],16);v[c]+=v[d];v[b]=rot(v[b]^v[c],63);};
 for(int r=0;r<12;++r){const uint8_t*s=sigma[r];
  G(0,4,8,12,m[s[0]],m[s[1]]);G(1,5,9,13,m[s[2]],m[s[3]]);G(2,6,10,14,m[s[4]],m[s[5]]);G(3,7,11,15,m[s[6]],m[s[7]]);
  G(0,5,10,15,m[s[8]],m[s[9]]);G(1,6,11,12,m[s[10]],m[s[11]]);G(2,7,8,13,m[s[12]],m[s[13]]);G(3,4,9,14,m[s[14]],m[s[15]]);}
 return h[0]^v[0]^v[8];
}
double hashUnit(std::initializer_list<int64_t> values){
 int64_t buf[16];size_t n=0;for(int64_t x:values)buf[n++]=x;
 return double(blake2b8(reinterpret_cast<const uint8_t*>(buf),n*8))/18446744073709551616.0;
}

// ---- Spatial structures --------------------------------------------------------
struct KD {
 struct Node{int32_t point,left,right;int axis;};
 std::vector<F3> p;std::vector<Node> nodes;int root=-1;
 KD()=default;
 explicit KD(std::vector<F3> pts):p(std::move(pts)){
  std::vector<int32_t> order(p.size());std::iota(order.begin(),order.end(),0);nodes.resize(p.size());int next=0;root=build(order,0,order.size(),0,next);
 }
 int build(std::vector<int32_t>&o,size_t a,size_t b,int axis,int&next){
  if(a==b)return -1;size_t mid=a+(b-a)/2;
  std::nth_element(o.begin()+a,o.begin()+mid,o.begin()+b,[&](int i,int j){return p[i][axis]<p[j][axis]||(p[i][axis]==p[j][axis]&&i<j);});
  int id=next++;nodes[id]={o[mid],-1,-1,axis};
  int l=build(o,a,mid,(axis+1)%3,next),r=build(o,mid+1,b,(axis+1)%3,next);nodes[id].left=l;nodes[id].right=r;return id;
 }
 void nearestRec(int node,const F3&q,int&best,double&d2)const{
  if(node<0)return;const Node&n=nodes[node];const double dx=double(q[0])-p[n.point][0],dy=double(q[1])-p[n.point][1],dz=double(q[2])-p[n.point][2];
  const double c=dx*dx+dy*dy+dz*dz;if(c<d2||(c==d2&&(best<0||n.point<best))){d2=c;best=n.point;}
  const double ax=n.axis==0?dx:n.axis==1?dy:dz;int a=ax<0?n.left:n.right,b=ax<0?n.right:n.left;
  nearestRec(a,q,best,d2);if(ax*ax<=d2)nearestRec(b,q,best,d2);
 }
 std::pair<int,double> nearest(const F3&q)const{int b=-1;double d=kInf;nearestRec(root,q,b,d);return {b,std::sqrt(d)};}
 template<class Fn> void rangeRec(int node,const F3&q,double r2,Fn&fn)const{
  if(node<0)return;const Node&n=nodes[node];const double dx=double(q[0])-p[n.point][0],dy=double(q[1])-p[n.point][1],dz=double(q[2])-p[n.point][2];
  const double c=dx*dx+dy*dy+dz*dz;if(c<=r2)fn(n.point,std::sqrt(c));
  const double ax=n.axis==0?dx:n.axis==1?dy:dz;int a=ax<0?n.left:n.right,b=ax<0?n.right:n.left;
  rangeRec(a,q,r2,fn);if(ax*ax<=r2)rangeRec(b,q,r2,fn);
 }
 template<class Fn> void range(const F3&q,double r,Fn&&fn)const{rangeRec(root,q,r*r,fn);}
};

struct Box{float lo[3]={FLT_MAX,FLT_MAX,FLT_MAX},hi[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
 void add(const F3&p){for(int i=0;i<3;++i){lo[i]=std::min(lo[i],p[i]);hi[i]=std::max(hi[i],p[i]);}}
 void add(const Box&b){for(int i=0;i<3;++i){lo[i]=std::min(lo[i],b.lo[i]);hi[i]=std::max(hi[i],b.hi[i]);}}
 bool overlaps(const Box&b)const{for(int i=0;i<3;++i)if(lo[i]>b.hi[i]||b.lo[i]>hi[i])return false;return true;}
 double dist2(const F3&q)const{double s=0;for(int i=0;i<3;++i){double d=std::max({double(lo[i])-q[i],0.,double(q[i])-hi[i]});s+=d*d;}return s;}
};
// Median-split triangle BVH. Node layout is implicit, so subtrees build in parallel.
struct TriBVH {
 struct Node{Box box;int32_t right=-1,start=0,count=0;};
 std::vector<std::array<F3,3>> tris;std::vector<int32_t> order;std::vector<Node> nodes;
 static size_t countNodes(size_t n){return n<=4?1:1+countNodes(n/2)+countNodes(n-n/2);}
 explicit TriBVH(std::vector<std::array<F3,3>> t):tris(std::move(t)),order(tris.size()){
  std::iota(order.begin(),order.end(),0);if(tris.empty())return;
  std::vector<F3> c(tris.size());pfor(tris.size(),[&](size_t i){const auto&q=tris[i];c[i]={(q[0][0]+q[1][0]+q[2][0])/3.f,(q[0][1]+q[1][1]+q[2][1])/3.f,(q[0][2]+q[1][2]+q[2][2])/3.f};});
  nodes.resize(countNodes(tris.size()));build(0,0,tris.size(),c);
 }
 void build(int id,size_t a,size_t b,const std::vector<F3>&c){
  Node&n=nodes[id];
  if(b-a<=4){n.start=int32_t(a);n.count=int32_t(b-a);for(size_t i=a;i<b;++i)for(const F3&p:tris[order[i]])n.box.add(p);return;}
  Box cb;for(size_t i=a;i<b;++i)cb.add(c[order[i]]);
  int axis=0;for(int i=1;i<3;++i)if(cb.hi[i]-cb.lo[i]>cb.hi[axis]-cb.lo[axis])axis=i;
  size_t mid=a+(b-a)/2;
  std::nth_element(order.begin()+a,order.begin()+mid,order.begin()+b,[&](int x,int y){return c[x][axis]<c[y][axis]||(c[x][axis]==c[y][axis]&&x<y);});
  const int left=id+1;const int right=left+int(countNodes(mid-a));n.right=right;
  if(b-a>16384)tbb::parallel_invoke([&]{build(left,a,mid,c);},[&]{build(right,mid,b,c);});
  else{build(left,a,mid,c);build(right,mid,b,c);}
  n.box=nodes[left].box;n.box.add(nodes[right].box);
 }
 static F3 closest(const F3&p,const std::array<F3,3>&t){
  // Ericson, in double precision.
  using D=std::array<double,3>;auto toD=[](const F3&v){return D{v[0],v[1],v[2]};};
  auto sub=[](const D&x,const D&y){return D{x[0]-y[0],x[1]-y[1],x[2]-y[2]};};auto dt=[](const D&x,const D&y){return x[0]*y[0]+x[1]*y[1]+x[2]*y[2];};
  auto at=[](const D&x,const D&e,double s){return F3{float(x[0]+e[0]*s),float(x[1]+e[1]*s),float(x[2]+e[2]*s)};};
  const D a=toD(t[0]),b=toD(t[1]),c=toD(t[2]),P=toD(p),ab=sub(b,a),ac=sub(c,a),ap=sub(P,a);
  const double d1=dt(ab,ap),d2=dt(ac,ap);if(d1<=0&&d2<=0)return t[0];
  const D bp=sub(P,b);const double d3=dt(ab,bp),d4=dt(ac,bp);if(d3>=0&&d4<=d3)return t[1];
  const double vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0)return at(a,ab,d1/(d1-d3));
  const D cp=sub(P,c);const double d5=dt(ab,cp),d6=dt(ac,cp);if(d6>=0&&d5<=d6)return t[2];
  const double vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0)return at(a,ac,d2/(d2-d6));
  const double va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0)return at(b,sub(c,b),(d4-d3)/((d4-d3)+(d5-d6)));
  const double den=1./(va+vb+vc),v=vb*den,w=vc*den;
  return {float(a[0]+ab[0]*v+ac[0]*w),float(a[1]+ab[1]*v+ac[1]*w),float(a[2]+ab[2]*v+ac[2]*w)};
 }
 struct Hit{F3 co{0,0,0},no{0,0,0};int tri=-1;double distance=kInf;};
 Hit nearest(const F3&q)const{
  Hit h;if(nodes.empty())return h;double best=kInf;
  int stack[128];int sp=0;stack[sp++]=0;
  while(sp){
   const int id=stack[--sp];const Node&n=nodes[id];if(n.box.dist2(q)>=best)continue;
   if(n.count){for(int i=n.start;i<n.start+n.count;++i){const int ti=order[i];const F3 c=closest(q,tris[ti]);const double dx=double(c[0])-q[0],dy=double(c[1])-q[1],dz=double(c[2])-q[2],d=dx*dx+dy*dy+dz*dz;
     if(d<best||(d==best&&ti<h.tri)){best=d;h.co=c;h.tri=ti;}}continue;}
   const int l=id+1,r=n.right;const double dl=nodes[l].box.dist2(q),dr=nodes[r].box.dist2(q);
   if(dl<dr){stack[sp++]=r;stack[sp++]=l;}else{stack[sp++]=l;stack[sp++]=r;}
  }
  if(h.tri>=0){h.distance=std::sqrt(best);const auto&t=tris[h.tri];h.no=triNormal(t[0],t[1],t[2]);}
  return h;
 }
 // Two sided first hit along a unit direction.
 double ray(const F3&o,const F3&dIn,double maxDist)const{
  if(nodes.empty())return kInf;const F3 d=normalized(dIn);double best=maxDist;bool found=false;
  double inv[3];for(int i=0;i<3;++i)inv[i]=d[i]!=0?1./d[i]:(d[i]>=0?1e30:-1e30);
  auto boxHit=[&](const Box&b){double t0=0,t1=best;for(int i=0;i<3;++i){double a=(b.lo[i]-double(o[i]))*inv[i],c=(b.hi[i]-double(o[i]))*inv[i];if(a>c)std::swap(a,c);t0=std::max(t0,a);t1=std::min(t1,c);if(t0>t1)return false;}return true;};
  int stack[128];int sp=0;stack[sp++]=0;
  while(sp){
   const int id=stack[--sp];const Node&n=nodes[id];if(!boxHit(n.box))continue;
   if(n.count){for(int i=n.start;i<n.start+n.count;++i){const auto&t=tris[order[i]];
     const double e1[3]={double(t[1][0])-t[0][0],double(t[1][1])-t[0][1],double(t[1][2])-t[0][2]},e2[3]={double(t[2][0])-t[0][0],double(t[2][1])-t[0][1],double(t[2][2])-t[0][2]};
     const double px=d[1]*e2[2]-d[2]*e2[1],py=d[2]*e2[0]-d[0]*e2[2],pz=d[0]*e2[1]-d[1]*e2[0];
     const double det=e1[0]*px+e1[1]*py+e1[2]*pz;if(std::fabs(det)<1e-30)continue;const double idet=1./det;
     const double tv[3]={double(o[0])-t[0][0],double(o[1])-t[0][1],double(o[2])-t[0][2]};
     const double u=(tv[0]*px+tv[1]*py+tv[2]*pz)*idet;if(u<0||u>1)continue;
     const double qx=tv[1]*e1[2]-tv[2]*e1[1],qy=tv[2]*e1[0]-tv[0]*e1[2],qz=tv[0]*e1[1]-tv[1]*e1[0];
     const double v=(d[0]*qx+d[1]*qy+d[2]*qz)*idet;if(v<0||u+v>1)continue;
     const double s=(e2[0]*qx+e2[1]*qy+e2[2]*qz)*idet;if(s>=0&&s<best){best=s;found=true;}}continue;}
   stack[sp++]=n.right;stack[sp++]=id+1;
  }
  return found?best:kInf;
 }
};

// Non-coplanar triangle intersection (coplanar pairs are not reported, like Blender's overlap).
bool triTri(const std::array<F3,3>&A,const std::array<F3,3>&B){
 using D=std::array<double,3>;auto toD=[](const F3&v){return D{v[0],v[1],v[2]};};
 auto sub=[](const D&a,const D&b){return D{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};auto cr=[](const D&a,const D&b){return D{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};auto dt=[](const D&a,const D&b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
 const D a0=toD(A[0]),a1=toD(A[1]),a2=toD(A[2]),b0=toD(B[0]),b1=toD(B[1]),b2=toD(B[2]);
 const D na=cr(sub(a1,a0),sub(a2,a0)),nb=cr(sub(b1,b0),sub(b2,b0));
 double da[3]={dt(nb,sub(a0,b0)),dt(nb,sub(a1,b0)),dt(nb,sub(a2,b0))};
 if((da[0]>0&&da[1]>0&&da[2]>0)||(da[0]<0&&da[1]<0&&da[2]<0))return false;
 double db[3]={dt(na,sub(b0,a0)),dt(na,sub(b1,a0)),dt(na,sub(b2,a0))};
 if((db[0]>0&&db[1]>0&&db[2]>0)||(db[0]<0&&db[1]<0&&db[2]<0))return false;
 const D dir=cr(na,nb);if(dt(dir,dir)<=0)return false;
 auto interval=[&](const D v[3],const double d[3],double r[2]){
  double lo=kInf,hi=-kInf;
  for(int i=0;i<3;++i){const int j=(i+1)%3;const double pi=dt(dir,v[i]),pj=dt(dir,v[j]);
   if(d[i]==0){lo=std::min(lo,pi);hi=std::max(hi,pi);}
   if((d[i]<0&&d[j]>0)||(d[i]>0&&d[j]<0)){const double t=d[i]/(d[i]-d[j]);const double x=pi+(pj-pi)*t;lo=std::min(lo,x);hi=std::max(hi,x);}}
  r[0]=lo;r[1]=hi;return lo<=hi;};
 const D va[3]={a0,a1,a2},vb[3]={b0,b1,b2};double ra[2],rb[2];
 if(!interval(va,da,ra)||!interval(vb,db,rb))return false;
 return ra[0]<=rb[1]&&rb[0]<=ra[1];
}

// ---- Procedural field ------------------------------------------------------------
int64_t pyRound(double x){return int64_t(std::nearbyint(x));}
double cellHash(const F3&p){
 const int64_t k0=pyRound(double(p[0])*4096.),k1=pyRound(double(p[1])*4096.),k2=pyRound(double(p[2])*4096.);
 int64_t h=(k0*73856093)^(k1*19349663)^(k2*83492791);
 const uint64_t m=uint64_t(h^(h>>13))*0x5bd1e995ULL&0xffffffffULL;
 const uint64_t r=(m^(m>>15))&0xffffffULL;
 return double(r)/double(0x800000)-1.;
}
inline double octaveFade(double freq,double detail){return std::max(0.,std::min(1.,detail/(3.2*freq)-.35));}

struct Region{F3 center,u,v,n;std::vector<std::array<double,2>> poly;double slab;bool area;std::vector<F3> line;};
struct PlateKey{int64_t a,b,c;bool operator==(const PlateKey&o)const{return a==o.a&&b==o.b&&c==o.c;}};
struct PlateHash{size_t operator()(const PlateKey&k)const{return size_t(k.a*73856093)^size_t(k.b*19349663)^size_t(k.c*83492791);}};

struct Drawing;
struct Field {
 const PassParams&c;const Drawing*drawing;
 struct Fade{double amp,freq;F3 o;double fade;};std::vector<Fade> fades;
 static constexpr double kCell=.2;
 mutable tbb::enumerable_thread_specific<std::unordered_map<PlateKey,double,PlateHash>> plates;
 Field(const PassParams&cfg,const Drawing*d):c(cfg),drawing(d){
  const double table[3][2]={{.30,4.3},{.16,8.9},{.085,17.6}};
  for(int i=0;i<3;++i){double f=octaveFade(table[i][1],c.detail);if(f>0)fades.push_back({table[i][0],table[i][1],c.O[4+i],f});}
 }
 F3 warped(const F3&q)const{
  const F3 s=mulf(q,.55);const F3 w{float(N(s+c.O[0])),float(N(s+c.O[1])),float(N(s+c.O[2]))};
  return q+mulf(w,.45);
 }
 double torn(const F3&qw,double scale)const{
  double r=0;for(const auto&f:fades)r+=f.amp*f.fade*N(mulf(qw,f.freq)+f.o);
  const double ridge=std::pow(std::max(0.,1.-std::fabs(N(mulf(qw,3.1)+c.O[7]))),7);
  const double side=std::max(-1.,std::min(1.,3.*N(mulf(qw,.8)+c.O[8])));
  return scale*(r+.24*ridge*side);
 }
 void cracks(const F3&q,F3&a,F3&b,double&w)const{
  F3 qc=divf(q,kCell);const F3 s=mulf(qc,.37);
  const F3 wv{float(N(s+c.O[9])),float(N(s+c.O[10])),float(N(s+c.O[11]))};
  qc=qc+mulf(wv,.22);float d[4],p[12];voronoi(qc,d,p);
  const double t=std::max(0.,std::min(1.,(double(d[1])-double(d[0]))/.16));
  a={p[0],p[1],p[2]};b={p[3],p[4],p[5]};w=t*t*(3.-2.*t);
 }
 double smooth(const F3&q)const{
  const F3 qw=warped(q);
  const double base=N(qw)+.36*N(mulf(qw,2.07)+c.O[3]);
  return (base+c.ragged*torn(qw,1.))/1.06-c.threshold;
 }
 double plateValue(const F3&p)const{
  const PlateKey key{pyRound(double(p[0])*4096.),pyRound(double(p[1])*4096.),pyRound(double(p[2])*4096.)};
  auto&m=plates.local();auto it=m.find(key);if(it!=m.end())return it->second;
  const double v=smooth(mulf(p,kCell));m.emplace(key,v);return v;
 }
 // Value given the drawing distance (when this pass is drawn).
 double fromDrawing(const F3&co,double distance)const{
  const F3 q=divf(co,c.size)+c.offset;
  double value=distance+c.ragged*torn(warped(q),.17);
  const double pl=std::max(0.,std::min(1.,c.plates));
  if(pl>0){F3 a,b;double w;cracks(q,a,b,w);const double ra=cellHash(a),rb=cellHash(b);value+=pl*.045*((ra+rb)*.5*(1.-w)+ra*w);}
  return value;
 }
 double procedural(const F3&co)const{
  const F3 q=divf(co,c.size)+c.offset;const double pl=std::max(0.,std::min(1.,c.plates));
  if(pl<=0)return smooth(q);
  F3 a,b;double w;cracks(q,a,b,w);
  const double va=plateValue(a),vb=plateValue(b),plate=(va+vb)*.5*(1.-w)+va*w;
  return smooth(q)*(1.-pl)+plate*pl;
 }
 double at(const F3&co)const;
 // ---- Damage Factor: the crack network that replaces peeling at low values.
 struct CrazeInfo{bool crack=false,line=false;double hash=0;};
 static constexpr double kCrazeCell=.24; // ~8 cm plates on a 35 cm Patch Size
 F3 crazeSpace(const F3&q)const{
  // Warped and squeezed along one direction: irregular, somewhat elongated plates.
  F3 qc=divf(q,kCrazeCell);const F3 sw=mulf(qc,.37);
  qc=qc+mulf(F3{float(N(sw+c.O[9])),float(N(sw+c.O[10])),float(N(sw+c.O[11]))},.3);
  const F3 axis=normalized(c.O[5]);return qc+mulf(axis,-.4*dot(qc,axis));
 }
 // Distance to the wall between the two nearest cells. (d1-d0)/2 would be far too
 // small everywhere around two close feature points and open a blob there.
 static double wallDistance(const float d[4],const float p[12]){
  const double sx=double(p[3])-p[0],sy=double(p[4])-p[1],sz=double(p[5])-p[2];
  const double sep=std::max(1e-9,std::sqrt(sx*sx+sy*sy+sz*sz));
  return (double(d[1])*d[1]-double(d[0])*d[0])/(2.*sep);
 }
 double crackPatch(double F)const{return smooth01((-F+.3)/.3);}
 double pitPatch(double F)const{return smooth01((-F+.75)/.5);}
 // Distance to the nearest active pit centre, and its radius (0 when none).
 double pit(const F3&co,double F,double&r)const{
  r=0;const double f=c.damage,fade=1.-smooth01((f-.7)/.28),m=pitPatch(F)*fade;if(m<=0)return kInf;
  const F3 q=divf(co,c.size)+c.offset;const double cellW=kCrazeCell*c.size/2.4;
  float d[4],p[12];voronoi(divf(q,kCrazeCell/2.4)+c.O[4],d,p);
  const double h=.5*(cellHash(F3{p[0],p[1],p[2]})+1.);
  if(h>=.16*m)return kInf;
  r=c.crack_width*(.8+6.*h);return double(d[0])*cellW;
 }
 // Plates cup up towards their borders; both sides meet in a soft ridge.
 double relief(const F3&co,double F)const{
  const double f=c.damage,fade=1.-smooth01((f-.7)/.28),m=crackPatch(F)*fade;if(m<=0)return 0.;
  const F3 q=divf(co,c.size)+c.offset;const F3 qc=crazeSpace(q);const double cellW=kCrazeCell*c.size;
  auto level=[&](const F3&space,double scale,double reach){
   float d[4],p[12];voronoi(space,d,p);
   const double b=wallDistance(d,p)*cellW/scale;const double ha=.5*(cellHash(F3{p[0],p[1],p[2]})+1.),hb=.5*(cellHash(F3{p[3],p[4],p[5]})+1.);
   const double aa=.25+.75*ha,ab=.25+.75*hb;
   const double t=smooth01(1.-b/(reach*cellW/scale));
   const double shared=smooth01(b/(.12*cellW/scale));
   return t*((aa+ab)*.5*(1.-shared)+aa*shared);
  };
  const double big=level(qc,1.,.42);
  const double fine=.35*level(mulf(qc,1.9)+c.O[8],1.9,.4)*smooth01((N(mulf(q,1.9)+c.O[3])+.05)/.3);
  return m*c.lift*.3*(big+fine);
 }
 double craze(const F3&co,double F,CrazeInfo*info=nullptr)const{
  const double f=c.damage,sizeW=c.size,toField=1.2/sizeW;
  const double s=smooth01((f-.45)/.55),fade=1.-smooth01((f-.7)/.28);
  const double shifted=F+(1.-s)*1.3;
  const double patch=smooth01((-F+.3)/.3);
  if(patch<=0||fade<=0)return shifted;
  const F3 q=divf(co,sizeW)+c.offset;
  const double cellW=kCrazeCell*sizeW,w=c.crack_width;
  const F3 qc=crazeSpace(q);float d[4],p[12];voronoi(qc,d,p);
  const double border=wallDistance(d,p)*cellW;const F3 plate{p[0],p[1],p[2]};
  const double hash=.5*(cellHash(plate)+1.);
  const double freq=std::min(70.,sizeW/std::max(1e-9,4.*c.contour_step));
  const double jitter=w*.5*N(mulf(q,freq)+c.O[6]);
  const double margin=.05*sizeW;
  // Most plate borders stay closed and only show as relief. Short stretches open
  // into thin tapered cracks, more of them as the damage grows.
  const double thr=.14-.5*smooth01(f/.5);
  const double gate1=smooth01((N(mulf(q,2.7)+c.O[7])-thr)/.22),w1=w*gate1*patch*fade;
  // Well away from a crack that has closed, the field must not sit near zero.
  auto closed=[&](double x){return x<.15?(.15-x)/.15*margin:0.;};
  double value=(border+jitter-w1+closed(w1/w))*toField;
  // Finer branches over part of the patch; they end where their gate closes.
  const double gate2=smooth01((N(mulf(q,1.9)+c.O[3])-(.3-.25*smooth01(f/.5)))/.22);
  if(gate2>0){
   float d2[4],p2[12];voronoi(mulf(qc,1.9)+c.O[8],d2,p2);
   const double w2=w*.7*gate2*patch*fade;
   const double border2=wallDistance(d2,p2)*cellW/1.9;
   value=std::min(value,(border2+jitter-w2+closed(w2/(w*.7)))*toField);
  }
  // Small pits, also a little beyond the cracks.
  {double r;const double dp=pit(co,F,r);if(r>0)value=std::min(value,(dp-r)*toField);}
  // Mid values: some whole plates flake off.
  const double frac=.4*smooth01((f-.3)/.6)*fade*patch;
  const double lines=value;
  if(hash<frac)value=std::min(value,-border*toField-1e-4);
  else if(frac>0){const F3 next{p[3],p[4],p[5]};if(.5*(cellHash(next)+1.)<frac)value=std::min(value,border*toField+1e-4);} // meets the flaked neighbour's outline
  if(info){info->crack=value<shifted;info->hash=hash;info->line=lines<=value+1e-12&&lines<shifted;}
  return std::min(shifted,value);
 }
};

struct Drawing {
 std::unique_ptr<TriBVH> surface;std::vector<float> guided;std::vector<int32_t> triPoly;
 std::vector<Region> regions;double radius=0,snap=0,size=1,gate=.35;
 double valueAt(const F3&co,const F3&normal)const{
  double best=1e6;
  for(const Region&r:regions){
   const F3 delta=co-r.center;if(std::fabs(dot(delta,r.n))>r.slab||dot(normal,r.n)<gate)continue;
   const size_t m=r.line.size();const size_t segs=r.area?m:(m?m-1:0);double d2=kInf;
   for(size_t j=0;j<segs;++j){
    const F3&A=r.line[j];const F3&B=r.line[(j+1)%m];
    const double ab[3]={double(B[0])-A[0],double(B[1])-A[1],double(B[2])-A[2]},aq[3]={double(co[0])-A[0],double(co[1])-A[1],double(co[2])-A[2]};
    const double t=std::clamp((aq[0]*ab[0]+aq[1]*ab[1]+aq[2]*ab[2])/std::max(1e-20,ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2]),0.,1.);
    const double x=aq[0]-ab[0]*t,y=aq[1]-ab[1]*t,z=aq[2]-ab[2]*t;d2=std::min(d2,x*x+y*y+z*z);
   }
   double distance=std::sqrt(d2);
   if(r.area){
    const double x=dot(delta,r.u),y=dot(delta,r.v);bool inside=false;const size_t k=r.poly.size();
    for(size_t j=0;j<k;++j){const auto&a=r.poly[j];const auto&b=r.poly[(j+1)%k];
     if((a[1]>y)!=(b[1]>y)){const double c=a[0]+(y-a[1])*(b[0]-a[0])/(b[1]-a[1]);if(x<c)inside=!inside;}}
    if(inside)distance=-distance;
   }else distance-=radius;
   best=std::min(best,distance/size);
  }
  return best;
 }
 TriBVH::Hit nearest(const F3&co)const{
  TriBVH::Hit h=surface->nearest(co);
  if(h.tri>=0&&!guided.empty()){const int p=triPoly[h.tri];const F3 g{guided[3*p],guided[3*p+1],guided[3*p+2]};if(dot(g,h.no)>.25)h.no=g;}
  return h;
 }
 double value(const F3&co)const{const auto h=nearest(co);return h.tri>=0?valueAt(co,h.no):1e6;}
};
double Field::at(const F3&co)const{
 const double F=c.use_drawing&&drawing?fromDrawing(co,drawing->value(co)):procedural(co);
 return c.damage<.999?craze(co,F):F;
}

template<class Fn> void triangleSamples(F3 a,F3 b,F3 c,double spacing,Fn&&fn){
 // The longest edge becomes the base; the first of equally long edges wins.
 const double ab=dist(a,b),bc=dist(b,c),ca=dist(c,a);
 if(bc>ab&&bc>=ca){F3 t=a;a=b;b=c;c=t;}
 else if(ca>ab&&ca>bc){F3 t=a;a=c;c=b;b=t;}
 const double len=dist(b,a);const double height=length(cross(c-a,b-a))/std::max(len,1e-12);
 const int rows=std::max(1,int(std::ceil(height/spacing)));
 for(int i=0;i<=rows;++i){const float t=float(double(i)/rows);const F3 L=lerp(a,c,t),R=lerp(b,c,t);
  const int cols=std::max(1,int(std::ceil(dist(R,L)/spacing)));
  for(int j=0;j<=cols;++j)fn(lerp(L,R,float(double(j)/cols)));}
}

// Ear clipping of a simple polygon in its local plane; only used to sample drawn areas.
std::vector<std::array<int,3>> earClip(const std::vector<std::array<double,2>>&p){
 std::vector<int> idx(p.size());std::iota(idx.begin(),idx.end(),0);std::vector<std::array<int,3>> out;
 double area=0;for(size_t i=0;i<p.size();++i){const auto&a=p[i];const auto&b=p[(i+1)%p.size()];area+=a[0]*b[1]-b[0]*a[1];}
 const double sign=area>=0?1:-1;
 auto cr=[&](int a,int b,int c){return sign*((p[b][0]-p[a][0])*(p[c][1]-p[a][1])-(p[b][1]-p[a][1])*(p[c][0]-p[a][0]));};
 size_t guard=idx.size()*idx.size()+8;size_t i=0;
 while(idx.size()>3&&guard--){
  const size_t n=idx.size();const int a=idx[(i+n-1)%n],b=idx[i%n],c=idx[(i+1)%n];bool ear=cr(a,b,c)>0;
  if(ear)for(int q:idx){if(q==a||q==b||q==c)continue;if(cr(a,b,q)>=0&&cr(b,c,q)>=0&&cr(c,a,q)>=0){ear=false;break;}}
  if(ear){out.push_back({a,b,c});idx.erase(idx.begin()+long(i%n));}else ++i;
 }
 if(idx.size()==3)out.push_back({idx[0],idx[1],idx[2]});
 return out;
}

// ---- Mesh -------------------------------------------------------------------------
struct Face {
 int8_t n=0;uint8_t alive=1,smooth=0,paper=0;
 int32_t v[4]{-1,-1,-1,-1},e[4]{-1,-1,-1,-1};
 float w[4][3]{};           // corner weights over the base triangle's corners
 int32_t base=-1,poly=-1,mat=0,stamp=0;F3 no{0,0,1};
};
struct EdgeRec {
 int32_t a,b,orig=-1;int32_t f[2]{-1,-1};int32_t nf=0;bool alive=true,sharp=false;std::vector<int32_t> extra;
 void addFace(int32_t x){if(nf<2)f[nf]=x;else extra.push_back(x);++nf;}
 void removeFace(int32_t x){
  if(nf>2){auto it=std::find(extra.begin(),extra.end(),x);if(it!=extra.end()){extra.erase(it);--nf;return;}
   if(f[0]==x){f[0]=extra.back();extra.pop_back();--nf;return;}
   if(f[1]==x){f[1]=extra.back();extra.pop_back();--nf;return;}
   return;}
  if(nf==2&&f[0]==x){f[0]=f[1];f[1]=-1;--nf;}else if(nf==2&&f[1]==x){f[1]=-1;--nf;}else if(nf==1&&f[0]==x){f[0]=-1;--nf;}
 }
};
struct EdgeTable { // open addressing, keys are (min,max) vertex pairs
 std::vector<uint64_t> keys;std::vector<int32_t> vals;size_t used=0;
 EdgeTable(){keys.assign(1<<16,~0ULL);vals.resize(1<<16);}
 static uint64_t mix(uint64_t x){x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;return x;}
 size_t slot(uint64_t k)const{size_t m=keys.size()-1,i=size_t(mix(k))&m;while(keys[i]!=~0ULL&&keys[i]!=k)i=(i+1)&m;return i;}
 int32_t get(uint64_t k)const{size_t i=slot(k);return keys[i]==k?vals[i]:-1;}
 void set(uint64_t k,int32_t v){
  if((used+1)*10>keys.size()*7){std::vector<uint64_t> ok;std::vector<int32_t> ov;ok.swap(keys);ov.swap(vals);keys.assign(ok.size()*2,~0ULL);vals.resize(ok.size()*2);used=0;
   for(size_t i=0;i<ok.size();++i)if(ok[i]!=~0ULL){size_t s=slot(ok[i]);keys[s]=ok[i];vals[s]=ov[i];++used;}}
  size_t i=slot(k);if(keys[i]!=k){keys[i]=k;++used;}vals[i]=v;
 }
};
struct Prov{int32_t s[3]{-1,-1,-1};float w[3]{0,0,0};};
Prov mergeProv(const Prov&a,const Prov&b,float t){
 int32_t ids[6];float ws[6];int n=0;
 auto add=[&](int32_t id,float wt){if(id<0)return;for(int i=0;i<n;++i)if(ids[i]==id){ws[i]+=wt;return;}ids[n]=id;ws[n]=wt;++n;};
 const float s=1.f-t;for(int i=0;i<3;++i)add(a.s[i],a.w[i]*s);for(int i=0;i<3;++i)add(b.s[i],b.w[i]*t);
 if(n>3){for(int i=0;i<n;++i)for(int j=i+1;j<n;++j)if(ws[j]>ws[i]){std::swap(ws[i],ws[j]);std::swap(ids[i],ids[j]);}float sum=ws[0]+ws[1]+ws[2];if(sum>0)for(int i=0;i<3;++i)ws[i]/=sum;n=3;}
 Prov out;for(int i=0;i<n;++i){out.s[i]=ids[i];out.w[i]=ws[i];}return out;
}

// Quad diagonal by Blender's beauty rule: negative if (1-3) improves on (2-4).
double beautyCost(const F3&v1,const F3&v2,const F3&v3,const F3&v4){
 F3 no=cross(v1-v3,v2-v4);
 F3 n=normalized(no);if(length(n)==0)n={0,0,1};
 const F3 t=normalized(orthogonal(n));const F3 bt=cross(n,t);
 auto p2=[&](const F3&v){return std::array<double,2>{dot(v,t),dot(v,bt)};};
 const auto a=p2(v1),b=p2(v2),c=p2(v3),d=p2(v4);
 auto cr=[](const std::array<double,2>&x,const std::array<double,2>&y,const std::array<double,2>&z){return (x[0]-y[0])*(y[1]-z[1])+(y[1]-x[1])*(y[0]-z[0]);};
 auto ln=[](const std::array<double,2>&x,const std::array<double,2>&y){return std::hypot(x[0]-y[0],x[1]-y[1]);};
 const double eps=1e-12;const double a234=cr(b,c,d),a241=cr(b,d,a),a123=cr(a,b,c),a134=cr(a,c,d);
 if((a123>=0)!=(a134>=0))return FLT_MAX;
 if(std::fabs(a123)<=eps||std::fabs(a134)<=eps)return FLT_MAX;
 if((a234>=0)!=(a241>=0))return -FLT_MAX;
 if(std::fabs(a234)<=eps||std::fabs(a241)<=eps)return -FLT_MAX;
 const double l12=ln(a,b),l23=ln(b,c),l34=ln(c,d),l41=ln(d,a),l13=ln(a,c),l24=ln(b,d);
 const double f24=std::fabs(a234)/(l23+l34+l24)+std::fabs(a241)/(l41+l12+l24);
 const double f13=std::fabs(a123)/(l12+l23+l13)+std::fabs(a134)/(l34+l41+l13);
 return f24-f13;
}

// Mean value weights of a 2D point over a triangle, as BMesh loop interpolation does.
void meanValue(const std::array<double,2> v[3],const std::array<double,2>&q,double out[3]){
 const double eps=1e-5;double d[3][2],len[3];
 for(int i=0;i<3;++i){d[i][0]=v[i][0]-q[0];d[i][1]=v[i][1]-q[1];len[i]=std::hypot(d[i][0],d[i][1]);if(len[i]<eps){out[0]=out[1]=out[2]=0;out[i]=1;return;}}
 double ht[3];
 for(int i=0;i<3;++i){const int j=(i+1)%3;const double area=d[i][0]*d[j][1]-d[i][1]*d[j][0];const double dt=d[i][0]*d[j][0]+d[i][1]*d[j][1];
  if(std::fabs(area)<eps*std::max(len[i],len[j])&&dt<0){const double t=len[i]/(len[i]+len[j]);out[0]=out[1]=out[2]=0;out[i]=1-t;out[j]=t;return;}
  const double den=len[i]*len[j]+dt;ht[i]=std::fabs(den)>1e-30?area/den:0;}
 double total=0;for(int i=0;i<3;++i){const int p=(i+2)%3;out[i]=(ht[p]+ht[i])/len[i];total+=out[i];}
 if(std::fabs(total)<1e-30){out[0]=out[1]=out[2]=1./3.;return;}
 for(int i=0;i<3;++i)out[i]/=total;
}

// ---- Secondary damage --------------------------------------------------------------
struct RimPoint{F3 co,n;};
struct PatchRec{F3 co,n;float r;};
struct SmallDamage {
 double size=1,maxRadius=0,gate=.65,heightLimit=.65;std::vector<PatchRec> patches;std::vector<std::pair<F3,double>> shapes;KD tree;bool hasTree=false;
 double value(const F3&co,const F3&normal)const{
  if(!hasTree)return kInf;double result=kInf;
  tree.range(co,maxRadius*1.6,[&](int index,double){
   const PatchRec&p=patches[index];const double radius=p.r;
   if(dot(normal,p.n)<gate||std::fabs(dot(co-p.co,p.n))>radius*heightLimit)return;
   const F3&tangent=shapes[index].first;const double aspect=shapes[index].second;const F3 delta=co-p.co;
   double along=dot(delta,tangent);const double across=dot(delta,cross(p.n,tangent))/aspect;
   along*=1.+.25*(along>0?1:-1)*.5;
   const double dn=dot(delta,p.n);const double radial=std::sqrt(along*along+across*across+dn*dn);
   const F3 dr=divf(delta,radius);
   const double irregular=1+.34*N(mulf(dr,2.1))+.12*N(mulf(dr,5.3));
   result=std::min(result,(radial-radius*irregular)/size);
  });
  return result;
 }
 double scale(const F3&co)const{
  if(!hasTree)return 1.;const auto hit=tree.nearest(co);const double radius=patches[hit.first].r;
  return hit.second<radius*1.5?std::max(.2,radius/(size*.16)):1.;
 }
};
struct Chain{F3 p0,p1,p2,n;double width,total;};
struct Tears {
 double size=1,reach=0,gate=.65;std::vector<PatchRec> patches;std::vector<Chain> chains;KD tree;bool hasTree=false;
 double value(const F3&co,const F3&normal)const{
  if(!hasTree)return kInf;double result=kInf;
  tree.range(co,reach,[&](int index,double){
   const Chain&c=chains[index];if(dot(normal,c.n)<gate)return;double best=kInf,run=0;
   const F3* seg[2][2]={{&c.p0,&c.p1},{&c.p1,&c.p2}};
   for(auto&s:seg){const F3 ab=*s[1]-*s[0];const double L=length(ab);const double t=std::max(0.,std::min(1.,dot(co-*s[0],ab)/std::max(L*L,1e-20)));
    const double d=length(co-(*s[0]+mulf(ab,t)));const double u=(run+t*L)/std::max(c.total,1e-12);
    const double half=c.width*std::pow(std::max(0.,1.-u),1.2);best=std::min(best,d-half);run+=L;}
   result=std::min(result,best/size);
  });
  return result;
 }
 bool along(const F3&co,double&out)const{
  if(!hasTree)return false;bool found=false;double best=0;
  tree.range(co,reach,[&](int index,double){
   const Chain&c=chains[index];double run=0;const F3* seg[2][2]={{&c.p0,&c.p1},{&c.p1,&c.p2}};
   for(auto&s:seg){const F3 ab=*s[1]-*s[0];const double L=length(ab);const double t=std::max(0.,std::min(1.,dot(co-*s[0],ab)/std::max(L*L,1e-20)));
    if(length(co-(*s[0]+mulf(ab,t)))<=c.width*1.6){const double u=(run+t*L)/std::max(c.total,1e-12);best=found?std::min(best,u):u;found=true;}run+=L;}
  });
  out=best;return found;
 }
};
struct Secondary {
 const SmallDamage*sat=nullptr;const Tears*rips=nullptr;std::vector<PatchRec> patches;
 double value(const F3&co,const F3&n)const{double r=kInf;if(sat)r=std::min(r,sat->value(co,n));if(rips)r=std::min(r,rips->value(co,n));return r;}
 double scale(const F3&co)const{return sat?sat->scale(co):1.;}
 bool along(const F3&co,double&s)const{return rips&&rips->along(co,s);}
};

struct PassStats{int faces=0,contour_edges=0,small_patches=0,tears=0;bool limited=false;};

// ---- Engine ---------------------------------------------------------------------------
class Engine {
public:
 const Input&in;
 std::vector<F3> co,vno;std::vector<uint8_t> origV,vAlive;std::vector<Prov> prov;
 std::vector<Face> faces;std::vector<EdgeRec> edges;EdgeTable table;
 std::vector<std::array<int32_t,3>> baseTri;
 size_t aliveFaces=0;int budget=0;
 std::unique_ptr<Drawing> drawing;

 explicit Engine(const Input&i):in(i),budget(i.budget){}

 static uint64_t key(int a,int b){if(a>b)std::swap(a,b);return (uint64_t(uint32_t(a))<<32)|uint32_t(b);}
 int getEdge(int a,int b){
  const uint64_t k=key(a,b);int e=table.get(k);
  if(e<0){e=int(edges.size());edges.push_back(EdgeRec{std::min(a,b),std::max(a,b)});table.set(k,e);}
  else if(!edges[e].alive){edges[e].alive=true;edges[e].orig=-1;edges[e].sharp=false;edges[e].nf=0;edges[e].f[0]=edges[e].f[1]=-1;edges[e].extra.clear();}
  return e;
 }
 int addVert(const F3&p,const Prov&pr,uint8_t orig,const F3&no){co.push_back(p);vno.push_back(no);origV.push_back(orig);vAlive.push_back(1);prov.push_back(pr);return int(co.size())-1;}
 int addFace(int n,const int*vs,const Face&ex,const float w[][3]){
  Face f=ex;f.n=int8_t(n);f.alive=1;
  for(int i=0;i<n;++i){f.v[i]=vs[i];std::memcpy(f.w[i],w[i],sizeof(float)*3);}
  for(int i=n;i<4;++i){f.v[i]=-1;f.e[i]=-1;}
  const int id=int(faces.size());
  for(int i=0;i<n;++i){const int e=getEdge(vs[i],vs[(i+1)%n]);f.e[i]=e;edges[e].addFace(id);}
  faces.push_back(f);++aliveFaces;return id;
 }
 void removeFace(int id){Face&f=faces[id];if(!f.alive)return;f.alive=0;--aliveFaces;for(int i=0;i<f.n;++i)edges[f.e[i]].removeFace(id);}

 void normalUpdate(){
  const size_t nf=faces.size(),nv=co.size();
  std::vector<std::array<float,4>> ang(nf);
  pfor(nf,[&](size_t i){
   Face&f=faces[i];if(!f.alive)return;
   if(f.n==3)f.no=triNormal(co[f.v[0]],co[f.v[1]],co[f.v[2]]);
   else f.no=normalized(cross(co[f.v[0]]-co[f.v[2]],co[f.v[1]]-co[f.v[3]]));
   for(int k=0;k<f.n;++k){
    const F3 a=normalized(co[f.v[(k+f.n-1)%f.n]]-co[f.v[k]]),b=normalized(co[f.v[(k+1)%f.n]]-co[f.v[k]]);
    const double d=std::clamp(dot(a,b),-1.,1.);ang[i][k]=float(std::acos(d));
   }
  },1024);
  std::vector<F3> sum(nv,F3{0,0,0});
  for(size_t i=0;i<nf;++i){const Face&f=faces[i];if(!f.alive)continue;for(int k=0;k<f.n;++k){const F3 add=f.no*ang[i][k];F3&s=sum[f.v[k]];s[0]+=add[0];s[1]+=add[1];s[2]+=add[2];}}
  pfor(nv,[&](size_t i){const F3 n=normalized(sum[i]);vno[i]=length(n)>0?n:normalized(co[i]);},4096);
 }

 // -------- Set up from host arrays
 void load(){
  const size_t nv=in.points.size()/3;co.resize(nv);vno.assign(nv,F3{0,0,1});origV.assign(nv,1);vAlive.assign(nv,1);prov.resize(nv);
  for(size_t i=0;i<nv;++i){co[i]={in.points[3*i],in.points[3*i+1],in.points[3*i+2]};prov[i].s[0]=int32_t(i);prov[i].w[0]=1;}
  // Polygon and vertex normals of the original, untriangulated polygons.
  const size_t np=in.poly_offsets.size()-1;std::vector<F3> polyNo(np);std::vector<F3> sum(nv,F3{0,0,0});
  for(size_t p=0;p<np;++p){
   const int a=in.poly_offsets[p],b=in.poly_offsets[p+1],n=b-a;F3 no{0,0,0};
   if(n==3){const int*c=&in.corner_vert[a];no=triNormal(co[c[0]],co[c[1]],co[c[2]]);}
   else if(n==4){const int*c=&in.corner_vert[a];no=normalized(cross(co[c[0]]-co[c[2]],co[c[1]]-co[c[3]]));}
   else if(n>4){double acc[3]={0,0,0};for(int i=0;i<n;++i){const F3&u=co[in.corner_vert[a+i]];const F3&w=co[in.corner_vert[a+(i+1)%n]];acc[0]+=(double(u[1])-w[1])*(double(u[2])+w[2]);acc[1]+=(double(u[2])-w[2])*(double(u[0])+w[0]);acc[2]+=(double(u[0])-w[0])*(double(u[1])+w[1]);}
    no=normalized(F3{float(acc[0]),float(acc[1]),float(acc[2])});}
   polyNo[p]=no;
   for(int i=0;i<n;++i){const int v=in.corner_vert[a+i];const F3 x=normalized(co[in.corner_vert[a+(i+n-1)%n]]-co[v]),y=normalized(co[in.corner_vert[a+(i+1)%n]]-co[v]);
    const float ang=float(std::acos(std::clamp(dot(x,y),-1.,1.)));const F3 add=no*ang;F3&s=sum[v];s[0]+=add[0];s[1]+=add[1];s[2]+=add[2];}
  }
  for(size_t i=0;i<nv;++i){const F3 n=normalized(sum[i]);vno[i]=length(n)>0?n:normalized(co[i]);}
  const size_t ne=in.edges.size()/2;edges.reserve(ne*2);
  for(size_t e=0;e<ne;++e){const int id=getEdge(in.edges[2*e],in.edges[2*e+1]);edges[id].orig=int32_t(e);edges[id].sharp=!in.edge_sharp.empty()&&in.edge_sharp[e];}
  const size_t nt=in.tri_poly.size();baseTri.resize(nt);faces.reserve(nt*3);
  for(size_t t=0;t<nt;++t){
   const int c[3]={in.tri_corners[3*t],in.tri_corners[3*t+1],in.tri_corners[3*t+2]};baseTri[t]={c[0],c[1],c[2]};
   const int vs[3]={in.corner_vert[c[0]],in.corner_vert[c[1]],in.corner_vert[c[2]]};const int p=in.tri_poly[t];
   Face ex;ex.base=int32_t(t);ex.poly=p;ex.mat=in.poly_material[p];ex.smooth=uint8_t(in.poly_smooth[p]!=0);ex.stamp=in.poly_stamp[p];ex.no=polyNo[p];ex.paper=0;
   const float w[3][3]={{1,0,0},{0,1,0},{0,0,1}};addFace(3,vs,ex,w);
  }
 }
 int components()const{
  std::vector<int32_t> uf(co.size());std::iota(uf.begin(),uf.end(),0);
  auto find=[&](int x){while(uf[x]!=x){uf[x]=uf[uf[x]];x=uf[x];}return x;};
  for(const auto&e:edges)if(e.alive){const int a=find(e.a),b=find(e.b);if(a!=b)uf[a]=b;}
  int c=0;for(size_t i=0;i<co.size();++i)if(vAlive[i]&&find(int(i))==int(i))++c;
  return c;
 }
 float groupWeight(int v)const{
  if(in.group_weight.empty())return 0.f;const Prov&p=prov[v];float s=0;for(int i=0;i<3;++i)if(p.s[i]>=0)s+=p.w[i]*in.group_weight[p.s[i]];return s;
 }
 void triangulateQuads(){
  const size_t n=faces.size();
  for(size_t i=0;i<n;++i){
   if(!faces[i].alive||faces[i].n!=4)continue;splitQuad(int(i));
  }
 }
 void splitQuad(int id){
  const Face f=faces[id];const F3&a=co[f.v[0]],&b=co[f.v[1]],&c=co[f.v[2]],&d=co[f.v[3]];
  const double cost=beautyCost(a,b,c,d);
  const int order[2][3]={{0,1,2},{0,2,3}},alt[2][3]={{0,1,3},{1,2,3}};
  const auto&use=cost<0?order:alt;
  removeFace(id);
  for(int t=0;t<2;++t){int vs[3];float w[3][3];for(int k=0;k<3;++k){vs[k]=f.v[use[t][k]];std::memcpy(w[k],f.w[use[t][k]],sizeof(float)*3);}
   const int nid=addFace(3,vs,f,w);faces[nid].no=f.no;}
 }
 // -------- Splits
 int splitEdge(int e,int from,float t,bool orig0){
  const EdgeRec old=edges[e];const int to=old.a==from?old.b:old.a;
  const F3 p=lerp(co[from],co[to],t);const F3 no=normalized(lerp(vno[from],vno[to],t));
  const int m=addVert(p,mergeProv(prov[from],prov[to],t),orig0?1:0,no);
   for(int x:{from,to}){const int ne=getEdge(x,m);edges[ne].orig=old.orig;edges[ne].sharp=old.sharp;}
  return m;
 }

 // -------- Peeling pass
 PassStats peelPass(const PassParams&c);
 void dissolve();
 void emit(Output&out);
 // Polygon result of dissolve() (loops).
 struct Loop{int v,next,prev,edge,face,corner;uint8_t alive;};
 std::vector<Loop> loops;std::vector<int> faceHead;
 bool hasDissolved=false;
};

PassStats Engine::peelPass(const PassParams&c){
 PassStats stats;Profile prof;
 const double size=c.size,step=c.step,bend=c.bend;
 Field field(c,drawing.get());
 auto faceOk=[&](const Face&f){return c.paper_only?f.paper==1:(f.stamp==0||f.stamp==kStamp);};
 auto edgeOk=[&](const EdgeRec&e){
  if(e.nf==0)return false;
  const auto check=[&](int fid){return faceOk(faces[fid]);};
  bool any=false,all=true;
  for(int i=0;i<std::min(e.nf,2);++i){const bool b=check(e.f[i]);any=any||b;all=all&&b;}
  for(int x:e.extra){const bool b=check(x);any=any||b;all=all&&b;}
  return c.edge_all?all:any;
 };
 auto edgeAny=[&](const EdgeRec&e,bool wanted){ // does any linked face have faceOk==wanted
  for(int i=0;i<std::min(e.nf,2);++i)if(faceOk(faces[e.f[i]])==wanted)return true;
  for(int x:e.extra)if(faceOk(faces[x])==wanted)return true;
  return false;
 };
 // Protected structure.
 std::vector<F3> guardPts;
 for(const EdgeRec&e:edges){
  if(!e.alive||e.nf==0||!edgeAny(e,true))continue;
  bool hit=e.nf!=2;
  if(!hit){const double d=std::clamp(dot(faces[e.f[0]].no,faces[e.f[1]].no),-1.,1.);hit=std::acos(d)>(c.wrap?kWrapHardEdge:.65);}
  if(!hit)hit=edgeAny(e,false);
  if(!hit)continue;
  const F3&a=co[e.a];const F3&b=co[e.b];
  const int n=std::max(1,std::min(2048,int(std::ceil(dist(b,a)/std::max(step,bend*.15)))));
  for(int j=0;j<=n;++j)guardPts.push_back(lerp(a,b,float(double(j)/n)));
 }
 const bool hasGuard=!guardPts.empty();KD guardTree(std::move(guardPts));
 const double collar=std::max(step*2,bend*1.1);
 std::vector<double> distances,fvals;std::vector<double> drawVals;
 const double nan=std::numeric_limits<double>::quiet_NaN();
 auto ensureDist=[&](){
  if(!hasGuard)return;const size_t old=distances.size();distances.resize(co.size(),nan);
  pfor(co.size()-old,[&](size_t k){const size_t i=old+k;distances[i]=guardTree.nearest(co[i]).second;},512);
 };
 auto refreshDrawing=[&](){
  if(!c.use_drawing||!drawing)return;drawVals.resize(co.size(),nan);
  pfor(co.size(),[&](size_t i){if(vAlive[i])drawVals[i]=drawing->valueAt(co[i],vno[i]);},256);
 };
 auto fieldVert=[&](size_t i){
  if(c.use_drawing&&drawing&&i<drawVals.size()&&!std::isnan(drawVals[i])){const double F=field.fromDrawing(co[i],drawVals[i]);return c.damage<.999?field.craze(co[i],F):F;}
  return field.at(co[i]);
 };
 // Region eligibility.
 std::vector<int32_t> eligibleFaces;for(size_t i=0;i<faces.size();++i)if(faces[i].alive&&faceOk(faces[i]))eligibleFaces.push_back(int32_t(i));
 if(eligibleFaces.empty())return stats;
 double area=0;for(int f:eligibleFaces){const Face&q=faces[f];area+=.5*length(cross(co[q.v[1]]-co[q.v[0]],co[q.v[2]]-co[q.v[0]]));}
 const double probe=std::max(size*.125,std::sqrt(area/150000.));
 std::vector<F3> seedPts;
 if(in.adaptive){
  if(c.use_drawing&&drawing){
   const double spacing=std::min(probe,std::max(step,drawing->radius*.7));
   for(const Region&r:drawing->regions){
    std::vector<std::array<double,2>> samples;
    if(r.area){
     for(const auto&t:earClip(r.poly)){
      auto P=[&](int i){return F3{float(r.poly[i][0]),float(r.poly[i][1]),0.f};};
      triangleSamples(P(t[0]),P(t[1]),P(t[2]),spacing,[&](const F3&q){samples.push_back({q[0],q[1]});});
     }
    }else{
     for(size_t i=0;i+1<r.poly.size();++i){
      const F3 a{float(r.poly[i][0]),float(r.poly[i][1]),0},b{float(r.poly[i+1][0]),float(r.poly[i+1][1]),0};
      const int cnt=std::max(1,int(std::ceil(dist(b,a)/spacing)));
      for(int j=0;j<=cnt;++j){const F3 q=lerp(a,b,float(double(j)/cnt));samples.push_back({q[0],q[1]});
       for(int k=0;k<8;++k){const double ang=k*3.14159265358979323846/4;samples.push_back({q[0]+std::cos(ang)*drawing->radius,q[1]+std::sin(ang)*drawing->radius});}}
     }
    }
    std::vector<uint8_t> ok(samples.size(),0);std::vector<F3> hits(samples.size());
    pfor(samples.size(),[&](size_t i){
     const F3 loc=r.center+mulf(r.u,samples[i][0])+mulf(r.v,samples[i][1]);
     const auto h=drawing->nearest(loc);
     if(h.tri>=0&&h.distance<=r.slab&&dot(h.no,r.n)>drawing->gate){ok[i]=1;hits[i]=h.co;}
    },64);
    for(size_t i=0;i<samples.size();++i)if(ok[i])seedPts.push_back(hits[i]);
   }
  }else{
   std::vector<std::vector<F3>> per(eligibleFaces.size());
   pfor(eligibleFaces.size(),[&](size_t i){
    const Face&q=faces[eligibleFaces[i]];
    if(c.use_group){bool any=false;for(int k=0;k<3;++k)if(groupWeight(q.v[k])>.49f)any=true;if(!any)return;}
    triangleSamples(co[q.v[0]],co[q.v[1]],co[q.v[2]],probe,[&](const F3&s){
     if(c.damage<.999?(field.pitPatch(field.procedural(s))>0||field.at(s)<.08):field.at(s)<.08)per[i].push_back(s);});
   },16);
   for(auto&v:per)seedPts.insert(seedPts.end(),v.begin(),v.end());
  }
  if(seedPts.empty())return stats;
 }
  prof.mark("seeds");
bool limited=probe>size*.126;
 const double fine=1.-.4*c.ragged;
 const double contourStep=c.contour_step,bendStep=c.bend_step,reach=c.reach;
 KD seedTree(std::move(seedPts));const bool hasSeeds=!seedTree.p.empty();
 const double toDistance=size/1.2;

 std::vector<int32_t> pool;for(size_t i=0;i<edges.size();++i)if(edges[i].alive&&edgeOk(edges[i]))pool.push_back(int32_t(i));

 // Rivara closure, then conforming midpoint split. Returns the edges of the new faces.
 std::vector<uint8_t> chosenFlag;std::vector<int32_t> midOf;
 auto closure=[&](std::vector<int32_t> sel){
  chosenFlag.assign(edges.size(),0);std::vector<int32_t> out;out.reserve(sel.size());
  for(int e:sel)if(!chosenFlag[e]){chosenFlag[e]=1;out.push_back(e);}
  std::vector<int32_t> queue=out;
  auto visit=[&](int fid){
   const Face&f=faces[fid];if(f.n!=3)return;int longest=f.e[0];double best=-1;
   for(int k=0;k<3;++k){const double L=dist(co[f.v[k]],co[f.v[(k+1)%3]]);if(L>best){best=L;longest=f.e[k];}}
   if(!chosenFlag[longest]&&edgeOk(edges[longest])){chosenFlag[longest]=1;out.push_back(longest);queue.push_back(longest);}
  };
  while(!queue.empty()){const int e=queue.back();queue.pop_back();
   for(int i=0;i<std::min(edges[e].nf,2);++i)visit(edges[e].f[i]);
   for(size_t i=0;i<edges[e].extra.size();++i)visit(edges[e].extra[i]);}
  return out;
 };
 auto splitTriangles=[&](const std::vector<int32_t>&sel){
  std::vector<int32_t> affected;
  std::vector<uint8_t> seenFace(faces.size(),0);
  for(int e:sel){const EdgeRec&r=edges[e];auto add=[&](int f){if(!seenFace[f]){seenFace[f]=1;affected.push_back(f);}};for(int i=0;i<std::min(r.nf,2);++i)add(r.f[i]);for(int x:r.extra)add(x);}
  midOf.assign(edges.size(),-1);
  std::vector<int32_t> old(sel);
  for(int e:sel){const int a=edges[e].a;midOf[e]=splitEdge(e,a,.5f,false);}
  std::vector<int32_t> updated;
  std::vector<uint8_t> edgeSeen;
  auto note=[&](int fid){
   const Face&f=faces[fid];edgeSeen.resize(edges.size(),0);
   for(int k=0;k<f.n;++k)if(!edgeSeen[f.e[k]]){edgeSeen[f.e[k]]=1;updated.push_back(f.e[k]);}
  };
  for(int fid:affected){
   const Face F=faces[fid];if(F.n!=3)throw std::runtime_error("Peeling: refinement expects triangles");
   int ms[3];for(int k=0;k<3;++k){const int e=F.e[k];ms[k]=e<int(midOf.size())?midOf[e]:-1;}
   const int n=(ms[0]>=0)+(ms[1]>=0)+(ms[2]>=0);
   removeFace(fid);
   struct C{int v;float w[3];};
   auto corner=[&](int k){C x;x.v=F.v[k];std::memcpy(x.w,F.w[k],sizeof x.w);return x;};
   auto middle=[&](int k){C x;x.v=ms[k];const int j=(k+1)%3;for(int q=0;q<3;++q)x.w[q]=(F.w[k][q]+F.w[j][q])*.5f;return x;};
   auto tri=[&](const C&A,const C&B,const C&Cc){const int vs[3]={A.v,B.v,Cc.v};const float w[3][3]={{A.w[0],A.w[1],A.w[2]},{B.w[0],B.w[1],B.w[2]},{Cc.w[0],Cc.w[1],Cc.w[2]}};note(addFace(3,vs,F,w));};
   if(n==3){const C a=corner(0),b=corner(1),cc=corner(2),ab=middle(0),bc=middle(1),ca=middle(2);tri(a,ab,ca);tri(ab,b,bc);tri(ca,bc,cc);tri(ab,bc,ca);}
   else if(n==1){int i=0;while(ms[i]<0)++i;const C a=corner(i),b=corner((i+1)%3),cc=corner((i+2)%3),m=middle(i);tri(a,m,cc);tri(m,b,cc);}
   else{int i=0;while(!(ms[i]>=0&&ms[(i+1)%3]>=0))++i;const C a=corner(i),b=corner((i+1)%3),cc=corner((i+2)%3),ab=middle(i),bc=middle((i+1)%3);tri(ab,b,bc);tri(a,ab,cc);tri(ab,bc,cc);}
  }
  for(int e:old){EdgeRec&r=edges[e];if(r.nf==0)r.alive=false;}
  return updated;
 };
 // Refinement.
 for(int iter=0;iter<16;++iter){
  fvals.resize(co.size(),nan);
  if(c.use_drawing)refreshDrawing();
  // Field values of every pool vertex.
  {std::vector<int32_t> need;std::vector<uint8_t> mark(co.size(),0);
   for(int e:pool)for(int v:{edges[e].a,edges[e].b})if(!mark[v]&&std::isnan(fvals[v])){mark[v]=1;need.push_back(v);}
   pfor(need.size(),[&](size_t i){fvals[need[i]]=fieldVert(need[i]);},64);}
  // Selection (RefineAdaptive with a shared seed tree).
  std::vector<double> prio(pool.size(),-1);
  pfor(pool.size(),[&](size_t i){
   const EdgeRec&e=edges[pool[i]];const F3&A=co[e.a];const F3&B=co[e.b];
   const double L=dist(B,A);if(L<=contourStep*fine*1.4)return;
   const F3 mid{float(.5*(double(A[0])+B[0])),float(.5*(double(A[1])+B[1])),float(.5*(double(A[2])+B[2]))};
   if(in.adaptive&&(!hasSeeds||seedTree.nearest(mid).second>L*.5+reach+probe*1.5))return;
   double target=bendStep,dc=reach;const double fa=fvals[e.a],fb=fvals[e.b];
   if(in.adaptive&&std::isfinite(fa)&&std::isfinite(fb)){
    dc=(fa<0)!=(fb<0)?0.:std::min(std::fabs(fa),std::fabs(fb))*toDistance;const bool coat=fa>=0&&fb>=0;
    if(dc<(c.damage<.999?1.:1.5)*contourStep)target=contourStep*fine;
    else if(coat&&dc<reach*1.15+L*.5)target=c.damage<.999?std::clamp(1.2*dc,contourStep*fine,bendStep):bendStep; // graded, so small plates keep an unbent middle
    else if(coat&&dc<reach*1.6+L*.5)target=bendStep*2.5;
    else if(coat&&c.damage>=.999)return;
    else if(coat)target=kInf;
    else target=dc<bend*.5+L*.5?bendStep:bendStep*3.;
   }
   if(c.damage<.999){
    // Enough mesh to carry the plate relief, and to catch pits smaller than an edge.
    const F3 midF=divf(A+B,2.);const double base=c.use_drawing&&drawing?field.fromDrawing(midF,drawing->value(midF)):field.procedural(midF);
    if(field.crackPatch(base)>0)target=std::min(target,c.relief_step);
    double r;const double dp=field.pit(midF,base,r);if(r>0&&dp<L*.5+r*2.5)target=std::min(target,contourStep*fine);
    if(!std::isfinite(target))return;
   }
   if(dot(vno[e.a],vno[e.b])<.98)target=std::min(target,bendStep*.75);
   if(L>target*1.4)prio[i]=L/target*(1.+contourStep/(contourStep+dc));
  },512);
  std::vector<std::pair<double,int32_t>> chosen;for(size_t i=0;i<pool.size();++i)if(prio[i]>=0)chosen.push_back({prio[i],pool[i]});
  std::stable_sort(chosen.begin(),chosen.end(),[](const auto&a,const auto&b){return a.first>b.first;});
  if(chosen.empty())break;
  std::vector<int32_t> sel;sel.reserve(chosen.size());for(const auto&x:chosen)sel.push_back(x.second);
  sel=closure(std::move(sel));
  const long long room=(((long long)(budget)-(long long)(aliveFaces))>=0)?((long long)(budget)-(long long)(aliveFaces))/2:-1;
  if(room<=0){limited=true;break;}
  if((long long)sel.size()>room){sel.resize(size_t(room));limited=true;}
  auto updated=splitTriangles(sel);pool.clear();
  for(int e:updated)if(edges[e].alive&&edgeOk(edges[e]))pool.push_back(e);
  normalUpdate();
  if(limited||pool.empty())break;
 }
 prof.mark("refine");
 normalUpdate();
 if(c.use_drawing)refreshDrawing();

 std::vector<uint8_t> eligibleV,protectedV;std::vector<double> values;
 SmallDamage satellites;Tears rips;Secondary secondary;bool hasSmall=false;
 const double gate=c.wrap?kWrapGate:.65;
 satellites.gate=rips.gate=gate;satellites.heightLimit=c.wrap?1.6:.65;
 auto markRegions=[&](){
  eligibleV.assign(co.size(),0);protectedV.assign(co.size(),0);
  for(const Face&f:faces){if(!f.alive)continue;const bool ok=faceOk(f);for(int k=0;k<f.n;++k)(ok?eligibleV:protectedV)[f.v[k]]=1;}
 };
 auto computeValues=[&](){
  ensureDist();fvals.resize(co.size(),nan);values.assign(co.size(),.5);
  pfor(co.size(),[&](size_t i){
   if(!vAlive[i]||!eligibleV[i])return;
   if(std::isnan(fvals[i]))fvals[i]=fieldVert(i);
   double val=fvals[i];
   if(hasSmall)val=std::min(val,secondary.value(co[i],vno[i]));
   if(hasGuard)val=std::max(val,(1.-distances[i]/collar)*.5);
   if(c.use_group)val=std::max(val,(.5-double(groupWeight(int(i))))*.8);
   if(protectedV[i])val=std::max(val,.1);
   values[i]=std::fabs(val)>1e-7?val:1e-7;
  },128);
 };
 markRegions();computeValues();

 prof.mark("values");
 auto rimPoints=[&](){
  std::vector<RimPoint> border;
  for(const EdgeRec&e:edges){
   if(!e.alive)continue;const double va=values[e.a],vb=values[e.b];
   if(va*vb<0){const float t=float(va/(va-vb));const F3 n=lerp(vno[e.a],vno[e.b],t);if(length(n)<1e-12)continue;border.push_back({lerp(co[e.a],co[e.b],t),normalized(n)});}
  }
  return border;
 };
 std::unique_ptr<TriBVH> surface;
 auto buildSurface=[&](){
  std::vector<std::array<F3,3>> tris;tris.reserve(aliveFaces);
  for(const Face&f:faces){if(!f.alive)continue;tris.push_back({co[f.v[0]],co[f.v[1]],co[f.v[2]]});}
  surface=std::make_unique<TriBVH>(std::move(tris));
 };
 using CellKey=std::array<int64_t,3>;
 auto cellOf=[](const F3&p,double cell){return CellKey{int64_t(std::floor(double(p[0])/cell)),int64_t(std::floor(double(p[1])/cell)),int64_t(std::floor(double(p[2])/cell))};};
 // --- Small satellite chips.
 if(c.small_amount>0){
  satellites.size=size;satellites.maxRadius=size*.1;
  const auto border=rimPoints();
  if(!border.empty()){
   std::vector<F3> rc;rc.reserve(border.size());for(const auto&b:border)rc.push_back(b.co);KD rim(std::move(rc));
   buildSurface();
   const double cell=size*.16;std::map<CellKey,F3> thin;for(const auto&b:border)thin.emplace(cellOf(b.co,cell),b.co);
   const int r=int(std::ceil(size*.36/cell));std::vector<CellKey> keys;keys.reserve(thin.size()*27);
   for(const auto&kv:thin){const CellKey k=cellOf(kv.second,cell);
    for(int dx=-r;dx<=r;++dx)for(int dy=-r;dy<=r;++dy)for(int dz=-r;dz<=r;++dz)keys.push_back({k[0]+dx,k[1]+dy,k[2]+dz});}
   std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
   struct Cand{bool ok=false;F3 co,no;int index;double radius,priority,h5,h6;};std::vector<Cand> cands(keys.size());
   pfor(keys.size(),[&](size_t i){
    const CellKey&k=keys[i];double h[7];for(int ch=0;ch<7;++ch)h[ch]=hashUnit({int64_t(in.seed),49157,k[0],k[1],k[2],ch});
    const F3 loc{float((k[0]+h[0])*cell),float((k[1]+h[1])*cell),float((k[2]+h[2])*cell)};
    const auto hit=surface->nearest(loc);if(hit.tri<0||hit.distance>cell)return;
    const auto rh=rim.nearest(hit.co);if(!(size*.07<rh.second&&rh.second<size*.36))return;
    if(dot(hit.no,border[rh.first].n)<gate)return;
    if(field.at(hit.co)<=0)return;
    const double ratio=(rh.second-size*.07)/(size*.29);
    Cand&q=cands[i];q.ok=true;q.co=hit.co;q.no=hit.no;q.index=rh.first;q.radius=size*(.022+.055*std::pow(h[3],2.2));q.priority=h[4]*(.55+.45*ratio);q.h5=h[5];q.h6=h[6];
   },64);
   std::map<CellKey,std::vector<int>> cells;std::vector<int> accepted;const double grid=size*.3;
   for(size_t i=0;i<cands.size();++i){
    const Cand&q=cands[i];if(!q.ok)continue;const CellKey cc=cellOf(q.co,grid);bool clash=false;
    for(int dx=-1;dx<=1&&!clash;++dx)for(int dy=-1;dy<=1&&!clash;++dy)for(int dz=-1;dz<=1&&!clash;++dz){
     auto it=cells.find({cc[0]+dx,cc[1]+dy,cc[2]+dz});if(it==cells.end())continue;
     for(int o:it->second)if(dist(q.co,cands[o].co)<(q.radius+cands[o].radius)*1.6){clash=true;break;}
    }
    if(clash)continue;accepted.push_back(int(i));cells[cc].push_back(int(i));
   }
   for(int i:accepted){
    const Cand&q=cands[i];if(!(q.priority<c.small_amount/100.))continue;
    const RimPoint&b=border[q.index];F3 g=q.co-b.co;g=g-mulf(b.n,dot(g,b.n));
    F3 tangent=length(g)>1e-12?normalized(cross(b.n,g)):normalized(orthogonal(b.n));
    const double turn=(q.h5-.5)*.9;
    tangent=normalized(mulf(tangent,std::cos(turn))+mulf(cross(b.n,tangent),std::sin(turn)));
    satellites.patches.push_back({q.co,b.n,float(q.radius)});
    satellites.shapes.push_back({tangent,.35+.4*q.h6});
   }
   if(!satellites.patches.empty()){
    std::vector<F3> pts;for(const auto&p:satellites.patches)pts.push_back(p.co);satellites.tree=KD(std::move(pts));satellites.hasTree=true;
    double m=0;for(const auto&p:satellites.patches)m=std::max(m,double(p.r));satellites.maxRadius=m;
   }
  }
  stats.small_patches=int(satellites.patches.size());
 }
 // --- Tears.
 if(c.tears>0){
  rips.size=size;
  const auto border=rimPoints();
  if(!border.empty()){
   const double cell=bend*.9;std::map<CellKey,std::vector<int>> groups;
   for(size_t i=0;i<border.size();++i)groups[cellOf(border[i].co,cell)].push_back(int(i));
   std::vector<const std::pair<const CellKey,std::vector<int>>*> list;for(const auto&g:groups)list.push_back(&g);
   struct Out{bool ok=false;Chain chain;std::vector<PatchRec> patches;};std::vector<Out> outs(list.size());
   pfor(list.size(),[&](size_t gi){
    const CellKey&k=list[gi]->first;const auto&members=list[gi]->second;double h[9];
    for(int ch=0;ch<9;++ch)h[ch]=hashUnit({int64_t(in.seed),7717,k[0],k[1],k[2],ch});
    if(h[0]>=c.tears/100.*.6)return;
    const F3 target{float((k[0]+h[1])*cell),float((k[1]+h[2])*cell),float((k[2]+h[3])*cell)};
    int index=members[0];double best=dist(border[index].co,target);
    for(int m:members){const double d=dist(border[m].co,target);if(d<best||(d==best&&m<index)){best=d;index=m;}}
    const F3 origin=border[index].co;const F3 n=border[index].n;
    const double hh=std::max(step*.5,size*.004);
    const float hf=float(hh);
    F3 grad{float(field.at(origin+F3{hf,0,0})-field.at(origin-F3{hf,0,0})),float(field.at(origin+F3{0,hf,0})-field.at(origin-F3{0,hf,0})),float(field.at(origin+F3{0,0,hf})-field.at(origin-F3{0,0,hf}))};
    grad=divf(grad,2*hh);grad=grad-mulf(n,dot(grad,n));
    if(length(grad)<1e-9)return;
    F3 d=normalized(grad);const double turn=(h[4]-.5)*.9;const F3 side=cross(n,d);
    d=normalized(mulf(d,std::cos(turn))+mulf(side,std::sin(turn)));
    const double len=bend*(.45+.75*h[5]),width=std::max(bend*.08,step*.6)*(.8+.5*h[6]),kink=(h[7]-.5)*.7;
    const F3 d2=normalized(mulf(d,std::cos(kink))+mulf(cross(n,d),std::sin(kink)));
    const F3 p0=origin-mulf(d,width*1.5),p1=origin+mulf(d,len*.5),p2=p1+mulf(d2,len*.5);
    Out&o=outs[gi];o.ok=true;o.chain={p0,p1,p2,n,width,len*(1.+1.5*width/len)};
    const double fineW=std::max(width*.9,step*.5);const int count=std::max(2,int(std::ceil(len/std::max(width,1e-9))));
    for(int j=0;j<=count;++j){const double t=double(j)/count;const F3 q=t<.5?lerp(p0,p1,float(std::min(1.,t*2))):lerp(p1,p2,float((t-.5)*2));o.patches.push_back({q,n,float(fineW/.4)});}
   },16);
   for(auto&o:outs)if(o.ok){rips.chains.push_back(o.chain);rips.patches.insert(rips.patches.end(),o.patches.begin(),o.patches.end());}
   if(!rips.chains.empty()){
    std::vector<F3> pts;double m=0;for(const auto&q:rips.chains){pts.push_back(divf(q.p0+q.p2,2.));m=std::max(m,q.total);}
    rips.tree=KD(std::move(pts));rips.hasTree=true;rips.reach=m;
   }
  }
  stats.tears=int(rips.chains.size());
 }
 if(!satellites.patches.empty()||!rips.patches.empty()){
  if(!satellites.patches.empty()){secondary.sat=&satellites;secondary.patches=satellites.patches;}
  if(!rips.patches.empty()){secondary.rips=&rips;secondary.patches.insert(secondary.patches.end(),rips.patches.begin(),rips.patches.end());}
  hasSmall=true;
  std::vector<F3> pts;pts.reserve(secondary.patches.size());for(const auto&p:secondary.patches)pts.push_back(p.co);KD patchTree(std::move(pts));
  std::vector<int32_t> micro;for(size_t i=0;i<edges.size();++i)if(edges[i].alive&&edges[i].nf>0)micro.push_back(int32_t(i));
  for(int it=0;it<12;++it){
   std::vector<uint8_t> pick(micro.size(),0);
   pfor(micro.size(),[&](size_t i){
    const EdgeRec&e=edges[micro[i]];if(!e.alive||!edgeOk(e))return;
    const F3&A=co[e.a];const F3&B=co[e.b];const F3 mid=divf(A+B,2.);const double L=dist(A,B);
    const auto hit=patchTree.nearest(mid);const PatchRec&p=secondary.patches[hit.first];const double radius=p.r;
    if(hit.second>L*.5+radius*2.2)return;
    if(dot(vno[e.a],p.n)<gate&&dot(vno[e.b],p.n)<gate)return;
    const double target=std::min(step,radius*.4);if(target<step&&L>target*1.4)pick[i]=1;
   },512);
   std::vector<int32_t> sel;for(size_t i=0;i<micro.size();++i)if(pick[i])sel.push_back(micro[i]);
   if(sel.empty())break;
   sel=closure(std::move(sel));
   if((long long)aliveFaces+(long long)sel.size()*2>budget){limited=true;break;}
   micro=splitTriangles(sel);normalUpdate();
  }
  if(c.use_drawing)refreshDrawing();
  markRegions();computeValues();
 }
 prof.mark("secondary");
 // Contour: split every sign-changing edge once.
 values.resize(co.size(),.5);
 midOf.assign(edges.size(),-1);
 {
  const size_t ne=edges.size();std::vector<int32_t> oldEdges;
  for(size_t e=0;e<ne;++e){
   if(!edges[e].alive)continue;const int a=edges[e].a,b=edges[e].b;const double fa=values[a],fb=values[b];
   if(fa*fb<0){
    const float t=float(std::min(.97,std::max(.03,fa/(fa-fb))));
    const int v=splitEdge(int(e),a,t,false);midOf[e]=v;values.push_back(0.);oldEdges.push_back(int32_t(e));
   }
  }
  const size_t nf=faces.size();
  for(size_t i=0;i<nf;++i){
   if(!faces[i].alive)continue;const Face F=faces[i];if(F.n!=3)continue;
   int ms[3];int count=0;for(int k=0;k<3;++k){ms[k]=midOf[F.e[k]];if(ms[k]>=0)++count;}
   if(!count)continue;
   float ts[3]={0,0,0};for(int k=0;k<3;++k)if(ms[k]>=0){ // parameter along v[k]->v[k+1]
     const F3&A=co[F.v[k]];const F3&B=co[F.v[(k+1)%3]];const F3&M=co[ms[k]];const double den=dist(A,B);ts[k]=den>0?float(dist(A,M)/den):.5f;}
   removeFace(int(i));
   struct C{int v;float w[3];};
   auto corner=[&](int k){C x;x.v=F.v[k];std::memcpy(x.w,F.w[k],sizeof x.w);return x;};
   auto middle=[&](int k){C x;x.v=ms[k];const int j=(k+1)%3;const float t=ts[k];for(int q=0;q<3;++q)x.w[q]=F.w[k][q]*(1.f-t)+F.w[j][q]*t;return x;};
   auto poly=[&](std::initializer_list<C> list){int vs[4];float w[4][3];int n=0;for(const C&x:list){vs[n]=x.v;std::memcpy(w[n],x.w,sizeof x.w);++n;}return addFace(n,vs,F,w);};
   if(count==2){
    int i0=0;while(!(ms[i0]>=0&&ms[(i0+2)%3]>=0))++i0; // corner i0 is cut off
    const C a=corner(i0),b=corner((i0+1)%3),cc=corner((i0+2)%3),m1=middle(i0),m2=middle((i0+2)%3);
    poly({a,m1,m2});const int q=poly({m1,b,cc,m2});splitQuad(q);
   }else if(count==1){
    int i0=0;while(ms[i0]<0)++i0;const C a=corner(i0),b=corner((i0+1)%3),cc=corner((i0+2)%3),m=middle(i0);poly({a,m,cc});poly({m,b,cc});
   }else{
    const C a=corner(0),b=corner(1),cc=corner(2),ab=middle(0),bc=middle(1),ca=middle(2);poly({a,ab,ca});poly({ab,b,bc});poly({ca,bc,cc});poly({ab,bc,ca});
   }
  }
  for(int e:oldEdges)if(edges[e].nf==0)edges[e].alive=false;else throw std::runtime_error("Peeling: contour edge kept a face");
 }
 normalUpdate();
 prof.mark("contour");
 // Compact face table for this stage.
 std::vector<int32_t> flist;flist.reserve(aliveFaces);std::vector<int32_t> findex(faces.size(),-1);
 for(size_t i=0;i<faces.size();++i)if(faces[i].alive){findex[i]=int32_t(flist.size());flist.push_back(int32_t(i));}
 const size_t nf=flist.size(),nv=co.size();
 std::vector<int32_t> hole0(nf,0);bool anyHole=false;
 for(size_t i=0;i<nf;++i){const Face&f=faces[flist[i]];double s=0;for(int k=0;k<f.n;++k)s+=values[f.v[k]];if(s<-1e-8){hole0[i]=1;anyHole=true;}}
 if(!anyHole){stats.limited=limited;return stats;}
 auto interfaceEdges=[&](const std::vector<uint8_t>&mask){
  std::vector<int32_t> out;for(size_t e=0;e<edges.size();++e){const EdgeRec&r=edges[e];if(r.alive&&r.nf==2&&mask[findex[r.f[0]]]!=mask[findex[r.f[1]]])out.push_back(int32_t(e));}return out;
 };
 std::vector<uint8_t> holeMask(nf);for(size_t i=0;i<nf;++i)holeMask[i]=uint8_t(hole0[i]);
 const auto initialIface=interfaceEdges(holeMask);
 // Rim parameters.
 std::vector<uint8_t> isRim(nv,0),crackRim(nv,0);std::vector<double> rimW(nv,1.),rimA(nv,1.);
 std::vector<int32_t> rimVerts;for(int e:initialIface)for(int v:{edges[e].a,edges[e].b})if(!isRim[v]){isRim[v]=1;rimVerts.push_back(v);}
 pfor(rimVerts.size(),[&](size_t i){
  const int v=rimVerts[i];const F3 q=divf(co[v],size)+c.rim_offset[0];const double k=size/std::max(1e-9,1.3*bend);
  double amp=1.-c.variation*(1.-std::max(0.,std::min(1.,.5+.75*N(mulf(q,k)))));
  const double ridge=1.-std::fabs(N(mulf(q,k*.7)+c.rim_offset[1]));
  const double spike=std::pow(std::max(0.,std::min(1.,(ridge-.95)/.05)),1.5);
  const double width=(.55+.2*N(mulf(q,k*.45)+c.rim_offset[2]))*(1.+c.tongues*2.2*spike);
  amp*=1.+.5*c.tongues*spike;
  rimW[v]=std::max(.3,width);rimA[v]=std::max(0.,amp);
  if(hasSmall&&fieldVert(size_t(v))>0){double s;if(secondary.along(co[v],s)){s=std::max(0.,std::min(1.,s));rimW[v]=-std::max(1e-3,rimW[v]*(1.-.55*s));rimA[v]=rimA[v]*std::pow(1.-s,1.5);}}
  if(c.damage<.999){
   // On a crack: the plate's edge cups up. The plate decides how much, so one
   // side of a crack often stands higher than the other; more with Damage Factor.
   const double base=c.use_drawing&&drawing?field.fromDrawing(co[v],drawing->value(co[v])):field.procedural(co[v]);
   Field::CrazeInfo info;field.craze(co[v],base,&info);
   if(info.crack){
    const double g=smooth01((c.damage-.1)/.7),h=info.hash,h2=std::fmod(h*7.31,1.);
    const double curlMore=h2<.35*g?1.+2.5*g:1.;
    rimW[v]=Field::kCrazeCell*size*(.13+.3*g)*(.6+.8*h2)*(curlMore>1.?1.4:1.)/bend;
    rimA[v]=(.25+.75*g)*(.3+.7*std::pow(h,1.5))*curlMore*std::max(.2,amp/std::max(1e-9,1.+.5*c.tongues*spike));
    crackRim[v]=1;
   }
  }
 },32);
 // Plan.
 std::vector<double> guards(nv,kInf);ensureDist();
 if(hasGuard||c.use_group){pfor(nv,[&](size_t i){double g=hasGuard?distances[i]:kInf;if(c.use_group&&groupWeight(int(i))<.25f)g=0.;if(std::isfinite(g))guards[i]=g;},2048);}
 std::vector<pd_peeling::V> dp(nv),dn(nv);
 for(size_t i=0;i<nv;++i){dp[i]={co[i][0],co[i][1],co[i][2]};dn[i]={vno[i][0],vno[i][1],vno[i][2]};}
 if(in.guide){
  std::vector<double> gp(nv*3);for(size_t i=0;i<nv;++i)for(int k=0;k<3;++k)gp[3*i+k]=dp[i][k];
  std::vector<int32_t> gq;gq.reserve(nf*4);
  for(int fid:flist){const Face&f=faces[fid];gq.insert(gq.end(),{f.v[0],f.v[1],f.v[2],f.v[2]});}
  const auto g=in.guide(gp,gq);if(g.size()==nv*3)for(size_t i=0;i<nv;++i)dn[i]={g[3*i],g[3*i+1],g[3*i+2]};
 }
 std::vector<pd_peeling::Tri> tri(nf);std::vector<int32_t> locked(nf,0);
 for(size_t i=0;i<nf;++i){const Face&f=faces[flist[i]];tri[i]={f.v[0],f.v[1],f.v[2]};bool l=!faceOk(f);for(int k=0;k<3&&!l;++k)if(edges[f.e[k]].nf!=2)l=true;locked[i]=l;}
 std::vector<double> scales(nv,1.);
 if(hasSmall){
  std::vector<int32_t> bv;for(int e:initialIface)for(int v:{edges[e].a,edges[e].b})bv.push_back(v);
  std::sort(bv.begin(),bv.end());bv.erase(std::unique(bv.begin(),bv.end()),bv.end());
  pfor(bv.size(),[&](size_t i){const int v=bv[i];if(fieldVert(size_t(v))>1e-5)scales[v]=secondary.scale(co[v]);},32);
 }
 std::vector<double> widths(nv,1.);for(size_t i=0;i<nv;++i)widths[i]=isRim[i]?rimW[i]:1.;
 prof.mark("rim");
 pd_peeling::CurlOptions planOptions;
 if(c.damage<.999){planOptions.fixed_scale=crackRim;planOptions.absorb=.05;}
 pd_peeling::CurlPlan plan=pd_peeling::PlanCurl(dp,dn,tri,hole0,locked,scales,widths,guards,bend,&planOptions);
 bool anyBorder=false,anyPlanHole=false;for(int b:plan.border)if(b){anyBorder=true;break;}for(int b:plan.hole)if(b){anyPlanHole=true;break;}
 if(!anyBorder||!anyPlanHole){stats.limited=limited;return stats;}
 prof.mark("plan");
 std::vector<uint8_t> hole(nf),band(nf),insideM(nf);
 for(size_t i=0;i<nf;++i){hole[i]=plan.hole[i]!=0;band[i]=plan.band[i]!=0;insideM[i]=hole[i]||band[i];}
 const auto boundary=interfaceEdges(hole);const auto outer=interfaceEdges(insideM);
 if(outer.empty()){stats.limited=limited;return stats;}
 const std::vector<F3> original=co;
 // Lip orientation, before the hole faces go away.
 struct Lip{int a,b,face;};std::vector<Lip> lips;lips.reserve(boundary.size());
 auto loopOf=[&](int fid,int e,int&a,int&b){const Face&f=faces[fid];for(int k=0;k<f.n;++k)if(f.e[k]==e){a=f.v[k];b=f.v[(k+1)%f.n];return;}throw std::runtime_error("Peeling: edge is not on its face");};
 for(int e:boundary){const EdgeRec&r=edges[e];const int fid=!hole[findex[r.f[0]]]?r.f[0]:r.f[1];Lip l;l.face=fid;loopOf(fid,e,l.a,l.b);lips.push_back(l);}
 // Local depth and offset normals.
 buildSurface();
 std::vector<uint8_t> insideV(nv,0),bandV(nv,0),topV(nv,0);
 for(size_t i=0;i<nf;++i){const Face&f=faces[flist[i]];if(insideM[i])for(int k=0;k<f.n;++k)insideV[f.v[k]]=1;if(band[i])for(int k=0;k<f.n;++k)bandV[f.v[k]]=1;}
 for(size_t i=0;i<nf;++i){if(hole[i])continue;const Face&f=faces[flist[i]];for(int k=0;k<f.n;++k)if(bandV[f.v[k]])topV[f.v[k]]=1;}
 // Skin that may move with the relief: not on hole-only vertices, not on anything protected.
 std::vector<uint8_t> eligibleSkin(nv,0);
 for(size_t i=0;i<nf;++i){if(hole[i])continue;const Face&f=faces[flist[i]];if(faceOk(f))for(int k=0;k<f.n;++k)eligibleSkin[f.v[k]]=1;}
 for(size_t i=0;i<nf;++i){if(locked[i]){const Face&f=faces[flist[i]];for(int k=0;k<f.n;++k)eligibleSkin[f.v[k]]=0;}}
 std::vector<int32_t> offsetVerts;for(size_t i=0;i<nv;++i)if(insideV[i])offsetVerts.push_back(int32_t(i));
 std::vector<double> localDepth(nv,c.depth);
 auto normalF=[&](size_t i){return F3{float(dn[i][0]),float(dn[i][1]),float(dn[i][2])};};
 const double eps=std::max(1e-8,std::min(step*.001,c.depth*.01));
 pfor(offsetVerts.size(),[&](size_t k){
  const int v=offsetVerts[k];const F3 nv3=normalF(v);
  const double hit=surface->ray(original[v]-mulf(nv3,eps),-nv3,c.depth*4);
  const double wall=std::isfinite(hit)?hit+eps:kInf;double d=c.depth;
  if(c.rough>0){const F3 q=divf(original[v],size)+c.rim_offset[0];d*=std::max(.35,1.+c.rough*(N(mulf(q,3.3))+.45*N(mulf(q,9.1)+c.rim_offset[1])));}
  localDepth[v]=std::min(d,wall*.4);
 },64);
 std::vector<pd_peeling::V> smoothed=dn;
 {
  const double radius=std::max(c.depth*(c.wrap?2.:1.5),1e-9);const double keep=c.wrap?kWrapGate:.5;std::vector<F3> pts;pts.reserve(offsetVerts.size());for(int v:offsetVerts)pts.push_back(co[v]);KD near(std::move(pts));
  pfor(offsetVerts.size(),[&](size_t k){
   const int v=offsetVerts[k];const F3 nv3=normalF(v);F3 acc{0,0,0};
   near.range(co[v],radius,[&](int j,double d){const F3 nu=normalF(offsetVerts[j]);if(dot(nu,nv3)>keep){const F3 add=mulf(nu,1.-d/radius);acc=acc+add;}});
   if(length(acc)>1e-12){const F3 u=normalized(acc);smoothed[v]={u[0],u[1],u[2]};}
  },64);
 }
 prof.mark("depth");
 // Deformation + collision pass.
 pd_peeling::CurlSettings s;s.lift=c.lift;s.bend=bend;s.thickness=c.thickness;s.curl=c.curl;s.droop=c.droop;s.rootBlend=.3;
 s.gravity={in.gravity[0],in.gravity[1],in.gravity[2]};
 std::vector<double> amps(nv,1.);for(size_t i=0;i<nv;++i)amps[i]=isRim[i]?rimA[i]:1.;
 pd_peeling::Offsets def=pd_peeling::DeformCurl(dp,smoothed,plan,amps,localDepth,s);
 // Exact inward offset at sharp edges and corners. Rounded normals alone would
 // pull the floor up along a crease by a different amount at every vertex and
 // leave it scalloped, so here the floor keeps depth to each crease plane.
 std::vector<std::pair<int32_t,pd_peeling::V>> floorFix;
 std::vector<int32_t> creaseEdges;
 if(c.wrap){
  struct Sample{F3 na,qa,nb,qb;bool convex;};std::vector<Sample> samples;std::vector<F3> samplePts;
  const double spacing=std::max(c.depth,step*.05);
  auto centre=[&](const Face&f){F3 m{0,0,0};for(int k=0;k<f.n;++k)m=m+co[f.v[k]];return divf(m,double(f.n));};
  for(size_t e=0;e<edges.size();++e){
   const EdgeRec&r=edges[e];if(!r.alive||r.nf!=2||!insideV[r.a]||!insideV[r.b])continue;
   const Face&x=faces[r.f[0]];const Face&y=faces[r.f[1]];
   const double angle=std::acos(std::clamp(dot(x.no,y.no),-1.,1.));if(angle<=.65||angle>kWrapHardEdge)continue;
   creaseEdges.push_back(int32_t(e));
   const bool convex=dot(x.no,centre(y)-co[r.a])<0;
   const int cnt=std::max(1,std::min(4096,int(std::ceil(dist(co[r.a],co[r.b])/spacing))));
   for(int j=0;j<=cnt;++j){samplePts.push_back(lerp(co[r.a],co[r.b],float(double(j)/cnt)));samples.push_back({x.no,co[r.a],y.no,co[r.a],convex});}
  }
  if(!samples.empty()){
   KD creaseTree(samplePts);floorFix.resize(offsetVerts.size(),{-1,pd_peeling::V{0,0,0}});
   std::vector<uint8_t> onCrease(nv,0);for(int e:creaseEdges){onCrease[edges[e].a]=1;onCrease[edges[e].b]=1;}
   pfor(offsetVerts.size(),[&](size_t k){
    const int v=offsetVerts[k];const double d=std::max(0.,localDepth[v]);if(d<=0)return;
    struct Plane{double n[3],q[3];bool convex;};Plane planes[12];int count=0;
    auto addPlane=[&](const F3&n,const F3&q,bool convex){
     for(int i=0;i<count;++i)if(planes[i].n[0]*n[0]+planes[i].n[1]*n[1]+planes[i].n[2]*n[2]>.9995){planes[i].convex=planes[i].convex&&convex;return;}
     if(count<12){planes[count]={{n[0],n[1],n[2]},{q[0],q[1],q[2]},convex};++count;}};
    creaseTree.range(co[v],d*2.2,[&](int j,double){const Sample&q=samples[j];addPlane(q.na,q.qa,q.convex);addPlane(q.nb,q.qb,q.convex);});
    if(count<2)return;
    // Depth wanted below each plane. Vertices that would pass the inner crease land
    // just beyond it, in their original order, so nothing folds or coincides.
    constexpr double kSpread=.05;
    double target[12];bool active[12];
    for(int i=0;i<count;++i){const Plane&P=planes[i];
     const double x0=std::max(0.,-((double(co[v][0])-P.q[0])*P.n[0]+(double(co[v][1])-P.q[1])*P.n[1]+(double(co[v][2])-P.q[2])*P.n[2]));
     active[i]=x0<d/(1.-kSpread)&&(P.convex||onCrease[v]);target[i]=d+kSpread*x0;}
    const F3 own=normalF(v);double x[3]={double(co[v][0])-own[0]*d,double(co[v][1])-own[1]*d,double(co[v][2])-own[2]*d};
    if(onCrease[v]){x[0]=co[v][0];x[1]=co[v][1];x[2]=co[v][2];}
    for(int it=0;it<24;++it){
     double worst=0;
     for(int i=0;i<count;++i){if(!active[i])continue;const Plane&P=planes[i];
      const double h=(x[0]-P.q[0])*P.n[0]+(x[1]-P.q[1])*P.n[1]+(x[2]-P.q[2])*P.n[2]+target[i];
      if(h>1e-9){x[0]-=P.n[0]*h;x[1]-=P.n[1]*h;x[2]-=P.n[2]*h;worst=std::max(worst,h);}
     }
     if(worst<d*1e-4)break;
    }
    // A near-parallel pair can run away along the crease; such a vertex keeps its normal offset.
    const double mx=x[0]-co[v][0],my=x[1]-co[v][1],mz=x[2]-co[v][2];
    if(std::sqrt(mx*mx+my*my+mz*mz)<=d*3.5)floorFix[k]={v,pd_peeling::V{x[0],x[1],x[2]}};
   },64);
  }
 }
 // Damage Factor: plate relief over the whole skin, carried by top and underside alike.
 std::vector<float> reliefH;
 if(c.damage<.999){
  reliefH.assign(nv,0.f);
  pfor(nv,[&](size_t i){
   if(!vAlive[i]||!eligibleSkin[i])return;
   const double F=c.use_drawing&&drawing?field.fromDrawing(original[i],drawing->value(original[i])):field.procedural(original[i]);
   double h=field.relief(original[i],F);
   if(hasGuard)h*=smooth01(distances[i]/collar);
   if(c.use_group)h*=smooth01(double(groupWeight(int(i)))*2.);
   reliefH[i]=float(h);
  },512);
  // A few smoothing passes over the skin: ridges cross triangles at any angle and
  // would otherwise show a washboard of the mesh.
  std::vector<std::pair<int32_t,int32_t>> links;links.reserve(edges.size());
  for(const EdgeRec&r:edges)if(r.alive&&size_t(r.a)<nv&&size_t(r.b)<nv&&eligibleSkin[r.a]&&eligibleSkin[r.b])links.push_back({r.a,r.b});
  std::vector<float> sum(nv),cnt(nv);
  for(int pass=0;pass<4;++pass){
   std::fill(sum.begin(),sum.end(),0.f);std::fill(cnt.begin(),cnt.end(),0.f);
   for(const auto&l:links){sum[l.first]+=reliefH[l.second];cnt[l.first]+=1;sum[l.second]+=reliefH[l.first];cnt[l.second]+=1;}
   for(size_t i=0;i<nv;++i)if(cnt[i]>0&&eligibleSkin[i])reliefH[i]=.5f*reliefH[i]+.5f*sum[i]/cnt[i];
  }
 }
 auto applyFloor=[&](){
  for(const auto&f:floorFix)if(f.first>=0)def.floor[f.first]=f.second;
  if(reliefH.empty())return;
  pfor(nv,[&](size_t i){const double h=reliefH[i];if(h==0.)return;
   for(int k=0;k<3;++k){def.top[i][k]+=smoothed[i][k]*h;def.back[i][k]+=smoothed[i][k]*h;}},4096);
 };
 applyFloor();
 // Collision polygons.
 struct CPoly{int32_t k[4];int8_t n;int32_t owner[3];int8_t no;};
 std::vector<CPoly> cpolys;std::vector<std::array<int32_t,2>> keys;std::vector<int32_t> keyIndex[4];
 for(auto&ki:keyIndex)ki.assign(nv,-1);
 auto at=[&](int kind,int v){int32_t&i=keyIndex[kind][v];if(i<0){i=int32_t(keys.size());keys.push_back({kind,v});}return i;};
 auto T=[&](int v){return at(topV[v]?0:3,v);};
 std::vector<uint8_t> inStatic(nf,0);
 {
  std::vector<std::vector<int32_t>> vf(nv);
  for(size_t i=0;i<nf;++i){const Face&f=faces[flist[i]];for(int k=0;k<f.n;++k)vf[f.v[k]].push_back(int32_t(i));}
  std::vector<int32_t> ring;std::vector<uint8_t> inRing(nv,0);
  for(size_t i=0;i<nf;++i)if(band[i]){const Face&f=faces[flist[i]];for(int k=0;k<f.n;++k)if(!inRing[f.v[k]]){inRing[f.v[k]]=1;ring.push_back(f.v[k]);}}
  for(int it=0;it<4;++it){
   std::vector<int32_t> fresh;for(int v:ring)for(int f:vf[v])if(!insideM[f]&&!inStatic[f]){inStatic[f]=1;fresh.push_back(f);}
   if(fresh.empty())break;
   std::vector<int32_t> nextRing;std::vector<uint8_t> seen(nv,0);
   for(int f:fresh){const Face&q=faces[flist[f]];for(int k=0;k<q.n;++k)if(!seen[q.v[k]]){seen[q.v[k]]=1;nextRing.push_back(q.v[k]);}}
   ring.swap(nextRing);
  }
 }
 auto pushPoly=[&](std::initializer_list<int32_t> ks,std::initializer_list<int32_t> own){CPoly q{};q.n=int8_t(ks.size());int i=0;for(int x:ks)q.k[i++]=x;q.no=int8_t(own.size());i=0;for(int x:own)q.owner[i++]=x;cpolys.push_back(q);};
 for(size_t i=0;i<nf;++i)if(band[i]){const Face&f=faces[flist[i]];const int a=f.v[0],b=f.v[1],d=f.v[2];pushPoly({T(a),T(b),T(d)},{a,b,d});pushPoly({at(1,d),at(1,b),at(1,a)},{a,b,d});}
 for(size_t i=0;i<nf;++i)if(insideM[i]){const Face&f=faces[flist[i]];const int a=f.v[0],b=f.v[1],d=f.v[2];pushPoly({at(2,a),at(2,b),at(2,d)},{a,b,d});}
 for(size_t i=0;i<nf;++i)if(inStatic[i]){const Face&f=faces[flist[i]];const int a=f.v[0],b=f.v[1],d=f.v[2];pushPoly({T(a),T(b),T(d)},{a,b,d});}
 for(const Lip&l:lips)pushPoly({T(l.b),T(l.a),at(1,l.a),at(1,l.b)},{l.a,l.b});
 for(int e:outer){const int a=edges[e].a,b=edges[e].b;if(bandV[a]&&bandV[b]&&insideV[a]&&insideV[b])pushPoly({at(2,b),at(2,a),at(1,a),at(1,b)},{a,b});}
 auto toF=[](const pd_peeling::V&d){return F3{float(d[0]),float(d[1]),float(d[2])};};
 auto collide=[&](std::vector<uint8_t>&hitOut){
  std::vector<F3> X(keys.size());
  pfor(keys.size(),[&](size_t i){const int kind=keys[i][0],v=keys[i][1];X[i]=kind==0?toF(def.top[v]):kind==1?toF(def.back[v]):kind==2?toF(def.floor[v]):reliefH.empty()?co[v]:co[v]+mulf(normalF(v),reliefH[v]);},4096);
  std::vector<std::array<F3,3>> tris;std::vector<int32_t> owner;tris.reserve(cpolys.size()*5/4);owner.reserve(cpolys.size()*5/4);
  for(size_t i=0;i<cpolys.size();++i){const CPoly&q=cpolys[i];tris.push_back({X[q.k[0]],X[q.k[1]],X[q.k[2]]});owner.push_back(int32_t(i));if(q.n==4){tris.push_back({X[q.k[0]],X[q.k[2]],X[q.k[3]]});owner.push_back(int32_t(i));}}
  TriBVH tree(std::move(tris));
  std::vector<std::atomic<uint8_t>> flags(cpolys.size());for(auto&f:flags)f.store(0,std::memory_order_relaxed);
  auto disjoint=[&](const CPoly&a,const CPoly&b){for(int i=0;i<a.n;++i)for(int j=0;j<b.n;++j)if(a.k[i]==b.k[j])return false;return true;};
  auto test=[&](int ti,int tj){
   int pi=owner[ti],pj=owner[tj];if(pi==pj)return;if(pi>pj)std::swap(pi,pj);
   if(flags[pi].load(std::memory_order_relaxed)&&flags[pj].load(std::memory_order_relaxed))return;
   if(!disjoint(cpolys[pi],cpolys[pj]))return;
   if(triTri(tree.tris[ti],tree.tris[tj])){flags[pi].store(1,std::memory_order_relaxed);flags[pj].store(1,std::memory_order_relaxed);}
  };
  if(!tree.nodes.empty()){
   // Each unordered node pair is visited once: self(n)=self(l)+self(r)+cross(l,r).
   tbb::task_group tg;
   std::function<void(int,int,int)> cross_=[&](int a,int b,int depth){
    const auto&A=tree.nodes[a];const auto&B=tree.nodes[b];if(!A.box.overlaps(B.box))return;
    if(A.count&&B.count){for(int i=A.start;i<A.start+A.count;++i)for(int j=B.start;j<B.start+B.count;++j)test(tree.order[i],tree.order[j]);return;}
    const bool splitA=!A.count&&(B.count||(A.box.hi[0]-A.box.lo[0])+(A.box.hi[1]-A.box.lo[1])+(A.box.hi[2]-A.box.lo[2])>=(B.box.hi[0]-B.box.lo[0])+(B.box.hi[1]-B.box.lo[1])+(B.box.hi[2]-B.box.lo[2]));
    const int c0=splitA?a+1:b+1,c1=splitA?A.right:B.right,other=splitA?b:a;
    if(depth<12){tg.run([&,c0,other,depth]{cross_(c0,other,depth+1);});tg.run([&,c1,other,depth]{cross_(c1,other,depth+1);});}
    else{cross_(c0,other,depth+1);cross_(c1,other,depth+1);}
   };
   std::function<void(int,int)> self=[&](int a,int depth){
    const auto&A=tree.nodes[a];
    if(A.count){for(int i=A.start;i<A.start+A.count;++i)for(int j=i+1;j<A.start+A.count;++j)test(tree.order[i],tree.order[j]);return;}
    const int l=a+1,r=A.right;
    if(depth<12){tg.run([&,l,depth]{self(l,depth+1);});tg.run([&,r,depth]{self(r,depth+1);});}
    else{self(l,depth+1);self(r,depth+1);}
    cross_(l,r,depth);
   };
   self(0,0);tg.wait();
  }
  hitOut.assign(nv,0);bool any=false;
  for(size_t i=0;i<cpolys.size();++i)if(flags[i].load(std::memory_order_relaxed)){any=true;for(int k=0;k<cpolys[i].no;++k)hitOut[cpolys[i].owner[k]]=1;}
  return any;
 };
 prof.mark("deform");
 for(int attempt=0;attempt<6;++attempt){
  std::vector<uint8_t> hit;if(!collide(hit))break;
  std::vector<uint8_t> src(nv,0);bool anySrc=false;
  for(size_t v=0;v<nv;++v)if(hit[v]&&plan.rim[v]>=0){src[plan.rim[v]]=1;anySrc=true;}
  if(!anySrc)break;
  if(attempt==5){for(size_t r=0;r<nv;++r)if(src[r]&&isRim[r])rimA[r]=0.;}
  else{
   std::vector<F3> pts;for(size_t r=0;r<nv;++r)if(src[r])pts.push_back(co[r]);KD near(std::move(pts));const double radius=bend*.6;
   pfor(rimVerts.size(),[&](size_t i){const int r=rimVerts[i];const double d=near.nearest(co[r]).second;if(d<radius)rimA[r]=rimA[r]*(.45+.55*smooth01(d/radius));},128);
  }
  for(size_t i=0;i<nv;++i)amps[i]=isRim[i]?rimA[i]:1.;
  def=pd_peeling::DeformCurl(dp,smoothed,plan,amps,localDepth,s);applyFloor();
 }
 prof.mark("collide");
 // Build.
 std::vector<int32_t> floorV(nv,-1),underV(nv,-1);
 for(int v:offsetVerts){floorV[v]=addVert(toF(def.floor[v]),prov[v],origV[v],vno[v]);}
 for(size_t v=0;v<nv;++v)if(bandV[v])underV[v]=addVert(toF(def.back[v]),prov[v],origV[v],vno[v]);
 auto duplicate=[&](int fid,const std::vector<int32_t>&map,bool reverse,int mat,int mark){
  const Face F=faces[fid];int vs[4];float w[4][3];
  for(int k=0;k<F.n;++k){const int j=reverse?F.n-1-k:k;vs[k]=map[F.v[j]];std::memcpy(w[k],F.w[j],sizeof(float)*3);}
  const int id=addFace(F.n,vs,F,w);Face&q=faces[id];q.stamp=kStamp;q.smooth=1;q.paper=uint8_t(mark);if(mat>=0)q.mat=mat;
 };
 // Floors under cracks read as shadow lines, not as exposed substrate.
 std::vector<uint8_t> crackFloor(nf,0);
 if(c.damage<.999&&c.crack_mat>=0)pfor(nf,[&](size_t i){
  if(!hole[i])return;const Face&f=faces[flist[i]];const F3 m=divf(co[f.v[0]]+co[f.v[1]]+co[f.v[2]],3.);
  const double base=c.use_drawing&&drawing?field.fromDrawing(m,drawing->value(m)):field.procedural(m);
  Field::CrazeInfo info;field.craze(m,base,&info);crackFloor[i]=info.line;
 },256);
 for(size_t i=0;i<nf;++i)if(insideM[i])duplicate(flist[i],floorV,false,crackFloor[i]?c.crack_mat:c.floor_mat,(c.mark_paper&&hole[i])?1:0);
 for(size_t i=0;i<nf;++i)if(band[i])duplicate(flist[i],underV,true,c.back_mat,0);
 auto interpQuad=[&](int fid,const int vs[4],float out[4][3]){
  // copy_from_face_interp: project on the source face, mean value weights, compose.
  const Face&F=faces[fid];F3 n=F.no;if(length(n)==0)n={0,0,1};
  const F3 t=normalized(orthogonal(n));const F3 b=cross(n,t);
  std::array<double,2> tp[3];for(int k=0;k<3;++k)tp[k]={dot(co[F.v[k]],t),dot(co[F.v[k]],b)};
  for(int q=0;q<4;++q){const std::array<double,2> pt{dot(co[vs[q]],t),dot(co[vs[q]],b)};double mv[3];meanValue(tp,pt,mv);
   for(int x=0;x<3;++x){double acc=0;for(int k=0;k<3;++k)acc+=mv[k]*F.w[k][x];out[q][x]=float(acc);}}
 };
 for(int e:outer){
  const EdgeRec r=edges[e];int fid=-1;for(int i=0;i<2;++i)if(band[findex[r.f[i]]])fid=r.f[i];
  if(fid<0)throw std::runtime_error("Peeling: root edge without a bent face; result cancelled");
  int a,b;loopOf(fid,e,a,b);const int vs[4]={floorV[b],floorV[a],underV[a],underV[b]};float w[4][3];interpQuad(fid,vs,w);
  const Face F=faces[fid];const int id=addFace(4,vs,F,w);Face&q=faces[id];q.stamp=kStamp;q.smooth=1;q.mat=c.bridge_mat;q.paper=0;
 }
 for(const Lip&l:lips){
  const int vs[4]={l.b,l.a,underV[l.a],underV[l.b]};float w[4][3];interpQuad(l.face,vs,w);
  const Face F=faces[l.face];const int id=addFace(4,vs,F,w);Face&q=faces[id];q.stamp=kStamp;q.smooth=1;q.paper=0;if(c.back_mat>=0)q.mat=c.back_mat;
  const int e=table.get(key(l.a,l.b));if(e>=0&&edges[e].alive)edges[e].sharp=true;
 }
 auto originalSmooth=[](const Face&f){return f.smooth!=0;};
 // Bent skin turns smooth shaded: a flat-shaded crease it crosses stays visibly sharp.
 if(c.wrap)for(size_t e=0;e<edges.size();++e){
  EdgeRec&r=edges[e];if(!r.alive||r.nf!=2||size_t(r.f[0])>=findex.size()||size_t(r.f[1])>=findex.size())continue;
  const int fa=findex[r.f[0]],fb=findex[r.f[1]];if(fa<0||fb<0||hole[fa]||hole[fb]||!(band[fa]||band[fb]))continue;
  const Face&x=faces[r.f[0]];const Face&y=faces[r.f[1]];if(x.smooth&&y.smooth)continue;
  if(std::acos(std::clamp(dot(x.no,y.no),-1.,1.))>.65)r.sharp=true;
 }
 // The substrate keeps a crisp edge under a flat-shaded crease too.
 for(int e:creaseEdges){
  const int a=edges[e].a,b=edges[e].b;if(floorV[a]<0||floorV[b]<0||edges[e].nf<2)continue;
  const Face&x=faces[edges[e].f[0]];const Face&y=faces[edges[e].f[1]];
  if(originalSmooth(x)&&originalSmooth(y))continue;
  const int fe=table.get(key(floorV[a],floorV[b]));if(fe>=0&&edges[fe].alive)edges[fe].sharp=true;
 }
 for(size_t v=0;v<nv;++v)if(topV[v]||(!reliefH.empty()&&reliefH[v]!=0.f&&eligibleSkin[v]))co[v]=toF(def.top[v]);
 // Moved skin is bent skin: stamped, so it keeps its triangles and smooth shading.
 if(!reliefH.empty())for(size_t i=0;i<nf;++i){if(hole[i]||band[i])continue;Face&q=faces[flist[i]];for(int k=0;k<q.n;++k)if(reliefH[q.v[k]]!=0.f){q.stamp=kStamp;q.smooth=1;q.paper=0;break;}}
 for(size_t i=0;i<nf;++i)if(band[i]){Face&q=faces[flist[i]];q.stamp=kStamp;q.smooth=1;q.paper=0;}
 for(size_t i=0;i<nf;++i)if(hole[i])removeFace(flist[i]);
 // Remove wire edges and loose vertices.
 std::vector<uint8_t> used(co.size(),0);
 for(auto&r:edges){if(r.alive&&r.nf==0)r.alive=false;if(r.alive){used[r.a]=1;used[r.b]=1;}}
 for(size_t v=0;v<co.size();++v)vAlive[v]=used[v];
 normalUpdate();
 // Closed lip check.
 for(const auto&r:edges)if(r.alive&&r.nf==1&&((size_t(r.a)<nv&&plan.border[r.a]&&vAlive[r.a])||(size_t(r.b)<nv&&plan.border[r.b]&&vAlive[r.b])))throw std::runtime_error("Peeling produced an open lip; result cancelled");
 prof.mark("build");
 size_t nb=0;for(size_t i=0;i<nf;++i)if(band[i])++nb;size_t ni=0;for(size_t i=0;i<nf;++i)if(insideM[i])++ni;
 stats.faces=int(nb+ni);stats.limited=limited;stats.contour_edges=int(boundary.size());
 return stats;
}

// Join refined triangles of one original polygon again wherever nothing was peeled.
void Engine::dissolve(){
 loops.clear();faceHead.assign(faces.size(),-1);
 std::vector<int32_t> uf(faces.size());std::iota(uf.begin(),uf.end(),0);std::vector<int32_t> fsize(faces.size(),0);
 auto find=[&](int x){while(uf[x]!=x){uf[x]=uf[uf[x]];x=uf[x];}return x;};
 std::vector<std::array<int32_t,2>> edgeLoops(edges.size(),std::array<int32_t,2>{-1,-1});
 for(size_t i=0;i<faces.size();++i){
  const Face&f=faces[i];if(!f.alive)continue;const int base=int(loops.size());faceHead[i]=base;fsize[i]=f.n;
  for(int k=0;k<f.n;++k){loops.push_back({f.v[k],base+(k+1)%f.n,base+(k+f.n-1)%f.n,f.e[k],int(i),k,1});
   auto&el=edgeLoops[f.e[k]];if(el[0]<0)el[0]=base+k;else el[1]=base+k;}
 }
 if(!in.adaptive){hasDissolved=true;return;}
 // Vertex -> loops, for the pinch test.
 std::vector<int32_t> vStart(co.size()+1,0);for(const Loop&l:loops)++vStart[l.v+1];for(size_t i=0;i<co.size();++i)vStart[i+1]+=vStart[i];
 std::vector<int32_t> vLoops(loops.size());{auto cur=vStart;for(size_t i=0;i<loops.size();++i)vLoops[cur[loops[i].v]++]=int32_t(i);}
 auto otherLoop=[&](int l){const auto&el=edgeLoops[loops[l].edge];return el[0]==l?el[1]:el[0];};
 for(size_t e=0;e<edges.size();++e){
  EdgeRec&r=edges[e];if(!r.alive||r.nf!=2)continue;
  const Face&fa=faces[r.f[0]];const Face&fb=faces[r.f[1]];
  if(fa.stamp==kStamp||fb.stamp==kStamp||fa.poly!=fb.poly)continue;
  const int la=edgeLoops[e][0],lb=edgeLoops[e][1];if(la<0||lb<0||!loops[la].alive||!loops[lb].alive)continue;
  int a=find(loops[la].face),b=find(loops[lb].face);if(a==b)continue;
  if(fsize[a]>fsize[b])std::swap(a,b); // a is the smaller cycle
  // Runs of edges shared with b, walking a's cycle.
  const int head=faceHead[a];int runs=0,shared=0,total=0,first=-1;bool prevShared=false;int l=head;
  auto isShared=[&](int q){const int ed=loops[q].edge;if(edges[ed].nf!=2)return false;const int o=otherLoop(q);return o>=0&&find(loops[o].face)==b;};
  {const int tail=loops[head].prev;prevShared=isShared(tail);}
  do{const bool s=isShared(l);if(s){++shared;if(!prevShared){++runs;first=l;}}prevShared=s;++total;l=loops[l].next;}while(l!=head);
  if(runs!=1||shared==total||shared>=fsize[b])continue;
  // Pinch: no vertex outside the shared chain may already be on the other cycle.
  std::vector<int32_t> chain;l=first;while(isShared(l)){chain.push_back(l);l=loops[l].next;}
  const int endV=loops[l].v,startV=loops[first].v;bool pinch=false;
  for(int q=l;q!=first&&!pinch;q=loops[q].next){const int v=loops[q].v;if(v==endV||v==startV)continue;
   for(int i=vStart[v];i<vStart[v+1];++i){const Loop&x=loops[vLoops[i]];if(x.alive&&find(x.face)==b){pinch=true;break;}}}
  if(pinch)continue;
  const int s=first,t=chain.back(),os=otherLoop(s),ot=otherLoop(t);
  const int ps=loops[s].prev,nt=loops[t].next,nos=loops[os].next,pot=loops[ot].prev;
  loops[ps].next=nos;loops[nos].prev=ps;loops[pot].next=nt;loops[nt].prev=pot;
  for(int q:chain){const int o=otherLoop(q);loops[q].alive=0;loops[o].alive=0;edges[loops[q].edge].alive=false;}
  uf[a]=b;fsize[b]=fsize[b]+fsize[a]-2*int(chain.size());faceHead[b]=nos;faceHead[a]=-1;
 }
 // Straight midpoints left by refinement.
 std::vector<int32_t> deg(co.size(),0);std::vector<uint8_t> blocked(co.size(),0);
 for(const auto&r:edges)if(r.alive&&r.nf>0){++deg[r.a];++deg[r.b];}
 for(const Loop&l:loops)if(l.alive&&faces[l.face].stamp==kStamp)blocked[l.v]=1;
 std::vector<int32_t> candidates;for(size_t v=0;v<co.size();++v)if(vAlive[v]&&!origV[v]&&deg[v]==2&&!blocked[v])candidates.push_back(int32_t(v));
 std::vector<int32_t> vl;
 for(int v:candidates){
  vl.clear();for(int i=vStart[v];i<vStart[v+1];++i)if(loops[vLoops[i]].alive)vl.push_back(vLoops[i]);
  if(vl.empty()||vl.size()>2)continue;
  const int l0=vl[0];const int n1=loops[loops[l0].next].v,n0=loops[loops[l0].prev].v;
  const F3 da=co[v]-co[n0],db=co[n1]-co[v];
  if(!(length(da)>1e-12&&length(db)>1e-12)||dot(normalized(da),normalized(db))<=.99999)continue;
  bool ok=true;for(int l:vl){const int root=find(loops[l].face);if(fsize[root]<=3)ok=false;}
  if(!ok)continue;
  const int existing=table.get(key(n0,n1));if(existing>=0&&edges[existing].alive)continue;
  const EdgeRec prevEdge=edges[loops[loops[l0].prev].edge];
  const int ne=getEdge(n0,n1);edges[ne].orig=prevEdge.orig;edges[ne].sharp=prevEdge.sharp;edges[ne].nf=int(vl.size());
  for(int l:vl){
   const int p=loops[l].prev,nx=loops[l].next;edges[loops[p].edge].alive=false;edges[loops[l].edge].alive=false;
   loops[p].edge=ne;loops[p].next=nx;loops[nx].prev=p;loops[l].alive=0;
   const int root=find(loops[l].face);--fsize[root];if(faceHead[root]==l)faceHead[root]=p;
  }
 }
 // Publish roots: heads must point at live loops.
 for(size_t i=0;i<faces.size();++i)if(faceHead[i]>=0&&find(int(i))!=int(i))faceHead[i]=-1;
 hasDissolved=true;
}

void Engine::emit(Output&out){
 std::vector<int32_t> vmap(co.size(),-1),emap(edges.size(),-1);
 out.poly_offsets.push_back(0);
 for(size_t i=0;i<faces.size();++i){
  int head=faceHead[i];if(head<0||!faces[i].alive)continue;
  if(!loops[head].alive)throw std::runtime_error("Peeling: lost a polygon while restoring faces");
  const Face&rep=faces[i];int l=head;int guard=0;
  do{
   const Loop&L=loops[l];int&nvx=vmap[L.v];
   if(nvx<0){nvx=int32_t(out.points.size()/3);out.points.insert(out.points.end(),co[L.v].begin(),co[L.v].end());
    for(int k=0;k<3;++k){out.vert_src.push_back(prov[L.v].s[k]);out.vert_w.push_back(prov[L.v].w[k]);}}
   int&ne=emap[L.edge];
   if(ne<0){ne=int32_t(out.edges.size()/2);const EdgeRec&r=edges[L.edge];out.edges.push_back(-1);out.edges.push_back(-1);out.edge_src.push_back(r.orig);out.edge_sharp.push_back(r.sharp?1:0);}
   out.corner_vert.push_back(nvx);out.corner_edge.push_back(ne);
   const Face&src=faces[L.face];const auto&b=baseTri[src.base];
   for(int k=0;k<3;++k){out.corner_src.push_back(b[k]);out.corner_w.push_back(src.w[L.corner][k]);}
   l=L.next;if(++guard>100000000)throw std::runtime_error("Peeling: broken polygon cycle");
  }while(l!=head);
  out.poly_offsets.push_back(int32_t(out.corner_vert.size()));
  out.face_src.push_back(rep.poly);out.material.push_back(rep.mat);out.smooth.push_back(rep.smooth);out.stamp.push_back(rep.stamp);
 }
 // Edge vertices, now that every vertex has its final index.
 for(size_t e=0;e<edges.size();++e)if(emap[e]>=0){out.edges[2*emap[e]]=vmap[edges[e].a];out.edges[2*emap[e]+1]=vmap[edges[e].b];}
 for(size_t e=0;e<out.edges.size();++e)if(out.edges[e]<0)throw std::runtime_error("Peeling: edge lost a vertex");
}

std::unique_ptr<Drawing> makeDrawing(const Input&in,const Engine&engine,double size){
 auto d=std::make_unique<Drawing>();d->gate=in.main.wrap?kWrapGate:.35;d->radius=in.drawing.radius;d->snap=in.drawing.snap;d->size=size;
 std::vector<std::array<F3,3>> tris;tris.reserve(in.tri_poly.size());d->triPoly=in.tri_poly;
 for(size_t t=0;t<in.tri_poly.size();++t){const int*c=&in.tri_corners[3*t];tris.push_back({engine.co[in.corner_vert[c[0]]],engine.co[in.corner_vert[c[1]]],engine.co[in.corner_vert[c[2]]]});}
 d->surface=std::make_unique<TriBVH>(std::move(tris));d->guided=in.guided_face_normals;
 const auto&off=in.drawing.offsets;
 for(size_t p=0;p+1<off.size();++p){
  const int a=off[p],b=off[p+1],n=b-a;if(n<2)continue;
  using D3=std::array<double,3>;std::vector<D3> path(n);for(int i=0;i<n;++i)path[i]={in.drawing.points[3*(a+i)],in.drawing.points[3*(a+i)+1],in.drawing.points[3*(a+i)+2]};
  auto norm=[](const D3&x,const D3&y){return std::sqrt((x[0]-y[0])*(x[0]-y[0])+(x[1]-y[1])*(x[1]-y[1])+(x[2]-y[2])*(x[2]-y[2]));};
  const bool closed=norm(path.front(),path.back())<1e-6,area=in.drawing.areas;
  if(area&&(n<3||(!closed&&!in.drawing.close_strokes)))continue;
  std::vector<D3> raw=path;if(closed&&area)raw.pop_back();
  if(int(raw.size())<(area?3:2))continue;
  const double spacing=std::max(size*.04,1e-9);std::vector<D3> dense;std::vector<D3> pts=raw;if(area)pts.push_back(raw[0]);
  for(size_t i=0;i+1<pts.size();++i){const int count=std::max(1,std::min(256,int(std::ceil(norm(pts[i+1],pts[i])/spacing))));
   for(int j=0;j<count;++j){const double t=double(j)/count;dense.push_back({pts[i][0]+(pts[i+1][0]-pts[i][0])*t,pts[i][1]+(pts[i+1][1]-pts[i][1])*t,pts[i][2]+(pts[i+1][2]-pts[i][2])*t});}}
  if(!area)dense.push_back(pts.back());
  std::vector<TriBVH::Hit> hits(dense.size());
  pfor(dense.size(),[&](size_t i){hits[i]=d->nearest(F3{float(dense[i][0]),float(dense[i][1]),float(dense[i][2])});},64);
  bool bad=false;for(const auto&h:hits)if(h.tri<0||h.distance>d->snap)bad=true;if(bad)continue;
  F3 normal{0,0,0};for(const auto&h:hits)normal=normal+h.no;if(length(normal)<1e-8)continue;normal=normalized(normal);
  for(const auto&h:hits)if(dot(normal,h.no)<d->gate)bad=true;if(bad)continue;
  F3 center{0,0,0};for(const auto&h:hits)center=center+h.co;center=divf(center,double(hits.size()));
  Region r;r.center=center;r.n=normal;r.u=normalized(orthogonal(normal));r.v=cross(normal,r.u);r.area=area;
  double spread=0;for(const auto&h:hits){const F3 x=h.co-center;r.poly.push_back({dot(x,r.u),dot(x,r.v)});r.line.push_back(h.co);spread=std::max(spread,std::fabs(dot(x,normal)));}
  r.slab=spread+d->snap;d->regions.push_back(std::move(r));
 }
 return d;
}
} // namespace

double Noise(float x,float y,float z){return N(F3{x,y,z});}
void Voronoi(float x,float y,float z,float da[4],float pa[12]){voronoi(F3{x,y,z},da,pa);}
double HashUnit(const std::vector<int64_t>&v){return double(blake2b8(reinterpret_cast<const uint8_t*>(v.data()),v.size()*8))/18446744073709551616.0;}

// What Damage Factor changes before a pass. At 1 this leaves every value as it was.
PassParams derived(PassParams c){
 const double f=std::clamp(c.damage,0.,1.);c.damage=f;
 const double fullReach=c.bend*(.8+1.4*c.tongues);
 if(f>=.999){c.contour_step=c.step;c.bend_step=c.step;c.reach=fullReach;c.crack_width=0;return c;}
 const double k=10.+(1.-10.)*smooth01((f-.6)/.38);
 c.contour_step=c.step/k;
 const double b=smooth01((f-.5)/.5);
 c.bend_step=std::exp(std::log(c.contour_step*5.)*(1.-b)+std::log(c.step)*b);
 const double plateReach=std::min(fullReach,.06*c.size);
 c.reach=plateReach+(fullReach-plateReach)*b;
 c.crack_width=std::max(c.size*.004,.7*c.contour_step);
 c.relief_step=std::max(c.size*.02,c.contour_step);
 const double g=smooth01((f-.35)/.6);
 c.tears*=g;c.small_amount*=g;
 return c;
}

Output Generate(const Input&in){
 Output out;
 Engine eng(in);eng.load();
 for(const F3&p:eng.co)for(float x:p)if(!std::isfinite(x))throw std::invalid_argument("Peeling: non-finite coordinates");
 if(in.has_drawing){
  eng.drawing=makeDrawing(in,eng,in.main.size);
  if(eng.drawing->regions.empty()){out.drawing_empty=true;return out;}
 }
 const int componentsBefore=eng.components();
 int boundaryBefore=0,nonManifoldBefore=0;for(const auto&e:eng.edges)if(e.alive){if(e.nf==1)++boundaryBefore;if(e.nf>2||(e.nf==0))++nonManifoldBefore;}
 const PassParams mainPass=derived(in.main);
 // A crack network needs far more edges per area than open patches do.
 eng.budget=int(double(in.budget)*(1.+3.*(1.-smooth01((mainPass.damage-.3)/.6))));
 const PassStats main=eng.peelPass(mainPass);
 out.limited=main.limited;
 if(!main.faces)return out;
 out.contour_edges=main.contour_edges;out.small_patches=main.small_patches;out.tears=main.tears;
 if(in.wallpaper){
  eng.triangulateQuads();eng.normalUpdate();
  const PassParams paperPass=derived(in.paper);
  const PassStats paper=eng.peelPass(paperPass);
  out.limited=out.limited||paper.limited;out.paper_contour_edges=paper.contour_edges;out.has_paper_stats=true;
 }
 eng.normalUpdate();
 if(eng.components()!=componentsBefore)throw std::runtime_error("Peeling would create a disconnected fragment; result cancelled");
 int boundaryAfter=0,nonManifoldAfter=0;bool openLip=false;
 for(const auto&e:eng.edges)if(e.alive){
  if(e.nf==1){++boundaryAfter;if(eng.faces[e.f[0]].stamp==kStamp)openLip=true;}
  if(e.nf>2||e.nf==0)++nonManifoldAfter;
 }
 if(boundaryAfter!=boundaryBefore&&openLip)throw std::runtime_error("Peeling produced an open lip; result cancelled");
 if(nonManifoldAfter>nonManifoldBefore)throw std::runtime_error("Peeling produced a non-manifold edge; result cancelled");
 int count=0;for(const Face&f:eng.faces)if(f.alive&&f.stamp==kStamp)++count;
 Profile prof;
 eng.dissolve();prof.mark("dissolve");eng.emit(out);prof.mark("emit");
 out.faces=count;
 return out;
}

std::vector<float> RestoreNormals(const std::vector<float>&oldPoints,const std::vector<int32_t>&oldTris,const std::vector<float>&oldNormals,
 const std::vector<float>&points,const std::vector<int32_t>&polyOffsets,const std::vector<int32_t>&cornerVert,const std::vector<int32_t>&stamp,const std::vector<float>&current){
 const size_t nt=oldTris.size()/3;std::vector<std::array<F3,3>> tris(nt);
 auto P=[&](const std::vector<float>&a,int i){return F3{a[3*i],a[3*i+1],a[3*i+2]};};
 for(size_t t=0;t<nt;++t)tris[t]={P(oldPoints,oldTris[3*t]),P(oldPoints,oldTris[3*t+1]),P(oldPoints,oldTris[3*t+2])};
 TriBVH tree(tris);std::vector<float> values=current;
 const size_t np=polyOffsets.size()-1;
 pfor(np,[&](size_t f){
  if(stamp[f]==kStamp)return;const int a=polyOffsets[f],b=polyOffsets[f+1];const int n=b-a;
  F3 center{0,0,0};for(int i=a;i<b;++i)center=center+P(points,cornerVert[i]);center=divf(center,double(n));
  const auto hit=tree.nearest(center);if(hit.tri<0)return;
  const auto&src=tris[hit.tri];const F3 ns[3]={P(oldNormals,3*hit.tri),P(oldNormals,3*hit.tri+1),P(oldNormals,3*hit.tri+2)};
  const F3 noSrc=triNormal(src[0],src[1],src[2]);const F3 noTar=triNormal(ns[0],ns[1],ns[2]);
  const F3 t=normalized(orthogonal(length(noSrc)>0?noSrc:F3{0,0,1}));const F3 bt=cross(length(noSrc)>0?noSrc:F3{0,0,1},t);
  std::array<double,2> xy[3];for(int k=0;k<3;++k)xy[k]={dot(src[k],t),dot(src[k],bt)};
  const double den=(xy[1][1]-xy[2][1])*(xy[0][0]-xy[2][0])+(xy[2][0]-xy[1][0])*(xy[0][1]-xy[2][1]);
  const double areaSrc=std::sqrt(std::fabs(den)*.5);
  const double areaTar=std::sqrt(.5*length(cross(ns[1]-ns[0],ns[2]-ns[0])));
  for(int i=a;i<b;++i){
   const F3 q=P(points,cornerVert[i]);const double x=dot(q,t),y=dot(q,bt),z=dot(q-src[0],length(noSrc)>0?noSrc:F3{0,0,1});
   double w0=1./3.,w1=1./3.;if(std::fabs(den)>1e-30){w0=((xy[1][1]-xy[2][1])*(x-xy[2][0])+(xy[2][0]-xy[1][0])*(y-xy[2][1]))/den;w1=((xy[2][1]-xy[0][1])*(x-xy[2][0])+(xy[0][0]-xy[2][0])*(y-xy[2][1]))/den;}
   const double w2=1.-w0-w1;double r[3];const double off=areaSrc>1e-30?z/areaSrc*areaTar:0.;
   for(int k=0;k<3;++k)r[k]=w0*ns[0][k]+w1*ns[1][k]+w2*ns[2][k]+noTar[k]*off;
   const double l=std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]);
   if(l>1e-8)for(int k=0;k<3;++k)values[3*i+k]=float(r[k]/l);
  }
 },64);
 return values;
}
}
