// рассчитываем разрушение здания вокруг заданных точек
// ProjectDamage building destruction — GPL-3.0-or-later; see LICENSE.
//
// Recipe: building-destruction-v1. Combined blast damage on a building,
// placed by empties and only in their proximity:
//   1. placement: every empty is carried into the source's local space by the
//      host (BuildingBlastEmpty::rel). Its Sphere display shape (x radius
//      scale) is the blast volume, an ellipsoid when the empty is scaled
//      non-uniformly. The blast distance field D(x) is the normalized
//      ellipsoid distance warped by fBm noise, so every layer below shares
//      one ragged outline. The empty's distance to the nearest surface point
//      fades the blast (Proximity): a weak blast shrinks and loses its inner
//      layers first;
//   2. building layout: the source's local +Z is up. Floors are bands of
//      floorHeight from the mesh bottom + floorOffset, each topped by a slab
//      of slabThickness; the roof is the last slab. Facades are found by
//      clustering the near-vertical polygons around the blast by plane, and a
//      facade counts only when nothing stands right in front of it (so the
//      inner faces of a hollow model are skipped);
//   3. core: a level-set blob of D < coreSize, everything removed;
//   4. slabs: each slab breaks off on a star outline of D < its own
//      threshold (between coreSize and roomSize, per slab), and the broken
//      end is shaved into a downward slant (slabDroop);
//   5. rooms: each storey band between two slabs is gutted on a faceted
//      outline of D < roomSize (roomFacets jittered corners, so the cut reads
//      as straight broken walls), trimmed behind every facade by wallDepth so
//      the facade wall is left for the masonry pass and the slabs stay as
//      ledges. Interior partitions on a building-wide grid square to the
//      nearest facade are left standing, some missing, some broken back;
//   6. masonry: on each facade a running-bond brick lattice; bricks with
//      D < wallSize (plus per-brick jitter) are removed through the wall as
//      merged course runs, so the opening edge is brick-stepped. A brick only
//      goes where a gutted room or the core is behind it (no blind pockets),
//      bricks in a slab band stay unless that slab broke there, and bricks
//      left with no path to the intact wall or a slab go too. A fringe past
//      the opening gets single bricks knocked in;
//   7. plaster: star-shaped spall patches out to spallReach, with the mortar
//      joints raked inside them so the brick bond shows;
//   8. cracks: stair-step cracks walked along the mortar joints out of the
//      opening, tapering.
// Candidates use SplitMix64 counter channels keyed by (seed, empty, channel),
// like concrete hit.
//
// Channel map, per empty: 0-9 noise offsets, 100 + k slab thresholds and
// droop, 200 + k room corners, 300 + k*1000 + g partitions, 1000 + f*100000
// facade channels (+ cell index for bricks, + 50000 + patch for spall,
// + 90000 + crack*64 for cracks).
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "building_destruction.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

double Random01(uint64_t seed, uint64_t candidate, uint64_t channel)
{
	uint64_t z = seed ^ (candidate * 0x9e3779b97f4a7c15ULL)
		^ ((channel + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}

V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Smooth(double e0, double e1, double x)
{
	if (e1 <= e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double Fbm(V q, int octaves)
{
	double sum = 0.0, amp = 1.0, norm = 0.0;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += amp;
		amp *= 0.5;
		q = q * 2.03 + V(17.13, -9.31, 5.77);
	}
	return sum / norm;
}

                                                                       
m::Manifold Orient(m::MeshGL& mesh)
{
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	if (solid.Status() != m::Manifold::Error::NoError) return m::Manifold();
	return solid;
}

                                                                          
                                                                            
                                                                           
                                            
m::Manifold StarPrism(const V& c, const V& e1, const V& e2, const V& e3, double h0, double h1,
	const std::vector<double>& ang, const std::vector<double>& rBot, const std::vector<double>& rTop)
{
	const size_t n = ang.size();
	if (n < 3 || !(h1 > h0)) return m::Manifold();
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	push(c + e3 * h0);
	push(c + e3 * h1);
	for (size_t i = 0; i < n; ++i) {
		const V d = e1 * std::cos(ang[i]) + e2 * std::sin(ang[i]);
		push(c + d * rBot[i] + e3 * h0);
		push(c + d * rTop[i] + e3 * h1);
	}
	auto B = [&](size_t i) { return uint32_t(2 + 2 * (i % n)); };
	auto T = [&](size_t i) { return uint32_t(3 + 2 * (i % n)); };
	for (size_t i = 0; i < n; ++i) {
		mesh.triVerts.insert(mesh.triVerts.end(), {0u, B(i + 1), B(i)});
		mesh.triVerts.insert(mesh.triVerts.end(), {1u, T(i), T(i + 1)});
		mesh.triVerts.insert(mesh.triVerts.end(), {B(i), B(i + 1), T(i + 1), B(i), T(i + 1), T(i)});
	}
	return Orient(mesh);
}

                                                                                     
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) return a;
	const V bp = p - b;
	const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
	const V cp = p - c;
	const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN, polyC;
	std::vector<double> polyA;
	ProjectDamageBVH bvh;
	V lo, hi;
	const std::vector<bool>* avoid = nullptr;

	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}
	bool Ray(const V& P, const V& dir, double maxDist, double& d, int32_t& poly) const
	{
		poly = -1;
		return bvh.Raycast(P, dir, maxDist, &d, &poly) && poly >= 0;
	}
	                                                                      
	bool Nearest(const V& p, V& q, int32_t& poly) const
	{
		double best = 1e300;
		poly = -1;
		for (int32_t i = 0; i < v->npolys; ++i) {
			const ProjectDamagePoly& f = v->polys[i];
			const V a = v->pts[f.a], b = v->pts[f.b], c = v->pts[f.c];
			V c0 = ClosestOnTri(p, a, b, c);
			double d0 = m::la::dot(c0 - p, c0 - p);
			if (f.c != f.d) {
				const V c1 = ClosestOnTri(p, a, c, v->pts[f.d]);
				const double d1 = m::la::dot(c1 - p, c1 - p);
				if (d1 < d0) { d0 = d1; c0 = c1; }
			}
			if (d0 < best) { best = d0; q = c0; poly = i; }
		}
		return poly >= 0;
	}
};

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, bool useAvoid, Surf& s)
{
	const MeshView& v = in.view;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	s.polyC.assign(size_t(v.npolys), V(0, 0, 0));
	s.polyA.assign(size_t(v.npolys), 0.0);
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		V cen = v.pts[q.a] + v.pts[q.b] + v.pts[q.c];
		double k = 3.0;
		if (q.c != q.d) {
			cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
			cen = cen + v.pts[q.d];
			k = 4.0;
		}
		s.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
		s.polyC[size_t(i)] = cen / k;
		s.polyA[size_t(i)] = 0.5 * m::la::length(cr);
	}
	s.bvh.Build(v);
	s.lo = V(1e300);
	s.hi = V(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { s.lo = m::la::min(s.lo, v.pts[i]); s.hi = m::la::max(s.hi, v.pts[i]); }
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                                    

                                                                          
                                                                           
struct Facade {
	V n, t;
	double d = 0.0, dLo = 0.0;                                                      
	double u0 = 0.0, u1 = 0.0, z0 = 0.0, z1 = 0.0;
	double area = 0.0;
	V near;                                                                   
	double nearD = 1e300;
};

                                                                         
                                                                        
struct Outline {
	std::vector<double> ang, rad;
	bool ok = false;

	                                                                               
	double RadiusAt(double th) const
	{
		if (!ok) return 0.0;
		const size_t n = ang.size();
		const double a0 = ang[0];
		double t = std::fmod(th - a0, 2.0 * m::kPi);
		if (t < 0.0) t += 2.0 * m::kPi;
		t += a0;
		size_t i = n - 1;
		for (size_t k = 0; k + 1 < n; ++k)
			if (t >= ang[k] && t < ang[k + 1]) { i = k; break; }
		const size_t j = (i + 1) % n;
		const double ax = rad[i] * std::cos(ang[i]), ay = rad[i] * std::sin(ang[i]);
		const double bx = rad[j] * std::cos(ang[j]), by = rad[j] * std::sin(ang[j]);
		const double dx = std::cos(th), dy = std::sin(th);
		const double den = dx * (by - ay) - dy * (bx - ax);
		if (std::abs(den) < 1e-12) return std::min(rad[i], rad[j]);
		return std::max(0.0, (ax * by - ay * bx) / den);
	}
	bool Inside(double x, double y, double cx, double cy) const
	{
		if (!ok) return false;
		const double dx = x - cx, dy = y - cy;
		return std::sqrt(dx * dx + dy * dy) < RadiusAt(std::atan2(dy, dx));
	}
};

struct Blast {
	uint64_t key = 0;
	V O;
	m::mat3 Minv;                                                         
	double R = 1.0, Rmax = 1.0;
	double I = 1.0;
	double jag = 0.0, feat = 1.0;
	V off;
	                                                                
	double thrCore = 0.0, thrRoom = 0.0, thrWall = 0.0, fringe = 0.0, thrSpall = 0.0, crackLen = 0.0;
	std::vector<Facade> facades;
	std::vector<Outline> rooms;                                          
	int mainFacade = -1;                                                                  

	double D(const V& x) const
	{
		const V q = Minv * (x - O);
		double d = m::la::length(q);
		if (jag > 0.0) d += jag * 0.35 * Fbm((x - O) / feat + off, 3);
		return d;
	}
};

struct Layout {
	double zmin = 0.0, zmax = 0.0;
	std::vector<double> slabTop;                                                
	double slabT = 1.0;
};

struct Ctx {
	const BuildingDestructionParams* p;
	const Surf* s;
	const Layout* L;
};

struct Out { std::vector<m::Manifold> holes, exposed, cracks; };

                                                                            
                                                              
double RadiusTo(const Blast& B, const V& c, const V& dir, double thr, double rmax, int steps)
{
	const double step = rmax / steps;
	double r = 0.0;
	for (int i = 0; i < steps; ++i) {
		const double r1 = r + step;
		if (B.D(c + dir * r1) >= thr) {
			double a = r, b = r1;
			for (int k = 0; k < 7; ++k) {
				const double mid = 0.5 * (a + b);
				if (B.D(c + dir * mid) >= thr) b = mid; else a = mid;
			}
			return 0.5 * (a + b);
		}
		r = r1;
	}
	return rmax;
}

int AngleCount(const BuildingDestructionParams& p) { return std::max(16, std::min(256, int(std::round(64.0 * std::sqrt(p.detail))))); }

                                                                         
                                           
bool HorizontalStar(const Blast& B, const BuildingDestructionParams& p, double z, double thr,
	std::vector<double>& ang, std::vector<double>& rad)
{
	const V c(B.O.x, B.O.y, z);
	if (thr <= 0.0 || B.D(c) >= thr) return false;
	const int n = AngleCount(p);
	ang.resize(size_t(n));
	rad.resize(size_t(n));
	const double rmax = 2.5 * B.Rmax;
	for (int i = 0; i < n; ++i) {
		ang[size_t(i)] = 2.0 * m::kPi * i / n;
		const V dir(std::cos(ang[size_t(i)]), std::sin(ang[size_t(i)]), 0.0);
		rad[size_t(i)] = std::max(1e-3 * B.R, RadiusTo(B, c, dir, thr, rmax, 48));
	}
	return true;
}

                                                                          
double ClosestZ(const Blast& B, double z0, double z1) { return ClampD(B.O.z, z0, z1); }

void Push(std::vector<m::Manifold>& dst, m::Manifold&& c)
{
	if (c.Status() == m::Manifold::Error::NoError && !c.IsEmpty()) dst.push_back(std::move(c));
}

                                
double SlabThr(const Blast& B, const BuildingDestructionParams& p, size_t k)
{
	if (B.thrRoom <= 0.0) return B.thrCore;
	const double r = Random01(B.key, 100 + uint64_t(k), 0);
	return Lerp(B.thrCore, std::max(B.thrCore, 0.97 * B.thrRoom), ClampD(p.slabBreak, 0.0, 1.0) * Lerp(0.25, 1.0, r));
}

                                                                                       

void BuildCore(const Ctx& cx, const Blast& B, Out& out)
{
	const double thr = B.thrCore;
	const double ext = B.Rmax * (thr + 0.4 * B.jag + 0.05);
	const m::Box box(B.O - V(ext), B.O + V(ext));
	const double edge = std::max(1e-3, B.R * thr / (10.0 * std::sqrt(cx.p->detail)));
	const Blast* Bp = &B;
	m::Manifold blob = m::Manifold::LevelSet([Bp, thr](V x) { return (thr - Bp->D(x)) * Bp->R; }, box, edge, 0.0, -1.0, false);
	Push(out.holes, std::move(blob));
}

                                                                                        

                                                                      
                                                                          
                                            
Outline RoomOutline(const Blast& B, const BuildingDestructionParams& p, const Layout& L, size_t k)
{
	Outline o;
	if (B.thrRoom <= 0.0 || k + 1 >= L.slabTop.size()) return o;
	const double z0 = L.slabTop[k], z1 = L.slabTop[k + 1] - L.slabT;
	if (!(z1 > z0)) return o;
	const V c(B.O.x, B.O.y, ClosestZ(B, z0, z1));
	if (B.D(c) >= B.thrRoom) return o;
	const int n = std::max(5, std::min(64, int(std::round(p.roomFacets))));
	const uint64_t key = 200 + uint64_t(k);
	const double rot = 2.0 * m::kPi * Random01(B.key, key, 0);
	o.ang.resize(size_t(n));
	o.rad.resize(size_t(n));
	for (int i = 0; i < n; ++i) {
		o.ang[size_t(i)] = rot + 2.0 * m::kPi * (i + 0.7 * (Random01(B.key, key, 1 + uint64_t(i)) - 0.5)) / n;
		const V dir(std::cos(o.ang[size_t(i)]), std::sin(o.ang[size_t(i)]), 0.0);
		o.rad[size_t(i)] = std::max(1e-3 * B.R, RadiusTo(B, c, dir, B.thrRoom, 2.5 * B.Rmax, 48));
	}
	o.ok = true;
	return o;
}

int StoreyAt(const Layout& L, double z)
{
	for (size_t k = 0; k + 1 < L.slabTop.size(); ++k)
		if (z >= L.slabTop[k] && z < L.slabTop[k + 1] - L.slabT) return int(k);
	return -1;
}

void BuildRoom(const Ctx& cx, const Blast& B, size_t k, Out& out)
{
	const Layout& L = *cx.L;
	const BuildingDestructionParams& p = *cx.p;
	const Outline& o = B.rooms[k];
	if (!o.ok) return;
	const double z0 = L.slabTop[k], z1 = L.slabTop[k + 1] - L.slabT;
	const double eps = 1e-3 * L.slabT;
	m::Manifold room = StarPrism(V(B.O.x, B.O.y, 0.0), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1), z0 - eps, z1 + eps, o.ang, o.rad, o.rad);
	                                               
	for (const Facade& f : B.facades) room = room.TrimByPlane(-f.n, -(f.d - p.wallDepth));
	                                                                         
	                                                 
	if (p.partitionSpacing > 0.0 && p.partitionThickness > 0.0 && B.mainFacade >= 0) {
		const Facade& f = B.facades[size_t(B.mainFacade)];
		double rm = 0.0;
		for (double r : o.rad) rm = std::max(rm, r);
		const double uO = m::la::dot(f.t, B.O);
		const double sp = std::max(p.partitionSpacing, 4.0 * p.partitionThickness);
		std::vector<m::Manifold> walls;
		for (int g = int(std::floor((uO - rm) / sp)); g <= int(std::ceil((uO + rm) / sp)); ++g) {
			const uint64_t key = 300 + uint64_t(k) * 1000 + uint64_t(uint32_t(g)) % 997u;
			auto rnd = [&](uint64_t ch) { return Random01(B.key, key, ch); };
			if (rnd(0) < 0.3) continue;
			const double u = g * sp, h = 0.5 * p.partitionThickness;
			const double front = p.wallDepth + rnd(1) * rnd(1) * 0.6 * B.R;
			const m::Box bx(V(u - h, z0 - 2.0 * eps, -(p.wallDepth + 4.0 * B.Rmax)), V(u + h, z1 + 2.0 * eps, -front));
			walls.push_back(m::Manifold::Cube(bx.Size()).Translate(bx.min)
				.Transform(m::mat3x4(f.t, V(0, 0, 1), f.n, f.n * f.d)));
		}
		if (!walls.empty()) room = room - m::Manifold::BatchBoolean(walls, m::OpType::Add);
	}
	Push(out.holes, std::move(room));
}

                                                                             
                                                                            
                                  
void BuildSlab(const Ctx& cx, const Blast& B, size_t k, Out& out)
{
	const Layout& L = *cx.L;
	const BuildingDestructionParams& p = *cx.p;
	const double zt = L.slabTop[k], zb = zt - L.slabT;
	if (zt <= L.zmin + 1e-6) return;
	std::vector<double> ang, rad;
	if (!HorizontalStar(B, p, ClosestZ(B, zb, zt), SlabThr(B, p, k), ang, rad)) return;
	const Outline* below = k > 0 ? &B.rooms[k - 1] : nullptr;
	const Outline* above = k + 1 < L.slabTop.size() ? &B.rooms[k] : nullptr;                            
	if (below && !below->ok && above && !above->ok) return;
	const double droop = ClampD(p.slabDroop, 0.0, 1.0) * std::min(5.0 * L.slabT, 0.25 * B.R);
	std::vector<double> bot(rad.size()), top(rad.size());
	bool any = false;
	for (size_t i = 0; i < rad.size(); ++i) {
		const double rb = below ? 0.98 * below->RadiusAt(ang[i]) : 1e300;
		const double ra = above ? 0.98 * above->RadiusAt(ang[i]) : 1e300;
		bot[i] = std::max(1e-3 * B.R, std::min({rad[i], rb, ra}));
		top[i] = std::max(bot[i], std::min(rad[i] + droop * Lerp(0.4, 1.0, Random01(B.key, 100 + uint64_t(k), 10 + i % 7)), ra));
		any |= bot[i] > 0.02 * B.R;
	}
	if (!any) return;
	const double eps = 0.02 * L.slabT;
	m::Manifold cut = StarPrism(V(B.O.x, B.O.y, 0.0), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1), zb - eps, zt + eps, ang, bot, top);
	Push(out.holes, std::move(cut));
}

                                                                                          
  
                                                                           
                                                                             
                      

struct Bond {
	double bl, ch, joint, zb;
	int Course(double z) const { return int(std::floor((z - zb) / ch)); }
	double Shift(int c) const { return (c & 1) ? 0.5 * bl : 0.0; }
	double CellU0(int c, int j) const { return j * bl + Shift(c); }
	int CellAt(int c, double u) const { return int(std::floor((u - Shift(c)) / bl)); }
	double CourseZ0(int c) const { return zb + c * ch; }
};

Bond MakeBond(const BuildingDestructionParams& p, const Layout& L)
{
	Bond b;
	b.joint = std::max(0.0, p.jointWidth);
	b.bl = std::max(1e-3, p.brickLength + b.joint);
	b.ch = std::max(1e-3, p.brickHeight + b.joint);
	b.zb = L.zmin;
	return b;
}

                                                                          
int SlabAt(const Layout& L, double z0, double z1)
{
	for (size_t k = 0; k < L.slabTop.size(); ++k) {
		const double st = L.slabTop[k], sb = st - L.slabT;
		if (std::min(z1, st) - std::max(z0, sb) > 0.5 * (z1 - z0)) return int(k);
	}
	return -1;
}

V OnFacade(const Facade& f, double u, double z, double h) { return f.n * (f.d + h) + f.t * u + V(0, 0, z); }

m::CrossSection Rect2(double u0, double u1, double z0, double z1)
{
	if (!(u1 > u0) || !(z1 > z0)) return m::CrossSection();
	return m::CrossSection(m::Rect(m::vec2(u0, z0), m::vec2(u1, z1)));
}

m::CrossSection Union2(std::vector<m::CrossSection>& parts)
{
	if (parts.empty()) return m::CrossSection();
	return m::CrossSection::BatchBoolean(parts, m::OpType::Add);
}

                                                                             
m::Manifold ExtrudeFacade(const Facade& f, const m::CrossSection& cs, double h0, double h1)
{
	if (cs.IsEmpty() || !(h1 > h0)) return m::Manifold();
	m::Manifold e = m::Manifold::Extrude(cs.ToPolygons(), h1 - h0);
	return e.Transform(m::mat3x4(f.t, V(0, 0, 1), f.n, f.n * (f.d + h0)));
}

                                                                               
bool FacadeWindow(const Facade& f, const Blast& B, double reachN, double& ua, double& ub, double& za, double& zb)
{
	const double reach = B.Rmax * (reachN + 0.4 * B.jag + 0.1);
	const double uO = m::la::dot(f.t, B.O);
	ua = std::max(f.u0, uO - reach);
	ub = std::min(f.u1, uO + reach);
	za = std::max(f.z0, B.O.z - reach);
	zb = std::min(f.z1, B.O.z + reach);
	return ub > ua && zb > za;
}

void BuildBricks(const Ctx& cx, const Blast& B, size_t fi, Out& out)
{
	const BuildingDestructionParams& p = *cx.p;
	const Layout& L = *cx.L;
	const Facade& f = B.facades[fi];
	const Bond bd = MakeBond(p, L);
	double ua, ub, za, zt;
	if (!FacadeWindow(f, B, B.thrWall + B.fringe, ua, ub, za, zt)) return;
	const double margin = 2.0 * bd.ch + p.plasterDepth;
	const double eps = 0.01 * bd.joint + 1e-4;
	const double jit = std::max(0.0, p.brickJitter);
	const uint64_t fkey = 1000 + uint64_t(fi) * 100000;
	const int c0 = bd.Course(za), c1 = bd.Course(zt);
	const int nc = c1 - c0 + 1;
	if (nc <= 0 || nc > 20000) return;
	                                                               
	std::vector<int> jlo(static_cast<size_t>(nc)), jhi(static_cast<size_t>(nc)), slabOf(static_cast<size_t>(nc));
	std::vector<std::vector<uint8_t>> st(static_cast<size_t>(nc));
	std::vector<std::vector<double>> dj(static_cast<size_t>(nc));
	const double fine = std::max(1e-3, 0.1 * B.R);
	for (int ci = 0; ci < nc; ++ci) {
		const int c = c0 + ci;
		const double z0 = bd.CourseZ0(c), z1 = z0 + bd.ch;
		const int slab = SlabAt(L, z0, z1);
		slabOf[size_t(ci)] = slab;
		const double sthr = slab >= 0 ? SlabThr(B, p, size_t(slab)) : 0.0;
		jlo[size_t(ci)] = bd.CellAt(c, ua);
		jhi[size_t(ci)] = bd.CellAt(c, ub);
		const size_t n = size_t(jhi[size_t(ci)] - jlo[size_t(ci)] + 1);
		st[size_t(ci)].assign(n, 0);
		dj[size_t(ci)].assign(n, 1e300);
		for (size_t k = 0; k < n; ++k) {
			const int j = jlo[size_t(ci)] + int(k);
			const double u0 = bd.CellU0(c, j);
			const V P = OnFacade(f, u0 + 0.5 * bd.bl, 0.5 * (z0 + z1), 0.0);
			const uint64_t cell = fkey + uint64_t(uint32_t(c * 7919 + j * 104729)) % 49000u;
			const double Dc = B.D(P);
			if (slab >= 0) { st[size_t(ci)][k] = Dc < sthr ? 1 : 2; continue; }
			const double Dj = Dc + 0.08 * B.jag * Fbm((P - B.O) / fine + B.off * 1.37, 2) + jit * (2.0 * Random01(B.key, cell, 0) - 1.0);
			dj[size_t(ci)][k] = Dj;
			bool open = Dj < B.thrWall;
			if (open && Dc >= B.thrCore) {
				                                                                   
				const V Q = OnFacade(f, u0 + 0.5 * bd.bl, 0.5 * (z0 + z1), -(p.wallDepth + 0.5 * bd.bl));
				const int sk = StoreyAt(L, Q.z);
				open = sk >= 0 && B.rooms[size_t(sk)].Inside(Q.x, Q.y, B.O.x, B.O.y);
			}
			st[size_t(ci)][k] = open ? 1 : 0;
		}
	}
	                                                                             
	{
		std::vector<std::vector<uint8_t>> seen(static_cast<size_t>(nc));
		std::vector<std::pair<int, int>> stack;
		for (int ci = 0; ci < nc; ++ci) {
			const size_t n = st[size_t(ci)].size();
			seen[size_t(ci)].assign(n, 0);
			for (size_t k = 0; k < n; ++k) {
				const bool edge = ci == 0 || ci == nc - 1 || k == 0 || k + 1 == n;
				if (st[size_t(ci)][k] != 1 && (edge || st[size_t(ci)][k] == 2)) { seen[size_t(ci)][k] = 1; stack.push_back({ci, int(k)}); }
			}
		}
		auto visit = [&](int ci, int k) {
			if (ci < 0 || ci >= nc || k < 0 || size_t(k) >= st[size_t(ci)].size()) return;
			if (seen[size_t(ci)][size_t(k)] || st[size_t(ci)][size_t(k)] == 1) return;
			seen[size_t(ci)][size_t(k)] = 1;
			stack.push_back({ci, k});
		};
		while (!stack.empty()) {
			const auto [ci, k] = stack.back();
			stack.pop_back();
			visit(ci, k - 1);
			visit(ci, k + 1);
			const int c = c0 + ci, j = jlo[size_t(ci)] + k;
			const double u0 = bd.CellU0(c, j), u1 = u0 + bd.bl;
			for (int dc : {-1, 1}) {
				const int cn = ci + dc;
				if (cn < 0 || cn >= nc) continue;
				const int ja = bd.CellAt(c0 + cn, u0 + 1e-6), jb = bd.CellAt(c0 + cn, u1 - 1e-6);
				for (int jj = ja; jj <= jb; ++jj) visit(cn, jj - jlo[size_t(cn)]);
			}
		}
		for (int ci = 0; ci < nc; ++ci)
			for (size_t k = 0; k < st[size_t(ci)].size(); ++k)
				if (st[size_t(ci)][k] == 0 && !seen[size_t(ci)][k]) st[size_t(ci)][k] = 1;
	}
	                                                                               
	std::vector<m::CrossSection> runs, pockets[3];
	const double fd = ClampD(p.fringeDensity, 0.0, 1.0);
	for (int ci = 0; ci < nc; ++ci) {
		const int c = c0 + ci;
		const double z0 = bd.CourseZ0(c), z1 = z0 + bd.ch;
		const size_t n = st[size_t(ci)].size();
		size_t k = 0;
		while (k < n) {
			if (st[size_t(ci)][k] != 1) {
				const double Dj = dj[size_t(ci)][k];
				if (st[size_t(ci)][k] == 0 && B.fringe > 0.0 && Dj < B.thrWall + B.fringe) {
					const int j = jlo[size_t(ci)] + int(k);
					const uint64_t cell = fkey + uint64_t(uint32_t(c * 7919 + j * 104729)) % 49000u;
					const double tf = ClampD((Dj - B.thrWall) / B.fringe, 0.0, 1.0);
					if (Random01(B.key, cell, 1) < fd * std::pow(1.0 - tf, 1.5)) {
						const int b = std::min(2, int(Random01(B.key, cell, 2) * 3.0));
						const double u0 = bd.CellU0(c, j);
						pockets[b].push_back(Rect2(u0 + bd.joint, u0 + bd.bl, z0 + bd.joint, z1));
					}
				}
				++k;
				continue;
			}
			size_t e = k;
			while (e < n && st[size_t(ci)][e] == 1) ++e;
			const double u0 = bd.CellU0(c, jlo[size_t(ci)] + int(k)), u1 = bd.CellU0(c, jlo[size_t(ci)] + int(e));
			runs.push_back(Rect2(std::max(u0, f.u0 - margin) - eps, std::min(u1, f.u1 + margin) + eps, z0 - eps, z1 + eps));
			k = e;
		}
	}
	Push(out.holes, ExtrudeFacade(f, Union2(runs), -p.wallDepth - eps, margin));
	const double depth[3] = {0.3, 0.55, 0.85};
	for (int b = 0; b < 3; ++b) Push(out.exposed, ExtrudeFacade(f, Union2(pockets[b]), -p.wallDepth * depth[b], margin));
}

                                                                   
void BuildSpall(const Ctx& cx, const Blast& B, size_t fi, Out& out)
{
	const BuildingDestructionParams& p = *cx.p;
	const Layout& L = *cx.L;
	const Facade& f = B.facades[fi];
	if (p.plasterDepth <= 0.0 || p.spallCover <= 0.0) return;
	const Bond bd = MakeBond(p, L);
	double ua, ub, za, zt;
	if (!FacadeWindow(f, B, B.thrSpall, ua, ub, za, zt)) return;
	const double pr = std::max(4.0 * bd.bl, 0.2 * B.R);
	const double sp = 1.1 * pr;
	const double margin = 2.0 * bd.ch + p.plasterDepth;
	const double deep = p.plasterDepth + std::max(0.0, p.jointDepth);
	const uint64_t fkey = 1000 + uint64_t(fi) * 100000 + 50000;
	const double inner = 0.6 * B.thrWall;
	std::vector<m::CrossSection> patches;
	const int nu = int(std::ceil((ub - ua) / sp)), nz = int(std::ceil((zt - za) / sp));
	if (nu > 4000 || nz > 4000) return;
	const int nAng = std::max(12, int(std::round(28.0 * std::sqrt(p.detail))));
	for (int iz = 0; iz <= nz; ++iz)
		for (int iu = 0; iu <= nu; ++iu) {
			const uint64_t cand = fkey + uint64_t(iz * (nu + 1) + iu) % 39000u;
			auto rnd = [&](uint64_t ch) { return Random01(B.key, cand, ch); };
			const double u = ua + (iu + rnd(0) - 0.5) * sp, z = za + (iz + rnd(1) - 0.5) * sp;
			const double Dc = B.D(OnFacade(f, u, z, 0.0));
			if (Dc >= B.thrSpall) continue;
			const double t = ClampD((Dc - inner) / std::max(1e-6, B.thrSpall - inner), 0.0, 1.0);
			if (rnd(2) >= ClampD(p.spallCover, 0.0, 1.0) * 1.3 * (1.0 - Smooth(0.3, 1.0, t))) continue;
			const double r0 = pr * Lerp(0.5, 1.1, rnd(3)) * Lerp(1.0, 0.55, t);
			const V noff(rnd(4) * 500.0, rnd(5) * 500.0, rnd(6) * 500.0);
			m::SimplePolygon poly;
			for (int a = 0; a < nAng; ++a) {
				const double th = 2.0 * m::kPi * a / nAng;
				const double ca = std::cos(th), sa = std::sin(th);
				const double r = r0 * std::max(0.2, 1.0 + 0.55 * Fbm(V(ca * 1.3, sa * 1.3, 0.0) + noff, 2)
					+ 0.25 * Fbm(V(ca * 4.0, sa * 4.0, 7.0) + noff, 2));
				poly.push_back(m::vec2(u + ca * r, z + sa * r));
			}
			patches.push_back(m::CrossSection(poly));
		}
	if (patches.empty()) return;
	m::CrossSection S = Union2(patches);
	if (S.IsEmpty()) return;
	Push(out.exposed, ExtrudeFacade(f, S, -p.plasterDepth, margin));
	if (p.jointDepth <= 0.0 || bd.joint <= 0.0) return;
	                                                                
	const m::Rect bb = S.Bounds();
	const int ca0 = bd.Course(bb.min.y), ca1 = bd.Course(bb.max.y);
	if (ca1 - ca0 > 20000) return;
	std::vector<m::CrossSection> joints;
	for (int c = ca0; c <= ca1; ++c) {
		const double z0 = bd.CourseZ0(c);
		joints.push_back(Rect2(bb.min.x, bb.max.x, z0, z0 + bd.joint));
		const int j0 = bd.CellAt(c, bb.min.x), j1 = bd.CellAt(c, bb.max.x);
		if (j1 - j0 > 20000) continue;
		for (int j = j0; j <= j1 + 1; ++j) {
			const double hu = bd.CellU0(c, j);
			joints.push_back(Rect2(hu, hu + bd.joint, z0, z0 + bd.ch));
		}
	}
	Push(out.exposed, ExtrudeFacade(f, Union2(joints) ^ S, -deep, margin));
}

                                                         
void BuildCrack(const Ctx& cx, const Blast& B, size_t fi, int ci, int count, Out& out)
{
	const BuildingDestructionParams& p = *cx.p;
	const Layout& L = *cx.L;
	const Facade& f = B.facades[fi];
	const Bond bd = MakeBond(p, L);
	const uint64_t key = 1000 + uint64_t(fi) * 100000 + 90000 + uint64_t(ci) * 64;
	auto rnd = [&](uint64_t ch) { return Random01(B.key, key, ch); };
	const double phi = 2.0 * m::kPi * (ci + 0.8 * rnd(0)) / count;
	const double du = std::cos(phi), dz = std::sin(phi);
	                                                                         
	const double uO = m::la::dot(f.t, B.O);
	const double startThr = B.thrWall + 0.35 * B.fringe;
	const double step = 0.5 * bd.ch;
	double u = uO, z = B.O.z;
	bool ok = false;
	for (int i = 0; i < 20000 && i * step < 3.0 * B.Rmax; ++i) {
		u += du * step;
		z += dz * step;
		if (u < f.u0 || u > f.u1 || z < f.z0 || z > f.z1) continue;
		if (B.D(OnFacade(f, u, z, 0.0)) >= startThr) { ok = true; break; }
	}
	if (!ok) return;
	const double su = du >= 0.0 ? 1.0 : -1.0, sz = dz >= 0.0 ? 1.0 : -1.0;
	const double len = p.crackLength * B.R * B.crackLen * Lerp(0.5, 1.0, rnd(1));
	if (len < 2.0 * bd.bl) return;
	                                                                         
	const double slope = std::abs(dz) / std::max(1e-3, std::abs(du));
	const double kf = ClampD(bd.ch / (slope * bd.bl) - 0.5, 0.0, 6.0);
	const int c = bd.Course(z);
	double zj = sz > 0.0 ? bd.CourseZ0(c + 1) : bd.CourseZ0(c);                           
	const int first = sz > 0.0 ? c + 1 : c - 1;                                            
	double uj = bd.CellU0(first, bd.CellAt(first, u));
	if (su > 0.0 && uj < u) uj += bd.bl;
	if (su < 0.0 && uj > u) uj -= bd.bl;
	std::vector<m::CrossSection> segs;
	double done = 0.0, cu = u;
	auto seg = [&](double u0, double u1, double z0, double z1) {
		const double w = 0.5 * std::max(0.05, p.crackWidth * (1.0 - 0.75 * ClampD(done / len, 0.0, 1.0)));
		segs.push_back(Rect2(std::min(u0, u1) - w, std::max(u0, u1) + w, std::min(z0, z1) - w, std::max(z0, z1) + w));
	};
	for (int it = 0; it < 2000 && done < len; ++it) {
		const double jz = zj + 0.5 * bd.joint;                           
		const int k = int(std::floor(kf + rnd(100 + uint64_t(it) % 50)));
		const double nu = uj + su * k * bd.bl;
		seg(cu, nu, jz, jz);
		done += std::abs(nu - cu);
		cu = nu;
		const double nz = zj + sz * bd.ch;
		seg(cu + 0.5 * bd.joint, cu + 0.5 * bd.joint, jz, nz + 0.5 * bd.joint);
		done += bd.ch;
		zj = nz;
		uj = cu + su * 0.5 * bd.bl;                                                          
		if (cu < f.u0 || cu > f.u1 || zj < f.z0 || zj > f.z1) break;
		if (B.D(OnFacade(f, cu, zj, 0.0)) < B.thrWall) break;                                  
	}
	const double margin = 2.0 * bd.ch + p.plasterDepth;
	const double dep = std::max(0.1, p.crackDepth) * Lerp(0.8, 1.2, rnd(2));
	Push(out.cracks, ExtrudeFacade(f, Union2(segs), -dep, margin));
}


                                                                     
void FindFacades(const Surf& s, const BuildingDestructionParams& p, const Layout& L, Blast& B)
{
	const MeshView& v = *s.v;
	const double reach = B.Rmax * (B.thrSpall + 0.4 * B.jag + 0.15);
	const double tolD = std::max(p.wallDepth, 1e-3 * (s.hi.z - s.lo.z) + 1e-4);
	std::vector<Facade> cl;
	std::vector<int> owner(size_t(v.npolys), -1);
	for (int32_t i = 0; i < v.npolys; ++i) {
		const V& c = s.polyC[size_t(i)];
		{
			                                                                              
			const ProjectDamagePoly& q = v.polys[i];
			V plo = m::la::min(m::la::min(v.pts[q.a], v.pts[q.b]), m::la::min(v.pts[q.c], v.pts[q.d]));
			V phi = m::la::max(m::la::max(v.pts[q.a], v.pts[q.b]), m::la::max(v.pts[q.c], v.pts[q.d]));
			const double g = reach + 2.0 * tolD;                                          
			if (plo.x > B.O.x + g || phi.x < B.O.x - g || plo.y > B.O.y + g || phi.y < B.O.y - g
				|| plo.z > B.O.z + g || phi.z < B.O.z - g) continue;
		}
		const V& n = s.polyN[size_t(i)];
		if (std::abs(n.z) > 0.3 || s.Avoided(i)) continue;
		const V nh = Unit(V(n.x, n.y, 0.0), V(1, 0, 0));
		const double a = s.polyA[size_t(i)];
		const double off = m::la::dot(nh, c);
		int hit = -1;
		for (size_t k = 0; k < cl.size(); ++k) {
			const V cn = Unit(cl[k].n, nh);
			                                                                       
			if (m::la::dot(cn, nh) > 0.985 && off > cl[k].dLo - tolD && off < cl[k].d + tolD) { hit = int(k); break; }
		}
		if (hit < 0) {
			cl.push_back(Facade());
			hit = int(cl.size() - 1);
			cl[size_t(hit)].n = V(0, 0, 0);
			cl[size_t(hit)].d = cl[size_t(hit)].dLo = off;
		}
		Facade& F = cl[size_t(hit)];
		F.n = F.n + nh * a;
		F.d = std::max(F.d, off);                                                
		F.dLo = std::min(F.dLo, off);
		F.area += a;
		owner[size_t(i)] = hit;
		const double dd = m::la::length(c - B.O);
		if (dd < F.nearD) { F.nearD = dd; F.near = c; }
	}
	for (Facade& F : cl) {
		F.n = Unit(F.n, V(1, 0, 0));
		F.t = m::la::cross(V(0, 0, 1), F.n);
		F.u0 = F.z0 = 1e300;
		F.u1 = F.z1 = -1e300;
	}
	for (int32_t i = 0; i < v.npolys; ++i) {
		const int k = owner[size_t(i)];
		if (k < 0) continue;
		Facade& F = cl[size_t(k)];
		const ProjectDamagePoly& q = v.polys[i];
		const int32_t ids[4] = {q.a, q.b, q.c, q.d};
		for (int32_t id : ids) {
			const V& P = v.pts[id];
			const double u = m::la::dot(F.t, P);
			F.u0 = std::min(F.u0, u); F.u1 = std::max(F.u1, u);
			F.z0 = std::min(F.z0, P.z); F.z1 = std::max(F.z1, P.z);
		}
	}
	const double minArea = std::pow(std::max(0.12 * B.R, 2.0 * (p.brickLength + p.jointWidth)), 2.0);
	for (Facade& F : cl) {
		if (F.area < minArea) continue;
		if (std::abs(m::la::dot(F.n, B.O) - F.d) > reach) continue;
		                                                                              
		double dist = 0.0;
		int32_t poly = -1;
		if (s.Ray(F.near + F.n * (1e-3 * L.slabT + 1e-4), F.n, 1.5 * p.floorHeight, dist, poly)) continue;
		B.facades.push_back(F);
	}
}

}               

void PlaceBuildingDestruction(const CoreInput& in, const BuildingDestructionParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& exposed,
	std::vector<m::Manifold>& cracks)
{
	if (!pIn.enable || pIn.empties.empty()) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	BuildingDestructionParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.floorHeight = std::max(1e-3, p.floorHeight);
	p.slabThickness = ClampD(p.slabThickness, 1e-3, 0.9 * p.floorHeight);
	p.wallDepth = std::max(1e-3, p.wallDepth);
	p.brickLength = std::max(1e-3, p.brickLength);
	p.brickHeight = std::max(1e-3, p.brickHeight);
	p.jointWidth = std::max(0.0, p.jointWidth);
	p.plasterDepth = std::max(0.0, p.plasterDepth);
	p.crackCount = std::max(0, std::min(p.crackCount, 64));
	p.noiseScale = std::max(0.02, p.noiseScale);

	Surf s;
	BuildSurf(in, p.avoid, s);
	Layout L;
	L.zmin = s.lo.z;
	L.zmax = s.hi.z;
	L.slabT = p.slabThickness;
	for (double z = L.zmin + std::max(0.0, p.floorOffset); z < L.zmax - 0.5 * p.floorHeight && L.slabTop.size() < 4000; z += p.floorHeight)
		L.slabTop.push_back(z);
	L.slabTop.push_back(L.zmax);          
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x424c415354424c44ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4244;                             
	Ctx cx{&p, &s, &L};

	std::vector<Blast> blasts;
	blasts.reserve(p.empties.size());
	for (size_t ei = 0; ei < p.empties.size(); ++ei) {
		const BuildingBlastEmpty& E = p.empties[ei];
		bool finite = std::isfinite(E.size) && std::isfinite(E.strength);
		for (double q : E.rel) finite = finite && std::isfinite(q);
		if (!finite || E.strength <= 0.0 || E.size <= 0.0) continue;
		const V O(E.rel[0], E.rel[1], E.rel[2]);
		const V X(E.rel[3], E.rel[4], E.rel[5]), Y(E.rel[6], E.rel[7], E.rel[8]), Z(E.rel[9], E.rel[10], E.rel[11]);
		const double base = E.size * p.radiusScale;
		const double lx = m::la::length(X), ly = m::la::length(Y), lz = m::la::length(Z);
		if (!(base > 1e-6) || !(lx > 1e-9) || !(ly > 1e-9) || !(lz > 1e-9)) continue;
		const m::mat3 Mx(X, Y, Z);
		if (std::abs(m::la::determinant(Mx)) < 1e-12) continue;
		Blast B;
		B.key = seed ^ ((uint64_t(ei) + 1) * 0xa24baed4963ee407ULL);
		auto rnd = [&](uint64_t ch) { return Random01(B.key, 0, ch); };
		V S;
		int32_t poly = -1;
		if (!s.Nearest(O, S, poly) || poly < 0 || s.Avoided(poly)) continue;
		const double R0 = base * (lx + ly + lz) / 3.0;
		double I = ClampD(E.strength, 0.0, 4.0);
		if (p.proximity > 0.0) I *= 1.0 - Smooth(0.25 * p.proximity * R0, p.proximity * R0, m::la::length(S - O));
		if (I <= 0.02) continue;
		if (mask && !mask->Keep(S, int32_t(ei), kStream)) continue;
		const double sc = mask ? mask->SizeScale(S) : 1.0;
		if (sc <= 0.0) continue;
		const double sizeF = sc * Lerp(1.0, std::sqrt(I), ClampD(p.proxShrink, 0.0, 1.0));
		B.O = O;
		B.Minv = m::la::inverse(Mx * (base * sizeF));
		B.R = R0 * sizeF;
		B.Rmax = base * sizeF * std::max(lx, std::max(ly, lz));
		B.I = I;
		B.jag = ClampD(p.jagged, 0.0, 2.0);
		B.feat = std::max(1e-3, p.noiseScale * B.R);
		B.off = V(rnd(1) * 1000.0, rnd(2) * 1000.0, rnd(3) * 1000.0);
		                                                   
		B.thrCore = std::max(0.0, p.coreSize) * Smooth(0.5, 0.9, I);
		B.thrRoom = std::max(0.0, p.roomSize) * Smooth(0.3, 0.7, I);
		B.thrWall = std::max(0.0, p.wallSize) * Smooth(0.25, 0.6, I);
		B.fringe = std::max(0.0, p.fringe) * Smooth(0.1, 0.5, I);
		B.thrSpall = std::max(0.0, p.spallReach) * Smooth(0.0, 0.3, I);
		B.crackLen = Smooth(0.05, 0.4, I);
		FindFacades(s, p, L, B);
		double best = 1e300;
		for (size_t fi = 0; fi < B.facades.size(); ++fi) {
			const double dd = std::abs(m::la::dot(B.facades[fi].n, B.O) - B.facades[fi].d);
			if (dd < best) { best = dd; B.mainFacade = int(fi); }
		}
		B.rooms.resize(L.slabTop.size());
		for (size_t k = 0; k + 1 < L.slabTop.size(); ++k) {
			const double zr = B.Rmax * (B.thrRoom + 0.4 * B.jag + 0.1);
			if (L.slabTop[k + 1] - L.slabT < B.O.z - zr || L.slabTop[k] > B.O.z + zr) continue;
			B.rooms[k] = RoomOutline(B, p, L, k);
		}
		blasts.push_back(std::move(B));
	}
	if (blasts.empty()) return;

	std::vector<std::function<void(Out&)>> jobs;
	for (size_t bi = 0; bi < blasts.size(); ++bi) {
		const Blast* Bp = &blasts[bi];
		const Blast& B = *Bp;
		if (B.thrCore > 0.03) jobs.push_back([&cx, Bp](Out& o) { BuildCore(cx, *Bp, o); });
		const double zr = B.Rmax * (std::max(B.thrRoom, B.thrCore) + 0.4 * B.jag + 0.1);
		for (size_t k = 0; k < L.slabTop.size(); ++k) {
			const double st = L.slabTop[k];
			if (st - L.slabT > B.O.z + zr || st < B.O.z - zr) continue;
			if (B.thrRoom > 0.0 || B.thrCore > 0.0) jobs.push_back([&cx, Bp, k](Out& o) { BuildSlab(cx, *Bp, k, o); });
			if (B.rooms[k].ok) jobs.push_back([&cx, Bp, k](Out& o) { BuildRoom(cx, *Bp, k, o); });
		}
		for (size_t fi = 0; fi < B.facades.size(); ++fi) {
			if (B.thrWall > 0.0 || B.fringe > 0.0) jobs.push_back([&cx, Bp, fi](Out& o) { BuildBricks(cx, *Bp, fi, o); });
			if (B.thrSpall > 0.0) jobs.push_back([&cx, Bp, fi](Out& o) { BuildSpall(cx, *Bp, fi, o); });
			if (p.crackEnable && B.crackLen > 0.0)
				for (int ci = 0; ci < p.crackCount; ++ci)
					jobs.push_back([&cx, Bp, fi, ci, n = p.crackCount](Out& o) { BuildCrack(cx, *Bp, fi, ci, n, o); });
		}
	}

	std::vector<Out> outs(jobs.size());
	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t i) { jobs[size_t(i)](outs[size_t(i)]); });
	for (Out& o : outs) {
		for (auto& c : o.holes) holes.push_back(std::move(c));
		for (auto& c : o.exposed) exposed.push_back(std::move(c));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
	}
}
PROJECT_DAMAGE_NS_MACRO_END
