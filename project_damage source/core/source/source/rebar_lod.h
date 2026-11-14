// общие объявления: уменьшаем число граней арматуры, сохраняя её основной профиль
#pragma once
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>

inline int RebarLodSides(int original, double q) {
    return 4 + int(std::lround((std::max(4,original)-4)*std::clamp(q,0.0,1.0)));
}

template<class V>
std::vector<size_t> RebarLodIndices(const std::vector<V>& input, double radius, double q, bool loop=false) {
    q=std::clamp(q,0.0,1.0);
    std::vector<size_t> all(input.size());std::iota(all.begin(),all.end(),0);
    if(q>=1.0 || input.size()<3)return all;
    auto points=input;if(loop)points.push_back(input.front());
    const size_t n=points.size();std::vector<double> arc(n,0.0);
    auto dist2=[](const V& a,const V& b){double v=0;for(int k=0;k<3;++k)v+=(a[k]-b[k])*(a[k]-b[k]);return v;};
    for(size_t i=1;i<n;++i)arc[i]=arc[i-1]+std::sqrt(dist2(points[i],points[i-1]));
    const double tolerance=std::max(1e-8,radius*0.25)*(1.0-q)*(1.0-q);
    const double maxArc=q>0.0?arc.back()/double(n-1)/q:arc.back()+1.0;
    std::vector<char> keep(n,0);keep[0]=keep[n-1]=1;
    std::vector<std::pair<size_t,size_t>> todo{{0,n-1}};
    while(!todo.empty()) {
        auto [a,b]=todo.back();todo.pop_back();if(b<=a+1)continue;
        double len2=dist2(points[a],points[b]),error=-1.0;size_t split=a+1;
        for(size_t i=a+1;i<b;++i) {
            double dot=0.0;for(int k=0;k<3;++k)dot+=(points[i][k]-points[a][k])*(points[b][k]-points[a][k]);
            double t=len2>1e-20?std::clamp(dot/len2,0.0,1.0):0.0,d=0.0;
            for(int k=0;k<3;++k){double v=points[i][k]-points[a][k]-t*(points[b][k]-points[a][k]);d+=v*v;}
            if(d>error){error=d;split=i;}
        }
        if(error<=tolerance*tolerance) {
            if(arc[b]-arc[a]<=maxArc)continue;
            split=a+(b-a)/2;
        }
        keep[split]=1;todo.emplace_back(a,split);todo.emplace_back(split,b);
    }
    std::vector<size_t> result;for(size_t i=0;i<input.size();++i)if(keep[i])result.push_back(i);
    return result;
}

template<class T>
void RebarLodSelect(std::vector<T>& values,const std::vector<size_t>& ids) {
    std::vector<T> result;result.reserve(ids.size());for(auto i:ids)result.push_back(values[i]);values.swap(result);
}
