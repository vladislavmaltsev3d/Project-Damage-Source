// общие объявления: общие размеры, меши и случайность для новых генераторов
// ProjectDamage damage generators: shared helpers - GPL-3.0-or-later; see LICENSE.
//
// Header-only toolkit for the standalone damage generators (metal_tear,
// wood_split, tile_damage, erosion, blast_crater). It needs only the
// standard library: no Manifold, no Clipper, no project_damage_core.h, so a
// generator compiles on its own for tests and inside the core unchanged.
//
//   V3 / V2        small vector types
//   Hash / Rng     SplitMix64 counter hashing (same seed = same result)
//   Noise2/3, Fbm  gradient noise, fractal sums, ridged noise, Worley cells
//   GenMesh        the result every generator returns (points, polygons,
//                  material per face, UV per face corner, float attributes)
//   MeshDomain2D   a 2D region { f(u,v) <= 0 } meshed as a grid clipped to
//                  the contour (the torn border follows f exactly)
//   SolidifySheet  a deformed 2D domain -> closed shell with walls
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdgen {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// vectors
struct V2
{
	double x = 0, y = 0;
	V2() = default;
	V2(double a, double b) : x(a), y(b) {}
	V2 operator+(const V2& o) const { return { x + o.x, y + o.y }; }
	V2 operator-(const V2& o) const { return { x - o.x, y - o.y }; }
	V2 operator*(double s) const { return { x * s, y * s }; }
};
inline double Dot(const V2& a, const V2& b) { return a.x * b.x + a.y * b.y; }
inline double Len(const V2& a) { return std::sqrt(a.x * a.x + a.y * a.y); }

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
	V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
	V3 operator*(double s) const { return { x * s, y * s, z * s }; }
	V3 operator-() const { return { -x, -y, -z }; }
	V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(const V3& a, const V3& fallback = V3(0, 0, 1))
{
	const double l = Len(a);
	return l > 1e-300 ? a * (1.0 / l) : fallback;
}

inline double Clamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
inline double Sat(double v) { return Clamp(v, 0.0, 1.0); }
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double Smooth(double t) { t = Sat(t); return t * t * (3.0 - 2.0 * t); }
inline double SmoothStep(double e0, double e1, double x) { return e1 == e0 ? (x >= e1 ? 1.0 : 0.0) : Smooth((x - e0) / (e1 - e0)); }
// smooth minimum (polynomial), k = blend width
inline double SMin(double a, double b, double k)
{
	if (k <= 0.0) return std::min(a, b);
	const double h = Sat(0.5 + 0.5 * (b - a) / k);
	return Lerp(b, a, h) - k * h * (1.0 - h);
}
inline double SMax(double a, double b, double k) { return -SMin(-a, -b, k); }

// ---------------------------------------------------------------------------
// hashing
inline uint64_t Mix64(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}
inline uint64_t Hash(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0)
{
	return Mix64(a ^ Mix64(b ^ Mix64(c ^ Mix64(d))));
}
inline double Hash01(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0)
{
	return double(Hash(a, b, c, d) >> 11) * (1.0 / 9007199254740992.0);
}

// A counter RNG: every draw is Hash(seed, channel, counter++).
struct Rng
{
	uint64_t seed, channel, n = 0;
	Rng(uint64_t s, uint64_t ch) : seed(s), channel(ch) {}
	double Next() { return Hash01(seed, channel, n++); }
	double Range(double a, double b) { return a + (b - a) * Next(); }
	int Int(int a, int b) { return b <= a ? a : a + int(std::min<double>(b - a, std::floor(Next() * (b - a + 1)))); }
	bool Chance(double p) { return Next() < p; }
	double Signed() { return Next() * 2.0 - 1.0; }
	double Normal() // Box-Muller
	{
		const double u = std::max(Next(), 1e-12), v = Next();
		return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
	}
};

// ---------------------------------------------------------------------------
// gradient noise (-1..1, roughly), quintic fade
namespace detail {
inline double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
inline double Grad2(uint64_t seed, int64_t ix, int64_t iy, double dx, double dy)
{
	const double a = Hash01(seed, uint64_t(ix), uint64_t(iy), 0x51) * 2.0 * kPi;
	return std::cos(a) * dx + std::sin(a) * dy;
}
inline double Grad3(uint64_t seed, int64_t ix, int64_t iy, int64_t iz, double dx, double dy, double dz)
{
	static const double g[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
		{ 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
	const double* v = g[Hash(seed, uint64_t(ix), uint64_t(iy), uint64_t(iz)) % 12];
	return v[0] * dx + v[1] * dy + v[2] * dz;
}
} // namespace detail

inline double Noise2(double x, double y, uint64_t seed)
{
	const double fx = std::floor(x), fy = std::floor(y);
	const int64_t ix = int64_t(fx), iy = int64_t(fy);
	const double dx = x - fx, dy = y - fy;
	const double u = detail::Fade(dx), v = detail::Fade(dy);
	const double n00 = detail::Grad2(seed, ix, iy, dx, dy), n10 = detail::Grad2(seed, ix + 1, iy, dx - 1, dy);
	const double n01 = detail::Grad2(seed, ix, iy + 1, dx, dy - 1), n11 = detail::Grad2(seed, ix + 1, iy + 1, dx - 1, dy - 1);
	return 1.4142 * Lerp(Lerp(n00, n10, u), Lerp(n01, n11, u), v);
}

inline double Noise3(double x, double y, double z, uint64_t seed)
{
	const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
	const int64_t ix = int64_t(fx), iy = int64_t(fy), iz = int64_t(fz);
	const double dx = x - fx, dy = y - fy, dz = z - fz;
	const double u = detail::Fade(dx), v = detail::Fade(dy), w = detail::Fade(dz);
	double c[2][2][2];
	for (int a = 0; a < 2; ++a)
		for (int b = 0; b < 2; ++b)
			for (int e = 0; e < 2; ++e)
				c[a][b][e] = detail::Grad3(seed, ix + a, iy + b, iz + e, dx - a, dy - b, dz - e);
	const double x00 = Lerp(c[0][0][0], c[1][0][0], u), x10 = Lerp(c[0][1][0], c[1][1][0], u);
	const double x01 = Lerp(c[0][0][1], c[1][0][1], u), x11 = Lerp(c[0][1][1], c[1][1][1], u);
	return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
}

inline double Fbm2(double x, double y, uint64_t seed, int oct = 4, double lac = 2.0, double gain = 0.5)
{
	double s = 0, a = 1, n = 0;
	for (int i = 0; i < oct; ++i, a *= gain)
	{
		s += a * Noise2(x, y, seed + 977u * uint64_t(i));
		n += a;
		x = x * lac + 17.3;
		y = y * lac - 9.1;
	}
	return n > 0 ? s / n * 1.4 : 0.0;
}

inline double Fbm3(double x, double y, double z, uint64_t seed, int oct = 4, double lac = 2.0, double gain = 0.5)
{
	double s = 0, a = 1, n = 0;
	for (int i = 0; i < oct; ++i, a *= gain)
	{
		s += a * Noise3(x, y, z, seed + 977u * uint64_t(i));
		n += a;
		x = x * lac + 17.3;
		y = y * lac - 9.1;
		z = z * lac + 5.7;
	}
	return n > 0 ? s / n * 1.4 : 0.0;
}

// ridged: 0 in the valleys .. 1 on sharp crests
inline double Ridged2(double x, double y, uint64_t seed, int oct = 3)
{
	double s = 0, a = 1, n = 0;
	for (int i = 0; i < oct; ++i, a *= 0.5)
	{
		const double v = 1.0 - std::abs(Noise2(x, y, seed + 31u * uint64_t(i)));
		s += a * v * v;
		n += a;
		x = x * 2.03 + 3.1;
		y = y * 2.03 - 1.7;
	}
	return s / n;
}

// Worley cells: F1, F2 distances and the id of the nearest feature point (2D)
struct Cell2 { double f1 = 1e30, f2 = 1e30; uint64_t id = 0; V2 p; };
inline Cell2 Worley2(double x, double y, uint64_t seed, double jitter = 1.0)
{
	Cell2 r;
	const int64_t ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y));
	for (int64_t j = iy - 1; j <= iy + 1; ++j)
		for (int64_t i = ix - 1; i <= ix + 1; ++i)
		{
			const double px = double(i) + 0.5 + jitter * (Hash01(seed, uint64_t(i), uint64_t(j), 1) - 0.5);
			const double py = double(j) + 0.5 + jitter * (Hash01(seed, uint64_t(i), uint64_t(j), 2) - 0.5);
			const double d = std::hypot(x - px, y - py);
			if (d < r.f1) { r.f2 = r.f1; r.f1 = d; r.id = Hash(seed, uint64_t(i), uint64_t(j), 3); r.p = V2(px, py); }
			else if (d < r.f2) r.f2 = d;
		}
	return r;
}

struct Cell3 { double f1 = 1e30, f2 = 1e30; uint64_t id = 0; };
inline Cell3 Worley3(double x, double y, double z, uint64_t seed)
{
	Cell3 r;
	const int64_t ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y)), iz = int64_t(std::floor(z));
	for (int64_t k = iz - 1; k <= iz + 1; ++k)
		for (int64_t j = iy - 1; j <= iy + 1; ++j)
			for (int64_t i = ix - 1; i <= ix + 1; ++i)
			{
				const double px = double(i) + Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 1);
				const double py = double(j) + Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 2);
				const double pz = double(k) + Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 3);
				const double d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
				if (d < r.f1) { r.f2 = r.f1; r.f1 = d; r.id = Hash(seed, uint64_t(i), uint64_t(j), uint64_t(k)); }
				else if (d < r.f2) r.f2 = d;
			}
	return r;
}

// distance from p to the segment a-b, t = position along it (0..1)
inline double SegDist(const V2& p, const V2& a, const V2& b, double* t = nullptr)
{
	const V2 ab = b - a;
	const double l2 = Dot(ab, ab);
	const double s = l2 > 0 ? Sat(Dot(p - a, ab) / l2) : 0.0;
	if (t) *t = s;
	return Len(p - (a + ab * s));
}

// ---------------------------------------------------------------------------
// result mesh
struct GenAttr
{
	std::string name;
	std::vector<float> values;   // one per point
};

struct GenMesh
{
	std::vector<V3> points;
	std::vector<int32_t> faceVerts, faceSizes, faceMat;
	std::vector<std::array<double, 2>> loopUv;   // one per face corner
	std::deque<GenAttr> attrs;                   // point float attributes (deque: references stay valid)
	std::vector<int32_t> facePart;               // optional: one id per face (piece / tile / splinter), else empty

	int32_t AddPoint(const V3& p) { points.push_back(p); return int32_t(points.size() - 1); }
	void AddFace(std::initializer_list<int32_t> v, int32_t mat, std::initializer_list<std::array<double, 2>> uv, int32_t part = -1)
	{
		faceVerts.insert(faceVerts.end(), v.begin(), v.end());
		faceSizes.push_back(int32_t(v.size()));
		faceMat.push_back(mat);
		if (uv.size() == v.size()) loopUv.insert(loopUv.end(), uv.begin(), uv.end());
		else for (size_t i = 0; i < v.size(); ++i) loopUv.push_back({ 0.0, 0.0 });
		if (part >= 0 || !facePart.empty())
		{
			facePart.resize(faceSizes.size() - 1, -1);
			facePart.push_back(part);
		}
	}
	void AddFaceV(const std::vector<int32_t>& v, int32_t mat, const std::vector<std::array<double, 2>>& uv, int32_t part = -1)
	{
		faceVerts.insert(faceVerts.end(), v.begin(), v.end());
		faceSizes.push_back(int32_t(v.size()));
		faceMat.push_back(mat);
		if (uv.size() == v.size()) loopUv.insert(loopUv.end(), uv.begin(), uv.end());
		else for (size_t i = 0; i < v.size(); ++i) loopUv.push_back({ 0.0, 0.0 });
		if (part >= 0 || !facePart.empty())
		{
			facePart.resize(faceSizes.size() - 1, -1);
			facePart.push_back(part);
		}
	}
	std::vector<float>& Attr(const std::string& name)
	{
		for (auto& a : attrs)
			if (a.name == name) { a.values.resize(points.size(), 0.0f); return a.values; }
		attrs.push_back({ name, std::vector<float>(points.size(), 0.0f) });
		return attrs.back().values;
	}
	void FinishAttrs() { for (auto& a : attrs) a.values.resize(points.size(), 0.0f); if (!facePart.empty()) facePart.resize(faceSizes.size(), -1); }
	// Append another mesh (offsetting indices); attributes are matched by name.
	void Append(const GenMesh& o, int32_t partOffset = 0)
	{
		const int32_t base = int32_t(points.size());
		FinishAttrs();
		points.insert(points.end(), o.points.begin(), o.points.end());
		for (int32_t v : o.faceVerts) faceVerts.push_back(v + base);
		faceSizes.insert(faceSizes.end(), o.faceSizes.begin(), o.faceSizes.end());
		faceMat.insert(faceMat.end(), o.faceMat.begin(), o.faceMat.end());
		loopUv.insert(loopUv.end(), o.loopUv.begin(), o.loopUv.end());
		if (!o.facePart.empty() || !facePart.empty())
		{
			facePart.resize(faceSizes.size() - o.faceSizes.size(), -1);
			for (size_t i = 0; i < o.faceSizes.size(); ++i)
				facePart.push_back(i < o.facePart.size() && o.facePart[i] >= 0 ? o.facePart[i] + partOffset : -1);
		}
		for (const auto& a : o.attrs)
		{
			auto& dst = Attr(a.name);
			dst.resize(base);
			dst.insert(dst.end(), a.values.begin(), a.values.end());
			dst.resize(points.size(), 0.0f);
		}
		FinishAttrs();
	}
};

// ---------------------------------------------------------------------------
// 2D domain mesher
//
// Meshes { (u,v) : f(u,v) <= 0 } inside the box [u0,u1] x [v0,v1] with a grid
// of edge h (right triangles, alternating diagonals). Grid vertices closer
// than 0.24 h to the contour are pulled onto it (no slivers), the remaining
// crossings are cut exactly. The result is a triangle mesh whose border
// follows the contour, CCW in (u,v) (normal +Z).
struct Domain2D
{
	std::vector<V2> rest;                  // vertex positions in (u,v)
	std::vector<std::array<int32_t, 3>> tris;
	std::vector<double> f;                 // f at each vertex (<= 0, 0 on the border)
	double h = 0.0;
};

// A vertex where two separate fans of triangles meet (two pieces touching in
// one point) is duplicated, one copy per fan, so every vertex is manifold.
inline void SplitPinchVertices(Domain2D& d)
{
	const size_t nv = d.rest.size();
	std::vector<std::vector<int32_t>> inc(nv);
	for (size_t t = 0; t < d.tris.size(); ++t)
		for (int k = 0; k < 3; ++k) inc[d.tris[t][k]].push_back(int32_t(t));
	for (size_t v = 0; v < nv; ++v)
	{
		auto& ts = inc[v];
		if (ts.size() < 2) continue;
		// union the triangles around v that share an edge through v
		std::vector<int32_t> grp(ts.size());
		for (size_t i = 0; i < ts.size(); ++i) grp[i] = int32_t(i);
		std::function<int32_t(int32_t)> find = [&](int32_t x) { while (grp[x] != x) x = grp[x] = grp[grp[x]]; return x; };
		auto others = [&](int32_t t, int32_t o[2])
		{
			int c = 0;
			for (int k = 0; k < 3; ++k)
				if (d.tris[t][k] != int32_t(v)) o[c++] = d.tris[t][k];
		};
		for (size_t i = 0; i < ts.size(); ++i)
			for (size_t j = i + 1; j < ts.size(); ++j)
			{
				int32_t a[2], b[2];
				others(ts[i], a);
				others(ts[j], b);
				if (a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1]) grp[find(int32_t(i))] = find(int32_t(j));
			}
		std::unordered_map<int32_t, int32_t> copyOf;
		const int32_t first = find(0);
		for (size_t i = 0; i < ts.size(); ++i)
		{
			const int32_t g = find(int32_t(i));
			if (g == first) continue;
			auto it = copyOf.find(g);
			int32_t nvId;
			if (it == copyOf.end())
			{
				nvId = int32_t(d.rest.size());
				d.rest.push_back(d.rest[v]);
				d.f.push_back(d.f[v]);
				copyOf.emplace(g, nvId);
			}
			else nvId = it->second;
			for (int k = 0; k < 3; ++k)
				if (d.tris[ts[i]][k] == int32_t(v)) d.tris[ts[i]][k] = nvId;
		}
	}
}

inline void MeshDomain2D(const std::function<double(double, double)>& field, double u0, double u1, double v0, double v1, double h,
	Domain2D& out, double minPieceArea = 0.0, double hvAniso = 0.0)
{
	out = Domain2D();
	const double hU = h, hV = hvAniso > 0.0 ? hvAniso : h;
	h = std::min(hU, hV);
	out.h = h;
	const int nu = std::max(1, int(std::ceil((u1 - u0) / hU))), nv = std::max(1, int(std::ceil((v1 - v0) / hV)));
	const double hu = (u1 - u0) / nu, hv = (v1 - v0) / nv;
	const int W = nu + 1;
	std::vector<V2> gp(size_t(W) * (nv + 1));
	std::vector<double> gf(gp.size());
	for (int j = 0; j <= nv; ++j)
		for (int i = 0; i <= nu; ++i)
		{
			const size_t k = size_t(j) * W + i;
			gp[k] = V2(u0 + i * hu, v0 + j * hv);
			gf[k] = field(gp[k].x, gp[k].y);
		}
	// snap near-contour vertices onto the contour (Newton step along the gradient)
	const double snap = 0.24 * std::min(hu, hv), eps = 0.05 * std::min(hu, hv);
	for (size_t k = 0; k < gp.size(); ++k)
	{
		if (std::abs(gf[k]) >= snap) continue;
		V2 p = gp[k];
		double fv = gf[k];
		for (int it = 0; it < 3 && std::abs(fv) > 1e-9 * h; ++it)
		{
			const double gx = (field(p.x + eps, p.y) - field(p.x - eps, p.y)) / (2 * eps);
			const double gy = (field(p.x, p.y + eps) - field(p.x, p.y - eps)) / (2 * eps);
			const double g2 = gx * gx + gy * gy;
			if (g2 < 1e-12) break;
			V2 q = p - V2(gx, gy) * (fv / g2);
			if (Len(q - gp[k]) > snap) break;
			p = q;
			fv = field(p.x, p.y);
		}
		if (std::abs(fv) < 0.02 * h && Len(p - gp[k]) <= snap)
		{
			gp[k] = p;
			gf[k] = 0.0;
		}
	}
	// vertex classes: -1 inside, 0 on, +1 outside
	auto cls = [&](size_t k) { return gf[k] < 0.0 ? -1 : (gf[k] > 0.0 ? 1 : 0); };
	std::vector<int32_t> gridIndex(gp.size(), -1);
	auto vid = [&](size_t k) -> int32_t
	{
		if (gridIndex[k] < 0)
		{
			gridIndex[k] = int32_t(out.rest.size());
			out.rest.push_back(gp[k]);
			out.f.push_back(std::min(gf[k], 0.0));
		}
		return gridIndex[k];
	};
	std::unordered_map<uint64_t, int32_t> cross;
	auto cid = [&](size_t a, size_t b) -> int32_t
	{
		const uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
		auto it = cross.find(key);
		if (it != cross.end()) return it->second;
		// bisection refinement of the linear estimate (the field is not linear)
		double fa = gf[a], fb = gf[b];
		V2 pa = gp[a], pb = gp[b];
		double t = fa / (fa - fb);
		V2 p = pa + (pb - pa) * t;
		for (int it2 = 0; it2 < 6; ++it2)
		{
			const double fp = field(p.x, p.y);
			if (std::abs(fp) < 1e-7 * h) break;
			if ((fp < 0) == (fa < 0)) { pa = p; fa = fp; }
			else { pb = p; fb = fp; }
			t = fa / (fa - fb);
			p = pa + (pb - pa) * t;
		}
		const int32_t id = int32_t(out.rest.size());
		out.rest.push_back(p);
		out.f.push_back(0.0);
		cross.emplace(key, id);
		return id;
	};
	auto emitTri = [&](size_t a, size_t b, size_t c)
	{
		const size_t v[3] = { a, b, c };
		int c3[3] = { cls(a), cls(b), cls(c) };
		if (c3[0] > 0 && c3[1] > 0 && c3[2] > 0) return;
		if (c3[0] >= 0 && c3[1] >= 0 && c3[2] >= 0) return;   // nothing strictly inside
		std::vector<int32_t> poly;
		for (int e = 0; e < 3; ++e)
		{
			const size_t p = v[e], q = v[(e + 1) % 3];
			const int cp = c3[e], cq = c3[(e + 1) % 3];
			if (cp <= 0) poly.push_back(vid(p));
			if ((cp < 0 && cq > 0) || (cp > 0 && cq < 0)) poly.push_back(cid(p, q));
		}
		for (size_t k = 1; k + 1 < poly.size(); ++k)
			if (poly[0] != poly[k] && poly[k] != poly[k + 1] && poly[0] != poly[k + 1])
				out.tris.push_back({ poly[0], poly[k], poly[k + 1] });
	};
	for (int j = 0; j < nv; ++j)
		for (int i = 0; i < nu; ++i)
		{
			const size_t a = size_t(j) * W + i, b = a + 1, c = a + W + 1, d = a + W;
			if (((i + j) & 1) == 0) { emitTri(a, b, c); emitTri(a, c, d); }
			else { emitTri(a, b, d); emitTri(b, c, d); }
		}
	// drop zero-area triangles
	{
		std::vector<std::array<int32_t, 3>> keep;
		keep.reserve(out.tris.size());
		const double minA = 1e-6 * h * h;
		for (const auto& t : out.tris)
		{
			const V2 e1 = out.rest[t[1]] - out.rest[t[0]], e2 = out.rest[t[2]] - out.rest[t[0]];
			if (e1.x * e2.y - e1.y * e2.x > minA) keep.push_back(t);
		}
		out.tris.swap(keep);
	}
	// pieces: drop the ones below minPieceArea (connected through edges);
	// a negative minPieceArea keeps only the largest piece
	if (minPieceArea != 0.0 && !out.tris.empty())
	{
		const size_t nt = out.tris.size();
		std::unordered_map<uint64_t, int32_t> edgeTri;
		std::vector<int32_t> parent(nt);
		for (size_t i = 0; i < nt; ++i) parent[i] = int32_t(i);
		std::function<int32_t(int32_t)> find = [&](int32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
		for (size_t i = 0; i < nt; ++i)
			for (int e = 0; e < 3; ++e)
			{
				const uint32_t a = uint32_t(out.tris[i][e]), b = uint32_t(out.tris[i][(e + 1) % 3]);
				const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
				auto it = edgeTri.find(key);
				if (it == edgeTri.end()) edgeTri.emplace(key, int32_t(i));
				else parent[find(int32_t(i))] = find(it->second);
			}
		std::unordered_map<int32_t, double> area;
		for (size_t i = 0; i < nt; ++i)
		{
			const auto& t = out.tris[i];
			const V2 e1 = out.rest[t[1]] - out.rest[t[0]], e2 = out.rest[t[2]] - out.rest[t[0]];
			area[find(int32_t(i))] += 0.5 * (e1.x * e2.y - e1.y * e2.x);
		}
		double minA = minPieceArea;
		if (minPieceArea < 0.0)
		{
			double best = 0.0;
			for (const auto& kv : area) best = std::max(best, kv.second);
			minA = best;
		}
		std::vector<std::array<int32_t, 3>> keep;
		for (size_t i = 0; i < nt; ++i)
			if (area[find(int32_t(i))] >= minA) keep.push_back(out.tris[i]);
		out.tris.swap(keep);
	}
	// compact: drop unused vertices
	std::vector<int32_t> remap(out.rest.size(), -1);
	std::vector<V2> rest;
	std::vector<double> fv;
	for (auto& t : out.tris)
		for (int32_t& v : t)
		{
			if (remap[v] < 0) { remap[v] = int32_t(rest.size()); rest.push_back(out.rest[v]); fv.push_back(out.f[v]); }
			v = remap[v];
		}
	out.rest.swap(rest);
	out.f.swap(fv);
	SplitPinchVertices(out);
}

// Boundary edges of a triangle mesh, oriented as in their triangle.
inline std::vector<std::array<int32_t, 2>> BoundaryEdges(const std::vector<std::array<int32_t, 3>>& tris)
{
	std::unordered_map<uint64_t, int> count;
	count.reserve(tris.size() * 3);
	for (const auto& t : tris)
		for (int e = 0; e < 3; ++e)
		{
			const uint32_t a = uint32_t(t[e]), b = uint32_t(t[(e + 1) % 3]);
			count[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)]++;
		}
	std::vector<std::array<int32_t, 2>> out;
	for (const auto& t : tris)
		for (int e = 0; e < 3; ++e)
		{
			const uint32_t a = uint32_t(t[e]), b = uint32_t(t[(e + 1) % 3]);
			if (count[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)] == 1) out.push_back({ int32_t(a), int32_t(b) });
		}
	return out;
}

// Area-weighted vertex normals of a deformed triangle mesh.
inline std::vector<V3> VertexNormals(const std::vector<V3>& p, const std::vector<std::array<int32_t, 3>>& tris)
{
	std::vector<V3> n(p.size());
	for (const auto& t : tris)
	{
		const V3 c = Cross(p[t[1]] - p[t[0]], p[t[2]] - p[t[0]]);
		n[t[0]] += c; n[t[1]] += c; n[t[2]] += c;
	}
	for (auto& v : n) v = Norm(v);
	return n;
}


// Connected pieces of a 2D domain (through shared edges): piece id per triangle.
inline int DomainPieces(const Domain2D& d, std::vector<int32_t>& triPiece)
{
	const size_t nt = d.tris.size();
	std::vector<int32_t> parent(nt);
	for (size_t i = 0; i < nt; ++i) parent[i] = int32_t(i);
	auto find = [&](int32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
	std::unordered_map<uint64_t, int32_t> edgeTri;
	for (size_t i = 0; i < nt; ++i)
		for (int e = 0; e < 3; ++e)
		{
			const uint32_t a = uint32_t(d.tris[i][e]), b = uint32_t(d.tris[i][(e + 1) % 3]);
			const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
			auto it = edgeTri.find(key);
			if (it == edgeTri.end()) edgeTri.emplace(key, int32_t(i));
			else parent[find(int32_t(i))] = find(it->second);
		}
	std::unordered_map<int32_t, int32_t> ids;
	triPiece.assign(nt, 0);
	for (size_t i = 0; i < nt; ++i)
	{
		const int32_t r = find(int32_t(i));
		auto it = ids.find(r);
		if (it == ids.end()) it = ids.emplace(r, int32_t(ids.size())).first;
		triPiece[i] = it->second;
	}
	return int(ids.size());
}

// A 2D domain extruded between per-vertex top and bottom points (both given
// in 3D, so the caller can bend, tilt or move them). Only the triangles with
// triPiece == piece are used (piece < 0: all). Top faces get topMat(tri),
// bottom faces matBottom, walls sideMat(rest midpoint). UVs: top / bottom from
// rest (u,v) * uvScale, walls along the border and up the height.
struct ExtrudeOptions
{
	std::function<int32_t(size_t)> topMat;
	int32_t matBottom = 1, matSide = 1;
	std::function<int32_t(const V2&)> sideMat;
	double uvScale = 1.0, uvU0 = 0.0, uvV0 = 0.0;
	int32_t part = -1;
	const std::vector<int32_t>* triPiece = nullptr;
	int32_t piece = -1;
	bool bottom = true;    // false: no bottom faces (the walls end in an open border)
};
// Returns, per domain vertex, the index of its top copy in out (-1 when unused);
// the bottom copy is that index + count of used vertices.
inline std::vector<int32_t> ExtrudeDomain(const Domain2D& d, const std::vector<V3>& top, const std::vector<V3>& bot,
	const ExtrudeOptions& o, GenMesh& out)
{
	std::vector<std::array<int32_t, 3>> tris;
	std::vector<size_t> triIx;
	for (size_t i = 0; i < d.tris.size(); ++i)
		if (o.piece < 0 || !o.triPiece || (*o.triPiece)[i] == o.piece) { tris.push_back(d.tris[i]); triIx.push_back(i); }
	std::vector<int32_t> map(d.rest.size(), -1);
	if (tris.empty()) return map;
	std::vector<int32_t> used;
	for (auto& t : tris)
		for (int32_t v : t)
			if (map[v] < 0) { map[v] = int32_t(used.size()); used.push_back(v); }
	const int32_t base = int32_t(out.points.size()), n = int32_t(used.size());
	for (int32_t v : used) out.points.push_back(top[v]);
	for (int32_t v : used) out.points.push_back(bot[v]);
	auto uv = [&](int32_t v, bool mirror) -> std::array<double, 2>
	{
		const double u = (d.rest[v].x - o.uvU0) * o.uvScale, w = (d.rest[v].y - o.uvV0) * o.uvScale;
		return { mirror ? -u : u, w };
	};
	for (size_t k = 0; k < tris.size(); ++k)
	{
		const auto& t = tris[k];
		const int32_t mat = o.topMat ? o.topMat(triIx[k]) : 0;
		out.AddFace({ base + map[t[0]], base + map[t[1]], base + map[t[2]] }, mat, { uv(t[0], false), uv(t[1], false), uv(t[2], false) }, o.part);
		if (o.bottom)
			out.AddFace({ base + n + map[t[0]], base + n + map[t[2]], base + n + map[t[1]] }, o.matBottom,
				{ uv(t[0], true), uv(t[2], true), uv(t[1], true) }, o.part);
	}
	for (const auto& e : BoundaryEdges(tris))
	{
		const double s0 = (d.rest[e[0]].x + d.rest[e[0]].y) * o.uvScale, s1 = s0 + Len(d.rest[e[1]] - d.rest[e[0]]) * o.uvScale;
		const double h0 = Len(top[e[0]] - bot[e[0]]) * o.uvScale, h1 = Len(top[e[1]] - bot[e[1]]) * o.uvScale;
		const int32_t mat = o.sideMat ? o.sideMat((d.rest[e[0]] + d.rest[e[1]]) * 0.5) : o.matSide;
		out.AddFace({ base + map[e[1]], base + map[e[0]], base + n + map[e[0]], base + n + map[e[1]] }, mat,
			{ { s1, h1 }, { s0, h0 }, { s0, 0.0 }, { s1, 0.0 } }, o.part);
	}
	return map;
}

// A deformed sheet -> closed shell. mid: deformed mid-surface per rest vertex.
// thickness 0 gives a single-sided surface (front faces only).
// Material ids: front, back, side. UVs: front/back from rest (u,v) * uvScale
// (+ uvOffset), the back mirrored in U; sides run along the border.
struct SheetOptions
{
	double thickness = 0.0;
	int32_t matFront = 0, matBack = 1, matSide = 2;
	double uvScale = 1.0, uvU0 = 0.0, uvV0 = 0.0;
	const std::vector<double>* thicknessScale = nullptr;   // optional per vertex factor
	int32_t part = -1;
	std::function<int32_t(const V2&)> sideMat;              // optional: material of a wall by its rest midpoint
	double sideV0 = 0.0;                                    // V of the back face on the walls (layer stacking)
};
inline void SolidifySheet(const Domain2D& d, const std::vector<V3>& mid, const SheetOptions& o, GenMesh& out)
{
	const int32_t base = int32_t(out.points.size());
	const size_t n = mid.size();
	auto uvOf = [&](int32_t v, bool back) -> std::array<double, 2>
	{
		const double u = (d.rest[v].x - o.uvU0) * o.uvScale, w = (d.rest[v].y - o.uvV0) * o.uvScale;
		return { back ? -u : u, w };
	};
	if (o.thickness <= 0.0)
	{
		for (size_t i = 0; i < n; ++i) out.points.push_back(mid[i]);
		for (const auto& t : d.tris)
			out.AddFace({ base + t[0], base + t[1], base + t[2] }, o.matFront, { uvOf(t[0], false), uvOf(t[1], false), uvOf(t[2], false) }, o.part);
		return;
	}
	const std::vector<V3> nrm = VertexNormals(mid, d.tris);
	for (size_t i = 0; i < n; ++i)
	{
		const double t = 0.5 * o.thickness * (o.thicknessScale ? (*o.thicknessScale)[i] : 1.0);
		out.points.push_back(mid[i] + nrm[i] * t);
	}
	for (size_t i = 0; i < n; ++i)
	{
		const double t = 0.5 * o.thickness * (o.thicknessScale ? (*o.thicknessScale)[i] : 1.0);
		out.points.push_back(mid[i] - nrm[i] * t);
	}
	const int32_t bb = base + int32_t(n);
	for (const auto& t : d.tris)
	{
		out.AddFace({ base + t[0], base + t[1], base + t[2] }, o.matFront, { uvOf(t[0], false), uvOf(t[1], false), uvOf(t[2], false) }, o.part);
		out.AddFace({ bb + t[0], bb + t[2], bb + t[1] }, o.matBack, { uvOf(t[0], true), uvOf(t[2], true), uvOf(t[1], true) }, o.part);
	}
	const auto edges = BoundaryEdges(d.tris);
	for (const auto& e : edges)
	{
		const double s0 = (d.rest[e[0]].x + d.rest[e[0]].y) * o.uvScale, s1 = s0 + Len(d.rest[e[1]] - d.rest[e[0]]) * o.uvScale;
		const double th = (o.sideV0 + o.thickness) * o.uvScale, tb = o.sideV0 * o.uvScale;
		const int32_t mat = o.sideMat ? o.sideMat((d.rest[e[0]] + d.rest[e[1]]) * 0.5) : o.matSide;
		out.AddFace({ base + e[1], base + e[0], bb + e[0], bb + e[1] }, mat, { { s1, th }, { s0, th }, { s0, tb }, { s1, tb } }, o.part);
	}
}

} // namespace pdgen
