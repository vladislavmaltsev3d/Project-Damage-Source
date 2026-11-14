// разрываем тонкую ткань и сохраняем внешний край отверстия
// ProjectDamage flat fabric tears — GPL-3.0-or-later; see LICENSE.
//
// Recipe fabric-flat-v1 (FabricTearParams::flat, the default). The cloth is
// treated as a SURFACE, not a solid: no Booleans, no thickness, works on an
// open single-layer mesh (cloth without Solidify) as well as on a closed one.
//   1. Tears: a ragged lens per tear in the tangent plane at a random,
//      area-weighted surface point - along the weave (holeDir) or random -
//      with a wandering centre line (Meander), low bulges (Lobes) and a
//      scalloped rim (Jagged), split into 1..holeSplitMax holes separated by
//      fabric bridges (holeBridge).
//   2. Torn edges: V-shaped slits running in from the open border of the
//      sheet (boundary edges), evenly spread, wide at the border, narrowing
//      to the tip.
//   3. Cutting: every triangle near an outline whose plane faces it is mapped
//      into the outline's 2D frame, the outlines are subtracted
//      (CrossSection = Clipper2), the rest is triangulated and lifted back
//      with the barycentrics of the source triangle - points stay exactly on
//      the surface, UVs are interpolated, the source polygon is kept for the
//      material. Untouched polygons are passed on as they are. A triangle far
//      from the outline plane (another layer, a fold) is never cut.
//   4. Threads (same material): bridges spanning a hole, sagging along
//      gravity; broken threads dangling from both rims; short fray stubs
//      along every rim; long strands hanging from the border slits.
//   Tear Shape Realistic (tearStyle 1, 2026-10-28): the outlines, flaps and
//   yarns come from fabric_tear_shapes.h (rips along the grain, corner
//   tears, worn holes, punctures; stair-stepped yarn-end rims). The flap of a
//   corner tear is the torn-out cloth itself, kept and folded back over its
//   hinge, so it keeps the source UVs and material.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "manifold/polygon.h"
#include "fabric_flat.h"
#include "fabric_tear_shapes.h"
#include "project_damage_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "fabric_rim_topology.h"

namespace m = manifold;
using namespace project_damage;                          
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
using V2 = m::vec2;

double R01(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
                                 
double Noise1(double x, uint64_t seed, uint64_t ch)
{
	const double f = std::floor(x), t = x - f;
	const double a = R01(seed, uint64_t(int64_t(f) + (1 << 20)), ch) * 2 - 1;
	const double b = R01(seed, uint64_t(int64_t(f) + 1 + (1 << 20)), ch) * 2 - 1;
	const double s = t * t * (3 - 2 * t);
	return a + (b - a) * s;
}
V Unit(const V& v, const V& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; }

                                                                           
struct Outline { V o, t, b, n; m::Polygons polys; double reach = 0; std::vector<std::pair<V2, V2>> rims; int kind = 0; size_t tri = 0;
	bool real = false; std::vector<fabric_shapes::Flap> flaps; std::vector<fabric_shapes::Yarn> yarns; };

                                                                  
struct WTri { V p[3]; V2 uv[3]; int32_t poly; bool hasUv; size_t src; };

                                                           
bool RayTri(const V& q, const V& n, const V& a, const V& b, const V& c, double& s)
{
	const V e1 = b - a, e2 = c - a;
	const V pv = m::la::cross(n, e2);
	const double det = m::la::dot(e1, pv);
	if (std::abs(det) < 1e-18) return false;
	const double inv = 1.0 / det;
	const V tv = q - a;
	const double u = m::la::dot(tv, pv) * inv;
	if (u < -1e-9 || u > 1 + 1e-9) return false;
	const V qv = m::la::cross(tv, e1);
	const double v = m::la::dot(n, qv) * inv;
	if (v < -1e-9 || u + v > 1 + 1e-9) return false;
	s = m::la::dot(e2, qv) * inv;
	return true;
}

}               

bool BuildFlatFabricLegacy(const CoreInput& in, const ProjectDamageParams& params, const ProjectDamageHost& host, CoreResult& out);

// основной вход: собираем геометрию по переданным настройкам
bool BuildFlatFabric(const CoreInput& in, const ProjectDamageParams& params, const ProjectDamageHost& host, CoreResult& out)
{
    if (!params.fabric.wovenThreads) return BuildFlatFabricLegacy(in,params,host,out);
	const FabricTearParams& p = params.fabric;
	const MeshView& mv = in.view;
	if (mv.npolys <= 0) return false;
	const uint64_t seed = (uint64_t(uint32_t(params.seed)) ^ 0x46414252494331ULL) ^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const bool haveUv = in.cornerUvs != nullptr && in.cornerUvs->size() >= size_t(mv.npolys) * 8;
	auto uvOf = [&](Int32 poly, int corner) {
		if (!haveUv) return V2(0.0);
		const double* u = &(*in.cornerUvs)[size_t(poly) * 8 + size_t(corner) * 2];
		return V2(u[0], u[1]);
	};
	                                                                                    
	struct STri { Int32 v[3]; int c[3]; Int32 poly; double area; V n; };
	std::vector<STri> tris;
	double totalArea = 0.0;
	V lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		const int n = q.c == q.d ? 3 : 4;
		for (int k = 1; k + 1 < n; ++k)
		{
			STri t{ { ids[0], ids[k], ids[k + 1] }, { 0, k, k + 1 }, i, 0.0, V(0.0) };
			const V cr = m::la::cross(V(mv.pts[t.v[1]]) - V(mv.pts[t.v[0]]), V(mv.pts[t.v[2]]) - V(mv.pts[t.v[0]]));
			t.area = 0.5 * m::la::length(cr);
			t.n = Unit(cr, V(0, 0, 1));
			totalArea += t.area;
			tris.push_back(t);
		}
	}
	for (Int32 i = 0; i < mv.npts; ++i) { lo = m::la::min(lo, V(mv.pts[i])); hi = m::la::max(hi, V(mv.pts[i])); }
	if (tris.empty() || !(totalArea > 0.0)) return false;
	const double diag = m::la::length(hi - lo);
	auto avoided = [&](Int32 poly) {
		return p.avoid && in.avoidPoly != nullptr && (size_t)poly < in.avoidPoly->size() && (*in.avoidPoly)[(size_t)poly];
	};
	std::vector<double> cdf;
	double acc = 0.0;
	for (const STri& t : tris) { acc += avoided(t.poly) ? 0.0 : t.area; cdf.push_back(acc); }
    auto samplePoint = [&](uint64_t base, V& pos, V& nrm, size_t& ti) {
        if (!(acc>0.))return false;
        V bestPos(0.),bestN(0.);size_t bestTri=0;double bestWeight=-1.;
        for(int attempt=0;attempt<32;++attempt){
            const uint64_t k=base+uint64_t(attempt)*100003;
            const double r=R01(seed,k,1)*acc;
            const size_t index=size_t(std::upper_bound(cdf.begin(),cdf.end(),r)-cdf.begin());
            if(index>=tris.size())continue;
            double u=R01(seed,k,2),v=R01(seed,k,3);if(u+v>1.){u=1.-u;v=1.-v;}
            const STri& t=tris[index];
            const V q=V(mv.pts[t.v[0]])*(1.-u-v)+V(mv.pts[t.v[1]])*u+V(mv.pts[t.v[2]])*v;
            const double weight=fabric_noise::Weight(p,q,seed);
            if(weight>bestWeight){bestWeight=weight;bestPos=q;bestN=GuidedNormal(mv,t.poly,t.n);bestTri=index;}
            if(R01(seed,k,81)<=weight){pos=q;nrm=GuidedNormal(mv,t.poly,t.n);ti=index;return true;}
        }
        if(bestWeight<0.)return false;
        pos=bestPos;nrm=bestN;ti=bestTri;return true;
    };
	const double rag = std::max(1e-6, 0.05 * diag);
	std::vector<Outline> outlines;

	                                                                                        
	if (p.holesEnable && !p.paintOnly)
		for (int h = 0; h < std::max(0, p.holeAmount); ++h)
		{
			const uint64_t hk = 1000 + uint64_t(h) * 97;
			V pos, n; size_t ti;
			if (!samplePoint(hk, pos, n, ti)) continue;
			V dir = p.holeDirMode == 1 ? V(p.holeDirX, p.holeDirY, p.holeDirZ)
				: V(R01(seed, hk, 4) - 0.5, R01(seed, hk, 5) - 0.5, R01(seed, hk, 6) - 0.5);
			dir = dir - n * m::la::dot(dir, n);
			if (m::la::length(dir) < 1e-6) dir = m::la::cross(n, std::abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0));
			dir = Unit(dir, V(1, 0, 0));
			const double jit = (R01(seed, hk, 7) * 2 - 1) * p.holeDirJitter * m::kPi / 180.0;
			const V b0 = m::la::cross(n, dir);
			dir = Unit(dir * std::cos(jit) + b0 * std::sin(jit), dir);
			Outline o;
			o.o = pos; o.n = n; o.t = dir; o.b = m::la::cross(n, dir); o.tri = ti;
			const double L = Lerp(p.holeLengthMin, p.holeLengthMax, R01(seed, hk, 8));
			const double W = Lerp(p.holeWidthMin, p.holeWidthMax, R01(seed, hk, 9));
			o.reach = 0.6 * L + W;
			if (p.tearStyle >= 1)
			{
				                                                         
				fabric_shapes::Shape sh = fabric_shapes::MakeTearShape(p, fabric_shapes::PickKind(p, R01(seed, hk, 11)), L, W, seed, hk);
				if (sh.holes.empty()) continue;
				o.polys = std::move(sh.holes);
				o.flaps = std::move(sh.flaps);
				o.yarns = std::move(sh.yarns);
				o.reach = sh.reach;
				o.real = true;
				outlines.push_back(std::move(o));
				continue;
			}
			                                  
			const int nSplit = 1 + int(R01(seed, hk, 10) * std::max(1, p.holeSplitMax));
			std::vector<std::pair<double, double>> spans;                       
			{
				std::vector<double> cuts = { -0.5 * L, 0.5 * L };
				for (int s = 1; s < nSplit; ++s) cuts.insert(cuts.end() - 1, -0.5 * L + L * (s + (R01(seed, hk, 20 + s) - 0.5) * 0.4) / nSplit);
				for (size_t s = 0; s + 1 < cuts.size(); ++s)
				{
					const double a0 = cuts[s] + (s > 0 ? 0.5 * p.holeBridge : 0.0);
					const double a1 = cuts[s + 1] - (s + 2 < cuts.size() ? 0.5 * p.holeBridge : 0.0);
					if (a1 - a0 > 0.15 * W) spans.push_back({ a0, a1 });
				}
			}
			const int nU = std::max(24, int(L / std::max(0.05, 0.12 * W / std::max(0.05, p.detail))));
			auto centreV = [&](double u) { return p.holeMeander * W * 1.2 * Noise1(u / std::max(1e-6, 0.4 * L) * 2.0, seed ^ hk, 31); };
			auto halfW = [&](double u, double u0, double u1, int side) {
				const double s = (u - u0) / std::max(1e-9, u1 - u0) * 2.0 - 1.0;                                   
				const double lens = std::pow(std::max(0.0, 1.0 - s * s), 0.65);
				const double lobes = 1.0 + p.holeLobes * 0.6 * Noise1(u / std::max(1e-6, 0.25 * L), seed ^ hk, 41 + side);
				const double jag = 1.0 + p.holeJagged * 0.45 * (Noise1(u / std::max(1e-6, 0.035 * L), seed ^ hk, 51 + side)
					+ 0.5 * Noise1(u / std::max(1e-6, 0.012 * L), seed ^ hk, 61 + side));
				return 0.5 * W * lens * std::max(0.05, lobes) * std::max(0.1, jag);
			};
			for (const auto& sp : spans)
			{
				m::SimplePolygon poly;
				std::vector<V2> top, bot;
				for (int i = 0; i <= nU; ++i)
				{
					const double u = Lerp(sp.first, sp.second, double(i) / nU);
					const double c = centreV(u);
					top.push_back(V2(u, c + halfW(u, sp.first, sp.second, 0)));
					bot.push_back(V2(u, c - halfW(u, sp.first, sp.second, 1)));
				}
				for (const V2& q : bot) poly.push_back(q);
				for (int i = (int)top.size() - 1; i >= 0; --i) poly.push_back(top[(size_t)i]);
				o.polys.push_back(poly);
				                                         
				for (int i = 1; i < nU; ++i) o.rims.push_back({ bot[(size_t)i], top[(size_t)i] });
			}
			o.kind = 0;
			if (!o.polys.empty()) outlines.push_back(std::move(o));
		}

	                                                                                            
	std::vector<std::pair<Int32, Int32>> border;                                              
	std::vector<size_t> borderTri;
	{
		std::unordered_map<uint64_t, int> use;
		auto key = [](Int32 a, Int32 b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
		for (const STri& t : tris) for (int k = 0; k < 3; ++k) use[key(t.v[k], t.v[(k + 1) % 3])]++;
		for (size_t i = 0; i < tris.size(); ++i)
			for (int k = 0; k < 3; ++k)
			{
				const Int32 a = tris[i].v[k], b = tris[i].v[(k + 1) % 3];
				if (!use.count(key(b, a))) { border.push_back({ a, b }); borderTri.push_back(i); }
			}
	}
	if (!p.paintOnly && p.edgeEnable && !border.empty() && p.edgeAmount > 0)
	{
		std::vector<double> bl;
		double bTotal = 0.0;
		for (const auto& e : border) { bTotal += m::la::length(V(mv.pts[e.second]) - V(mv.pts[e.first])); bl.push_back(bTotal); }
		for (int s = 0; s < p.edgeAmount; ++s)
		{
			const uint64_t sk = 50000 + uint64_t(s) * 131;
			const double at = bTotal * (s + 0.5 + (R01(seed, sk, 1) - 0.5) * p.edgeSpacingJitter) / p.edgeAmount;
			const size_t ei = std::min(border.size() - 1, (size_t)(std::lower_bound(bl.begin(), bl.end(), at) - bl.begin()));
			const V a = mv.pts[border[ei].first], b = mv.pts[border[ei].second];
			const double el = m::la::length(b - a);
			const double startL = bl[ei] - el;
			const V pos = a + (b - a) * std::clamp((at - startL) / std::max(1e-12, el), 0.0, 1.0);
			const STri& t = tris[borderTri[ei]];
			if (avoided(t.poly)) continue;
			const V n = t.n;
			const V eDir = Unit(b - a, V(1, 0, 0));
			V inward = m::la::cross(n, eDir);                                                  
			const double jit = (R01(seed, sk, 2) * 2 - 1) * p.edgeAngleJitter * m::kPi / 180.0;
			inward = Unit(inward * std::cos(jit) + eDir * std::sin(jit), inward);
			Outline o;
			o.o = pos; o.n = n; o.t = inward; o.b = m::la::cross(n, inward); o.tri = borderTri[ei];
			const double L = Lerp(p.edgeLengthMin, p.edgeLengthMax, R01(seed, sk, 3));
			const double W = Lerp(p.edgeWidthMin, p.edgeWidthMax, R01(seed, sk, 4));
			o.reach = L + W;
			if (p.tearStyle >= 1)
			{
				                                                                  
				                                                              
				V g = p.holeDirMode == 1 ? V(p.holeDirX, p.holeDirY, p.holeDirZ) : inward;
				g = g - n * m::la::dot(g, n);
				g = Unit(g, inward);
				o.t = g; o.b = m::la::cross(n, g);
				V2 in2(m::la::dot(inward, o.t), m::la::dot(inward, o.b));
				if (R01(seed, sk, 9) < p.grainSnap)
					in2 = std::abs(in2.x) >= std::abs(in2.y) ? V2(in2.x > 0 ? 1.0 : -1.0, 0.0) : V2(0.0, in2.y > 0 ? 1.0 : -1.0);
				const double l2 = m::la::length(in2);
				in2 = l2 > 1e-9 ? in2 / l2 : V2(1, 0);
				fabric_shapes::Shape sh = fabric_shapes::MakeTearShape(p, fabric_shapes::kEdge, L, W, seed, sk, in2);
				if (sh.holes.empty()) continue;
				o.polys = std::move(sh.holes);
				o.yarns = std::move(sh.yarns);
				o.reach = sh.reach;
				o.real = true;
				o.kind = 1;
				outlines.push_back(std::move(o));
				continue;
			}
			const int nU = std::max(16, int(L / std::max(0.05, 0.2 * W / std::max(0.05, p.detail))));
			std::vector<V2> top, bot;
			for (int i = 0; i <= nU; ++i)
			{
				const double f = double(i) / nU;
				const double u = -0.3 * W + (L + 0.3 * W) * f;                                                   
				const double c = p.edgeMeander * W * 2.0 * Noise1(u / std::max(1e-6, 0.3 * L), seed ^ sk, 5) * f;
				const double w = 0.5 * W * std::pow(std::max(0.0, 1.0 - std::max(0.0, u) / L), std::max(0.2, p.edgeTaper));
				const double j0 = 1.0 + p.edgeJagged * 0.5 * Noise1(u / std::max(1e-6, 0.05 * L), seed ^ sk, 6);
				const double j1 = 1.0 + p.edgeJagged * 0.5 * Noise1(u / std::max(1e-6, 0.05 * L), seed ^ sk, 7);
				top.push_back(V2(u, c + w * std::max(0.05, j0)));
				bot.push_back(V2(u, c - w * std::max(0.05, j1)));
			}
			m::SimplePolygon poly;
			for (const V2& q : bot) poly.push_back(q);
			for (int i = (int)top.size() - 1; i >= 0; --i) poly.push_back(top[(size_t)i]);
			o.polys.push_back(poly);
			for (int i = 1; i < nU; i += 2) o.rims.push_back({ bot[(size_t)i], top[(size_t)i] });
			o.kind = 1;
			outlines.push_back(std::move(o));
		}
	}
	                                                                         
	                                                                         
	auto addDrawn = [&](const std::vector<SurfacePaintRegion>* regions, int kind) {
		if (!regions) return;
		for (const auto& region : *regions) {
			if (region.points.size() < 3) continue;
			V centre(0.0);
			for (const auto& point : region.points) centre += V(point);
			centre /= double(region.points.size());
			const V normal=Unit(V(region.normal),V(0,0,1));
			const double projection=std::max(1e-3,params.surfPaintSnap);
			double best=1e300, shift=0.0; size_t ti=tris.size();
			for (size_t i=0;i<tris.size();++i) {
				const auto& tri=tris[i]; double distance=0.0;
				if (avoided(tri.poly) || m::la::dot(tri.n,normal)<0.17) continue;
				if (RayTri(centre,normal,V(mv.pts[tri.v[0]]),V(mv.pts[tri.v[1]]),V(mv.pts[tri.v[2]]),distance)
					&& std::abs(distance)<=projection && std::abs(distance)<best) {
					best=std::abs(distance);shift=distance;ti=i;
				}
			}
			if (ti==tris.size()) continue;
			Outline o;o.o=centre+normal*shift;o.n=tris[ti].n;o.tri=ti;o.kind=kind;
			V tangent=p.holeDirMode == 1 ? V(p.holeDirX,p.holeDirY,p.holeDirZ) : V(region.points[1])-V(region.points[0]);
			tangent-=o.n*m::la::dot(tangent,o.n);
			o.t=Unit(tangent,Unit(m::la::cross(o.n,V(0,1,0)),V(1,0,0)));
			o.b=Unit(m::la::cross(o.n,o.t),V(0,1,0));
			m::SimplePolygon poly;
			for (size_t i=0;i<region.points.size();++i) {
				const V delta=V(region.points[i])-o.o;
				V2 q(m::la::dot(delta,o.t),m::la::dot(delta,o.b));
				if (!poly.empty() && m::la::length(q-poly.back())<1e-8) continue;
				poly.push_back(q);o.reach=std::max(o.reach,m::la::length(q));
			}
			if (poly.size()>3 && m::la::length(poly.front()-poly.back())<1e-8) poly.pop_back();
			if (poly.size()<3) continue;
			o.polys=m::CrossSection({poly},m::CrossSection::FillRule::EvenOdd).ToPolygons();
			if (o.polys.empty()) continue;
            fabric_shapes::Shape sh;
            sh.pitch = std::max(2.5 * std::max(1e-5,p.threadRadius), p.yarnSize);
            sh.holes = fabric_shapes::Comb(o.polys, sh.pitch,
                std::max(0.0,p.paintJagged) * std::max(0.001,p.paintRagSize) * 0.25,
                seed, 0x44524157ULL + outlines.size());
            if (sh.holes.empty()) continue;
            o.polys = sh.holes;
            fabric_shapes::PopulateYarns(sh,p,seed,0x44524157ULL + outlines.size());
            o.yarns = std::move(sh.yarns);
            o.real = true;

			outlines.push_back(std::move(o));
		}
	};
	if (p.paintHolesEnable)addDrawn(in.fabricHoles,0);
	if (p.paintEdgesEnable)addDrawn(in.fabricEdges,1);
	if (outlines.empty()) return false;
	HostStatus(host, "fabric: cutting " + std::to_string(outlines.size()) + " tears", 40);

	                                                                                                 
	                                                                          
	                                                                           
	                                                                          
	                                                                          
	                               
	                                                                          
	                                                                         
	                                                                         
	                                     
	std::unordered_map<uint64_t, std::vector<uint32_t>> edgeTris;
	auto ekey = [](Int32 a, Int32 b) { const Int32 lo2 = std::min(a, b), hi2 = std::max(a, b); return (uint64_t(uint32_t(lo2)) << 32) | uint32_t(hi2); };
	for (size_t i = 0; i < tris.size(); ++i)
		for (int k = 0; k < 3; ++k) edgeTris[ekey(tris[i].v[k], tris[i].v[(k + 1) % 3])].push_back((uint32_t)i);
	auto centroid = [&](size_t i) { return (V(mv.pts[tris[i].v[0]]) + V(mv.pts[tris[i].v[1]]) + V(mv.pts[tris[i].v[2]])) / 3.0; };
	const double layer = std::max(0.0, p.layerDepth);
	std::vector<std::vector<char>> outerOf(outlines.size()), innerOf(outlines.size());
	std::vector<char> touched(tris.size(), 0);
	for (size_t oi = 0; oi < outlines.size(); ++oi)
	{
		const Outline& o = outlines[oi];
		std::vector<char>& in1 = outerOf[oi];
		in1.assign(tris.size(), 0);
		std::vector<size_t> stack = { o.tri };
		in1[o.tri] = 1;
		while (!stack.empty())
		{
			const size_t i = stack.back(); stack.pop_back();
			for (int k = 0; k < 3; ++k)
				for (uint32_t j : edgeTris[ekey(tris[i].v[k], tris[i].v[(k + 1) % 3])])
				{
					if (in1[j] || avoided(tris[j].poly)) continue;
					if (m::la::dot(tris[j].n, o.n) < 0.17) continue;
					if (m::la::length(centroid(j) - o.o) > o.reach + 2.0 * std::sqrt(tris[j].area)) continue;
					in1[j] = 1;
					stack.push_back(j);
				}
		}
		std::vector<char>& in2 = innerOf[oi];
		in2.assign(tris.size(), 0);
		if (layer > 0.0)
			for (size_t i = 0; i < tris.size(); ++i)
			{
				if (in1[i] || avoided(tris[i].poly) || m::la::dot(tris[i].n, o.n) > -0.17) continue;
				const V d = centroid(i) - o.o;
				const double behind = -m::la::dot(d, o.n);
				if (behind < -0.25 * layer || behind > 1.5 * layer) continue;
				if (m::la::length(d + o.n * behind) > o.reach + 2.0 * std::sqrt(tris[i].area)) continue;
				in2[i] = 1;
			}
		for (size_t i = 0; i < tris.size(); ++i) if (in1[i] || in2[i]) touched[i] = 1;
	}
	                                                                             
	std::vector<char> polyTouched((size_t)mv.npolys, 0);
	for (size_t i = 0; i < tris.size(); ++i) if (touched[i]) polyTouched[(size_t)tris[i].poly] = 1;
	std::vector<char> polyCut((size_t)mv.npolys, 0);
	std::vector<WTri> work;
	for (size_t i = 0; i < tris.size(); ++i)
	{
		if (!polyTouched[(size_t)tris[i].poly]) continue;
		WTri w;
		for (int k = 0; k < 3; ++k) { w.p[k] = mv.pts[tris[i].v[k]]; w.uv[k] = uvOf(tris[i].poly, tris[i].c[k]); }
		w.poly = tris[i].poly; w.hasUv = haveUv; w.src = i;
		work.push_back(w);
	}
	                                                                       
	                                                 
	std::vector<std::vector<size_t>> outerIdx(outlines.size()), innerIdx(outlines.size());
	for (size_t oi = 0; oi < outlines.size(); ++oi)
		for (size_t i = 0; i < tris.size(); ++i)
		{
			if (outerOf[oi][i]) outerIdx[oi].push_back(i);
			if (innerOf[oi][i]) innerIdx[oi].push_back(i);
		}
	auto lift = [&](const Outline& o, const std::vector<char>& set, const V2& s2, V& out, int32_t& poly, V2& uv) {
		const V q = o.o + o.t * s2.x + o.b * s2.y;
		double best = 1e300;
		bool found = false;
		const size_t oi = size_t(&o - outlines.data());
		const std::vector<size_t>& idx = &set == &innerOf[oi] ? innerIdx[oi] : outerIdx[oi];
		for (size_t i : idx)
		{
			if (!set[i]) continue;
			const V a = mv.pts[tris[i].v[0]], b = mv.pts[tris[i].v[1]], c = mv.pts[tris[i].v[2]];
			double sh;
			if (!RayTri(q, o.n, a, b, c, sh)) continue;
			if (std::abs(sh) < best)
			{
				best = std::abs(sh);
				out = q + o.n * sh;
				poly = tris[i].poly;
				                 
				const V e1 = b - a, e2 = c - a, ep = out - a;
				const double d00 = m::la::dot(e1, e1), d01 = m::la::dot(e1, e2), d11 = m::la::dot(e2, e2);
				const double d20 = m::la::dot(ep, e1), d21 = m::la::dot(ep, e2), den = d00 * d11 - d01 * d01;
				const double l1 = den != 0 ? (d11 * d20 - d01 * d21) / den : 0, l2 = den != 0 ? (d00 * d21 - d01 * d20) / den : 0;
				uv = uvOf(tris[i].poly, tris[i].c[0]) * (1 - l1 - l2) + uvOf(tris[i].poly, tris[i].c[1]) * l1 + uvOf(tris[i].poly, tris[i].c[2]) * l2;
				found = true;
			}
		}
		return found;
	};
	HostStatus(host, "fabric: cutting " + std::to_string(outlines.size()) + " tears", 40);
	                                                                       
	                                                                     
	std::vector<WTri> flapWork;
	std::vector<int> flapFoldOf;
	struct FlapRef { size_t outline; size_t flap; };
	std::vector<FlapRef> flapRefs;
	for (size_t oi = 0; oi < outlines.size(); ++oi)
	{
		const Outline& o = outlines[oi];
		const m::CrossSection hole(o.polys, m::CrossSection::FillRule::NonZero);
		std::vector<m::CrossSection> flapCs;
		for (size_t fi = 0; fi < o.flaps.size(); ++fi)
		{
			flapCs.push_back(m::CrossSection(o.flaps[fi].poly, m::CrossSection::FillRule::Positive));
			flapRefs.push_back({ oi, fi });
		}
		const int flapBase = (int)flapRefs.size() - (int)o.flaps.size();
		std::vector<WTri> next;
		next.reserve(work.size());
		for (const WTri& w : work)
		{
			if (!outerOf[oi][w.src] && !innerOf[oi][w.src]) { next.push_back(w); continue; }
			V2 q[3];
			for (int k = 0; k < 3; ++k) { const V d = w.p[k] - o.o; q[k] = V2(m::la::dot(d, o.t), m::la::dot(d, o.b)); }
			const double area2 = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[2].x - q[0].x) * (q[1].y - q[0].y);
			if (std::abs(area2) < 1e-14) { next.push_back(w); continue; }
			m::SimplePolygon tp = area2 > 0 ? m::SimplePolygon{ q[0], q[1], q[2] } : m::SimplePolygon{ q[0], q[2], q[1] };
			const m::CrossSection tri(tp);
			const m::CrossSection rest = tri - hole;
			const double restA = rest.Area(), triA = 0.5 * std::abs(area2);
			if (restA > triA * (1.0 - 1e-9)) { next.push_back(w); continue; }               
			polyCut[(size_t)w.poly] = 1;
			auto liftW = [&](const V2& s2, V& pos, V2& uv) {
				const double d = area2;
				const double l1 = ((s2.x - q[0].x) * (q[2].y - q[0].y) - (q[2].x - q[0].x) * (s2.y - q[0].y)) / d;
				const double l2 = ((q[1].x - q[0].x) * (s2.y - q[0].y) - (s2.x - q[0].x) * (q[1].y - q[0].y)) / d;
				const double l0 = 1.0 - l1 - l2;
				pos = w.p[0] * l0 + w.p[1] * l1 + w.p[2] * l2;
				uv = w.uv[0] * l0 + w.uv[1] * l1 + w.uv[2] * l2;
			};
			auto emit = [&](const m::CrossSection& cs, std::vector<WTri>& dst, int fold) {
				const m::Polygons rp = cs.ToPolygons();
				std::vector<V2> flat;
				for (const auto& sp : rp) for (const V2& v : sp) flat.push_back(v);
				std::vector<m::ivec3> ids;
				try { ids = m::Triangulate(rp, 1e-9 * diag); } catch (...) { return false; }
				for (const m::ivec3& tr : ids)
				{
					WTri nw;
					nw.poly = w.poly; nw.hasUv = w.hasUv; nw.src = w.src;
					int order[3] = { 0, 1, 2 };
					const V2 a = flat[(size_t)tr[0]], b = flat[(size_t)tr[1]], c = flat[(size_t)tr[2]];
					const double sa = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
					if ((sa > 0) != (area2 > 0)) std::swap(order[1], order[2]);
					for (int k = 0; k < 3; ++k) liftW(flat[(size_t)tr[order[k]]], nw.p[k], nw.uv[k]);
					dst.push_back(nw);
					if (fold >= 0) flapFoldOf.push_back(fold);
				}
				return true;
			};
			if (outerOf[oi][w.src])
				for (size_t fi = 0; fi < flapCs.size(); ++fi)
				{
					const m::CrossSection piece = tri ^ flapCs[fi];
					if (piece.Area() > triA * 1e-9) emit(piece, flapWork, flapBase + (int)fi);
				}
			if (restA <= triA * 1e-9) continue;                                                   
			if (!emit(rest, next, -1)) next.push_back(w);
		}
		work.swap(next);
	}

	                                                                                              
	struct WallTri { V p[3]; V2 uv[3]; int32_t poly; };
	std::vector<WallTri> walls;
	struct Tube { std::vector<V> path; double r0, r1; int32_t poly; };
	std::vector<Tube> tubes;
	const V gravW = Unit(V(p.gravityX, p.gravityY, p.gravityZ), V(0, 0, -1));
	const double tr = std::max(1e-5, p.threadRadius);
	const int cap = std::max(0, p.maxThreads);
	for (size_t oi = 0; oi < outlines.size(); ++oi)
	{
		const Outline& o = outlines[oi];
		bool anyInner = false;
		for (char c : innerOf[oi]) if (c) { anyInner = true; break; }
		if (anyInner)
			for (const m::SimplePolygon& outline : o.polys)
			{
				const m::SimplePolygon loop = fabric_surface::ConformRim(outline, o.o, o.t, o.b, mv, tris, outerOf[oi], innerOf[oi], 1e-8 * diag + 1e-12);
				const size_t nl = loop.size();
				std::vector<V> po(nl), pi(nl);
				std::vector<V2> uo(nl), ui(nl);
				std::vector<int32_t> pp(nl, -1);
				std::vector<char> ok(nl, 0);
				for (size_t i = 0; i < nl; ++i)
				{
					int32_t p1 = -1, p2 = -1;
					ok[i] = lift(o, outerOf[oi], loop[i], po[i], p1, uo[i]) && lift(o, innerOf[oi], loop[i], pi[i], p2, ui[i]);
					pp[i] = p1;
				}
				for (size_t i = 0; i < nl; ++i)
				{
					const size_t j = (i + 1) % nl;
					if (!ok[i] || !ok[j]) continue;
					                                                                     
					walls.push_back({ { po[i], po[j], pi[j] }, { uo[i], uo[j], ui[j] }, pp[i] });
					walls.push_back({ { po[i], pi[j], pi[i] }, { uo[i], ui[j], ui[i] }, pp[i] });
				}
			}
		if (!p.threadsEnable) continue;
		                                                                        
		const V inward = o.n * -1.0;                                            
		const V gT = Unit(gravW - o.n * m::la::dot(gravW, o.n), o.t);                             
		{
			std::vector<fabric_shapes::YarnPath> paths;
			fabric_shapes::Shape sh;
            if (o.real) sh.yarns = o.yarns;
            else {
                sh.holes = o.polys;
                sh.pitch = std::max(p.yarnSize, 2.5 * tr);
                fabric_shapes::PopulateYarns(sh,p,seed,oi);
            }
			auto liftYarn = [&](const V2& s2, V& pos, int& tag) {
				int32_t pp = -1; V2 uv;
				if (!lift(o, outerOf[oi], s2, pos, pp, uv)) return false;
				tag = pp;
				return true;
			};
			fabric_shapes::YarnPaths(sh, p, seed ^ (0x5941524EULL + oi), liftYarn, o.t, inward, gT, (size_t)std::max(0, cap - (int)tubes.size()), paths);
			for (auto& yp : paths)
			{
				if (yp.tag < 0) continue;
				Tube tb; tb.path = std::move(yp.path); tb.r0 = yp.r0; tb.r1 = yp.r1; tb.poly = yp.tag;
				tubes.push_back(std::move(tb));
			}
			continue;
		}
	}

	                                                                                                  
	fabric_surface::Weld weld(out.points, 1e-7 * diag + 1e-12);
	auto vid = [&](const V& pos) { return weld.Add(pos); };
	m::MeshGL mesh;
	mesh.numProp = 5;
	auto corner = [&](const V& pos, const V2& uv) {
		const Int32 id = (Int32)mesh.NumVert();
		mesh.vertProperties.insert(mesh.vertProperties.end(), { (float)pos.x, (float)pos.y, (float)pos.z, (float)uv.x, (float)uv.y });
		return id;
	};
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		if (polyCut[(size_t)i]) continue;
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		Int32 o4[4], c4[4];
		for (int k = 0; k < 4; ++k) { o4[k] = vid(V(mv.pts[ids[k]])); c4[k] = corner(V(mv.pts[ids[k]]), uvOf(i, k)); }
		out.polys.push_back({ o4[0], o4[1], o4[2], q.c == q.d ? o4[2] : o4[3] });
		out.basePolyOf.push_back(i);
        out.effectIds.push_back(0);
		for (int k = 0; k < 4; ++k) out.propCorner.push_back(q.c == q.d && k == 3 ? NOTOK : c4[k]);
	}
	for (const WTri& w : work)
	{
		if (!polyCut[(size_t)w.poly]) continue;
		Int32 o3[3], c3[3];
		for (int k = 0; k < 3; ++k) { o3[k] = vid(w.p[k]); c3[k] = corner(w.p[k], w.uv[k]); }
		if (o3[0] == o3[1] || o3[1] == o3[2] || o3[0] == o3[2]) continue;
		out.polys.push_back({ o3[0], o3[1], o3[2], o3[2] });
		out.basePolyOf.push_back(w.poly);
        out.effectIds.push_back(9);
		out.propCorner.insert(out.propCorner.end(), { c3[0], c3[1], c3[2], NOTOK });
	}
	                                                                          
	                                                           
	if (!flapWork.empty())
	{
		std::vector<fabric_shapes::Fold> folds(flapRefs.size());
		std::vector<char> foldOk(flapRefs.size(), 0);
		for (size_t f = 0; f < flapRefs.size(); ++f)
		{
			const Outline& o = outlines[flapRefs[f].outline];
			const fabric_shapes::Flap& fl = o.flaps[flapRefs[f].flap];
			V A, B, C; int32_t pp; V2 uv;
			if (!lift(o, outerOf[flapRefs[f].outline], fl.h0, A, pp, uv) || !lift(o, outerOf[flapRefs[f].outline], fl.h1, B, pp, uv)
				|| !lift(o, outerOf[flapRefs[f].outline], fl.tip, C, pp, uv)) continue;
			fabric_shapes::Fold& fd = folds[f];
			fd.H = A;
			fd.a = Unit(B - A, o.t);
			fd.e = Unit((C - A) - fd.a * m::la::dot(C - A, fd.a), o.b);
			fd.n = Unit(m::la::cross(fd.a, fd.e), o.n);
			if (m::la::dot(fd.n, o.n) < 0) fd.n = fd.n * -1.0;
			const double size = m::la::length(B - A);
			fd.R = std::max(0.08 * size, 3.0 * tr);                               
			fd.theta = fl.angle;
			foldOk[f] = 1;
		}
		                                                                      
		                                                                  
		                                                               
		std::vector<int> segsOf(flapRefs.size(), 1);
		for (size_t i = 0; i < flapWork.size(); ++i)
		{
			const int f = flapFoldOf[i];
			if (f < 0 || !foldOk[(size_t)f]) continue;
			const WTri& w = flapWork[i];
			double longest = 0.0;
			for (int k = 0; k < 3; ++k) longest = std::max(longest, (double)m::la::length(w.p[(k + 1) % 3] - w.p[k]));
			const double target = 0.35 * folds[(size_t)f].R;
			int segs = 1;
			while (segs < 32 && longest / segs > target) segs *= 2;
			segsOf[(size_t)f] = std::max(segsOf[(size_t)f], segs);
		}
		for (size_t i = 0; i < flapWork.size(); ++i)
		{
			const int f = flapFoldOf[i];
			if (f < 0 || !foldOk[(size_t)f]) continue;
			const WTri& w = flapWork[i];
			const int ns = segsOf[(size_t)f];
			auto at = [&](int a, int b, V& pos, V2& uv) {
				const double u = double(a) / ns, v = double(b) / ns;
				pos = w.p[0] + (w.p[1] - w.p[0]) * u + (w.p[2] - w.p[0]) * v;
				uv = w.uv[0] + (w.uv[1] - w.uv[0]) * u + (w.uv[2] - w.uv[0]) * v;
				pos = fabric_shapes::FoldPoint(folds[(size_t)f], pos);
			};
			auto emitTri = [&](int a0, int b0, int a1, int b1, int a2, int b2) {
				V pf[3]; V2 uv[3];
				at(a0, b0, pf[0], uv[0]); at(a1, b1, pf[1], uv[1]); at(a2, b2, pf[2], uv[2]);
				Int32 o3[3], c3[3];
				for (int k = 0; k < 3; ++k) { o3[k] = vid(pf[k]); c3[k] = corner(pf[k], uv[k]); }
				if (o3[0] == o3[1] || o3[1] == o3[2] || o3[0] == o3[2]) return;
				out.polys.push_back({ o3[0], o3[1], o3[2], o3[2] });
				out.basePolyOf.push_back(w.poly);
				out.effectIds.push_back(9);
				out.propCorner.insert(out.propCorner.end(), { c3[0], c3[1], c3[2], NOTOK });
			};
			for (int a = 0; a < ns; ++a)
				for (int b = 0; a + b < ns; ++b)
				{
					emitTri(a, b, a + 1, b, a, b + 1);
					if (a + b + 1 < ns) emitTri(a + 1, b, a + 1, b + 1, a, b + 1);
				}
		}
	}
	                                                   
	for (const WallTri& w : walls)
	{
		Int32 o3[3], c3[3];
		for (int k = 0; k < 3; ++k) { o3[k] = vid(w.p[k]); c3[k] = corner(w.p[k], w.uv[k]); }
		if (o3[0] == o3[1] || o3[1] == o3[2] || o3[0] == o3[2] || w.poly < 0) continue;
		out.polys.push_back({ o3[0], o3[1], o3[2], o3[2] });
		out.basePolyOf.push_back(w.poly);
        out.effectIds.push_back(9);
		out.propCorner.insert(out.propCorner.end(), { c3[0], c3[1], c3[2], NOTOK });
	}
	                                                                          
	fabric_surface::StitchRim(out, mesh, 1e-7 * diag + 1e-12);
	const int sides = std::clamp(p.threadSides, 3, 12);
	for (const Tube& tb : tubes)
	{
		if (tb.path.size() < 2 || tb.poly < 0) continue;
		const size_t ns = tb.path.size();
		std::vector<std::vector<Int32>> rings(ns);
		V prevN = m::la::cross(Unit(tb.path[1] - tb.path[0], V(1, 0, 0)), V(0, 0, 1));
		if (m::la::length(prevN) < 1e-6) prevN = V(1, 0, 0);
		const Int32 startPt = (Int32)out.points.size();
		for (size_t i = 0; i < ns; ++i)
		{
			const V tdir = Unit(tb.path[std::min(ns - 1, i + 1)] - tb.path[i > 0 ? i - 1 : 0], V(1, 0, 0));
			V nx = Unit(prevN - tdir * m::la::dot(prevN, tdir), m::la::cross(tdir, V(0, 0, 1)));
			const V ny = m::la::cross(tdir, nx);
			prevN = nx;
			const double r = Lerp(tb.r0, tb.r1, double(i) / (ns - 1));
			for (int s = 0; s < sides; ++s)
			{
				const double ang = 2.0 * m::kPi * s / sides;
				const V pp = tb.path[i] + (nx * std::cos(ang) + ny * std::sin(ang)) * r;
				rings[i].push_back((Int32)out.points.size());
				out.points.push_back(EVec(pp.x, pp.y, pp.z));
			}
		}
		const Int32 capA = (Int32)out.points.size(); out.points.push_back(tb.path.front());
		const Int32 capB = (Int32)out.points.size(); out.points.push_back(tb.path.back());
		(void)startPt;
		auto quad = [&](Int32 a, Int32 b, Int32 c, Int32 d) {
			out.polys.push_back({ a, b, c, d });
			out.basePolyOf.push_back(tb.poly);
            out.effectIds.push_back(9);
			for (int k = 0; k < 4; ++k) out.propCorner.push_back(corner(V(out.points[(size_t)a]), uvOf(tb.poly, 0)));
		};
		auto tri3 = [&](Int32 a, Int32 b, Int32 c) {
			out.polys.push_back({ a, b, c, c });
			out.basePolyOf.push_back(tb.poly);
            out.effectIds.push_back(9);
			for (int k = 0; k < 3; ++k) out.propCorner.push_back(corner(V(out.points[(size_t)a]), uvOf(tb.poly, 0)));
			out.propCorner.push_back(NOTOK);
		};
		for (size_t i = 0; i + 1 < ns; ++i)
			for (int s = 0; s < sides; ++s)
			{
				const int s1 = (s + 1) % sides;
				quad(rings[i][(size_t)s], rings[i][(size_t)s1], rings[i + 1][(size_t)s1], rings[i + 1][(size_t)s]);
			}
		for (int s = 0; s < sides; ++s)
		{
			const int s1 = (s + 1) % sides;
			tri3(capA, rings[0][(size_t)s1], rings[0][(size_t)s]);
			tri3(capB, rings[ns - 1][(size_t)s], rings[ns - 1][(size_t)s1]);
		}
	}
	out.mesh = std::move(mesh);
	out.inputValid = true;
	HostLog(host, "[ProjectDamage] flat fabric: " + std::to_string(outlines.size()) + " tears, " + std::to_string(tubes.size()) + " threads, "
		+ std::to_string(walls.size()) + " side-wall triangles");
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
