// вырезаем грунтовый разлом и строим его края и наклонённые плиты
// ProjectDamage ground fissures — GPL-3.0-or-later; see LICENSE.
//
// Recipe: ground-fissure-v1 (Classic) and ground-fissure-v2 (Realistic, 2026-11-07: faceted
// fracture faces, chips, crumbled rim, ledged walls, sheared rubble, debris; wedge and
// triangle slabs: neighbouring cross cracks lean opposite ways, and corner breaks and
// diagonal cracks split slabs into folded triangular pieces). Huge earthquake fissures along drawn strokes:
//   1. track: each stroke is dropped onto the ground along -up and turned
//      into a resampled centre line in the ground plane, with arc length s
//      and a lateral offset d (d > 0 is the right-hand side, looking along
//      the stroke). Every solid below is a grid over (s, d) mapped through
//      that track, so all pieces share one coordinate system and fit each
//      other exactly. The offset normals come from a strongly smoothed
//      tangent, and each side has a fold-free offset limit, so the mapping
//      never folds on the inside of a bend;
//   2. chasm: a closed height-field box over (s, d). Its top floats above
//      the ground, its bottom is the ground minus the depth: sheer walls down
//      to ~55-85 % of the depth, then a flat or V floor. The rim is an
//      angular, two-scale value noise per side (broken, straight-ish chunks
//      like the reference), the walls lean in or out (undercuts), the walls
//      and floor are rough, and the ends taper to a narrow crack. Depth is
//      capped by the local ground thickness so the fissure never breaks
//      through;
//   3. slabs: 1-3 rows per side of ground slabs broken off along the rim.
//      Rows have their own width along s; slabs have random lengths and
//      slanted, jagged cross cracks. For each slab a socket is cut
//      (thickness minus an embed) and the source mesh's own piece (in.box
//      intersected with the slab prism) is moved rigidly: tilted about its
//      back (hinge) edge so the rim side lifts (or drops into the chasm), plus
//      a small roll. The piece keeps an embedded strip under its hinge and
//      its back face swings into intact ground, so the buildup union welds
//      it to the same mesh. The rim-side top edge is bevelled (crumbled);
//   4. branches: tapering V-groove cracks walked out from the chasm into the
//      ground, meandering, sometimes forking;
//   5. rubble: convex chunks jammed between the walls or lying on the floor,
//      sized to the local width so they always touch rock.
// Randomness uses SplitMix64 counter channels keyed by (stroke, side, row,
// slab / branch / chunk index, channel), like the other modules.
//
// Channel map, per stroke key: 0-7 noise offsets, 8 width, 9 depth. Slab
// rows: key SlabKey(stroke, side, row, 0xffff): 0 width, 1 phase. Slab k:
// SlabKey(.., k): 0 length, 1 thickness, 2 flat, 3 sink, 4 tilt, 5 roll,
// 6 bevel; boundary k: BoundKey(..): 0 skew. Branch k: 0 side, 1 position,
// 2 angle, 3 length, 4 width, 5 depth, 6-14 walk/fork. Rubble k: 0 position,
// 1 kind, 2 size, 3 depth, 4 offset, 10+ hull points.
#include "manifold/manifold.h"
#include "ground_fissure.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
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

// 1D value noise with LINEAR interpolation in [-1, 1]: straight runs with
// sharp kinks, the broken-rock look of the reference rims.
double Angular(uint64_t seed, uint64_t key, double t)
{
	const double f = std::floor(t);
	const int64_t i = int64_t(f);
	const double a = t - f;
	const double v0 = Random01(seed, key, uint64_t(i + (int64_t(1) << 40)));
	const double v1 = Random01(seed, key, uint64_t(i + 1 + (int64_t(1) << 40)));
	return 2.0 * Lerp(v0, v1, a) - 1.0;
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

// Orient the solid outward by volume (same fix-up as heat damage / fabric tears).
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

// Closed grid box: rows i (0..ns) by columns j (0..nw); top and bottom
// sheets plus side walls. Valid whenever bottom lies below top along one
// common direction, which is how every caller builds it.
m::Manifold SlabSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(size_t(ns + 1) * size_t(nw + 1) * 6);
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
	return Orient(mesh);
}

// Like SlabSolid, but the side walls run through nl - 1 extra rings between
// the top and bottom sheets. mid(i, j, l) gives the ring vertex at boundary
// node (i, j) for level l (1..nl-1, top to bottom). Lets a fracture face get
// relief over its height (ledges, undercuts, a crumbled top edge) while the
// top and bottom sheets stay plain grids. nl = 1 is SlabSolid.
m::Manifold RingSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw, int nl,
	const std::function<V(int, int, int)>& mid)
{
	nl = std::max(1, nl);
	std::vector<std::pair<int, int>> ring;
	ring.reserve(size_t(2 * (ns + nw)));
	for (int i = 0; i < ns; ++i) ring.push_back({i, 0});
	for (int j = 0; j < nw; ++j) ring.push_back({ns, j});
	for (int i = ns; i > 0; --i) ring.push_back({i, nw});
	for (int j = nw; j > 0; --j) ring.push_back({0, j});
	const size_t R = ring.size();
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
	const uint32_t base = uint32_t(2 * (ns + 1) * (nw + 1));
	for (int l = 1; l < nl; ++l)
		for (size_t r = 0; r < R; ++r) push(mid(ring[r].first, ring[r].second, l));
	auto B = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j)); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j) + 1); };
	auto lev = [&](int l, size_t r) {
		if (l == 0) return T(ring[r].first, ring[r].second);
		if (l == nl) return B(ring[r].first, ring[r].second);
		return base + uint32_t(size_t(l - 1) * R + r);
	};
	auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
	};
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			quad(T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			quad(B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
		}
	for (int l = 0; l < nl; ++l)
		for (size_t r = 0; r < R; ++r) {
			const size_t q = (r + 1) % R;
			quad(lev(l + 1, r), lev(l + 1, q), lev(l, q), lev(l, r));
		}
	return Orient(mesh);
}

// A bite out of an edge: a faceted convex chunk centred on the edge point c.
// along runs with the edge, in points into the solid, up is the surface
// normal. It reaches len along the edge, dep into the solid (deepest at the
// top, like a conchoidal spall), hgt down the face, and pokes out past the
// edge so the cut is clean.
m::Manifold ChipHull(uint64_t seed, uint64_t key, const V& c, const V& along, const V& in, const V& up,
	double len, double dep, double hgt)
{
	std::vector<V> pts;
	for (int q = 0; q < 14; ++q) {
		const double a = 2.0 * Random01(seed, key, 40 + uint64_t(q) * 3) - 1.0;
		const double b = std::pow(Random01(seed, key, 41 + uint64_t(q) * 3), 0.7);
		const double z = Random01(seed, key, 42 + uint64_t(q) * 3);   // 0 bottom .. 1 top
		const double reach = dep * b * Lerp(0.25, 1.0, z) * (1.0 - 0.45 * std::abs(a));
		pts.push_back(c + along * (0.5 * len * a * (1.0 - 0.35 * b)) + in * reach + up * (hgt * (z - 1.0)));
	}
	// outside corners: over the edge and above the surface
	pts.push_back(c - in * (0.35 * dep) + along * (0.3 * len) + up * (0.4 * hgt));
	pts.push_back(c - in * (0.35 * dep) - along * (0.3 * len) + up * (0.4 * hgt));
	pts.push_back(c - in * (0.35 * dep) - up * (0.7 * hgt));
	pts.push_back(c + in * (0.15 * dep) + up * (0.5 * hgt));
	m::Manifold h = m::Manifold::Hull(pts);
	if (h.Status() != m::Manifold::Error::NoError || !(h.Volume() > 0.0)) return m::Manifold();
	return h;
}

// A broken concrete chunk: a jittered box (sx along a, sy along b, sz along
// c, centred on C) with corners and edges sheared off by 2-5 random
// fracture planes. acrossKeep < 1 keeps the planes from shortening the b
// extent (wedged chunks must still reach both walls).
m::Manifold ShearChunk(uint64_t seed, uint64_t key, m::Manifold h, const V& C, const V& a, const V& b, const V& c,
	double sx, double sy, double sz, double acrossKeep, int maxCuts);

m::Manifold FracturedChunk(uint64_t seed, uint64_t key, const V& C, const V& a, const V& b, const V& c,
	double sx, double sy, double sz, double acrossKeep, int maxCuts)
{
	std::vector<V> pts;
	for (int q = 0; q < 8; ++q) {
		const double fx = (q & 1) ? 0.5 : -0.5, fy = (q & 2) ? 0.5 : -0.5, fz = (q & 4) ? 0.5 : -0.5;
		const double jx = Lerp(0.7, 1.15, Random01(seed, key, 60 + uint64_t(q) * 3));
		const double jy = Lerp(0.8, 1.1, Random01(seed, key, 61 + uint64_t(q) * 3));
		const double jz = Lerp(0.6, 1.2, Random01(seed, key, 62 + uint64_t(q) * 3));
		pts.push_back(C + a * (fx * sx * jx) + b * (fy * sy * jy) + c * (fz * sz * jz));
	}
	return ShearChunk(seed, key, m::Manifold::Hull(pts), C, a, b, c, sx, sy, sz, acrossKeep, maxCuts);
}

m::Manifold ShearChunk(uint64_t seed, uint64_t key, m::Manifold h, const V& C, const V& a, const V& b, const V& c,
	double sx, double sy, double sz, double acrossKeep, int maxCuts)
{
	if (h.Status() != m::Manifold::Error::NoError || !(h.Volume() > 0.0)) return m::Manifold();
	const int cuts = 2 + int(Random01(seed, key, 90) * double(std::max(0, maxCuts - 1)));
	for (int k = 0; k < cuts; ++k) {
		const double ux = 2.0 * Random01(seed, key, 91 + uint64_t(k) * 4) - 1.0;
		const double uy = (2.0 * Random01(seed, key, 92 + uint64_t(k) * 4) - 1.0) * acrossKeep;
		const double uz = 2.0 * Random01(seed, key, 93 + uint64_t(k) * 4) - 1.0;
		const V u = Unit(a * ux + b * uy + c * uz, c);
		const double ext = 0.5 * (std::abs(m::la::dot(u, a)) * sx + std::abs(m::la::dot(u, b)) * sy + std::abs(m::la::dot(u, c)) * sz);
		const double r = Lerp(0.5, 0.85, Random01(seed, key, 94 + uint64_t(k) * 4)) * ext;
		// keep dot(-u, p) >= dot(-u, C) - r: shears off what lies past r along u
		m::Manifold t = h.TrimByPlane(-u, -(m::la::dot(u, C) + r));
		if (t.Status() == m::Manifold::Error::NoError && t.Volume() > 0.35 * h.Volume()) h = t;
	}
	return h;
}

// --- the ground ----------------------------------------------------------------

struct Ground {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	ProjectDamageBVH bvh;
	const std::vector<bool>* avoid = nullptr;
	V up, ex, ey;
	double hTop = 0.0, span = 1.0;

	V At(double x, double y, double h) const { return ex * x + ey * y + up * h; }

	// Drops the column at plane coords (x, y) onto the ground from above.
	// h: height of the first upward-facing hit; thick: ground thickness
	// below it (1e300 for an open bottom); avoided: the hit polygon is in
	// the Avoid Selection.
	bool Drop(double x, double y, double& h, double& thick, bool& avoided) const
	{
		const V org = At(x, y, hTop);
		double t = 0.0;
		int32_t poly = -1;
		avoided = false;
		thick = 1e300;
		if (!bvh.Raycast(org, -up, span, &t, &poly) || poly < 0) return false;
		if (m::la::dot(polyN[size_t(poly)], up) < 0.05) return false;
		h = hTop - t;
		avoided = avoid && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
		const double eps = 1e-6 * span;
		double t2 = 0.0;
		int32_t p2 = -1;
		if (bvh.Raycast(At(x, y, h - eps), -up, span, &t2, &p2) && p2 >= 0) thick = t2 + eps;
		return true;
	}
};

void BuildGround(const CoreInput& in, const GroundFissureParams& p, Ground& g)
{
	const MeshView& v = in.view;
	g.v = &v;
	g.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
		g.polyN[size_t(i)] = Unit(cr, V(0, 0, 1));
	}
	g.bvh.Build(v);
	g.up = Unit(V(p.upX, p.upY, p.upZ), V(0, 0, 1));
	g.ex = AnyPerp(g.up);
	g.ey = m::la::cross(g.up, g.ex);
	double lo = 1e300, hi = -1e300;
	V blo(1e300), bhi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) {
		const double h = m::la::dot(v.pts[i], g.up);
		lo = std::min(lo, h); hi = std::max(hi, h);
		blo = m::la::min(blo, v.pts[i]); bhi = m::la::max(bhi, v.pts[i]);
	}
	const double diag = std::max(1e-6, m::la::length(bhi - blo));
	g.hTop = hi + 0.05 * diag + 1e-3;
	g.span = (g.hTop - lo) + 0.05 * diag + 1e-3;
	g.avoid = p.avoid ? in.avoidPoly : nullptr;
}

// --- the track: centre line in the ground plane, (s, d) mapping -------------------

struct Track {
	std::vector<double> cx, cy, nx, ny, limR, limL;   // per sample
	double step = 1.0, len = 0.0;

	size_t N() const { return cx.size(); }
	void Locate(double s, size_t& i, double& a) const
	{
		const double f = ClampD(s / step, 0.0, double(N() - 1));
		i = std::min(N() - 2, size_t(f));
		a = f - double(i);
	}
	void Map(double s, double d, double& X, double& Y) const
	{
		size_t i; double a;
		Locate(s, i, a);
		double nxx = Lerp(nx[i], nx[i + 1], a), nyy = Lerp(ny[i], ny[i + 1], a);
		const double nl = std::sqrt(nxx * nxx + nyy * nyy);
		if (nl > 1e-12) { nxx /= nl; nyy /= nl; }
		X = Lerp(cx[i], cx[i + 1], a) + d * nxx;
		Y = Lerp(cy[i], cy[i + 1], a) + d * nyy;
	}
	// Fold-free offset limit on one side (side 1 = right, d > 0).
	double Limit(int side, double s) const
	{
		size_t i; double a;
		Locate(s, i, a);
		const std::vector<double>& L = side ? limR : limL;
		return std::min(L[i], L[i + 1]);
	}
	// Tangent direction (plane coords) at s.
	void Tangent(double s, double& tx, double& ty) const
	{
		// the right normal is (ty, -tx), so the tangent is (-ny, nx)
		size_t i; double a;
		Locate(s, i, a);
		const double nxx = Lerp(nx[i], nx[i + 1], a), nyy = Lerp(ny[i], ny[i + 1], a);
		const double nl = std::max(1e-12, std::sqrt(nxx * nxx + nyy * nyy));
		tx = -nyy / nl; ty = nxx / nl;
	}
};

// Builds the track from raw stroke points. Returns false for degenerate strokes.
bool BuildTrack(const Ground& g, const std::vector<V>& pts, double nominalStep, double reach, Track& tr)
{
	std::vector<double> px, py;
	for (const V& q : pts) {
		const double x = m::la::dot(q, g.ex), y = m::la::dot(q, g.ey);
		if (!px.empty() && std::hypot(x - px.back(), y - py.back()) < 1e-9) continue;
		px.push_back(x); py.push_back(y);
	}
	if (px.size() < 2) return false;
	double len = 0.0;
	for (size_t i = 0; i + 1 < px.size(); ++i) len += std::hypot(px[i + 1] - px[i], py[i + 1] - py[i]);
	if (len <= 1e-6) return false;
	// uniform resample
	const double step = std::max(std::min(nominalStep, len / 8.0), len / 4000.0);
	const int n = std::max(2, int(std::round(len / step)) + 1);
	const double st = len / (n - 1);
	std::vector<double> rx(static_cast<size_t>(n)), ry(static_cast<size_t>(n));
	{
		size_t seg = 0;
		double acc = 0.0;
		for (int k = 0; k < n; ++k) {
			const double s = std::min(len, k * st);
			while (seg + 2 < px.size()) {
				const double sl = std::hypot(px[seg + 1] - px[seg], py[seg + 1] - py[seg]);
				if (acc + sl >= s) break;
				acc += sl;
				++seg;
			}
			const double sl = std::max(1e-12, std::hypot(px[seg + 1] - px[seg], py[seg + 1] - py[seg]));
			const double a = ClampD((s - acc) / sl, 0.0, 1.0);
			rx[size_t(k)] = Lerp(px[seg], px[seg + 1], a);
			ry[size_t(k)] = Lerp(py[seg], py[seg + 1], a);
		}
	}
	// light smoothing of the centre line (pen jitter), ends pinned
	for (int pass = 0; pass < 3; ++pass) {
		std::vector<double> sx = rx, sy = ry;
		for (int k = 1; k + 1 < n; ++k) {
			sx[size_t(k)] = 0.25 * rx[size_t(k - 1)] + 0.5 * rx[size_t(k)] + 0.25 * rx[size_t(k + 1)];
			sy[size_t(k)] = 0.25 * ry[size_t(k - 1)] + 0.5 * ry[size_t(k)] + 0.25 * ry[size_t(k + 1)];
		}
		rx.swap(sx); ry.swap(sy);
	}
	// tangents, then a wide moving average for the offset normals
	std::vector<double> tx(static_cast<size_t>(n)), ty(static_cast<size_t>(n));
	for (int k = 0; k < n; ++k) {
		const int a = std::max(0, k - 1), b = std::min(n - 1, k + 1);
		double dx = rx[size_t(b)] - rx[size_t(a)], dy = ry[size_t(b)] - ry[size_t(a)];
		const double l = std::max(1e-12, std::hypot(dx, dy));
		tx[size_t(k)] = dx / l; ty[size_t(k)] = dy / l;
	}
	const int win = std::max(1, std::min(n / 3, int(std::round(0.6 * reach / st))));
	tr.cx = rx; tr.cy = ry;
	tr.nx.assign(size_t(n), 0.0); tr.ny.assign(size_t(n), 0.0);
	{
		// prefix sums for the moving average
		std::vector<double> ax(size_t(n + 1), 0.0), ay(size_t(n + 1), 0.0);
		for (int k = 0; k < n; ++k) { ax[size_t(k + 1)] = ax[size_t(k)] + tx[size_t(k)]; ay[size_t(k + 1)] = ay[size_t(k)] + ty[size_t(k)]; }
		for (int k = 0; k < n; ++k) {
			const int a = std::max(0, k - win), b = std::min(n - 1, k + win);
			double mx = ax[size_t(b + 1)] - ax[size_t(a)], my = ay[size_t(b + 1)] - ay[size_t(a)];
			double l = std::hypot(mx, my);
			if (l < 1e-9) { mx = tx[size_t(k)]; my = ty[size_t(k)]; l = 1.0; }
			mx /= l; my /= l;
			tr.nx[size_t(k)] = my; tr.ny[size_t(k)] = -mx;   // right-hand normal
		}
	}
	// fold-free limits: between neighbours, the offset segment must keep
	// running forward ( (dc + d dn) . t >= 0.35 |dc| )
	tr.limR.assign(size_t(n), 1e300);
	tr.limL.assign(size_t(n), 1e300);
	for (int k = 0; k + 1 < n; ++k) {
		const double dcx = rx[size_t(k + 1)] - rx[size_t(k)], dcy = ry[size_t(k + 1)] - ry[size_t(k)];
		const double dl = std::hypot(dcx, dcy);
		if (dl < 1e-12) continue;
		const double ux = dcx / dl, uy = dcy / dl;
		const double a = (tr.nx[size_t(k + 1)] - tr.nx[size_t(k)]) * ux + (tr.ny[size_t(k + 1)] - tr.ny[size_t(k)]) * uy;
		const double room = 0.65 * dl;
		if (a < -1e-12) { const double L = room / -a; tr.limR[size_t(k)] = std::min(tr.limR[size_t(k)], L); tr.limR[size_t(k + 1)] = std::min(tr.limR[size_t(k + 1)], L); }
		if (a > 1e-12)  { const double L = room / a;  tr.limL[size_t(k)] = std::min(tr.limL[size_t(k)], L); tr.limL[size_t(k + 1)] = std::min(tr.limL[size_t(k + 1)], L); }
	}
	// spread each limit over the reach (a slab spans several samples)
	const int spread = std::max(1, int(std::ceil(reach / st)));
	for (std::vector<double>* Lp : {&tr.limR, &tr.limL}) {
		std::vector<double> o(static_cast<size_t>(n));
		for (int k = 0; k < n; ++k) {
			double mn = 1e300;
			for (int q = std::max(0, k - spread); q <= std::min(n - 1, k + spread); ++q) mn = std::min(mn, (*Lp)[size_t(q)]);
			o[size_t(k)] = mn;
		}
		Lp->swap(o);
	}
	tr.step = st;
	tr.len = len;
	return true;
}

// --- one fissure -------------------------------------------------------------------

struct Fissure {
	uint64_t key = 0;
	Track tr;
	double W = 1.0;     // full width (nominal)
	double D = 1.0;     // depth (nominal)
	double sc = 1.0;    // mask size scale
	V off[4];           // smooth noise offsets
};

struct Ctx {
	const GroundFissureParams* p = nullptr;
	const Ground* g = nullptr;
	const m::Manifold* box = nullptr;
	uint64_t seed = 0;
};

// Width/depth envelope toward the stroke ends: 0 at the tip, 1 inside.
double EndEnv(const Ctx& cx, const Fissure& F, double s)
{
	const double tl = std::max(1e-6, ClampD(cx.p->taper, 0.0, 0.5) * F.tr.len);
	return Smooth(0.0, 1.0, std::min(s, F.tr.len - s) / tl);
}

// Half-width of the chasm on one side (0 left, 1 right).
double HalfW(const Ctx& cx, const Fissure& F, int side, double s)
{
	const GroundFissureParams& p = *cx.p;
	const double env = EndEnv(cx, F, s);
	const double slow = 1.0 + ClampD(p.widthRandom, 0.0, 1.0) * 0.8 * Fbm(V(s / (3.0 * F.W), 0.5, 0.0) + F.off[0], 2);
	const double feat = std::max(1e-3, 0.7 * F.W * std::max(0.05, p.edgeScale));
	const double jag = ClampD(p.jagged, 0.0, 2.0);
	const uint64_t k = F.key * 4 + uint64_t(side);
	const double rim = 0.32 * Angular(cx.seed, k ^ 0x52494d31ULL, s / feat)
		+ 0.12 * Angular(cx.seed, k ^ 0x52494d32ULL, s / (0.29 * feat));
	double w = 0.5 * F.W * slow * Lerp(0.05, 1.0, std::pow(env, 0.7)) * (1.0 + jag * rim);
	w = std::max(w, 0.02 * F.W);
	return std::min(w, 0.85 * F.tr.Limit(side, s));
}

double DepthAt(const Ctx& cx, const Fissure& F, double s)
{
	const double env = EndEnv(cx, F, s);
	const double v = 1.0 + ClampD(cx.p->depthRandom, 0.0, 1.0) * 0.8 * Fbm(V(s / (4.0 * F.W), 2.5, 0.0) + F.off[1], 2);
	return F.D * std::max(0.1, v) * Lerp(0.12, 1.0, env);
}

// Wall lean on one side, as a share of the half-width (+ = undercut).
double Lean(const Ctx& cx, const Fissure& F, int side, double s)
{
	const double feat = std::max(1e-3, 1.6 * F.W);
	return ClampD(cx.p->wallLean, 0.0, 1.0) * Angular(cx.seed, (F.key * 4 + uint64_t(side)) ^ 0x4c45414eULL, s / feat);
}

// Depth below the ground across the chasm (u -1 left rim .. 1 right rim),
// before the thickness cap: sheer walls, then the flat-to-V floor, rough.
double ChasmDepth(const Ctx& cx, const Fissure& F, double s, double u, double Dc)
{
	const GroundFissureParams& p = *cx.p;
	const double fe = Lerp(0.85, 0.55, ClampD(p.floorShape, 0.0, 1.0));
	const double pw = Lerp(5.0, 1.2, ClampD(p.floorShape, 0.0, 1.0));
	const double rough = ClampD(p.wallRough, 0.0, 2.0);
	double dep = Dc * (fe + (1.0 - fe) * (1.0 - std::pow(std::abs(u), pw)));
	const V q = V(s / (0.35 * F.W), u * 1.7, 0.0) + F.off[2];
	dep += rough * Dc * (0.05 * Fbm(q, 3) + 0.03 * Fbm(q * 3.1 + V(4.2, 1.1, 0.0), 2));
	return dep;
}

struct Out {
	std::vector<m::Manifold> cutters, cracks, slabs;
	m::Manifold region;   // slabs only: the source clipped to the slab area
	m::Box clip;          // ... and the clip box
};

// --- the chasm ----------------------------------------------------------------------

void BuildChasm(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const double ds = std::max(tr.len / 3000.0, std::min(F.W / 7.0, tr.len / 10.0) / p.detail);
	const int ns = std::max(4, int(std::ceil(tr.len / ds)));
	int nu = std::max(8, int(std::round(16.0 * p.detail)));
	nu += nu & 1;
	const double H = 0.25 * F.W + 1e-3;
	const double cap = ClampD(p.thicknessCap, 0.0, 0.98);
	std::vector<V> top(size_t(ns + 1) * size_t(nu + 1)), bot(top.size());
	double lastH = 0.0, lastT = 1e300;
	bool haveH = false;
	for (int i = 0; i <= ns; ++i) {
		const double s = tr.len * double(i) / ns;
		const double wl = HalfW(cx, F, 0, s), wr = HalfW(cx, F, 1, s);
		const double Dc = DepthAt(cx, F, s);
		const double ll = Lean(cx, F, 0, s), lr = Lean(cx, F, 1, s);
		// row heights: drop every column, fill misses from neighbours
		std::vector<double> hh(size_t(nu + 1), 0.0), th(size_t(nu + 1), 1e300);
		std::vector<char> ok(size_t(nu + 1), 0), av(size_t(nu + 1), 0);
		for (int j = 0; j <= nu; ++j) {
			const double u = -1.0 + 2.0 * j / nu;
			const double d = u < 0.0 ? u * wl : u * wr;
			double X, Y, h, t;
			bool a;
			tr.Map(s, d, X, Y);
			if (g.Drop(X, Y, h, t, a)) { hh[size_t(j)] = h; th[size_t(j)] = t; ok[size_t(j)] = 1; av[size_t(j)] = a ? 1 : 0; }
		}
		// misses (past the ground's edge) take the nearest hit's height and
		// thickness, so the cap still holds where the chasm runs off the edge
		for (int j = 0; j <= nu; ++j) {
			if (ok[size_t(j)]) { lastH = hh[size_t(j)]; lastT = th[size_t(j)]; haveH = true; break; }
		}
		for (int j = 0; j <= nu; ++j) {
			if (ok[size_t(j)]) { lastH = hh[size_t(j)]; lastT = th[size_t(j)]; continue; }
			hh[size_t(j)] = haveH ? lastH : 0.0;
			th[size_t(j)] = haveH ? lastT : 1e300;
		}
		for (int j = 0; j <= nu; ++j) {
			const double u = -1.0 + 2.0 * j / nu;
			const double au = std::abs(u);
			const double d = u < 0.0 ? u * wl : u * wr;
			const double lean = u < 0.0 ? ll : lr;
			const double db = d * (1.0 + lean * std::pow(au, 3.0));
			double dep = ChasmDepth(cx, F, s, u, Dc);
			if (cap > 0.0 && th[size_t(j)] < 1e299) dep = std::min(dep, cap * th[size_t(j)]);
			if (av[size_t(j)]) dep = -0.1 * H;   // Avoid Selection: float above the ground
			const double h = hh[size_t(j)];
			double X, Y, Xb, Yb;
			tr.Map(s, d, X, Y);
			tr.Map(s, db, Xb, Yb);
			const size_t k = size_t(i) * size_t(nu + 1) + size_t(j);
			top[k] = g.At(X, Y, h + H);
			bot[k] = g.At(Xb, Yb, h - dep);
		}
	}
	m::Manifold c = SlabSolid(top, bot, ns, nu);
	if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
}

// Realistic chasm (style 1): the same height-field cutter as BuildChasm, but
// its two long walls run through extra rings: the walls get stratified relief
// (horizontal ledges and undercut bands, eroded outward into the ground),
// the rim crumbles into an irregular chamfer, and discrete chips are bitten
// out of the rim edge.
// Value in [0, 1] with horizontal ledges: smooth noise stretched along the
// wall, partly snapped to steps.
double Ledges(const V& q)
{
	const double n = 0.5 + 0.5 * Fbm(q, 3);
	const double st = std::floor(n * 4.0) / 4.0;
	return ClampD(Lerp(n, st, 0.55), 0.0, 1.0);
}

// Piecewise LINEAR value noise in [0, 1]: random values on a rotated
// lattice, interpolated inside the six tetrahedra of each cell (Kuhn
// split). Linear per tetrahedron, so a displaced surface breaks into flat
// facets with crisp creases: broken stone, not clay, and no grid pattern.
double LinFacet(uint64_t seed, const V& qIn)
{
	// fixed rotation so the facets do not line up with the axes
	const V q(0.6 * qIn.x - 0.64 * qIn.y + 0.48 * qIn.z, 0.8 * qIn.x + 0.48 * qIn.y - 0.36 * qIn.z, 0.6 * qIn.y + 0.8 * qIn.z);
	const double bx = std::floor(q.x), by = std::floor(q.y), bz = std::floor(q.z);
	double f[3] = {q.x - bx, q.y - by, q.z - bz};
	int ax[3] = {0, 1, 2};
	std::sort(ax, ax + 3, [&](int a, int b) { return f[a] > f[b]; });
	int64_t c[3] = {int64_t(bx), int64_t(by), int64_t(bz)};
	auto val = [&]() {
		const uint64_t key = uint64_t(c[0]) * 0x8da6b343ULL ^ uint64_t(c[1]) * 0xd8163841ULL ^ uint64_t(c[2]) * 0xcb1ab31fULL;
		return Random01(seed, key, 7);
	};
	double v = (1.0 - f[ax[0]]) * val();
	c[ax[0]] += 1; v += (f[ax[0]] - f[ax[1]]) * val();
	c[ax[1]] += 1; v += (f[ax[1]] - f[ax[2]]) * val();
	c[ax[2]] += 1; v += f[ax[2]] * val();
	return v;
}

void BuildChasmV2(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const double ds = std::max(tr.len / 3000.0, std::min(F.W / 9.0, tr.len / 12.0) / p.detail);
	const int ns = std::max(4, int(std::ceil(tr.len / ds)));
	int nu = std::max(8, int(std::round(16.0 * p.detail)));
	nu += nu & 1;
	const int nl = std::max(5, std::min(24, int(std::round(11.0 * p.detail))));
	const double H = 0.25 * F.W + 1e-3;
	const double cap = ClampD(p.thicknessCap, 0.0, 0.98);
	const double rough = ClampD(p.faceRough, 0.0, 2.0);
	const double ed = ClampD(p.edgeDamage, 0.0, 2.0);
	const size_t N = size_t(ns + 1) * size_t(nu + 1);
	std::vector<V> top(N), bot(N);
	std::vector<double> gh(N, 0.0), gd(N, 0.0), gdb(N, 0.0), gdep(N, 0.0), gs(N, 0.0);
	double lastH = 0.0, lastT = 1e300;
	bool haveH = false;
	for (int i = 0; i <= ns; ++i) {
		const double s = tr.len * double(i) / ns;
		const double wl = HalfW(cx, F, 0, s), wr = HalfW(cx, F, 1, s);
		const double Dc = DepthAt(cx, F, s);
		const double ll = Lean(cx, F, 0, s), lr = Lean(cx, F, 1, s);
		std::vector<double> hh(size_t(nu + 1), 0.0), th(size_t(nu + 1), 1e300);
		std::vector<char> ok(size_t(nu + 1), 0), av(size_t(nu + 1), 0);
		for (int j = 0; j <= nu; ++j) {
			const double u = -1.0 + 2.0 * j / nu;
			const double d = u < 0.0 ? u * wl : u * wr;
			double X, Y, h, t;
			bool a;
			tr.Map(s, d, X, Y);
			if (g.Drop(X, Y, h, t, a)) { hh[size_t(j)] = h; th[size_t(j)] = t; ok[size_t(j)] = 1; av[size_t(j)] = a ? 1 : 0; }
		}
		for (int j = 0; j <= nu; ++j) {
			if (ok[size_t(j)]) { lastH = hh[size_t(j)]; lastT = th[size_t(j)]; haveH = true; break; }
		}
		for (int j = 0; j <= nu; ++j) {
			if (ok[size_t(j)]) { lastH = hh[size_t(j)]; lastT = th[size_t(j)]; continue; }
			hh[size_t(j)] = haveH ? lastH : 0.0;
			th[size_t(j)] = haveH ? lastT : 1e300;
		}
		for (int j = 0; j <= nu; ++j) {
			const double u = -1.0 + 2.0 * j / nu;
			const double au = std::abs(u);
			const double d = u < 0.0 ? u * wl : u * wr;
			const double lean = u < 0.0 ? ll : lr;
			const double db = d * (1.0 + lean * std::pow(au, 3.0));
			double dep = ChasmDepth(cx, F, s, u, Dc);
			if (cap > 0.0 && th[size_t(j)] < 1e299) dep = std::min(dep, cap * th[size_t(j)]);
			if (av[size_t(j)]) dep = -0.1 * H;
			const double h = hh[size_t(j)];
			double X, Y, Xb, Yb;
			tr.Map(s, d, X, Y);
			tr.Map(s, db, Xb, Yb);
			const size_t k = size_t(i) * size_t(nu + 1) + size_t(j);
			top[k] = g.At(X, Y, h + H);
			bot[k] = g.At(Xb, Yb, h - dep);
			gh[k] = h; gd[k] = d; gdb[k] = db; gdep[k] = dep; gs[k] = s;
		}
	}
	// wall relief: outward erosion of the wall at node (i, j) and depth z
	// below the ground (0 at the rim)
	auto crumbW = [&](int side, double s) {   // rim chamfer width at the surface
		const uint64_t k = (F.key * 4 + uint64_t(side)) ^ 0x4352554dULL;
		const double a = Angular(cx.seed, k, s / (0.22 * F.W + 1e-6));
		const double b = Angular(cx.seed, k ^ 0x77ULL, s / (0.07 * F.W + 1e-6));
		return ed * F.W * std::max(0.0, 0.05 + 0.06 * a + 0.025 * b);
	};
	auto crumbD = [&](int side, double s) {   // ... and how far down it reaches
		const uint64_t k = (F.key * 4 + uint64_t(side)) ^ 0x43525544ULL;
		return ed * F.W * Lerp(0.04, 0.16, 0.5 + 0.5 * Angular(cx.seed, k, s / (0.3 * F.W + 1e-6)));
	};
	auto mid = [&](int i, int j, int l) -> V {
		const size_t k = size_t(i) * size_t(nu + 1) + size_t(j);
		const bool wall = (j == 0 || j == nu) && i > 0 && i < ns && gdep[k] > 0.0;
		if (!wall) {
			const double f = double(l) / double(nl);
			return top[k] + (bot[k] - top[k]) * f;
		}
		const int side = j == 0 ? 0 : 1;
		const double sgn = side ? 1.0 : -1.0;
		const double s = gs[k], dep = gdep[k], h = gh[k];
		const double env = EndEnv(cx, F, s);
		const double cw = crumbW(side, s) * env, cd = std::min(crumbD(side, s) * env, 0.3 * dep);
		double z;   // depth below the ground
		if (l == 1) z = -std::min(0.05 * F.W, 0.3 * H);
		else if (l == 2) z = std::max(cd, 0.02 * dep);
		else z = std::max(cd, 0.02 * dep) + (dep - std::max(cd, 0.02 * dep)) * double(l - 2) / double(nl - 2);
		const double f = ClampD(z / std::max(1e-9, dep), 0.0, 1.0);
		double dw = gd[k] + (gdb[k] - gd[k]) * f;
		const double hw = std::abs(gd[k]);
		const double amp = rough * 0.11 * F.W * env * std::min(1.0, hw / (0.35 * F.W));
		const V q = V(s / (0.5 * F.W), std::max(0.0, z) / (0.12 * F.W), double(side) * 13.7) + F.off[3];
		const double nw = 0.5 * Ledges(q) + 0.5 * LinFacet(cx.seed ^ F.key ^ 0x57ULL, V(q.x * 0.7, q.y * 0.6, q.z));
		double off = amp * nw * Smooth(0.0, 0.5 * std::max(cd, 0.05 * dep) + 1e-9, std::max(0.0, z));
		if (l == 1) off = cw + amp * 0.3;
		else if (l == 2) off = std::max(off, 0.25 * cw);
		off *= Smooth(0.0, 0.25, 1.0 - f) * 0.7 + 0.3;   // relax toward the floor
		const double lim = 0.9 * tr.Limit(side, s);
		dw = sgn * std::min(std::abs(dw) + off, std::max(std::abs(dw), lim));
		double X, Y;
		tr.Map(s, dw, X, Y);
		return g.At(X, Y, h - z);
	};
	m::Manifold c = RingSolid(top, bot, ns, nu, nl, mid);
	if (!c.IsEmpty()) out.cutters.push_back(std::move(c));
	if (ed <= 0.0) return;
	// discrete chips bitten out of the rim edge (the ground side)
	for (int side = 0; side < 2; ++side) {
		const uint64_t ck = (F.key << 24) ^ (uint64_t(side) << 20) ^ 0x43484950ULL;   // "CHIP"
		const double sgn = side ? 1.0 : -1.0;
		const double step = 0.28 * F.W;
		const int n = int(tr.len / step);
		for (int q = 0; q < n && q < 20000; ++q) {
			if (Random01(cx.seed, ck + uint64_t(q), 0) > std::min(1.0, 0.45 * ed)) continue;
			const double s = (double(q) + Random01(cx.seed, ck + uint64_t(q), 1)) * step;
			const double env = EndEnv(cx, F, s);
			if (env < 0.2) continue;
			const double hw = HalfW(cx, F, side, s);
			double X, Y, h, t, tx, ty;
			bool av;
			tr.Map(s, sgn * (hw + crumbW(side, s) * env), X, Y);
			if (!g.Drop(X, Y, h, t, av) || av) continue;
			tr.Tangent(s, tx, ty);
			const V along = Unit(g.ex * tx + g.ey * ty, g.ex);
			const V in = Unit(m::la::cross(g.up, along), g.ey) * sgn;   // into the ground, away from the chasm
			const double sz = F.W * Lerp(0.06, 0.16, Random01(cx.seed, ck + uint64_t(q), 2)) * Lerp(0.7, 1.2, ClampD(ed, 0.0, 2.0) / 2.0) * env;
			m::Manifold chip = ChipHull(cx.seed, ck + uint64_t(q), g.At(X, Y, h), along, in, g.up,
				sz * Lerp(1.4, 2.6, Random01(cx.seed, ck + uint64_t(q), 3)), sz, sz * Lerp(0.6, 1.2, Random01(cx.seed, ck + uint64_t(q), 4)));
			if (!chip.IsEmpty()) out.cutters.push_back(std::move(chip));
		}
	}
}

// --- slabs ---------------------------------------------------------------------------

uint64_t SlabKey(const Fissure& F, int side, int row, uint64_t k)
{
	return (F.key << 24) ^ (uint64_t(side) << 22) ^ (uint64_t(row) << 20) ^ (k & 0xfffffULL) ^ 0x534c4142ULL;
}

struct Row {
	double base = 1.0;    // nominal width
	uint64_t key = 0;
};

double RowWidth(const Ctx& cx, const Row& R, double s)
{
	double v = 1.0 + 0.3 * Angular(cx.seed, R.key ^ 0x52574944ULL, s / (1.3 * R.base + 1e-6));
	if (cx.p->realistic) {
		// broken, not sawn: a fine zigzag on the crack between two rows
		const double jag = ClampD(cx.p->slabJagged, 0.0, 2.0);
		v += jag * (0.08 * Angular(cx.seed, R.key ^ 0x5257ULL, s / (0.33 * R.base + 1e-6))
			+ 0.035 * Angular(cx.seed, R.key ^ 0x5258ULL, s / (0.11 * R.base + 1e-6)));
	}
	return R.base * v;
}

// Inner offset (|d|) of a row at s: the chasm rim plus all rows inside it.
double RowInner(const Ctx& cx, const Fissure& F, int side, const std::vector<Row>& rows, int r, double s)
{
	double d = HalfW(cx, F, side, s);
	for (int q = 0; q < r; ++q) d += RowWidth(cx, rows[size_t(q)], s);
	return d;
}

void BuildSlabs(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const int nRows = std::max(1, std::min(p.slabRows, 3));
	const double lMin = std::max(1e-3, std::min(p.slabLengthMin, p.slabLengthMax)) * F.sc;
	const double lMax = std::max(p.slabLengthMin, p.slabLengthMax) * F.sc;
	const double wMin = std::max(1e-3, std::min(p.slabWidthMin, p.slabWidthMax)) * F.sc;
	const double wMax = std::max(p.slabWidthMin, p.slabWidthMax) * F.sc;
	const double tMin = std::max(1e-3, std::min(p.slabThickMin, p.slabThickMax)) * F.sc;
	const double tMax = std::max(p.slabThickMin, p.slabThickMax) * F.sc;
	const double gap = std::max(0.0, p.slabGap) * F.sc;
	const double fall = ClampD(p.slabRowFalloff, 0.05, 1.5);
	const double jag = ClampD(p.slabJagged, 0.0, 2.0);
	const double cap = ClampD(p.thicknessCap, 0.0, 0.98);
	const double H = 0.25 * F.W + tMax * 0.5 + 1e-3;

	// slabs only where the chasm is open: env >= 0.5
	double sa = 0.0, sb = tr.len;
	{
		const double tl = ClampD(p.taper, 0.0, 0.5) * tr.len;
		sa = 0.5 * tl; sb = tr.len - 0.5 * tl;
	}
	if (sb - sa < 0.5 * lMin) return;

	struct Pose { double ang = 0.0, roll = 0.0; };
	struct Job { int side, row; uint64_t k; double s0, s1; uint64_t b0, b1; double sk0 = 0.0, sk1 = 0.0; };
	std::vector<Job> jobs;
	std::vector<Row> rows[2];
	for (int side = 0; side < 2; ++side) {
		for (int r = 0; r < nRows; ++r) {
			Row R;
			R.key = SlabKey(F, side, r, 0xfffff);
			R.base = Lerp(wMin, wMax, Random01(cx.seed, R.key, 0)) * std::pow(fall, double(r));
			rows[side].push_back(R);
		}
		for (int r = 0; r < nRows; ++r) {
			const uint64_t rk = rows[side][size_t(r)].key;
			double s = sa - Random01(cx.seed, rk, 1) * 0.5 * lMin;
			uint64_t k = 0;
			while (s < sb - 1e-6 && k < 100000) {
				const uint64_t key = SlabKey(F, side, r, k);
				double L = Lerp(lMin, lMax, Random01(cx.seed, key, 0));
				// realistic: now and then a short broken-off piece between full slabs
				if (p.realistic && Random01(cx.seed, key, 7) < 0.22) L = Lerp(0.4, 0.65, Random01(cx.seed, key, 8)) * lMin;
				double s1 = s + L;
				if (sb - s1 < 0.4 * lMin) s1 = sb;
				const double c0 = std::max(s, sa), c1 = std::min(s1, sb);
				if (c1 - c0 > 0.3 * lMin)
					jobs.push_back({side, r, k, c0, c1, SlabKey(F, side, r, k) ^ 0x424e44ULL, SlabKey(F, side, r, k + 1) ^ 0x424e44ULL, 0.0, 0.0});
				s = s1;
				++k;
			}
		}
	}
	if (jobs.empty()) return;
	// slant of every cross crack. Realistic: neighbouring cracks lean the
	// opposite way, so the slabs come out as wedges and near-triangles, the
	// way a broken rim really splits; the lean is held short of crossing.
	{
		const double skS = ClampD(p.slabSkew, 0.0, 2.0);
		auto classic = [&](uint64_t bkey) { return skS * (2.0 * Random01(cx.seed, bkey, 0) - 1.0); };
		size_t a = 0;
		while (a < jobs.size()) {
			size_t e = a + 1;
			while (e < jobs.size() && jobs[e].side == jobs[a].side && jobs[e].row == jobs[a].row) ++e;
			const double rowW = rows[jobs[a].side][size_t(jobs[a].row)].base;
			const uint64_t rk = rows[jobs[a].side][size_t(jobs[a].row)].key;
			double flip = Random01(cx.seed, rk, 9) < 0.5 ? 1.0 : -1.0;
			for (size_t q = a; q < e; ++q) {
				Job& J = jobs[q];
				if (!p.realistic) { J.sk0 = classic(J.b0); J.sk1 = classic(J.b1); continue; }
				const bool shared = q > a && jobs[q - 1].b1 == J.b0;
				if (shared) { J.sk0 = jobs[q - 1].sk1; }
				else {
					const double lim = 0.6 * (J.s1 - J.s0) / std::max(1e-9, rowW);
					J.sk0 = flip * std::min(lim, skS / 0.35 * Lerp(0.35, 1.1, Random01(cx.seed, J.b0, 1)));
				}
				// next crack leans the other way (now and then the same way: a parallelogram)
				if (Random01(cx.seed, J.b1, 2) > 0.15) flip = -flip;
				const double lNext = q + 1 < e && jobs[q + 1].b0 == J.b1 ? jobs[q + 1].s1 - jobs[q + 1].s0 : 1e300;
				const double lim = 0.6 * std::min(J.s1 - J.s0, lNext) / std::max(1e-9, rowW);
				double m1 = skS / 0.35 * Lerp(0.35, 1.1, Random01(cx.seed, J.b1, 1));
				// both cracks of one slab must not use up more than its length
				m1 = std::min(m1, std::max(0.0, 1.3 * (J.s1 - J.s0) / std::max(1e-9, rowW) - std::abs(J.sk0)));
				J.sk1 = flip * std::min(lim, m1);
			}
			a = e;
		}
	}

	// Slab footprint node: s along the slab (a 0..1), b across (0 inner .. 1 outer).
	// Cross cracks are slanted and jagged; the same boundary function is used
	// by both slabs that share it, so neighbouring sockets meet exactly.
	auto boundS = [&](uint64_t bkey, double sNom, double b, double rowW, double skew) {
		double sOut = sNom + skew * (b - 0.5) * rowW + jag * 0.12 * rowW * Angular(cx.seed, bkey, b * 3.0);
		if (p.realistic) sOut += jag * rowW * (0.05 * Angular(cx.seed, bkey ^ 0x46ULL, b * 8.0) + 0.02 * Angular(cx.seed, bkey ^ 0x47ULL, b * 23.0));
		return sOut;
	};

	std::vector<m::Manifold> prisms(jobs.size()), sockets(jobs.size());
	// realistic: chips and hairline cracks cut from each slab before it is
	// posed, and chips bitten from the intact ground behind the outer row
	std::vector<std::vector<m::Manifold>> slabCuts(jobs.size()), groundChips(jobs.size());
	struct Split { m::Manifold cut, keep; m::mat3x4 fold; };
	std::vector<std::vector<Split>> splits(jobs.size());
	std::vector<m::mat3x4> poses(jobs.size());
	std::vector<char> live(jobs.size(), 0);

	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t ji) {
		const Job& J = jobs[size_t(ji)];
		const Row& R = rows[J.side][size_t(J.row)];
		const uint64_t key = SlabKey(F, J.side, J.row, J.k);
		const double sgn = J.side ? 1.0 : -1.0;
		const double rowW = R.base;
		const double ext = J.row == 0 ? 0.25 * HalfW(cx, F, J.side, 0.5 * (J.s0 + J.s1)) : 0.5 * gap;
		const double sLen = J.s1 - J.s0;
		const bool v2 = p.realistic;
		const double cell = v2 ? std::max(1e-3, std::min({F.W / 8.0, rowW / 8.0, sLen / 8.0}) / p.detail)
			: std::max(1e-3, std::min({F.W / 6.0, rowW / 4.0, sLen / 4.0}) / p.detail);
		const int na = std::max(4, int(std::ceil(sLen / cell)));
		const int nb = std::max(4, int(std::ceil(rowW / cell)));
		double T = Lerp(tMin, tMax, Random01(cx.seed, key, 1));
		const double bevel = v2 ? 0.0 : jag * Lerp(0.05, 0.3, Random01(cx.seed, key, 6)) * T;
		std::vector<V> gnd;   // ground point under every node of the last grid() call

		// insets: the socket is the full footprint (plus ext into the chasm
		// or the inner row's socket), the slab is inset by half a gap on its
		// sides and back, and a hair on the rim side
		double minWide = 1e300;   // narrowest the row gets under this slab (bend limits)
		auto grid = [&](bool socket, std::vector<V>& top, std::vector<V>& bot, double depthTop, double depthBot,
			double& minThick, bool& avoided, bool& anyHit, V* corners) {
			top.assign(size_t(na + 1) * size_t(nb + 1), V(0.0));
			bot.assign(top.size(), V(0.0));
			minThick = 1e300;
			avoided = false;
			anyHit = false;
			// neighbouring sockets overlap a little: two cutters meeting
			// exactly on a shared face would leave a zero-thickness wall
			const double inS = socket ? -0.25 * gap - 1e-3 * rowW : 0.5 * gap;
			const double inIn = socket ? -ext - 1e-3 * rowW : (J.row == 0 ? 0.1 * gap : 0.5 * gap);
			const double inOut = socket ? -1e-3 * rowW : 0.5 * gap;
			std::vector<double> hs(top.size(), 0.0);
			std::vector<char> ok(top.size(), 0);
			std::vector<double> XS(top.size()), YS(top.size()), XB(top.size()), YB(top.size());
			for (int i = 0; i <= na; ++i)
				for (int j = 0; j <= nb; ++j) {
					const double a = double(i) / na, b = double(j) / nb;
					const double sA = boundS(J.b0, J.s0, b, rowW, J.sk0) + inS;
					const double sB = boundS(J.b1, J.s1, b, rowW, J.sk1) - inS;
					const double s = ClampD(Lerp(sA, sB, a), 0.0, tr.len);
					const double dIn = RowInner(cx, F, J.side, rows[J.side], J.row, s);
					double dOut = dIn + RowWidth(cx, R, s);
					if (J.row + 1 == nRows) dOut += jag * 0.1 * rowW * Angular(cx.seed, key ^ 0x4f5554ULL, s / (0.4 * rowW + 1e-6));
					dOut = std::min(dOut, 0.92 * tr.Limit(J.side, s));
					const double lo = dIn + inIn, hi = std::max(lo + 1e-3, dOut - inOut);
					minWide = std::min(minWide, dOut - dIn);
					const double d = sgn * Lerp(lo, hi, b);
					double X, Y, h, t;
					bool av;
					tr.Map(s, d, X, Y);
					const size_t k = size_t(i) * size_t(nb + 1) + size_t(j);
					XS[k] = X; YS[k] = Y;
					// broken rim face: the slab's bottom rim edge wanders in and
					// out, so the face it leaves toward the chasm is slanted
					if (!socket && j == 0 && !v2) {
						const double wob = jag * 0.3 * T * Angular(cx.seed, key ^ 0x46414345ULL, double(i) * 0.8);
						tr.Map(s, sgn * (std::max(0.0, std::abs(d) + wob)), XB[k], YB[k]);
					} else { XB[k] = X; YB[k] = Y; }
					if (g.Drop(X, Y, h, t, av)) {
						hs[k] = h; ok[k] = 1; anyHit = true;
						minThick = std::min(minThick, t);
						avoided = avoided || av;
					}
				}
			if (!anyHit) return;
			// fill misses (past the ground's edge) with the nearest hit in the row
			for (int i = 0; i <= na; ++i) {
				double last = 0.0;
				bool have = false;
				for (int j = 0; j <= nb; ++j) { const size_t k = size_t(i) * size_t(nb + 1) + size_t(j); if (ok[k]) { last = hs[k]; have = true; break; } }
				if (!have) {
					for (size_t k = 0; k < hs.size(); ++k) if (ok[k]) { last = hs[k]; have = true; break; }
				}
				for (int j = 0; j <= nb; ++j) {
					const size_t k = size_t(i) * size_t(nb + 1) + size_t(j);
					if (ok[k]) last = hs[k]; else hs[k] = last;
				}
			}
			for (int i = 0; i <= na; ++i)
				for (int j = 0; j <= nb; ++j) {
					const size_t k = size_t(i) * size_t(nb + 1) + size_t(j);
					double tTop = depthTop;
					// bevel (crumbled rim edge): the prism top dips under the
					// ground on the rim column and rises over the next one
					if (!socket && j == 0) tTop = -bevel * (0.6 + 0.4 * Angular(cx.seed, key ^ 0x42564cULL, double(i) * 0.7));
					else if (!socket && j == 1 && bevel > 0.0) tTop = 0.1 * T;
					top[k] = g.At(XS[k], YS[k], hs[k] + tTop);
					double dBot = depthBot;
					if (v2 && !socket) {
						// uneven break underneath: thickness wanders toward the rim side,
						// the hinge side keeps the full depth (it holds the weld)
						const double b = double(j) / nb;
						dBot *= 1.0 + ClampD(p.faceRough, 0.0, 2.0) * 0.22 * (1.0 - b) * Fbm(V(XS[k] / (0.9 * depthBot), YS[k] / (0.9 * depthBot), 3.3) + F.off[1], 2);
					}
					bot[k] = g.At(XB[k], YB[k], hs[k] - dBot);
				}
			gnd.resize(top.size());
			for (size_t k = 0; k < top.size(); ++k) gnd[k] = g.At(XS[k], YS[k], hs[k]);
			if (corners) {
				// back (hinge) edge ends and the rim midpoint, at ground level
				const size_t kb0 = size_t(0) * size_t(nb + 1) + size_t(nb);
				const size_t kb1 = size_t(na) * size_t(nb + 1) + size_t(nb);
				const size_t km = size_t(na / 2) * size_t(nb + 1);
				corners[0] = g.At(XS[kb0], YS[kb0], hs[kb0]);
				corners[1] = g.At(XS[kb1], YS[kb1], hs[kb1]);
				corners[2] = g.At(XS[km], YS[km], hs[km]);
			}
		};

		std::vector<V> top, bot;
		double minThick;
		// with the source available the prism reaches over the ground and is
		// intersected with it; without, the prism itself is the slab
		const double prismTop = (cx.box && !cx.box->IsEmpty()) ? H : 0.0;
		bool avoided, anyHit;
		V corners[3];
		// slab prism first, to learn the ground thickness under the slab
		grid(false, top, bot, prismTop, T, minThick, avoided, anyHit, corners);
		if (!anyHit || avoided) return;
		if (minWide < 0.3 * rowW) return;   // squeezed out on the inside of a sharp bend
		if (minThick < 1e299) {
			if (cap <= 0.0 && T > 0.9 * minThick) return;
			T = std::min(T, 0.5 * minThick);
			if (T < 0.02 * tMin) return;
			grid(false, top, bot, prismTop, T, minThick, avoided, anyHit, corners);
		}
		// realistic: the side walls get the relief of a broken face. The slab
		// face is eroded inward and the socket face outward by the same noise,
		// so the two faces of every crack match like real fracture surfaces.
		const int nlv = std::max(5, std::min(20, int(std::round(8.0 * p.detail))));
		const double slant = 0.45 * (2.0 * Random01(cx.seed, key, 32) - 1.0);
		const double rough = ClampD(p.faceRough, 0.0, 2.0);
		auto idx = [&](int i, int j) { return size_t(i) * size_t(nb + 1) + size_t(j); };
		auto outward = [&](const std::vector<V>& gp, int i, int j) {
			const int i2 = i == 0 ? 1 : (i == na ? na - 1 : i);
			const int j2 = j == 0 ? 1 : (j == nb ? nb - 1 : j);
			const V dv = gp[idx(i, j)] - gp[idx(i2, j2)];
			return Unit(dv - g.up * m::la::dot(dv, g.up), g.ex);
		};
		auto faceMid = [&](const std::vector<V>& tp, const std::vector<V>& bt, bool socket, double capA) {
			return [&, socket, capA](int i, int j, int l) -> V {
				const size_t k = idx(i, j);
				const V G = gnd[k];
				const double tTop = m::la::dot(tp[k] - G, g.up);
				const double depthBot = std::max(1e-9, m::la::dot(G - bt[k], g.up));
				const double z1 = tTop >= 0.06 * T ? 0.03 * T : tTop - 0.03 * T;
				const double zr = l == 1 ? z1 : z1 - (z1 + depthBot) * double(l - 1) / double(nlv - 1);
				const double f = ClampD(-zr / std::max(1e-9, depthBot), 0.0, 1.0);
				const V P = G + g.up * zr;
				// broad slant and bulges (the face is rarely square to the top),
				// a few ledges, and a crumbled band under the top edge
				const double px = m::la::dot(P, g.ex), py = m::la::dot(P, g.ey);
				// facets several grid cells across, or the creases snap to the grid
				const double fs = std::max(1.1 * T, 4.0 * cell);
				const V qf = V(px / fs, py / fs, zr / (0.8 * T)) + F.off[3];
				const double nLow = LinFacet(cx.seed ^ F.key, qf);
				const double fm = std::max(0.6 * T, 3.0 * cell);
				const double nMid = LinFacet(cx.seed ^ F.key ^ 0x4dULL, V(px / fm, py / fm, zr / (0.35 * T)) + F.off[2]);
				const double nTop = LinFacet(cx.seed ^ F.key ^ 0x54ULL, V(px / fm, py / fm, 0.5) + F.off[1]);
				const double rimK = !socket && j == 0 ? 1.4 : 1.0;
				const double aLow = rough * T * 0.24 * std::pow(f, 0.7) * rimK;
				const double aMid = rough * T * 0.04 * Smooth(0.0, 0.25, f);
				const double aTop = rough * T * 0.035 * (1.0 - Smooth(0.0, 0.3, f));
				const double relax = 0.35 + 0.65 * Smooth(0.0, 0.2, 1.0 - f);
				const double amp = std::min(aLow + aMid + aTop, capA) * relax;
				const double nn = (aLow * nLow + aMid * nMid + aTop * nTop) / std::max(1e-12, aLow + aMid + aTop);
				double off = socket ? amp * nn : -amp * (1.0 - nn);
				if (!socket && j == 0) {
					// the rim face looks into the open chasm: it may bulge out as well
					// as in, and the whole break runs at a slant (a foot or an undercut)
					off = std::max(-capA, std::min(capA, 2.0 * amp * (nn - 0.45) + slant * rough * T * f));
				}
				return P + outward(gnd, i, j) * off;
			};
		};
		m::Manifold prism = v2 && rough > 0.0
			? RingSolid(top, bot, na, nb, nlv, faceMid(top, bot, false, 0.2 * std::min(minWide, sLen)))
			: SlabSolid(top, bot, na, nb);
		if (prism.IsEmpty()) return;
		const std::vector<V> pg = gnd;   // the slab's ground nodes (the socket grid replaces gnd)

		// pose: tilt about the hinge (back top edge), then a small roll
		const double env = EndEnv(cx, F, 0.5 * (J.s0 + J.s1));
		const double sideScale = ClampD(1.0 + (J.side ? 1.0 : -1.0) * ClampD(p.sideBias, -1.0, 1.0), 0.0, 2.0);
		double ang = 0.0, roll = 0.0;
		if (Random01(cx.seed, key, 2) >= ClampD(p.flatChance, 0.0, 1.0)) {
			const double tl = std::min(p.tiltMin, p.tiltMax), th = std::max(p.tiltMin, p.tiltMax);
			ang = Lerp(tl, th, Random01(cx.seed, key, 4)) * std::pow(fall, double(J.row)) * sideScale * Lerp(0.4, 1.0, env);
			if (Random01(cx.seed, key, 3) < ClampD(p.sinkChance, 0.0, 1.0)) ang = -0.6 * ang;
			roll = p.roll * (2.0 * Random01(cx.seed, key, 5) - 1.0) * std::pow(fall, double(J.row));
		}
		ang = ClampD(ang, -80.0, 80.0) * m::kPi / 180.0;
		roll = ClampD(roll, -45.0, 45.0) * m::kPi / 180.0;
		if (minThick < 1e299) {
			// on thin ground a dropped rim or a rolled end must not push out
			// through the underside
			const double budget = std::max(0.0, 0.9 * minThick - T);
			const double drop = (ang < 0.0 ? rowW * std::sin(-ang) : 0.0) + 0.5 * sLen * std::abs(std::sin(roll));
			if (drop > budget) {
				const double f = budget / drop;
				ang = ang < 0.0 ? -std::asin(ClampD(std::sin(-ang) * f, 0.0, 1.0)) : ang;
				roll = std::asin(ClampD(std::sin(roll) * f, -1.0, 1.0));
			}
		}
		const V pivot = (corners[0] + corners[1]) * 0.5;
		V axis = corners[1] - corners[0];
		axis = Unit(axis - g.up * m::la::dot(axis, g.up), g.ex);
		// positive angle must lift the rim side
		{
			const V r0 = corners[2] - pivot;
			if (m::la::dot(Rotate(r0, axis, 0.1) - r0, g.up) < 0.0) axis = -axis;
		}
		const V across = Unit(m::la::cross(g.up, axis), g.ey);
		auto rot = [&](const V& v) { return Rotate(Rotate(v, axis, ang), across, roll); };
		if (minThick < 1e299 && (ang != 0.0 || roll != 0.0)) {
			// exact check of the posed prism bottom against the underside
			// (lowest ground point under the slab minus its thinnest spot)
			double gLo = 1e300;
			for (const V& q : bot) gLo = std::min(gLo, m::la::dot(q, g.up));
			const double floorH = gLo + T - 0.92 * minThick;   // bot[] sits at ground - T here
			for (int it = 0; it < 6; ++it) {
				const V ax = axis, ac = Unit(m::la::cross(g.up, ax), g.ey);
				double lo = 1e300;
				for (const V& q : bot) lo = std::min(lo, m::la::dot(pivot + Rotate(Rotate(q - pivot, ax, ang), ac, roll), g.up));
				if (lo >= floorH) break;
				ang *= 0.7; roll *= 0.7;
				if (it == 5) { ang = 0.0; roll = 0.0; }
			}
		}
		// the socket is shallower than the slab by the embed: the slab's
		// bottom under the hinge stays below the socket floor (it rises by
		// T (1 - cos tilt) when tilted), so the union welds it in
		const double embed = std::min(0.6 * T, T * (0.12 + 1.0 - std::cos(ang)));
		grid(true, top, bot, H, T - embed, minThick, avoided, anyHit, nullptr);
		m::Manifold sock = v2 && rough > 0.0
			? RingSolid(top, bot, na, nb, nlv, faceMid(top, bot, true, 0.3 * rowW))
			: SlabSolid(top, bot, na, nb);
		if (sock.IsEmpty()) return;
		if (v2) {
			std::vector<m::Manifold>& cutsJ = slabCuts[size_t(ji)];
			const double ed = ClampD(p.edgeDamage, 0.0, 2.0);
			const double capS = 0.22 * std::min(minWide, sLen);
			std::vector<std::pair<int, int>> ring;
			for (int i = 0; i < na; ++i) ring.push_back({i, 0});
			for (int j = 0; j < nb; ++j) ring.push_back({na, j});
			for (int i = na; i > 0; --i) ring.push_back({i, nb});
			for (int j = nb; j > 0; --j) ring.push_back({0, j});
			const size_t R = ring.size();
			auto PG = [&](size_t r) { return pg[idx(ring[r].first, ring[r].second)]; };
			auto OUT = [&](size_t r) { return outward(pg, ring[r].first, ring[r].second); };
			if (ed > 0.0) {
				// chips along the top edge (and the bottom rim edge, seen when lifted)
				double acc = 0.0, next = T * Lerp(0.3, 1.5, Random01(cx.seed, key, 30));
				uint64_t q = 0;
				for (size_t r = 0; r < R && q < 4000; ++r) {
					const V a = PG(r), b = PG((r + 1) % R);
					const double sl = m::la::length(b - a);
					while (sl > 1e-12 && acc + sl >= next && q < 4000) {
						const double t = (next - acc) / sl;
						const V c = a + (b - a) * t;
						const V along = (b - a) / sl;
						const V in = -Unit(OUT(r) + OUT((r + 1) % R), V(0.0));
						const bool rim = ring[r].second == 0 && ring[(r + 1) % R].second == 0;
						const uint64_t ck = (key * 0x2545F4914F6CDD1DULL) ^ (0x43484950ULL + q);
						if (Random01(cx.seed, ck, 0) < std::min(0.95, 0.45 * ed * (rim ? 1.4 : 1.0))) {
							const double sz = std::min(capS, T * Lerp(0.15, 0.5, std::pow(Random01(cx.seed, ck, 1), 1.5)) * (0.6 + 0.25 * ed) * (rim ? 1.3 : 1.0));
							m::Manifold ch = ChipHull(cx.seed, ck, c, along, in, g.up, sz * Lerp(1.6, 3.6, Random01(cx.seed, ck, 2)),
								sz, sz * Lerp(0.4, 0.9, Random01(cx.seed, ck, 3)));
							if (!ch.IsEmpty()) cutsJ.push_back(std::move(ch));
						}
						if (rim && Random01(cx.seed, ck, 5) < 0.35 * ed) {
							const double sz = std::min(capS, T * Lerp(0.12, 0.3, Random01(cx.seed, ck, 6)));
							m::Manifold ch = ChipHull(cx.seed, ck ^ 0x42ULL, c - g.up * T, along, in, -g.up, sz * Lerp(1.3, 2.4, Random01(cx.seed, ck, 7)),
								sz, sz * Lerp(0.5, 1.0, Random01(cx.seed, ck, 8)));
							if (!ch.IsEmpty()) cutsJ.push_back(std::move(ch));
						}
						next += T * Lerp(0.7, 2.2, Random01(cx.seed, ck, 9));
						++q;
					}
					acc += sl;
				}
				// broken-off corners
				const int ci[4][2] = {{0, 0}, {na, 0}, {na, nb}, {0, nb}};
				for (int c4 = 0; c4 < 4; ++c4) {
					const uint64_t ck = (key * 0x2545F4914F6CDD1DULL) ^ (0x434f524eULL + uint64_t(c4));
					if (Random01(cx.seed, ck, 0) >= std::min(0.9, 0.5 * ed)) continue;
					const V c = pg[idx(ci[c4][0], ci[c4][1])];
					const V in = -outward(pg, ci[c4][0], ci[c4][1]);
					const V along = Unit(m::la::cross(g.up, in), g.ex);
					const double sz = std::min(1.2 * capS, T * Lerp(0.25, 0.5, Random01(cx.seed, ck, 1)) * (0.6 + 0.25 * ed));
					// often reaching well down the vertical edge
					m::Manifold ch = ChipHull(cx.seed, ck, c, along, in, g.up, sz * Lerp(1.2, 2.0, Random01(cx.seed, ck, 2)),
						1.2 * sz, std::min(T, sz * Lerp(0.8, 2.2, Random01(cx.seed, ck, 3))));
					if (!ch.IsEmpty()) cutsJ.push_back(std::move(ch));
				}
				// the intact ground behind the outer row crumbles too
				if (J.row + 1 == nRows) {
					double acc2 = 0.0, next2 = T * Lerp(0.2, 0.9, Random01(cx.seed, key, 31));
					uint64_t q2 = 0;
					for (int i = 0; i < na && q2 < 2000; ++i) {
						const V a = gnd[idx(i, nb)], b = gnd[idx(i + 1, nb)];
						const double sl = m::la::length(b - a);
						while (sl > 1e-12 && acc2 + sl >= next2 && q2 < 2000) {
							const double t = (next2 - acc2) / sl;
							const uint64_t ck = (key * 0x2545F4914F6CDD1DULL) ^ (0x47524e44ULL + q2);
							if (Random01(cx.seed, ck, 0) < 0.4 * ed) {
								const V in = Unit(outward(gnd, i, nb) + outward(gnd, i + 1, nb), V(0.0));
								const double sz = T * Lerp(0.08, 0.22, Random01(cx.seed, ck, 1)) * (0.6 + 0.25 * ed);
								m::Manifold ch = ChipHull(cx.seed, ck, a + (b - a) * t, (b - a) / sl, in, g.up,
									sz * Lerp(1.3, 2.6, Random01(cx.seed, ck, 2)), sz, sz * Lerp(0.5, 1.0, Random01(cx.seed, ck, 3)));
								if (!ch.IsEmpty()) groundChips[size_t(ji)].push_back(std::move(ch));
							}
							next2 += T * Lerp(0.4, 1.2, Random01(cx.seed, ck, 9));
							++q2;
						}
						acc2 += sl;
					}
				}
			}
			// hairline cracks across the slab top, from an edge into the slab
			const double scr = ClampD(p.surfaceCracks, 0.0, 3.0);
			const int nC = std::min(4, int(scr * Lerp(0.4, 1.6, Random01(cx.seed, key, 20)) + Random01(cx.seed, key, 21)));
			for (int c = 0; c < nC; ++c) {
				const uint64_t ck = (key * 0x2545F4914F6CDD1DULL) ^ (0x53435241ULL + uint64_t(c) * 97);
				const size_t r = std::min(R - 1, size_t(Random01(cx.seed, ck, 0) * double(R)));
				const V in = -OUT(r);
				const V S0 = PG(r) - in * (0.04 * T);
				const double base = std::atan2(m::la::dot(in, g.ey), m::la::dot(in, g.ex)) + 0.7 * (2.0 * Random01(cx.seed, ck, 1) - 1.0);
				const double len = Lerp(0.35, 0.8, Random01(cx.seed, ck, 2)) * std::min(minWide, sLen);
				const int n = std::max(6, std::min(80, int(len / (0.5 * cell))));
				const double st = len / n;
				const double w0 = std::max(0.45 * gap, 0.02 * T) * Lerp(0.7, 1.4, Random01(cx.seed, ck, 3));
				const double d0 = T * Lerp(0.25, 0.5, Random01(cx.seed, ck, 4));
				double x = m::la::dot(S0, g.ex), y = m::la::dot(S0, g.ey);
				std::vector<V> topv, botv;
				int rowsN = 0;
				for (int i = 0; i <= n; ++i) {
					const double f = double(i) / n;
					const double ang = base + 0.35 * Angular(cx.seed, ck ^ 1ULL, f * 4.0) + 0.15 * Angular(cx.seed, ck ^ 2ULL, f * 13.0);
					if (i > 0) { x += std::cos(ang) * st; y += std::sin(ang) * st; }
					double h, t;
					bool av;
					if (!g.Drop(x, y, h, t, av)) break;
					const V S = g.At(x, y, h);
					const V dirW = Unit(g.ex * std::cos(ang) + g.ey * std::sin(ang), g.ex);
					const V sd = Unit(m::la::cross(g.up, dirW), g.ey);
					const double hw = 0.5 * w0 * (1.0 - 0.8 * std::pow(f, 1.2)) + 1e-4;
					const double dep = d0 * (1.0 - 0.7 * f);
					const double mc = 0.25 * dep + 2.0 * hw;
					topv.push_back(S - sd * hw + g.up * mc); topv.push_back(S + g.up * mc); topv.push_back(S + sd * hw + g.up * mc);
					botv.push_back(S - sd * hw + g.up * (0.3 * mc)); botv.push_back(S - g.up * dep); botv.push_back(S + sd * hw + g.up * (0.3 * mc));
					++rowsN;
				}
				if (rowsN >= 3) {
					m::Manifold cr = SlabSolid(topv, botv, rowsN - 1, 2);
					if (!cr.IsEmpty()) cutsJ.push_back(std::move(cr));
				}
			}
			// corner breaks and shattered slabs: a jagged crack splits a
			// triangular piece off the slab, and that piece folds down a few
			// degrees about the bottom of the crack (it lost its support)
			if (Random01(cx.seed, key, 40) < ClampD(p.splitChance, 0.0, 1.0) && sLen > 0.25 * lMin) {
				const V cen = [&] { V c(0.0); for (const V& q : pg) c += q; return c / double(pg.size()); }();
				const double big = 0.6 * std::max(rowW, sLen) + 4.0 * T;
				auto at = [&](double fi, double fj) {
					// ground point at fractional grid coords (along one edge)
					const double ii = ClampD(fi, 0.0, 1.0) * na, jj = ClampD(fj, 0.0, 1.0) * nb;
					const int i0 = std::min(na - 1, int(ii)), j0 = std::min(nb - 1, int(jj));
					const double ti = ii - i0, tj = jj - j0;
					const V a = pg[idx(i0, j0)] * (1.0 - tj) + pg[idx(i0, j0 + 1)] * tj;
					const V b = pg[idx(i0 + 1, j0)] * (1.0 - tj) + pg[idx(i0 + 1, j0 + 1)] * tj;
					return a * (1.0 - ti) + b * ti;
				};
				auto ext = [&](const V& q) { return q + Unit(q - cen - g.up * m::la::dot(q - cen, g.up), g.ex) * big; };
				const bool rh = m::la::dot(m::la::cross(g.ex, g.ey), g.up) > 0.0;
				const V ey2 = rh ? g.ey : -g.ey;
				auto to2 = [&](const V& q) { return m::vec2(m::la::dot(q, g.ex), m::la::dot(q, ey2)); };
				double zLo = 1e300, zHi = -1e300;
				for (const V& q : pg) { zLo = std::min(zLo, m::la::dot(q, g.up)); zHi = std::max(zHi, m::la::dot(q, g.up)); }
				zLo -= 4.0 * T + rowW; zHi += 4.0 * T + rowW;
				const m::mat3x4 frame({g.ex, ey2, g.up, g.up * zLo});
				// one split: crack from P to Q, B-side corners in order from Q round to P
				auto addSplit = [&](uint64_t sk, const V& P, const V& Q, const std::vector<V>& bc, double dMin, double dMax) {
					const V chord = Q - P - g.up * m::la::dot(Q - P, g.up);
					const double len = m::la::length(chord);
					if (len < 2.0 * cell) return;
					const V dir = chord / len;
					V nB = m::la::cross(g.up, dir);   // toward the B side
					V bMid(0.0);
					for (const V& q : bc) bMid += q;
					bMid = bMid / double(bc.size());
					if (m::la::dot(bMid - P, nB) < 0.0) nB = -nB;
					// jagged crack in plan, pinned at its ends
					const int n = std::max(6, std::min(60, int(len / (0.6 * cell))));
					const double amp = (0.035 + 0.05 * jag) * len;
					std::vector<V> crack;
					for (int i = 0; i <= n; ++i) {
						const double f = double(i) / n;
						const double w = std::sqrt(std::max(0.0, std::sin(m::kPi * f)));
						const double o = amp * w * (0.7 * Angular(cx.seed, sk ^ 0x31ULL, f * 3.5) + 0.3 * Angular(cx.seed, sk ^ 0x32ULL, f * 11.0));
						crack.push_back(P + (Q - P) * f + nB * o);
					}
					// B region in plan: the crack, then round the B-side corners far outside
					auto poly = [&](double shift) {
						m::SimplePolygon sp;
						for (const V& q : crack) sp.push_back(to2(q - nB * shift));
						sp.push_back(to2(Q + dir * big - nB * shift));
						for (const V& q : bc) sp.push_back(to2(ext(q) + nB * big));
						sp.push_back(to2(P - dir * big - nB * shift));
						double ar = 0.0;
						for (size_t i = 0; i < sp.size(); ++i) { const m::vec2& a = sp[i]; const m::vec2& b = sp[(i + 1) % sp.size()]; ar += a.x * b.y - b.x * a.y; }
						if (ar < 0.0) std::reverse(sp.begin(), sp.end());
						return m::Manifold::Extrude(m::Polygons{sp}, zHi - zLo).Transform(frame);
					};
					Split S;
					S.cut = poly(-0.03 * T);   // off the groove's bottom edge
					// B reaches a little into A in the lower half only: the two weld at
					// the bottom, and the open top of the crack stays clean (a band
					// over the groove would close a pinhole void when B folds)
					{
						const double zTop = std::min(m::la::dot(P, g.up), m::la::dot(Q, g.up)) - 0.55 * T;
						S.keep = poly(0.0) + poly(0.08 * T).TrimByPlane(-g.up, -zTop);
					}
					if (S.cut.IsEmpty() || S.keep.IsEmpty()) return;
					// fold about the bottom of the crack, B side down
					double dl = Lerp(dMin, dMax, Random01(cx.seed, sk, 3)) * m::kPi / 180.0;
					double lB = 0.0;
					for (const V& q : bc) lB = std::max(lB, m::la::dot(q - P, nB));
					if (minThick < 1e299) {
						const double used = (ang < 0.0 ? rowW * std::sin(-ang) : 0.0) + 0.5 * sLen * std::abs(std::sin(roll));
						const double avail = 0.85 * minThick - (1.0 + 0.3 * rough) * T - used;   // the underside stays whole (bottom relief included)
						if (avail <= 0.0) dl = 0.0;
						else if (lB * std::sin(dl) > avail) dl = std::asin(ClampD(avail / std::max(1e-12, lB), 0.0, 1.0));
					}
					const V piv = P - g.up * T;
					V ax = dir;
					if (m::la::dot(Rotate(nB, ax, 0.1) - nB, g.up) > 0.0) ax = -ax;
					const V r0 = Rotate(V(1, 0, 0), ax, dl), r1 = Rotate(V(0, 1, 0), ax, dl), r2 = Rotate(V(0, 0, 1), ax, dl);
					S.fold = m::mat3x4({r0, r1, r2, piv - Rotate(piv, ax, dl)});
					splits[size_t(ji)].push_back(std::move(S));
					// the crack reads even where the piece barely moves
					const double w0 = std::max(0.7 * gap, 0.035 * T), d0 = 0.3 * T;
					std::vector<V> topv, botv;
					int rowsN = 0;
					for (const V& c : crack) {
						double h, t;
						bool av;
						if (!g.Drop(m::la::dot(c, g.ex), m::la::dot(c, g.ey), h, t, av)) continue;
						const V Sx = g.At(m::la::dot(c, g.ex), m::la::dot(c, g.ey), h);
						const double hw = 0.5 * w0, mc = 0.25 * d0 + 2.0 * hw;
						topv.push_back(Sx - nB * hw + g.up * mc); topv.push_back(Sx + g.up * mc); topv.push_back(Sx + nB * hw + g.up * mc);
						botv.push_back(Sx - nB * hw + g.up * (0.3 * mc)); botv.push_back(Sx - g.up * d0); botv.push_back(Sx + nB * hw + g.up * (0.3 * mc));
						++rowsN;
					}
					if (rowsN >= 3) {
						m::Manifold cr = SlabSolid(topv, botv, rowsN - 1, 2);
						if (!cr.IsEmpty()) slabCuts[size_t(ji)].push_back(std::move(cr));
					}
				};
				const uint64_t sk = (key * 0x2545F4914F6CDD1DULL) ^ 0x53504c54ULL;
				const double pick = Random01(cx.seed, sk, 0);
				const V c00 = pg[idx(0, 0)], cA0 = pg[idx(na, 0)], cAB = pg[idx(na, nb)], c0B = pg[idx(0, nb)];
				auto corner = [&](int which, uint64_t k2, double fMax) {
					const double f1 = Lerp(0.22, fMax, Random01(cx.seed, k2, 1));
					const double f2 = Lerp(0.35, 0.8, Random01(cx.seed, k2, 2));
					if (which == 0) addSplit(k2, at(f1, 0.0), at(0.0, f2), {c00}, 6.0, 16.0);
					else addSplit(k2, at(1.0 - f1, 0.0), at(1.0, f2), {cA0}, 6.0, 16.0);
				};
				if (pick < 0.45) corner(Random01(cx.seed, sk, 5) < 0.5 ? 0 : 1, sk ^ 0x11ULL, 0.6);
				else if (pick < 0.7) { corner(0, sk ^ 0x11ULL, 0.42); corner(1, sk ^ 0x22ULL, 0.42); }
				else if (Random01(cx.seed, sk, 5) < 0.5) {
					// rim corner to the far back corner: two triangles
					const double fb = Lerp(0.8, 1.0, Random01(cx.seed, sk, 6));
					if (fb > 0.99) addSplit(sk ^ 0x33ULL, c00, cAB, {cA0}, 3.0, 9.0);
					else addSplit(sk ^ 0x33ULL, c00, at(fb, 1.0), {cAB, cA0}, 3.0, 9.0);
				} else {
					const double fb = Lerp(0.0, 0.2, Random01(cx.seed, sk, 6));
					if (fb < 0.01) addSplit(sk ^ 0x33ULL, cA0, c0B, {c00}, 3.0, 9.0);
					else addSplit(sk ^ 0x33ULL, cA0, at(fb, 1.0), {c0B, c00}, 3.0, 9.0);
				}
			}
		}
		const V c0 = rot(V(1, 0, 0)), c1 = rot(V(0, 1, 0)), c2 = rot(V(0, 0, 1));
		const V tvec = pivot - rot(pivot);
		poses[size_t(ji)] = m::mat3x4({c0, c1, c2, tvec});
		prisms[size_t(ji)] = std::move(prism);
		sockets[size_t(ji)] = std::move(sock);
		live[size_t(ji)] = 1;
	});

	// the source's own pieces: clip the source to the stroke's slab region
	// once, then intersect that with every prism
	m::Box bb;
	bool any = false;
	for (size_t i = 0; i < jobs.size(); ++i) {
		if (!live[i]) continue;
		const m::Box b = prisms[i].BoundingBox();
		if (!any) { bb = b; any = true; }
		else bb = bb.Union(b);
	}
	if (!any) return;
	m::Manifold region;
	m::Box clip;
	const bool haveBox = cx.box && !cx.box->IsEmpty();
	if (haveBox) {
		// deep enough to hold the chasm floor and its rubble
		const V pad = (bb.max - bb.min) * 0.05 + V(std::max(tMax, 1.1 * F.D) + 1e-3);
		const V lo = bb.min - pad, size = bb.max - bb.min + pad * 2.0;
		clip = m::Box(lo, lo + size);
		region = *cx.box ^ m::Manifold::Cube(size).Translate(lo);
	}
	std::vector<m::Manifold> pieces(jobs.size());
	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t ji) {
		const size_t i = size_t(ji);
		if (!live[i]) return;
		m::Manifold piece = haveBox ? (region ^ prisms[i]) : prisms[i];
		if (piece.IsEmpty() || piece.Volume() <= 0.0) return;

		// a prism grazing the ground's edge can clip off specks: keep the slab
		const std::vector<m::Manifold> parts = piece.Decompose();
		if (parts.size() > 1) {
			size_t best = 0;
			double bv = -1.0;
			for (size_t q = 0; q < parts.size(); ++q) { const double v = parts[q].Volume(); if (v > bv) { bv = v; best = q; } }
			piece = parts[best];
		}
		if (!slabCuts[i].empty()) {
			// chips and hairline cracks; a chip can clip off a speck, keep the slab
			m::Manifold cut = piece - m::Manifold::BatchBoolean(slabCuts[i], m::OpType::Add);
			if (cut.Status() == m::Manifold::Error::NoError) {
				const std::vector<m::Manifold> cp = cut.Decompose();
				size_t best = 0;
				double bv = -1.0;
				for (size_t q = 0; q < cp.size(); ++q) { const double v = cp[q].Volume(); if (v > bv) { bv = v; best = q; } }
				if (!cp.empty() && bv > 0.5 * piece.Volume()) piece = cp[best];
			}
		}
		if (!splits[i].empty()) {
			// split pieces: A keeps the hinge, each B folds about its crack
			auto largest = [](const m::Manifold& mf) {
				const std::vector<m::Manifold> cp = mf.Decompose();
				size_t best = 0;
				double bv = -1.0;
				for (size_t q = 0; q < cp.size(); ++q) { const double v = cp[q].Volume(); if (v > bv) { bv = v; best = q; } }
				return cp.empty() ? m::Manifold() : cp[best];
			};
			const double v0 = piece.Volume();
			std::vector<m::Manifold> cuts;
			for (const Split& S : splits[i]) cuts.push_back(S.cut);
			m::Manifold A = largest(piece - m::Manifold::BatchBoolean(cuts, m::OpType::Add));
			if (!A.IsEmpty() && A.Volume() > 0.3 * v0) {
				std::vector<m::Manifold> parts{A.Transform(poses[i])};
				for (const Split& S : splits[i]) {
					m::Manifold B = largest(piece ^ S.keep);
					if (B.IsEmpty() || B.Volume() < 0.03 * v0) continue;
					B = B.Transform(S.fold);
					// it must still hold on to A (the crack can run off the ground's edge)
					if (!((B ^ A).Volume() > 1e-4 * B.Volume())) continue;
					parts.push_back(B.Transform(poses[i]));
				}
				// one solid, no inner shell (where B meets A at a slant the union
				// can close a pinhole void; dropping its shell fills it)
				pieces[i] = largest(m::Manifold::BatchBoolean(parts, m::OpType::Add));
				return;
			}
		}
		pieces[i] = piece.Transform(poses[i]);
	});
	for (size_t i = 0; i < jobs.size(); ++i) {
		if (!live[i] || pieces[i].IsEmpty()) continue;
		out.cutters.push_back(std::move(sockets[i]));
		for (auto& c : groundChips[i]) out.cutters.push_back(std::move(c));
		out.slabs.push_back(std::move(pieces[i]));
	}
	if (haveBox && !out.slabs.empty()) { out.region = std::move(region); out.clip = clip; }
}

bool Overlaps(const m::Box& a, const m::Box& b)
{
	return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y
		&& a.min.z <= b.max.z && b.min.z <= a.max.z;
}

// Rebuilds the slab area locally (the clipped source - every cutter and
// crack + every slab and chunk still in play) and checks it:
//  - floating: a positive component other than the ground that does not
//    reach the clip box is a piece that lost its hold (e.g. a slab whose
//    embed was cut away by a crossing fissure). The adds that make it are
//    listed, and the caller drops them: the slab has fallen out, its socket
//    stays open;
//  - pockets (when `fillers` is given): where a tilted or rolled slab
//    pinches its cracks shut, the space under it can end up sealed, a closed
//    air pocket inside the ground. Each one is filled with its convex hull,
//    pushed out a hair from its centre so the filler overlaps the pocket
//    walls instead of meeting them exactly. The final mesh then has no inner
//    shells. Pockets are small, flat and nearly convex.
// A flat pocket or speck grown only outward from its centre stays flat:
// the hull gets a little thickness in every direction too.
void Thicken(std::vector<V>& pts, double d)
{
	const size_t n = pts.size();
	for (size_t i = 0; i < n; ++i)
		for (int a = 0; a < 3; ++a) {
			V o(0.0);
			o[a] = d;
			pts.push_back(pts[i] + o);
			pts.push_back(pts[i] - o);
		}
}

void Settle(const m::Manifold& region, const m::Box& clip, const V& up, const std::vector<m::Manifold>& subs,
	const std::vector<m::Manifold>& adds, const std::vector<char>& dropped,
	std::vector<size_t>& floating, std::vector<m::Manifold>* fillers, std::vector<m::Manifold>* specks = nullptr,
	std::vector<std::pair<size_t, m::Manifold>>* addTrims = nullptr)
{
	const bool p_thicken = specks != nullptr;   // realistic only: classic stays as it was
	const m::Box rb = region.BoundingBox();
	std::vector<m::Manifold> cut, add;
	std::vector<size_t> addIdx;
	for (const m::Manifold& c : subs) if (Overlaps(rb, c.BoundingBox())) cut.push_back(c);
	add.push_back(cut.empty() ? region : region - m::Manifold::BatchBoolean(cut, m::OpType::Add));
	for (size_t i = 0; i < adds.size(); ++i)
		if (!dropped[i] && Overlaps(rb, adds[i].BoundingBox())) { add.push_back(adds[i]); addIdx.push_back(i); }
	const m::Manifold local = m::Manifold::BatchBoolean(add, m::OpType::Add);
	if (local.Status() != m::Manifold::Error::NoError) return;
	const std::vector<m::Manifold> comps = local.Decompose();
	if (comps.size() < 2) return;
	std::vector<double> vol(comps.size());
	size_t ground = 0;
	for (size_t c = 0; c < comps.size(); ++c) {
		vol[c] = comps[c].Volume();
		if (vol[c] > vol[ground]) ground = c;
	}
	const double diag = m::la::length(rb.max - rb.min);
	const double eps = 1e-4 * diag;
	for (size_t c = 0; c < comps.size(); ++c) {
		if (c == ground || !(vol[c] > 0.0)) continue;
		const m::Box cb = comps[c].BoundingBox();
		// ground cut off by the clip box's sides or bottom is joined outside it
		// (the box face toward `up` is in the air, so reaching it says nothing)
		bool touches = false;
		for (int a = 0; a < 3; ++a) {
			if (cb.min[a] <= clip.min[a] + eps && -up[a] < 0.5) touches = true;
			if (cb.max[a] >= clip.max[a] - eps && up[a] < 0.5) touches = true;
		}
		if (touches) continue;
		bool fromAdd = false;
		size_t owner = SIZE_MAX;
		for (size_t q = 0; q < addIdx.size(); ++q) {
			const m::Manifold& a = adds[addIdx[q]];
			if (!Overlaps(cb, a.BoundingBox())) continue;
			const double iv = (a ^ comps[c]).Volume();
			if (iv <= 0.0) continue;
			// a sliver hanging off a slab by a pinch (a chip almost cut it off):
			// trim the sliver, keep the slab
			if (specks && iv < 1e-3 * a.Volume()) { owner = addIdx[q]; continue; }
			if (iv > 1e-12 * diag * diag * diag) { floating.push_back(addIdx[q]); fromAdd = true; }
		}
		// a speck of ground left between rough cutters (or a sliver off a
		// slab): cut it away
		if (!fromAdd && specks) {
			const m::MeshGL64 mg = comps[c].GetMeshGL64();
			std::vector<V> pts;
			V cen(0.0);
			for (size_t v = 0; v < mg.NumVert(); ++v) {
				pts.push_back(V(mg.vertProperties[v * mg.numProp], mg.vertProperties[v * mg.numProp + 1], mg.vertProperties[v * mg.numProp + 2]));
				cen += pts.back();
			}
			if (pts.size() >= 4) {
				cen = cen / double(pts.size());
				const double grow = std::max(0.02, 1e-4 * diag);
				for (V& q : pts) q = q + Unit(q - cen, V(0.0)) * grow;
				if (p_thicken) Thicken(pts, grow);
				m::Manifold f = m::Manifold::Hull(pts);
				if (f.Status() == m::Manifold::Error::NoError && f.Volume() > 0.0) {
					if (owner != SIZE_MAX && addTrims) addTrims->push_back({owner, f});
					specks->push_back(std::move(f));
				}
			}
		}
	}
	if (!fillers) return;
	const double grow = std::max(0.01, 1e-5 * diag);
	for (size_t c = 0; c < comps.size(); ++c) {
		if (!(vol[c] < 0.0)) continue;
		const m::MeshGL64 mg = comps[c].GetMeshGL64();
		std::vector<V> pts;
		V cen(0.0);
		for (size_t v = 0; v < mg.NumVert(); ++v) {
			pts.push_back(V(mg.vertProperties[v * mg.numProp], mg.vertProperties[v * mg.numProp + 1], mg.vertProperties[v * mg.numProp + 2]));
			cen += pts.back();
		}
		if (pts.size() < 4) continue;
		cen = cen / double(pts.size());
		for (V& q : pts) q = q + Unit(q - cen, V(0.0)) * grow;
		if (p_thicken) Thicken(pts, grow);
		m::Manifold f = m::Manifold::Hull(pts);
		if (f.Status() == m::Manifold::Error::NoError && f.Volume() > 0.0) fillers->push_back(std::move(f));
	}
}

// --- side branches ---------------------------------------------------------------------

void BuildBranches(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const int count = int(std::round(std::max(0.0, p.branchDensity) * tr.len / 100.0));
	if (count <= 0) return;
	const double lMin = std::max(1e-3, std::min(p.branchLengthMin, p.branchLengthMax)) * F.sc;
	const double lMax = std::max(p.branchLengthMin, p.branchLengthMax) * F.sc;
	const uint64_t bk0 = (F.key << 24) ^ 0x4252414eULL;   // "BRAN"
	struct Seed { double x, y, ang, len, w, dep; uint64_t key; int gen; };
	std::vector<Seed> todo;
	for (int k = 0; k < count && k < 4096; ++k) {
		const uint64_t key = bk0 + uint64_t(k) * 64;
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed, key, ch); };
		const int side = rnd(0) < 0.5 ? 0 : 1;
		const double tl = ClampD(p.taper, 0.0, 0.5) * tr.len;
		const double s = Lerp(0.5 * tl, tr.len - 0.5 * tl, rnd(1));
		const double d = (side ? 1.0 : -1.0) * 0.8 * HalfW(cx, F, side, s);
		double X, Y, tx, ty;
		tr.Map(s, d, X, Y);
		tr.Tangent(s, tx, ty);
		// outward normal: right = (ty, -tx)
		const double ox = side ? ty : -ty, oy = side ? -tx : tx;
		const double dev = ClampD(p.branchAngle, 0.0, 85.0) * m::kPi / 180.0 * (2.0 * rnd(2) - 1.0);
		const double ang = std::atan2(oy, ox) + dev;
		const double env = EndEnv(cx, F, s);
		todo.push_back({X, Y, ang, Lerp(lMin, lMax, rnd(3)) * Lerp(0.5, 1.0, env),
			std::max(0.0, p.branchWidth) * F.sc * Lerp(0.6, 1.4, rnd(4)),
			std::max(0.0, p.branchDepth) * F.sc * Lerp(0.6, 1.2, rnd(5)), key, 0});
	}
	for (size_t ti = 0; ti < todo.size(); ++ti) {
		const Seed sd = todo[ti];
		if (sd.w <= 0.0 || sd.dep <= 0.0 || sd.len <= 0.0) continue;
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed, sd.key, ch); };
		const double step = std::max(std::max(1.2 * sd.w, sd.len / 60.0), sd.len / 400.0) / std::max(0.05, p.detail);
		const int ns = std::max(2, int(std::ceil(sd.len / step)));
		const V noff(rnd(6) * 1000.0, rnd(7) * 1000.0, 0.0);
		std::vector<V> topv, botv;
		double ang = sd.ang, x = sd.x, y = sd.y;
		int rowsN = 0;
		const double mean = ClampD(p.branchMeander, 0.0, 2.0);
		const double fork = Lerp(0.3, 0.7, rnd(8));
		bool forked = false;
		for (int i = 0; i <= ns; ++i) {
			const double f = double(i) / ns;
			if (i > 0) {
				ang += mean * 0.9 * Angular(cx.seed, sd.key ^ 0x57414cULL, f * sd.len / (0.12 * sd.len + 1e-6)) * (step / (0.12 * sd.len + 1e-6))
					+ mean * 0.6 * Fbm(V(f * 3.0, 0.5, 0.0) + noff, 2) * (step / (0.3 * sd.len + 1e-6));
				x += std::cos(ang) * step;
				y += std::sin(ang) * step;
			}
			double h, t;
			bool av;
			if (!g.Drop(x, y, h, t, av) || av) break;
			const V S = g.At(x, y, h);
			const V dirW = Unit(g.ex * std::cos(ang) + g.ey * std::sin(ang), g.ex);
			const V side = Unit(m::la::cross(g.up, dirW), g.ey);
			const double taper = 1.0 - 0.9 * std::pow(f, 1.3);
			const double hw = 0.5 * sd.w * taper + 1e-4;
			double dep = sd.dep * (1.0 - 0.8 * std::pow(f, 1.5));
			if (t < 1e299) dep = std::min(dep, 0.5 * t);
			const double mc = 0.25 * sd.dep + 2.0 * hw;
			topv.push_back(S - side * hw + g.up * mc); topv.push_back(S + g.up * mc); topv.push_back(S + side * hw + g.up * mc);
			botv.push_back(S - side * hw + g.up * (0.3 * mc)); botv.push_back(S - g.up * dep); botv.push_back(S + side * hw + g.up * (0.3 * mc));
			++rowsN;
			if (!forked && sd.gen < 2 && i > 0 && i < ns && f >= fork && rnd(9) < ClampD(p.branchFork, 0.0, 1.0)) {
				forked = true;
				const double sg = rnd(10) < 0.5 ? -1.0 : 1.0;
				todo.push_back({x, y, ang + sg * Lerp(0.4, 0.9, rnd(11)), sd.len * (1.0 - f) * Lerp(0.5, 0.9, rnd(12)),
					sd.w * taper * 0.75, sd.dep * 0.8, sd.key * 31 + 7, sd.gen + 1});
			}
		}
		if (rowsN >= 3) {
			m::Manifold c = SlabSolid(topv, botv, rowsN - 1, 2);
			if (!c.IsEmpty()) out.cracks.push_back(std::move(c));
		}
	}
}

// --- rubble ----------------------------------------------------------------------------

void BuildRubble(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const int count = int(std::round(std::max(0.0, p.rubbleDensity) * tr.len / 100.0));
	const double fe = Lerp(0.85, 0.55, ClampD(p.floorShape, 0.0, 1.0));
	const double cap = ClampD(p.thicknessCap, 0.0, 0.98);
	const uint64_t rk0 = (F.key << 24) ^ 0x5255424cULL;   // "RUBL"
	for (int k = 0; k < count && k < 4096; ++k) {
		const uint64_t key = rk0 + uint64_t(k) * 64;
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed, key, ch); };
		const double tl = ClampD(p.taper, 0.0, 0.5) * tr.len;
		const double s = Lerp(0.6 * tl, tr.len - 0.6 * tl, rnd(0));
		const double wl = HalfW(cx, F, 0, s), wr = HalfW(cx, F, 1, s);
		const bool wedged = rnd(1) < ClampD(p.rubbleWedged, 0.0, 1.0);
		double hc, tc;
		bool av;
		double Xc, Yc;
		// wedged chunks sit midway between the rims, floor chunks on the centre line
		tr.Map(s, wedged ? 0.5 * (wr - wl) : 0.0, Xc, Yc);
		// ground height at the two rims (the chasm centre is not cut yet,
		// but the rims are the reference the walls hang from)
		double X0, Y0, X1, Y1, h0 = 0.0, h1 = 0.0, t0, t1;
		bool a0, a1;
		tr.Map(s, -wl, X0, Y0);
		tr.Map(s, wr, X1, Y1);
		const bool ok0 = g.Drop(X0, Y0, h0, t0, a0), ok1 = g.Drop(X1, Y1, h1, t1, a1);
		if (!ok0 && !ok1) continue;
		if (!g.Drop(Xc, Yc, hc, tc, av)) continue;   // past the ground's edge
		if (av || (ok0 && a0) || (ok1 && a1)) continue;
		const double h = std::min(ok0 ? h0 : h1, ok1 ? h1 : h0);
		double Dc = DepthAt(cx, F, s);
		double floorDep = ChasmDepth(cx, F, s, 0.0, Dc);
		if (cap > 0.0 && tc < 1e299) { Dc = std::min(Dc, cap * tc); floorDep = std::min(floorDep, cap * tc); }
		const double smin = std::min(p.rubbleSizeMin, p.rubbleSizeMax), smax = std::max(p.rubbleSizeMin, p.rubbleSizeMax);
		const double full = wl + wr;
		double z, across;
		if (wedged) {
			// jammed: spans the gap between the leaning walls at its depth
			const double f = Lerp(0.15, 0.6, rnd(3));
			z = h - f * Dc * fe;
			const double ll = Lean(cx, F, 0, s), lr = Lean(cx, F, 1, s);
			const double lean = std::max(0.0, std::max(ll, lr));
			across = full * (1.0 + lean * f) * Lerp(1.08, 1.3, rnd(2));
		} else {
			across = full * Lerp(std::max(0.05, smin), std::max(0.05, smax), rnd(2)) * 0.6;
			z = 0.0;   // set below, once the height is known
		}
		if (!(across > 0.0)) continue;
		double tx, ty;
		tr.Tangent(s, tx, ty);
		const V along = Unit(g.ex * tx + g.ey * ty, g.ex);
		const V side = Unit(m::la::cross(g.up, along), g.ey);
		const double sAlong = across * Lerp(0.5, 1.2, rnd(4));
		const double sUp = across * Lerp(0.35, 0.7, rnd(5));
		if (wedged) z = std::min(z, h - 0.6 * sUp - 0.05 * Dc);   // keep the chunk below the rim
		else z = hc - floorDep + 0.15 * sUp;                        // a third sunk into the floor
		const V C = g.At(Xc, Yc, z);
		std::vector<V> pts;
		const bool v2 = p.realistic;
		if (v2) {
			// a broken block, not a pebble: jittered box corners, sheared below
			for (int q = 0; q < 8; ++q) {
				const double fx = (q & 1) ? 0.5 : -0.5, fy = (q & 2) ? 0.5 : -0.5, fz = (q & 4) ? 0.5 : -0.5;
				pts.push_back(C + along * (fx * sAlong * Lerp(0.7, 1.15, rnd(60 + uint64_t(q) * 3)))
					+ side * (fy * across * Lerp(wedged ? 0.92 : 0.7, 1.1, rnd(61 + uint64_t(q) * 3)))
					+ g.up * (fz * sUp * Lerp(0.6, 1.2, rnd(62 + uint64_t(q) * 3))));
			}
		}
		for (int q = 0; q < (v2 ? 0 : 12); ++q) {
			const double a = 2.0 * rnd(10 + uint64_t(q) * 3) - 1.0;
			const double b = 2.0 * rnd(11 + uint64_t(q) * 3) - 1.0;
			const double c = 2.0 * rnd(12 + uint64_t(q) * 3) - 1.0;
			pts.push_back(C + along * (0.5 * sAlong * a) + side * (0.5 * across * b) + g.up * (0.5 * sUp * c));
		}
		// the two wall contacts, so a wedged chunk always spans the gap
		if (wedged) {
			pts.push_back(C + side * (0.5 * across));
			pts.push_back(C - side * (0.5 * across));
		} else {
			pts.push_back(C - g.up * (0.5 * sUp));
		}
		const double tilt = (2.0 * rnd(6) - 1.0) * 0.5;
		for (V& q : pts) q = C + Rotate(q - C, along, tilt);
		// never poke out of the ground's underside (thin ground) or over the rim
		{
			const double zc = m::la::dot(C, g.up);
			const double floorLim = tc < 1e299 ? hc - 0.92 * tc : -1e300;
			const double topLim = h - 0.02 * Dc;
			double lo = 0.0, hi = 0.0;
			for (const V& q : pts) { const double dz = m::la::dot(q, g.up) - zc; lo = std::min(lo, dz); hi = std::max(hi, dz); }
			double f = 1.0;
			if (lo < 0.0 && zc + lo < floorLim) f = std::min(f, (zc - floorLim) / -lo);
			if (hi > 0.0 && zc + hi > topLim) f = std::min(f, (topLim - zc) / hi);
			if (f < 0.35) continue;
			if (f < 1.0) for (V& q : pts) q = C + (q - C) * f;
		}
		m::Manifold chunk = m::Manifold::Hull(pts);
		if (v2 && !chunk.IsEmpty()) {
			double sc = 1.0;
			{
				const V c0 = pts[0], c7 = pts[7];
				sc = m::la::length(c7 - c0) / std::max(1e-9, std::sqrt(sAlong * sAlong + across * across + sUp * sUp));
			}
			const V tAlong = Rotate(along, along, tilt), tUp = Unit(Rotate(g.up, along, tilt), g.up), tSide = Unit(m::la::cross(tUp, tAlong), side);
			chunk = ShearChunk(cx.seed, key, chunk, C, tAlong, tSide, tUp, sAlong * sc, across * sc, sUp * sc, wedged ? 0.2 : 1.0, 5);
		}
		if (!chunk.IsEmpty() && chunk.Volume() > 0.0) out.slabs.push_back(std::move(chunk));
	}
}

// Small broken fragments (style 1): angular bits of concrete scattered on
// the chasm floor and on the ground along the rims, half sunk so they weld
// into the same mesh. Sizes follow the slab thickness (the chips they came
// from), counts follow the fissure length.
// основной вход: собираем геометрию по переданным настройкам
void BuildDebris(const Ctx& cx, const Fissure& F, Out& out)
{
	const GroundFissureParams& p = *cx.p;
	const Ground& g = *cx.g;
	const Track& tr = F.tr;
	const double dens = ClampD(p.debris, 0.0, 3.0);
	if (dens <= 0.0) return;
	const double szMul = ClampD(p.debrisSize, 0.25, 3.0);
	const double tMid = 0.5 * (std::max(1e-3, p.slabThickMin) + std::max(1e-3, p.slabThickMax)) * F.sc;
	const double base = std::min(0.35 * tMid, 0.12 * F.W) * szMul;
	const double cap = ClampD(p.thicknessCap, 0.0, 0.98);
	const int count = std::min(3000, int(std::round(dens * 5.0 * tr.len / F.W)));
	const uint64_t dk0 = (F.key << 24) ^ 0x44425253ULL;   // "DBRS"
	// how far the slab rows reach on each side (same row widths BuildSlabs draws)
	const int nRows = p.slabEnable ? std::max(1, std::min(p.slabRows, 3)) : 0;
	const double fall = ClampD(p.slabRowFalloff, 0.05, 1.5);
	const double wMin = std::max(1e-3, std::min(p.slabWidthMin, p.slabWidthMax)) * F.sc;
	const double wMax = std::max(p.slabWidthMin, p.slabWidthMax) * F.sc;
	double reach[2] = {0.0, 0.0};
	for (int side = 0; side < 2; ++side)
		for (int r = 0; r < nRows; ++r)
			reach[side] += 1.15 * Lerp(wMin, wMax, Random01(cx.seed, SlabKey(F, side, r, 0xfffff), 0)) * std::pow(fall, double(r));
	for (int k = 0; k < count; ++k) {
		const uint64_t key = dk0 + uint64_t(k) * 64;
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed, key, ch); };
		const double s = Lerp(0.0, tr.len, rnd(0));
		const double env = EndEnv(cx, F, s);
		const bool floor = rnd(1) < 0.55;
		const int side = rnd(2) < 0.5 ? 0 : 1;
		const double sgn = side ? 1.0 : -1.0;
		const double hw = HalfW(cx, F, side, s);
		double d;
		double size = base * Lerp(0.35, 1.0, std::pow(rnd(3), 1.8));
		if (floor) {
			if (env < 0.3) continue;
			d = sgn * hw * Lerp(0.0, 0.65, rnd(4));
			size = std::min(size * 1.4, 0.45 * hw * 2.0);
		} else {
			// just past the slab rows (or the rim where there are none), thinning out
			const bool slabsHere = nRows > 0 && env >= 0.5;
			d = sgn * (hw + (slabsHere ? reach[side] : 0.0) + base * Lerp(0.1, 2.2, std::pow(rnd(4), 2.0)));
		}
		if (std::abs(d) > 0.92 * tr.Limit(side, s)) continue;
		double X, Y, h, t;
		bool av;
		tr.Map(s, d, X, Y);
		if (!g.Drop(X, Y, h, t, av) || av) continue;
		double z;
		const double sUp = size * Lerp(0.45, 0.8, rnd(5));
		if (floor) {
			const double Dc = DepthAt(cx, F, s);
			const double u = d / std::max(1e-9, hw) * sgn;
			double dep = ChasmDepth(cx, F, s, u * sgn, Dc);
			if (cap > 0.0 && t < 1e299) dep = std::min(dep, cap * t);
			z = h - dep + 0.15 * sUp;   // sunk into the rough floor
		} else {
			z = h - 0.3 * sUp;
		}
		double sx = size * Lerp(0.8, 1.4, rnd(8)), sy = size * Lerp(0.6, 1.0, rnd(9)), sz = sUp;
		// thin ground: never poke out of the underside
		if (t < 1e299) {
			const double floorH = h - 0.92 * t;
			auto low = [&](double k) { return z - k * (0.6 * sz + 0.5 * std::max(sx, sy) * 0.4); };
			double k = 1.0;
			while (k > 0.45 && low(k) < floorH) k *= 0.8;
			if (low(k) < floorH) continue;
			if (k < 1.0) {
				// shrink about the surface contact so the fragment stays seated
				const double top = z + 0.5 * sz;
				sx *= k; sy *= k; sz *= k;
				z = top - 0.5 * sz;
			}
		}
		double tx, ty;
		tr.Tangent(s, tx, ty);
		const double yaw = rnd(6) * 2.0 * m::kPi;
		const V a0 = Unit(g.ex * tx + g.ey * ty, g.ex);
		const V a = Rotate(a0, g.up, yaw);
		const V b = Unit(m::la::cross(g.up, a), g.ey);
		const V c = Rotate(g.up, a, (2.0 * rnd(7) - 1.0) * 0.35);
		m::Manifold ch = FracturedChunk(cx.seed, key, g.At(X, Y, z), a, Rotate(b, a, (2.0 * rnd(7) - 1.0) * 0.35), c,
			sx, sy, sz, 1.0, 4);
		if (!ch.IsEmpty()) out.slabs.push_back(std::move(ch));
	}
}

}   // namespace

void PlaceGroundFissures(const CoreInput& in, const GroundFissureParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& slabs)
{
	if (!pIn.enable) return;
	if (!paintStrokes || paintStrokes->empty()) return;
	const MeshView& v = in.view;
	if (v.npts <= 0 || v.npolys <= 0 || !v.pts || !v.polys) return;
	GroundFissureParams p = pIn;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.width = std::max(1e-4, p.width);
	p.depth = std::max(1e-4, p.depth);

	Ground g;
	BuildGround(in, p, g);
	Ctx cx;
	cx.p = &p;
	cx.g = &g;
	cx.box = in.box;
	cx.seed = (uint64_t(uint32_t(mainSeed)) ^ 0x4649535355524553ULL)     // "FISSURES"
		^ (uint64_t(uint32_t(p.seed)) * 0x9e3779b97f4a7c15ULL);
	const int32_t kStream = 0x4746;   // mask hash stream ("GF")

	std::vector<Fissure> fis;
	for (size_t si = 0; si < paintStrokes->size(); ++si) {
		std::vector<V> pts;
		for (const EVec& q : (*paintStrokes)[si])
			if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)) pts.push_back(q);
		if (pts.size() < 2) continue;
		const V mid = pts[pts.size() / 2];
		if (mask && !mask->Keep(mid, int32_t(si), kStream)) continue;
		const double sc = mask ? mask->SizeScale(mid) : 1.0;
		if (sc <= 0.0) continue;
		Fissure F;
		F.key = uint64_t(si) + 1;
		F.sc = sc;
		auto rnd = [&](uint64_t ch) { return Random01(cx.seed, F.key, ch); };
		for (int o = 0; o < 4; ++o)
			F.off[o] = V(rnd(uint64_t(o) * 2) * 1000.0, rnd(uint64_t(o) * 2 + 1) * 1000.0, 0.0);
		F.W = p.width * sc * Lerp(0.9, 1.1, rnd(8));
		F.D = p.depth * sc * Lerp(0.9, 1.1, rnd(9));
		const int rowsN = p.slabEnable ? std::max(1, std::min(p.slabRows, 3)) : 0;
		const double reach = 0.6 * F.W + rowsN * std::max(p.slabWidthMin, p.slabWidthMax) * sc;
		if (!BuildTrack(g, pts, std::max(1e-3, F.W / 8.0), reach, F.tr)) continue;
		fis.push_back(std::move(F));
	}
	if (fis.empty()) return;

	// one job per (fissure, part): chasm, slabs, branches, rubble
	std::vector<Out> outs(fis.size() * 4);
	ProjectDamageParallelFor(0, int64_t(outs.size()), [&](int64_t i) {
		const Fissure& F = fis[size_t(i) / 4];
		Out& o = outs[size_t(i)];
		switch (int(i % 4)) {
		case 0: if (p.realistic) BuildChasmV2(cx, F, o); else BuildChasm(cx, F, o); break;
		case 1: if (p.slabEnable) BuildSlabs(cx, F, o); break;
		case 2: if (p.branchEnable) BuildBranches(cx, F, o); break;
		case 3:
			if (p.rubbleEnable) BuildRubble(cx, F, o);
			if (p.realistic) BuildDebris(cx, F, o);
			break;
		}
	});
	std::vector<m::Manifold> subs, adds;
	for (const Out& o : outs) {
		for (const auto& c : o.cutters) subs.push_back(c);
		for (const auto& c : o.cracks) subs.push_back(c);
		for (const auto& a : o.slabs) adds.push_back(a);
	}
	// settle: drop pieces that lost their hold (repeat: a slab may hang
	// only on one that fell), then fill sealed pockets
	std::vector<char> dropped(adds.size(), 0);
	std::vector<std::vector<m::Manifold>> fill(fis.size()), speck(fis.size());
	std::vector<std::vector<std::pair<size_t, m::Manifold>>> trims(fis.size());
	if (p.realistic) {
		// one look per pass finds the fallen pieces, pockets and specks
		// together; the pass that drops nothing more is the final one
		for (int pass = 0; pass < 4; ++pass) {
			std::vector<std::vector<size_t>> fl(fis.size());
			for (size_t f = 0; f < fis.size(); ++f) { fill[f].clear(); speck[f].clear(); trims[f].clear(); }
			ProjectDamageParallelFor(0, int64_t(fis.size()), [&](int64_t f) {
				const Out& o = outs[size_t(f) * 4 + 1];
				if (!o.region.IsEmpty()) Settle(o.region, o.clip, g.up, subs, adds, dropped, fl[size_t(f)], &fill[size_t(f)],
					&speck[size_t(f)], &trims[size_t(f)]);
			});
			if (pass == 3) break;
			bool any = false;
			for (const auto& l : fl) for (size_t i : l) if (!dropped[i]) { dropped[i] = 1; any = true; }
			if (!any) break;
		}
	} else {
		for (int pass = 0; pass < 3; ++pass) {
			std::vector<std::vector<size_t>> fl(fis.size());
			ProjectDamageParallelFor(0, int64_t(fis.size()), [&](int64_t f) {
				const Out& o = outs[size_t(f) * 4 + 1];
				if (!o.region.IsEmpty()) Settle(o.region, o.clip, g.up, subs, adds, dropped, fl[size_t(f)], nullptr);
			});
			bool any = false;
			for (const auto& l : fl) for (size_t i : l) if (!dropped[i]) { dropped[i] = 1; any = true; }
			if (!any) break;
		}
		ProjectDamageParallelFor(0, int64_t(fis.size()), [&](int64_t f) {
			const Out& o = outs[size_t(f) * 4 + 1];
			std::vector<size_t> unused;
			if (!o.region.IsEmpty()) Settle(o.region, o.clip, g.up, subs, adds, dropped, unused, &fill[size_t(f)],
				p.realistic ? &speck[size_t(f)] : nullptr, p.realistic ? &trims[size_t(f)] : nullptr);
		});
	}
	for (auto& tl : trims)
		for (auto& t : tl) {
			if (t.first >= adds.size() || dropped[t.first]) continue;
			const m::Manifold cut = adds[t.first] - t.second;
			if (cut.Status() != m::Manifold::Error::NoError) continue;
			const std::vector<m::Manifold> cp = cut.Decompose();
			size_t best = 0;
			double bv = -1.0;
			for (size_t q = 0; q < cp.size(); ++q) { const double v = cp[q].Volume(); if (v > bv) { bv = v; best = q; } }
			if (!cp.empty() && bv > 0.9 * adds[t.first].Volume()) adds[t.first] = cp[best];
		}
	if (p.realistic) {
		// second look: the fills and trims above (and the split slab pieces
		// meeting at slants) can leave a last speck or a pinhole pocket
		std::vector<m::Manifold> subs2 = subs, adds2;
		for (const auto& sp : speck) for (const auto& c : sp) subs2.push_back(c);
		for (size_t i = 0; i < adds.size(); ++i) if (!dropped[i]) adds2.push_back(adds[i]);
		for (const auto& f : fill) for (const auto& c : f) adds2.push_back(c);
		const std::vector<char> none(adds2.size(), 0);
		std::vector<std::vector<m::Manifold>> fill2(fis.size()), speck2(fis.size());
		ProjectDamageParallelFor(0, int64_t(fis.size()), [&](int64_t f) {
			const Out& o = outs[size_t(f) * 4 + 1];
			std::vector<size_t> unused;
			// only where the first look had something to fix
			if (fill[size_t(f)].empty() && speck[size_t(f)].empty()) return;
			if (!o.region.IsEmpty()) Settle(o.region, o.clip, g.up, subs2, adds2, none, unused, &fill2[size_t(f)], &speck2[size_t(f)], nullptr);
		});
		for (size_t f = 0; f < fis.size(); ++f) {
			for (auto& c : fill2[f]) fill[f].push_back(std::move(c));
			for (auto& c : speck2[f]) speck[f].push_back(std::move(c));
		}
	}
	for (auto& sp : speck)
		for (auto& c : sp) cutters.push_back(std::move(c));
	for (Out& o : outs) {
		for (auto& c : o.cutters) cutters.push_back(std::move(c));
		for (auto& c : o.cracks) cracks.push_back(std::move(c));
	}
    // сохраняем разлом, но не выдаём наклонённые плиты и обломки
    if (!p.generatedGeometry) return;
	for (size_t i = 0; i < adds.size(); ++i)
		if (!dropped[i]) slabs.push_back(std::move(adds[i]));
	for (auto& f : fill)
		for (auto& s : f) slabs.push_back(std::move(s));
}
PROJECT_DAMAGE_NS_MACRO_END
