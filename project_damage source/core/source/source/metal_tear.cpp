// режем металлический лист вдоль рисунка и отгибаем его края
// ProjectDamage metal tear — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Recipe: metal-tear-v1. A slit torn through sheet metal along drawn strokes.
//
//   strands: every stroke is resampled (step ~ a third of the slit width),
//     each point snapped to the nearest surface point; normals and tangents
//     are smoothed along the stroke. Per sample: half widths left / right
//     (taper from a hairline at the start, sharp tips, torn notches),
//     a meander offset of the slit line, and the dent depth;
//   dent (base pass): a smooth position warp, see metal_tear.h;
//   cut (cutters): convex hulls of consecutive cross-sections, unioned.
//
// Pure function of its inputs; noise is keyed by (mainSeed, seed, stroke).
#include "metal_tear.h"
#include "adaptive_refine.h"
#include "manifold/manifold.h"
#include "project_damage_host.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

inline double MtClamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline double MtSmooth(double e0, double e1, double x) { const double t = MtClamp01((x - e0) / (e1 - e0)); return t * t * (3.0 - 2.0 * t); }
inline double MtDot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V MtCross(const V& a, const V& b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline double MtLen(const V& a) { return std::sqrt(MtDot(a, a)); }
inline V MtUnit(const V& a, const V& fb) { const double l = MtLen(a); return l > 1e-30 ? a / l : fb; }

inline uint64_t MtMix(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}
inline double MtU01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }

                                                                        
double MtNoise1(double x, uint64_t salt)
{
	const double f = std::floor(x), t = x - f;
	const double a = MtU01(MtMix((uint64_t)(int64_t)f ^ salt)) * 2.0 - 1.0;
	const double b = MtU01(MtMix((uint64_t)(int64_t)(f + 1.0) ^ salt)) * 2.0 - 1.0;
	const double s = t * t * (3.0 - 2.0 * t);
	return a + (b - a) * s;
}
double MtFbm1(double x, uint64_t salt) { return (MtNoise1(x, salt) + 0.5 * MtNoise1(x * 2.13, salt + 1) + 0.25 * MtNoise1(x * 4.37, salt + 2)) / 1.75; }

struct MtSample
{
	V c{ 0.0 }, n{ 0.0, 0.0, 1.0 }, t{ 1.0, 0.0, 0.0 }, side{ 0.0, 1.0, 0.0 };
	double u = 0.0, f = 0.0;                                              
	double hwL = 0.0, hwR = 0.0;                                        
	double off = 0.0;                                               
	double depth = 0.0;                            
};
using MtStrand = std::vector<MtSample>;

                                                                        
struct MtSurface
{
	std::vector<V> a, b, c, guide;
	void Build(const MeshView& v)
	{
		for (int32_t f = 0; f < v.npolys; ++f) {
			const ProjectDamagePoly& q = v.polys[f];
			a.push_back(v.pts[q.a]); b.push_back(v.pts[q.b]); c.push_back(v.pts[q.c]);
			guide.push_back(GuidedNormal(v, f, MtUnit(MtCross(b.back()-a.back(),c.back()-a.back()),V(0,0,1))));
			if (q.c != q.d) { a.push_back(v.pts[q.a]); b.push_back(v.pts[q.c]); c.push_back(v.pts[q.d]); guide.push_back(GuidedNormal(v, f, MtUnit(MtCross(b.back()-a.back(),c.back()-a.back()),V(0,0,1)))); }
		}
	}
	static V Closest(const V& p, const V& A, const V& B, const V& C)
	{
		const V ab = B - A, ac = C - A, ap = p - A;
		const double d1 = MtDot(ab, ap), d2 = MtDot(ac, ap);
		if (d1 <= 0 && d2 <= 0) return A;
		const V bp = p - B; const double d3 = MtDot(ab, bp), d4 = MtDot(ac, bp);
		if (d3 >= 0 && d4 <= d3) return B;
		const double vc = d1 * d4 - d3 * d2;
		if (vc <= 0 && d1 >= 0 && d3 <= 0) return A + ab * (d1 / (d1 - d3));
		const V cp = p - C; const double d5 = MtDot(ab, cp), d6 = MtDot(ac, cp);
		if (d6 >= 0 && d5 <= d6) return C;
		const double vb = d5 * d2 - d1 * d6;
		if (vb <= 0 && d2 >= 0 && d6 <= 0) return A + ac * (d2 / (d2 - d6));
		const double va = d3 * d6 - d5 * d4;
		if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return B + (C - B) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
		const double den = 1.0 / (va + vb + vc);
		return A + ab * (vb * den) + ac * (vc * den);
	}
	bool Project(const V& p, V& q, V& n) const
	{
		double best = 1e300; size_t hit = (size_t)-1;
		for (size_t i = 0; i < a.size(); ++i) {
			                                                        
			const V lo = m::la::min(a[i], m::la::min(b[i], c[i])), hi = m::la::max(a[i], m::la::max(b[i], c[i]));
			const double dx = std::max({ lo.x - p.x, 0.0, p.x - hi.x }), dy = std::max({ lo.y - p.y, 0.0, p.y - hi.y }), dz = std::max({ lo.z - p.z, 0.0, p.z - hi.z });
			if (dx * dx + dy * dy + dz * dz >= best) continue;
			const V cp = Closest(p, a[i], b[i], c[i]);
			const double d2 = MtDot(cp - p, cp - p);
			if (d2 < best) { best = d2; hit = i; q = cp; }
		}
		if (hit == (size_t)-1) return false;
		n = guide[hit];
		return true;
	}
};

std::vector<MtStrand> MtBuildStrands(const CoreInput& in, const MetalTearParams& p, Int32 mainSeed)
{
	std::vector<MtStrand> out;
	if (in.view.pts == nullptr || in.view.npolys <= 0) return out;
	MtSurface surf; surf.Build(in.view);
	const double width = std::max(1e-4, p.width);
	const double step = std::max(std::max(1e-4, p.detail) * 0.8, width * 0.33);
	const double taper = MtClamp01(p.taper / 100.0), jag = MtClamp01(p.jagged / 100.0), mea = MtClamp01(p.meander / 100.0);
	size_t at = 0;
	for (size_t s = 0; s < p.strokeCount.size(); ++s) {
		const int32_t cnt = p.strokeCount[s];
		if (cnt < 2 || (at + (size_t)cnt) * 3 > p.strokePts.size()) { at += (size_t)std::max(0, cnt); continue; }
		std::vector<V> pts;
		for (int32_t k = 0; k < cnt; ++k) pts.push_back(V(p.strokePts[(at + k) * 3], p.strokePts[(at + k) * 3 + 1], p.strokePts[(at + k) * 3 + 2]));
		at += (size_t)cnt;
		                         
		std::vector<double> acc(pts.size(), 0.0);
		for (size_t k = 1; k < pts.size(); ++k) acc[k] = acc[k - 1] + MtLen(pts[k] - pts[k - 1]);
		const double total = acc.back();
		if (total < width) continue;
		const int nS = std::max(3, (int)std::ceil(total / step) + 1);
		MtStrand st((size_t)nS);
		size_t seg = 0;
		for (int i = 0; i < nS; ++i) {
			const double u = total * i / (nS - 1);
			while (seg + 2 < pts.size() && acc[seg + 1] < u) ++seg;
			const double l = acc[seg + 1] - acc[seg];
			const double tt = l > 0 ? (u - acc[seg]) / l : 0.0;
			st[(size_t)i].c = pts[seg] + (pts[seg + 1] - pts[seg]) * tt;
			st[(size_t)i].u = u; st[(size_t)i].f = u / total;
		}
		                        
		ProjectDamageParallelFor(0, (int64_t)nS, [&](int64_t i) {
			V q, n;
			if (surf.Project(st[(size_t)i].c, q, n)) { st[(size_t)i].c = q; st[(size_t)i].n = n; }
		});
		                                 
		std::vector<V> ns((size_t)nS), ts((size_t)nS);
		for (int i = 0; i < nS; ++i) {
			V sn(0.0); for (int k = std::max(0, i - 3); k <= std::min(nS - 1, i + 3); ++k) sn += st[(size_t)k].n;
			ns[(size_t)i] = MtUnit(sn, V(0, 0, 1));
			const V d = st[(size_t)std::min(nS - 1, i + 1)].c - st[(size_t)std::max(0, i - 1)].c;
			ts[(size_t)i] = d;
		}
		const uint64_t salt = MtMix(((uint64_t)(uint32_t)mainSeed << 32) ^ (uint64_t)(uint32_t)p.seed ^ (s * 0x9E37ull + 0x7EA8ull));
		for (int i = 0; i < nS; ++i) {
			MtSample& q = st[(size_t)i];
			q.n = ns[(size_t)i];
			q.t = MtUnit(ts[(size_t)i] - q.n * MtDot(ts[(size_t)i], q.n), V(1, 0, 0));
			q.side = MtUnit(MtCross(q.n, q.t), V(0, 1, 0));
			const double r = (1.0 - taper) + taper * q.f;                                                      
			const double tip = MtSmooth(0.0, 0.12, q.f) * MtSmooth(0.0, 0.06, 1.0 - q.f);
			const double hw = 0.5 * width * r * tip;
			const double na = MtFbm1(q.u / (width * 0.7), salt), nb = MtFbm1(q.u / (width * 0.7), salt + 11);
			const double ra = MtU01(MtMix(salt ^ ((uint64_t)i * 31))), rb = MtU01(MtMix(salt ^ ((uint64_t)i * 37 + 5)));
			q.hwL = std::max(0.02 * width, hw * (1.0 + jag * (0.9 * na + 0.7 * (ra - 0.5))));
			q.hwR = std::max(0.02 * width, hw * (1.0 + jag * (0.9 * nb + 0.7 * (rb - 0.5))));
			q.off = mea * width * 0.6 * MtFbm1(q.u / (width * 4.0), salt + 23) * tip;
			q.depth = std::max(0.0, p.dentDepth) * (0.35 + 0.65 * r) * MtSmooth(0.0, 0.25, q.f) * MtSmooth(0.0, 0.15, 1.0 - q.f);
		}
		out.push_back(std::move(st));
	}
	return out;
}

                                                                         
                                                                             
double MtPointSegmentDist2(const V& p,const V& a,const V& b)
{
    const V ab=b-a;
    const double t=MtClamp01(MtDot(p-a,ab)/std::max(MtDot(ab,ab),1e-30));
    const V d=p-(a+ab*t);return MtDot(d,d);
}

double MtSegmentDist2(const V& p,const V& q,const V& a,const V& b)
{
    const V u=q-p,v=b-a,w=p-a;
    const double A=MtDot(u,u),B=MtDot(u,v),C=MtDot(v,v),D=MtDot(u,w),E=MtDot(v,w);
    if(A<1e-24)return MtPointSegmentDist2(p,a,b);
    if(C<1e-24)return MtPointSegmentDist2(a,p,q);
    const double den=A*C-B*B;
    double s=den>1e-24?MtClamp01((B*E-C*D)/den):0.0;
    double t=(B*s+E)/C;
    if(t<0.0){t=0.0;s=MtClamp01(-D/A);}
    else if(t>1.0){t=1.0;s=MtClamp01((B-D)/A);}
    const V d=w+u*s-v*t;return MtDot(d,d);
}

double MtTriangleSegmentDist2(const V& a,const V& b,const V& c,const V& p,const V& q)
{
    const V ab=b-a,ac=c-a,dir=q-p,h=MtCross(dir,ac);
    const double det=MtDot(ab,h);
    if(std::fabs(det)>1e-20){
        const V s=p-a;const double u=MtDot(s,h)/det;
        const V cross=MtCross(s,ab);const double v=MtDot(dir,cross)/det;
        const double t=MtDot(ac,cross)/det;
        if(u>=0.0&&v>=0.0&&u+v<=1.0&&t>=0.0&&t<=1.0)return 0.0;
    }
    const V pa=p-MtSurface::Closest(p,a,b,c),qa=q-MtSurface::Closest(q,a,b,c);
    return std::min({MtDot(pa,pa),MtDot(qa,qa),MtSegmentDist2(p,q,a,b),
                     MtSegmentDist2(p,q,b,c),MtSegmentDist2(p,q,c,a)});
}

                                                                        
                                                                             
class MtRefinementRibbon
{
    struct Segment {V a,b,n,lo,hi;};
    struct Node {V lo,hi;size_t begin=0,end=0;int left=-1,right=-1;};
    std::vector<Segment> segments;
    std::vector<size_t> order;
    std::vector<Node> nodes;
    double support2,nearSize,fineRadius,normalScale;
    int Build(size_t begin,size_t end){
        Node node;node.lo=V(1e300);node.hi=V(-1e300);node.begin=begin;node.end=end;
        for(size_t i=begin;i<end;++i){const auto& s=segments[order[i]];node.lo=m::la::min(node.lo,s.lo);node.hi=m::la::max(node.hi,s.hi);}
        const int id=(int)nodes.size();nodes.push_back(node);
        if(end-begin<=8)return id;
        const V extent=node.hi-node.lo;const int axis=extent.x>=extent.y&&extent.x>=extent.z?0:extent.y>=extent.z?1:2;
        const size_t mid=(begin+end)/2;
        std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](size_t a,size_t b){return segments[a].lo[axis]+segments[a].hi[axis]<segments[b].lo[axis]+segments[b].hi[axis];});
        const int left=Build(begin,mid),right=Build(mid,end);nodes[id].left=left;nodes[id].right=right;return id;
    }
public:
    MtRefinementRibbon(const std::vector<MtStrand>& strands,double support,double detail,double fine,double depth)
        :support2(support*support),nearSize(detail),fineRadius(fine),normalScale(support/std::max(support,depth)){
        for(const auto& st:strands)for(size_t i=1;i<st.size();++i){
            const V a=st[i-1].c+st[i-1].side*st[i-1].off,b=st[i].c+st[i].side*st[i].off;
            const V n=MtUnit(st[i-1].n+st[i].n,st[i].n);
            const V lo=m::la::min(m::la::min(a-n*depth,a+n*depth),m::la::min(b-n*depth,b+n*depth));
            const V hi=m::la::max(m::la::max(a-n*depth,a+n*depth),m::la::max(b-n*depth,b+n*depth));
            segments.push_back({a,b,n,lo,hi});
        }
        order.resize(segments.size());std::iota(order.begin(),order.end(),0);
        if(!segments.empty())Build(0,segments.size());
    }
    double Target(const V& a,const V& b,const V& c)const{
        if(nodes.empty())return 1e300;
        const V lo=m::la::min(a,m::la::min(b,c)),hi=m::la::max(a,m::la::max(b,c)),center=(a+b+c)/3.0;
        double distance2=1e300;bool touches=false;
        std::vector<int> stack;stack.reserve(32);stack.push_back(0);
        while(!stack.empty()){
            const auto& n=nodes[(size_t)stack.back()];stack.pop_back();
            const V gap=m::la::max(m::la::max(n.lo-hi,lo-n.hi),V(0.0));
            if(MtDot(gap,gap)>support2)continue;
            if(n.left>=0){stack.push_back(n.left);stack.push_back(n.right);continue;}
            for(size_t k=n.begin;k<n.end;++k){const auto& s=segments[order[k]];
                                                                                
                                                                            
                auto metric=[&](const V& p){return p-s.n*(MtDot(p-s.a,s.n)*(1.0-normalScale));};
                if(MtTriangleSegmentDist2(metric(a),metric(b),metric(c),s.a,metric(s.b))>support2)continue;
                const V projected=center-s.n*MtDot(center-s.a,s.n);
                touches=true;distance2=std::min(distance2,MtPointSegmentDist2(projected,s.a,s.b));
            }
        }
                                                                            
                                                                                
        return touches?nearSize+0.65*std::max(0.0,std::sqrt(distance2)-fineRadius):1e300;
    }
};

}             

// основной вход: собираем геометрию по переданным настройкам
bool ApplyMetalTearDent(const CoreInput& in, const MetalTearParams& p, Int32 mainSeed,
	const m::Manifold& base, m::Manifold& out, std::string* log)
{
	if (!p.enable || base.Status() != m::Manifold::Error::NoError) return false;
	const std::vector<MtStrand> strands = MtBuildStrands(in, p, mainSeed);
	if (strands.empty()) return false;
	const double W = std::max(1e-4, p.dentWidth);
	const double lip = MtClamp01(p.lipCurl / 100.0);
	const double reachN = std::max({ 1.5 * p.cutDepth, W, 2.0 * p.dentDepth });
	const double Rinf = 2.6 * W;

	                                                                
	struct Seg { const MtSample* a; const MtSample* b; };
	std::vector<Seg> segs;
	const std::vector<m::Box> boxes;                                  
	for (const MtStrand& st : strands)
		for (size_t k = 0; k + 1 < st.size(); ++k) {
			segs.push_back({ &st[k], &st[k + 1] });
		}
	const double detail=std::max(1e-4,p.detail);
    const double fineRadius=std::max(p.width,3.0*std::max(0.0,p.blisterSize));
    double refineSupport=Rinf;
    for(const auto& strand:strands)for(const auto& sample:strand)
        refineSupport=std::max(refineSupport,std::max(sample.hwL,sample.hwR)+3.0*std::max(0.0,p.blisterSize));
    const MtRefinementRibbon ribbon(strands,refineSupport,detail,fineRadius,reachN);
    const AdaptiveTriangleSize localSize=[&](const V& a,const V& b,const V& c){return ribbon.Target(a,b,c);};
    m::Manifold fine = RefineNearDamage(base, boxes, detail, 0.65, 16,
        (size_t)std::max<int32_t>(20000,p.maxTris),nullptr,&localSize);
	if (fine.Status() != m::Manifold::Error::NoError || fine.NumTri() == 0) fine = base;

	                                                 
	struct Bead { V c, n; double r, a; };
	std::vector<Bead> beads;
	{
		const double bs = std::max(1e-4, p.blisterSize), amount = MtClamp01(p.blisters / 100.0);
		for (size_t si = 0; si < strands.size(); ++si) {
			const MtStrand& st = strands[si];
			const double len = st.back().u;
			const int count = (int)std::round(amount * len / (bs * 2.5));
			const uint64_t salt = MtMix(((uint64_t)(uint32_t)mainSeed << 32) ^ (uint64_t)(uint32_t)p.seed ^ (si * 0xB3EDull + 0xBEADull));
			for (int i = 0; i < count; ++i) {
				const uint64_t h = MtMix(salt + (uint64_t)i * 0x9E3779B9ull);
				const double u = MtU01(h) * len;
				size_t k = (size_t)std::min<double>((double)st.size() - 1, std::floor(u / len * (double)(st.size() - 1)));
				const MtSample& q = st[k];
				const double sgn = MtU01(MtMix(h + 1)) < 0.5 ? 1.0 : -1.0;
				const double lat = (sgn > 0 ? q.hwL : q.hwR) + bs * (0.2 + 1.3 * MtU01(MtMix(h + 2)));
				Bead b;
				b.c = q.c + q.side * (q.off + sgn * lat);
				b.n = q.n;
				b.r = bs * (0.5 + 0.8 * MtU01(MtMix(h + 3)));
				b.a = b.r * 0.55 * (0.4 + 0.6 * MtU01(MtMix(h + 4))) * MtSmooth(0.0, 0.1, q.f) * MtSmooth(0.0, 0.05, 1.0 - q.f);
				beads.push_back(b);
			}
		}
	}

	                                     
	const double cell = Rinf;
	auto key = [](int x, int y, int z) { return ((uint64_t)(uint32_t)(x + 1048576) << 42) ^ ((uint64_t)(uint32_t)(y + 1048576) << 21) ^ (uint64_t)(uint32_t)(z + 1048576); };
	std::unordered_map<uint64_t, std::vector<int32_t>> segCells, beadCells;
	for (size_t s = 0; s < segs.size(); ++s) {
		const V lo = m::la::min(segs[s].a->c, segs[s].b->c) - V(Rinf + reachN), hi = m::la::max(segs[s].a->c, segs[s].b->c) + V(Rinf + reachN);
		for (int z = (int)std::floor(lo.z / cell); z <= (int)std::floor(hi.z / cell); ++z)
			for (int y = (int)std::floor(lo.y / cell); y <= (int)std::floor(hi.y / cell); ++y)
				for (int x = (int)std::floor(lo.x / cell); x <= (int)std::floor(hi.x / cell); ++x)
					segCells[key(x, y, z)].push_back((int32_t)s);
	}
	for (size_t b = 0; b < beads.size(); ++b) {
		const V& c = beads[b].c;
		beadCells[key((int)std::floor(c.x / cell), (int)std::floor(c.y / cell), (int)std::floor(c.z / cell))].push_back((int32_t)b);
	}

	m::MeshGL64 mesh = fine.GetMeshGL64();
	const size_t np = mesh.numProp, nv = mesh.NumVert();
	std::vector<int32_t> geo(nv);
	std::iota(geo.begin(), geo.end(), 0);
	for (size_t k = 0; k < mesh.mergeFromVert.size() && k < mesh.mergeToVert.size(); ++k) geo[(size_t)mesh.mergeFromVert[k]] = (int32_t)mesh.mergeToVert[k];
	for (size_t i = 0; i < nv; ++i) { int32_t r = geo[i]; int g = 0; while (geo[(size_t)r] != r && g++ < 64) r = geo[(size_t)r]; geo[i] = r; }

	std::vector<V> moved(nv);
	ProjectDamageParallelFor(0, (int64_t)nv, [&](int64_t ii) {
		const size_t i = (size_t)ii;
		const V x(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]);
		moved[i] = x;
		if (geo[i] != (int32_t)i) return;
		const int cx = (int)std::floor(x.x / cell), cy = (int)std::floor(x.y / cell), cz = (int)std::floor(x.z / cell);
		auto it = segCells.find(key(cx, cy, cz));
		if (it == segCells.end()) return;
		                                                                          
		double sumD = 0.0; V sumV(0.0); V nFront(0.0); double hFront = 0.0, bestS = 1e300;
		for (int32_t s : it->second) {
			const MtSample& A = *segs[(size_t)s].a; const MtSample& B = *segs[(size_t)s].b;
			const V ca = A.c + A.side * A.off, cb = B.c + B.side * B.off;
			const V ab = cb - ca;
			const double l2 = MtDot(ab, ab);
			const double tau = l2 > 0 ? MtClamp01(MtDot(x - ca, ab) / l2) : 0.0;
			const V c = ca + ab * tau;
			const V n = MtUnit(A.n + (B.n - A.n) * tau, A.n);
			const V o = x - c;
			const double hN = MtDot(o, n);
			if (std::fabs(hN) > reachN) continue;
			const double sLat = MtLen(o - n * hN);
			if (sLat > Rinf) continue;
			const double depth = A.depth + (B.depth - A.depth) * tau;
			const double prof = (1.0 - lip) * std::exp(-(sLat / W) * (sLat / W)) + lip * std::exp(-(sLat / (0.3 * W)) * (sLat / (0.3 * W)));
			const double D = depth * prof;
			if (D <= 0.0) continue;
			sumD += D; sumV += n * (D * D);
			if (sLat < bestS) { bestS = sLat; nFront = n; hFront = hN; }
		}
		V d(0.0);
		if (sumD > 0.0) d = d - sumV / sumD;
		                                                                         
		if (!beads.empty() && hFront > -0.3 * reachN) {
			for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int xx = -1; xx <= 1; ++xx) {
				auto bt = beadCells.find(key(cx + xx, cy + y, cz + z));
				if (bt == beadCells.end()) continue;
				for (int32_t b : bt->second) {
					const Bead& e = beads[(size_t)b];
					const V o = x - e.c;
					const double hN = MtDot(o, e.n);
					if (hN < -0.5 * reachN || hN > reachN) continue;
					const double s = MtLen(o - e.n * hN) / e.r;
					if (s < 2.2) d += e.n * (e.a * std::exp(-s * s * 1.6));
				}
			}
		}
		moved[i] = x + d;
	});
	for (size_t i = 0; i < nv; ++i) {
		const V& q = moved[(size_t)geo[i]];
		mesh.vertProperties[i * np] = q.x; mesh.vertProperties[i * np + 1] = q.y; mesh.vertProperties[i * np + 2] = q.z;
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0) {
		if (log) *log = "[ProjectDamage] Metal Tear: the dented mesh failed Manifold's checks - dent skipped, the slit is still cut";
		return false;
	}
	out = std::move(result);
	if (log) *log = "[ProjectDamage] Metal Tear: " + std::to_string(strands.size()) + " strokes, " + std::to_string(out.NumTri()) + " triangles";
	return true;
}

void PlaceMetalTearCuts(const CoreInput& in, const MetalTearParams& p, Int32 mainSeed, std::vector<m::Manifold>& cutters)
{
	if (!p.enable) return;
	const std::vector<MtStrand> strands = MtBuildStrands(in, p, mainSeed);
	const double top = std::max(0.3 * p.dentWidth, 3.0 * p.blisterSize) + 0.05;
	const double bot = std::max(0.0, p.dentDepth) + std::max(1e-3, p.cutDepth);
	for (const MtStrand& st : strands) {
		std::vector<m::Manifold> hulls;
		hulls.reserve(st.size());
		auto ring = [&](const MtSample& q, std::vector<V>& pts) {
			const V c = q.c + q.side * q.off;
			const V L = c + q.side * q.hwL, R = c - q.side * q.hwR;
			pts.push_back(L + q.n * top); pts.push_back(R + q.n * top);
			pts.push_back(L - q.n * bot); pts.push_back(R - q.n * bot);
		};
		for (size_t k = 0; k + 1 < st.size(); ++k) {
			std::vector<V> pts;
			ring(st[k], pts); ring(st[k + 1], pts);
			m::Manifold h = m::Manifold::Hull(pts);
			if (h.Status() == m::Manifold::Error::NoError && h.NumTri() > 0) hulls.push_back(std::move(h));
		}
		if (hulls.empty()) continue;
		m::Manifold cut = m::Manifold::BatchBoolean(hulls, m::OpType::Add);
		if (cut.Status() == m::Manifold::Error::NoError && cut.NumTri() > 0) cutters.push_back(std::move(cut));
	}
}

PROJECT_DAMAGE_NS_MACRO_END
