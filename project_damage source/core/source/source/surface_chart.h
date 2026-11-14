// общие объявления: раскладываем поверхность на участки для устойчивого размещения деталей
#pragma once
#include <array>
#include <queue>
#include <unordered_map>
#include <limits>
#include <cmath>

PROJECT_DAMAGE_NS_MACRO_BEGIN
struct SurfaceChartIndex {
    std::vector<EVec> centers;
    std::vector<std::vector<int32_t>> adjacent;
    void Build(const MeshView& mesh) {
        centers.resize(mesh.npolys);adjacent.resize(mesh.npolys);
        std::unordered_map<uint64_t,int32_t> edges;
        for(int32_t i=0;i<mesh.npolys;++i) {
            const auto& f=mesh.polys[i];
            const int32_t ids[4]={f.a,f.b,f.c,f.d};const int count=f.c==f.d?3:4;
            EVec center(0.0);
            for(int j=0;j<count;++j)center+=mesh.pts[ids[j]];
            centers[i]=center/double(count);
            for(int j=0;j<count;++j) {
                const auto a=uint32_t(std::min(ids[j],ids[(j+1)%count]));
                const auto b=uint32_t(std::max(ids[j],ids[(j+1)%count]));
                const uint64_t key=(uint64_t(a)<<32)|b;
                auto old=edges.emplace(key,i);
                if(!old.second) {adjacent[i].push_back(old.first->second);adjacent[old.first->second].push_back(i);}
            }
        }
    }
    std::vector<char> Select(const EVec& origin,const EVec& normal,double radius,const std::vector<EVec>& normals) const {
        std::vector<char> allowed(centers.size(),0);if(centers.empty())return allowed;
        int32_t seed=-1;double best=std::numeric_limits<double>::infinity();
        for(size_t i=0;i<centers.size();++i) {
            if(manifold::la::dot(normals[i],normal)<0.3)continue;
            double d=manifold::la::length(centers[i]-origin);
            if(d<best) {seed=int32_t(i);best=d;}
        }
        if(seed<0)return allowed;
        using Item=std::pair<double,int32_t>;
        std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        std::vector<double> distance(centers.size(),std::numeric_limits<double>::infinity());
        distance[seed]=0;queue.emplace(0,seed);
        while(!queue.empty()) {
            auto [d,i]=queue.top();queue.pop();if(d!=distance[i])continue;
            allowed[i]=1;
            for(int32_t j:adjacent[i]) {
                if(manifold::la::dot(normals[i],normals[j])<0.2 || manifold::la::dot(normals[j],normal)<0.3)continue;
                const double next=d+manifold::la::length(centers[j]-centers[i]);
                if(next<=radius+best && next<distance[j]) {distance[j]=next;queue.emplace(next,j);}
            }
        }
        return allowed;
    }
};

template<class Grid> void ConnectSurfaceChart(Grid& g,double u0,double v0,double cell,double du,double dv,const std::vector<double>* us=nullptr,const std::vector<double>* vs=nullptr) {
    int seed=-1;double nearest=std::numeric_limits<double>::infinity();
    for(int i=0;i<=g.nu;++i)for(int j=0;j<=g.nv;++j) {
        const auto k=g.I(i,j);if(!g.valid[k])continue;
        const double u=us?(*us)[size_t(i)]:u0+i*du,v=vs?(*vs)[size_t(j)]:v0+j*dv,d=u*u+v*v;
        if(d<nearest){nearest=d;seed=int(k);}
    }
    if(seed<0)return;
    std::vector<char> keep(g.valid.size(),0);std::queue<int> pending;keep[seed]=1;pending.push(seed);
    const int stride=g.nv+1;
    while(!pending.empty()) {
        int k=pending.front();pending.pop();int i=k/stride,j=k%stride;
        const int near[4]={i>0?k-stride:-1,i<g.nu?k+stride:-1,j>0?k-1:-1,j<g.nv?k+1:-1};
        for(int q:near)if(q>=0&&!keep[q]&&g.valid[q]&&std::abs(g.h[q]-g.h[k])<=4.0*cell) {keep[q]=1;pending.push(q);}
    }
    g.valid.swap(keep);
}
PROJECT_DAMAGE_NS_MACRO_END
