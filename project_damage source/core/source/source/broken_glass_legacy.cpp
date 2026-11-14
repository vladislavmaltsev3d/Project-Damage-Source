// предыдущий вариант построения разбитого стекла
// ProjectDamage broken glass, LEGACY thickness logic — GPL-3.0-or-later; see LICENSE.
//
// This is broken_glass.cpp exactly as it was before the statue fix (v1),
// kept so BrokenGlassParams::thicknessLogic = 0 reproduces older results.
// Only the entry point is renamed (PlaceBrokenGlassLegacy); everything else
// lives in this file's own anonymous namespace. Do not change it - fixes go
// into broken_glass.cpp.
//
// Recipe: broken-glass-v1. A procedural appearance model of a shattered pane,
// not a fracture simulation.
//   1. pane analysis: every triangle casts a ray inward; the area-weighted
//      median ray length is the thickness T. A THIN PANE (T <= 6% of the
//      diagonal) is cut through; anything thicker gets a skin of skinDepth.
//      On thin panes, triangles whose inward ray is much longer than T (or
//      misses) are the pane's edge faces and never seed an impact;
//   2. impacts: automatic ones are seeded area-weighted on the pane faces;
//      drawn ones come from Grease Pencil (closed contour = opening outline,
//      dot = impact point, longer stroke = crack line). Each impact works in
//      its own plane frame and is draped onto the surface along its normal;
//   3. opening: a base outline (a lobed circle, or the drawn contour) is split
//      into sectors. Where a sector boundary meets the opening, a radial crack
//      starts and the opening reaches a little further out (a notch). Between
//      notches a sector either keeps one or two long dagger shards pointing
//      inward, or is knocked out further. Edges are straight with small kinks
//      - glass breaks along lines, not along noise. The outline is cleaned by
//      CrossSection, extruded, refined and draped (through a pane, a pocket in
//      a solid);
//   4. cracks: radial cracks run almost straight out of every notch and fork;
//      broken concentric cracks join neighbouring radials as slightly bowed
//      chords; daggers get a crack across them. Each crack is a thin V-groove
//      draped onto the front face (and the back face, depth split) that stops
//      where it runs off the pane, so the pane stays a single piece;
//   5. chips: shallow conchoidal scoops on the exit face along the opening,
//      deepest at the rim.
// Every impact draws from its own SplitMix64 stream keyed by seed (main seed
// and Glass Seed) and impact id, so adding an impact or a stroke never
// reshuffles the others.
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "broken_glass.h"
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

                                                                            
                                                                 
struct Stream {
	uint64_t seed = 0, channel = 0, n = 0;
	Stream(uint64_t s, uint64_t ch) : seed(s), channel(ch) {}
	double operator()() { return Random01(seed, n++, channel); }
	double Range(double a, double b) { const double u = (*this)(); return a + (b - a) * u; }
	double Sym() { return 2.0 * (*this)() - 1.0; }
};

V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}
V2 Unit2(const V2& v, const V2& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}
V2 Perp(const V2& v) { return V2(-v.y, v.x); }
V2 Rot2(const V2& v, double a) { const double c = std::cos(a), s = std::sin(a); return V2(v.x * c - v.y * s, v.x * s + v.y * c); }
double Cross2(const V2& a, const V2& b) { return a.x * b.y - a.y * b.x; }

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

                                                                         
m::Manifold Oriented(m::MeshGL64& mesh)
{
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	return solid.Status() == m::Manifold::Error::NoError ? solid : m::Manifold();
}

                                                                                   

struct Tri { int32_t a, b, c, poly; };

                                                                               
                                                                             
struct SpanHit { double top = 0.0, th = 0.0; bool through = true; V nrm; };

struct Pane {
	const MeshView* v = nullptr;
	std::vector<Tri> tris;
	std::vector<V> tn, tc;
	std::vector<double> area;
	std::vector<V> polyN;
	std::vector<char> edgeFace;
	ProjectDamageBVH bvh;
	const std::vector<bool>* avoid = nullptr;
	double T = 0.0, eps = 1e-6, diag = 1.0;
	bool thin = true;
	double layer = 0.0;                                                
	double throughMax = 0.0;                                                         
	double pocket = 0.0;                                         
	double margin = 0.0;                          

	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}

	                                                                         
	                                   
	bool Span(const V& P, const V& N, double R, SpanHit& o, bool useAvoid) const
	{
		double d1 = 0.0;
		int32_t poly = -1;
		if (!bvh.Raycast(P + N * R, -N, 2.0 * R, &d1, &poly) || poly < 0) return false;
		if (useAvoid && Avoided(poly)) return false;
		const V hn = polyN[size_t(poly)];
		if (m::la::dot(hn, N) < 0.25) return false;                                  
		o.top = R - d1;
		o.nrm = hn;
		const double back = 10.0 * eps;
		double d2 = 0.0;
		double th = 1e300;
		if (bvh.Raycast(P + N * (o.top - back), -N, 1.5 * throughMax + margin, &d2)) th = d2 + back;
		if (th <= throughMax) { o.th = th; o.through = true; }
		else { o.th = pocket; o.through = false; }
		return true;
	}
};

// основной вход: собираем геометрию по переданным настройкам
bool BuildPane(const CoreInput& in, const BrokenGlassParams& p, Pane& s)
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
	for (V& n : s.polyN) n = Unit(n, V(0, 0, 1));
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
	if (p.thickness > 0.0) s.T = p.thickness;
	s.thin = p.thickness > 0.0 || (s.T > 0.0 && s.T <= 0.06 * s.diag);
	const double skin = p.skinDepth > 0.0 ? p.skinDepth : 0.015 * s.diag;
	if (s.thin) {
		s.layer = s.T;
		s.throughMax = 3.0 * s.T;
		s.pocket = s.T;
	} else {
		s.layer = skin;
		s.throughMax = skin;
		s.pocket = skin;
	}
	if (!(s.layer > 0.0)) return false;
	s.margin = std::max(ClampD(p.cutMargin, 0.05, 4.0) * s.layer, 4.0 * s.eps);
	s.edgeFace.assign(s.tris.size(), 0);
	if (s.thin)
		for (size_t i = 0; i < s.tris.size(); ++i)
			s.edgeFace[i] = (dist[i] < 0.0 || dist[i] > 3.0 * s.T) ? 1 : 0;
	if (p.avoid) s.avoid = in.avoidPoly;
	return true;
}

                                                                                     

struct Frame {
	V o, n, t, b;
	V World(const V2& q) const { return o + t * q.x + b * q.y; }
	V Dir3(const V2& d) const { return Unit(t * d.x + b * d.y, t); }
	V2 Local(const V& q) const { return V2(m::la::dot(q - o, t), m::la::dot(q - o, b)); }
};

Frame MakeFrame(const V& o, const V& n, double spin)
{
	Frame f;
	f.o = o;
	f.n = Unit(n, V(0, 0, 1));
	const V axis = std::abs(f.n.z) < 0.9 ? V(0, 0, 1) : V(0, 1, 0);
	f.t = Rotate(Unit(m::la::cross(axis, f.n), AnyPerp(f.n)), f.n, spin);
	f.b = m::la::cross(f.n, f.t);
	return f;
}

                                                                            
                                                                          
                                                                              
                                                                           
                                                                  
template <class DepthFn>
m::Manifold Drape(const Pane& s, const m::Polygons& polys, const Frame& f, double side, double reach,
	double cell, bool through, DepthFn depthOf)
{
	m::Manifold prism = m::Manifold::Extrude(polys, 1.0);
	if (prism.Status() != m::Manifold::Error::NoError || prism.NumTri() == 0) return m::Manifold();
	prism = prism.RefineToLength(cell);
	m::MeshGL64 mesh = prism.GetMeshGL64();
	const size_t np = mesh.vertProperties.size() / mesh.numProp;
	const V N = f.n * side;
	std::vector<SpanHit> h(np);
	std::vector<char> ok(np, 0);
	const double R = reach + s.layer + s.margin;
	ProjectDamageParallelFor(0, int64_t(np), [&](int64_t i) {
		const double* v = &mesh.vertProperties[size_t(i) * mesh.numProp];
		if (s.Span(f.World(V2(v[0], v[1])), N, R, h[size_t(i)], !through)) ok[size_t(i)] = 1;
	});
	std::vector<size_t> good;
	for (size_t i = 0; i < np; ++i) if (ok[i]) good.push_back(i);
	if (good.empty()) return m::Manifold();
	if (good.size() < np) {
		const double gc = 2.0 * cell;
		std::unordered_map<uint64_t, std::vector<size_t>> g;
		auto key = [&](int64_t x, int64_t y) { return (uint64_t(x & 0xffffffff) << 32) | uint64_t(y & 0xffffffff); };
		auto cellOf = [&](size_t i, int64_t& x, int64_t& y) {
			x = int64_t(std::floor(mesh.vertProperties[i * mesh.numProp] / gc));
			y = int64_t(std::floor(mesh.vertProperties[i * mesh.numProp + 1] / gc));
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
			for (int ring = 0; ring < 4096 && !found; ++ring)
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
			h[i] = h[best];
		}
	}
	for (size_t i = 0; i < np; ++i) {
		double* v = &mesh.vertProperties[i * mesh.numProp];
		const V2 q(v[0], v[1]);
		const double w = ClampD(v[2], 0.0, 1.0);
		const double top = h[i].top + s.margin;
		double bot;
		if (through) bot = h[i].through ? h[i].top - h[i].th - s.margin : h[i].top - h[i].th;
		else bot = h[i].top - ClampD(depthOf(q), 0.02, 0.95) * h[i].th;
		const V P = f.World(q) + N * Lerp(bot, top, w);
		v[0] = P.x; v[1] = P.y; v[2] = P.z;
	}
	return Oriented(mesh);
}

                                                                                       

struct Line {
	std::vector<V2> pts;
	std::vector<double> w;                           
};

                                                                          
                 
m::Manifold GrooveSolid(const std::vector<V>& tl, const std::vector<V>& tr, const std::vector<V>& bl, const std::vector<V>& br)
{
	const size_t n = tl.size();
	if (n < 2) return m::Manifold();
	m::MeshGL64 mesh;
	mesh.numProp = 3;
	auto push = [&](const V& q) { mesh.vertProperties.insert(mesh.vertProperties.end(), {q.x, q.y, q.z}); };
	for (size_t i = 0; i < n; ++i) { push(tl[i]); push(tr[i]); push(br[i]); push(bl[i]); }
	auto I = [&](size_t i, int k) { return uint64_t(4 * i + size_t(k)); };
	auto quad = [&](uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
		mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
	};
	for (size_t i = 0; i + 1 < n; ++i)
		for (int k = 0; k < 4; ++k) quad(I(i, k), I(i + 1, k), I(i + 1, (k + 1) % 4), I(i, (k + 1) % 4));
	quad(I(0, 0), I(0, 1), I(0, 2), I(0, 3));
	quad(I(n - 1, 3), I(n - 1, 2), I(n - 1, 1), I(n - 1, 0));
	return Oriented(mesh);
}

                                                                        
                                                                             
                                                               
void Groove(const Pane& s, const Frame& f, const Line& line, double side, double depthShare, double reach,
	double step, std::vector<m::Manifold>& out)
{
	if (line.pts.size() < 2) return;
	std::vector<V2> q;
	std::vector<double> w;
	for (size_t i = 0; i + 1 < line.pts.size(); ++i) {
		const double len = m::la::length(line.pts[i + 1] - line.pts[i]);
		const int k = std::max(1, int(std::ceil(len / step)));
		for (int j = 0; j < k; ++j) {
			const double u = double(j) / k;
			q.push_back(line.pts[i] + (line.pts[i + 1] - line.pts[i]) * u);
			w.push_back(Lerp(line.w[i], line.w[i + 1], u));
		}
	}
	q.push_back(line.pts.back());
	w.push_back(line.w.back());
	const V N = f.n * side;
	const double R = reach + s.layer + s.margin;
	std::vector<V> tl, tr, bl, br;
	auto flush = [&]() {
		if (tl.size() >= 2) {
			m::Manifold g = GrooveSolid(tl, tr, bl, br);
			if (g.NumTri() > 0) out.push_back(std::move(g));
		}
		tl.clear(); tr.clear(); bl.clear(); br.clear();
	};
	for (size_t i = 0; i < q.size(); ++i) {
		SpanHit h;
		if (!s.Span(f.World(q[i]), N, R, h, true)) { flush(); continue; }
		const V2 tg = Unit2(q[std::min(i + 1, q.size() - 1)] - q[i > 0 ? i - 1 : 0], V2(1, 0));
		const V sd = f.Dir3(Perp(tg));
		const V S = f.World(q[i]) + N * h.top;
		const double depth = ClampD(depthShare, 0.02, 0.95) * h.th;
		const double hw = 0.5 * w[i], hb = 0.12 * hw;
		tl.push_back(S + N * s.margin + sd * hw);
		tr.push_back(S + N * s.margin - sd * hw);
		bl.push_back(S - N * depth + sd * hb);
		br.push_back(S - N * depth - sd * hb);
	}
	flush();
}

                                                                            
                                                                      
Line RadialLine(const V2& start, const V2& dir, double len, double step, double meander, double width,
	double taper, const V& noise, Stream& rs)
{
	Line l;
	const int n = std::max(2, int(std::ceil(len / step)));
	const double st = len / n;
	const double a0 = std::atan2(dir.y, dir.x);
	V2 pos = start;
	l.pts.push_back(pos);
	l.w.push_back(width);
	for (int i = 1; i <= n; ++i) {
		const double s = double(i) / n;
		const double a = a0 + meander * (0.9 * Fbm(V(s * 2.2, 0.31, 0.0) + noise, 2) + 0.35 * rs.Sym());
		pos = pos + V2(std::cos(a), std::sin(a)) * st;
		l.pts.push_back(pos);
		const double tip = ClampD((s - 0.65) / 0.35, 0.0, 1.0);
		l.w.push_back(width * Lerp(1.0, ClampD(taper, 0.05, 1.0), tip * tip));
	}
	return l;
}

V2 PointAlong(const Line& l, double d, bool& ok)
{
	double acc = 0.0;
	for (size_t i = 0; i + 1 < l.pts.size(); ++i) {
		const double len = m::la::length(l.pts[i + 1] - l.pts[i]);
		if (acc + len >= d) { ok = true; return l.pts[i] + (l.pts[i + 1] - l.pts[i]) * ((d - acc) / std::max(1e-12, len)); }
		acc += len;
	}
	ok = false;
	return l.pts.back();
}

double LineLength(const Line& l)
{
	double acc = 0.0;
	for (size_t i = 0; i + 1 < l.pts.size(); ++i) acc += m::la::length(l.pts[i + 1] - l.pts[i]);
	return acc;
}

                                                                                        

                                                             
struct Loop {
	std::vector<V2> poly, outN;
	std::vector<double> arc;
	double perim = 0.0;
	bool Build(std::vector<V2> raw)
	{
		if (raw.size() < 3) return false;
		double area2 = 0.0;
		for (size_t i = 0, j = raw.size() - 1; i < raw.size(); j = i++) area2 += Cross2(raw[j], raw[i]);
		if (std::abs(area2) < 1e-12) return false;
		if (area2 < 0.0) std::reverse(raw.begin(), raw.end());
		poly = std::move(raw);
		const size_t n = poly.size();
		outN.resize(n);
		arc.resize(n);
		perim = 0.0;
		for (size_t i = 0; i < n; ++i) {
			const V2 tg = poly[(i + 1) % n] - poly[(i + n - 1) % n];
			outN[i] = Unit2(V2(tg.y, -tg.x), V2(1, 0));                    
			arc[i] = perim;
			perim += m::la::length(poly[(i + 1) % n] - poly[i]);
		}
		return perim > 0.0;
	}
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
		nrm = Unit2(outN[i] * (1.0 - f) + outN[j] * f, outN[i]);
	}
};

double SignedDist(const std::vector<V2>& poly, const V2& q)
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

                                                                         
                                 
double RayExit(const std::vector<V2>& poly, const V2& q, const V2& d, double sMin)
{
	double best = 1e300;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		const V2 a = poly[j], e = poly[i] - poly[j];
		const double den = Cross2(d, e);
		if (std::abs(den) < 1e-12) continue;
		const V2 aq = a - q;
		const double s = Cross2(aq, e) / den, u = Cross2(aq, d) / den;
		if (s > sMin && u >= 0.0 && u <= 1.0 && s < best) best = s;
	}
	return best;
}

struct Seed { V2 q, dir; };
struct Spike { V2 a, b, tip; };

                                                
struct Opening {
	Frame f;
	V2 c;                                    
	double R = 1.0;                           
	bool hole = true;
	std::vector<V2> outline;                                     
	std::vector<Seed> seeds;                                                    
	std::vector<Spike> spikes;
	double reach = 1.0;
	bool fromRim = false;                                                  
};

                                                      
void KinkedEdge(std::vector<V2>& out, const V2& P, const V2& Q, double kink, double R, Stream& rs)
{
	const double len = m::la::length(Q - P);
	const int k = std::max(1, std::min(4, int(len / std::max(1e-9, 0.3 * R)) + 1));
	const V2 pn = Unit2(Perp(Q - P), V2(0, 1));
	for (int i = 1; i < k; ++i) {
		const double u = (i + 0.3 * rs.Sym()) / k;
		out.push_back(P + (Q - P) * u + pn * (kink * len / k * rs.Sym()));
	}
	out.push_back(Q);
}

                                                                             
                                             
void Shatter(const Loop& base, const BrokenGlassParams& p, int nSec, double inR, Opening& op, Stream& rs)
{
	const double R = op.R;
	const double off = rs();
	const size_t nS = static_cast<size_t>(nSec);
	std::vector<double> a(nS), blow(nS, 0.0), push(nS);
	for (int i = 0; i < nSec; ++i)
		a[size_t(i)] = base.perim * (i + off + 0.45 * ClampD(p.radialJitter, 0.0, 1.0) * rs.Sym()) / nSec;
	for (int i = 0; i < nSec; ++i)
		if (rs() < ClampD(p.blowout, 0.0, 1.0)) blow[size_t(i)] = std::max(0.0, p.blowoutReach) * R * rs.Range(0.4, 1.0);
	std::vector<V2> notch(nS), notchN(nS);
	for (int i = 0; i < nSec; ++i) {
		V2 q, on;
		base.At(a[size_t(i)], q, on);
		push[size_t(i)] = std::max(0.0, p.notchJitter) * R * std::pow(rs(), 1.5)
			+ std::max(blow[size_t(i)], blow[size_t((i + nSec - 1) % nSec)]);
		notch[size_t(i)] = q + on * push[size_t(i)];
		notchN[size_t(i)] = Unit2(on + Unit2(q - op.c, on) * 0.6, on);
	}
	const double kink = std::max(0.0, p.edgeKink);
	op.outline.clear();
	for (int i = 0; i < nSec; ++i) {
		const int j = (i + 1) % nSec;
		const double aA = a[size_t(i)];
		const double aB = a[size_t(j)] + (j == 0 ? base.perim : 0.0);
		op.outline.push_back(notch[size_t(i)]);
		op.seeds.push_back({notch[size_t(i)], notchN[size_t(i)]});
		if (blow[size_t(i)] <= 0.0 && rs() < ClampD(p.spikeChance, 0.0, 1.0)) {
			                                                                          
			std::vector<double> ba{aA};
			std::vector<V2> bp{notch[size_t(i)]};
			if (rs() < ClampD(p.spikeSplit, 0.0, 1.0)) {
				const double am = Lerp(aA, aB, rs.Range(0.4, 0.6));
				V2 q, on;
				base.At(am, q, on);
				ba.push_back(am);
				bp.push_back(q + on * (0.5 * std::max(0.0, p.notchJitter) * R * rs()));
			}
			ba.push_back(aB);
			bp.push_back(notch[size_t(j)]);
			for (size_t k = 0; k + 1 < bp.size(); ++k) {
				if (k > 0) {
					V2 q, on;
					base.At(ba[k], q, on);
					op.seeds.push_back({bp[k], Unit2(on + Unit2(q - op.c, on) * 0.6, on)});
				}
				                                                                 
				                                                              
				const double span = ba[k + 1] - ba[k];
				const double am = Lerp(ba[k], ba[k + 1], 0.5 + 0.3 * ClampD(p.spikeLean, 0.0, 1.0) * rs.Sym());
				const double hw = 0.5 * ClampD(p.spikeWidth, 0.05, 0.95) * span * rs.Range(0.45, 1.3);
				const double aL = std::max(ba[k] + 0.04 * span, am - hw), aR = std::min(ba[k + 1] - 0.04 * span, am + hw);
				V2 qL, onL, qR, onR, qm, onm;
				base.At(aL, qL, onL);
				base.At(aR, qR, onR);
				base.At(0.5 * (aL + aR), qm, onm);
				qL = qL + onL * (0.08 * R * rs());
				qR = qR + onR * (0.08 * R * rs());
				const V2 in = Unit2(-onm + Unit2(op.c - qm, -onm) * 0.5, -onm);
				const double lMax = std::min(0.88 * m::la::length(qm - op.c), 0.9 * inR + 0.3 * R);
				                                                        
				const double L = std::min(std::max(0.0, p.spikeLength) * R * Lerp(0.15, 1.0, std::pow(rs(), 1.4)), lMax);
				const double bw = m::la::length(qR - qL);
				KinkedEdge(op.outline, bp[k], qL, kink, R, rs);
				if (L < 0.6 * bw) {
					KinkedEdge(op.outline, qR, bp[k + 1], kink, R, rs);
					continue;
				}
				                                                                     
				const V2 tip = qm + Unit2(in + Perp(in) * (0.45 * rs.Sym()), in) * L;
				op.spikes.push_back({qL, qR, tip});
				                                                          
				                                        
				auto side = [&](const V2& from, const V2& to, double sgn) {
					if (rs() < 0.35 && L > 1.5 * bw) {
						const double t = rs.Range(0.3, 0.6);
						const V2 e = to - from;
						const V2 out = Unit2(Perp(e) * sgn, Perp(e));
						const V2 sh = from + e * t;
						KinkedEdge(op.outline, from, sh + out * (rs.Range(0.15, 0.35) * bw), 0.5 * kink, R, rs);
						op.outline.push_back(sh + e * 0.04 - out * (0.05 * bw));
						KinkedEdge(op.outline, sh + e * 0.04 - out * (0.05 * bw), to, 0.5 * kink, R, rs);
					} else {
						KinkedEdge(op.outline, from, to, 0.5 * kink, R, rs);
					}
				};
				side(qL, tip, 1.0);
				side(tip, qR, 1.0);
				KinkedEdge(op.outline, qR, bp[k + 1], kink, R, rs);
			}
			op.outline.pop_back();                                          
		} else {
			                                                                 
			const int k = 2;
			for (int s = 1; s < k; ++s) {
				const double u = Lerp(0.3, 0.7, rs());
				V2 q, on;
				base.At(Lerp(aA, aB, u), q, on);
				const double pu = Lerp(push[size_t(i)], push[size_t(j)], u) + blow[size_t(i)] * std::sin(m::kPi * u);
				op.outline.push_back(q + on * pu + Perp(on) * (kink * R * 0.1 * rs.Sym()));
			}
		}
	}
}

                                                          
void ProceduralOpening(const BrokenGlassParams& p, Opening& op, Stream& rs)
{
	const int nSec = std::max(3, int(std::lround(rs.Range(double(p.radialMin), double(p.radialMax) + 0.999) - 0.5)));
	if (!op.hole) {
		const double crush = std::max(1e-4, p.crushSize) * op.R;
		const double off = rs();
		for (int i = 0; i < nSec; ++i) {
			const double ang = 2.0 * m::kPi * (i + off + 0.45 * ClampD(p.radialJitter, 0.0, 1.0) * rs.Sym()) / nSec;
			const V2 d(std::cos(ang), std::sin(ang));
			op.seeds.push_back({op.c + d * crush, d});
		}
		return;
	}
	const V noise(rs() * 500.0, rs() * 500.0, rs() * 500.0);
	                                                                            
	                                                  
	const double lobes = std::max(0.0, p.holeLobes);
	const double stretch = 1.0 + std::min(1.0, lobes) * rs.Range(0.0, 0.8);
	const double rot = rs() * m::kPi;
	std::vector<V2> raw;
	const int nb = 96;
	for (int i = 0; i < nb; ++i) {
		const double ang = 2.0 * m::kPi * i / nb;
		const V2 d(std::cos(ang), std::sin(ang));
		const double r = op.R * std::max(0.35, 1.0 + lobes * Fbm(V(d.x * 1.1, d.y * 1.1, 0.0) + noise, 2));
		const V2 e = Rot2(V2(d.x * std::sqrt(stretch), d.y / std::sqrt(stretch)), rot);
		raw.push_back(op.c + e * r);
	}
	Loop base;
	if (!base.Build(raw)) { op.hole = false; return; }
	Shatter(base, p, nSec, op.R, op, rs);
}

                                                                             
                                                                         
                                                                               
                                                    
void RemoveIslands(const Pane& s, const Frame& f, double reach, m::CrossSection& hole)
{
	const m::Rect bb = hole.Bounds();
	const V2 pad = (bb.max - bb.min) * 0.15 + V2(4.0 * s.layer);
	const V2 lo = bb.min - pad, hi = bb.max + pad;
	const double band = 0.02 * std::max(hi.x - lo.x, hi.y - lo.y);
	std::vector<m::CrossSection> tris;
	const MeshView& v = *s.v;
	for (size_t i = 0; i < s.tris.size(); ++i) {
		if (m::la::dot(s.tn[i], f.n) < 0.3) continue;
		if (std::abs(m::la::dot(s.tc[i] - f.o, f.n)) > reach + 3.0 * s.layer) continue;
		const Tri& t = s.tris[i];
		V2 q[3] = {f.Local(v.pts[t.a]), f.Local(v.pts[t.b]), f.Local(v.pts[t.c])};
		const V2 tl = m::la::min(q[0], m::la::min(q[1], q[2])), th = m::la::max(q[0], m::la::max(q[1], q[2]));
		if (th.x < lo.x || th.y < lo.y || tl.x > hi.x || tl.y > hi.y) continue;
		if (Cross2(q[1] - q[0], q[2] - q[0]) <= 0.0) continue;
		tris.push_back(m::CrossSection(m::Polygons{{q[0], q[1], q[2]}}));
		if (tris.size() > 40000) return;                                                     
	}
	if (tris.empty()) return;
	const m::CrossSection window(m::Polygons{{lo, V2(hi.x, lo.y), hi, V2(lo.x, hi.y)}});
	const m::CrossSection inner(m::Polygons{{lo + V2(band), V2(hi.x - band, lo.y + band), hi - V2(band), V2(lo.x + band, hi.y - band)}});
	const m::CrossSection ring = window - inner;
	const m::CrossSection pane = m::CrossSection::BatchBoolean(tris, m::OpType::Add) ^ window;
	const m::CrossSection rest = pane - hole;
	                                                                         
	                                                                       
	                   
	const double grow = std::max(2.0 * s.layer, 0.01 * std::max(hi.x - lo.x, hi.y - lo.y));
	for (const m::CrossSection& piece : rest.Decompose())
		if ((piece ^ ring).Area() <= 1e-9 * (hi.x - lo.x) * (hi.y - lo.y))
			hole = hole + (piece.Offset(grow, m::CrossSection::JoinType::Miter) - (rest - piece));
}

                                                                                          

enum class Kind { Auto, DrawnHole, DrawnPoint, DrawnLine };
struct ImpactSpec {
	Kind kind = Kind::Auto;
	uint64_t id = 0;
	V pos, n;                                                          
	double R = 1.0;
	bool hole = true;
	int src = -1;                                           
};

struct ImpactOut {
	m::Manifold hole, cracks, chips;
};

                                                                            
                               
std::vector<V> CleanPoints(const SurfacePaintRegion& r, bool closed)
{
	std::vector<V> pts;
	for (const EVec& q : r.points)
		if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)
			&& (pts.empty() || m::la::length(q - pts.back()) > 1e-9))
			pts.push_back(q);
	if (closed && pts.size() > 3 && m::la::length(pts.front() - pts.back()) <= 1e-9) pts.pop_back();
	return pts;
}

}             

void PlaceBrokenGlassLegacy(const CoreInput& in, const BrokenGlassParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& chips, const BrokenGlassPaint* paint)
{
	if (!pIn.enable) return;
	BrokenGlassParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.cutMargin = ClampD(p.cutMargin, 0.05, 4.0);
	p.impactAmount = std::max(0, std::min(p.impactAmount, 200));
	p.radialMin = std::max(3, std::min(p.radialMin, 64));
	p.radialMax = std::max(p.radialMin, std::min(p.radialMax, 64));
	p.ringCount = std::max(0, std::min(p.ringCount, 16));
	p.maxCracks = std::max(0, std::min(p.maxCracks, 100000));
	p.crackWidth = std::max(1e-5, p.crackWidth);
	p.paintShardWidth = std::max(1e-3, p.paintShardWidth);
	p.ringGrowth = std::max(1.05, p.ringGrowth);
	if (p.holeSizeMax < p.holeSizeMin) std::swap(p.holeSizeMin, p.holeSizeMax);

	const bool drawnHoles = paint && paint->holes && !paint->holes->empty() && p.paintHolesEnable;
	const bool drawnImpacts = paint && paint->impacts && !paint->impacts->empty() && p.paintImpactsEnable;
	const bool autoOn = !(p.paintOnly && (drawnHoles || drawnImpacts))
		&& p.impactEnable && p.impactAmount > 0 && p.holeSizeMax > 0.0;
	if (!autoOn && !drawnHoles && !drawnImpacts) return;

	Pane s;
	if (!BuildPane(in, p, s)) return;
	const MeshView& v = in.view;
	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) ^ 0x474c415353425231ULL)                
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x474c;                             
	const double paintReach = paint ? std::max(1e-3, paint->reach) : 5.0;

	std::vector<ImpactSpec> specs;

	                                                                                  
	if (autoOn) {
		std::vector<double> cdf(s.tris.size());
		double area = 0.0;
		for (size_t i = 0; i < s.tris.size(); ++i) {
			const bool skip = s.edgeFace[i] || s.Avoided(s.tris[i].poly);
			area += skip ? 0.0 : s.area[i];
			cdf[i] = area;
		}
		const uint64_t is = seed ^ 0x494d5041ULL;            
		std::vector<std::pair<V, double>> placed;
		int accepted = 0;
		for (uint64_t k = 0; k < uint64_t(p.impactAmount) * 32 && area > 0.0 && accepted < p.impactAmount; ++k) {
			auto rnd = [&](uint64_t ch) { return Random01(is, k, ch); };
			const size_t ti = size_t(std::upper_bound(cdf.begin(), cdf.end(), rnd(0) * area) - cdf.begin());
			if (ti >= s.tris.size()) continue;
			const Tri& t = s.tris[ti];
			double r1 = rnd(1), r2 = rnd(2);
			if (r1 + r2 > 1.0) { r1 = 1.0 - r1; r2 = 1.0 - r2; }
			const V pos = v.pts[t.a] * (1.0 - r1 - r2) + v.pts[t.b] * r1 + v.pts[t.c] * r2;
			if (mask && !mask->Keep(pos, int32_t(k), kStream)) continue;
			const double sc = mask ? mask->SizeScale(pos) : 1.0;
			if (sc <= 0.0) continue;
			ImpactSpec sp;
			sp.kind = Kind::Auto;
			sp.id = k;
			sp.pos = pos;
			sp.n = s.tn[ti];
			sp.R = Lerp(p.holeSizeMin, p.holeSizeMax, rnd(3)) * sc;
			sp.hole = rnd(4) < ClampD(p.holeChance, 0.0, 1.0);
			if (!(sp.R > 0.0)) continue;
			bool clash = false;
			for (const auto& q : placed)
				if (m::la::length(q.first - pos) < std::max(0.0, p.impactSpacing) * (q.second + sp.R)) { clash = true; break; }
			if (clash) continue;
			placed.push_back({pos, sp.R});
			specs.push_back(sp);
			++accepted;
		}
	}
	if (drawnHoles)
		for (size_t i = 0; i < paint->holes->size(); ++i) {
			ImpactSpec sp;
			sp.kind = Kind::DrawnHole;
			sp.id = uint64_t(i) | (uint64_t(1) << 40);
			sp.src = int(i);
			specs.push_back(sp);
		}
	if (drawnImpacts)
		for (size_t i = 0; i < paint->impacts->size(); ++i) {
			const SurfacePaintRegion& r = (*paint->impacts)[i];
			const std::vector<V> pts = CleanPoints(r, false);
			if (pts.empty()) continue;
			V lo(1e300), hi(-1e300), c(0.0);
			for (const V& q : pts) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); c = c + q; }
			c = c / double(pts.size());
			ImpactSpec sp;
			sp.id = uint64_t(i) | (uint64_t(2) << 40);
			sp.src = int(i);
			sp.n = Unit(V(r.normal), V(0, 0, 1));
			if (pts.size() < 2 || m::la::length(hi - lo) < std::max(0.0, p.paintPointSize)) {
				sp.kind = Kind::DrawnPoint;
				sp.pos = c;
				sp.R = std::max(1e-3, p.paintImpactSize);
				sp.hole = p.paintPointHole;
			} else {
				sp.kind = Kind::DrawnLine;
			}
			specs.push_back(sp);
		}
	if (specs.empty()) return;

	const int crackBudget = std::max(1, p.maxCracks / int(specs.size()));
	std::vector<ImpactOut> outs(specs.size());

	ProjectDamageParallelFor(0, int64_t(specs.size()), [&](int64_t si) {
		const ImpactSpec& sp = specs[size_t(si)];
		ImpactOut& out = outs[size_t(si)];
		const uint64_t isd = seed ^ ((uint64_t(sp.kind) + 1) * 0xa0761d6478bd642fULL) ^ (sp.id * 0xe7037ed1a0b428dbULL);
		Stream rs(isd, 1), rc(isd, 2), rk(isd, 3);
		Opening op;
		std::vector<Line> lines;                                              

		if (sp.kind == Kind::Auto || sp.kind == Kind::DrawnPoint) {
			V pos = sp.pos, n = sp.n;
			if (sp.kind == Kind::DrawnPoint) {
				                                         
				SpanHit h;
				if (!s.Span(pos, n, paintReach + s.layer + s.margin, h, false)) return;
				pos = pos + n * h.top;
			}
			op.f = MakeFrame(pos, n, rs() * 2.0 * m::kPi);
			op.c = V2(0.0);
			op.R = sp.R;
			op.hole = sp.hole;
			op.reach = std::max(3.0 * s.layer, 0.25 * op.R);
			ProceduralOpening(p, op, rs);
		} else if (sp.kind == Kind::DrawnHole) {
			const SurfacePaintRegion& src = (*paint->holes)[size_t(sp.src)];
			const std::vector<V> pts = CleanPoints(src, true);
			if (pts.size() < 3) return;
			V o(0.0);
			for (const V& q : pts) o = o + q;
			o = o / double(pts.size());
			op.f = MakeFrame(o, V(src.normal), 0.0);
			op.reach = paintReach;
			op.fromRim = true;
			std::vector<V2> raw;
			for (const V& q : pts) raw.push_back(op.f.Local(q));
			Loop contour;
			if (!contour.Build(raw)) return;
			                                                                               
			V2 lo(1e300), hi(-1e300);
			for (const V2& q : contour.poly) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
			double inR = 0.0;
			V2 c = (lo + hi) * 0.5;
			for (int i = 0; i < 40; ++i)
				for (int j = 0; j < 40; ++j) {
					const V2 q(lo.x + (hi.x - lo.x) * (i + 0.5) / 40, lo.y + (hi.y - lo.y) * (j + 0.5) / 40);
					const double d = SignedDist(contour.poly, q);
					if (d > inR) { inR = d; c = q; }
				}
			if (inR <= 0.0) return;
			                                            
			std::vector<V2> res;
			const int nr = std::max(24, std::min(4000, int(std::ceil(contour.perim / std::max(1e-6, 0.1 * p.paintShardWidth)))));
			for (int i = 0; i < nr; ++i) { V2 q, on; contour.At(contour.perim * i / nr, q, on); res.push_back(q); }
			Loop base;
			if (!base.Build(res)) return;
			op.c = c;
			op.R = inR;
			op.hole = true;
			const int nSec = std::max(3, std::min(400, int(std::lround(base.perim / p.paintShardWidth))));
			Shatter(base, p, nSec, inR, op, rs);
			if (!p.paintHoleCracks) op.seeds.clear();
		} else {
			                                                   
			const SurfacePaintRegion& src = (*paint->impacts)[size_t(sp.src)];
			const std::vector<V> pts = CleanPoints(src, false);
			V o(0.0);
			for (const V& q : pts) o = o + q;
			o = o / double(pts.size());
			op.f = MakeFrame(o, V(src.normal), 0.0);
			op.reach = paintReach;
			op.hole = false;
			Line l;
			for (const V& q : pts) { l.pts.push_back(op.f.Local(q)); l.w.push_back(p.crackWidth); }
			const double len = LineLength(l);
			op.R = std::max(p.paintShardWidth, 0.1 * len);
			                                                            
			Line k;
			for (size_t i = 0; i + 1 < l.pts.size(); ++i) {
				const double sl = m::la::length(l.pts[i + 1] - l.pts[i]);
				const int nk = std::max(1, int(std::ceil(sl / std::max(1e-6, 0.5 * p.paintShardWidth))));
				const V2 pn = Unit2(Perp(l.pts[i + 1] - l.pts[i]), V2(0, 1));
				for (int j = 0; j < nk; ++j) {
					const double u = double(j) / nk;
					const double jit = (i == 0 && j == 0) ? 0.0 : std::max(0.0, p.crackMeander) * 0.25 * p.paintShardWidth * rc.Sym();
					k.pts.push_back(l.pts[i] + (l.pts[i + 1] - l.pts[i]) * u + pn * jit);
					k.w.push_back(p.crackWidth);
				}
			}
			k.pts.push_back(l.pts.back());
			k.w.push_back(p.crackWidth * ClampD(p.crackTaper, 0.05, 1.0));
			lines.push_back(k);
			const int forks = int(std::floor(len / (2.0 * p.paintShardWidth) * ClampD(p.crackBranch, 0.0, 1.0) + rc()));
			for (int b = 0; b < forks; ++b) {
				bool ok = false;
				const double at = len * rc.Range(0.1, 0.9);
				const V2 q = PointAlong(k, at, ok);
				bool ok2 = false;
				const V2 q2 = PointAlong(k, std::min(len, at + 0.05 * len), ok2);
				const V2 tg = Unit2(q2 - q, V2(1, 0));
				const V2 d = Rot2(tg, (rc() < 0.5 ? -1.0 : 1.0) * rc.Range(0.35, 0.7));
				const double bl = std::min(3.0 * p.paintShardWidth, len * rc.Range(0.1, 0.3));
				lines.push_back(RadialLine(q, d, bl, std::max(2.0 * p.crackWidth, bl / 12.0), p.crackMeander,
					p.crackWidth * 0.8, p.crackTaper, V(rc() * 300.0, rc() * 300.0, 0.0), rc));
			}
		}

		const double R = op.R;
		const double cell = std::max(0.35 * s.layer, R / 24.0) / p.detail;

		                    
		std::vector<V2> outlineClean;
		if (op.hole && op.outline.size() >= 3) {
			m::CrossSection cs(m::Polygons{op.outline}, m::CrossSection::FillRule::Positive);
			if (s.thin && cs.Area() > 1e-9) RemoveIslands(s, op.f, op.reach, cs);
			const m::Polygons polys = cs.ToPolygons();
			if (!polys.empty() && cs.Area() > 1e-9) {
				m::Manifold cut = Drape(s, polys, op.f, 1.0, op.reach, cell, true, [](const V2&) { return 1.0; });
				if (cut.NumTri() > 0) out.hole = std::move(cut);
			}
		}

		                   
		if (p.crackEnable) {
			const double step = std::max({2.0 * p.crackWidth, 0.5 * s.layer, R / 40.0}) / p.detail;
			std::vector<Line> radial(op.seeds.size());
			std::vector<char> hasRadial(op.seeds.size(), 0);
			for (size_t i = 0; i < op.seeds.size(); ++i) {
				const Seed& sd = op.seeds[i];
				const double fromC = op.fromRim ? 0.0 : m::la::length(sd.q - op.c);
				const double base = op.fromRim ? (std::max(0.0, p.crackReach - 1.0) * R) : (p.crackReach * R - fromC);
				const double len = base * (1.0 + ClampD(p.crackReachJitter, 0.0, 1.0) * rc.Sym());
				if (len < 0.1 * R) continue;
				const V2 start = sd.q - sd.dir * (op.hole ? 2.0 * p.crackWidth + 0.02 * R : 0.0);
				radial[i] = RadialLine(start, sd.dir, len, step, p.crackMeander, p.crackWidth, p.crackTaper,
					V(rc() * 300.0, rc() * 300.0, 0.0), rc);
				hasRadial[i] = 1;
				lines.push_back(radial[i]);
				                                                                 
				if (rc() < ClampD(p.crackBranch, 0.0, 1.0)) {
					bool ok = false;
					const double at = len * rc.Range(0.25, 0.7);
					const V2 q = PointAlong(radial[i], at, ok);
					if (ok) {
						const V2 d = Rot2(sd.dir, (rc() < 0.5 ? -1.0 : 1.0) * rc.Range(0.25, 0.6));
						const double bl = (len - at) * rc.Range(0.4, 0.85);
						if (bl > 0.05 * R)
							lines.push_back(RadialLine(q, d, bl, step, p.crackMeander, 0.8 * p.crackWidth, p.crackTaper,
								V(rc() * 300.0, rc() * 300.0, 0.0), rc));
					}
				}
			}
			                                                                
			const size_t ns = op.seeds.size();
			for (int k = 0; k < p.ringCount && ns >= 2; ++k) {
				const double d = std::max(0.0, p.ringFirst) * R * std::pow(p.ringGrowth, k);
				for (size_t i = 0; i < ns; ++i) {
					const size_t j = (i + 1) % ns;
					if (rk() >= ClampD(p.ringFill, 0.0, 1.0)) continue;
					if (!hasRadial[i] || !hasRadial[j]) continue;
					const double jit = ClampD(p.ringJitter, 0.0, 1.0);
					const double dA = d * (1.0 + jit * rk.Sym()) + (op.hole ? 2.0 * p.crackWidth : 0.0);
					const double dB = d * (1.0 + jit * rk.Sym()) + (op.hole ? 2.0 * p.crackWidth : 0.0);
					bool okA = false, okB = false;
					const V2 A = PointAlong(radial[i], dA, okA), B = PointAlong(radial[j], dB, okB);
					if (!okA || !okB) continue;
					const double span = m::la::length(B - A);
					if (span < 4.0 * p.crackWidth || span > 2.5 * (d + R)) continue;
					const V2 mid = (A + B) * 0.5;
					const V2 toC = Unit2(op.c - mid, V2(0, 0));
					Line l;
					const int nseg = std::max(2, std::min(8, int(span / std::max(1e-9, 0.15 * R)) + 2));
					for (int q = 0; q <= nseg; ++q) {
						const double u = double(q) / nseg;
						const double bow = std::max(0.0, p.ringBow) * span * 4.0 * u * (1.0 - u);
						const double kink = (q == 0 || q == nseg) ? 0.0 : 0.04 * span / nseg * rk.Sym();
						l.pts.push_back(A + (B - A) * u + toC * bow + Unit2(Perp(B - A), V2(0, 1)) * kink);
						l.w.push_back(0.85 * p.crackWidth);
					}
					lines.push_back(l);
				}
			}
			                              
			for (const Spike& sk : op.spikes) {
				if (rk() >= ClampD(p.shardCrack, 0.0, 1.0)) continue;
				const double f = rk.Range(0.2, 0.65);
				const V2 A = sk.a + (sk.tip - sk.a) * (f * (1.0 + 0.2 * rk.Sym()));
				const V2 B = sk.b + (sk.tip - sk.b) * (f * (1.0 + 0.2 * rk.Sym()));
				const V2 d = Unit2(B - A, V2(1, 0));
				const double ext = 0.15 * m::la::length(B - A) + 2.0 * p.crackWidth;
				Line l;
				l.pts = {A - d * ext, (A + B) * 0.5 + Perp(d) * (0.05 * m::la::length(B - A) * rk.Sym()), B + d * ext};
				l.w = {0.8 * p.crackWidth, 0.8 * p.crackWidth, 0.8 * p.crackWidth};
				lines.push_back(l);
			}
			std::vector<m::Manifold> grooves;
			const double front = p.crackBothSides && s.thin ? 0.5 * p.crackDepth : p.crackDepth;
			for (const Line& l : lines) {
				if (int(grooves.size()) >= crackBudget) break;
				Groove(s, op.f, l, 1.0, front, op.reach, step, grooves);
				if (p.crackBothSides && s.thin) Groove(s, op.f, l, -1.0, 0.5 * p.crackDepth, op.reach, step, grooves);
			}
			if (!grooves.empty()) {
				m::Manifold merged = m::Manifold::BatchBoolean(grooves, m::OpType::Add);
				if (merged.Status() == m::Manifold::Error::NoError && merged.NumTri() > 0) out.cracks = merged;
			}
		}

		                                              
		if (p.chipEnable && op.hole && s.thin && op.outline.size() >= 3) {
			Loop rim;
			if (rim.Build(op.outline)) {
				std::vector<m::Manifold> scoops;
				const double sp = std::max(0.05, p.chipSpacing);
				double lastEnd = -1e300;
				for (double a = sp * rk(); a < rim.perim; a += sp * (0.5 + rk())) {
					if (rk() >= ClampD(p.chipChance, 0.0, 1.0)) continue;
					V2 q, on;
					rim.At(a, q, on);
					const double la = 0.5 * std::max(1e-3, p.chipSize) * rk.Range(0.5, 1.0);
					const double lb = la * rk.Range(0.4, 0.8);
					                                                                
					                                                        
					if (a - la < lastEnd) continue;
					if (RayExit(rim.poly, q, on, 1e-4 * rim.perim) < 3.0 * lb) continue;
					{
						const V2 tg0 = Perp(on);
						if (RayExit(rim.poly, q + tg0 * la, on, 1e-4 * rim.perim) < 2.0 * lb) continue;
						if (RayExit(rim.poly, q - tg0 * la, on, 1e-4 * rim.perim) < 2.0 * lb) continue;
					}
					lastEnd = a + la;
					const V2 tg = Perp(on);
					std::vector<V2> ell;
					for (int k = 0; k < 14; ++k) {
						const double ang = 2.0 * m::kPi * k / 14;
						const double wob = 1.0 + 0.15 * rk.Sym();
						ell.push_back(q + tg * (la * std::cos(ang) * wob) + on * (lb * std::sin(ang) * wob));
					}
					const double depth = ClampD(p.chipDepth, 0.02, 0.9) * rk.Range(0.6, 1.0);
					auto depthOf = [&](const V2& x) {
						const double out = ClampD(m::la::dot(x - q, on) / lb, 0.0, 1.0);
						return depth * Lerp(1.0, 0.1, out);
					};
					m::Manifold c = Drape(s, m::Polygons{ell}, op.f, -1.0, op.reach,
						std::max(0.25 * la, 0.5 * s.layer), false, depthOf);
					if (c.NumTri() > 0) scoops.push_back(std::move(c));
				}
				if (!scoops.empty()) {
					m::Manifold merged = m::Manifold::BatchBoolean(scoops, m::OpType::Add);
					if (merged.Status() == m::Manifold::Error::NoError && merged.NumTri() > 0) out.chips = merged;
				}
			}
		}
	});

	for (ImpactOut& o : outs) {
		if (o.hole.NumTri() > 0) holes.push_back(std::move(o.hole));
		if (o.cracks.NumTri() > 0) cracks.push_back(std::move(o.cracks));
		if (o.chips.NumTri() > 0) chips.push_back(std::move(o.chips));
	}
}

PROJECT_DAMAGE_NS_MACRO_END
