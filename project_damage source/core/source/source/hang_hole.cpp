// пробиваем отверстия и оставляем в них арматуру и кабели
#include "rebar_lod.h"
// ProjectDamage hanging-rebar holes — GPL-3.0-or-later; see LICENSE.
//
// Recipe hang-hole-v1 (HangHoleParams): holes blown through a wall / slab
// with the reinforcement and cables hanging out of them (bomb-damaged
// ceilings, breached walls).
//   Holes - Auto: area-weighted sites; at each the wall thickness is measured
//     by a ray; a closed cutter is lofted through it: a ragged outline
//     (Jagged), wider at the face it was hit from (Flare), rough walls
//     (procedural noise, Roughness), optionally an image displacement on top
//     (Texture, like the breakers; only vertices inside the object move).
//     Breakers: the holes are the linked Boolean breakers' cuts; this module
//     only adds the bars and cables, using the real opening of each cut
//     (breaker ∩ object, sliced at the depth needed).
//   Bars - two families in the wall at Bar Depth, Bar Spacing apart, turned at
//     random; a bar crossing the opening is intact (slightly bowed) or snapped
//     at a random point - each loose end leaves the concrete at the rim, bends
//     and hangs along gravity (Droop).
//   Cables - thinner and floppier: hanging from rim points into and out of the
//     hole, or slung as a loop between two rim points (Sag).
// Bars and cables are closed tubes, returned with the break rebar mesh.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "hang_hole.h"
#include "surface_noise.h"
#include "break_texture.h"
#include "project_damage_bvh.h"
#include "project_damage_internal.h"
#include <algorithm>
#include <cmath>
#include <vector>

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
V Unit(const V& v, const V& fb) { const double l = m::la::length(v); return l > 1e-12 ? v / l : fb; }
                                                             
double RingNoise(double a01, int cells, uint64_t seed, uint64_t ch)
{
	const double x = a01 * cells, f = std::floor(x), t = x - f;
	const int i0 = ((int)f % cells + cells) % cells, i1 = (i0 + 1) % cells;
	const double s = t * t * (3 - 2 * t);
	return Lerp(R01(seed, (uint64_t)i0, ch) * 2 - 1, R01(seed, (uint64_t)i1, ch) * 2 - 1, s);
}
double Noise3(const V& p, uint64_t seed)
{
	const V f(std::floor(p.x), std::floor(p.y), std::floor(p.z));
	const V t = p - f;
	const V s(t.x * t.x * (3 - 2 * t.x), t.y * t.y * (3 - 2 * t.y), t.z * t.z * (3 - 2 * t.z));
	auto h = [&](int i, int j, int k) {
		return R01(seed, uint64_t(int64_t(f.x) + i + 4096) * 73856093ULL ^ uint64_t(int64_t(f.y) + j + 4096) * 19349663ULL, uint64_t(int64_t(f.z) + k + 4096)) * 2 - 1;
	};
	const double x00 = Lerp(h(0, 0, 0), h(1, 0, 0), s.x), x10 = Lerp(h(0, 1, 0), h(1, 1, 0), s.x);
	const double x01 = Lerp(h(0, 0, 1), h(1, 0, 1), s.x), x11 = Lerp(h(0, 1, 1), h(1, 1, 1), s.x);
	return Lerp(Lerp(x00, x10, s.y), Lerp(x01, x11, s.y), s.z);
}

                                                                           
                                                                             
                                                                           
double WallNoise(V w, double R, uint64_t key, const HangHoleParams& p)
{
	const double ns = std::clamp(p.noiseScale, 0.05, 20.0);
	const double detail = std::clamp(p.noiseDetail, 1.0, 8.0), gain = std::clamp(p.noiseRoughness, 0.0, 0.95);
	const double f0 = std::max(1e-6, 0.25 * R * ns);
	if (p.noiseDistortion > 0.0) w = pd_surface_noise::Warp(w, f0, p.noiseDistortion, V(3.1, -7.7, 11.3));
	double nz = Noise3(w / f0, key) * 0.7;
	double feat = 0.08 * R * ns, amp = 0.6;
	for (int k = 1; k < 8 && double(k) < detail; ++k)
	{
		amp *= gain;
		const double weight = std::min(1.0, detail - double(k));
		nz += Noise3(w / std::max(1e-6, feat), k == 1 ? key ^ 77 : key ^ (77ULL * uint64_t(k + 1) + 0x5bd1e995ULL)) * (weight < 1.0 ? amp * weight : amp);
		feat /= 3.125;
	}
	return nz;
}

m::Manifold Tube(const std::vector<V>& path, double r0, double r1, int sides)
{
	const size_t ns = path.size();
	if (ns < 2) return m::Manifold();
	m::MeshGL64 mg;
	mg.numProp = 3;
	V prevN = m::la::cross(Unit(path[1] - path[0], V(1, 0, 0)), V(0, 0, 1));
	if (m::la::length(prevN) < 1e-6) prevN = V(1, 0, 0);
	for (size_t i = 0; i < ns; ++i)
	{
		const V t = Unit(path[std::min(ns - 1, i + 1)] - path[i > 0 ? i - 1 : 0], V(1, 0, 0));
		const V nx = Unit(prevN - t * m::la::dot(prevN, t), m::la::cross(t, V(0, 0, 1)));
		const V ny = m::la::cross(t, nx);
		prevN = nx;
		const double r = Lerp(r0, r1, double(i) / (ns - 1));
		for (int s = 0; s < sides; ++s)
		{
			const double a = 2.0 * m::kPi * s / sides;
			const V q = path[i] + (nx * std::cos(a) + ny * std::sin(a)) * r;
			mg.vertProperties.insert(mg.vertProperties.end(), { q.x, q.y, q.z });
		}
	}
	const uint64_t ca = ns * (size_t)sides, cb = ca + 1;
	mg.vertProperties.insert(mg.vertProperties.end(), { path.front().x, path.front().y, path.front().z, path.back().x, path.back().y, path.back().z });
	for (size_t i = 0; i + 1 < ns; ++i)
		for (int s = 0; s < sides; ++s)
		{
			const uint64_t a = i * sides + s, b = i * sides + (s + 1) % sides, c = (i + 1) * sides + (s + 1) % sides, d = (i + 1) * sides + s;
			mg.triVerts.insert(mg.triVerts.end(), { a, b, c, a, c, d });
		}
	for (int s = 0; s < sides; ++s)
	{
		mg.triVerts.insert(mg.triVerts.end(), { ca, (uint64_t)((s + 1) % sides), (uint64_t)s });
		mg.triVerts.insert(mg.triVerts.end(), { cb, (uint64_t)((ns - 1) * sides + s), (uint64_t)((ns - 1) * sides + (s + 1) % sides) });
	}
	double vol = 0.0;
	auto P = [&](uint64_t i) { return V(mg.vertProperties[3 * i], mg.vertProperties[3 * i + 1], mg.vertProperties[3 * i + 2]); };
	for (size_t q = 0; q < mg.triVerts.size(); q += 3) vol += m::la::dot(P(mg.triVerts[q]), m::la::cross(P(mg.triVerts[q + 1]), P(mg.triVerts[q + 2])));
	if (vol < 0) for (size_t q = 0; q < mg.triVerts.size(); q += 3) std::swap(mg.triVerts[q + 1], mg.triVerts[q + 2]);
	m::Manifold t(mg);
	return t.Status() == m::Manifold::Error::NoError ? t : m::Manifold();
}

                                                                          
                                                                   
struct Site
{
	V o, n, t, b;
	double thick = 1.0, R = 10.0;
	uint64_t key = 0;
	m::Manifold cut;                                                                              
	bool fromCut = false;
};

struct Ctx
{
	const MeshView* mv = nullptr;
	ProjectDamageBVH bvh;
	std::vector<V> fn;
	double diag = 1.0, eps = 1e-6;
	void Build(const MeshView& v)
	{
		mv = &v;
		bvh.Build(v);
		V lo(1e300), hi(-1e300);
		for (Int32 i = 0; i < v.npts; ++i) { lo = m::la::min(lo, V(v.pts[i])); hi = m::la::max(hi, V(v.pts[i])); }
		diag = m::la::length(hi - lo);
		eps = 1e-6 * diag + 1e-9;
		fn.resize((size_t)v.npolys);
		for (Int32 i = 0; i < v.npolys; ++i)
		{
			const ProjectDamagePoly& q = v.polys[i];
			V s = m::la::cross(V(v.pts[q.b]) - V(v.pts[q.a]), V(v.pts[q.c]) - V(v.pts[q.a]));
			if (q.c != q.d) s += m::la::cross(V(v.pts[q.c]) - V(v.pts[q.a]), V(v.pts[q.d]) - V(v.pts[q.a]));
			fn[(size_t)i] = GuidedNormal(v, i, Unit(s, V(0, 0, 1)));
		}
	}
	                                                             
	double Thickness(const V& q, const V& n, double requestedReach = 0.0) const
	{
		const double rayReach = std::max(0.5 * diag, std::min(diag, requestedReach));
		V o = q - n * eps;
		double done = 0.0;
		for (int g = 0; g < 32; ++g)
		{
			double d = 0.0; int32_t poly = -1;
			if (!bvh.Raycast(o, n * -1.0, rayReach, &d, &poly)) return -1.0;
			done += d;
			if (poly >= 0 && m::la::dot(fn[(size_t)poly], n) < -0.1) return done;
			o = o - n * (d + eps);
			done += eps;
		}
		return -1.0;
	}
};

                                                                  
m::Polygons OpeningAt(const Site& s, const HangHoleParams& p, double z)
{
	if (s.fromCut)
	{
		const m::CrossSection cs = s.cut.Slice(z);
		return cs.ToPolygons();
	}
	const int K = 72;
	const double collar=std::min(.45*s.thick,std::max(0.0,p.edgeWallDepth)*.01*s.R);
	const double f = std::clamp((-z-collar) / std::max(1e-9, s.thick-collar), 0.0, 1.0);                      
	const double flare = 1.0 + p.flare * (1.0 - f) - 0.3 * p.flare * f;
	m::SimplePolygon poly;
	for (int k = 0; k < K; ++k)
	{
		const double a01 = double(k) / K;
		const double jag = 1.0 + p.jagged * (0.35 * RingNoise(a01, 7, s.key, 1) + 0.2 * RingNoise(a01, 23, s.key, 2 + (uint64_t)(f * 3)));
		const double r = s.R * flare * std::max(0.25, jag);
		poly.push_back(V2(r * std::cos(2 * m::kPi * a01), r * std::sin(2 * m::kPi * a01)));
	}
	return { poly };
}

                                                                        
bool Chord(const m::Polygons& polys, const V2& o2, const V2& d2, double& s0, double& s1)
{
	std::vector<double> xs;
	for (const auto& pl : polys)
		for (size_t i = 0; i < pl.size(); ++i)
		{
			const V2 a = pl[i], b = pl[(i + 1) % pl.size()];
			const V2 e = b - a;
			const double den = d2.x * e.y - d2.y * e.x;
			if (std::abs(den) < 1e-12) continue;
			const V2 w = a - o2;
			const double s = (w.x * e.y - w.y * e.x) / den;
			const double u = (w.x * d2.y - w.y * d2.x) / den;
			if (u >= 0.0 && u <= 1.0) xs.push_back(s);
		}
	if (xs.size() < 2) return false;
	std::sort(xs.begin(), xs.end());
	                                                                      
	for (size_t i = 0; i + 1 < xs.size(); ++i)
		if (xs[i] <= 0.0 && xs[i + 1] >= 0.0) { s0 = xs[i]; s1 = xs[i + 1]; return true; }
	s0 = xs[0]; s1 = xs[1];
	return s1 - s0 > 1e-6;
}

std::vector<Site> Sites(const CoreInput& in, const HangHoleParams& p, Int32 mainSeed, const Ctx& cx,
	const std::vector<m::Manifold>* breakSolids, const m::Manifold* base)
{
	std::vector<Site> out;
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x48414E47484F4C45ULL) ^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const MeshView& mv = in.view;
	if (p.mode == 1)
	{
		if (breakSolids == nullptr || base == nullptr) return out;
		for (size_t i = 0; i < breakSolids->size(); ++i)
		{
			const m::Manifold I = (*breakSolids)[i] ^ *base;
			if (I.IsEmpty()) continue;
			const m::Box bb = I.BoundingBox();
			const V c = (bb.min + bb.max) * 0.5;
			                                                         
			double bd = 1e300; Int32 bp = -1;
			for (Int32 k = 0; k < mv.npolys; ++k)
			{
				const ProjectDamagePoly& q = mv.polys[k];
				const V qc = (V(mv.pts[q.a]) + V(mv.pts[q.b]) + V(mv.pts[q.c])) / 3.0;
				const double d = m::la::length(qc - c);
				if (d < bd) { bd = d; bp = k; }
			}
			if (bp < 0) continue;
			Site s;
			s.n = cx.fn[(size_t)bp];
			const ProjectDamagePoly& q = mv.polys[bp];
			const V qa = mv.pts[q.a];
			s.o = c + s.n * m::la::dot(qa - c, s.n);                                                       
			s.t = Unit(m::la::cross(s.n, std::abs(s.n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
			s.b = m::la::cross(s.n, s.t);
			s.thick = cx.Thickness(s.o, s.n);
			if (!(s.thick > 0.0)) s.thick = 0.5 * m::la::length(bb.max - bb.min);
			s.R = 0.5 * m::la::length(bb.max - bb.min);
			s.key = seed ^ (uint64_t(i) * 7919);
			                                                     
			const m::mat3x4 toFrame(V(s.t.x, s.b.x, s.n.x), V(s.t.y, s.b.y, s.n.y), V(s.t.z, s.b.z, s.n.z),
				V(-m::la::dot(s.o, s.t), -m::la::dot(s.o, s.b), -m::la::dot(s.o, s.n)));
			s.cut = I.Transform(toFrame);
			s.fromCut = true;
			out.push_back(std::move(s));
		}
		return out;
	}
	                                                   
	std::vector<double> cdf;
	std::vector<std::array<Int32, 3>> tri;
	std::vector<Int32> triPoly;
	double acc = 0.0;
	for (Int32 i = 0; i < mv.npolys; ++i)
	{
		const ProjectDamagePoly& q = mv.polys[i];
		const Int32 ids[4] = { q.a, q.b, q.c, q.d };
		for (int k = 1; k + 1 < (q.c == q.d ? 3 : 4); ++k)
		{
			const double a = 0.5 * m::la::length(m::la::cross(V(mv.pts[ids[k]]) - V(mv.pts[ids[0]]), V(mv.pts[ids[k + 1]]) - V(mv.pts[ids[0]])));
			const bool avoid = p.avoid && in.avoidPoly != nullptr && (size_t)i < in.avoidPoly->size() && (*in.avoidPoly)[(size_t)i];
			acc += avoid ? 0.0 : a;
			cdf.push_back(acc);
			tri.push_back({ ids[0], ids[k], ids[k + 1] });
			triPoly.push_back(i);
		}
	}
	if (!(acc > 0.0)) return out;
	const double thicknessScale = p.thickness / 100.0;
	for (int h = 0; h < p.amount; ++h)
		for (int attempt = 0; attempt < 30; ++attempt)
		{
			const uint64_t k = 100 + uint64_t(h) * 131 + uint64_t(attempt) * 100003;
			const size_t ti = (size_t)(std::upper_bound(cdf.begin(), cdf.end(), R01(seed, k, 1) * acc) - cdf.begin());
			if (ti >= tri.size()) continue;
			double u = R01(seed, k, 2), v = R01(seed, k, 3);
			if (u + v > 1) { u = 1 - u; v = 1 - v; }
			Site s;
			s.o = V(mv.pts[tri[ti][0]]) * (1 - u - v) + V(mv.pts[tri[ti][1]]) * u + V(mv.pts[tri[ti][2]]) * v;
			s.n = cx.fn[(size_t)triPoly[ti]];
			s.R = 0.5 * Lerp(p.sizeMin, p.sizeMax, R01(seed, k, 4));
			const double maxThickness = 4.0 * s.R * thicknessScale;
			const double requestedReach = thicknessScale > 1.0 ? maxThickness : 0.0;
			s.thick = cx.Thickness(s.o, s.n, requestedReach);
			if (!(s.thick > 0.0) || s.thick > maxThickness) continue;
			bool clash = false;
			for (const Site& o : out) if (m::la::length(o.o - s.o) < o.R + s.R) { clash = true; break; }
			if (clash) continue;
			const double ang = 2 * m::kPi * R01(seed, k, 5);
			const V t0 = Unit(m::la::cross(s.n, std::abs(s.n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0)), V(1, 0, 0));
			const V b0 = m::la::cross(s.n, t0);
			s.t = t0 * std::cos(ang) + b0 * std::sin(ang);
			s.b = m::la::cross(s.n, s.t);
			s.key = seed ^ (k * 2654435761ULL);
			out.push_back(std::move(s));
			break;
		}
	return out;
}

}               

void PlaceHangHoleCutters(const CoreInput& in, const HangHoleParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, const ProjectDamageHost& host)
{
	if (!p.enable || p.mode != 0 || in.view.npolys <= 0) return;
	Ctx cx; cx.Build(in.view);
	const std::vector<Site> sites = Sites(in, p, mainSeed, cx, nullptr, nullptr);
	std::shared_ptr<ProjectDamageBVH> objBvh;
	std::function<bool(const EVec&)> inside;
	for (const Site& s : sites)
	{
		                                                        
		const int layers = p.edgeWallDepth>0.0 ? 10 : 8;
		const double z0 = 0.25 * s.thick + 0.05 * s.R, z1 = -1.2 * s.thick - 0.05 * s.R;
		m::MeshGL64 mg;
		mg.numProp = 3;
		size_t K = 0;
		for (int l = 0; l <= layers; ++l)
		{
			const double collar=std::min(.45*s.thick,std::max(0.0,p.edgeWallDepth)*.01*s.R);
            const double z = p.edgeWallDepth>0.0 ? (l==0?z0:l==1?0.0:l==2?-collar:Lerp(-collar,z1,double(l-2)/(layers-2))) : Lerp(z0, z1, double(l) / layers);
			const m::Polygons ring = OpeningAt(s, p, std::min(0.0, std::max(z, -s.thick)));
			const auto& pl = ring[0];
			K = pl.size();
			for (const V2& q : pl)
			{
				V w = s.o + s.t * q.x + s.b * q.y + s.n * z;
				                                                      
				const double nz = p.roughness * WallNoise(w, s.R, s.key, p) * std::clamp(p.noiseStrength, 0.0, 4.0);
				const V radial = Unit(s.t * q.x + s.b * q.y, s.t);
				w = w + radial * nz;
				mg.vertProperties.insert(mg.vertProperties.end(), { w.x, w.y, w.z });
			}
		}
		const uint64_t ca = (uint64_t)(K * (layers + 1)), cb = ca + 1;
		const V top = s.o + s.n * z0, bot = s.o + s.n * z1;
		mg.vertProperties.insert(mg.vertProperties.end(), { top.x, top.y, top.z, bot.x, bot.y, bot.z });
		for (int l = 0; l < layers; ++l)
			for (size_t k = 0; k < K; ++k)
			{
				const uint64_t a = l * K + k, b = l * K + (k + 1) % K, c = (l + 1) * K + (k + 1) % K, d = (l + 1) * K + k;
				mg.triVerts.insert(mg.triVerts.end(), { a, d, c, a, c, b });
			}
		for (size_t k = 0; k < K; ++k)
		{
			mg.triVerts.insert(mg.triVerts.end(), { ca, (uint64_t)k, (uint64_t)((k + 1) % K) });
			mg.triVerts.insert(mg.triVerts.end(), { cb, (uint64_t)(layers * K + (k + 1) % K), (uint64_t)(layers * K + k) });
		}
		double vol = 0.0;
		auto P = [&](uint64_t i) { return V(mg.vertProperties[3 * i], mg.vertProperties[3 * i + 1], mg.vertProperties[3 * i + 2]); };
		for (size_t q = 0; q < mg.triVerts.size(); q += 3) vol += m::la::dot(P(mg.triVerts[q]), m::la::cross(P(mg.triVerts[q + 1]), P(mg.triVerts[q + 2])));
		if (vol < 0) for (size_t q = 0; q < mg.triVerts.size(); q += 3) std::swap(mg.triVerts[q + 1], mg.triVerts[q + 2]);
		m::Manifold cut(mg);
		if (cut.Status() != m::Manifold::Error::NoError || cut.IsEmpty()) continue;
		if (p.surfMode == 1 && p.texStrength > 0.0)
		{
			if (!inside) inside = MakeInsideTest(in.view, objBvh);
			TexSpec t;
			t.path = p.texPath; t.scale = p.texScale; t.depth = p.texStrength;
			t.offX = p.texOffX; t.offY = p.texOffY; t.offZ = p.texOffZ; t.edge = p.texEdge;
			std::string msg;
			cut = TextureDisplaceSolid(cut, t, &msg, &inside);
			if (!msg.empty()) HostLog(host, msg);
		}
		cutters.push_back(std::move(cut));
	}
	HostLog(host, "[ProjectDamage] hanging-rebar holes: " + std::to_string(cutters.size()) + " holes");
}

// основной вход: собираем геометрию по переданным настройкам
void BuildHangHoleTubes(const CoreInput& in, const HangHoleParams& p, Int32 mainSeed, const m::Manifold& base,
	const std::vector<m::Manifold>& breakSolids, std::vector<m::Manifold>& tubes, std::vector<Int32>* tubeKinds)
{
	if (!p.enable || in.view.npolys <= 0) return;
	Ctx cx; cx.Build(in.view);
	const std::vector<Site> sites = Sites(in, p, mainSeed, cx, &breakSolids, &base);
	const V grav = Unit(V(p.gravityX, p.gravityY, p.gravityZ), V(0, 0, -1));
	const int sides = std::clamp(p.sides, 4, 16);
	for (const Site& s : sites)
	{
		auto to3 = [&](const V2& q, double z) { return s.o + s.t * q.x + s.b * q.y + s.n * z; };
		               
		if (p.barsEnable && p.barSpacing > 0.0)
		{
			const double zb = -std::clamp(p.barDepth, 0.05, 0.95) * s.thick;
			const m::Polygons open = OpeningAt(s, p, zb);
			const double ang0 = 2 * m::kPi * R01(s.key, 1, 1);
			for (int fam = 0; fam < 2; ++fam)
			{
				const double ang = ang0 + fam * 0.5 * m::kPi;
				const V2 d2(std::cos(ang), std::sin(ang)), side2(-d2.y, d2.x);
				const double reach = 2.5 * s.R;
				const int lines = int(2 * reach / p.barSpacing) + 1;
				for (int li = 0; li < lines; ++li)
				{
					const double off = -reach + li * p.barSpacing + (R01(s.key, 10 + fam, li) - 0.5) * 0.2 * p.barSpacing;
					const V2 o2 = side2 * off;
					double s0, s1;
					if (!Chord(open, o2, d2, s0, s1)) continue;
					const uint64_t bk = s.key ^ (uint64_t(fam) << 40) ^ uint64_t(li + 1000);
					const double embed = 2.0 * p.barRadius + 0.05 * s.R;
					const V A = to3(o2 + d2 * (s0 - embed), zb), Ain = to3(o2 + d2 * s0, zb);
					const V B = to3(o2 + d2 * (s1 + embed), zb), Bin = to3(o2 + d2 * s1, zb);
					const V lineDir = Unit(B - A, s.t);
					if (R01(bk, 1, 1) >= p.barBroken)
					{
						                                   
						std::vector<V> path;
						for (int k = 0; k <= 12; ++k)
						{
							const double f = double(k) / 12, bell = 4 * f * (1 - f);
							path.push_back(A + (B - A) * f + grav * (p.barBow * (s1 - s0) * 0.12 * bell));
						}
						if(p.rebarResolution<1.0) {const auto ids=RebarLodIndices(path,p.barRadius,p.rebarResolution);RebarLodSelect(path,ids);}
                        m::Manifold tb = Tube(path, p.barRadius, p.barRadius, RebarLodSides(sides,p.rebarResolution));
						if (!tb.IsEmpty()) { tubes.push_back(std::move(tb)); if (tubeKinds) tubeKinds->push_back(0); }
						continue;
					}
					                                                  
					const double snap = Lerp(0.2, 0.8, R01(bk, 2, 1));
					for (int end = 0; end < 2; ++end)
					{
						const V root = end == 0 ? A : B, rim = end == 0 ? Ain : Bin;
						V dir = end == 0 ? lineDir : lineDir * -1.0;
						const double free = (s1 - s0) * (end == 0 ? snap : 1.0 - snap) * Lerp(0.8, 1.3, R01(bk, 3 + end, 1));
						std::vector<V> path = { root, rim };
						V cur = rim;
						const int ns = 10;
						for (int k = 1; k <= ns; ++k)
						{
							const double f = double(k) / ns;
							const double w = std::min(1.0, p.droop * std::pow(f, 1.2) * 1.4);
							dir = Unit(dir * (1.0 - w) + grav * w + s.n * ((R01(bk, 10 + end, k) - 0.5) * 0.3), dir);
							cur = cur + dir * (free / ns);
							path.push_back(cur);
						}
						if(p.rebarResolution<1.0) {const auto ids=RebarLodIndices(path,p.barRadius,p.rebarResolution);RebarLodSelect(path,ids);}
                        m::Manifold tb = Tube(path, p.barRadius, p.barRadius * 0.85, RebarLodSides(sides,p.rebarResolution));
						if (!tb.IsEmpty()) { tubes.push_back(std::move(tb)); if (tubeKinds) tubeKinds->push_back(0); }
					}
				}
			}
		}
		                 
		if (p.cablesEnable && p.cableAmount > 0)
			for (int c = 0; c < p.cableAmount; ++c)
			{
				const uint64_t ck = s.key ^ (0xCAB1E000ULL + uint64_t(c));
				const double zc = -Lerp(0.15, 0.85, R01(ck, 1, 1)) * s.thick;
				const m::Polygons open = OpeningAt(s, p, zc);
				if (open.empty() || open[0].size() < 3) continue;
				const auto& pl = open[0];
				const V2 qa = pl[(size_t)(R01(ck, 2, 1) * pl.size()) % pl.size()];
				const V rimA = to3(qa, zc), inA = to3(qa * 1.15, zc);                                  
				const double len = p.cableLength * s.R * Lerp(0.6, 1.5, R01(ck, 3, 1));
				std::vector<V> path;
				if (R01(ck, 4, 1) < p.cableLoops)
				{
					                                      
					const V2 qb = pl[(size_t)(R01(ck, 5, 1) * pl.size()) % pl.size()];
					const V rimB = to3(qb, zc), inB = to3(qb * 1.15, zc);
					const double span = m::la::length(rimB - rimA);
					if (span < 0.2 * s.R) continue;
					path.push_back(inA);
					for (int k = 0; k <= 16; ++k)
					{
						const double f = double(k) / 16, bell = 4 * f * (1 - f);
						path.push_back(rimA + (rimB - rimA) * f + grav * (p.cableSag * (0.4 * span + 0.3 * len) * bell));
					}
					path.push_back(inB);
				}
				else
				{
					                                                         
					path.push_back(inA);
					path.push_back(rimA);
					V dir = Unit((s.o + s.n * zc) - rimA, s.t);
					dir = Unit(dir + s.n * ((R01(ck, 6, 1) - 0.3) * 0.8), dir);
					V cur = rimA;
					const int ns = 16;
					for (int k = 1; k <= ns; ++k)
					{
						const double f = double(k) / ns;
						const double w = std::min(1.0, (0.35 + p.cableSag) * f * 2.2);
						dir = Unit(dir * (1.0 - w) + grav * w + s.t * ((R01(ck, 20, k) - 0.5) * 0.25), dir);
						cur = cur + dir * (len / ns);
						path.push_back(cur);
					}
				}
				m::Manifold tb = Tube(path, p.cableRadius, p.cableRadius, sides);
				if (!tb.IsEmpty()) { tubes.push_back(std::move(tb)); if (tubeKinds) tubeKinds->push_back(1); }
			}
	}
}

PROJECT_DAMAGE_NS_MACRO_END
