// общие объявления: вход для согласованной обработки соседних внутренних граней
// SPDX-License-Identifier: GPL-3.0-or-later
// Shared, piecewise-affine fracture warp. Every fragment samples the SAME
// tetrahedral field, so an outward bump is the neighbour's inward socket.
#pragma once
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include "manifold/manifold.h"
#include <array>
#include <map>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace pd_matched {
using V=manifold::vec3;
inline double dot(const V&a,const V&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline V cross(const V&a,const V&b){return V(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
inline double norm(const V&v){return std::sqrt(dot(v,v));}
inline V unit(const V&v){return v/std::max(norm(v),1e-30);}
inline uint64_t mix(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
struct Key {int64_t x,y,z;int owner=0;bool operator==(const Key& b)const{return x==b.x&&y==b.y&&z==b.z&&owner==b.owner;}};
struct Hash {size_t operator()(const Key&k)const{return mix(k.x)^mix(k.y+0x1234)^mix(k.z+0x5678)^mix(k.owner+19);}};
inline V position(const Key&k,double L){return V(double(k.x)*L,double(k.y)*L,double(k.z)*L);}
inline Key corner(Key k,int axis){if(axis==0)++k.x;else if(axis==1)++k.y;else ++k.z;return k;}
static const int permutations[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};

struct Weld {
    double eps;std::vector<V> p;std::unordered_map<Key,std::vector<int32_t>,Hash> map;
    explicit Weld(double e):eps(e){}
    int32_t get(int owner,const V&v){
        Key k{int64_t(std::floor(v.x/eps)),int64_t(std::floor(v.y/eps)),int64_t(std::floor(v.z/eps)),owner};
        for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
            auto it=map.find({k.x+x,k.y+y,k.z+z,owner});if(it==map.end())continue;
            for(auto i:it->second)if(norm(p[i]-v)<=eps)return i;
        }
        int32_t i=(int32_t)p.size();p.push_back(v);map[k].push_back(i);return i;
    }
};
struct Edge {int32_t a,b;bool operator==(const Edge&o)const{return a==o.a&&b==o.b;}};
struct EdgeHash {size_t operator()(const Edge&e)const{return mix((uint64_t(uint32_t(e.a))<<32)|uint32_t(e.b));}};
inline Edge edge(int32_t a,int32_t b){return a<b?Edge{a,b}:Edge{b,a};}

inline V closest(const V&p,const V&a,const V&b,const V&c){
    const V ab=b-a,ac=c-a,ap=p-a;double d1=dot(ab,ap),d2=dot(ac,ap);
    if(d1<=0&&d2<=0)return a;V bp=p-b;double d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0&&d4<=d3)return b;double vc=d1*d4-d3*d2;
    if(vc<=0&&d1>=0&&d3<=0)return a+ab*(d1/(d1-d3));V cp=p-c;double d5=dot(ab,cp),d6=dot(ac,cp);
    if(d6>=0&&d5<=d6)return c;double vb=d5*d2-d1*d6;
    if(vb<=0&&d2>=0&&d6<=0)return a+ac*(d2/(d2-d6));double va=d3*d6-d5*d4;
    if(va<=0&&d4-d3>=0&&d5-d6>=0)return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    double den=va+vb+vc;if(std::fabs(den)<1e-30)return a;
    return a+ab*(vb/den)+ac*(vc/den);
}
struct Shell {
    struct Tri {V a,b,c,n,lo,hi;bool fixed;};
    struct Node {V lo,hi;size_t begin,end;int left=-1,right=-1;};
    std::vector<Tri> tris;std::vector<size_t> order;std::vector<Node> nodes;
    void add(V a,V b,V c,bool fixed){
        V n=cross(b-a,c-a);if(norm(n)<1e-20)return;
        tris.push_back({a,b,c,unit(n),manifold::la::min(a,manifold::la::min(b,c)),manifold::la::max(a,manifold::la::max(b,c)),fixed});
    }
    int build(size_t begin,size_t end){
        Node n{V(1e300),V(-1e300),begin,end};
        for(size_t i=begin;i<end;++i){n.lo=manifold::la::min(n.lo,tris[order[i]].lo);n.hi=manifold::la::max(n.hi,tris[order[i]].hi);}
        int id=nodes.size();nodes.push_back(n);if(end-begin<=8)return id;
        V d=n.hi-n.lo;int axis=d.x>=d.y&&d.x>=d.z?0:d.y>=d.z?1:2;size_t mid=(begin+end)/2;
        std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](size_t a,size_t b){return tris[a].lo[axis]+tris[a].hi[axis]<tris[b].lo[axis]+tris[b].hi[axis];});
        int a=build(begin,mid),b=build(mid,end);nodes[id].left=a;nodes[id].right=b;return id;
    }
    void prepare(){order.resize(tris.size());std::iota(order.begin(),order.end(),0);if(!order.empty())build(0,order.size());}
    V constrain(const V&p,V d,double radius,double jagged)const{
        if(nodes.empty())return d;
        std::array<V,3> basis;int rank=0;std::vector<int> stack{0};double r2=radius*radius;
        while(!stack.empty()){
            const auto&node=nodes[stack.back()];stack.pop_back();
            V gap=manifold::la::max(manifold::la::max(node.lo-p,p-node.hi),V(0.0));if(dot(gap,gap)>r2)continue;
            if(node.left>=0){stack.push_back(node.left);stack.push_back(node.right);continue;}
            for(size_t i=node.begin;i<node.end;++i){const auto&t=tris[order[i]];
                V gap=manifold::la::max(manifold::la::max(t.lo-p,p-t.hi),V(0.0));if(dot(gap,gap)>r2)continue;
                V q=closest(p,t.a,t.b,t.c);if(dot(q-p,q-p)>r2)continue;
                                                                         
                                                                       
                                                         
                if(t.fixed&&dot(q-p,q-p)<=std::max(1e-22,r2*1e-10))return V(0.0);
                V n=t.n;for(int j=0;j<rank;++j)n-=basis[j]*dot(n,basis[j]);
                if(norm(n)>1e-5){basis[rank++]=unit(n);if(rank==3)return V(0.0);}
            }
        }
        if(rank){for(int j=0;j<rank;++j)d-=basis[j]*dot(d,basis[j]);d*=jagged;}
        return d;
    }
};

struct Vertex {V p,w;};
using Poly=std::vector<Vertex>;
inline Poly clip(const Poly&in,const V&normal,double offset,double eps){
    Poly out;if(in.empty())return out;
    Vertex a=in.back();double da=dot(a.p,normal)-offset;
    for(const auto&b:in){double db=dot(b.p,normal)-offset;
        if((da>=-eps)!=(db>=-eps)){
            double t=std::clamp(da/(da-db),0.0,1.0);out.push_back({a.p+(b.p-a.p)*t,a.w+(b.w-a.w)*t});
        }
        if(db>=-eps)out.push_back(b);a=b;da=db;
    }
    Poly clean;for(const auto&v:out)if(clean.empty()||norm(clean.back().p-v.p)>eps)clean.push_back(v);
    if(clean.size()>1&&norm(clean.back().p-clean.front().p)<=eps)clean.pop_back();return clean;
}
inline double area(const Poly&p){V n(0.0);if(p.size()<3)return 0;for(size_t i=1;i+1<p.size();++i)n+=cross(p[i].p-p[0].p,p[i+1].p-p[0].p);return norm(n)*.5;}

struct Output {
    std::vector<double> points,rest,pointWeights,cornerWeights;
    std::vector<int32_t> pointSources,cornerSources,loops,starts,totals,faceSources,owners;
    double factor=1.;
};
struct Engine {
    const double* xyz;const int32_t *loops,*starts,*totals,*triLoops,*triFaces,*eligible,*held,*owners;
    size_t nv,nf,nt;double L,eps,strength,scale,rocky,jagged;int seed;size_t budget;
    Weld canonical,vertices;
    Shell shell;Output out;
    std::vector<int32_t> canon,pointOwner;
    std::unordered_map<Edge,std::vector<int32_t>,EdgeHash> cuts;
    std::unordered_map<Key,V,Hash> nodes;
    std::unordered_map<Key,char,Hash> cells;
    Engine(const double*x,const int32_t*l,const int32_t*s,const int32_t*t,const int32_t*tl,const int32_t*tf,const int32_t*e,const int32_t*h,const int32_t*o,size_t n,size_t f,size_t tr,double cell,double amp,double sc,double rock,double jag,int sd,size_t cap)
        :xyz(x),loops(l),starts(s),totals(t),triLoops(tl),triFaces(tf),eligible(e),held(h),owners(o),nv(n),nf(f),nt(tr),L(cell),eps(std::max(1e-11,cell*1e-8)),strength(amp),scale(sc),rocky(rock),jagged(jag),seed(sd),budget(cap),canonical(eps),vertices(eps){}
    V pos(int i)const{return V(xyz[3*i],xyz[3*i+1],xyz[3*i+2]);}
    int add(int owner,const Vertex&v,const std::array<int32_t,3>&source){
        size_t before=vertices.p.size();int id=vertices.get(owner,v.p);
        if(vertices.p.size()!=before){
            if(vertices.p.size()>budget)throw std::runtime_error("Noisify budget reached; lower Resolution or increase Detail Size");
            for(int j=0;j<3;++j){out.pointSources.push_back(source[j]);out.pointWeights.push_back(v.w[j]);}
        }return id;
    }
    void face(const Poly&p,const std::array<int32_t,3>&vl,const std::array<int32_t,3>&cl,int source){
        if(area(p)<eps*eps)return;
        std::vector<int32_t> indices;for(const auto&v:p)indices.push_back(add(owners[source],v,vl));
        if(indices.size()<3)return;
        out.starts.push_back(out.loops.size());out.totals.push_back(indices.size());out.faceSources.push_back(source);out.owners.push_back(owners[source]);
        for(size_t i=0;i<indices.size();++i){out.loops.push_back(indices[i]);for(int j=0;j<3;++j){out.cornerSources.push_back(cl[j]);out.cornerWeights.push_back(p[i].w[j]);}}
    }
    void inner(int triangle){
        int f=triFaces[triangle];std::array<int32_t,3> cl,vl;
        Poly poly;for(int j=0;j<3;++j){cl[j]=triLoops[3*triangle+j];vl[j]=loops[cl[j]];V w(0.0);w[j]=1;poly.push_back({pos(vl[j]),w});}
        std::vector<Poly> pieces{poly};
        for(int axis=0;axis<3;++axis){
            std::vector<Poly> next;V n(0.0);n[axis]=1.;
            for(const auto&p:pieces){double lo=1e300,hi=-1e300;for(auto&v:p){lo=std::min(lo,v.p[axis]);hi=std::max(hi,v.p[axis]);}
                int64_t a=int64_t(std::floor((lo+eps)/L)),b=int64_t(std::floor((hi-eps)/L));if(b<a)b=a;
                if(b-a>100000)throw std::runtime_error("Noisify: too many grid cells; lower Resolution");
                for(int64_t k=a;k<=b;++k){auto q=clip(clip(p,n,double(k)*L,eps),-n,-double(k+1)*L,eps);if(area(q)>eps*eps)next.push_back(std::move(q));}
            }pieces=std::move(next);
            if(pieces.size()>budget)throw std::runtime_error("Noisify budget reached; lower Resolution");
        }
        for(const auto&p:pieces){V center(0.0);for(auto&v:p)center+=v.p;center/=double(p.size());
            Key cell{int64_t(std::floor((center.x+eps)/L)),int64_t(std::floor((center.y+eps)/L)),int64_t(std::floor((center.z+eps)/L))};cells[cell]=1;
            for(const auto&perm:permutations){Poly q=p;bool skip=false;
                for(int j=0;j<2;++j){int a=perm[j],b=perm[j+1];V n(0.0);n[a]=1.;n[b]=-1.;V low=position(cell,L);double offset=low[a]-low[b];
                    bool on=true;for(auto&v:q)if(std::fabs(dot(v.p,n)-offset)>eps){on=false;break;}
                    if(on&&a>b){skip=true;break;}
                    q=clip(q,n,offset,eps);if(q.size()<3){skip=true;break;}
                }
                if(skip||area(q)<eps*eps)continue;
                                                                      
                for(size_t i=1;i+1<q.size();++i){Poly tri{q[0],q[i],q[i+1]};if(area(tri)<eps*eps)continue;face(tri,vl,cl,f);}
                                                                               
                for(auto&v:q)for(int zero=0;zero<3;++zero)if(std::fabs(v.w[zero])<1e-7){
                    int a=(zero+1)%3,b=(zero+2)%3;Edge e=edge(canon[vl[a]],canon[vl[b]]);
                    cuts[e].push_back(add(owners[f],v,vl));
                }
            }
        }
    }
    double noise(const V&p,uint64_t salt)const{
        int64_t a[3];V t;for(int j=0;j<3;++j){a[j]=int64_t(std::floor(p[j]));double f=p[j]-double(a[j]);t[j]=f*f*(3-2*f);}
        double sum=0;
        for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){
            uint64_t h=mix(uint64_t(a[0]+x)^salt)^mix(uint64_t(a[1]+y)+salt+31)^mix(uint64_t(a[2]+z)+salt+71);
            double v=(double(mix(h)>>11)*(1./9007199254740992.))*2.-1.;
            sum+=v*(x?t.x:1-t.x)*(y?t.y:1-t.y)*(z?t.z:1-t.z);
        }return sum;
    }
    V node(Key k){k.owner=0;auto it=nodes.find(k);if(it!=nodes.end())return it->second;
        V p=position(k,L),d(0.0);double weight=0.,freq=1.,amp=1.;
        for(int o=0;o<3;++o){for(int c=0;c<3;++c){double v=noise(p*(freq/scale),mix(uint64_t(seed)+c*173+o*913));double ridge=(std::pow(1.-std::fabs(v),2)-.5)*2.;d[c]+=amp*(v*(1-rocky)+ridge*rocky);}weight+=amp;freq*=2.;amp*=.5;}
        d*=strength/weight;d=shell.constrain(p,d,L*.35,jagged);nodes.emplace(k,d);return d;
    }
    V displacement(const V&p){Key k{int64_t(std::floor(p.x/L)),int64_t(std::floor(p.y/L)),int64_t(std::floor(p.z/L))};V f=(p-position(k,L))/L;
        std::array<int,3> perm{0,1,2};std::stable_sort(perm.begin(),perm.end(),[&](int a,int b){return f[a]>f[b];});
        Key k1=corner(k,perm[0]),k2=corner(k1,perm[1]),k3=corner(k2,perm[2]);
        return node(k)*(1-f[perm[0]])+node(k1)*(f[perm[0]]-f[perm[1]])+node(k2)*(f[perm[1]]-f[perm[2]])+node(k3)*f[perm[2]];
    }
    bool closedOutput() const {
        std::unordered_map<Edge,int,EdgeHash> counts;
        for(size_t f=0;f<out.starts.size();++f){int start=out.starts[f],n=out.totals[f];
            for(int j=0;j<n;++j){int a=out.loops[start+j],b=out.loops[start+(j+1)%n];if(a==b)return false;++counts[edge(a,b)];}}
        for(const auto&entry:counts)if(entry.second!=2)return false;return true;
    }
    Output refinedOutput(){
                                                                            
                                                                              
                                                                         
        Output safe;safe.factor=out.factor;
        std::map<int,std::vector<size_t>> groups;
        for(size_t t=0;t<nt;++t)groups[owners[triFaces[t]]].push_back(t);
        for(const auto&entry:groups){
            manifold::MeshGL64 input;input.numProp=7;std::map<int,int> first;
            for(size_t t:entry.second){
                for(int j=0;j<3;++j){int v=loops[triLoops[3*t+j]],idx=input.NumVert();V p=pos(v);
                    input.vertProperties.insert(input.vertProperties.end(),{p.x,p.y,p.z,j==0?1.:0.,j==1?1.:0.,j==2?1.:0.,double(t)});
                    input.triVerts.push_back(idx);
                    auto found=first.emplace(v,idx);if(!found.second){input.mergeFromVert.push_back(idx);input.mergeToVert.push_back(found.first->second);}}
                input.faceID.push_back(t);
            }
            input.runIndex={0,uint32_t(input.triVerts.size())};input.runOriginalID={manifold::Manifold::ReserveIDs(1)};
            manifold::Manifold solid(input);
            if(solid.Status()!=manifold::Manifold::Error::NoError)throw std::runtime_error("Noisify: source fragment must have closed, consistently oriented topology");
            auto mesh=solid.RefineToLength(L).GetMeshGL64();size_t n=mesh.NumVert();
            std::vector<int> parent(n);std::iota(parent.begin(),parent.end(),0);
            auto root=[&](int x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
            for(size_t i=0;i<mesh.mergeFromVert.size();++i)parent[root(mesh.mergeFromVert[i])]=root(mesh.mergeToVert[i]);
            std::map<int,int> ids;std::vector<int> map(n);std::vector<bool> active;
            size_t offset=safe.points.size()/3;
            for(size_t i=0;i<n;++i){int r=root(i);auto found=ids.emplace(r,int(safe.points.size()/3));map[i]=found.first->second;
                if(!found.second)continue;
                auto*p=&mesh.vertProperties[i*mesh.numProp];V position(p[0],p[1],p[2]);
                size_t t=std::min(nt-1,size_t(std::max(0.,std::round(p[6]))));
                safe.rest.insert(safe.rest.end(),{position.x,position.y,position.z});safe.points.insert(safe.points.end(),{position.x,position.y,position.z});
                for(int j=0;j<3;++j){safe.pointSources.push_back(loops[triLoops[3*t+j]]);safe.pointWeights.push_back(p[3+j]);}active.push_back(false);
                if(safe.points.size()/3>budget)throw std::runtime_error("Noisify budget reached; lower Resolution");
            }
            for(size_t t=0;t<mesh.NumTri();++t){int original=mesh.faceID[t],f=triFaces[original];safe.starts.push_back(safe.loops.size());safe.totals.push_back(3);safe.faceSources.push_back(f);safe.owners.push_back(entry.first);
                for(int j=0;j<3;++j){int v=mesh.triVerts[3*t+j],id=map[v];auto*p=&mesh.vertProperties[size_t(v)*mesh.numProp];
                    size_t origin=std::min(nt-1,size_t(std::max(0.,std::round(p[6]))));
                    safe.loops.push_back(id);if(eligible[f]&&!held[f])active[id-offset]=true;
                    for(int k=0;k<3;++k){safe.cornerSources.push_back(triLoops[3*origin+k]);safe.cornerWeights.push_back(p[3+k]);}}
            }
            for(size_t j=0;j<active.size();++j)if(active[j]){size_t i=offset+j;V p(safe.rest[3*i],safe.rest[3*i+1],safe.rest[3*i+2]);V q=p+shell.constrain(p,displacement(p)*safe.factor,L*1e-7,1.);for(int c=0;c<3;++c)safe.points[3*i+c]=q[c];}
        }
        return safe;
    }

    Output run(){
        pointOwner.assign(nv,-1);for(size_t f=0;f<nf;++f)for(int j=0;j<totals[f];++j)pointOwner[loops[starts[f]+j]]=owners[f];
        canon.resize(nv);for(size_t i=0;i<nv;++i)canon[i]=canonical.get(pointOwner[i],pos(i));
        for(size_t t=0;t<nt;++t){int f=triFaces[t];if(!eligible[f]||held[f])shell.add(pos(loops[triLoops[3*t]]),pos(loops[triLoops[3*t+1]]),pos(loops[triLoops[3*t+2]]),held[f]!=0);}
        shell.prepare();
        for(size_t t=0;t<nt;++t)if(eligible[triFaces[t]]&&!held[triFaces[t]])inner(t);
                                                                              
        size_t affected=vertices.p.size();
        for(auto&item:cuts){auto&list=item.second;std::sort(list.begin(),list.end());list.erase(std::unique(list.begin(),list.end()),list.end());}
        for(size_t f=0;f<nf;++f)if(!eligible[f]||held[f]){
            int start=out.loops.size();
            for(int j=0;j<totals[f];++j){int a=loops[starts[f]+j],b=loops[starts[f]+(j+1)%totals[f]],la=starts[f]+j,lb=starts[f]+(j+1)%totals[f];V ab=pos(b)-pos(a);double len2=dot(ab,ab);
                std::vector<std::pair<double,V>> along{{0.,pos(a)}};auto it=cuts.find(edge(canon[a],canon[b]));
                if(it!=cuts.end()&&len2>eps*eps)for(int id:it->second){const V&p=vertices.p[id];double t=dot(p-pos(a),ab)/len2;if(t>eps/std::sqrt(len2)&&t<1.-eps/std::sqrt(len2))along.push_back({t,p});}
                std::sort(along.begin(),along.end(),[](auto&a,auto&b){return a.first<b.first;});
                for(auto&item:along){Vertex v{item.second,V(1.-item.first,item.first,0.)};int id=add(owners[f],v,{a,b,b});
                    if(out.loops.size()>size_t(start)&&out.loops.back()==id)continue;
                    out.loops.push_back(id);for(int k=0;k<3;++k){out.cornerSources.push_back(k==0?la:lb);out.cornerWeights.push_back(v.w[k]);}
                }
            }
            out.starts.push_back(start);out.totals.push_back(out.loops.size()-start);out.faceSources.push_back(f);out.owners.push_back(owners[f]);
        }
                                                                            
                                                                            
        double gradient=0.;
        for(auto&item:cells)for(auto&perm:permutations){Key a=item.first,b=corner(a,perm[0]),c=corner(b,perm[1]),d=corner(c,perm[2]);
            V x=node(b)-node(a),y=node(c)-node(b),z=node(d)-node(c);
            gradient=std::max(gradient,std::sqrt(dot(x,x)+dot(y,y)+dot(z,z))/L);
        }
        out.factor=gradient>.45?.45/gradient:1.;
        for(size_t i=0;i<vertices.p.size();++i){V p=vertices.p[i],q=p;if(i<affected)q+=shell.constrain(p,displacement(p)*out.factor,L*1e-7,1.);for(int j=0;j<3;++j){out.rest.push_back(p[j]);out.points.push_back(q[j]);}}
        if(!closedOutput())return refinedOutput();
        return std::move(out);
    }
};

template<typename T> nanobind::ndarray<nanobind::numpy,T> array(std::vector<T>&&v,size_t cols=0){
    auto* p=new std::vector<T>(std::move(v));if(p->empty())p->reserve(1);
    nanobind::capsule owner(p,[](void*x)noexcept{delete static_cast<std::vector<T>*>(x);});
    if(cols)return {p->data(),{p->size()/cols,cols},owner};return {p->data(),{p->size()},owner};
}
}                        

inline void BindInnerMatched(nanobind::module_&mod){
    namespace nb=nanobind;using A=nb::ndarray<const double,nb::c_contig,nb::device::cpu>;using I=nb::ndarray<const int32_t,nb::c_contig,nb::device::cpu>;
    mod.def("noisify_matched",[](A xyz,I loops,I starts,I totals,I triLoops,I triFaces,I eligible,I held,I owners,double cell,double strength,double scale,double rocky,double jagged,int seed,size_t budget){
        size_t nv=xyz.size()/3,nf=starts.size(),nt=triFaces.size();
        if(xyz.size()%3||totals.size()!=nf||eligible.size()!=nf||held.size()!=nf||owners.size()!=nf||triLoops.size()!=nt*3)throw std::invalid_argument("Noisify input array size mismatch");
        if(!(std::isfinite(cell)&&cell>0&&std::isfinite(scale)&&scale>0&&std::isfinite(strength)&&strength>=0&&std::isfinite(rocky)&&std::isfinite(jagged)))throw std::invalid_argument("Noisify parameters must be finite and sizes positive");
        for(size_t i=0;i<xyz.size();++i)if(!std::isfinite(xyz.data()[i]))throw std::invalid_argument("Noisify points must be finite");
        for(size_t i=0;i<loops.size();++i)if(loops.data()[i]<0||size_t(loops.data()[i])>=nv)throw std::invalid_argument("Noisify vertex index out of range");
        for(size_t i=0;i<nf;++i)if(starts.data()[i]<0||totals.data()[i]<3||size_t(starts.data()[i])+size_t(totals.data()[i])>loops.size())throw std::invalid_argument("Noisify face range invalid");
        for(size_t i=0;i<nt*3;++i)if(triLoops.data()[i]<0||size_t(triLoops.data()[i])>=loops.size())throw std::invalid_argument("Noisify triangle loop out of range");
        for(size_t i=0;i<nt;++i)if(triFaces.data()[i]<0||size_t(triFaces.data()[i])>=nf)throw std::invalid_argument("Noisify triangle face out of range");
        pd_matched::Output out;
        {nb::gil_scoped_release release;
            pd_matched::Engine engine(xyz.data(),loops.data(),starts.data(),totals.data(),triLoops.data(),triFaces.data(),eligible.data(),held.data(),owners.data(),nv,nf,nt,cell,strength,scale,std::clamp(rocky,0.,1.),std::clamp(jagged,0.,1.5),seed,budget);
            out=engine.run();}
        nb::dict result;result["points"]=pd_matched::array(std::move(out.points),3);result["rest_points"]=pd_matched::array(std::move(out.rest),3);
        result["loops"]=pd_matched::array(std::move(out.loops));result["starts"]=pd_matched::array(std::move(out.starts));result["totals"]=pd_matched::array(std::move(out.totals));
        result["face_sources"]=pd_matched::array(std::move(out.faceSources));result["owners"]=pd_matched::array(std::move(out.owners));
        result["point_sources"]=pd_matched::array(std::move(out.pointSources),3);result["point_weights"]=pd_matched::array(std::move(out.pointWeights),3);
        result["corner_sources"]=pd_matched::array(std::move(out.cornerSources),3);result["corner_weights"]=pd_matched::array(std::move(out.cornerWeights),3);result["strength_factor"]=out.factor;return result;
    },nb::arg("points"),nb::arg("loops"),nb::arg("starts"),nb::arg("totals"),nb::arg("tri_loops"),nb::arg("tri_faces"),nb::arg("inner"),nb::arg("held"),nb::arg("owners"),nb::arg("cell_size"),nb::arg("strength"),nb::arg("scale"),nb::arg("rocky")=.6,nb::arg("jagged")=.8,nb::arg("seed")=0,nb::arg("max_vertices")=400000);
    mod.attr("matched_inner_faces")=true;
}
