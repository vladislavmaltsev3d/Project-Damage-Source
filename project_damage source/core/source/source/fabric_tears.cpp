// общие расчёты разрывов ткани и её нитей
// ProjectDamage fabric tears — GPL-3.0-or-later; see LICENSE.
//
// Recipe: fabric-tears-v2. A procedural appearance model of torn cloth, not a
// cloth simulation.
//   1. layer analysis: every triangle casts a ray inward; the area-weighted
//      median ray length is the thickness T. A THIN SHEET (cloth with
//      Solidify, T <= 6% of the diagonal) is cut through; any thicker solid
//      (a cube, a cushion) is treated as a fabric SKIN of skinDepth over the
//      object - tears cut a pocket that deep and threads span the pocket. The
//      decision is also made per sample, so a thin part of a thick model is
//      still cut through. On thin sheets, triangles whose inward ray is much
//      longer than T (or misses) are the sheet's open BORDER;
//   2. interior tears: a path walked ON the surface from a random seed, along
//      a weave axis (or a random direction) with gentle meander, split into
//      1..N holes separated by fabric bridges. Each hole is a lens-shaped
//      strip with independent ragged rims, turned into a closed slab that
//      follows the surface;
//   3. edge tears: slits that start outside the sheet border and run inward,
//      narrowing to a tip, spread evenly along the border with jitter;
//   4. drawn (Grease Pencil) holes and torn edges: each closed contour is
//      resampled, given a ragged rim, extruded, refined and draped onto the
//      surface under it. A drawn torn edge also sprouts slits that run from
//      its rim out into the fabric, plus long hanging threads;
//   5. threads (each kind toggleable): bridges spanning a gap with sag and
//      wobble, threads broken mid-gap with both ends dangling under gravity,
//      a short fringe along every rim, and long strands hanging out of
//      borders. Threads are swept tubes anchored INSIDE the remaining fabric
//      at mid-layer, so the union welds them on.
// Candidates use SplitMix64 counter channels like surface deposits and
// coating peeling, keyed by seed (main seed and Fabric Seed), candidate id and
// channel, so changing one Amount does not reshuffle the other tears.
//
// Channel map (per candidate): 0 triangle / border slot, 1-2 barycentric,
// 3 direction, 4 direction jitter, 5 length, 6 width, 7 split count,
// 8-10 noise offset, 11 border jitter, 20+ split pieces; threads use their
// own per-tear stream (thread counter x channel).
#include "manifold/manifold.h"
#include "fabric_tears.h"
#include "fabric_noise.h"
#include "fabric_tear_shapes.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;
using V2 = m::vec2;

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

                                             
V Rotate(const V& v, const V& axis, double ang)
{
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + m::la::cross(axis, v) * s + axis * (m::la::dot(axis, v) * (1.0 - c));
}

V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

                                                                 
V TurnToward(const V& from, const V& to, double maxAng)
{
	const double c = ClampD(m::la::dot(from, to), -1.0, 1.0);
	const double ang = std::acos(c);
	if (ang <= maxAng) return to;
	const V axis = Unit(m::la::cross(from, to), AnyPerp(from));
	return Unit(Rotate(from, axis, maxAng), from);
}

                                                                                   

struct Tri { int32_t a, b, c, poly; };

                                                                              
                                                                       
struct SpanHit { double top = 0, cutTop = 0, cutBot = 0, anchor = 0, th = -1.0; V nrm; };                                        

struct Sheet {
	const MeshView* v = nullptr;
	std::vector<Tri> tris;
	std::vector<V> tn, tc;
	std::vector<double> area;
	std::vector<V> polyN;
	std::vector<char> border;
	ProjectDamageBVH bvh;
	double T = 0.0, eps = 1e-6, diag = 1.0;
	bool thin = true;                                                             
	double layer = 0.0;                                                   
	double throughMax = 0.0;                                                      
	double pocket = 0.0;                                       
	double margin = 0.0;                            
	double cell = 1.0;
	std::unordered_map<uint64_t, std::vector<int32_t>> grid;                               

	uint64_t Key(int64_t x, int64_t y, int64_t z) const
	{
		return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
	}
	void Cell(const V& p, int64_t& x, int64_t& y, int64_t& z) const
	{
		x = int64_t(std::floor(p.x / cell)); y = int64_t(std::floor(p.y / cell)); z = int64_t(std::floor(p.z / cell));
	}

	                                                                        
	                                                                     
	                                                                        
	                                                                     
	bool NearestSheetNormal(const V& p, V& n) const
	{
		int64_t cx, cy, cz;
		Cell(p, cx, cy, cz);
		double best = 1e300;
		int32_t bi = -1;
		for (int ring = 0; ring <= 3 && bi < 0; ++ring)
			for (int64_t dx = -ring; dx <= ring; ++dx)
				for (int64_t dy = -ring; dy <= ring; ++dy)
					for (int64_t dz = -ring; dz <= ring; ++dz) {
						if (std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz))) != ring) continue;
						auto it = grid.find(Key(cx + dx, cy + dy, cz + dz));
						if (it == grid.end()) continue;
						for (int32_t t : it->second) {
							const double d = m::la::length2(tc[size_t(t)] - p);
							if (d < best) { best = d; bi = t; }
						}
					}
		if (bi < 0) {
			for (size_t i = 0; i < tris.size(); ++i) {
				if (border[i]) continue;
				const Tri& t = tris[i];
				const V a = v->pts[t.a], b = v->pts[t.b], c = v->pts[t.c];
				const V normal = tn[i];
				const V q = p - normal * m::la::dot(p - a, normal);
				double d = 1e300;
				if (m::la::dot(m::la::cross(b-a, q-a), normal) >= -eps &&
					m::la::dot(m::la::cross(c-b, q-b), normal) >= -eps &&
					m::la::dot(m::la::cross(a-c, q-c), normal) >= -eps) {
					d = m::la::length2(p-q);
				} else {
					const V starts[3] = {a,b,c}, ends[3] = {b,c,a};
					for (int e = 0; e < 3; ++e) {
						const V edge = ends[e] - starts[e];
						const double f = ClampD(m::la::dot(p-starts[e], edge) / std::max(1e-30, m::la::length2(edge)), 0.0, 1.0);
						d = std::min(d, m::la::length2(p - starts[e] - edge*f));
					}
				}
				if (d < best) { best = d; bi = int32_t(i); }
			}
		}
		if (bi < 0) return false;
		n = GuidedNormal(*v,tris[size_t(bi)].poly,tn[size_t(bi)]);
		return true;
	}

	                                                                      
	                                                                        
	                                                         
	bool Span(const V& P, const V& N, double R, SpanHit& o) const
	{
		double d1 = 0.0;
		int32_t poly = -1;
		if (!bvh.Raycast(P + N * R, -N, 2.0 * R, &d1, &poly) || poly < 0) return false;
		V hn = polyN[size_t(poly)];
		if (m::la::dot(hn, N) < 0.25) return false;                                
		o.top = R - d1;
		o.nrm = hn;
		const double back = 10.0 * eps;
		double d2 = 0.0;
		double th = 1e300;
		if (bvh.Raycast(P + N * (o.top - back), -N, 1.5 * throughMax + margin, &d2)) th = d2 + back;
		o.cutTop = o.top + margin;
		if (th <= throughMax) {
			o.cutBot = o.top - th - margin;
			o.anchor = o.top - 0.5 * th;
			o.th = th;
		} else {
			o.cutBot = o.top - pocket;
			o.anchor = o.top - 0.5 * pocket;
		}
		return true;
	}

	                                    
	bool Project(const V& P, const V& N, double R, V& Q, V& Nq) const
	{
		double d = 0.0;
		int32_t poly = -1;
		if (!bvh.Raycast(P + N * R, -N, 2.0 * R, &d, &poly) || poly < 0) return false;
		Q = P + N * (R - d);
		Nq = polyN[size_t(poly)];
		double c = m::la::dot(Nq, N);
		if (c < 0.0) { Nq = -Nq; c = -c; }
		if (c < 0.3) return false;                                            
		return true;
	}
};

bool BuildSheet(const CoreInput& in, const FabricTearParams& p, Sheet& s)
{
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return false;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0.0));
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		s.tris.push_back({q.a, q.b, q.c, i});
		if (q.c != q.d) s.tris.push_back({q.a, q.c, q.d, i});
	}
	s.tn.resize(s.tris.size());
	s.tc.resize(s.tris.size());
	s.area.resize(s.tris.size());
	for (size_t i = 0; i < s.tris.size(); ++i) {
		const Tri& t = s.tris[i];
		const V cr = m::la::cross(v.pts[t.b] - v.pts[t.a], v.pts[t.c] - v.pts[t.a]);
		s.area[i] = 0.5 * m::la::length(cr);
		s.tn[i] = Unit(cr, V(0, 0, 1));
		s.tc[i] = (v.pts[t.a] + v.pts[t.b] + v.pts[t.c]) / 3.0;
		s.polyN[size_t(t.poly)] = s.polyN[size_t(t.poly)] + cr;
	}
	for(size_t i=0;i<s.polyN.size();++i)s.polyN[i]=GuidedNormal(v,int32_t(i),Unit(s.polyN[i],V(0,0,1)));
	s.bvh.Build(v);

	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.eps = std::max(1e-7, 1e-7 * s.diag);

	std::vector<double> dist(s.tris.size(), -1.0);
	ProjectDamageParallelFor(0, int64_t(s.tris.size()), [&](int64_t i) {
		double d = 0.0;
		const V org = s.tc[size_t(i)] - s.tn[size_t(i)] * (10.0 * s.eps);
		if (s.bvh.Raycast(org, -s.tn[size_t(i)], 2.0 * s.diag, &d)) dist[size_t(i)] = d + 10.0 * s.eps;
	});
	{
		std::vector<std::pair<double, double>> da;
		double tot = 0.0;
		for (size_t i = 0; i < s.tris.size(); ++i)
			if (dist[i] > 20.0 * s.eps && s.area[i] > 0.0) { da.push_back({dist[i], s.area[i]}); tot += s.area[i]; }
		std::sort(da.begin(), da.end());
		double acc = 0.0;
		for (const auto& e : da) { acc += e.second; if (acc >= 0.5 * tot) { s.T = e.first; break; } }
	}
	                                                                      
	                                                         
	                                                                       
	                                                                       
	                                                              
	s.thin = s.T > 0.0 && s.T <= 0.06 * s.diag;
	const double skin = p.skinDepth > 0.0 ? p.skinDepth : 0.01 * s.diag;
	s.layer = s.thin ? s.T : skin;
	if (p.thickness > 0.0) {
		s.throughMax = p.thickness;
		s.pocket = p.thickness;
	} else {
		s.throughMax = 4.0 * s.diag;                    
		s.pocket = s.layer;
	}
	if (!(s.layer > 0.0)) return false;
	s.margin = std::max(ClampD(p.cutMargin, 0.05, 4.0) * s.layer, 4.0 * s.eps);

	s.border.assign(s.tris.size(), 0);
	if (s.thin)
		for (size_t i = 0; i < s.tris.size(); ++i)
			s.border[i] = (dist[i] < 0.0 || dist[i] > 3.0 * s.T) ? 1 : 0;

	s.cell = std::max(4.0 * s.layer, s.diag / 256.0);
	for (size_t i = 0; i < s.tris.size(); ++i) {
		if (s.border[i]) continue;
		int64_t x, y, z;
		s.Cell(s.tc[i], x, y, z);
		s.grid[s.Key(x, y, z)].push_back(int32_t(i));
	}
	return true;
}

                                                                                   

struct PathPt { V pos, n, dir; };

                                                                             
                                                                            
                                                                     
std::vector<PathPt> Walk(const Sheet& s, const V& p0, const V& n0, const V& d0, double step, int count,
	double meander, double feature, const V& noiseOff)
{
	std::vector<PathPt> out;
	V n = n0;
	V dir = Unit(d0 - n * m::la::dot(d0, n), AnyPerp(n));
	V pos = p0;
	out.push_back({pos, n, dir});
	const double R = std::max(3.0 * s.layer, 2.0 * step);
	for (int i = 1; i <= count; ++i) {
		const double sa = i * step;
		const double turn = meander * 1.6 * Fbm(V(sa / feature, 0.37, 0.0) + noiseOff, 2) * (step / feature);
		dir = Rotate(dir, n, turn);
		const V cand = pos + dir * step;
		V Q, Nq;
		if (!s.Project(cand, n, R, Q, Nq)) break;
		if (m::la::length(Q - cand) > 1.5 * step + s.layer) break;                               
		const V nd = Unit(Q - pos, dir);
		dir = Unit(nd - Nq * m::la::dot(nd, Nq), dir);
		n = Nq;
		pos = Q;
		out.push_back({pos, n, dir});
	}
	return out;
}

                                                                                   

                                                                              
                                                                           
                                             
m::Manifold StripSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	for (int i = 0; i <= ns; ++i)
		for (int j = 0; j <= nw; ++j) {
			push(bot[size_t(i * (nw + 1) + j)]);
			push(top[size_t(i * (nw + 1) + j)]);
		}
	auto B = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j)); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j) + 1); };
	auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
	};
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			quad(T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			quad(B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
		}
	for (int i = 0; i < ns; ++i) {
		quad(B(i, 0), B(i + 1, 0), T(i + 1, 0), T(i, 0));
		quad(B(i, nw), T(i, nw), T(i + 1, nw), B(i + 1, nw));
	}
	for (int j = 0; j < nw; ++j) {
		quad(B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
		quad(B(ns, j), B(ns, j + 1), T(ns, j + 1), T(ns, j));
	}
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	return solid;
}

                                                                              
                                                                              
                                     
m::Manifold Tube(const std::vector<V>& raw, double r0, double r1, int sides)
{
	std::vector<V> pts;
	const double minSeg = 0.25 * std::max(r0, r1);
	for (const V& q : raw)
		if (pts.empty() || m::la::length(q - pts.back()) > minSeg) pts.push_back(q);
	if (pts.size() < 2) return m::Manifold();
	const size_t n = pts.size();
	std::vector<V> tan(n);
	for (size_t i = 0; i < n; ++i) {
		if (i == 0) tan[i] = Unit(pts[1] - pts[0], V(0, 0, 1));
		else if (i + 1 == n) tan[i] = Unit(pts[i] - pts[i - 1], V(0, 0, 1));
		else tan[i] = Unit(Unit(pts[i] - pts[i - 1], V(0, 0, 1)) + Unit(pts[i + 1] - pts[i], V(0, 0, 1)), tan[i - 1]);
	}
	m::MeshGL mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) {
		mesh.vertProperties.push_back(float(q.x));
		mesh.vertProperties.push_back(float(q.y));
		mesh.vertProperties.push_back(float(q.z));
	};
	V N = AnyPerp(tan[0]);
	for (size_t i = 0; i < n; ++i) {
		if (i > 0) N = Unit(N - tan[i] * m::la::dot(N, tan[i]), AnyPerp(tan[i]));
		const V Bv = m::la::cross(tan[i], N);
		double rr = Lerp(r0, r1, double(i) / double(n - 1));
		if (i > 0 && i + 1 < n) {
			const double ch = m::la::dot(Unit(pts[i] - pts[i - 1], tan[i]), tan[i]);
			rr /= std::max(0.6, ch);                                                 
		}
		for (int k = 0; k < sides; ++k) {
			const double a = 2.0 * m::kPi * k / sides;
			push(pts[i] + (N * std::cos(a) + Bv * std::sin(a)) * rr);
		}
	}
	const uint32_t c0 = uint32_t(n * sides), c1 = c0 + 1;
	push(pts[0]);
	push(pts[n - 1]);
	auto R = [&](size_t i, int k) { return uint32_t(i * sides + size_t((k % sides + sides) % sides)); };
	for (size_t i = 0; i + 1 < n; ++i)
		for (int k = 0; k < sides; ++k) {
			const uint32_t a = R(i, k), b = R(i + 1, k), c = R(i + 1, k + 1), d = R(i, k + 1);
			mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
		}
	for (int k = 0; k < sides; ++k) {
		mesh.triVerts.insert(mesh.triVerts.end(), {c0, R(0, k), R(0, k + 1)});
		mesh.triVerts.insert(mesh.triVerts.end(), {c1, R(n - 1, k + 1), R(n - 1, k)});
	}
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	return solid;
}

                                                                                      

struct Row { V c, side, n; double wl, wr; bool real; };

                                                                       
                                                  
struct Strip {
	m::Manifold solid;
	std::vector<V> left, right, side, n;
	std::vector<double> arc;                                       
	std::vector<char> ok;                                                                
};

bool BuildStrip(const Sheet& s, const std::vector<Row>& rows, double probe, Strip& out)
{
	const int ns = int(rows.size()) - 1;
	const int nw = 4;
	if (ns < 2) return false;
	const size_t cnt = size_t((ns + 1) * (nw + 1));
	std::vector<V> P(cnt);
	std::vector<SpanHit> h(cnt);
	std::vector<char> val(cnt, 0);
	std::vector<char> rowAny(size_t(ns + 1), 0);
	for (int i = 0; i <= ns; ++i) {
		const Row& r = rows[size_t(i)];
		for (int j = 0; j <= nw; ++j) {
			const double a = -1.0 + 2.0 * j / nw;
			const double off = a < 0.0 ? a * r.wl : a * r.wr;
			const size_t k = size_t(i * (nw + 1) + j);
			P[k] = r.c + r.side * off;
			if (s.Span(P[k], r.n, probe, h[k])) { val[k] = 1; rowAny[size_t(i)] = 1; }
		}
	}
	                                                                          
	                                                              
	for (int i = 0; i <= ns; ++i) {
		if (!rowAny[size_t(i)]) continue;
		for (int j = 0; j <= nw; ++j) {
			const size_t k = size_t(i * (nw + 1) + j);
			if (val[k]) continue;
			for (int d = 1; d <= nw; ++d) {
				if (j - d >= 0 && val[size_t(i * (nw + 1) + j - d)]) { h[k] = h[size_t(i * (nw + 1) + j - d)]; break; }
				if (j + d <= nw && val[size_t(i * (nw + 1) + j + d)]) { h[k] = h[size_t(i * (nw + 1) + j + d)]; break; }
			}
		}
	}
	bool any = false;
	for (char c : rowAny) any = any || c;
	if (!any) return false;
	for (int i = 0; i <= ns; ++i) {
		if (rowAny[size_t(i)]) continue;
		int src = -1;
		for (int d = 1; d <= ns && src < 0; ++d) {
			if (i - d >= 0 && rowAny[size_t(i - d)]) src = i - d;
			else if (i + d <= ns && rowAny[size_t(i + d)]) src = i + d;
		}
		for (int j = 0; j <= nw; ++j) h[size_t(i * (nw + 1) + j)] = h[size_t(src * (nw + 1) + j)];
	}
	std::vector<V> top(cnt), bot(cnt);
	for (int i = 0; i <= ns; ++i)
		for (int j = 0; j <= nw; ++j) {
			const size_t k = size_t(i * (nw + 1) + j);
			top[k] = P[k] + rows[size_t(i)].n * h[k].cutTop;
			bot[k] = P[k] + rows[size_t(i)].n * h[k].cutBot;
		}
	out.solid = StripSolid(top, bot, ns, nw);
	if (out.solid.Status() != m::Manifold::Error::NoError || out.solid.NumTri() == 0) return false;
	out.left.resize(size_t(ns + 1)); out.right.resize(size_t(ns + 1));
	out.side.resize(size_t(ns + 1)); out.n.resize(size_t(ns + 1));
	out.arc.resize(size_t(ns + 1)); out.ok.resize(size_t(ns + 1));
	double acc = 0.0;
	for (int i = 0; i <= ns; ++i) {
		const Row& r = rows[size_t(i)];
		const size_t k0 = size_t(i * (nw + 1)), k1 = k0 + size_t(nw);
		out.left[size_t(i)] = P[k0] + r.n * h[k0].anchor;
		out.right[size_t(i)] = P[k1] + r.n * h[k1].anchor;
		out.side[size_t(i)] = r.side;
		out.n[size_t(i)] = r.n;
		if (i > 0) acc += m::la::length(r.c - rows[size_t(i - 1)].c);
		out.arc[size_t(i)] = acc;
		out.ok[size_t(i)] = (r.real && rowAny[size_t(i)] && val[k0] && val[k1]) ? 1 : 0;
	}
	return true;
}

                                                                                 
double Ragged(double sa, double feature, double lobes, double jag, const V& off)
{
	const double low = Fbm(V(sa / (2.5 * feature), 0.3, 0.0) + off, 3);
	const double mid = std::abs(Fbm(V(sa / (0.7 * feature), 1.7, 0.0) + off * 1.7, 2));
	const double fine = std::abs(Fbm(V(sa / (0.22 * feature), 2.9, 0.0) + off * 2.3, 2));
	return std::max(0.08, 1.0 + lobes * low - jag * (0.75 * mid + 0.5 * fine));
}

                                                                                         

double Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }

                                                               
double Signed2D(const std::vector<V2>& poly, const V2& q)
{
	bool inside = false;
	double nearest = 1e300;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		const V2 a = poly[j], b = poly[i];
		if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
		const V2 ab = b - a, aq = q - a;
		const double den = m::la::dot(ab, ab);
		const double t = den > 0.0 ? ClampD(m::la::dot(aq, ab) / den, 0.0, 1.0) : 0.0;
		nearest = std::min(nearest, m::la::length(aq - ab * t));
	}
	return inside ? nearest : -nearest;
}

                                                                             
                                                                        
struct Region {
	bool edge = false;
	uint64_t id = 0;
	V o, n, t, b;
	std::vector<V2> poly, outN;
	std::vector<double> arc;
	double perim = 0.0, inradius = 0.0, reach = 5.0;
	V noise;
	m::Manifold cutter;

	V World(const V2& q) const { return o + t * q.x + b * q.y; }
	V Dir3(const V2& d) const { return Unit(t * d.x + b * d.y, t); }
	                                                                     
	void At(double a, V2& q, V2& nrm) const
	{
		a = std::fmod(a, perim);
		if (a < 0.0) a += perim;
		size_t i = size_t(std::upper_bound(arc.begin(), arc.end(), a) - arc.begin());
		i = i == 0 ? 0 : i - 1;
		const size_t j = (i + 1) % poly.size();
		const double seg = (i + 1 < arc.size() ? arc[i + 1] : perim) - arc[i];
		const double f = seg > 0.0 ? (a - arc[i]) / seg : 0.0;
		q = poly[i] + (poly[j] - poly[i]) * f;
		nrm = m::la::normalize(outN[i] + outN[j]);
	}
	                                                                          
	double RayExit(const V2& q, const V2& d, double sMin) const
	{
		double best = 0.0;
		for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
			const V2 a = poly[j], e = poly[i] - poly[j];
			const double den = Cross2(d, e);
			if (std::abs(den) < 1e-12) continue;
			const V2 aq = a - q;
			const double s = Cross2(aq, e) / den, u = Cross2(aq, d) / den;
			if (s > sMin && u >= 0.0 && u <= 1.0 && (best == 0.0 || s < best)) best = s;
		}
		return best;
	}
};

                                                                              
                           
// основной вход: собираем геометрию по переданным настройкам
bool BuildRegion(const SurfacePaintRegion& src, const FabricTearParams& p, double reach, double amp, Region& r)
{
	std::vector<V> pts;
	for (const EVec& q : src.points)
		if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
			&& (pts.empty() || m::la::length(q - pts.back()) > 1e-9))
			pts.push_back(q);
	if (pts.size() > 3 && m::la::length(pts.front() - pts.back()) <= 1e-9) pts.pop_back();
	if (pts.size() < 3) return false;
	r.n = Unit(V(src.normal), V(0, 0, 1));
	r.o = V(0.0);
	for (const V& q : pts) r.o = r.o + q;
	r.o = r.o / double(pts.size());
	const V axis = std::abs(r.n.z) < 0.9 ? V(0, 0, 1) : V(0, 1, 0);
	r.t = Unit(m::la::cross(axis, r.n), AnyPerp(r.n));
	r.b = m::la::cross(r.n, r.t);
	r.reach = reach;
	std::vector<V2> raw;
	for (const V& q : pts) raw.push_back(V2(m::la::dot(q - r.o, r.t), m::la::dot(q - r.o, r.b)));
	double area2 = 0.0;
	for (size_t i = 0, j = raw.size() - 1; i < raw.size(); j = i++) area2 += Cross2(raw[j], raw[i]);
	if (std::abs(area2) < 1e-9) return false;
	if (area2 < 0.0) std::reverse(raw.begin(), raw.end());
	                                                                            
	V2 lo(1e300), hi(-1e300);
	for (const V2& q : raw) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
	for (int i = 0; i < 32; ++i)
		for (int j = 0; j < 32; ++j)
			r.inradius = std::max(r.inradius, Signed2D(raw, V2(lo.x + (hi.x - lo.x) * (i + 0.5) / 32, lo.y + (hi.y - lo.y) * (j + 0.5) / 32)));
	if (r.inradius <= 0.0) return false;
	double perim = 0.0;
	for (size_t i = 0; i < raw.size(); ++i) perim += m::la::length(raw[(i + 1) % raw.size()] - raw[i]);
	const double rag = std::max(1e-4, p.paintRagSize);
	const double h = std::max(0.18 * rag / ClampD(p.detail, 0.05, 4.0), perim / 3000.0);
	const int nOut = std::max(12, int(std::ceil(perim / h)));
	                                   
	std::vector<V2> res;
	{
		double acc = 0.0;
		size_t i = 0;
		double segStart = 0.0;
		for (int k = 0; k < nOut; ++k) {
			const double a = perim * k / nOut;
			while (i < raw.size()) {
				const double len = m::la::length(raw[(i + 1) % raw.size()] - raw[i]);
				if (segStart + len >= a || i + 1 == raw.size()) {
					const double f = len > 0.0 ? ClampD((a - segStart) / len, 0.0, 1.0) : 0.0;
					res.push_back(raw[i] + (raw[(i + 1) % raw.size()] - raw[i]) * f);
					break;
				}
				segStart += len;
				++i;
			}
			(void)acc;
		}
	}
	const size_t n = res.size();
	std::vector<V2> nrm(n);
	for (size_t i = 0; i < n; ++i) {
		const V2 tg = res[(i + 1) % n] - res[(i + n - 1) % n];
		nrm[i] = m::la::length(tg) > 1e-12 ? V2(tg.y, -tg.x) / m::la::length(tg) : V2(1, 0);                    
	}
	const double a = std::min(amp, 0.35 * r.inradius);
	r.poly.resize(n);
	for (size_t i = 0; i < n; ++i) {
		const double s = perim * double(i) / double(n);
		const double low = Fbm(V(s / (2.2 * rag), 0.4, 0.0) + r.noise, 3);
		const double fine = std::abs(Fbm(V(s / (0.45 * rag), 1.9, 0.0) + r.noise * 1.3, 2));
		r.poly[i] = res[i] + nrm[i] * (a * (0.6 * low + 0.4 * (1.0 - 2.5 * fine)));
	}
	r.outN.resize(n);
	r.arc.resize(n);
	r.perim = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const V2 tg = r.poly[(i + 1) % n] - r.poly[(i + n - 1) % n];
		r.outN[i] = m::la::length(tg) > 1e-12 ? V2(tg.y, -tg.x) / m::la::length(tg) : nrm[i];
		r.arc[i] = r.perim;
		r.perim += m::la::length(r.poly[(i + 1) % n] - r.poly[i]);
	}
	return r.perim > 0.0;
}

                                                                             
                                                                          
                                                                             
                                                                            
                                                                      
                                                                             
                                                                       
m::Manifold DrapeRegion(const Sheet& s, const Region& r, double detail, double bevel = 0.0, int segs = 3)
{
	const m::Polygons polys{r.poly};
	double area = 0.0;
	for (size_t i = 0, j = r.poly.size() - 1; i < r.poly.size(); j = i++) area += 0.5 * Cross2(r.poly[j], r.poly[i]);
	                                                                         
	double c = std::max(0.35 * s.layer, r.inradius / 40.0) / ClampD(detail, 0.05, 4.0);
	c = std::max(std::min(c, 0.5 * r.inradius), std::sqrt(std::abs(area) / 60000.0));
	segs = std::max(1, std::min(segs, 12));
	bevel = ClampD(bevel, 0.0, 1.0);
	const int rows = 2 * segs + 3;                                            
	m::Manifold prism = m::Manifold::Extrude(polys, c, bevel > 0.0 ? rows - 1 : 0);
	if (prism.Status() != m::Manifold::Error::NoError || prism.NumTri() == 0) return m::Manifold();
	prism = prism.RefineToLength(c);
	m::MeshGL64 mesh = prism.GetMeshGL64();
	const size_t np = mesh.vertProperties.size() / mesh.numProp;
	std::vector<SpanHit> h(np);
	std::vector<char> ok(np, 0);
	const double R = r.reach + s.layer + s.margin;
	ProjectDamageParallelFor(0, int64_t(np), [&](int64_t i) {
		const double* v = &mesh.vertProperties[size_t(i) * mesh.numProp];
		if (s.Span(r.World(V2(v[0], v[1])), r.n, R, h[size_t(i)])) ok[size_t(i)] = 1;
	});
	                                
	std::vector<size_t> good;
	for (size_t i = 0; i < np; ++i) if (ok[i]) good.push_back(i);
	if (good.empty()) return m::Manifold();
	if (good.size() < np) {
		const double cell = 2.0 * c;
		std::unordered_map<uint64_t, std::vector<size_t>> g;
		auto key = [&](int64_t x, int64_t y) { return (uint64_t(x & 0xffffffff) << 32) | uint64_t(y & 0xffffffff); };
		auto cellOf = [&](size_t i, int64_t& x, int64_t& y) {
			x = int64_t(std::floor(mesh.vertProperties[i * mesh.numProp] / cell));
			y = int64_t(std::floor(mesh.vertProperties[i * mesh.numProp + 1] / cell));
		};
		for (size_t i : good) { int64_t x, y; cellOf(i, x, y); g[key(x, y)].push_back(i); }
		for (size_t i = 0; i < np; ++i) {
			if (ok[i]) continue;
			int64_t x, y;
			cellOf(i, x, y);
			const V2 q(mesh.vertProperties[i * mesh.numProp], mesh.vertProperties[i * mesh.numProp + 1]);
			size_t best = good[0];
			double bd = 1e300;
			bool found = false;
			for (int ring = 0; ring < 4096 && !found; ++ring) {
				for (int64_t dx = -ring; dx <= ring; ++dx)
					for (int64_t dy = -ring; dy <= ring; ++dy) {
						if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
						auto it = g.find(key(x + dx, y + dy));
						if (it == g.end()) continue;
						for (size_t k : it->second) {
							const V2 w(mesh.vertProperties[k * mesh.numProp], mesh.vertProperties[k * mesh.numProp + 1]);
							const double d = m::la::length2(w - q);
							if (d < bd) { bd = d; best = k; found = true; }
						}
					}
			}
			h[i] = h[best];
		}
	}
	                                                                        
	                                               
	std::vector<V2> outN;
	std::vector<double> rad;
	if (bevel > 0.0) {
		outN.assign(np, V2(0.0));
		std::vector<char> wall(np, 0);
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) {
			const size_t a = size_t(mesh.triVerts[k]), b = size_t(mesh.triVerts[k + 1]), d = size_t(mesh.triVerts[k + 2]);
			auto P3 = [&](size_t i) { return V(mesh.vertProperties[i * mesh.numProp], mesh.vertProperties[i * mesh.numProp + 1], mesh.vertProperties[i * mesh.numProp + 2]); };
			const V cr = m::la::cross(P3(b) - P3(a), P3(d) - P3(a));
			const double l = m::la::length(cr);
			if (l < 1e-30 || std::abs(cr.z) > 0.3 * l) continue;                    
			for (size_t i : { a, b, d }) { outN[i] += V2(cr.x, cr.y); wall[i] = 1; }
		}
		rad.assign(np, 0.0);
		for (size_t i = 0; i < np; ++i) {
			if (!wall[i]) continue;
			const double l = m::la::length(outN[i]);
			outN[i] = l > 1e-30 ? outN[i] / l : V2(0.0);
			                                                                                     
			rad[i] = 0.48 * (h[i].th > 0.0 ? h[i].th : std::min(s.layer, std::max(0.0, h[i].top - h[i].cutBot)));
		}
		                                                                     
		for (int tries = 0; tries < 5 && bevel > 0.0; ++tries) {
			bool folded = false;
			for (size_t k = 0; k < mesh.triVerts.size() && !folded; k += 3) {
				V2 a[3], b[3];
				bool cap = true;
				for (int j = 0; j < 3; ++j) {
					const size_t i = size_t(mesh.triVerts[k + j]);
					const double* v = &mesh.vertProperties[i * mesh.numProp];
					a[j] = V2(v[0], v[1]);
					b[j] = a[j] + outN[i] * (bevel * rad[i]);
					if (j > 0 && std::abs(v[2] - mesh.vertProperties[size_t(mesh.triVerts[k]) * mesh.numProp + 2]) > 1e-9 * c) cap = false;
				}
				if (!cap) continue;
				if (Cross2(a[1] - a[0], a[2] - a[0]) * Cross2(b[1] - b[0], b[2] - b[0]) <= 0.0) folded = true;
			}
			if (!folded) break;
			bevel = tries < 4 ? 0.5 * bevel : 0.0;
		}
	}
	for (size_t i = 0; i < np; ++i) {
		double* v = &mesh.vertProperties[i * mesh.numProp];
		const double w = ClampD(v[2] / c, 0.0, 1.0);
		double height = Lerp(h[i].cutBot, h[i].cutTop, w), off = 0.0;
		if (bevel > 0.0 && rad[i] > 0.0) {
			                                                                  
			                                                                   
			const double R = bevel * rad[i], top = h[i].top;
			auto at = [&](int k, double& hk, double& ok) {
				const double bot = h[i].th > 0.0 ? top - h[i].th : 0.0;
				if (k <= 0) { hk = h[i].cutBot; ok = h[i].th > 0.0 ? R : 0.0; }
				else if (k >= rows) { hk = h[i].cutTop; ok = R; }
				else if (k <= segs + 1) {
					if (h[i].th > 0.0) {
						const double ph = 0.5 * m::kPi * double(k - 1) / segs;
						hk = bot + R - R * std::cos(ph); ok = R - R * std::sin(ph);
					} else { hk = Lerp(h[i].cutBot, top - R, double(k) / (segs + 2)); ok = 0.0; }
				} else {
					const double ph = 0.5 * m::kPi * double(rows - 1 - k) / segs;
					hk = top - (R - R * std::cos(ph)); ok = R - R * std::sin(ph);
				}
			};
			const double kf = w * rows;
			const int k0 = std::min(rows - 1, int(std::floor(kf)));
			double h0, o0, h1, o1;
			at(k0, h0, o0); at(k0 + 1, h1, o1);
			const double f = ClampD(kf - k0, 0.0, 1.0);
			height = Lerp(h0, h1, f); off = Lerp(o0, o1, f);
		}
		const V2 q = V2(v[0], v[1]) + (bevel > 0.0 && !outN.empty() ? outN[i] * off : V2(0.0));
		const V P = r.World(q) + r.n * height;
		v[0] = P.x; v[1] = P.y; v[2] = P.z;
	}
	m::Manifold out(mesh);
	if (out.Status() == m::Manifold::Error::NoError && out.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		out = m::Manifold(mesh);
	}
	return out.Status() == m::Manifold::Error::NoError ? out : m::Manifold();
}

                                                                                      

struct ThreadCtx {
	const FabricTearParams* p = nullptr;
	const Sheet* s = nullptr;
	V G;                                 
	double radius = 0.05, anchorIn = 0.1;
	int sides = 6;
	int budget = 0;
	uint64_t seed = 0;
	uint64_t counter = 0;
	std::vector<m::Manifold> tubes;

	double Rnd(uint64_t ch) const { return Random01(seed, counter, ch); }
	bool Full() const { return int(tubes.size()) >= budget; }
	void Add(const std::vector<V>& pts, double r0, double r1)
	{
		if (Full()) return;
		m::Manifold t = Tube(pts, r0, r1, sides);
		if (t.Status() == m::Manifold::Error::NoError && t.NumTri() > 0) tubes.push_back(std::move(t));
	}

	                                                                             
	                                                                            
	void Loose(const V& anchor, const V& dir0, const V& sheetN, double length)
	{
		if (length <= 2.0 * radius) return;
		const double seg = std::max(3.0 * radius, length / 18.0);
		const int nseg = std::max(3, std::min(48, int(std::ceil(length / seg))));
		const double st = length / nseg;
		const V noff(Rnd(1) * 400.0, Rnd(2) * 400.0, Rnd(3) * 400.0);
		std::vector<V> pts{anchor};
		V dir = Unit(dir0, AnyPerp(sheetN));
		V pos = anchor + dir * (anchorIn + 0.5 * st);                             
		pts.push_back(pos);
		const double droop = ClampD(p->threadDroop, 0.0, 1.0);
		const double curl = ClampD(p->threadCurl, 0.0, 1.0);
		for (int i = 0; i < nseg; ++i) {
			const V q = V(i * 0.35, 0.0, 0.0) + noff;
			const V axis = Unit(V(Fbm(q, 2), Fbm(q + V(31.7, 0, 0), 2), Fbm(q + V(0, 57.1, 0), 2)), sheetN);
			const V curlDir = Unit(m::la::cross(dir, axis), AnyPerp(dir));
			const double pull = droop * (0.15 + 0.5 * double(i) / nseg);
			const V want = Unit(dir + G * pull + curlDir * (curl * 0.55), dir);
			dir = TurnToward(dir, want, 0.45);
			pos = pos + dir * st;
			pts.push_back(pos);
		}
		const double taper = ClampD(p->threadTaper, 0.05, 1.0);
		Add(pts, radius, radius * taper);
	}

	                                                                       
	void Bridge(const V& A, const V& B, const V& sheetN)
	{
		const double span = m::la::length(B - A);
		if (span <= 4.0 * radius) return;
		const V sd = (B - A) / span;
		V sagDir = G - sd * m::la::dot(G, sd);
		sagDir = m::la::length(sagDir) > 0.2 ? Unit(sagDir, -sheetN) : -sheetN;
		const V wob = Unit(m::la::cross(sd, sagDir), AnyPerp(sd));
		const double sag = ClampD(p->threadSag, 0.0, 0.24) * span * (0.5 + Rnd(4));
		const double wobble = ClampD(p->threadWobble, 0.0, 1.0) * span * 0.08;
		const int nseg = std::max(4, std::min(48, int(std::ceil(span / std::max(3.0 * radius, span / 20.0)))));
		const V noff(Rnd(5) * 400.0, Rnd(6) * 400.0, 0.0);
		std::vector<V> pts;
		for (int i = 0; i <= nseg; ++i) {
			const double t = double(i) / nseg;
			const double env = std::sin(m::kPi * t);
			pts.push_back(A + (B - A) * t + sagDir * (sag * 4.0 * t * (1.0 - t))
				+ wob * (wobble * env * Fbm(V(t * 3.0, 0.5, 0.0) + noff, 2)));
		}
		Add(pts, radius, radius);
	}

	                                                                      
	                                              
	void Span(const V& L, const V& Rr, const V& dirL, const V& dirR, const V& n)
	{
		const double span = m::la::length(Rr - L);
		if (span < 6.0 * radius) return;
		const double r = Rnd(8);
		const double bShare = p->bridgeEnable ? ClampD(p->bridgeShare, 0.0, 1.0) : 0.0;
		const double kShare = p->brokenEnable ? (1.0 - bShare) * ClampD(p->brokenShare, 0.0, 1.0) : 0.0;
		if (r < bShare) {
			Bridge(L, Rr, n);
		} else if (r < bShare + kShare) {
			const double u = Lerp(0.15, 0.85, Rnd(10));
			if (Rnd(11) < 0.85) Loose(L, dirL, n, u * span * Lerp(1.0, 1.6, Rnd(12)));
			++counter;
			if (Rnd(13) < 0.85) Loose(Rr, dirR, n, (1.0 - u) * span * Lerp(1.0, 1.6, Rnd(14)));
		}
	}

	                                                               
	void Fray(const V& anchor, const V& into, const V& n)
	{
		V d = Rotate(into, n, (2.0 * Rnd(17) - 1.0) * 0.8);
		d = Unit(d + n * ((2.0 * Rnd(18) - 1.0) * 0.45), into);
		Loose(anchor, d, n, Lerp(p->frayLengthMin, p->frayLengthMax, Rnd(19)));
	}

	                                                                            
	void AlongStrip(const Strip& st)
	{
		const size_t nr = st.arc.size();
		if (nr < 3) return;
		const double total = st.arc.back();
		auto rowAt = [&](double a) {
			const size_t i = size_t(std::lower_bound(st.arc.begin(), st.arc.end(), a) - st.arc.begin());
			return std::min(i, nr - 1);
		};
		const double rowStep = total / double(nr - 1);
		if (p->bridgeEnable || p->brokenEnable) {
			const double sp = std::max(2.5 * radius, p->threadSpacing);
			for (double a = sp * (0.5 + Random01(seed, counter++, 0)); a < total && !Full(); a += sp * (0.5 + Rnd(7))) {
				++counter;
				const size_t i = rowAt(a);
				if (!st.ok[i]) continue;
				const double span = m::la::length(st.right[i] - st.left[i]);
				const int dj = int(std::lround(ClampD(p->threadSkew, 0.0, 1.0) * (2.0 * Rnd(9) - 1.0) * span / std::max(1e-9, rowStep)));
				const size_t j = size_t(std::max<long long>(0, std::min<long long>((long long)nr - 1, (long long)i + dj)));
				if (!st.ok[j]) continue;
				Span(st.left[i] - st.side[i] * anchorIn, st.right[j] + st.side[j] * anchorIn, st.side[i], -st.side[j], st.n[i]);
			}
		}
		if (p->frayEnable) {
			const double sp = std::max(2.5 * radius, p->fraySpacing);
			for (int sideSign = -1; sideSign <= 1; sideSign += 2)
				for (double a = sp * Random01(seed, counter++, 15); a < total && !Full(); a += sp * (0.5 + Rnd(16))) {
					++counter;
					const size_t i = rowAt(a);
					if (!st.ok[i]) continue;
					const V rim = sideSign < 0 ? st.left[i] : st.right[i];
					const V into = st.side[i] * double(-sideSign);                                  
					Fray(rim - into * anchorIn, into, st.n[i]);
				}
		}
	}

	                                                                         
	                                    
	bool RegionAnchor(const Region& r, const V2& q, const V2& outN, double back, V& a, V& n) const
	{
		SpanHit h;
		const V P = r.World(q + outN * back);
		if (!s->Span(P, r.n, r.reach + s->layer + s->margin, h)) return false;
		a = P + r.n * h.anchor;
		n = h.nrm;
		return true;
	}

	                                                                          
	                                                                 
	void AlongRegion(const Region& r, const V& weave3)
	{
		V2 w(m::la::dot(weave3, r.t), m::la::dot(weave3, r.b));
		w = m::la::length(w) > 0.2 ? m::la::normalize(w) : V2(0.0, 1.0);
        const V2 v(-w.y,w.x);
        fabric_shapes::Shape sh;
        sh.pitch = std::max(2.5 * radius,p->yarnSize);
        m::SimplePolygon ring;
        for (const V2& q:r.poly) ring.push_back(V2(m::la::dot(q,w),m::la::dot(q,v)));
        sh.holes.push_back(std::move(ring));
        fabric_shapes::PopulateYarns(sh,*p,seed,r.id);
        const V T = r.t*w.x + r.b*w.y;
        const V gT = Unit(G-r.n*m::la::dot(G,r.n),T);
        auto lift = [&](const V2& q,V& pos,int& tag) {
            V n;
            tag=0;
            return RegionAnchor(r,w*q.x+v*q.y,V2(0.0),0.0,pos,n);
        };
        std::vector<fabric_shapes::YarnPath> paths;
        const size_t room = size_t(std::max(0,budget-int(tubes.size())));
        fabric_shapes::YarnPaths(sh,*p,seed,lift,T,-r.n,gT,room,paths);
        for (const auto& path:paths) Add(path.path,path.r0,path.r1);

		if (r.edge && p->hangEnable) {
			const double sp = std::max(4.0 * radius, p->paintHangSpacing);
			for (double a = sp * Random01(seed, counter++, 20); a < r.perim && !Full(); a += sp * (0.5 + Rnd(21))) {
				++counter;
				V2 q, on;
				r.At(a, q, on);
				V A, nA;
				if (!RegionAnchor(r, q, on, anchorIn, A, nA)) continue;
				const V into = Unit(r.Dir3(-on) - nA * m::la::dot(r.Dir3(-on), nA), r.Dir3(-on));
				Loose(A, Rotate(into, nA, (2.0 * Rnd(22) - 1.0) * 0.5), nA, Lerp(p->hangLengthMin, p->hangLengthMax, Rnd(23)));
			}
		}
	}
};

                                                                                      

enum class Kind { Hole, Edge, Hang, Drawn };
struct TearSpec {
	Kind kind = Kind::Hole;
	uint64_t id = 0;
	V pos, n, dir;                                                                     
	double length = 0.0, halfW = 0.0;
	int split = 1;
	V noise;
	int region = -1;                                             
};

struct TearOut {
	std::vector<m::Manifold> cuts;
	m::Manifold threads;
};

}             

void PlaceFabricTearsLegacy(const CoreInput& in, const FabricTearParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& threads,
	const std::vector<double>* zoneWeights, const FabricTearPaint* paint);

void PlaceFabricTears(const CoreInput& in, const FabricTearParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& threads,
	const std::vector<double>* zoneWeights, const FabricTearPaint* paint)
{
    if (!pIn.wovenThreads) { PlaceFabricTearsLegacy(in,pIn,mainSeed,mask,cutters,threads,zoneWeights,paint); return; }
	if (!pIn.enable) return;
	FabricTearParams p = pIn;
	                                                                                       
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.cutMargin = ClampD(p.cutMargin, 0.05, 4.0);
	p.holeAmount = std::max(0, std::min(p.holeAmount, 500));
	p.edgeAmount = std::max(0, std::min(p.edgeAmount, 1000));
	p.hangAmount = std::max(0, std::min(p.hangAmount, 2000));
	p.holeSplitMax = std::max(1, std::min(p.holeSplitMax, 8));
	p.threadSides = std::max(3, std::min(p.threadSides, 12));
	p.threadRadius = std::max(1e-5, p.threadRadius);
	p.maxThreads = std::max(0, std::min(p.maxThreads, 50000));
	p.paintEdgeSpacing = std::max(0.05, p.paintEdgeSpacing);
	if (p.holeLengthMax < p.holeLengthMin) std::swap(p.holeLengthMin, p.holeLengthMax);
	if (p.holeWidthMax < p.holeWidthMin) std::swap(p.holeWidthMin, p.holeWidthMax);
	if (p.edgeLengthMax < p.edgeLengthMin) std::swap(p.edgeLengthMin, p.edgeLengthMax);
	if (p.edgeWidthMax < p.edgeWidthMin) std::swap(p.edgeWidthMin, p.edgeWidthMax);
	if (p.frayLengthMax < p.frayLengthMin) std::swap(p.frayLengthMin, p.frayLengthMax);
	if (p.hangLengthMax < p.hangLengthMin) std::swap(p.hangLengthMin, p.hangLengthMax);
	if (p.paintEdgeLengthMax < p.paintEdgeLengthMin) std::swap(p.paintEdgeLengthMin, p.paintEdgeLengthMax);
	if (p.paintEdgeWidthMax < p.paintEdgeWidthMin) std::swap(p.paintEdgeWidthMin, p.paintEdgeWidthMax);

	const bool drawnHoles = paint && paint->holes && !paint->holes->empty() && p.paintHolesEnable;
	const bool drawnEdges = paint && paint->edges && !paint->edges->empty() && p.paintEdgesEnable;
	const bool procedural = !(p.paintOnly && (drawnHoles || drawnEdges));
	const bool holesOn = procedural && p.holesEnable && p.holeAmount > 0 && p.holeLengthMax > 0.0 && p.holeWidthMax > 0.0;
	const bool edgesOn = procedural && p.edgeEnable && p.edgeAmount > 0 && p.edgeLengthMax > 0.0;
	const bool hangOn = procedural && p.threadsEnable && p.hangEnable && p.hangAmount > 0 && p.hangLengthMax > 0.0;
	if (!holesOn && !edgesOn && !hangOn && !drawnHoles && !drawnEdges) return;

	Sheet s;
	if (!BuildSheet(in, p, s)) return;
	const MeshView& v = in.view;
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4641425249435445ULL)                  
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4654;                             
	const V weaveAxis = Unit(V(p.holeDirX, p.holeDirY, p.holeDirZ), V(1, 0, 0));

	auto triWeight = [&](size_t ti) {
		const Tri& t = s.tris[ti];
		if (p.avoid && in.avoidPoly && size_t(t.poly) < in.avoidPoly->size() && (*in.avoidPoly)[size_t(t.poly)]) return 0.0;
		double w = 1.0;
		if (zoneWeights && zoneWeights->size() >= size_t(v.npts))
			w = ClampD(((*zoneWeights)[size_t(t.a)] + (*zoneWeights)[size_t(t.b)] + (*zoneWeights)[size_t(t.c)]) / 3.0, 0.0, 1.0);
		return w;
	};
	auto samplePoint = [&](size_t ti, double r1, double r2) {
		const Tri& t = s.tris[ti];
		if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
		return v.pts[t.a] * (1.0 - r1 - r2) + v.pts[t.b] * r1 + v.pts[t.c] * r2;
	};

	std::vector<TearSpec> specs;

	                                                                                
	if (holesOn) {
		const uint64_t hs = seed ^ 0x484f4c45ULL;            
		std::vector<double> cdf(s.tris.size());
		double area = 0.0;
		for (size_t i = 0; i < s.tris.size(); ++i) {
			area += s.border[i] ? 0.0 : s.area[i] * triWeight(i);
			cdf[i] = area;
		}
		std::vector<std::pair<V, double>> placed;
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.holeAmount) * 24 && area > 0.0 && accepted < p.holeAmount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(hs, k, ch); };
			const size_t ti = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
			if (ti >= s.tris.size()) continue;
			const V pos = samplePoint(ti, rnd(1), rnd(2));
            if(rnd(91)>fabric_noise::Weight(p,V(pos),seed))continue;
			if (mask && !mask->Keep(pos, int32_t(k), kStream)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			const V n = GuidedNormal(*s.v,s.tris[ti].poly,s.tn[ti]);
			V dir;
			const V proj = weaveAxis - n * m::la::dot(weaveAxis, n);
			if (p.holeDirMode == 1 && m::la::length(proj) > 0.15) dir = Unit(proj, AnyPerp(n));
			else dir = Rotate(AnyPerp(n), n, rnd(3) * 2.0 * m::kPi);
			dir = Rotate(dir, n, (2.0 * rnd(4) - 1.0) * p.holeDirJitter * m::kPi / 180.0);
			TearSpec t;
			t.kind = Kind::Hole; t.id = k; t.pos = pos; t.n = n; t.dir = dir;
			t.length = Lerp(p.holeLengthMin, p.holeLengthMax, rnd(5)) * sc;
			t.halfW = 0.5 * Lerp(p.holeWidthMin, p.holeWidthMax, rnd(6)) * sc;
			t.split = 1 + int(std::floor(rnd(7) * p.holeSplitMax));
			t.split = std::max(1, std::min(t.split, p.holeSplitMax));
			t.noise = V(rnd(8) * 1000.0, rnd(9) * 1000.0, rnd(10) * 1000.0);
			const double rad = 0.5 * t.length + t.halfW;
			bool clash = false;
			for (const auto& q : placed)
				if (m::la::length(q.first - pos) < 0.6 * (q.second + rad)) { clash = true; break; }
			if (clash) continue;
			placed.push_back({pos, rad});
			specs.push_back(t);
			++accepted;
		}
	}

	                                                                                
	std::vector<double> bcdf(s.tris.size());
	double barea = 0.0;
	for (size_t i = 0; i < s.tris.size(); ++i) {
		barea += s.border[i] ? s.area[i] * triWeight(i) : 0.0;
		bcdf[i] = barea;
	}
	                                                                         
	                                                                         
	auto borderFrame = [&](size_t ti, const V& pos, V& n, V& inward) {
		V sn;
		if (!s.NearestSheetNormal(pos, sn)) return false;
		const V nr = s.tn[ti];
		const V out = nr - sn * m::la::dot(nr, sn);
		if (m::la::length(out) < 0.3) return false;
		inward = -Unit(out, AnyPerp(sn));
		n = sn;
		return true;
	};

	if (edgesOn && barea > 0.0) {
		const uint64_t es = seed ^ 0x45444745ULL;            
		for (int k = 0; k < p.edgeAmount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(es, uint64_t(k), ch); };
			const double jit = ClampD(p.edgeSpacingJitter, 0.0, 1.0);
			const double u = (k + 0.5 + jit * (rnd(11) - 0.5)) / p.edgeAmount;
			const size_t ti = size_t(std::upper_bound(bcdf.begin(), bcdf.end(), ClampD(u, 0.0, 0.999999) * barea) - bcdf.begin());
			if (ti >= s.tris.size()) continue;
			const V pos = samplePoint(ti, rnd(1), rnd(2));
			if (mask && !mask->Keep(pos, int32_t(k), kStream + 1)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			V n, inward;
			if (!borderFrame(ti, pos, n, inward)) continue;
			TearSpec t;
			t.kind = Kind::Edge; t.id = uint64_t(k); t.pos = pos; t.n = n;
			t.dir = Rotate(inward, n, (2.0 * rnd(4) - 1.0) * p.edgeAngleJitter * m::kPi / 180.0);
			t.length = Lerp(p.edgeLengthMin, p.edgeLengthMax, std::pow(rnd(5), 1.3)) * sc;
			t.halfW = 0.5 * Lerp(p.edgeWidthMin, p.edgeWidthMax, std::pow(rnd(6), 1.5)) * sc;
			t.noise = V(rnd(8) * 1000.0, rnd(9) * 1000.0, rnd(10) * 1000.0);
			specs.push_back(t);
		}
	}
	if (hangOn && barea > 0.0) {
		TearSpec t;
		t.kind = Kind::Hang; t.id = 0;
		specs.push_back(t);
	}

	                                                                                
	std::vector<Region> regions;
	{
		const double reach = paint ? std::max(1e-3, paint->reach) : 5.0;
		const double amp = std::max(0.0, p.paintJagged) * std::max(1e-4, p.paintRagSize);
		auto addRegions = [&](const std::vector<SurfacePaintRegion>* list, bool edge) {
			for (size_t i = 0; i < list->size(); ++i) {
				Region r;
				r.edge = edge;
				r.id = uint64_t(i) | (edge ? (uint64_t(1) << 40) : 0);
				const uint64_t rs = seed ^ 0x4452574eULL ^ (r.id * 0x9e3779b97f4a7c15ULL);            
				r.noise = V(Random01(rs, 0, 8) * 1000.0, Random01(rs, 0, 9) * 1000.0, Random01(rs, 0, 10) * 1000.0);
				if (!BuildRegion((*list)[i], p, reach, amp, r)) continue;
				regions.push_back(std::move(r));
			}
		};
		if (drawnHoles) addRegions(paint->holes, false);
		if (drawnEdges) addRegions(paint->edges, true);
	}
	for (size_t ri = 0; ri < regions.size(); ++ri) {
		const Region& r = regions[ri];
		TearSpec t;
		t.kind = Kind::Drawn; t.id = r.id; t.region = int(ri);
		specs.push_back(t);
		if (!r.edge) continue;
		                                                         
		const uint64_t es = seed ^ 0x534c4954ULL ^ (r.id * 0xe7037ed1a0b428dbULL);            
		uint64_t k = 0;
		for (double a = p.paintEdgeSpacing * Random01(es, 0, 0); a < r.perim; a += p.paintEdgeSpacing * (0.5 + Random01(es, k, 1)), ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(es, k, ch); };
			V2 q, on;
			r.At(a, q, on);
			SpanHit h;
			const V P = r.World(q);
			if (!s.Span(P, r.n, r.reach + s.layer + s.margin, h)) continue;
			const V hn = h.nrm;
			V out3 = r.Dir3(on);
			out3 = out3 - hn * m::la::dot(out3, hn);
			if (m::la::length(out3) < 0.2) continue;
			TearSpec e;
			e.kind = Kind::Edge;
			e.id = (r.id << 16) ^ k ^ 0x5a5a000000000000ULL;
			e.pos = P + r.n * h.top;
			e.n = hn;
			e.dir = Rotate(Unit(out3, AnyPerp(hn)), hn, (2.0 * rnd(4) - 1.0) * p.edgeAngleJitter * m::kPi / 180.0);
			e.length = Lerp(p.paintEdgeLengthMin, p.paintEdgeLengthMax, std::pow(rnd(5), 1.3));
			e.halfW = 0.5 * Lerp(p.paintEdgeWidthMin, p.paintEdgeWidthMax, std::pow(rnd(6), 1.5));
			e.noise = V(rnd(8) * 1000.0, rnd(9) * 1000.0, rnd(10) * 1000.0);
			e.region = -2;                                                 
			specs.push_back(e);
		}
	}
	if (specs.empty()) return;

	const V G = Unit(V(p.gravityX, p.gravityY, p.gravityZ), V(0, 0, -1));
	const int budget = p.threadsEnable ? std::max(0, p.maxThreads / int(specs.size())) : 0;
	std::vector<TearOut> outs(specs.size());

	ProjectDamageParallelFor(0, int64_t(specs.size()), [&](int64_t si) {
		const TearSpec& t = specs[size_t(si)];
		TearOut& out = outs[size_t(si)];
		ThreadCtx tc;
		tc.p = &p; tc.s = &s; tc.G = G;
		tc.radius = p.threadRadius;
		tc.anchorIn = std::max(2.0 * p.threadRadius, 0.3 * s.layer);
		tc.sides = p.threadSides;
		tc.budget = budget;
		tc.seed = seed ^ (uint64_t(t.kind) + 1) * 0xa0761d6478bd642fULL ^ (t.id * 0xe7037ed1a0b428dbULL);

		if (t.kind == Kind::Hole) {
			const double feature = std::max(1e-6, t.halfW);
			const double stepT = std::max({0.11 * t.halfW, 0.25 * s.layer, t.length / 400.0}) / p.detail;
			const int half = std::max(4, int(std::ceil(0.5 * t.length / stepT)));
			const double step = 0.5 * t.length / half;
			                                                                
			auto fwd = Walk(s, t.pos, t.n, t.dir, step, half, p.holeMeander, 2.0 * feature, t.noise);
			auto bwd = Walk(s, t.pos, t.n, -t.dir, step, half, p.holeMeander, 2.0 * feature, t.noise + V(53.0, 0, 0));
			std::vector<PathPt> path;
			for (size_t i = bwd.size(); i-- > 1;) path.push_back({bwd[i].pos, bwd[i].n, -bwd[i].dir});
			for (const PathPt& q : fwd) path.push_back(q);
			if (path.size() < 6) return;
			const int np = int(path.size());
			const double L = step * (np - 1);
			                                         
			int split = t.split;
			const double bridge = std::max(0.0, p.holeBridge);
			while (split > 1 && L - (split - 1) * bridge < 0.4 * L) --split;
			std::vector<double> wts(static_cast<size_t>(split));
			double wsum = 0.0;
			for (int k = 0; k < split; ++k) { wts[size_t(k)] = 0.5 + Random01(tc.seed, uint64_t(k), 20); wsum += wts[size_t(k)]; }
			const double avail = L - (split - 1) * bridge;
			double a0 = 0.0;
			for (int k = 0; k < split; ++k) {
				const double plen = avail * wts[size_t(k)] / wsum;
				const int i0 = int(std::lround(a0 / step)), i1 = std::min(np - 1, int(std::lround((a0 + plen) / step)));
				a0 += plen + bridge;
				if (i1 - i0 < 5) continue;
				const double wScale = split > 1 ? Lerp(0.55, 1.0, Random01(tc.seed, uint64_t(k), 21)) : 1.0;
				const V offL = t.noise + V(0.0, 11.0 * (k + 1), 0.0), offR = t.noise + V(0.0, -23.0 * (k + 1), 7.0);
				std::vector<Row> rows;
				for (int i = i0; i <= i1; ++i) {
					const PathPt& q = path[size_t(i)];
					const double tt = double(i - i0) / (i1 - i0);
					const double sa = i * step;
					const double base = std::pow(std::sin(m::kPi * tt), 0.55);
					const double minW = std::max(0.25 * p.threadRadius, 1e-3 * t.halfW);
					Row r;
					r.c = q.pos; r.n = q.n;
					r.side = Unit(m::la::cross(q.n, q.dir), AnyPerp(q.n));
					r.wl = std::max(minW, t.halfW * wScale * base * Ragged(sa, feature, p.holeLobes, p.holeJagged, offL));
					r.wr = std::max(minW, t.halfW * wScale * base * Ragged(sa, feature, p.holeLobes, p.holeJagged, offR));
					r.real = tt > 0.02 && tt < 0.98;
					rows.push_back(r);
				}
				Strip st;
				const double probe = 2.0 * s.layer + 0.6 * t.halfW + s.margin;
				if (!BuildStrip(s, rows, probe, st)) continue;
				out.cuts.push_back(st.solid);
				if (p.threadsEnable) tc.AlongStrip(st);
			}
		} else if (t.kind == Kind::Edge) {
			const double feature = std::max(1e-6, std::max(t.halfW, 0.08 * t.length));
			const double stepT = std::max({0.06 * t.length, 0.25 * s.layer}) / p.detail;
			const double stepT2 = std::min(stepT, std::max(0.35 * feature, 0.25 * s.layer) / p.detail);
			const int count = std::max(6, std::min(400, int(std::ceil(t.length / stepT2))));
			const double step = t.length / count;
			                                                                       
			                              
			V start, sn;
			const double lead = t.region == -2 ? 0.0 : std::max(s.layer, step);
			if (!s.Project(t.pos + t.dir * lead, t.n, 3.0 * s.layer + step, start, sn)) return;
			auto path = Walk(s, start, sn, t.dir, step, count, p.edgeMeander, 2.0 * feature, t.noise);
			if (path.size() < 4) return;
			const double ext = 1.5 * s.layer + t.halfW + 2.0 * s.margin;                                                       
			std::vector<Row> rows;
			const V offL = t.noise + V(0.0, 13.0, 0.0), offR = t.noise + V(0.0, -29.0, 5.0);
			const PathPt& p0 = path.front();
			const V side0 = Unit(m::la::cross(p0.n, p0.dir), AnyPerp(p0.n));
			const double startOff = m::la::length(start - t.pos) + ext;
			for (int k = 0; k < 3; ++k) {
				Row r;
				r.c = p0.pos - p0.dir * (startOff * (1.0 - k / 3.0)); r.n = p0.n; r.side = side0;
				r.wl = r.wr = t.halfW * 1.15;
				r.real = false;
				rows.push_back(r);
			}
			const int np = int(path.size());
			for (int i = 0; i < np; ++i) {
				const PathPt& q = path[size_t(i)];
				const double tt = double(i) / (np - 1);
				const double base = std::pow(std::max(0.0, 1.0 - tt), std::max(0.1, p.edgeTaper));
				const double minW = std::max(0.25 * p.threadRadius, 1e-3 * t.halfW);
				const double sa = i * step;
				Row r;
				r.c = q.pos; r.n = q.n;
				r.side = Unit(m::la::cross(q.n, q.dir), AnyPerp(q.n));
				r.wl = std::max(minW, t.halfW * base * Ragged(sa, feature, 0.25, p.edgeJagged, offL));
				r.wr = std::max(minW, t.halfW * base * Ragged(sa, feature, 0.25, p.edgeJagged, offR));
				r.real = tt < 0.97;
				rows.push_back(r);
			}
			Strip st;
			const double probe = 2.0 * s.layer + 0.6 * t.halfW + s.margin;
			if (!BuildStrip(s, rows, probe, st)) return;
			out.cuts.push_back(st.solid);
			if (p.threadsEnable) tc.AlongStrip(st);
		} else if (t.kind == Kind::Hang) {
			                                               
			const uint64_t hs = seed ^ 0x48414e47ULL;            
			for (int k = 0; k < p.hangAmount && !tc.Full(); ++k) {
				auto rnd = [&](uint64_t ch) { return Random01(hs, uint64_t(k), ch); };
				const size_t ti = size_t(std::upper_bound(bcdf.begin(), bcdf.end(), rnd(0) * barea) - bcdf.begin());
				if (ti >= s.tris.size()) continue;
				const V pos = samplePoint(ti, rnd(1), rnd(2));
				if (mask && !mask->Keep(pos, int32_t(k), kStream + 2)) continue;
				V n, inward;
				if (!borderFrame(ti, pos, n, inward)) continue;
				const V a = pos + inward * tc.anchorIn;
				SpanHit h;
				if (!s.Span(a, n, 3.0 * s.layer, h)) continue;
				tc.counter = uint64_t(k) * 64;
				const V away = Rotate(-inward, n, (2.0 * rnd(3) - 1.0) * 0.5);
				tc.Loose(a + n * h.anchor, away, n, Lerp(p.hangLengthMin, p.hangLengthMax, rnd(4)));
			}
		} else {
			const Region& r = regions[size_t(t.region)];
			m::Manifold cut = DrapeRegion(s, r, p.detail, p.solid ? p.solidBevel : 0.0, p.solidBevelSegments);
			if (cut.NumTri() == 0) return;
			out.cuts.push_back(std::move(cut));
			if (p.threadsEnable) tc.AlongRegion(r, weaveAxis);
		}
		if (!tc.tubes.empty()) {
			m::Manifold merged = m::Manifold::BatchBoolean(tc.tubes, m::OpType::Add);
			if (merged.Status() == m::Manifold::Error::NoError && merged.NumTri() > 0) out.threads = merged;
		}
	});

	for (TearOut& o : outs) {
		for (m::Manifold& c : o.cuts) cutters.push_back(std::move(c));
		if (o.threads.NumTri() > 0) threads.push_back(std::move(o.threads));
	}
}

PROJECT_DAMAGE_NS_MACRO_END
