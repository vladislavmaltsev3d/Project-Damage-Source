// общие объявления: формируем неровные пятна разрывов ткани через шум
#pragma once
#include "fabric_tears_params.h"
#include "manifold/cross_section.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fabric_noise {
using V=manifold::vec3;
using V2=manifold::vec2;
inline double Hash(uint64_t seed,int64_t x,int64_t y,int64_t z) {
    uint64_t n=seed^(uint64_t(x)*0x9e3779b97f4a7c15ULL)^(uint64_t(y)*0xd1b54a32d192ed03ULL)^(uint64_t(z)*0x94d049bb133111ebULL);
    n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL;n=(n^(n>>27))*0x94d049bb133111ebULL;n^=n>>31;
    return double(n>>11)*(1.0/9007199254740992.0);
}
inline double Fade(double x){return x*x*x*(x*(x*6.0-15.0)+10.0);}
inline double Value(const V& q,uint64_t seed) {
    const int64_t x=int64_t(std::floor(q.x)),y=int64_t(std::floor(q.y)),z=int64_t(std::floor(q.z));
    const double fx=Fade(q.x-x),fy=Fade(q.y-y),fz=Fade(q.z-z);
    double n=0.0;
    for(int a=0;a<2;++a)for(int b=0;b<2;++b)for(int c=0;c<2;++c)
        n+=Hash(seed,x+a,y+b,z+c)*(a?fx:1-fx)*(b?fy:1-fy)*(c?fz:1-fz);
    return n;
}
inline double Field(V q,uint64_t seed) {
    double n=0.,weight=1.,sum=0.;
    for(int o=0;o<3;++o){n+=weight*Value(q,seed+uint64_t(o)*1013);sum+=weight;weight*=.42;q=q*2.07+V(13.7,-8.1,4.6);}
    return n/sum;
}
inline double Weight(const FabricTearParams& p,const V& pos,uint64_t seed) {
    const double s=std::clamp(p.distributionStrength,0.,1.);
    if(s<=0.)return 1.;
    const V offset(Hash(seed,1,2,3)*100.,Hash(seed,4,5,6)*100.,Hash(seed,7,8,9)*100.);
    const double f=Field(pos/std::max(.0001,p.distributionScale)+offset,seed^0x4641424eULL);
    const double n=std::clamp(.5+1.6*(f-.5),0.,1.);
    return .002+.998*std::pow(n,6.*s);
}
inline manifold::Polygons Outline(const FabricTearParams& p,double L,double W,uint64_t seed,uint64_t key,bool edge=false,V2 inward=V2(1,0)) {
    L=std::max(.0001,L);W=std::max(.0001,W);
    const double pitch=std::max(.0001,p.yarnSize/std::clamp(p.detail,.05,4.));
    const double ns=std::max({p.shapeNoiseScale,4.*pitch,.02*std::max(L,W)}),strength=std::clamp(p.shapeNoiseStrength,0.,1.);
    const double perimeter=manifold::kPi*(L+W);
    const int count=std::clamp(int(std::ceil(perimeter/std::max(pitch,.002*perimeter))),48,512);
    const V offset(Hash(seed,key,1,0)*50.,Hash(seed,key,2,0)*50.,Hash(seed,key,3,0)*50.);
    const double beta=Hash(seed,key,33,0)*6.283185307179586;
    const V2 axis=edge?inward:V2(std::cos(beta)*.14+1.,std::sin(beta)*.14);
    const double len=manifold::la::length(axis);const V2 d=axis/len,b(-d.y,d.x);
    const V2 centre=edge?d*(.32*L):V2(0.);
    manifold::SimplePolygon ring;ring.reserve(size_t(count));
    for(int i=0;i<count;++i){
        const double a=6.283185307179586*double(i)/count;
        const V2 e(std::cos(a)*.5*L,std::sin(a)*.5*W);
        const V q(e.x/ns,e.y/ns,0.);
        const double wave=(Field(q+offset,seed^key)-.5)*2.;
        const double radius=std::clamp(1.+strength*.85*wave,.5,1.5);
        ring.push_back(centre+(d*e.x+b*e.y)*radius);
    }
    return {ring};
}
}
