// общие объявления: определяем направление поверхности без влияния мелкого рельефа
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "project_damage_types.h"
#include "project_damage_bvh.h"
#include <vector>
#include <map>
#include <algorithm>

                                                                         
                                                                         
class SurfaceGuidance : public SurfaceDirectionField
{
public:
	std::vector<EVec> points, normals, vertexNormals;
	void Bind(MeshView& view)
	{
		if (!view.pts || !view.polys || view.npts <= 0 || view.npolys <= 0) return;
		points.assign(view.pts, view.pts + view.npts);
		const size_t nv = points.size(), nf = size_t(view.npolys);
		struct Edge { int count = 0; int first = -1, second = -1; };
		std::map<std::pair<int,int>, Edge> edges;
		std::vector<std::vector<int>> adjacent(nv), faceAdjacent(nf);
		std::vector<bool> pinned(nv, false);
		std::vector<double> areas(nf), limit(nv);
		normals.resize(nf); vertexNormals.resize(nv);
		EVec lo=points[0], hi=lo;
		for (const auto& p:points) { lo=m::la::min(lo,p); hi=m::la::max(hi,p); }
		const double radius=std::max(1e-12,0.02*m::la::length(hi-lo));
		auto crossFace = [&](size_t f, const std::vector<EVec>& pts) {
			const auto& q=view.polys[f];
			EVec n=m::la::cross(pts[q.b]-pts[q.a],pts[q.c]-pts[q.a]);
			if(q.c!=q.d)n+=m::la::cross(pts[q.c]-pts[q.a],pts[q.d]-pts[q.a]);
			return n;
		};
		for (size_t f=0;f<nf;++f)
		{
			const auto& q=view.polys[f]; const int v[4]={q.a,q.b,q.c,q.d}; const int count=q.c==q.d?3:4;
			const EVec n=crossFace(f,points); areas[f]=m::la::length(n); normals[f]=Unit(n);
			for(int k=0;k<count;++k)
			{
				auto& edge=edges[std::minmax(v[k],v[(k+1)%count])];
				if(edge.count==0)edge.first=int(f);else edge.second=int(f);++edge.count;
			}
		}
		for(const auto& item:edges)
		{
			const int a=item.first.first,b=item.first.second;const auto& e=item.second;
			adjacent[a].push_back(b);adjacent[b].push_back(a);
			if(e.count!=2 || m::la::length(points[a]-points[b])>2*radius) pinned[a]=pinned[b]=true;
			if(e.count==2 && m::la::dot(normals[e.first],normals[e.second])>0.25)
			{faceAdjacent[e.first].push_back(e.second);faceAdjacent[e.second].push_back(e.first);}
			else pinned[a]=pinned[b]=true;
		}
		for(size_t v=0;v<nv;++v)
		{
			double sum=0;for(int n:adjacent[v])sum+=m::la::length(points[v]-points[n]);
			limit[v]=std::min(radius,adjacent[v].empty()?0.0:sum/adjacent[v].size());
		}
		std::vector<EVec> next(nv);
		for(int pass=0;pass<10;++pass)
		{
			std::fill(vertexNormals.begin(),vertexNormals.end(),EVec(0.0));
			for(size_t f=0;f<nf;++f)
			{
				const auto& q=view.polys[f];const EVec n=crossFace(f,points);
				vertexNormals[q.a]+=n;vertexNormals[q.b]+=n;vertexNormals[q.c]+=n;if(q.d!=q.c)vertexNormals[q.d]+=n;
			}
			next=points;
			for(size_t v=0;v<nv;++v)
			{
				if(pinned[v]||adjacent[v].empty())continue;
				EVec sum(0.0);for(int n:adjacent[v])sum+=points[n];
				const EVec n=Unit(vertexNormals[v]);
				next[v]+=n*(0.45*m::la::dot(sum/double(adjacent[v].size())-points[v],n));
				const EVec d=next[v]-view.pts[v];const double length=m::la::length(d);
				if(length>limit[v])next[v]=view.pts[v]+d*(limit[v]/length);
			}
			bool valid=false;
			for(int trial=0;trial<8;++trial)
			{
				valid=true;
				for(size_t f=0;f<nf;++f)
				{
					const auto& q=view.polys[f];const int ids[4]={q.a,q.b,q.c,q.d};
					for(int k=1;k<(q.c==q.d?2:3);++k)
					{
						const EVec a=m::la::cross(points[ids[k]]-points[ids[0]],points[ids[k+1]]-points[ids[0]]);
						const EVec b=m::la::cross(next[ids[k]]-next[ids[0]],next[ids[k+1]]-next[ids[0]]);
						if(m::la::length(a)>1e-14 && m::la::dot(b,Unit(a))<0.2*m::la::length(a)){valid=false;break;}
					}
					if(!valid)break;
				}
				if(valid)break;
				for(size_t v=0;v<nv;++v)next[v]=(next[v]+points[v])*0.5;
			}
			if(!valid)break;
			points.swap(next);
		}
		for(size_t f=0;f<nf;++f)normals[f]=Unit(crossFace(f,points));
		                                                                    
		                                                                  
		std::vector<EVec> filtered(nf);
		for(int pass=0;pass<4;++pass)
		{
			for(size_t f=0;f<nf;++f)
			{
				EVec sum=normals[f]*std::max(areas[f],1e-20);
				for(int n:faceAdjacent[f])sum+=normals[n]*std::max(areas[n],1e-20);
				filtered[f]=Unit(sum);
			}
			normals.swap(filtered);
		}
		std::fill(vertexNormals.begin(),vertexNormals.end(),EVec(0.0));
		for(size_t f=0;f<nf;++f)
		{
			const auto& q=view.polys[f];const EVec n=normals[f]*areas[f];
			vertexNormals[q.a]+=n;vertexNormals[q.b]+=n;vertexNormals[q.c]+=n;if(q.d!=q.c)vertexNormals[q.d]+=n;
		}
		for(auto& n:vertexNormals)n=Unit(n);
		probe=radius; bvh.Build(view);
		view.guideNormals=normals.data();view.guideVertexNormals=vertexNormals.data();view.guidePoints=points.data();view.guideField=this;
	}
	EVec Near(const EVec& p,const EVec& exact) const override
	{
		if(probe<=0 || m::la::length(exact)<1e-12)return exact;
		const EVec n=Unit(exact);double best=probe;int poly=-1;
		for(int sign:{-1,1})
		{
			double d;int hit=-1;const EVec ray=n*double(sign);
			if(bvh.Raycast(p-ray*probe,ray,probe*2,&d,&hit) && hit>=0)
			{
				const double distance=std::abs(d-probe);
				if(distance<best && m::la::dot(normals[hit],n)>0.25){best=distance;poly=hit;}
			}
		}
		return poly>=0?normals[poly]:exact;
	}
private:
	ProjectDamageBVH bvh;
	double probe=0.0;
	static EVec Unit(const EVec& n){const double l=m::la::length(n);return l>1e-20?n/l:EVec(0,0,1);}
};
