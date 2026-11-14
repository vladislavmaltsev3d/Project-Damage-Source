// общий вход расчёта отслоения покрытия
// SPDX-License-Identifier: GPL-3.0-or-later
#include "peeling.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <tbb/parallel_for.h>

namespace pd_peeling {
static V add(V a,const V&b){for(int i=0;i<3;++i)a[i]+=b[i];return a;}
static V sub(V a,const V&b){for(int i=0;i<3;++i)a[i]-=b[i];return a;}
static V mul(V a,double s){for(auto&v:a)v*=s;return a;}
static double dot(const V&a,const V&b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static double len(const V&a){return std::sqrt(dot(a,a));}
static constexpr double inf=std::numeric_limits<double>::infinity();
static void validate(const std::vector<V>&p){for(const auto&v:p)for(double x:v)if(!std::isfinite(x))throw std::invalid_argument("Peeling: non-finite coordinates");}
struct Tree {
 struct Node {int32_t point,left=-1,right=-1;int axis=0;};
 const std::vector<V>&p;std::vector<Node> nodes;std::vector<int32_t> order;int root=-1;
 explicit Tree(const std::vector<V>&v):p(v),order(v.size()){
  std::iota(order.begin(),order.end(),0);nodes.reserve(v.size());root=build(0,order.size(),0);
 }
 int build(size_t a,size_t b,int axis){
  if(a==b)return -1;size_t mid=a+(b-a)/2;
  std::nth_element(order.begin()+a,order.begin()+mid,order.begin()+b,[&](int i,int j){return p[i][axis]<p[j][axis]||(p[i][axis]==p[j][axis]&&i<j);});
  int id=(int)nodes.size();nodes.push_back({order[mid],-1,-1,axis});
  int l=build(a,mid,(axis+1)%3),r=build(mid+1,b,(axis+1)%3);nodes[id].left=l;nodes[id].right=r;return id;
 }
 void query(int node,const V&v,int&best,double&d2)const{
  if(node<0)return;const auto&n=nodes[node];V d=sub(v,p[n.point]);double q=dot(d,d);
  if(q<d2||(q==d2&&(best<0||n.point<best))){d2=q;best=n.point;}
  double dx=d[n.axis];int a=dx<0?n.left:n.right,b=dx<0?n.right:n.left;
  query(a,v,best,d2);if(dx*dx<=d2)query(b,v,best,d2);
 }
 std::pair<int,double> nearest(const V&v)const{int i=-1;double d=inf;query(root,v,i,d);return {i,std::sqrt(d)};}
};

std::vector<int32_t> RefineEdges(const std::vector<V>&p,const std::vector<V>&n,
 const std::vector<Edge>&edges,const std::vector<int32_t>&ok,const std::vector<V>&seeds,
 const std::vector<Patch>&patches,double step,double bend,double probe,bool adaptive,bool micro){
 validate(p);if(n.size()!=p.size()||ok.size()!=edges.size()||!(step>0))throw std::invalid_argument("Peeling: invalid refinement arrays");
 std::vector<V> centers;for(const auto&a:patches)centers.push_back({a[0],a[1],a[2]});
 Tree seedTree(seeds),patchTree(centers);std::vector<int32_t> selected;
 for(size_t i=0;i<edges.size();++i){
  if(!ok[i])continue;auto e=edges[i];if(e[0]<0||e[1]<0||size_t(e[0])>=p.size()||size_t(e[1])>=p.size())throw std::invalid_argument("Peeling: invalid edge index");
  V mid=mul(add(p[e[0]],p[e[1]]),.5);double length=len(sub(p[e[1]],p[e[0]]));
  if(micro){
   if(patches.empty())continue;auto hit=patchTree.nearest(mid);const auto&patch=patches[hit.first];double radius=patch[6];
   if(hit.second>length*.5+radius*2.2)continue;
   V normal{patch[3],patch[4],patch[5]};if(dot(n[e[0]],normal)<.65&&dot(n[e[1]],normal)<.65)continue;
   double target=std::min(step,radius*.4);if(target<step&&length>target*1.4)selected.push_back((int32_t)i);
  }else{
   if(length<=step*1.4)continue;
   if(adaptive&&(seeds.empty()||seedTree.nearest(mid).second>length*.5+bend+probe*1.5))continue;
   selected.push_back((int32_t)i);
  }
 }
 return selected;
}

struct MeshEdge{int32_t a,b;std::vector<int32_t> faces;};
Plan PlanAttached(const std::vector<V>&p,const std::vector<Tri>&tri,
 const std::vector<int32_t>&initial,const std::vector<double>&vscale,double bend){
 validate(p);if(initial.size()!=tri.size()||vscale.size()!=p.size()||!(bend>0))throw std::invalid_argument("Peeling: invalid plan arrays");
 Plan out;out.hole=initial;out.band.assign(tri.size(),0);out.border.assign(p.size(),0);out.distance.assign(p.size(),inf);out.rimDistance.assign(p.size(),inf);out.rootDistance.assign(p.size(),inf);out.scale.assign(p.size(),1.);
 std::vector<MeshEdge> edges;std::unordered_map<uint64_t,size_t> byEdge;
 std::vector<std::vector<int32_t>> vertexFaces(p.size()),graph(p.size()),adj(tri.size());
 for(size_t f=0;f<tri.size();++f){
  for(int a:tri[f]){if(a<0||size_t(a)>=p.size())throw std::invalid_argument("Peeling: invalid triangle index");vertexFaces[a].push_back((int32_t)f);}
  for(int j=0;j<3;++j){int a=tri[f][j],b=tri[f][(j+1)%3];if(a>b)std::swap(a,b);uint64_t key=(uint64_t(uint32_t(a))<<32)|uint32_t(b);
   auto found=byEdge.find(key);size_t index;
   if(found==byEdge.end()){index=edges.size();byEdge[key]=index;edges.push_back({a,b,{}});graph[a].push_back(b);graph[b].push_back(a);}
   else index=found->second;edges[index].faces.push_back((int32_t)f);
  }
 }
 for(const auto&e:edges)if(e.faces.size()==2){adj[e.faces[0]].push_back(e.faces[1]);adj[e.faces[1]].push_back(e.faces[0]);}
 auto boundary=[&](const std::vector<int32_t>&mask){std::vector<int32_t> ids;std::vector<uint8_t> seen(p.size(),0);
  for(const auto&e:edges)if(e.faces.size()==2&&bool(mask[e.faces[0]])!=bool(mask[e.faces[1]])){for(int v:{e.a,e.b})if(!seen[v]){seen[v]=1;ids.push_back(v);}}
  return ids;
 };
 auto ids=boundary(out.hole);if(ids.empty())return out;
 std::vector<V> boundaryPoints;for(int i:ids)boundaryPoints.push_back(p[i]);Tree initialTree(boundaryPoints);
 std::vector<uint8_t> visited(tri.size(),0);
 for(size_t f=0;f<tri.size();++f){
  if(out.hole[f]||visited[f])continue;std::vector<int32_t> component,stack{(int32_t)f};visited[f]=1;bool absorb=true;
  while(!stack.empty()){
   int q=stack.back();stack.pop_back();component.push_back(q);const auto&t=tri[q];V c=mul(add(add(p[t[0]],p[t[1]]),p[t[2]]),1./3.);
   auto hit=initialTree.nearest(c);if(hit.second/std::max(1e-12,vscale[ids[hit.first]])>=bend*.6)absorb=false;
   for(int g:adj[q])if(!out.hole[g]&&!visited[g]){visited[g]=1;stack.push_back(g);}
  }
  if(absorb)for(int q:component)out.hole[q]=1;
 }
 ids=boundary(out.hole);if(ids.empty())return out;
 boundaryPoints.clear();for(int i:ids){boundaryPoints.push_back(p[i]);out.border[i]=1;}Tree tree(boundaryPoints);
 std::vector<double> travel(p.size(),inf);using Item=std::pair<double,int32_t>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
 for(int i:ids){travel[i]=0;queue.push({0,i});}
 while(!queue.empty()){
  auto [d,v]=queue.top();queue.pop();if(d!=travel[v])continue;
  for(int u:graph[v]){double next=d+len(sub(p[u],p[v]));if(next<=bend*1.8&&next<travel[u]){travel[u]=next;queue.push({next,u});}}
 }
 for(size_t i=0;i<p.size();++i){auto hit=tree.nearest(p[i]);out.scale[i]=std::max(1e-12,vscale[ids[hit.first]]);out.rimDistance[i]=hit.second/out.scale[i];if(std::isfinite(travel[i]))out.distance[i]=out.rimDistance[i];}
 for(size_t f=0;f<tri.size();++f)if(!out.hole[f])for(int v:tri[f])if(out.border[v]||out.distance[v]<bend*.55){out.band[f]=1;break;}
 for(int pass=0;pass<8;++pass){
  std::vector<int> degree(p.size(),0);for(const auto&e:edges)if(e.faces.size()==2){bool a=out.hole[e.faces[0]]||out.band[e.faces[0]],b=out.hole[e.faces[1]]||out.band[e.faces[1]];if(a!=b){++degree[e.a];++degree[e.b];}}
  bool pinch=false;for(size_t v=0;v<p.size();++v)if(degree[v]>2){pinch=true;for(int f:vertexFaces[v])if(!out.hole[f])out.band[f]=1;}
  if(!pinch)break;
 }
 std::vector<int32_t> inside=out.hole;for(size_t f=0;f<tri.size();++f)inside[f]=inside[f]||out.band[f];
 auto roots=boundary(inside);if(roots.empty()){std::fill(out.band.begin(),out.band.end(),0);return out;}
 std::vector<V> rootPoints;for(int i:roots)rootPoints.push_back(p[i]);Tree rootTree(rootPoints);
 for(size_t i=0;i<p.size();++i)out.rootDistance[i]=rootTree.nearest(p[i]).second;
 return out;
}

std::vector<double> DrawingValues(const std::vector<V>&p,const std::vector<V>&normals,
 const std::vector<std::array<double,14>>&frames,const std::vector<std::array<double,2>>&poly,
 const std::vector<int32_t>&offsets,double size,double radius){
 validate(p);if(normals.size()!=p.size()||offsets.size()!=frames.size()+1||!(size>0))throw std::invalid_argument("Peeling: invalid drawing arrays");
 if(offsets.empty()||offsets.front()!=0||offsets.back()!=int32_t(poly.size()))throw std::invalid_argument("Peeling: invalid drawing offsets");
 std::vector<double> result(p.size(),1e6);
 for(size_t i=0;i<p.size();++i)for(size_t r=0;r<frames.size();++r){
  const auto&f=frames[r];V delta=sub(p[i],{f[0],f[1],f[2]}),u{f[3],f[4],f[5]},v{f[6],f[7],f[8]},n{f[9],f[10],f[11]};
  if(std::abs(dot(delta,n))>f[12]||dot(normals[i],n)<.35)continue;
  double x=dot(delta,u),y=dot(delta,v),distance=inf;bool inside=false,area=f[13]!=0;
  int a=offsets[r],end=offsets[r+1];if(a<0||end<a||end>int(poly.size()))throw std::invalid_argument("Peeling: invalid contour offset");
  for(int j=a;j<end-(area?0:1);++j){const auto&A=poly[j];const auto&B=poly[j+1<end?j+1:a];double dx=B[0]-A[0],dy=B[1]-A[1];
   double t=std::clamp(((x-A[0])*dx+(y-A[1])*dy)/std::max(1e-20,dx*dx+dy*dy),0.,1.);
   distance=std::min(distance,std::hypot(x-A[0]-t*dx,y-A[1]-t*dy));
   if((A[1]>y)!=(B[1]>y)){double cross=A[0]+(y-A[1])*dx/dy;if(x<cross)inside=!inside;}
  }
  double value=area?(inside?-distance:distance):distance-radius;result[i]=std::min(result[i],value/size);
 }
 return result;
}

Offsets Deform(const std::vector<V>&p,const std::vector<V>&normals,const Plan&plan,
 const std::vector<double>&noise,const std::vector<double>&depth,double lift,double bend,double thickness,double variation){
 if(normals.size()!=p.size()||noise.size()!=p.size()||depth.size()!=p.size()||plan.distance.size()!=p.size()||plan.rimDistance.size()!=p.size()||plan.rootDistance.size()!=p.size()||plan.scale.size()!=p.size()||!(bend>0))throw std::invalid_argument("Peeling: invalid deformation arrays");
 Offsets out;out.top.resize(p.size());out.floor.resize(p.size());out.back.resize(p.size());
 for(size_t i=0;i<p.size();++i){
  double d=plan.distance[i],height=0;
  if(std::isfinite(d)){double t=std::max(0.,1.-d/bend);double amp=1.-variation*(1.-std::clamp(.5+.5*noise[i],0.,1.));height=lift*t*t*amp*plan.scale[i];}
  // A band's opposite-side vertices may be outside geodesic reach; use their
  // Euclidean rim distance supplied by the host in that case.
  double dist=std::isfinite(d)?d:plan.rimDistance[i],root=plan.rootDistance[i];double t=dist/std::max(1e-12,dist+root);if(!std::isfinite(t))t=1.;
  t=std::clamp(t,0.,1.);double smooth=t*t*(3.-2.*t),thick=std::min(thickness,depth[i]*.5);
  double z=(height-thick)*(1.-smooth)-(depth[i]-thick*.25)*smooth;
  out.top[i]=add(p[i],mul(normals[i],height));out.floor[i]=sub(p[i],mul(normals[i],depth[i]));out.back[i]=add(p[i],mul(normals[i],z));
 }
 return out;
}

// ===========================================================================
// Surface Peel v2: curl kernels.
//
// Why v1 looked wrong (kept above for compatibility):
//  * Deform raised the coating straight along the normal by lift*(1-d/bend)^2
//    over a FULL bend width, while the hidden underside only spanned 55% of it:
//    the outer 45% rose as solid material (a cushion), every hole got the same
//    ring-shaped swelling, and the lip never rotated or rolled.
//  * The underside blended linearly from the lip down to the floor across the
//    whole band, so the cross-section was a wedge: swollen edges, and through
//    the opening a sloped shelf (terrace) was visible between lip and floor.
//  * Per-vertex noise for the lift varied across the band as much as along
//    it, so neighbouring vertices of one flap disagreed (lumps, no tongues).
// v2 ties every lifted vertex to one rim vertex, bends the sheet as an
// elastica-like arc (angle theta = Theta * u^p from the attached root u=0 to
// the free lip u=1), keeps the skin a constant thickness and lets the hidden
// underside meet the floor only in the root zone.
// ===========================================================================
namespace {

static V tangential(const V&v,const V&n){return sub(v,mul(n,dot(v,n)));}
static V unit(const V&v,const V&fallback){double l=len(v);return l>1e-20?mul(v,1./l):fallback;}
static double smooth01(double t){t=std::clamp(t,0.,1.);return t*t*(3.-2.*t);}

struct Topology {
 std::vector<MeshEdge> edges;
 std::vector<std::vector<int32_t>> vertexFaces,graph,adj;
 Topology(const std::vector<V>&p,const std::vector<Tri>&tri):vertexFaces(p.size()),graph(p.size()),adj(tri.size()){
  // Open addressing: this table is the hottest part of planning large meshes.
  size_t cap=64;while(cap<tri.size()*4)cap<<=1;
  std::vector<uint64_t> keys(cap,~0ULL);std::vector<int32_t> vals(cap);const size_t mask=cap-1;
  edges.reserve(tri.size()*3/2+8);for(auto&a:adj)a.reserve(3);
  for(size_t f=0;f<tri.size();++f){
   for(int a:tri[f]){if(a<0||size_t(a)>=p.size())throw std::invalid_argument("Peeling: invalid triangle index");vertexFaces[a].push_back((int32_t)f);}
   for(int j=0;j<3;++j){int a=tri[f][j],b=tri[f][(j+1)%3];if(a>b)std::swap(a,b);const uint64_t key=(uint64_t(uint32_t(a))<<32)|uint32_t(b);
    uint64_t h=key^(key>>33);h*=0xff51afd7ed558ccdULL;h^=h>>33;size_t i=size_t(h)&mask;while(keys[i]!=~0ULL&&keys[i]!=key)i=(i+1)&mask;
    size_t index;
    if(keys[i]==~0ULL){index=edges.size();keys[i]=key;vals[i]=int32_t(index);edges.push_back({a,b,{}});graph[a].push_back(b);graph[b].push_back(a);}
    else index=size_t(vals[i]);edges[index].faces.push_back((int32_t)f);
   }
  }
  for(const auto&e:edges)if(e.faces.size()==2){adj[e.faces[0]].push_back(e.faces[1]);adj[e.faces[1]].push_back(e.faces[0]);}
 }
 std::vector<int32_t> boundary(size_t n,const std::vector<int32_t>&mask)const{
  std::vector<int32_t> ids;std::vector<uint8_t> seen(n,0);
  for(const auto&e:edges)if(e.faces.size()==2&&bool(mask[e.faces[0]])!=bool(mask[e.faces[1]])){for(int v:{e.a,e.b})if(!seen[v]){seen[v]=1;ids.push_back(v);}}
  std::sort(ids.begin(),ids.end());return ids;
 }
};

// Normalised height and run of an arc whose angle grows as Theta*u^p,
// integrated from the root (u=0) to u. Simpson, enough for smooth output.
struct Arc {double x,z;};
static Arc ArcAt(double Theta,double p,double u,int steps=24){
 if(u<=0)return {0,0};
 if(steps%2)++steps;double h=u/steps,x=0,z=0;
 for(int i=0;i<=steps;++i){double s=h*i,th=Theta*std::pow(s,p),w=(i==0||i==steps)?1:(i%2?4:2);x+=w*std::cos(th);z+=w*std::sin(th);}
 return {x*h/3.,z*h/3.};
}
// Tip height per unit width as a function of Theta, monotone part only.
struct ThetaTable {
 std::vector<double> theta,height;
 ThetaTable(double p,double cap){
  const int n=96;for(int i=0;i<=n;++i){double th=cap*i/n,z=ArcAt(th,p,1.).z;if(!height.empty()&&z<=height.back())break;theta.push_back(th);height.push_back(z);}
 }
 double Solve(double ratio)const{
  if(ratio<=0)return 0;if(ratio>=height.back())return theta.back();
  size_t i=size_t(std::upper_bound(height.begin(),height.end(),ratio)-height.begin());
  double t=(ratio-height[i-1])/std::max(1e-20,height[i]-height[i-1]);return theta[i-1]+(theta[i]-theta[i-1])*t;
 }
};
}

CurlPlan PlanCurl(const std::vector<V>&p,const std::vector<V>&normals,const std::vector<Tri>&tri,
 const std::vector<int32_t>&initial,const std::vector<int32_t>&locked,const std::vector<double>&vscale,
 const std::vector<double>&rimWidth,const std::vector<double>&guard,double bend,const CurlOptions*options){
 validate(p);
 const double absorbLimit=options?options->absorb:.6;
 if(initial.size()!=tri.size()||locked.size()!=tri.size()||vscale.size()!=p.size()||normals.size()!=p.size()||
    rimWidth.size()!=p.size()||guard.size()!=p.size()||!(bend>0))throw std::invalid_argument("Peeling: invalid curl plan arrays");
 const size_t n=p.size();
 CurlPlan out;out.hole=initial;out.band.assign(tri.size(),0);out.border.assign(n,0);out.rim.assign(n,-1);
 out.distance.assign(n,inf);out.rimDistance.assign(n,inf);out.rootDistance.assign(n,inf);out.scale.assign(n,1.);
 out.width.assign(n,0.);out.direction.assign(n,V{0,0,0});
 for(auto&h:out.hole)h=h?1:0;
 Topology topo(p,tri);
 std::vector<uint8_t> pinned(n,0);
 for(size_t f=0;f<tri.size();++f)if(locked[f]){out.hole[f]=0;for(int v:tri[f])pinned[v]=1;}
 auto ids=topo.boundary(n,out.hole);if(ids.empty())return out;
 // Islands of coating narrower than the curl cannot be rooted: absorb them
 // (never if they contain protected faces: a hole must not reach those).
 {
  std::vector<V> pts;for(int i:ids)pts.push_back(p[i]);Tree tree(pts);
  std::vector<uint8_t> visited(tri.size(),0);
  for(size_t f=0;f<tri.size();++f){
   if(out.hole[f]||visited[f])continue;std::vector<int32_t> component,stack{(int32_t)f};visited[f]=1;bool absorb=true;
   while(!stack.empty()){
    int q=stack.back();stack.pop_back();component.push_back(q);const auto&t=tri[q];
    if(locked[q])absorb=false;
    V c=mul(add(add(p[t[0]],p[t[1]]),p[t[2]]),1./3.);auto hit=tree.nearest(c);
    if(hit.second/std::max(1e-12,vscale[ids[hit.first]])>=bend*absorbLimit)absorb=false;
    for(int g:topo.adj[q])if(!out.hole[g]&&!visited[g]){visited[g]=1;stack.push_back(g);}
   }
   if(absorb)for(int q:component)out.hole[q]=1;
  }
 }
 ids=topo.boundary(n,out.hole);if(ids.empty())return out;
 for(int i:ids)out.border[i]=1;
 // Small openings: a rim around a hole narrower than the bend cannot lift a
 // full flap (v1 raised the same ring around every pore). Each opening's
 // inradius (Dijkstra through its faces) scales width and lift of its rim.
 std::vector<double> vs(vscale.begin(),vscale.end());
 {
  std::vector<std::vector<int32_t>> holeGraph(n);
  for(const auto&e:topo.edges){bool h=false;for(int f:e.faces)if(out.hole[f])h=true;if(h){holeGraph[e.a].push_back(e.b);holeGraph[e.b].push_back(e.a);}}
  std::vector<double> in(n,inf);using Item=std::pair<double,int32_t>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> q;
  for(int i:ids){in[i]=0;q.push({0,i});}
  const double cap=bend*1.6*(*std::max_element(vscale.begin(),vscale.end()));
  while(!q.empty()){auto [d,v]=q.top();q.pop();if(d!=in[v])continue;
   for(int u:holeGraph[v]){double nx=d+len(sub(p[u],p[v]));if(nx<=cap&&nx<in[u]){in[u]=nx;q.push({nx,u});}}}
  std::vector<int32_t> comp(tri.size(),-1);std::vector<double> radius;
  for(size_t f=0;f<tri.size();++f){
   if(!out.hole[f]||comp[f]>=0)continue;int c=(int)radius.size();double r=0;std::vector<int32_t> st{(int32_t)f};comp[f]=c;
   while(!st.empty()){int g=st.back();st.pop_back();for(int v:tri[g])if(std::isfinite(in[v]))r=std::max(r,in[v]);else r=std::max(r,cap);
    for(int h:topo.adj[g])if(out.hole[h]&&comp[h]<0){comp[h]=c;st.push_back(h);}}
   radius.push_back(r);
  }
  for(int i:ids){double r=0;for(int f:topo.vertexFaces[i])if(out.hole[f]&&comp[f]>=0)r=std::max(r,radius[comp[f]]);
   vs[i]=options&&!options->fixed_scale.empty()&&options->fixed_scale[i]?vscale[i]:vscale[i]*std::clamp(r/std::max(1e-12,1.3*bend*vscale[i]),.15,1.);}
 }
 // Coating graph: only edges of coating faces, so the two sides of a narrow
 // tear (separated by hole faces) do not share a rim or a direction.
 std::vector<std::vector<int32_t>> coatGraph(n);
 for(const auto&e:topo.edges){bool coat=false;for(int f:e.faces)if(!out.hole[f])coat=true;if(coat){coatGraph[e.a].push_back(e.b);coatGraph[e.b].push_back(e.a);}}
 // Rim width (world) of each rim vertex; the Dijkstra source tells every
 // vertex which rim vertex it belongs to.
 auto rimW=[&](int r){return bend*std::clamp(std::abs(rimWidth[r]),.2,3.)*std::max(1e-12,vs[r]);};
 // A negative rim width marks a cut edge (tear wall): it is a lip, but not a
 // curl source. The flaps beside a slit peel from the main rim; lifting them
 // towards the slit would curl both sides into each other.
 auto cut=[&](int r){return rimWidth[r]<0;};
 std::vector<double> travel(n,inf);std::vector<int32_t> src(n,-1);
 using Item=std::pair<double,int32_t>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
 for(int i:ids){travel[i]=cut(i)?1.3*rimW(i):0;src[i]=i;queue.push({travel[i],i});}
 while(!queue.empty()){
  auto [d,v]=queue.top();queue.pop();if(d!=travel[v])continue;
  if(pinned[v]&&!out.border[v])continue; // reached, never expanded past protected structure
  const double cap=1.3*rimW(src[v]);
  for(int u:coatGraph[v]){double next=d+len(sub(p[u],p[v]));if(next<=cap&&next<travel[u]){travel[u]=next;src[u]=src[v];queue.push({next,u});}}
 }
 // Narrow coating (a strip between two openings, a pointed tooth): where
 // fronts from opposing rims meet, both flaps would lift past the spine and
 // fold into each other. Cap each rim's width below half the strip width so
 // the spine stays attached.
 std::vector<double> strip(n,inf);
 for(const auto&e:topo.edges){
  const int a=e.a,b=e.b;if(src[a]<0||src[b]<0||src[a]==src[b]||cut(src[a])||cut(src[b]))continue;
  bool coat=false;for(int f:e.faces)if(!out.hole[f])coat=true;if(!coat)continue;
  if(dot(sub(p[src[a]],p[a]),sub(p[src[b]],p[b]))>=0)continue;
  const double w=travel[a]+travel[b]+len(sub(p[a],p[b]));
  strip[src[a]]=std::min(strip[src[a]],w);strip[src[b]]=std::min(strip[src[b]],w);
 }
 {
  std::vector<V> pts;for(int i:ids)pts.push_back(p[i]);Tree tree(pts);
  tbb::parallel_for(tbb::blocked_range<size_t>(0,n,512),[&](const tbb::blocked_range<size_t>&range){for(size_t i=range.begin();i!=range.end();++i){
   if(src[i]>=0){
    const int r=src[i];out.rim[i]=r;out.scale[i]=std::max(1e-12,vs[r]);
    const double world=len(sub(p[i],p[r]));
    out.distance[i]=out.rimDistance[i]=world/out.scale[i];
    double w=cut(r)?0.:std::min(rimW(r),.45*strip[r]);
    if(std::isfinite(guard[i]))w=std::min(w,std::max(0.,.9*guard[i]));
    if(pinned[i]&&!out.border[i])w=0;
    out.width[i]=w/out.scale[i];
   }else{
    auto hit=tree.nearest(p[i]);out.scale[i]=std::max(1e-12,vs[ids[hit.first]]);out.rimDistance[i]=hit.second/out.scale[i];
   }
  }});
 }
 // Width is Lipschitz along the coating (|dW| <= edge length): a lip vertex
 // next to protected structure (W~0) cannot sit beside a fully curled one,
 // which would twist the lip face between them.
 {
  std::vector<double> w(n,inf);std::priority_queue<Item,std::vector<Item>,std::greater<Item>> q;
  for(size_t i=0;i<n;++i)if(src[i]>=0){w[i]=out.width[i]*out.scale[i];q.push({w[i],(int32_t)i});}
  while(!q.empty()){auto [d,v]=q.top();q.pop();if(d!=w[v])continue;
   for(int u:coatGraph[v]){if(src[u]<0)continue;double nx=d+len(sub(p[u],p[v]));if(nx<w[u]){w[u]=nx;q.push({nx,u});}}}
  for(size_t i=0;i<n;++i)if(src[i]>=0)out.width[i]=w[i]/out.scale[i];
 }
 // Band: every coating face with a lifted vertex (or a lip vertex). There is
 // no raised coating outside the band, so nothing swells without a gap below.
 for(size_t f=0;f<tri.size();++f){
  if(out.hole[f]||locked[f])continue;
  for(int v:tri[f])if(out.border[v]||out.distance[v]<out.width[v]){out.band[f]=1;break;}
 }
 for(int pass=0;pass<8;++pass){
  std::vector<int> degree(n,0);
  for(const auto&e:topo.edges)if(e.faces.size()==2){bool a=out.hole[e.faces[0]]||out.band[e.faces[0]],b=out.hole[e.faces[1]]||out.band[e.faces[1]];if(a!=b){++degree[e.a];++degree[e.b];}}
  bool pinch=false;for(size_t v=0;v<n;++v)if(degree[v]>2){pinch=true;for(int f:topo.vertexFaces[v])if(!out.hole[f]&&!locked[f])out.band[f]=1;}
  if(!pinch)break;
 }
 // Keep only band connected (through vertices) to an opening: with a
 // per-vertex width, d<W can hold in an isolated spot whose underside and
 // floor would close on themselves, detached from any lip.
 {
  std::vector<uint8_t> seen(tri.size(),0);std::vector<int32_t> st;
  for(size_t f=0;f<tri.size();++f)if(out.hole[f]){seen[f]=1;st.push_back((int32_t)f);}
  while(!st.empty()){int g=st.back();st.pop_back();
   for(int v:tri[g])for(int h:topo.vertexFaces[v])if(!seen[h]&&out.band[h]){seen[h]=1;st.push_back(h);}}
  for(size_t f=0;f<tri.size();++f)if(out.band[f]&&!seen[f])out.band[f]=0;
 }
 {
  std::vector<uint8_t> visited(tri.size(),0);bool absorbed=false;
  for(size_t f=0;f<tri.size();++f){
   if(out.hole[f]||visited[f])continue;
   std::vector<int32_t> stack{int32_t(f)},component;visited[f]=1;
   bool rooted=false;int anchor=-1;double best=-1.;
   while(!stack.empty()){
    const int q=stack.back();stack.pop_back();component.push_back(q);
    rooted=rooted||!out.band[q];
    double score=inf;for(int v:tri[q])score=std::min(score,out.distance[v]*out.scale[v]);
    bool inner=true;for(int g:topo.adj[q])if(out.hole[g])inner=false;
    if(inner&&score>best){best=score;anchor=q;}
    for(int g:topo.adj[q])if(!out.hole[g]&&!visited[g]){visited[g]=1;stack.push_back(g);}
   }
   if(!rooted){
    if(anchor>=0)out.band[anchor]=0;
    else{for(int q:component)out.hole[q]=1;absorbed=true;}
   }
  }
  if(absorbed)return PlanCurl(p,normals,tri,out.hole,locked,vscale,rimWidth,guard,bend,options);
 }
 std::vector<int32_t> inside=out.hole;for(size_t f=0;f<tri.size();++f)inside[f]=inside[f]||out.band[f];
 auto roots=topo.boundary(n,inside);
 if(roots.empty()){std::fill(out.band.begin(),out.band.end(),0);return out;}
 {std::vector<V> pts;for(int i:roots)pts.push_back(p[i]);Tree rootTree(pts);tbb::parallel_for(tbb::blocked_range<size_t>(0,n,512),[&](const tbb::blocked_range<size_t>&range){for(size_t i=range.begin();i!=range.end();++i)out.rootDistance[i]=rootTree.nearest(p[i]).second;});}
 // A root vertex never lifts, whatever its rim distance: the band edge is
 // the attachment line and stays exactly on the original surface.
 for(int i:roots)if(!out.border[i])out.width[i]=std::min(out.width[i],out.distance[i]);
 // Peel direction, root -> rim, smoothed over the coating graph of the band.
 std::vector<uint8_t> active(n,0);
 for(size_t f=0;f<tri.size();++f)if(out.band[f])for(int v:tri[f])active[v]=1;
 std::vector<V> g(n,V{0,0,0});
 for(size_t i=0;i<n;++i){
  if(!active[i]||out.rim[i]<0)continue;
  const V nn=unit(normals[i],V{0,0,1});
  g[i]=unit(tangential(sub(p[out.rim[i]],p[i]),nn),V{0,0,0});
 }
 for(size_t i=0;i<n;++i){ // lip vertices: average of their coating neighbours
  if(!active[i]||!out.border[i])continue;V s{0,0,0};
  for(int u:coatGraph[i])if(active[u]&&!out.border[u])s=add(s,g[u]);
  g[i]=unit(tangential(s,unit(normals[i],V{0,0,1})),V{0,0,0});
 }
 for(int it=0;it<6;++it){
  std::vector<V> next=g;
  tbb::parallel_for(tbb::blocked_range<size_t>(0,n,2048),[&](const tbb::blocked_range<size_t>&range){for(size_t i=range.begin();i!=range.end();++i){
   if(!active[i])continue;V s{0,0,0};int c=0;
   for(int u:coatGraph[i])if(active[u]){s=add(s,g[u]);++c;}
   if(c)next[i]=add(mul(g[i],.5),mul(s,.5/c));
   next[i]=tangential(next[i],unit(normals[i],V{0,0,1}));
   double l=len(next[i]);if(l>1)next[i]=mul(next[i],1./l);
  }});
  g.swap(next);
 }
 out.direction=std::move(g);
 return out;
}

Offsets DeformCurl(const std::vector<V>&p,const std::vector<V>&normals,const CurlPlan&plan,
 const std::vector<double>&rimAmp,const std::vector<double>&depth,const CurlSettings&s){
 const size_t n=p.size();
 if(normals.size()!=n||rimAmp.size()!=n||depth.size()!=n||plan.distance.size()!=n||plan.width.size()!=n||
    plan.rim.size()!=n||plan.direction.size()!=n||plan.scale.size()!=n||plan.border.size()!=n||!(s.bend>0))
  throw std::invalid_argument("Peeling: invalid curl deformation arrays");
 const double curl=std::clamp(s.curl,0.,1.),power=1.+2.5*curl;
 // Up to ~150 degrees: a rolled lip, never folding back into its own sheet.
 const double thetaCap=(95.+55.*curl)*3.14159265358979323846/180.;
 const ThetaTable table(power,thetaCap);
 const V gravity=unit(s.gravity,V{0,0,-1});
 Offsets out;out.top.resize(n);out.floor.resize(n);out.back.resize(n);
 tbb::parallel_for(tbb::blocked_range<size_t>(0,n,1024),[&](const tbb::blocked_range<size_t>&range){for(size_t i=range.begin();i!=range.end();++i){
  const V nn=unit(normals[i],V{0,0,1});
  const double dep=std::max(0.,depth[i]);
  out.floor[i]=sub(p[i],mul(nn,dep));
  const double thick=std::min(s.thickness,dep*.5);
  const double W=plan.width[i]*plan.scale[i],d=plan.distance[i]*plan.scale[i];
  double u=0;
  if(plan.border[i]&&plan.rim[i]==int32_t(i))u=1;
  else if(std::isfinite(d)&&W>1e-12&&d<W)u=1.-d/W;
  if(u<=0||plan.rim[i]<0){
   out.top[i]=p[i];
   out.back[i]=sub(p[i],mul(nn,std::max(thick,dep*.96)));
   continue;
  }
  const V g=plan.direction[i];const double m=len(g);const V gu=m>1e-12?mul(g,1./m):V{0,0,0};
  double amp=std::max(0.,rimAmp[plan.rim[i]]);
  if(s.droop>0&&m>1e-12)amp*=std::max(.15,1.+std::clamp(s.droop,0.,1.)*dot(gu,gravity)*m);
  const double Wr=std::max(W,1e-12);
  // Target tip height: Edge Lift, scaled like v1 (rim scale) and by flap
  // width (tongues stand higher), solved for the arc angle; the curvature
  // radius at the lip stays >= 2.2 x thickness so the skin cannot crease.
  const double H=s.lift*amp*plan.scale[i]*std::sqrt(std::max(.2,plan.width[i]/s.bend));
  double Theta=table.Solve(H/Wr);
  if(thick>0)Theta=std::min(Theta,Wr/(2.2*power*thick));
  const double a=u*Wr;const Arc arc=ArcAt(Theta,power,u);
  const double th=Theta*std::pow(u,power);
  // top = attached root + arc; the horizontal component follows the smoothed
  // direction field (length < 1 where two rims compete, e.g. on a tongue).
  out.top[i]=add(p[i],add(mul(g,Wr*arc.x-a),mul(nn,Wr*arc.z)));
  const V nb=unit(sub(mul(nn,std::cos(th)),mul(g,std::sin(th))),nn);
  // Constant skin thickness; only near the hidden root the underside drops
  // to (just above) the floor, so the opening shows no shelf.
  const double blend=1.-smooth01(u/std::max(1e-6,s.rootBlend));
  const double backDepth=thick+(std::max(thick,dep*.96)-thick)*blend;
  out.back[i]=sub(out.top[i],mul(nb,backDepth));
 }});
 return out;
}

std::vector<int32_t> RefineAdaptive(const std::vector<V>&p,const std::vector<V>&n,
 const std::vector<Edge>&edges,const std::vector<int32_t>&ok,const std::vector<double>&values,
 const std::vector<V>&seeds,double step,double bend,double reach,double probe,double size,double fine,bool adaptive){
 validate(p);
 if(n.size()!=p.size()||ok.size()!=edges.size()||values.size()!=p.size()||!(step>0)||!(size>0))throw std::invalid_argument("Peeling: invalid adaptive refinement arrays");
 Tree seedTree(seeds);
 std::vector<std::pair<double,int32_t>> chosen;
 // Field units are Patch Size; the noise/drawing field has a gradient of
 // roughly 1.2 / Patch Size, so |value| * size / 1.2 estimates the distance.
 const double toDistance=size/1.2;
 for(size_t i=0;i<edges.size();++i){
  if(!ok[i])continue;auto e=edges[i];
  if(e[0]<0||e[1]<0||size_t(e[0])>=p.size()||size_t(e[1])>=p.size())throw std::invalid_argument("Peeling: invalid edge index");
  const V mid=mul(add(p[e[0]],p[e[1]]),.5);const double length=len(sub(p[e[1]],p[e[0]]));
  if(length<=step*fine*1.4)continue;
  if(adaptive&&(seeds.empty()||seedTree.nearest(mid).second>length*.5+reach+probe*1.5))continue;
  double target=step,dc=reach;
  const double fa=values[e[0]],fb=values[e[1]];
  if(adaptive&&std::isfinite(fa)&&std::isfinite(fb)){
   dc=(fa<0)!=(fb<0)?0.:std::min(std::abs(fa),std::abs(fb))*toDistance;
   const bool coat=fa>=0&&fb>=0;
   if(dc<1.5*step)target=step*fine;                              // the torn contour itself
   else if(coat&&dc<reach*1.15+length*.5)target=step;             // bending zone
   else if(coat&&dc<reach*1.6+length*.5)target=step*2.5;          // grading towards the original mesh
   else if(coat)continue;                                         // untouched coating
   else target=dc<bend*.5+length*.5?step:step*3.;                // flat exposed floor
  }
  // Curved support: refine where the surface turns within the edge.
  const double c=dot(unit(n[e[0]],V{0,0,1}),unit(n[e[1]],V{0,0,1}));
  if(c<.98)target=std::min(target,step*.75);
  if(length>target*1.4)chosen.push_back({length/target*(1.+step/(step+dc)),(int32_t)i});
 }
 std::stable_sort(chosen.begin(),chosen.end(),[](const auto&a,const auto&b){return a.first>b.first;});
 std::vector<int32_t> out;out.reserve(chosen.size());for(const auto&c:chosen)out.push_back(c.second);
 return out;
}

std::vector<double> DrawingValuesSurface(const std::vector<V>&p,const std::vector<V>&normals,
 const std::vector<std::array<double,14>>&frames,const std::vector<std::array<double,2>>&poly,
 const std::vector<V>&line,const std::vector<int32_t>&offsets,double size,double radius){
 validate(p);
 if(normals.size()!=p.size()||offsets.size()!=frames.size()+1||!(size>0)||line.size()!=poly.size())throw std::invalid_argument("Peeling: invalid drawing arrays");
 if(offsets.empty()||offsets.front()!=0||offsets.back()!=int32_t(poly.size()))throw std::invalid_argument("Peeling: invalid drawing offsets");
 std::vector<double> result(p.size(),1e6);
 for(size_t i=0;i<p.size();++i)for(size_t r=0;r<frames.size();++r){
  const auto&f=frames[r];V delta=sub(p[i],{f[0],f[1],f[2]}),u{f[3],f[4],f[5]},v{f[6],f[7],f[8]},nr{f[9],f[10],f[11]};
  if(std::abs(dot(delta,nr))>f[12]||dot(normals[i],nr)<.35)continue;
  double x=dot(delta,u),y=dot(delta,v),distance=inf;bool inside=false,area=f[13]!=0;
  int a=offsets[r],end=offsets[r+1];if(a<0||end<a||end>int(poly.size()))throw std::invalid_argument("Peeling: invalid contour offset");
  for(int j=a;j<end-(area?0:1);++j){
   const int k=j+1<end?j+1:a;
   const V A=line[j],B=line[k],AB=sub(B,A);
   double t=std::clamp(dot(sub(p[i],A),AB)/std::max(1e-20,dot(AB,AB)),0.,1.);
   distance=std::min(distance,len(sub(p[i],add(A,mul(AB,t)))));
   const auto&A2=poly[j];const auto&B2=poly[k];
   if(area&&(A2[1]>y)!=(B2[1]>y)){double cr=A2[0]+(y-A2[1])*(B2[0]-A2[0])/(B2[1]-A2[1]);if(x<cr)inside=!inside;}
  }
  double value=area?(inside?-distance:distance):distance-radius;result[i]=std::min(result[i],value/size);
 }
 return result;
}
}
