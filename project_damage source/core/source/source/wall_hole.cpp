// строим проём и кирпичное заполнение старого варианта стены
// ProjectDamage wall hole with bricks — GPL-3.0-or-later; see LICENSE.
//
// Recipe wall-hole-v1 (WallHoleParams). A breach THROUGH a plastered brick
// wall: unlike Brick Damage (bricks laid in the face, under the render), the
// bricks here fill the wall's THICKNESS - courses up the wall, wythes across
// it - so the hole shows the masonry from inside.
//   site     an empty / the 3D cursor, dropped on the nearest face; the wall
//            thickness T is measured by a ray; frame t (along the wall,
//            horizontal), u (up the wall), w (into it)
//   masonry  plaster coats on both faces; between them k wythes (as many as
//            fit Brick Width + Mortar); courses Brick Height + Mortar high;
//            Running bond (stretchers, half-brick shift per course) or
//            Header courses (every second course of headers across the wall)
//   hole     a ragged tall opening; each wythe gets its own slightly smaller /
//            shifted outline, so the edge steps back into the wall; a brick
//            whose centre lies inside its wythe's outline is taken out whole
//            (its cell, joints included) - the edge follows the joints
//   joints   the joint network (bed, head and collar joints) clipped to the
//            removed cells grown by Joint Depth: a shallow raked joint all
//            round the opening, so the bricks read one by one
//   plaster  knocked off both faces in a ragged band wider than the brick hole
//   custom   (customBricks) the bricks on the rim are cut out of the wall too
//            and returned as frames (some broken by a plane): your models are
//            placed there by the add-on
//   rubble   the removed bricks lie in a heap at the foot of the wall, on both
//            sides: frames for your models, or simple brick boxes
// Classes: removed cells BrickBreak, raked joints BrickSeam, plaster band
// BrickRender - the Brick Damage materials apply.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "wall_hole.h"
#include "through_wall_exit.h"
#include "project_damage_internal.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace m = manifold;
using namespace project_damage;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
double R01(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}
V Unit(const V& v, const V& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; }
double RingNoise(double a01, int cells, uint64_t seed, uint64_t ch)
{
	const double x = a01 * cells, f = std::floor(x), t = x - f;
	const int i0 = ((int)f % cells + cells) % cells, i1 = (i0 + 1) % cells;
	const double s = t * t * (3 - 2 * t);
	const double a = R01(seed, (uint64_t)i0, ch) * 2 - 1, b = R01(seed, (uint64_t)i1, ch) * 2 - 1;
	return a + (b - a) * s;
}

struct Frame { V o, t, u, w; };
                                                                        
m::Manifold Box(const Frame& f, double a0, double a1, double b0, double b1, double c0, double c1)
{
	const V size(a1 - a0, b1 - b0, c1 - c0);
	if (size.x <= 0 || size.y <= 0 || size.z <= 0) return m::Manifold();
	const m::mat3x4 M(f.t, f.u, f.w, f.o + f.t * a0 + f.u * b0 + f.w * c0);
	return m::Manifold::Cube(size).Transform(M);
}
                                                                                       
void PushFrame(std::vector<double>& out, const V& ax, const V& ay, const V& az, const V& c)
{
	out.insert(out.end(), { ax.x, ax.y, ax.z, ay.x, ay.y, ay.z, az.x, az.y, az.z, c.x, c.y, c.z });
}

struct Brick { double a0, a1, b0, b1, c0, c1; int wythe; bool header; };

}               

void PlaceWallHoles(const CoreInput& in, const WallHoleParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, std::vector<Int32>& classes, WallHoleOut& outB, const ProjectDamageHost& host)
{
	const MeshView& mv = in.view;
	if (!p.enable || mv.npolys <= 0 || p.sites.empty()) return;
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x57414C4C484F4C45ULL) ^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	ProjectDamageBVH bvh;
	bvh.Build(mv);
	V lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < mv.npts; ++i) { lo = m::la::min(lo, V(mv.pts[i])); hi = m::la::max(hi, V(mv.pts[i])); }
	const double diag = m::la::length(hi - lo), eps = 1e-6 * diag + 1e-9;
	const V upW = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	const double pitchL = p.brickLength + p.mortar, pitchH = p.brickHeight + p.mortar;
	for (size_t si = 0; si < p.sites.size(); si += 3)
	{
		const V s(p.sites[si], p.sites[si + 1], p.sites[si + 2]);
		                                 
		double bd = 1e300; V hitP(0.0), n(0, -1, 0);
		for (Int32 k = 0; k < mv.npolys; ++k)
		{
			const ProjectDamagePoly& q = mv.polys[k];
			const Int32 ids[4] = { q.a, q.b, q.c, q.d };
			for (int tri = 1; tri + 1 < (q.c == q.d ? 3 : 4); ++tri)
			{
				const V A = mv.pts[ids[0]], B = mv.pts[ids[tri]], C = mv.pts[ids[tri + 1]];
				const V nn = Unit(m::la::cross(B - A, C - A), V(0, 0, 1));
				                                                             
				const V pp = s - nn * m::la::dot(s - A, nn);
				const V v0 = B - A, v1 = C - A, v2 = pp - A;
				const double d00 = m::la::dot(v0, v0), d01 = m::la::dot(v0, v1), d11 = m::la::dot(v1, v1);
				const double d20 = m::la::dot(v2, v0), d21 = m::la::dot(v2, v1), den = d00 * d11 - d01 * d01;
				if (std::abs(den) < 1e-18) continue;
				double bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
				bv = std::clamp(bv, 0.0, 1.0); bw = std::clamp(bw, 0.0, 1.0 - bv);
				const V q2 = A + v0 * bv + v1 * bw;
				const double d = m::la::length(q2 - s);
				if (d < bd) { bd = d; hitP = q2; n = nn; }
			}
		}
		                               
		const double T = FindWallExit(bvh, mv, hitP, n, 2 * eps, diag);
		if (!(T > 0.0)) { HostLog(host, "[ProjectDamage] wall hole: no wall thickness at a site; skipped"); continue; }
		Frame f;
		f.w = n * -1.0;
		f.t = Unit(m::la::cross(upW, n), Unit(m::la::cross(V(0, 0, 1), n), V(1, 0, 0)));
		f.u = m::la::cross(n, f.t);
		f.o = hitP;
		const uint64_t hk = seed ^ (uint64_t(si / 3 + 1) * 0x2545F4914F6CDD1DULL);
		                                       
		const double plaster = std::clamp(p.plasterThickness, 0.0, 0.45 * T);
		const double Ti = T - 2.0 * plaster;
		const int k = std::max(1, (int)std::round(Ti / std::max(1e-6, p.brickWidth + p.mortar)));
		const double dw = Ti / k;
		                                                                   
		const double HW = 0.5 * p.holeWidth, HH = 0.5 * p.holeHeight;
		const double bottomB = -HH + p.holeLift;
		auto inside = [&](double a, double b, int wy, double grow) {
			const double sc = 1.0 - p.depthVariation * 0.5 * R01(hk, 900 + (uint64_t)wy, 1);
			const double sa = (R01(hk, 900 + (uint64_t)wy, 2) - 0.5) * p.depthVariation * 0.3 * HW;
			const double x = (a - sa) / (HW * sc + grow), y = (b - p.holeLift) / (HH * sc + grow);
			const double r = std::sqrt(x * x + y * y);
			if (r < 1e-9) return true;
			const double ang = std::atan2(y, x) / (2 * m::kPi) + 0.5;
			const double lim = 1.0 + p.jagged * (0.25 * RingNoise(ang, 9, hk, 3) + 0.12 * RingNoise(ang, 27, hk, 4 + (uint64_t)wy));
			return r < lim;
		};
		(void)bottomB;
		                                      
		std::vector<Brick> bricks;
		const double margin = 2.0 * pitchL + p.plasterBand;
		const double aMin = -HW * (1 + p.jagged) - margin, aMax = HW * (1 + p.jagged) + margin;
		const double bMin = p.holeLift - HH * (1 + p.jagged) - margin, bMax = p.holeLift + HH * (1 + p.jagged) + margin;
		const long rLo = (long)std::floor(bMin / pitchH), rHi = (long)std::ceil(bMax / pitchH);
		for (long r = rLo; r <= rHi; ++r)
		{
			const double b0 = r * pitchH + 0.5 * p.mortar, b1 = b0 + p.brickHeight;
			const bool headerCourse = p.bond == 1 && (r % 2 != 0) && k >= 2;
			if (headerCourse)
			{
				                                                                                                 
				const double hp = p.brickWidth + p.mortar;
				const int nz = std::max(1, (int)std::round(Ti / std::max(1e-6, pitchL)));
				const double dz = Ti / nz;
				const long c0 = (long)std::floor(aMin / hp), c1 = (long)std::ceil(aMax / hp);
				for (int z = 0; z < nz; ++z)
					for (long c = c0; c <= c1; ++c)
					{
						const double a0 = c * hp + 0.5 * p.mortar + ((r / 2) % 2 ? 0.5 * hp : 0.0);
						bricks.push_back({ a0, a0 + p.brickWidth, b0, b1, plaster + z * dz + 0.5 * p.mortar, plaster + (z + 1) * dz - 0.5 * p.mortar,
							std::min(k - 1, (int)((z + 0.5) * dz / dw)), true });
					}
				continue;
			}
			for (int wy = 0; wy < k; ++wy)
			{
				const double shift = ((r % 2) != 0 ? 0.5 * pitchL : 0.0) + (wy % 2 ? 0.25 * pitchL : 0.0);
				const long c0 = (long)std::floor((aMin - shift) / pitchL), c1 = (long)std::ceil((aMax - shift) / pitchL);
				for (long c = c0; c <= c1; ++c)
				{
					const double a0 = c * pitchL + shift + 0.5 * p.mortar;
					bricks.push_back({ a0, a0 + p.brickLength, b0, b1, plaster + wy * dw + 0.5 * p.mortar, plaster + (wy + 1) * dw - 0.5 * p.mortar, wy, false });
				}
			}
		}
		           
		std::vector<m::Manifold> removed, removedGrown;
		std::vector<size_t> rim;
		size_t nRemoved = 0;
		const double jd = std::max(0.0, p.jointDepth);
		for (size_t bi = 0; bi < bricks.size(); ++bi)
		{
			const Brick& b = bricks[bi];
			const double ca = 0.5 * (b.a0 + b.a1), cb = 0.5 * (b.b0 + b.b1);
			                                                                         
			const double rag = (R01(hk, bi, 80) - 0.45) * p.edgeRagged * pitchL;
			const bool centreIn = inside(ca, cb, b.wythe, rag);
			int cornersIn = 0;
			for (double a : { b.a0, b.a1 }) for (double bb : { b.b0, b.b1 }) cornersIn += inside(a, bb, b.wythe, 0.0) ? 1 : 0;
			                                                                                  
			const double c0 = b.c0 - 0.5 * p.mortar - (b.c0 - 0.5 * p.mortar <= plaster + 1e-9 ? plaster + 0.05 * T : 0.0);
			const double c1 = b.c1 + 0.5 * p.mortar + (b.c1 + 0.5 * p.mortar >= T - plaster - 1e-9 ? plaster + 0.05 * T : 0.0);
			if (centreIn)
			{
				removed.push_back(Box(f, b.a0 - 0.5 * p.mortar, b.a1 + 0.5 * p.mortar, b.b0 - 0.5 * p.mortar, b.b1 + 0.5 * p.mortar, c0, c1));
				removedGrown.push_back(Box(f, b.a0 - 0.5 * p.mortar - jd, b.a1 + 0.5 * p.mortar + jd, b.b0 - 0.5 * p.mortar - jd, b.b1 + 0.5 * p.mortar + jd, c0 - jd, c1 + jd));
				++nRemoved;
				         
				if (p.rubbleEnable && R01(hk, bi, 50) < p.rubbleShare && (int)outB.rubbleCount < p.rubbleMax)
				{
					const double side = R01(hk, bi, 51) < 0.6 ? -1.0 : 1.0;                                  
					const double spread = p.holeWidth * 0.5 * p.rubbleSpread;
					const double ra = (R01(hk, bi, 52) * 2 - 1) * spread * 1.3;
					const double rw = (side < 0 ? -1.0 : T + 0.0) + side * (R01(hk, bi, 53) * spread * 0.9 + 0.5 * p.brickLength);
					                                            
					const double heapH = p.holeHeight * 0.18 * p.rubbleSpread * std::max(0.0, 1.0 - std::abs(ra) / (spread * 1.4)) * std::max(0.0, 1.0 - std::abs(rw - (side < 0 ? 0.0 : T)) / (spread * 1.2));
					                                                            
					const double floorB = m::la::dot(lo - f.o, f.u) < m::la::dot(hi - f.o, f.u) ? std::min(m::la::dot(lo - f.o, f.u), m::la::dot(hi - f.o, f.u)) : m::la::dot(lo - f.o, f.u);
					double floorU = 1e300;
					for (const V& c : { V(lo.x, lo.y, lo.z), V(hi.x, lo.y, lo.z), V(lo.x, hi.y, lo.z), V(hi.x, hi.y, lo.z), V(lo.x, lo.y, hi.z), V(hi.x, lo.y, hi.z), V(lo.x, hi.y, hi.z), V(hi.x, hi.y, hi.z) })
						floorU = std::min(floorU, m::la::dot(c - f.o, f.u));
					(void)floorB;
					const double rb = floorU + 0.5 * p.brickHeight + R01(hk, bi, 54) * heapH;
					const V centre = f.o + f.t * ra + f.u * rb + f.w * rw;
					                                        
					const double yaw = R01(hk, bi, 55) * 2 * m::kPi, tilt = (R01(hk, bi, 56) - 0.5) * 0.9, roll = (R01(hk, bi, 57) - 0.5) * 0.9;
					V ax = f.t * std::cos(yaw) + f.w * std::sin(yaw);
					V ay = f.u;
					V az = m::la::cross(ax, ay);
					ax = Unit(ax * std::cos(tilt) + ay * std::sin(tilt), ax); ay = Unit(m::la::cross(az, ax), ay);
					az = Unit(az * std::cos(roll) + ay * std::sin(roll), az); ay = Unit(m::la::cross(az, ax), ay);
					const double bl = b.header ? p.brickLength : p.brickLength, bh = p.brickHeight, bw = p.brickWidth;
					                                    
					const double partL = R01(hk, bi, 58) < p.brokenChance ? bl * (0.45 + 0.3 * R01(hk, bi, 59)) : bl;
					PushFrame(outB.rubbleFrame, ax * partL, ay * bh, az * bw, centre);
					outB.rubbleKey.push_back((Int32)(R01(hk, bi, 60) * 1000));
					++outB.rubbleCount;
				}
				continue;
			}
			if (cornersIn > 0) rim.push_back(bi);
		}
		if (removed.empty()) { HostLog(host, "[ProjectDamage] wall hole: the opening is smaller than a brick; skipped"); continue; }
		                                                                                
		if (p.customBricks)
			for (size_t ri : rim)
			{
				const Brick& b = bricks[ri];
				const double c0 = b.c0 - 0.5 * p.mortar - (b.c0 - 0.5 * p.mortar <= plaster + 1e-9 ? plaster + 0.05 * T : 0.0);
				const double c1 = b.c1 + 0.5 * p.mortar + (b.c1 + 0.5 * p.mortar >= T - plaster - 1e-9 ? plaster + 0.05 * T : 0.0);
				removed.push_back(Box(f, b.a0 - 0.5 * p.mortar, b.a1 + 0.5 * p.mortar, b.b0 - 0.5 * p.mortar, b.b1 + 0.5 * p.mortar, c0, c1));
				const V centre = f.o + f.t * (0.5 * (b.a0 + b.a1)) + f.u * (0.5 * (b.b0 + b.b1)) + f.w * (0.5 * (b.c0 + b.c1));
				                                                
				if (b.header) PushFrame(outB.rimFrame, f.w * (b.c1 - b.c0), f.u * (b.b1 - b.b0), f.t * (b.a1 - b.a0), centre);
				else PushFrame(outB.rimFrame, f.t * (b.a1 - b.a0), f.u * (b.b1 - b.b0), f.w * (b.c1 - b.c0), centre);
				outB.rimKey.push_back((Int32)(R01(hk, ri, 70) * 1000));
				                                                                        
				double cut[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
				if (R01(hk, ri, 71) < p.brokenChance)
				{
					const double ca = 0.5 * (b.a0 + b.a1), cb = 0.5 * (b.b0 + b.b1);
					const double sx = ca > 0 ? 1.0 : -1.0;                                                         
					cut[0] = b.header ? 0.0 : sx; cut[1] = (R01(hk, ri, 72) - 0.5) * 0.6; cut[2] = b.header ? sx * 0.3 : (R01(hk, ri, 73) - 0.5) * 0.4;
					const double l = std::sqrt(cut[0] * cut[0] + cut[1] * cut[1] + cut[2] * cut[2]);
					for (int c = 0; c < 3; ++c) cut[c] /= std::max(1e-9, l);
					cut[3] = (R01(hk, ri, 74) - 0.6) * 0.5;
					(void)cb;
				}
				outB.rimCut.insert(outB.rimCut.end(), cut, cut + 8);
				++outB.rimCount;
			}
		                                                                      
		                                                                       
		for (int wy = 0; wy < k; ++wy)
		{
			const double sc = 1.0 - p.depthVariation * 0.5 * R01(hk, 900 + (uint64_t)wy, 1);
			const double sa = (R01(hk, 900 + (uint64_t)wy, 2) - 0.5) * p.depthVariation * 0.3 * HW;
			m::SimplePolygon ring;
			const int K = 120;
			for (int i = 0; i < K; ++i)
			{
				const double a01 = double(i) / K;
				const double ang = (a01 - 0.5) * 2 * m::kPi;                                         
				const double lim = 1.0 + p.jagged * (0.25 * RingNoise(a01, 9, hk, 3) + 0.12 * RingNoise(a01, 27, hk, 4 + (uint64_t)wy));
				ring.push_back(m::vec2(sa + HW * sc * lim * std::cos(ang), p.holeLift + HH * sc * lim * std::sin(ang)));
			}
			const double c0 = wy == 0 ? -0.05 * T : plaster + wy * dw;
			const double c1 = wy == k - 1 ? T + 0.05 * T : plaster + (wy + 1) * dw;
			const m::CrossSection cs(ring);
			const m::mat3x4 M(f.t, f.u, f.w, f.o + f.w * c0);
			removed.push_back(m::Manifold::Extrude(cs.ToPolygons(), c1 - c0).Transform(M));
		}
		const m::Manifold holeCut = m::Manifold::BatchBoolean(removed, m::OpType::Add);
		cutters.push_back(holeCut);
		classes.push_back((Int32)DamageClass::BrickBreak);
		                                                                           
		if (jd > 0.0)
		{
			std::vector<m::Manifold> joints;
			for (const Brick& b : bricks)
			{
				                                                                    
				joints.push_back(Box(f, b.a0 - 0.5 * p.mortar, b.a1 + 0.5 * p.mortar, b.b1, b.b1 + p.mortar, b.c0 - 0.5 * p.mortar, b.c1 + 0.5 * p.mortar));
				joints.push_back(Box(f, b.a1, b.a1 + p.mortar, b.b0 - 0.5 * p.mortar, b.b1 + 0.5 * p.mortar, b.c0 - 0.5 * p.mortar, b.c1 + 0.5 * p.mortar));
				joints.push_back(Box(f, b.a0 - 0.5 * p.mortar, b.a1 + 0.5 * p.mortar, b.b0 - 0.5 * p.mortar, b.b1 + 0.5 * p.mortar, b.c1, b.c1 + p.mortar));
			}
			const m::Manifold net = m::Manifold::BatchBoolean(joints, m::OpType::Add);
			const m::Manifold zone = m::Manifold::BatchBoolean(removedGrown, m::OpType::Add);
			const m::Manifold groove = net ^ zone;
			if (!groove.IsEmpty()) { cutters.push_back(groove); classes.push_back((Int32)DamageClass::BrickSeam); }
		}
		                                                                      
		if (plaster > 0.0 && p.plasterBand > 0.0)
		{
			m::SimplePolygon ring;
			const int K = 96;
			for (int i = 0; i < K; ++i)
			{
				const double a01 = double(i) / K, ang = 2 * m::kPi * a01;
				const double grow = 1.0 + p.jagged * (0.25 * RingNoise(a01, 9, hk, 3) + 0.12 * RingNoise(a01, 27, hk, 4)) + 0.35 * p.jagged * RingNoise(a01, 17, hk, 8);
				ring.push_back(m::vec2((HW * grow + p.plasterBand * (0.5 + R01(hk, (uint64_t)i / 6, 9))) * std::cos(ang),
					p.holeLift + (HH * grow + p.plasterBand * (0.5 + R01(hk, (uint64_t)i / 6, 10))) * std::sin(ang)));
			}
			const m::CrossSection cs(ring);
			                                                                               
			const m::mat3x4 front(f.t, f.u, f.w, f.o + f.w * (-0.05 * T));
			const m::mat3x4 back(f.t, f.u, f.w, f.o + f.w * (T - plaster));
			std::vector<m::Manifold> bands = {
				m::Manifold::Extrude(cs.ToPolygons(), plaster + 0.05 * T).Transform(front),
				m::Manifold::Extrude(cs.ToPolygons(), plaster + 0.05 * T).Transform(back) };
			cutters.push_back(m::Manifold::BatchBoolean(bands, m::OpType::Add));
			classes.push_back((Int32)DamageClass::BrickRender);
		}
		HostLog(host, "[ProjectDamage] wall hole: " + std::to_string(nRemoved) + " bricks out, " + std::to_string(k)
			+ " wythes, wall " + std::to_string((int)std::round(T)) + " units thick");
	}
}

PROJECT_DAMAGE_NS_MACRO_END
