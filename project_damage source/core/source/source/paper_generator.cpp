// мнем и рвём исходную геометрию как бумагу
// ProjectDamage paper generator — GPL-3.0-or-later; see LICENSE.
//
// A sheet of paper built from scratch.
//
// 1. Outline as an implicit field. On a fine grid (half the mesh resolution)
//    the field is positive on paper: the sheet rectangle (slightly ragged),
//    minus the interior holes (procedural lobed holes, Grease Pencil contours
//    and gashes) and torn-away areas (closed GP strokes). Every torn feature is
//    evaluated in a domain-warped space (two fbm octaves: Jagged + Fiber), so
//    its rim is ragged like real torn paper. A Gaussian blur of that field
//    (Roundness) rounds the holes. Tear lines (open GP strokes, procedural
//    corner tears and border bites) are then cut in as thin warped slits that
//    split the sheet into pieces; the pieces are labelled on the grid and all
//    but the largest are dropped (or kept apart, Keep Pieces).
// 2. Marching squares turns the zero level of the field into closed, non
//    self-intersecting boundary loops (inside on the left), resampled by arc
//    length at the mesh resolution.
// 3. Clean topology: a regular grid of interior points (none closer than ~half
//    an edge to a border) plus the loop points go into an incremental Delaunay
//    triangulation; missing border edges are recovered by splitting them
//    (conforming Delaunay), and the triangles are classified inside / outside
//    by parity across the border edges. Triangle pairs that fill one grid cell
//    are merged back into quads, so the inside is a clean quad grid and only
//    the torn borders are triangulated. Every edge is shared by at most two
//    faces, every border is one closed loop, no degenerate faces.
// 4. Crumple: up to three octaves of a piecewise linear height field over
//    random coarse Delaunay triangulations (sharp creases between flat
//    facets), blended with smooth value noise (Sharpness), plus a large
//    waviness, zig-zag folds and the curl of torn rims. Points move only along
//    Z, so UVs (the flat sheet mapped to 0..1) never stretch.
// 5. Thickness > 0 turns the surface into a closed shell (offset along the
//    vertex normals, side walls along every border).
#include "paper_generator.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

constexpr double kPi = 3.14159265358979323846;

struct P2
{
	double x = 0.0, y = 0.0;
	P2() = default;
	P2(double a, double b) : x(a), y(b) {}
};
inline P2 operator+(const P2& a, const P2& b) { return P2(a.x + b.x, a.y + b.y); }
inline P2 operator-(const P2& a, const P2& b) { return P2(a.x - b.x, a.y - b.y); }
inline P2 operator*(const P2& a, double s) { return P2(a.x * s, a.y * s); }
inline double Dot(const P2& a, const P2& b) { return a.x * b.x + a.y * b.y; }
inline double Cross(const P2& a, const P2& b) { return a.x * b.y - a.y * b.x; }
inline double Len(const P2& a) { return std::sqrt(Dot(a, a)); }
inline double Clamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }

uint64_t Hash(uint64_t x)
{
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
	return x;
}
inline uint64_t Mix(uint64_t a, uint64_t b) { return Hash(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2))); }

struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(Hash(seed * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL) | 1ULL) {}
	double U()
	{
		s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
		return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
	}
	double R(double a, double b) { return a + (b - a) * U(); }
	int I(int n) { return std::min(n - 1, int(U() * n)); }
};

// Counter based channels (as in the other modules): changing one amount does
// not reshuffle the others.
enum Channel : uint64_t { kChHoles = 1, kChCorners, kChBites, kChWarpX, kChWarpY, kChFiberX, kChFiberY,
	kChRough, kChCrumple, kChSoft, kChWave, kChCurl, kChTornLobe, kChTongue, kChTongueDir, kChTornFine, kChFlake, kChPeel };

inline double Lattice(uint64_t key, int64_t ix, int64_t iy)
{
	const uint64_t h = Mix(Mix(key, uint64_t(ix) * 0x8da6b343ULL), uint64_t(iy) * 0xd8163841ULL);
	return double(h >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

// smooth 2D value noise in [-1, 1]
double VNoise(uint64_t key, double x, double y)
{
	const double fx = std::floor(x), fy = std::floor(y);
	const int64_t ix = int64_t(fx), iy = int64_t(fy);
	double tx = x - fx, ty = y - fy;
	tx = tx * tx * tx * (tx * (tx * 6.0 - 15.0) + 10.0);
	ty = ty * ty * ty * (ty * (ty * 6.0 - 15.0) + 10.0);
	const double a = Lattice(key, ix, iy), b = Lattice(key, ix + 1, iy);
	const double c = Lattice(key, ix, iy + 1), d = Lattice(key, ix + 1, iy + 1);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

double Fbm(uint64_t key, double x, double y, int oct)
{
	double s = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
	for (int o = 0; o < oct; ++o)
	{
		s += amp * VNoise(Mix(key, uint64_t(o) + 17), x * f + 0.37 * o, y * f - 0.21 * o);
		norm += amp; amp *= 0.5; f *= 2.03;
	}
	return s / norm;
}

// ---------------------------------------------------------------------------
// Incremental Delaunay (Bowyer-Watson) with triangle adjacency.
// ---------------------------------------------------------------------------
inline double Orient(const P2& a, const P2& b, const P2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

inline double InCircle(const P2& a, const P2& b, const P2& c, const P2& d)
{
	const double adx = a.x - d.x, ady = a.y - d.y, bdx = b.x - d.x, bdy = b.y - d.y, cdx = c.x - d.x, cdy = c.y - d.y;
	const double ad = adx * adx + ady * ady, bd = bdx * bdx + bdy * bdy, cd = cdx * cdx + cdy * cdy;
	return adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx);
}

struct Delaunay
{
	struct Tri { int v[3]; int n[3]; };   // n[k]: neighbour across the edge opposite v[k]
	std::vector<P2> p;
	std::vector<Tri> t;
	std::vector<uint8_t> dead;
	std::vector<uint32_t> stamp;
	uint32_t curStamp = 0;
	int last = 0;
	std::vector<int> cav, stack;
	struct BEdge { int a, b, nb; };
	std::vector<BEdge> bnd;
	std::vector<int> made;
	std::vector<int> freeT;   // dead slots, reused by the next insertions

	void Init(double minX, double minY, double maxX, double maxY, size_t expect = 0)
	{
		if (expect) { p.reserve(expect + 8); t.reserve(expect * 2 + 64); dead.reserve(expect * 2 + 64); stamp.reserve(expect * 2 + 64); }
		freeT.clear();
		const double cx = 0.5 * (minX + maxX), cy = 0.5 * (minY + maxY);
		const double M = 8.0 * std::max({ maxX - minX, maxY - minY, 1e-6 });
		p = { P2(cx - 2.0 * M, cy - M), P2(cx + 2.0 * M, cy - M), P2(cx, cy + 2.0 * M) };
		t.clear(); dead.clear(); stamp.clear();
		t.push_back(Tri{ { 0, 1, 2 }, { -1, -1, -1 } });
		dead.push_back(0); stamp.push_back(0);
		last = 0;
	}

	int Locate(const P2& q, int start) const
	{
		int ti = (start >= 0 && start < int(t.size()) && !dead[start]) ? start : last;
		if (ti < 0 || ti >= int(t.size()) || dead[ti])
		{
			ti = -1;
			for (int i = int(t.size()) - 1; i >= 0; --i) if (!dead[i]) { ti = i; break; }
			if (ti < 0) return -1;
		}
		const int cap = 64 + 4 * int(std::sqrt(double(t.size())) + 1.0) * 4;
		for (int step = 0; step < cap; ++step)
		{
			const Tri& T = t[ti];
			int next = -2;
			for (int kk = 0; kk < 3; ++kk)
			{
				const int k = (kk + step) % 3;
				if (Orient(p[T.v[(k + 1) % 3]], p[T.v[(k + 2) % 3]], q) < 0.0) { next = T.n[k]; break; }
			}
			if (next == -2) return ti;
			if (next < 0) return -1;
			ti = next;
		}
		for (int i = 0; i < int(t.size()); ++i)   // fallback (never expected)
		{
			if (dead[i]) continue;
			const Tri& T = t[i];
			if (Orient(p[T.v[0]], p[T.v[1]], q) >= 0.0 && Orient(p[T.v[1]], p[T.v[2]], q) >= 0.0 && Orient(p[T.v[2]], p[T.v[0]], q) >= 0.0)
				return i;
		}
		return -1;
	}

	// Returns the vertex index (an existing one for a duplicate point), -1 on failure.
	int Insert(const P2& q, double dupTol)
	{
		const int t0 = Locate(q, last);
		if (t0 < 0) return -1;
		for (int k = 0; k < 3; ++k)
			if (Len(p[t[t0].v[k]] - q) <= dupTol) return t[t0].v[k];
		const int vi = int(p.size());
		p.push_back(q);
		++curStamp;
		cav.clear(); stack.clear(); bnd.clear(); made.clear();
		stack.push_back(t0); stamp[t0] = curStamp; cav.push_back(t0);
		while (!stack.empty())
		{
			const int ti = stack.back(); stack.pop_back();
			for (int k = 0; k < 3; ++k)
			{
				const int nb = t[ti].n[k];
				if (nb < 0 || stamp[nb] == curStamp) continue;
				const Tri& N = t[nb];
				// q on the far side of the shared edge: the neighbour must go too (keeps the
				// cavity star shaped even when the in-circle test is off by rounding)
				const int a = t[ti].v[(k + 1) % 3], b = t[ti].v[(k + 2) % 3];
				if (Orient(p[a], p[b], q) <= 0.0 || InCircle(p[N.v[0]], p[N.v[1]], p[N.v[2]], q) > 0.0)
				{ stamp[nb] = curStamp; cav.push_back(nb); stack.push_back(nb); }
			}
		}
		for (int ti : cav)
			for (int k = 0; k < 3; ++k)
			{
				const int nb = t[ti].n[k];
				if (nb >= 0 && stamp[nb] == curStamp) continue;
				bnd.push_back(BEdge{ t[ti].v[(k + 1) % 3], t[ti].v[(k + 2) % 3], nb });
			}
		for (int ti : cav) { dead[ti] = 1; freeT.push_back(ti); }
		for (const BEdge& e : bnd)
		{
			int ni;
			if (!freeT.empty()) { ni = freeT.back(); freeT.pop_back(); t[ni] = Tri{ { e.a, e.b, vi }, { -1, -1, e.nb } }; dead[ni] = 0; stamp[ni] = 0; }
			else { ni = int(t.size()); t.push_back(Tri{ { e.a, e.b, vi }, { -1, -1, e.nb } }); dead.push_back(0); stamp.push_back(0); }
			made.push_back(ni);
			if (e.nb >= 0)
			{
				Tri& N = t[e.nb];
				for (int k = 0; k < 3; ++k)
					if (N.v[(k + 1) % 3] == e.b && N.v[(k + 2) % 3] == e.a) { N.n[k] = ni; break; }
			}
		}
		// link the fan: tri (a,b,vi): across (b,vi) is the tri starting at b, across (vi,a) the tri ending at a
		if (made.size() > 24)
		{
			std::unordered_map<int, int> byA, byB;
			byA.reserve(made.size() * 2); byB.reserve(made.size() * 2);
			for (int ni : made) { byA[t[ni].v[0]] = ni; byB[t[ni].v[1]] = ni; }
			for (int ni : made)
			{
				Tri& T = t[ni];
				auto ia = byA.find(T.v[1]); if (ia != byA.end()) T.n[0] = ia->second;
				auto ib = byB.find(T.v[0]); if (ib != byB.end()) T.n[1] = ib->second;
			}
		}
		else
			for (int ni : made)
			{
				Tri& T = t[ni];
				for (int oj : made)
				{
					if (t[oj].v[0] == T.v[1]) T.n[0] = oj;
					if (t[oj].v[1] == T.v[0]) T.n[1] = oj;
				}
			}
		last = made.empty() ? last : made.back();
		return vi;
	}

	// piecewise linear interpolation of per point values
	double Interp(const P2& q, const std::vector<double>& h, int& hint) const
	{
		const int ti = Locate(q, hint);
		if (ti < 0) return 0.0;
		hint = ti;
		const Tri& T = t[ti];
		const P2 &a = p[T.v[0]], &b = p[T.v[1]], &c = p[T.v[2]];
		const double area = Orient(a, b, c);
		if (std::fabs(area) < 1e-30) return h[T.v[0]];
		const double wa = Orient(b, c, q) / area, wb = Orient(c, a, q) / area, wc = 1.0 - wa - wb;
		return wa * h[T.v[0]] + wb * h[T.v[1]] + wc * h[T.v[2]];
	}
};

// Hilbert curve index on a 2^16 grid: insertion order for the Delaunay (row by
// row insertion would make every cavity span the whole previous row).
uint64_t Hilbert(uint32_t x, uint32_t y)
{
	uint64_t d = 0;
	for (uint32_t s = 1u << 15; s > 0; s >>= 1)
	{
		const uint32_t rx = (x & s) ? 1u : 0u, ry = (y & s) ? 1u : 0u;
		d += uint64_t(s) * s * ((3u * rx) ^ ry);
		if (ry == 0) { if (rx == 1) { x = s - 1 - x; y = s - 1 - y; } std::swap(x, y); }
	}
	return d;
}

template <class T> void HilbertSort(std::vector<T>& v, double minX, double minY, double maxX, double maxY)
{
	const double sx = 65535.0 / std::max(maxX - minX, 1e-12), sy = 65535.0 / std::max(maxY - minY, 1e-12);
	std::vector<std::pair<uint64_t, size_t>> k(v.size());
	for (size_t i = 0; i < v.size(); ++i)
		k[i] = { Hilbert(uint32_t(Clamp((v[i].q.x - minX) * sx, 0.0, 65535.0)), uint32_t(Clamp((v[i].q.y - minY) * sy, 0.0, 65535.0))), i };
	std::sort(k.begin(), k.end());
	std::vector<T> o; o.reserve(v.size());
	for (auto& e : k) o.push_back(v[e.second]);
	v.swap(o);
}

inline uint64_t EdgeKey(int a, int b) { if (a > b) std::swap(a, b); return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }

// ---------------------------------------------------------------------------
// Strokes and shapes
// ---------------------------------------------------------------------------
struct Shape2
{
	std::vector<P2> pts;
	bool closed = false;
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
	void Bounds()
	{
		minX = minY = std::numeric_limits<double>::max(); maxX = maxY = -minX;
		for (const P2& q : pts) { minX = std::min(minX, q.x); minY = std::min(minY, q.y); maxX = std::max(maxX, q.x); maxY = std::max(maxY, q.y); }
	}
};

double PolyLength(const std::vector<P2>& v, bool closed)
{
	double L = 0.0;
	for (size_t i = 1; i < v.size(); ++i) L += Len(v[i] - v[i - 1]);
	if (closed && v.size() > 2) L += Len(v.front() - v.back());
	return L;
}

// uniform arc length resampling; closed loops do not repeat the first point
std::vector<P2> Resample(const std::vector<P2>& v, bool closed, double step, int minCount)
{
	std::vector<P2> out;
	if (v.size() < 2) return v;
	std::vector<P2> w = v;
	if (closed) w.push_back(v.front());
	std::vector<double> acc(w.size(), 0.0);
	for (size_t i = 1; i < w.size(); ++i) acc[i] = acc[i - 1] + Len(w[i] - w[i - 1]);
	const double L = acc.back();
	if (L <= 1e-15) return { v.front() };
	int n = std::max(minCount, int(std::lround(L / std::max(step, 1e-12))));
	const int cnt = closed ? n : n + 1;
	out.reserve(cnt);
	size_t j = 1;
	for (int i = 0; i < cnt; ++i)
	{
		const double s = L * double(i) / double(n);
		while (j + 1 < w.size() && acc[j] < s) ++j;
		const double seg = acc[j] - acc[j - 1];
		const double u = seg > 1e-15 ? Clamp((s - acc[j - 1]) / seg, 0.0, 1.0) : 0.0;
		out.push_back(w[j - 1] + (w[j] - w[j - 1]) * u);
	}
	return out;
}

double SegDist2(const P2& q, const P2& a, const P2& b)
{
	const P2 ab = b - a, aq = q - a;
	const double l2 = Dot(ab, ab);
	const double u = l2 > 1e-30 ? Clamp(Dot(aq, ab) / l2, 0.0, 1.0) : 0.0;
	const P2 d = aq - ab * u;
	return Dot(d, d);
}

// distance to the polyline; for closed shapes signed (negative inside)
double ShapeDist(const Shape2& s, const P2& q)
{
	double best = std::numeric_limits<double>::max();
	bool inside = false;
	const size_t n = s.pts.size();
	const size_t segs = s.closed ? n : n - 1;
	for (size_t i = 0; i < segs; ++i)
	{
		const P2& a = s.pts[i];
		const P2& b = s.pts[(i + 1) % n];
		best = std::min(best, SegDist2(q, a, b));
		if (s.closed && ((a.y > q.y) != (b.y > q.y)) && (q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)) inside = !inside;
	}
	const double d = std::sqrt(best);
	return inside ? -d : d;
}

// quadratic Bezier
std::vector<P2> Bezier(const P2& a, const P2& c, const P2& b, int n)
{
	std::vector<P2> v;
	for (int i = 0; i <= n; ++i)
	{
		const double u = double(i) / n;
		v.push_back(a * ((1 - u) * (1 - u)) + c * (2 * u * (1 - u)) + b * (u * u));
	}
	return v;
}

// ---------------------------------------------------------------------------
// Field grid
// ---------------------------------------------------------------------------
struct Grid
{
	int nx = 0, ny = 0;
	double x0 = 0, y0 = 0, g = 1;
	std::vector<double> f;
	std::vector<uint8_t> torn;   // 1 where a torn feature (hole / tear) is the nearest boundary
	P2 Pos(int i, int j) const { return P2(x0 + i * g, y0 + j * g); }
	size_t Id(int i, int j) const { return size_t(j) * nx + i; }
	double Bilinear(const P2& q) const
	{
		const double fx = Clamp((q.x - x0) / g, 0.0, nx - 1.000001), fy = Clamp((q.y - y0) / g, 0.0, ny - 1.000001);
		const int i = int(fx), j = int(fy);
		const double u = fx - i, v = fy - j;
		const double a = f[Id(i, j)], b = f[Id(i + 1, j)], c = f[Id(i, j + 1)], d = f[Id(i + 1, j + 1)];
		return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
	}
	uint8_t TornAt(const P2& q) const
	{
		const int i = int(std::lround(Clamp((q.x - x0) / g, 0.0, nx - 1.0)));
		const int j = int(std::lround(Clamp((q.y - y0) / g, 0.0, ny - 1.0)));
		// a boundary point lies between nodes; look at the 3x3 around it
		for (int dj = -1; dj <= 1; ++dj)
			for (int di = -1; di <= 1; ++di)
			{
				const int a = i + di, b = j + dj;
				if (a < 0 || b < 0 || a >= nx || b >= ny) continue;
				if (torn[Id(a, b)]) return 1;
			}
		return 0;
	}
};

void GaussianBlur(std::vector<double>& f, int nx, int ny, double sigma)
{
	if (sigma < 0.3) return;
	const int r = std::max(1, int(std::ceil(3.0 * sigma)));
	std::vector<double> k(2 * r + 1);
	double sum = 0.0;
	for (int i = -r; i <= r; ++i) { k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma)); sum += k[i + r]; }
	for (double& w : k) w /= sum;
	std::vector<double> tmp(f.size());
	for (int j = 0; j < ny; ++j)
		for (int i = 0; i < nx; ++i)
		{
			double s = 0.0;
			for (int d = -r; d <= r; ++d) s += k[d + r] * f[size_t(j) * nx + std::clamp(i + d, 0, nx - 1)];
			tmp[size_t(j) * nx + i] = s;
		}
	for (int j = 0; j < ny; ++j)
		for (int i = 0; i < nx; ++i)
		{
			double s = 0.0;
			for (int d = -r; d <= r; ++d) s += k[d + r] * tmp[size_t(std::clamp(j + d, 0, ny - 1)) * nx + i];
			f[size_t(j) * nx + i] = s;
		}
}

// Squared Euclidean distance (in cells) to the nearest node with mask == want
// (Felzenszwalb & Huttenlocher, separable, exact).
std::vector<double> SquaredEdt(const std::vector<uint8_t>& mask, int nx, int ny, int want)
{
	const double INF = 1e20;
	std::vector<double> d(mask.size());
	for (size_t i = 0; i < mask.size(); ++i) d[i] = (mask[i] == want) ? 0.0 : INF;
	const int n = std::max(nx, ny);
	std::vector<double> f(n), out(n), z(n + 1);
	std::vector<int> v(n);
	auto pass = [&](int len)
	{
		int k = 0; v[0] = 0; z[0] = -INF; z[1] = INF;
		for (int q = 1; q < len; ++q)
		{
			double sgm;
			for (;;)
			{
				sgm = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
				if (sgm <= z[k] && k > 0) { --k; continue; }
				break;
			}
			++k; v[k] = q; z[k] = sgm; z[k + 1] = INF;
		}
		k = 0;
		for (int q = 0; q < len; ++q)
		{
			while (z[k + 1] < q) ++k;
			out[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
		}
	};
	for (int j = 0; j < ny; ++j)
	{
		for (int i = 0; i < nx; ++i) f[i] = d[size_t(j) * nx + i];
		pass(nx);
		for (int i = 0; i < nx; ++i) d[size_t(j) * nx + i] = out[i];
	}
	for (int i = 0; i < nx; ++i)
	{
		for (int j = 0; j < ny; ++j) f[j] = d[size_t(j) * nx + i];
		pass(ny);
		for (int j = 0; j < ny; ++j) d[size_t(j) * nx + i] = out[j];
	}
	return d;
}

// Marching squares: closed loops of the zero level, positive (paper) on the left.
std::vector<std::vector<P2>> Contour(const Grid& G)
{
	const int nx = G.nx, ny = G.ny;
	auto val = [&](int i, int j) { const double v = G.f[G.Id(i, j)]; return v == 0.0 ? -1e-12 : v; };
	// edge key: 2*(j*nx+i) horizontal (i,j)-(i+1,j), +1 vertical (i,j)-(i,j+1)
	auto hk = [&](int i, int j) { return int64_t(2) * (int64_t(j) * nx + i); };
	auto vk = [&](int i, int j) { return int64_t(2) * (int64_t(j) * nx + i) + 1; };
	std::unordered_map<int64_t, int64_t> next;
	std::unordered_map<int64_t, P2> pos;
	next.reserve(size_t(nx + ny) * 8);
	auto cross = [&](int64_t key) -> P2
	{
		auto it = pos.find(key);
		if (it != pos.end()) return it->second;
		const int64_t c = key >> 1;
		const int i = int(c % nx), j = int(c / nx);
		const bool vert = key & 1;
		const int i2 = vert ? i : i + 1, j2 = vert ? j + 1 : j;
		const double a = val(i, j), b = val(i2, j2);
		const double u = Clamp(a / (a - b), 0.0, 1.0);
		const P2 q = G.Pos(i, j) + (G.Pos(i2, j2) - G.Pos(i, j)) * u;
		pos[key] = q;
		return q;
	};
	for (int j = 0; j + 1 < ny; ++j)
		for (int i = 0; i + 1 < nx; ++i)
		{
			const double c[4] = { val(i, j), val(i + 1, j), val(i + 1, j + 1), val(i, j + 1) };
			const bool in[4] = { c[0] > 0, c[1] > 0, c[2] > 0, c[3] > 0 };
			if (in[0] == in[1] && in[1] == in[2] && in[2] == in[3]) continue;
			// cell boundary CCW: e0 v0-v1 (bottom), e1 v1-v2 (right), e2 v2-v3 (top), e3 v3-v0 (left)
			const int64_t ek[4] = { hk(i, j), vk(i + 1, j), hk(i, j + 1), vk(i, j) };
			int64_t xs[4]; bool isA[4]; int n = 0;
			for (int e = 0; e < 4; ++e)
			{
				const bool a = in[e], b = in[(e + 1) % 4];
				if (a != b) { xs[n] = ek[e]; isA[n] = a; ++n; }   // A: paper -> void along the CCW walk
			}
			const bool centerIn = (c[0] + c[1] + c[2] + c[3]) > 0.0;
			for (int k = 0; k < n; ++k)
			{
				if (!isA[k]) continue;
				const int to = (n == 4 && !centerIn) ? (k + n - 1) % n : (k + 1) % n;
				next[xs[k]] = xs[to];
			}
		}
	std::vector<std::vector<P2>> loops;
	std::unordered_set<int64_t> used;
	used.reserve(next.size() * 2);
	for (const auto& kv : next)
	{
		if (used.count(kv.first)) continue;
		std::vector<P2> loop;
		int64_t k = kv.first;
		size_t guard = 0;
		while (!used.count(k) && guard++ < next.size() + 2)
		{
			used.insert(k);
			loop.push_back(cross(k));
			auto it = next.find(k);
			if (it == next.end()) break;
			k = it->second;
		}
		if (loop.size() >= 3) loops.push_back(std::move(loop));
	}
	// deterministic order (unordered_map iteration is not)
	std::sort(loops.begin(), loops.end(), [](const std::vector<P2>& a, const std::vector<P2>& b)
	{
		auto key = [](const std::vector<P2>& v) { P2 m = v[0]; for (const P2& q : v) if (q.y < m.y || (q.y == m.y && q.x < m.x)) m = q; return m; };
		const P2 ka = key(a), kb = key(b);
		return ka.y != kb.y ? ka.y < kb.y : ka.x < kb.x;
	});
	for (auto& L : loops)   // start each loop at its lowest point, also for determinism
	{
		size_t s = 0;
		for (size_t i = 1; i < L.size(); ++i) if (L[i].y < L[s].y || (L[i].y == L[s].y && L[i].x < L[s].x)) s = i;
		std::rotate(L.begin(), L.begin() + s, L.end());
	}
	return loops;
}

double SignedArea(const std::vector<P2>& v)
{
	double a = 0.0;
	for (size_t i = 0; i < v.size(); ++i) a += Cross(v[i], v[(i + 1) % v.size()]);
	return 0.5 * a;
}

bool SegCross(const P2& a, const P2& b, const P2& c, const P2& d)
{
	const double d1 = Orient(a, b, c), d2 = Orient(a, b, d), d3 = Orient(c, d, a), d4 = Orient(c, d, b);
	return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

// spatial hash of 2D points
struct PointHash
{
	double cell = 1.0;
	std::unordered_map<int64_t, std::vector<int>> m;
	static int64_t K(int64_t i, int64_t j) { return int64_t((uint64_t(i) << 32) ^ (uint64_t(j) & 0xffffffffULL)); }
	void Add(const P2& q, int id) { m[K(int64_t(std::floor(q.x / cell)), int64_t(std::floor(q.y / cell)))].push_back(id); }
	template <class F> void Near(const P2& q, double r, F&& fn) const
	{
		const int64_t i0 = int64_t(std::floor((q.x - r) / cell)), i1 = int64_t(std::floor((q.x + r) / cell));
		const int64_t j0 = int64_t(std::floor((q.y - r) / cell)), j1 = int64_t(std::floor((q.y + r) / cell));
		for (int64_t j = j0; j <= j1; ++j)
			for (int64_t i = i0; i <= i1; ++i)
			{
				auto it = m.find(K(i, j));
				if (it == m.end()) continue;
				for (int id : it->second) fn(id);
			}
	}
};


struct PaperMeshFace
{
	std::array<int, 3> vertex;
	int source;
};

bool PaperMeshTopology(const PaperGenSurface& surface, const Grid& field, double h, int budget, bool uncut, const PaperGenParams& prm, int& pieceCount,
	std::vector<EVec>& positions, std::vector<EVec>& normals, std::vector<int>& faceV,
	std::vector<int>& faceN, std::vector<int>& sources)
{
	positions = surface.points;
	normals.assign(positions.size(), EVec(0.0));
	std::vector<PaperMeshFace> faces;
	for (size_t i = 0; i < surface.triangles.size(); ++i)
	{
		const auto& t = surface.triangles[i];
		const EVec normal = manifold::la::cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
		if (manifold::la::length(normal) < 1e-20) continue;
		faces.push_back({{t[0], t[1], t[2]}, int(i)});
		for (int v : t) normals[v] += normal;
	}
	auto normalize = [](const EVec& n) {
		const double length = manifold::la::length(n);
		return length > 1e-20 ? n / length : EVec(0.0, 0.0, 1.0);
	};
	for (auto& n : normals) n = normalize(n);
	for (int pass = 0; pass < 24; ++pass)
	{
		std::unordered_map<uint64_t, int> mids;
		for (const auto& f : faces)
			for (int k = 0; k < 3; ++k)
			{
				const int a = f.vertex[k], b = f.vertex[(k + 1) % 3];
				if (manifold::la::length(positions[b] - positions[a]) > h * 1.5)
					mids.emplace(EdgeKey(a, b), -1);
			}
		if (mids.empty() || positions.size() + mids.size() > size_t(budget)) break;
		std::vector<PaperMeshFace> refined;
		refined.reserve(faces.size() * 2);
		auto midpoint = [&](int a, int b) {
			auto it = mids.find(EdgeKey(a, b));
			if (it == mids.end()) return -1;
			if (it->second < 0)
			{
				it->second = int(positions.size());
				positions.push_back((positions[a] + positions[b]) * 0.5);
				normals.push_back(normalize(normals[a] + normals[b]));
			}
			return it->second;
		};
		for (const auto& f : faces)
		{
			const auto& t = f.vertex;
			const int mid[3] = {midpoint(t[0], t[1]), midpoint(t[1], t[2]), midpoint(t[2], t[0])};
			const int count = (mid[0] >= 0) + (mid[1] >= 0) + (mid[2] >= 0);
			auto emit = [&](int a, int b, int c) { refined.push_back({{a, b, c}, f.source}); };
			if (count == 0) refined.push_back(f);
			else if (count == 3)
			{
				emit(t[0], mid[0], mid[2]); emit(mid[0], t[1], mid[1]);
				emit(mid[2], mid[1], t[2]); emit(mid[0], mid[1], mid[2]);
			}
			else if (count == 1)
			{
				int k = 0; while (mid[k] < 0) ++k;
				emit(t[k], mid[k], t[(k + 2) % 3]);
				emit(mid[k], t[(k + 1) % 3], t[(k + 2) % 3]);
			}
			else
			{
				int k = 0; while (mid[k] >= 0) ++k;
				const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
				const int bc = mid[(k + 1) % 3], ca = mid[(k + 2) % 3];
				emit(c, ca, bc); emit(a, b, ca); emit(b, bc, ca);
			}
		}
		faces.swap(refined);
	}
	std::vector<double> value;
	value.reserve(positions.size());
	for (const auto& q : positions) value.push_back(uncut ? h : field.Bilinear(P2(q.x, q.y)));
	std::unordered_map<uint64_t, int> crossings;
	for (const auto& f : faces)
	{
		std::vector<int> polygon;
		for (int k = 0; k < 3; ++k)
		{
			const int a = f.vertex[k], b = f.vertex[(k + 1) % 3];
			if (value[a] > 0.0) polygon.push_back(a);
			if ((value[a] > 0.0) == (value[b] > 0.0)) continue;
			auto [it, added] = crossings.emplace(EdgeKey(a, b), int(positions.size()));
			if (added)
			{
				double low = 0.0, high = 1.0;
				for (int iteration = 0; iteration < 12; ++iteration)
				{
					const double fraction = (low + high) * 0.5;
					const EVec q = positions[a] * (1.0 - fraction) + positions[b] * fraction;
					if ((field.Bilinear(P2(q.x, q.y)) > 0.0) == (value[a] > 0.0)) low = fraction;
					else high = fraction;
				}
				const double fraction = (low + high) * 0.5;
				positions.push_back(positions[a] * (1.0 - fraction) + positions[b] * fraction);
				normals.push_back(normalize(normals[a] * (1.0 - fraction) + normals[b] * fraction));
			}
			polygon.push_back(it->second);
		}
		if (polygon.size() < 3) continue;
		faceV.insert(faceV.end(), polygon.begin(), polygon.end());
		faceN.push_back(int(polygon.size()));
		sources.push_back(f.source);
	}
	std::vector<int> component(positions.size()), sourceComponent(surface.points.size());
	for (size_t i = 0; i < component.size(); ++i) component[i] = int(i);
	for (size_t i = 0; i < sourceComponent.size(); ++i) sourceComponent[i] = int(i);
	auto root = [](std::vector<int>& parents, int vertex) {
		while (parents[vertex] != vertex) { parents[vertex] = parents[parents[vertex]]; vertex = parents[vertex]; }
		return vertex;
	};
	for (const auto& t : surface.triangles)
		for (int k = 1; k < 3; ++k) sourceComponent[root(sourceComponent, t[k])] = root(sourceComponent, t[0]);
	size_t offset = 0;
	for (int count : faceN)
	{
		for (int k = 1; k < count; ++k) component[root(component, faceV[offset + k])] = root(component, faceV[offset]);
		offset += count;
	}
	std::vector<double> area(positions.size(), 0.0), largest(surface.points.size(), 0.0);
	offset = 0;
	for (size_t f = 0; f < faceN.size(); ++f)
	{
		const int group = root(component, faceV[offset]);
		for (int k = 1; k + 1 < faceN[f]; ++k)
			area[group] += 0.5 * manifold::la::length(manifold::la::cross(
				positions[faceV[offset + k]] - positions[faceV[offset]],
				positions[faceV[offset + k + 1]] - positions[faceV[offset]]));
		offset += faceN[f];
	}
	offset = 0;
	for (size_t f = 0; f < faceN.size(); ++f)
	{
		const int original = root(sourceComponent, surface.triangles[sources[f]][0]);
		largest[original] = std::max(largest[original], area[root(component, faceV[offset])]);
		offset += faceN[f];
	}
	std::vector<int> keptVerts, keptSizes, keptSources;
	std::unordered_set<int> keptGroups;
	offset = 0;
	for (size_t f = 0; f < faceN.size(); ++f)
	{
		const int group = root(component, faceV[offset]);
		const int original = root(sourceComponent, surface.triangles[sources[f]][0]);
		const double minimum = largest[original] * (prm.keepPieces ? Clamp(prm.minPiece, 0.0, 1.0) : 1.0);
		const bool flake = area[group] < prm.tornScale * prm.tornScale * 0.6
			&& area[group] > 2.0 * h * h && Rng(Hash(uint64_t(group) + uint64_t(prm.seed))).R(0.0, 1.0) < prm.flakes;
		if (area[group] >= minimum - 1e-16 || flake)
		{
			keptVerts.insert(keptVerts.end(), faceV.begin() + offset, faceV.begin() + offset + faceN[f]);
			keptSizes.push_back(faceN[f]); keptSources.push_back(sources[f]); keptGroups.insert(group);
		}
		offset += faceN[f];
	}
	faceV.swap(keptVerts); faceN.swap(keptSizes); sources.swap(keptSources);
	pieceCount = int(keptGroups.size());
	std::vector<int> remap(positions.size(), -1);
	std::vector<EVec> compact, compactNormals;
	for (int& v : faceV)
	{
		if (remap[v] < 0)
		{
			remap[v] = int(compact.size()); compact.push_back(positions[v]); compactNormals.push_back(normals[v]);
		}
		v = remap[v];
	}
	positions.swap(compact); normals.swap(compactNormals);
	return !faceN.empty();
}

// Crumple height: octaves of random-height piecewise linear facets.
struct CrumpleOctave
{
	Delaunay dt;
	std::vector<double> h;
	double size = 1.0, amp = 0.0;
	uint64_t key = 0;
	int hint = 0;
};

} // namespace

static bool BuildPaperImpl(const PaperGenParams& prm, const PaperGenPaint& paint, PaperGenResult& out, const PaperGenSurface* surface)
{
	out = PaperGenResult{};
	const double W = std::max(prm.width, 1e-6), H = std::max(prm.height, 1e-6);
	const double hw = 0.5 * W, hh = 0.5 * H;
	double h = std::max(prm.resolution, 1e-6);
	{
		const double est = (W * H) / (h * h) * (prm.quads ? 1.0 : 1.15) * (prm.thickness > 0.0 ? 2.0 : 1.0);
		const double cap = std::max(1000.0, double(prm.maxVerts));
		if (est > cap) h *= std::sqrt(est / cap);
		h = std::max(h, std::min(W, H) / 4000.0);   // keep the field grid bounded
	}
	out.resolution = h;
	const bool uncutSurface = surface != nullptr && paint.tears.empty() && paint.holes.empty()
		&& prm.holeAmount == 0 && prm.cornerTears == 0 && prm.edgeBites == 0;
	const double g = uncutSurface ? std::max(0.5 * h, std::min(W, H) / 128.0) : 0.5 * h;
	const uint64_t seed = Hash(uint64_t(uint32_t(prm.seed)) + 0x5a17ULL);
	auto ch = [&](uint64_t c) { return Mix(seed, c); };

	const double rag = std::max(prm.ragSize, 2.0 * g);
	const double jag = Clamp(prm.jagged, 0.0, 1.0);
	const double fib = Clamp(prm.fiber, 0.0, 1.0);
	const double warpA = jag * rag * 0.45;
	const double fiberS = std::max(rag * 0.25, 2.5 * g);
	const double fiberA = fib * fiberS * 0.45;
	const double warpMax = 1.2 * (warpA + fiberA);

	// ---- shapes ---------------------------------------------------------------
	auto toShape = [&](const PaperGenStroke& s, bool& closed) -> Shape2
	{
		Shape2 sh;
		for (const EVec& v : s.pts)
		{
			if (!std::isfinite(v.x) || !std::isfinite(v.y)) continue;
			const P2 q(v.x, v.y);
			if (sh.pts.empty() || Len(q - sh.pts.back()) > 1e-9) sh.pts.push_back(q);
		}
		if (sh.pts.size() < 2) { sh.pts.clear(); return sh; }
		const double L = PolyLength(sh.pts, false);
		closed = sh.pts.size() >= 3 && (s.cyclic || Len(sh.pts.front() - sh.pts.back()) <= std::max(prm.closeShare, 0.0) * L);
		sh.closed = closed;
		return sh;
	};
	// nearest point on the sheet border pushed outside: lets a tear line reach the border
	auto borderOut = [&](const P2& q, double push) -> P2
	{
		const double dl = q.x + hw, dr = hw - q.x, db = q.y + hh, dt = hh - q.y;
		const double m = std::min({ dl, dr, db, dt });
		if (m == dl) return P2(-hw - push, Clamp(q.y, -hh, hh));
		if (m == dr) return P2(hw + push, Clamp(q.y, -hh, hh));
		if (m == db) return P2(Clamp(q.x, -hw, hw), -hh - push);
		return P2(Clamp(q.x, -hw, hw), hh + push);
	};
	auto insideDist = [&](const P2& q) { return std::min({ q.x + hw, hw - q.x, q.y + hh, hh - q.y }); };

	std::vector<Shape2> regions;   // holes (rounded style), field = signed distance
	std::vector<Shape2> tearRegions;   // closed Tear strokes: areas torn away (torn style)
	std::vector<Shape2> slits;     // tear lines: thin cuts that separate pieces
	std::vector<double> slitHalf;
	std::vector<Shape2> gashes;    // open hole strokes
	const double slitHalfW = 0.5 * std::max(prm.tearGap, 2.4 * g);

	bool anyPaint = false;
	for (const PaperGenStroke& s : paint.tears)
	{
		bool closed = false;
		Shape2 sh = toShape(s, closed);
		if (sh.pts.empty()) continue;
		anyPaint = true;
		if (closed)
		{
			sh.pts = Resample(sh.pts, true, g * 1.5, 8);
			tearRegions.push_back(std::move(sh));
			++out.tears;
			continue;
		}
		// extend ends that stop near (or beyond) the border to just outside it
		const double snap = std::max(prm.tearSnap, 0.0) + warpMax;
		const double push = 3.0 * g + warpMax + slitHalfW;
		if (insideDist(sh.pts.front()) < snap) sh.pts.insert(sh.pts.begin(), borderOut(sh.pts.front(), push));
		if (insideDist(sh.pts.back()) < snap) sh.pts.push_back(borderOut(sh.pts.back(), push));
		sh.pts = Resample(sh.pts, false, g * 1.5, 2);
		slits.push_back(std::move(sh));
		slitHalf.push_back(slitHalfW);
		++out.tears;
	}
	for (const PaperGenStroke& s : paint.holes)
	{
		bool closed = false;
		Shape2 sh = toShape(s, closed);
		if (sh.pts.empty()) continue;
		anyPaint = true;
		sh.pts = Resample(sh.pts, closed, g * 1.5, closed ? 8 : 2);
		if (closed) regions.push_back(std::move(sh)); else gashes.push_back(std::move(sh));
		++out.holes;
	}
	const bool procedural = !(prm.paintOnly && anyPaint);

	// procedural holes: lobed ellipses
	struct Hole { P2 c; double r, irr, ang, ex, ey; double ak[4], ph[4]; };
	std::vector<Hole> holes;
	if (procedural && prm.holeAmount > 0)
	{
		const double rMin = std::max(std::min(prm.holeSizeMin, prm.holeSizeMax), 1.5 * h);
		const double rMax = std::max({ prm.holeSizeMin, prm.holeSizeMax, rMin });
		for (int i = 0; i < std::min(prm.holeAmount, 4096); ++i)
		{
			Rng rr(Mix(ch(kChHoles), uint64_t(i)));
			Hole ho;
			ho.r = rr.R(rMin, rMax);
			const double el = Clamp(prm.holeElongation, 0.0, 1.0) * rr.U();
			ho.ex = 1.0 + 0.8 * el; ho.ey = 1.0 / (1.0 + 0.5 * el);
			const double reach = ho.r * ho.ex + std::max(prm.holeMargin, 0.0);
			const double ax = std::max(0.0, hw - reach), ay = std::max(0.0, hh - reach);
			ho.c = P2(rr.R(-ax, ax), rr.R(-ay, ay));
			ho.ang = rr.R(0.0, kPi);
			ho.irr = Clamp(prm.holeIrregular, 0.0, 1.0);
			double norm = 0.0;
			for (int k = 0; k < 4; ++k) { ho.ak[k] = rr.U() / (k + 1.5); ho.ph[k] = rr.R(0.0, 2.0 * kPi); norm += ho.ak[k]; }
			for (int k = 0; k < 4; ++k) ho.ak[k] *= 0.45 / std::max(norm, 1e-9);
			holes.push_back(ho);
			++out.holes;
		}
	}
	// procedural corner tears and border bites: curved tear lines from border to border
	if (procedural && prm.cornerTears > 0)
	{
		const P2 corners[4] = { P2(-hw, -hh), P2(hw, -hh), P2(hw, hh), P2(-hw, hh) };
		const P2 legA[4] = { P2(1, 0), P2(0, 1), P2(-1, 0), P2(0, -1) };   // along the border, CCW
		const P2 legB[4] = { P2(0, 1), P2(-1, 0), P2(0, -1), P2(1, 0) };   // along the border, CW
		int order[4] = { 0, 1, 2, 3 };
		Rng ro(ch(kChCorners));
		for (int i = 3; i > 0; --i) std::swap(order[i], order[ro.I(i + 1)]);
		const double push = 3.0 * g + warpMax + slitHalfW;
		for (int n = 0; n < std::min(prm.cornerTears, 4); ++n)
		{
			const int c = order[n];
			Rng rr(Mix(ch(kChCorners), uint64_t(c) + 101));
			const double sMax = 0.45 * std::min(W, H);
			const double s = std::min(rr.R(std::min(prm.cornerSizeMin, prm.cornerSizeMax), std::max(prm.cornerSizeMin, prm.cornerSizeMax)), sMax);
			const double la = s * rr.R(0.6, 1.4), lb = s * rr.R(0.6, 1.4);
			const P2 A = corners[c] + legA[c] * std::min(la, sMax) + legB[c] * (-push);
			const P2 B = corners[c] + legB[c] * std::min(lb, sMax) + legA[c] * (-push);
			const P2 mid = (corners[c] + legA[c] * la * 0.5 + legB[c] * lb * 0.5);
			const P2 inward = (legA[c] + legB[c]) * (1.0 / std::sqrt(2.0));
			const P2 ctrl = mid + inward * (s * rr.R(-0.25, 0.35));
			Shape2 sh; sh.pts = Resample(Bezier(A, ctrl, B, 24), false, g * 1.5, 2);
			slits.push_back(std::move(sh)); slitHalf.push_back(slitHalfW);
			++out.tears;
		}
	}
	if (procedural && prm.edgeBites > 0)
	{
		const double per = 2.0 * (W + H);
		const double push = 3.0 * g + warpMax + slitHalfW;
		for (int i = 0; i < std::min(prm.edgeBites, 256); ++i)
		{
			Rng rr(Mix(ch(kChBites), uint64_t(i)));
			const double w = std::min(rr.R(std::min(prm.biteSizeMin, prm.biteSizeMax), std::max(prm.biteSizeMin, prm.biteSizeMax)), 0.6 * std::min(W, H));
			const double depth = std::min(w * std::max(prm.biteDepth, 0.05) * rr.R(0.7, 1.3), 0.45 * std::min(W, H));
			double s = rr.R(0.0, per);
			P2 o, t, nIn;   // point on the border, tangent, inward normal
			double sideLen;
			if (s < W) { o = P2(-hw + s, -hh); t = P2(1, 0); nIn = P2(0, 1); sideLen = W; }
			else if ((s -= W) < H) { o = P2(hw, -hh + s); t = P2(0, 1); nIn = P2(-1, 0); sideLen = H; }
			else if ((s -= H) < W) { o = P2(hw - s, hh); t = P2(-1, 0); nIn = P2(0, -1); sideLen = W; }
			else { s -= W; o = P2(-hw, hh - s); t = P2(0, -1); nIn = P2(1, 0); sideLen = H; }
			(void)sideLen;
			const double skew = rr.R(-0.3, 0.3) * w;
			const P2 A = o - t * (0.5 * w) - nIn * push;
			const P2 B = o + t * (0.5 * w) - nIn * push;
			const P2 ctrl = o + t * skew + nIn * (2.0 * depth + push);
			Shape2 sh; sh.pts = Resample(Bezier(A, ctrl, B, 24), false, g * 1.5, 2);
			slits.push_back(std::move(sh)); slitHalf.push_back(slitHalfW);
			++out.tears;
		}
	}
	for (Shape2& s : regions) s.Bounds();
	for (Shape2& s : tearRegions) s.Bounds();
	for (Shape2& s : slits) s.Bounds();
	for (Shape2& s : gashes) s.Bounds();

	// ---- field grid -------------------------------------------------------------
	const double cap = std::max(8.0 * h, 2.0 * rag);
	const double tornA = std::max(prm.tornWidth, 0.0);
	const double tornS = std::max(prm.tornScale, 4.0 * g);
	const double tornBound = 2.4 * tornA + 4.0 * g;   // |torn noise| never exceeds this
	const double margin = 6.0 * g + 2.0 * warpMax + rag + tornBound;
	Grid G;
	G.g = g; G.x0 = -hw - margin; G.y0 = -hh - margin;
	G.nx = int(std::ceil((W + 2 * margin) / g)) + 1;
	G.ny = int(std::ceil((H + 2 * margin) / g)) + 1;
	const size_t NN = size_t(G.nx) * G.ny;
	G.f.assign(NN, 0.0);
	G.torn.assign(NN, 0);
	std::vector<double> fTorn(NN, cap);   // torn features only (for the torn attribute)
	// domain warp of the torn features, evaluated lazily (only near a torn feature)
	std::vector<P2> wpv(NN);
	std::vector<uint8_t> wpDone(NN, 0);
	const uint64_t kwx = ch(kChWarpX), kwy = ch(kChWarpY), kfx = ch(kChFiberX), kfy = ch(kChFiberY), kro = ch(kChRough);
	auto wp = [&](size_t id) -> const P2&
	{
		if (!wpDone[id])
		{
			const P2 q = G.Pos(int(id % G.nx), int(id / G.nx));
			P2 w = q;
			if (warpA > 0.0) w = w + P2(Fbm(kwx, q.x / rag, q.y / rag, 3), Fbm(kwy, q.x / rag, q.y / rag, 3)) * warpA;
			wpv[id] = w; wpDone[id] = 1;
		}
		return wpv[id];
	};
	const double roughA = Clamp(prm.edgeRough, 0.0, 1.0) * std::max(rag * 0.12, g * 0.6);
	for (int j = 0; j < G.ny; ++j)
		for (int i = 0; i < G.nx; ++i)
		{
			const P2 q = G.Pos(i, j);
			double fr = std::min({ q.x + hw, hw - q.x, q.y + hh, hh - q.y });
			if (roughA > 0.0 && std::fabs(fr) < cap + roughA) fr += roughA * Fbm(kro, q.x / (rag * 0.35), q.y / (rag * 0.35), 3);
			G.f[G.Id(i, j)] = Clamp(fr, -cap, cap);
		}
	// holes
	for (const Hole& ho : holes)
	{
		const double reach = ho.r * ho.ex * (1.0 + ho.irr) + warpMax + cap;
		const double ca = std::cos(ho.ang), sa = std::sin(ho.ang);
		const int i0 = std::max(0, int((ho.c.x - reach - G.x0) / g)), i1 = std::min(G.nx - 1, int((ho.c.x + reach - G.x0) / g) + 1);
		const int j0 = std::max(0, int((ho.c.y - reach - G.y0) / g)), j1 = std::min(G.ny - 1, int((ho.c.y + reach - G.y0) / g) + 1);
		for (int j = j0; j <= j1; ++j)
			for (int i = i0; i <= i1; ++i)
			{
				const size_t id = G.Id(i, j);
				const P2 d = wp(id) - ho.c;
				const P2 l(( d.x * ca + d.y * sa) / ho.ex, (-d.x * sa + d.y * ca) / ho.ey);
				const double th = std::atan2(l.y, l.x);
				double rr = 0.0;
				for (int k = 0; k < 4; ++k) rr += ho.ak[k] * std::cos((k + 2) * th + ho.ph[k]);
				const double rad = ho.r * (1.0 + ho.irr * rr);
				const double v = (Len(l) - rad) * std::min(ho.ey, 1.0);
				fTorn[id] = std::min(fTorn[id], v);
			}
	}
	auto stampShape = [&](const Shape2& s, double half, bool signedRegion)
	{
		const double reach = warpMax + cap + half;
		const int i0 = std::max(0, int((s.minX - reach - G.x0) / g)), i1 = std::min(G.nx - 1, int((s.maxX + reach - G.x0) / g) + 1);
		const int j0 = std::max(0, int((s.minY - reach - G.y0) / g)), j1 = std::min(G.ny - 1, int((s.maxY + reach - G.y0) / g) + 1);
		for (int j = j0; j <= j1; ++j)
			for (int i = i0; i <= i1; ++i)
			{
				const size_t id = G.Id(i, j);
				// region: signed distance, positive OUTSIDE the drawn area; gash: a band around the stroke
				const double d = ShapeDist(s, wp(id));
				fTorn[id] = std::min(fTorn[id], signedRegion ? d : d - half);
			}
	};
	for (const Shape2& s : regions) stampShape(s, 0.0, true);
	for (const Shape2& s : gashes) stampShape(s, 0.5 * std::max(prm.strokeHoleWidth, 2.4 * g), false);
	// round the holes / torn-away areas (Gaussian blur of their field only: the
	// sheet's own corners stay sharp), then combine with the sheet
	for (size_t id = 0; id < NN; ++id) fTorn[id] = Clamp(fTorn[id], -cap, cap);
	GaussianBlur(fTorn, G.nx, G.ny, Clamp(prm.roundness, 0.0, 1.0) * rag * 0.35 / g);
	for (size_t id = 0; id < NN; ++id)
		if (fTorn[id] < G.f[id]) { G.f[id] = fTorn[id]; G.torn[id] = 1; }
	// fine fibrous jaggedness on the rounded holes: added to the field after the blur,
	// so Roundness rounds the shape but not the torn fibres
	if (fiberA > 0.0)
		for (size_t id = 0; id < NN; ++id)
		{
			if (!G.torn[id] || std::fabs(G.f[id]) > 2.0 * fiberA + 2.0 * g) continue;
			const P2 q = G.Pos(int(id % G.nx), int(id / G.nx));
			G.f[id] += fiberA * (0.7 * Fbm(kfx, q.x / fiberS, q.y / fiberS, 3) + 0.3 * Fbm(kfy, q.x / (fiberS * 0.45), q.y / (fiberS * 0.45), 2));
		}
	// ---- torn edges: Tear strokes, corner tears, border bites ---------------------
	// The strokes only say WHICH area goes, not the exact torn form:
	// (1) the removed area is found on the grid without any noise - closed strokes
	//     are filled, open tear lines cut the sheet and every piece but the
	//     largest (or the kept ones) goes;
	// (2) its exact signed distance field gets the torn-paper noise: big lobes,
	//     long sharp tongues pulled along Tongue Angle and fractal fibres down to
	//     the resolution. The outline wanders around the drawn line by up to Torn
	//     Width, with tongues, notches and loose flakes, like a poster torn off a wall.
	if (!tearRegions.empty() || !slits.empty())
	{
		const int si0 = std::max(0, int(std::ceil((-hw - G.x0) / g))), si1 = std::min(G.nx - 1, int(std::floor((hw - G.x0) / g)));
		const int sj0 = std::max(0, int(std::ceil((-hh - G.y0) / g))), sj1 = std::min(G.ny - 1, int(std::floor((hh - G.y0) / g)));
		std::vector<uint8_t> rem(NN, 0), band(NN, 0);
		auto boxOf = [&](const Shape2& sh, double reach, int& i0, int& i1, int& j0, int& j1)
		{
			i0 = std::max(si0, int((sh.minX - reach - G.x0) / g)); i1 = std::min(si1, int((sh.maxX + reach - G.x0) / g) + 1);
			j0 = std::max(sj0, int((sh.minY - reach - G.y0) / g)); j1 = std::min(sj1, int((sh.maxY + reach - G.y0) / g) + 1);
		};
		// closed areas: even-odd scanline fill
		std::vector<double> xs;
		for (const Shape2& sh : tearRegions)
		{
			int i0, i1, j0, j1; boxOf(sh, g, i0, i1, j0, j1);
			const size_t n = sh.pts.size();
			for (int j = j0; j <= j1; ++j)
			{
				const double y = G.y0 + j * g;
				xs.clear();
				for (size_t k = 0; k < n; ++k)
				{
					const P2& a = sh.pts[k];
					const P2& b = sh.pts[(k + 1) % n];
					if ((a.y > y) != (b.y > y)) xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
				}
				std::sort(xs.begin(), xs.end());
				for (size_t k = 0; k + 1 < xs.size(); k += 2)
				{
					const int ia = std::max(i0, int(std::ceil((xs[k] - G.x0) / g))), ib = std::min(i1, int(std::floor((xs[k + 1] - G.x0) / g)));
					for (int i = ia; i <= ib; ++i) rem[G.Id(i, j)] = 1;
				}
			}
		}
		// tear lines: a band along every segment
		for (size_t si = 0; si < slits.size(); ++si)
		{
			const Shape2& sh = slits[si];
			const double half = std::max(slitHalf[si], 1.1 * g);
			for (size_t k = 0; k + 1 < sh.pts.size(); ++k)
			{
				const P2 &a = sh.pts[k], &b = sh.pts[k + 1];
				const int i0 = std::max(si0, int((std::min(a.x, b.x) - half - G.x0) / g)), i1 = std::min(si1, int((std::max(a.x, b.x) + half - G.x0) / g) + 1);
				const int j0 = std::max(sj0, int((std::min(a.y, b.y) - half - G.y0) / g)), j1 = std::min(sj1, int((std::max(a.y, b.y) + half - G.y0) / g) + 1);
				for (int j = j0; j <= j1; ++j) for (int i = i0; i <= i1; ++i)
					if (SegDist2(G.Pos(i, j), a, b) < half * half) band[G.Id(i, j)] = 1;
			}
		}
		// pieces of the sheet left by the cut
		std::vector<int> lab(NN, -1);
		std::vector<size_t> sz;
		{
			std::vector<size_t> st;
			for (int j = sj0; j <= sj1; ++j)
				for (int i = si0; i <= si1; ++i)
				{
					const size_t s0 = G.Id(i, j);
					if (rem[s0] || band[s0] || lab[s0] >= 0) continue;
					const int L = int(sz.size());
					size_t cnt = 0;
					st.push_back(s0); lab[s0] = L;
					while (!st.empty())
					{
						const size_t id = st.back(); st.pop_back(); ++cnt;
						const int ci = int(id % G.nx), cj = int(id / G.nx);
						const int nb[4][2] = { { ci - 1, cj }, { ci + 1, cj }, { ci, cj - 1 }, { ci, cj + 1 } };
						for (auto& q : nb)
						{
							if (q[0] < si0 || q[1] < sj0 || q[0] > si1 || q[1] > sj1) continue;
							const size_t k = G.Id(q[0], q[1]);
							if (!rem[k] && !band[k] && lab[k] < 0) { lab[k] = L; st.push_back(k); }
						}
					}
					sz.push_back(cnt);
				}
		}
		std::vector<uint8_t> keepL(sz.size(), 0);
		if (!sz.empty())
		{
			const size_t bigL = size_t(std::max_element(sz.begin(), sz.end()) - sz.begin());
			for (size_t L = 0; L < sz.size(); ++L)
				keepL[L] = L == bigL || (prm.keepPieces && double(sz[L]) >= std::max(16.0, Clamp(prm.minPiece, 0.0, 1.0) * double(sz[bigL])));
		}
		// removed = inside the sheet and not part of a kept piece; outside the sheet the
		// nearest sheet node decides (so a torn corner does not leave a strip along the border)
		std::vector<uint8_t> gone(NN, 0);
		for (int j = 0; j < G.ny; ++j)
			for (int i = 0; i < G.nx; ++i)
			{
				const size_t k = G.Id(std::clamp(i, si0, si1), std::clamp(j, sj0, sj1));
				gone[G.Id(i, j)] = (lab[k] < 0 || !keepL[lab[k]]) ? 1 : 0;
			}
		// exact signed distance to the removed area (Felzenszwalb EDT), lightly smoothed
		std::vector<double> dIn = SquaredEdt(gone, G.nx, G.ny, 1), dOut = SquaredEdt(gone, G.nx, G.ny, 0);
		std::vector<double> sd(NN);
		for (size_t id = 0; id < NN; ++id)
			sd[id] = gone[id] ? -(std::sqrt(dOut[id]) - 0.5) * g : (std::sqrt(dIn[id]) - 0.5) * g;
		for (double& v : sd) v = Clamp(v, -cap - tornBound, cap + tornBound);
		GaussianBlur(sd, G.nx, G.ny, 0.8);
		// torn-paper noise
		const uint64_t kLobe = ch(kChTornLobe), kTongue = ch(kChTongue), kTdir = ch(kChTongueDir), kFine = ch(kChTornFine);
		const double tg = Clamp(prm.tongues, 0.0, 1.0), det = Clamp(prm.tornDetail, 0.0, 1.0);
		const double stretch = Clamp(prm.tongueStretch, 1.0, 8.0);
		const double ang0 = prm.tongueAngle * kPi / 180.0, angR = Clamp(prm.tongueAngleRandom, 0.0, 1.0) * kPi;
		const double tongueW = tornS * 0.6;
		const double fineS = tornS * 0.22;
		const int fineOct = std::clamp(int(std::floor(std::log2(fineS / (2.0 * g)))) + 1, 1, 5);
		for (size_t id = 0; id < NN; ++id)
		{
			double v = sd[id];
			if (tornA > 0.0 && std::fabs(v) < tornBound)
			{
				const P2 q = G.Pos(int(id % G.nx), int(id / G.nx));
				const double lobes = Fbm(kLobe, q.x / tornS, q.y / tornS, 4);
				double tongue = 0.0;
				if (tg > 0.0)
				{
					const double a = ang0 + angR * Fbm(kTdir, q.x / (tornS * 4.0), q.y / (tornS * 4.0), 2);
					const double ca = std::cos(a), sa = std::sin(a);
					const double u = (ca * q.x + sa * q.y) / (tongueW * stretch), w = (-sa * q.x + ca * q.y) / tongueW;
					double r = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
					for (int o = 0; o < 3; ++o)
					{
						const double n = 1.0 - std::fabs(VNoise(Mix(kTongue, uint64_t(o)), u * f + 0.31 * o, w * f - 0.17 * o));
						r += amp * n * n * std::sqrt(n); norm += amp; amp *= 0.55; f *= 2.1;
					}
					// fingers of paper pulled out along the tear direction: strongest where the
					// torn border faces that direction (or away from it), weak along it
					const int ci = int(id % G.nx), cj = int(id / G.nx);
					const double gx = sd[G.Id(std::min(ci + 1, G.nx - 1), cj)] - sd[G.Id(std::max(ci - 1, 0), cj)];
					const double gy = sd[G.Id(ci, std::min(cj + 1, G.ny - 1))] - sd[G.Id(ci, std::max(cj - 1, 0))];
					const double gl = std::sqrt(gx * gx + gy * gy);
					const double face = gl > 1e-12 ? std::fabs(gx * ca + gy * sa) / gl : 1.0;
					tongue = (0.3 + 0.7 * face) * 1.8 * (r / norm - 0.42);
				}
				double fine = 0.0;
				if (det > 0.0)
				{
					// smooth fibres + sharp little teeth (ridged)
					double r = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
					for (int o = 0; o < fineOct; ++o)
					{
						const double n = 1.0 - std::fabs(VNoise(Mix(kFine, uint64_t(o) + 50), q.x / fineS * f, q.y / fineS * f));
						r += amp * n * n; norm += amp; amp *= 0.55; f *= 2.05;
					}
					fine = 0.5 * Fbm(kFine, q.x / fineS, q.y / fineS, fineOct) + 1.1 * (r / norm - 0.45);
				}
				v += tornA * (1.1 * lobes + tg * 0.9 * tongue + det * 0.4 * fine);
			}
			v = Clamp(v, -cap, cap);
			if (v < G.f[id]) { G.f[id] = v; G.torn[id] = 1; }
			else if (v < 3.0 * g) G.torn[id] = 1;
		}
		// Keep Pieces: the noise must not weld the pieces together again -> cut the
		// tear lines in once more as thin warped slits
		if (prm.keepPieces)
		for (size_t si = 0; si < slits.size(); ++si)
		{
			const Shape2& s = slits[si];
			const double half = slitHalf[si];
			const double reach = warpMax + half + 4.0 * g;
			const int i0 = std::max(0, int((s.minX - reach - G.x0) / g)), i1 = std::min(G.nx - 1, int((s.maxX + reach - G.x0) / g) + 1);
			const int j0 = std::max(0, int((s.minY - reach - G.y0) / g)), j1 = std::min(G.ny - 1, int((s.maxY + reach - G.y0) / g) + 1);
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					const size_t id = G.Id(i, j);
					P2 w = wp(id);
					if (fiberA > 0.0)
					{
						const P2 q = G.Pos(i, j);
						w = w + P2(Fbm(kfx, q.x / fiberS, q.y / fiberS, 3), Fbm(kfy, q.x / fiberS, q.y / fiberS, 3)) * (0.8 * fiberA);
					}
					const double v = ShapeDist(s, w) - half;
					if (v < G.f[id]) { G.f[id] = v; G.torn[id] = 1; }
					else if (v < 4.0 * g) G.torn[id] = 1;
				}
		}
	}
	// nodes next to a torn feature count as torn (the attribute samples near the zero line)
	for (size_t id = 0; id < NN; ++id) if (fTorn[id] < 3.0 * g && fTorn[id] <= G.f[id] + 3.0 * g) G.torn[id] = 1;
	// the grid frame is always void
	for (int i = 0; i < G.nx; ++i) { G.f[G.Id(i, 0)] = -cap; G.f[G.Id(i, G.ny - 1)] = -cap; }
	for (int j = 0; j < G.ny; ++j) { G.f[G.Id(0, j)] = -cap; G.f[G.Id(G.nx - 1, j)] = -cap; }

	// ---- pieces: keep the largest (or all big enough) ---------------------------
	{
		std::vector<int> label(NN, -1);
		std::vector<size_t> sizes;
		std::vector<size_t> stack;
		for (size_t s0 = 0; s0 < NN; ++s0)
		{
			if (G.f[s0] <= 0.0 || label[s0] >= 0) continue;
			const int L = int(sizes.size());
			size_t cnt = 0;
			stack.push_back(s0); label[s0] = L;
			while (!stack.empty())
			{
				const size_t id = stack.back(); stack.pop_back(); ++cnt;
				const int i = int(id % G.nx), j = int(id / G.nx);
				const int nb[4][2] = { { i - 1, j }, { i + 1, j }, { i, j - 1 }, { i, j + 1 } };
				for (auto& q : nb)
				{
					if (q[0] < 0 || q[1] < 0 || q[0] >= G.nx || q[1] >= G.ny) continue;
					const size_t k = G.Id(q[0], q[1]);
					if (G.f[k] > 0.0 && label[k] < 0) { label[k] = L; stack.push_back(k); }
				}
			}
			sizes.push_back(cnt);
		}
		if (sizes.empty()) return false;
		const size_t big = *std::max_element(sizes.begin(), sizes.end());
		int bigL = int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
		std::vector<uint8_t> keep(sizes.size(), 0);
		const double minCells = std::max(16.0, (prm.keepPieces ? Clamp(prm.minPiece, 0.0, 1.0) : 1.0) * double(big));
		// flakes: small loose bits the torn-edge noise left near a torn border
		const double flakeMin = std::max(12.0, 2.0 * (h / g) * (h / g));
		const double flakeMax = std::max(flakeMin, 0.6 * (std::max(prm.tornScale, 4.0 * g) / g) * (std::max(prm.tornScale, 4.0 * g) / g));
		const uint64_t kFl = ch(kChFlake);
		for (size_t L = 0; L < sizes.size(); ++L)
		{
			keep[L] = prm.keepPieces ? (double(sizes[L]) >= minCells || int(L) == bigL) : int(L) == bigL;
			if (!keep[L] && prm.flakes > 0.0 && double(sizes[L]) >= flakeMin && double(sizes[L]) <= flakeMax)
				keep[L] = (double(Mix(kFl, uint64_t(L)) >> 11) * (1.0 / 9007199254740992.0)) < Clamp(prm.flakes, 0.0, 1.0);
		}
		out.pieces = 0;
		for (size_t L = 0; L < sizes.size(); ++L) out.pieces += keep[L];
		for (size_t id = 0; id < NN; ++id)
			if (label[id] >= 0 && !keep[label[id]]) G.f[id] = -std::max(std::fabs(G.f[id]), g);
	}

	// ---- boundary loops -----------------------------------------------------------
	std::vector<std::vector<P2>> raw = Contour(G);
	std::vector<std::vector<P2>> loops;
	for (auto& L : raw)
	{
		const double len = PolyLength(L, true);
		const double area = std::fabs(SignedArea(L));
		if (len < 3.0 * h || area < 0.5 * h * h) continue;   // dropping a loop only fills a speck / removes a crumb
		std::vector<P2> r = Resample(L, true, h, 4);
		if (std::fabs(SignedArea(r)) < 1e-3 * area) continue;
		loops.push_back(std::move(r));
	}
	// resampling may cut a corner into a neighbouring loop: then that loop keeps a denser sampling
	{
		auto anyCross = [&](size_t li) -> bool
		{
			PointHash ph; ph.cell = 2.0 * h;
			std::vector<std::pair<size_t, size_t>> segs;
			for (size_t a = 0; a < loops.size(); ++a)
				for (size_t i = 0; i < loops[a].size(); ++i) { ph.Add((loops[a][i] + loops[a][(i + 1) % loops[a].size()]) * 0.5, int(segs.size())); segs.push_back({ a, i }); }
			const auto& A = loops[li];
			for (size_t i = 0; i < A.size(); ++i)
			{
				const P2 &a0 = A[i], &a1 = A[(i + 1) % A.size()];
				bool hit = false;
				ph.Near((a0 + a1) * 0.5, 2.5 * h, [&](int sid)
				{
					if (hit) return;
					const auto& sg = segs[sid];
					const auto& B = loops[sg.first];
					const size_t j = sg.second;
					if (sg.first == li && (j == i || (j + 1) % A.size() == i || (i + 1) % A.size() == j)) return;
					if (SegCross(a0, a1, B[j], B[(j + 1) % B.size()])) hit = true;
				});
				if (hit) return true;
			}
			return false;
		};
		for (int pass = 0; pass < 3; ++pass)
		{
			bool changed = false;
			for (size_t li = 0; li < loops.size(); ++li)
				if (anyCross(li))
				{
					// re-derive this loop from the raw contour (densest sampling we have)
					const P2 c0 = loops[li][0];
					for (auto& Lr : raw)
					{
						bool match = false;
						for (const P2& q : Lr) if (Len(q - c0) < 1.5 * h) { match = true; break; }
						if (match && std::fabs(SignedArea(Lr)) > 0.5 * h * h) { loops[li] = Resample(Lr, true, h * (pass == 0 ? 0.6 : 0.5), 4); changed = true; break; }
					}
				}
			if (!changed) break;
		}
	}
	if (loops.empty()) return false;
	out.loops = int(loops.size());

	std::vector<int> faceV, faceN, sourceFaces;
	std::vector<P2> P;
	std::vector<EVec> basePositions, baseNormals;
	if (surface != nullptr)
	{
		const int budget = std::max<int>(int(surface->points.size()), prm.maxVerts / (prm.thickness > 0.0 ? 2 : 1));
		if (!PaperMeshTopology(*surface, G, h, budget, uncutSurface, prm, out.pieces, basePositions, baseNormals, faceV, faceN, sourceFaces)) return false;
		for (const EVec& q : basePositions) P.push_back(P2(q.x, q.y));
	}
	else
	{
	// ---- triangulation ------------------------------------------------------------
	Delaunay dt;
	const double dup = 1e-4 * h;
	std::vector<std::vector<int>> loopIds(loops.size());
	PointHash bh; bh.cell = h;
	std::vector<P2> bpts;
	for (size_t li = 0; li < loops.size(); ++li)
		for (const P2& q : loops[li]) { bh.Add(q, int(bpts.size())); bpts.push_back(q); }
	// interior grid points first (spatially coherent order), then the loops
	const int gi0 = int(std::floor((-hw) / h)) - 1, gi1 = int(std::ceil(hw / h)) + 1;
	const int gj0 = int(std::floor((-hh) / h)) - 1, gj1 = int(std::ceil(hh / h)) + 1;
	const int gCols = gi1 - gi0 + 1, gRows = gj1 - gj0 + 1;
	std::vector<int> gridOf;   // vertex -> grid node (i - gi0) + (j - gj0) * gCols, -1 = not a grid point
	const double keepOff = 0.55 * h;
	struct GPt { P2 q; int i, j; };
	std::vector<GPt> gpts;
	for (int j = gj0; j <= gj1; ++j)
		for (int i = gi0; i <= gi1; ++i)
		{
			P2 q(i * h + ((!prm.quads && (j & 1)) ? 0.5 * h : 0.0), j * h);   // triangles: offset rows (hex-like lattice)
			if (G.Bilinear(q) <= 0.0) continue;
			bool nearB = false;
			bh.Near(q, keepOff, [&](int id) { if (!nearB && Len(bpts[id] - q) < keepOff) nearB = true; });
			if (nearB) continue;
			// a tiny deterministic jitter avoids exactly co-circular grid quadruples
			const double jx = (double(Hash(uint64_t(int64_t(i)) * 7919ULL + uint64_t(int64_t(j)) * 104729ULL) >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 1e-5 * h;
			const double jy = (double(Hash(uint64_t(int64_t(j)) * 6007ULL + uint64_t(int64_t(i)) * 15485863ULL + 3) >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 1e-5 * h;
			gpts.push_back(GPt{ P2(q.x + jx, q.y + jy), i, j });
		}
	HilbertSort(gpts, -hw, -hh, hw, hh);
	dt.Init(-hw - margin, -hh - margin, hw + margin, hh + margin, gpts.size() + bpts.size() * 2);
	for (const GPt& gp : gpts)
	{
		const int v = dt.Insert(gp.q, dup);
		if (v >= 3 && prm.quads)
		{
			if (int(gridOf.size()) <= v) gridOf.resize(v + 1, -1);
			gridOf[v] = (gp.i - gi0) + (gp.j - gj0) * gCols;
		}
	}
	for (size_t li = 0; li < loops.size(); ++li)
		for (const P2& q : loops[li])
		{
			const int v = dt.Insert(q, dup);
			if (v >= 3 && (loopIds[li].empty() || loopIds[li].back() != v)) loopIds[li].push_back(v);
		}
	for (auto& L : loopIds) while (L.size() > 1 && L.front() == L.back()) L.pop_back();
	// conforming: split border edges the Delaunay did not produce
	std::unordered_set<uint64_t> cons;
	std::vector<uint8_t> isLoop;
	auto markLoops = [&]()
	{
		isLoop.assign(dt.p.size(), 0);
		for (auto& L : loopIds) for (int v : L) isLoop[v] = 1;
	};
	bool conforming = false;
	for (int round = 0; round < 24 && !conforming; ++round)
	{
		markLoops();
		std::unordered_set<uint64_t> edges;   // only edges between two border vertices matter
		edges.reserve(bpts.size() * 4);
		for (size_t ti = 0; ti < dt.t.size(); ++ti)
		{
			if (dt.dead[ti]) continue;
			const auto& T = dt.t[ti];
			for (int k = 0; k < 3; ++k)
				if (isLoop[T.v[k]] && isLoop[T.v[(k + 1) % 3]]) edges.insert(EdgeKey(T.v[k], T.v[(k + 1) % 3]));
		}
		bool missing = false;
		for (auto& L : loopIds)
		{
			std::vector<int> nl;
			nl.reserve(L.size() + 8);
			for (size_t i = 0; i < L.size(); ++i)
			{
				const int a = L[i], b = L[(i + 1) % L.size()];
				nl.push_back(a);
				if (edges.count(EdgeKey(a, b))) continue;
				missing = true;
				const P2 m = (dt.p[a] + dt.p[b]) * 0.5;
				const int v = dt.Insert(m, dup * 0.01);
				if (v >= 3 && v != a && v != b) nl.push_back(v);
			}
			L.swap(nl);
		}
		conforming = !missing;
	}
	markLoops();
	for (auto& L : loopIds)
		for (size_t i = 0; i < L.size(); ++i) cons.insert(EdgeKey(L[i], L[(i + 1) % L.size()]));
	// inside / outside by parity across the border edges, flooding from the super triangle
	std::vector<int8_t> inside(dt.t.size(), -1);
	{
		int s0 = -1;
		for (size_t ti = 0; ti < dt.t.size() && s0 < 0; ++ti)
			if (!dt.dead[ti] && (dt.t[ti].v[0] < 3 || dt.t[ti].v[1] < 3 || dt.t[ti].v[2] < 3)) s0 = int(ti);
		if (s0 < 0) return false;
		std::vector<int> st{ s0 };
		inside[s0] = 0;
		while (!st.empty())
		{
			const int ti = st.back(); st.pop_back();
			const auto& T = dt.t[ti];
			for (int k = 0; k < 3; ++k)
			{
				const int nb = T.n[k];
				if (nb < 0 || dt.dead[nb] || inside[nb] >= 0) continue;
				const int ea = T.v[(k + 1) % 3], eb = T.v[(k + 2) % 3];
				const bool c = isLoop[ea] && isLoop[eb] && cons.count(EdgeKey(ea, eb)) > 0;
				inside[nb] = int8_t(inside[ti] ^ (c ? 1 : 0));
				st.push_back(nb);
			}
		}
		if (!conforming)   // never seen in tests; parity is unsafe then, classify by the field instead
			for (size_t ti = 0; ti < dt.t.size(); ++ti)
			{
				if (dt.dead[ti]) continue;
				const auto& T = dt.t[ti];
				inside[ti] = G.Bilinear((dt.p[T.v[0]] + dt.p[T.v[1]] + dt.p[T.v[2]]) * (1.0 / 3.0)) > 0.0 ? 1 : 0;
			}
	}
	// faces (CCW in XY, front +Z)

	{
		std::vector<std::array<int, 3>> cellTris;   // per grid cell: count, tri, tri
		std::vector<int> tris;
		for (size_t ti = 0; ti < dt.t.size(); ++ti)
		{
			if (dt.dead[ti] || inside[ti] != 1) continue;
			const auto& T = dt.t[ti];
			if (T.v[0] < 3 || T.v[1] < 3 || T.v[2] < 3) continue;
			if (Orient(dt.p[T.v[0]], dt.p[T.v[1]], dt.p[T.v[2]]) <= 1e-18 * h * h) continue;   // never expected
			tris.push_back(int(ti));
		}
		std::vector<uint8_t> merged(dt.t.size(), 0);
		if (prm.quads)
		{
			cellTris.assign(size_t(gCols) * gRows, { 0, -1, -1 });
			for (int ti : tris)
			{
				const auto& T = dt.t[ti];
				int mi = INT32_MAX, mj = INT32_MAX, Mi = INT32_MIN, Mj = INT32_MIN;
				bool ok = true;
				for (int k = 0; k < 3 && ok; ++k)
				{
					const int gid = T.v[k] < int(gridOf.size()) ? gridOf[T.v[k]] : -1;
					if (gid < 0) { ok = false; break; }
					const int ci = gid % gCols, cj = gid / gCols;
					mi = std::min(mi, ci); Mi = std::max(Mi, ci);
					mj = std::min(mj, cj); Mj = std::max(Mj, cj);
				}
				if (!ok || Mi - mi != 1 || Mj - mj != 1) continue;
				auto& c = cellTris[size_t(mi) + size_t(mj) * gCols];
				if (c[0] < 2) c[1 + c[0]] = ti;
				++c[0];
			}
			for (const auto& c : cellTris)   // grid order: deterministic
			{
				if (c[0] != 2) continue;
				const auto& A = dt.t[c[1]];
				const auto& B = dt.t[c[2]];
				// shared edge = the diagonal; quad = A's verts with B's opposite vertex inserted
				int shared = 0;
				for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) shared += A.v[a] == B.v[b];
				if (shared != 2) continue;
				int ka = -1;   // vertex of A not in B
				for (int a = 0; a < 3; ++a) { bool inB = false; for (int b = 0; b < 3; ++b) inB |= A.v[a] == B.v[b]; if (!inB) ka = a; }
				int ob = -1;
				for (int b = 0; b < 3; ++b) { bool inA = false; for (int a = 0; a < 3; ++a) inA |= A.v[a] == B.v[b]; if (!inA) ob = B.v[b]; }
				// A = (va, vb, vc) CCW with va = ka; the diagonal is vb-vc; B's free vertex goes between vb and vc
				const int va = A.v[ka], vb = A.v[(ka + 1) % 3], vc = A.v[(ka + 2) % 3];
				faceV.insert(faceV.end(), { va, vb, ob, vc });
				faceN.push_back(4);
				merged[c[1]] = merged[c[2]] = 1;
			}
		}
		for (int ti : tris)
		{
			if (merged[ti]) continue;
			const auto& T = dt.t[ti];
			faceV.insert(faceV.end(), { T.v[0], T.v[1], T.v[2] });
			faceN.push_back(3);
		}
	}
	if (faceN.empty()) return false;
	// compact vertices
	std::vector<int> remap(dt.p.size(), -1);

	for (int& v : faceV)
	{
		if (remap[v] < 0) { remap[v] = int(P.size()); P.push_back(dt.p[v]); }
		v = remap[v];
	}

	}
	const size_t NV = P.size();
	// border edges of the final surface (they can only join two border vertices) and vertex adjacency
	std::vector<std::vector<int>> adj(NV);
	std::vector<std::pair<int, int>> borderE;   // directed, paper on the left
	{

		std::unordered_map<uint64_t, int> edgeUse;
		size_t o = 0;
		for (int n : faceN)
		{
			for (int k = 0; k < n; ++k)
			{
				const int a = faceV[o + k], b = faceV[o + (k + 1) % n];
				adj[a].push_back(b); adj[b].push_back(a);
				++edgeUse[EdgeKey(a, b)];
			}
			o += n;
		}
		o = 0;
		for (int n : faceN)
		{
			for (int k = 0; k < n; ++k)
			{
				const int a = faceV[o + k], b = faceV[o + (k + 1) % n];
				if (edgeUse[EdgeKey(a, b)] == 1) borderE.push_back({ a, b });
			}
			o += n;
		}
		for (auto& A : adj) { std::sort(A.begin(), A.end()); A.erase(std::unique(A.begin(), A.end()), A.end()); }
	}

	// ---- distances to borders ----------------------------------------------------
	const double dCap = std::max({ prm.edgeDistMax, prm.curlWidth, 2.3 * prm.peelWidth, h });
	std::vector<double> eDist(NV, dCap), tDist(NV, dCap);
	{
		PointHash ph; ph.cell = std::max(dCap, h);
		std::vector<P2> bp; std::vector<uint8_t> btorn;
		for (auto& e : borderE)
		{
			const P2 a = P[e.first], b = P[e.second];
			const int n = std::max(1, int(std::ceil(Len(b - a) / (0.5 * h))));
			for (int k = 0; k < n; ++k)
			{
				const P2 q = a + (b - a) * (double(k) / n);
				ph.Add(q, int(bp.size())); bp.push_back(q); btorn.push_back(G.TornAt(q));
			}
		}
		for (size_t v = 0; v < NV; ++v)
			ph.Near(P[v], dCap, [&](int id)
			{
				const double d = Len(bp[id] - P[v]);
				if (d < eDist[v]) eDist[v] = d;
				if (btorn[id] && d < tDist[v]) tDist[v] = d;
			});
	}

	// ---- crumple height ------------------------------------------------------------
	const double strength = Clamp(prm.crumpleStrength, 0.0, 3.0);
	const double cs = std::max(prm.crumpleSize, 2.0 * h);
	const double sharp = Clamp(prm.crumpleSharpness, 0.0, 1.0);
	const double detail = Clamp(prm.crumpleDetail, 0.0, 1.0);
	std::vector<CrumpleOctave> oct;
	if (strength > 0.0)
	{
		const double sz[3] = { 1.0, 0.46, 0.21 };
		const double wt[3] = { 1.0, detail * 0.8, detail * detail * 0.6 };
		for (int o = 0; o < 3; ++o)
		{
			const double s = cs * sz[o];
			if (o > 0 && (wt[o] <= 1e-4 || s < 2.0 * h)) continue;
			CrumpleOctave co;
			co.size = s; co.amp = strength * s * 0.5 * wt[o]; co.key = Mix(ch(kChCrumple), uint64_t(o));
			if (sharp > 0.0)
			{
				const double ext = s * 2.0;
				co.dt.Init(-hw - ext, -hh - ext, hw + ext, hh + ext);
				co.h.assign(3, 0.0);
				const int ni = int(std::ceil((W + 2 * ext) / s)), nj = int(std::ceil((H + 2 * ext) / s));
				struct CPt { P2 q; double hv; };
				std::vector<CPt> cp;
				for (int j = 0; j <= nj; ++j)
					for (int i = 0; i <= ni; ++i)
					{
						Rng rr(Mix(co.key, uint64_t(j) * 100003ULL + uint64_t(i)));
						const P2 q(-hw - ext + (i + rr.R(0.1, 0.9)) * s, -hh - ext + (j + rr.R(0.1, 0.9)) * s);
						cp.push_back(CPt{ q, rr.R(-1.0, 1.0) });
					}
				HilbertSort(cp, -hw - ext, -hh - ext, hw + ext + s, hh + ext + s);
				for (const CPt& c : cp)
				{
					const int v = co.dt.Insert(c.q, 1e-9);
					if (v >= int(co.h.size())) { co.h.resize(v + 1, 0.0); co.h[v] = c.hv; }
				}
			}
			oct.push_back(std::move(co));
		}
	}
	const uint64_t kSoft = ch(kChSoft), kWave = ch(kChWave), kCurl = ch(kChCurl);
	const double waveA = Clamp(prm.waviness, 0.0, 1.0) * std::max(prm.waveSize, h) * 0.12;
	const double waveS = std::max(prm.waveSize, 2.0 * h);
	const double curlW = std::max(prm.curlWidth, h);
	const double curlA = Clamp(prm.edgeCurl, 0.0, 1.0) * curlW * 0.6;
	auto foldH = [&](double x, double lo, double hi, int folds) -> double
	{
		if (folds <= 0) return 0.0;
		// zig-zag: 0 at both borders, +-1 alternating at the fold lines
		const double t = (x - lo) / (hi - lo) * (folds + 1);
		const int k = std::clamp(int(std::floor(t)), 0, folds);
		auto hv = [&](int idx) -> double { return (idx <= 0 || idx >= folds + 1) ? 0.0 : ((idx % 2) ? 1.0 : -1.0); };
		const double u = t - k;
		return hv(k) * (1 - u) + hv(k + 1) * u;
	};
	std::vector<double> Z(NV, 0.0);
	for (size_t v = 0; v < NV; ++v)
	{
		const P2 q = P[v];
		double z = 0.0;
		for (CrumpleOctave& co : oct)
		{
			double pl = 0.0, sm = 0.0;
			if (sharp > 0.0) pl = co.dt.Interp(q, co.h, co.hint);
			if (sharp < 1.0) sm = VNoise(Mix(kSoft, co.key), q.x / co.size, q.y / co.size) * 1.1;
			z += co.amp * (sharp * pl + (1.0 - sharp) * sm);
		}
		if (waveA > 0.0) z += waveA * Fbm(kWave, q.x / waveS, q.y / waveS, 2);
		z += prm.foldDepth * (foldH(q.x, -hw, hw, prm.foldsX) + foldH(q.y, -hh, hh, prm.foldsY));
		if (curlA > 0.0 && tDist[v] < curlW)
		{
			const double s = 1.0 - tDist[v] / curlW;
			z += curlA * s * s * (0.55 + 0.45 * VNoise(kCurl, q.x / (curlW * 3.0), q.y / (curlW * 3.0)));
		}
		Z[v] = z;
	}
	// crease attribute: signed umbrella Laplacian of the height, ridge > 0
	std::vector<double> crease(NV, 0.0);
	for (size_t v = 0; v < NV; ++v)
	{
		if (adj[v].empty()) continue;
		double s = 0.0, wsum = 0.0;
		for (int u : adj[v]) { const double w = 1.0 / std::max(Len(P[u] - P[v]), 1e-9); s += w * Z[u]; wsum += w; }
		const double lap = Z[v] - s / wsum;   // > 0 on ridges
		crease[v] = Clamp(lap / (0.12 * h + 1e-12), -1.0, 1.0);
	}

	// peel: band along the torn borders where the printed top layer tore further
	// back than the white base (posters, newspapers), with its own ragged edge
	std::vector<double> peel(NV, 0.0);
	if (prm.peelWidth > 0.0)
	{
		const double pw = prm.peelWidth;
		const uint64_t kp = ch(kChPeel);
		for (size_t v = 0; v < NV; ++v)
		{
			if (tDist[v] >= 2.2 * pw) continue;
			const P2 q = P[v];
			const double n = Clamp(0.5 + 0.7 * Fbm(kp, q.x / (pw * 3.0), q.y / (pw * 3.0), 5), 0.0, 1.0);
			const double edge = pw * (0.15 + 2.0 * n * std::sqrt(n));
			peel[v] = Clamp((edge - tDist[v]) / (0.15 * pw) + 0.5, 0.0, 1.0);
		}
	}

	// ---- output ----------------------------------------------------------------
	auto uvOf = [&](const P2& q) { return std::array<double, 2>{ (q.x + hw) / W, (q.y + hh) / H }; };
	const bool shell = prm.thickness > 0.0;
	{
		const size_t nv = shell ? 2 * NV : NV, nl = faceV.size() * (shell ? 2 : 1) + (shell ? 4 * borderE.size() : 0);
		out.points.reserve(nv); out.edgeDist.reserve(nv); out.tearDist.reserve(nv); out.crease.reserve(nv); out.peel.reserve(nv);
		out.faceVerts.reserve(nl); out.loopUv.reserve(nl);
	}
	std::vector<EVec> pts3(NV);
	for (size_t v = 0; v < NV; ++v)
		pts3[v] = surface != nullptr ? basePositions[v] + baseNormals[v] * Z[v] : EVec(P[v].x, P[v].y, Z[v]);
	auto provenance = [&]()
	{
		if (surface == nullptr) return;
		std::vector<int> firstSource(NV, -1);
		size_t corner = 0;
		for (size_t face = 0; face < faceN.size(); ++face)
		{
			for (int k = 0; k < faceN[face]; ++k) firstSource[faceV[corner++]] = sourceFaces[face];
			out.sourceFace.push_back(sourceFaces[face]);
			if (shell) out.sourceFace.push_back(sourceFaces[face]);
		}
		if (shell) for (auto edge : borderE) out.sourceFace.push_back(firstSource[edge.first]);
		corner = 0;
		for (size_t face = 0; face < out.faceSizes.size(); ++face)
		{
			const auto& triangle = surface->triangles[out.sourceFace[face]];
			const EVec a = surface->points[triangle[0]], u = surface->points[triangle[1]] - a, v = surface->points[triangle[2]] - a;
			const double uu = manifold::la::dot(u, u), uv = manifold::la::dot(u, v), vv = manifold::la::dot(v, v);
			const double denominator = std::max(uu * vv - uv * uv, 1e-30);
			for (int k = 0; k < out.faceSizes[face]; ++k)
			{
				const EVec offset = basePositions[out.faceVerts[corner++] % int(NV)] - a;
				const double wu = manifold::la::dot(offset, u), wv = manifold::la::dot(offset, v);
				const double b = (wu * vv - wv * uv) / denominator, c = (wv * uu - wu * uv) / denominator;
				out.sourceBary.push_back({1.0 - b - c, b, c});
			}
		}
	};
	auto pushAttr = [&](size_t v)
	{
		out.edgeDist.push_back(float(eDist[v]));
		out.tearDist.push_back(float(tDist[v]));
		out.crease.push_back(float(crease[v]));
		out.peel.push_back(float(peel[v]));
	};
	if (!shell)
	{
		out.points = pts3;
		for (size_t v = 0; v < NV; ++v) pushAttr(v);
		size_t o = 0;
		for (int n : faceN)
		{
			for (int k = 0; k < n; ++k) { out.faceVerts.push_back(faceV[o + k]); out.loopUv.push_back(uvOf(P[faceV[o + k]])); }
			out.faceSizes.push_back(n); out.faceMat.push_back(0);
			o += n;
		}
		provenance();
		return true;
	}
	// vertex normals of the crumpled surface (area weighted, quads as two triangles)
	std::vector<std::array<double, 3>> nrm(NV, { 0.0, 0.0, 0.0 });
	{
		size_t o = 0;
		for (int n : faceN)
		{
			for (int k = 1; k + 1 < n; ++k)
			{
				const EVec &a = pts3[faceV[o]], &b = pts3[faceV[o + k]], &c = pts3[faceV[o + k + 1]];
				const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
				const double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
				for (int idx : { faceV[o], faceV[o + k], faceV[o + k + 1] }) { nrm[idx][0] += nx; nrm[idx][1] += ny; nrm[idx][2] += nz; }
			}
			o += n;
		}
		for (auto& nn : nrm)
		{
			const double l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
			if (l > 1e-20) { nn[0] /= l; nn[1] /= l; nn[2] /= l; } else nn = { 0.0, 0.0, 1.0 };
		}
	}
	const double ht = 0.5 * prm.thickness;
	for (size_t v = 0; v < NV; ++v) { out.points.push_back(EVec(pts3[v].x + nrm[v][0] * ht, pts3[v].y + nrm[v][1] * ht, pts3[v].z + nrm[v][2] * ht)); pushAttr(v); }
	for (size_t v = 0; v < NV; ++v) { out.points.push_back(EVec(pts3[v].x - nrm[v][0] * ht, pts3[v].y - nrm[v][1] * ht, pts3[v].z - nrm[v][2] * ht)); pushAttr(v); }
	const int off = int(NV);
	size_t o = 0;
	for (int n : faceN)
	{
		for (int k = 0; k < n; ++k) { out.faceVerts.push_back(faceV[o + k]); out.loopUv.push_back(uvOf(P[faceV[o + k]])); }
		out.faceSizes.push_back(n); out.faceMat.push_back(0);
		for (int k = n - 1; k >= 0; --k) { out.faceVerts.push_back(faceV[o + k] + off); out.loopUv.push_back(uvOf(P[faceV[o + k]])); }
		out.faceSizes.push_back(n); out.faceMat.push_back(1);
		o += n;
	}
	for (auto& e : borderE)
	{
		const int a = e.first, b = e.second;
		const int q[4] = { a + off, b + off, b, a };
		for (int k = 0; k < 4; ++k) { out.faceVerts.push_back(q[k]); out.loopUv.push_back(uvOf(P[q[k] % off])); }
		out.faceSizes.push_back(4); out.faceMat.push_back(2);
	}
	provenance();
	return true;
}
bool BuildPaper(const PaperGenParams& p, const PaperGenPaint& paint, PaperGenResult& out)
{
	return BuildPaperImpl(p, paint, out, nullptr);
}

// основной вход: собираем геометрию по переданным настройкам
bool ApplyPaper(const PaperGenSurface& surface, const PaperGenParams& p, const PaperGenPaint& paint, PaperGenResult& out)
{
	return BuildPaperImpl(p, paint, out, &surface);
}
PROJECT_DAMAGE_NS_MACRO_END
