// разрушаем поверхность камня по выбранному рисунку
// ProjectDamage erosion / weathering generator - GPL-3.0-or-later; see LICENSE.
//
// 1. the shape as a signed distance volume d0 (< 0 inside), exact in a band
//    around the surface and clamped to +-B outside it: a generated shape is
//    evaluated directly; a source mesh gets closest-triangle distances in the
//    band and inside/outside from ray winding numbers along X, Y and Z (majority vote)
// 2. rounded edges: d0 blurred (3 box passes ~ Gaussian), dR = max(d0, mix(d0,
//    blur, wear)) - convex edges round, concave corners stay
// 3. wear field: amount x variation noise x exposure x per-stone factor +
//    marker zones
// 4. features near the surface, each from a hashed cell grid in object space
//    (stable when the resolution changes): edge chunks (Voronoi cells that
//    contain an edge are cut away, fresh faces), strata grooves, granular
//    roughness, hollows and pits (ellipsoids sunk into the surface), exposed
//    aggregate (stones kept, cement worn back)
// 5. manifold surface nets: one vertex per marching-cubes component of a
//    cell (face ambiguities resolved by the face centre), one quad per sign
//    change of a grid edge; vertices projected onto the zero level
// 6. small loose bits dropped, box UVs, attributes, materials
#include "erosion.h"

#include <cstring>
#include <thread>
#ifdef PDGEN_TRACE
#include <chrono>
#include <cstdio>
#define TRACE(name) do { static auto t_ = std::chrono::steady_clock::now(); (void)t_; std::fprintf(stderr, "  [%s] %.0f ms\n", name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceT0).count()); } while (0)
#else
#define TRACE(name) do {} while (0)
#endif

namespace pdgen {
namespace {

enum Label : uint8_t { kWeathered = 0, kFresh = 1, kStone = 2, kMortar = 3 };

// f(k) for k in [0, n) on all cores (each k is a slab of independent voxels)
template <class F> void ParallelFor(int n, F&& f)
{
	const int nt = std::max(1, std::min(n, int(std::thread::hardware_concurrency())));
	if (nt <= 1 || n < 8)
	{
		for (int k = 0; k < n; ++k) f(k);
		return;
	}
	std::vector<std::thread> th;
	th.reserve(nt);
	for (int t = 0; t < nt; ++t)
		th.emplace_back([&, t] { for (int k = t; k < n; k += nt) f(k); });
	for (auto& x : th) x.join();
}

struct Grid
{
	int nx = 0, ny = 0, nz = 0;
	double h = 1.0;
	V3 o;
	size_t N() const { return size_t(nx) * ny * nz; }
	size_t I(int i, int j, int k) const { return (size_t(k) * ny + j) * nx + i; }
	V3 P(int i, int j, int k) const { return V3(o.x + i * h, o.y + j * h, o.z + k * h); }
};

double Sample(const Grid& g, const std::vector<float>& v, const V3& p)
{
	double fx = (p.x - g.o.x) / g.h, fy = (p.y - g.o.y) / g.h, fz = (p.z - g.o.z) / g.h;
	fx = Clamp(fx, 0.0, g.nx - 1.000001);
	fy = Clamp(fy, 0.0, g.ny - 1.000001);
	fz = Clamp(fz, 0.0, g.nz - 1.000001);
	const int i = int(fx), j = int(fy), k = int(fz);
	const double u = fx - i, w = fy - j, t = fz - k;
	auto at = [&](int a, int b, int c) { return double(v[g.I(i + a, j + b, k + c)]); };
	const double x00 = Lerp(at(0, 0, 0), at(1, 0, 0), u), x10 = Lerp(at(0, 1, 0), at(1, 1, 0), u);
	const double x01 = Lerp(at(0, 0, 1), at(1, 0, 1), u), x11 = Lerp(at(0, 1, 1), at(1, 1, 1), u);
	return Lerp(Lerp(x00, x10, w), Lerp(x01, x11, w), t);
}

V3 Gradient(const Grid& g, const std::vector<float>& v, const V3& p)
{
	const double e = 0.5 * g.h;
	return V3(Sample(g, v, p + V3(e, 0, 0)) - Sample(g, v, p - V3(e, 0, 0)), Sample(g, v, p + V3(0, e, 0)) - Sample(g, v, p - V3(0, e, 0)),
			   Sample(g, v, p + V3(0, 0, e)) - Sample(g, v, p - V3(0, 0, e))) *
		(1.0 / (2.0 * e));
}

// three box-blur passes per axis (radius r voxels, clamp at the borders) ~ a Gaussian of sigma sqrt(r (r+1))
void Blur(const Grid& g, std::vector<float>& v, int r)
{
	if (r < 1) return;
	const int dims[3] = { g.nx, g.ny, g.nz };
	const size_t stride[3] = { 1, size_t(g.nx), size_t(g.nx) * g.ny };
	std::vector<double> line, sum;
	for (int axis = 0; axis < 3; ++axis)
	{
		const int n = dims[axis];
		line.resize(n);
		sum.resize(n + 1);
		const int a1 = axis == 0 ? 1 : 0, a2 = axis == 2 ? 1 : 2;
		for (int q = 0; q < dims[a2]; ++q)
			for (int s = 0; s < dims[a1]; ++s)
			{
				const size_t base = size_t(s) * stride[a1] + size_t(q) * stride[a2];
				for (int x = 0; x < n; ++x) line[x] = v[base + size_t(x) * stride[axis]];
				for (int pass = 0; pass < 3; ++pass)
				{
					sum[0] = 0.0;
					for (int x = 0; x < n; ++x) sum[x + 1] = sum[x] + line[x];
					for (int x = 0; x < n; ++x)
					{
						// clamp-to-edge: values beyond the border repeat the border value
						const int lo = x - r, hi = x + r;
						double s2 = sum[std::min(hi, n - 1) + 1] - sum[std::max(lo, 0)];
						if (lo < 0) s2 += double(-lo) * line[0];
						if (hi > n - 1) s2 += double(hi - (n - 1)) * line[n - 1];
						line[x] = s2 / double(2 * r + 1);
					}
				}
				for (int x = 0; x < n; ++x) v[base + size_t(x) * stride[axis]] = float(line[x]);
			}
	}
}

// ---------------------------------------------------------------------------
// shapes

double SdRoundBox(const V3& p, const V3& half, double r)
{
	r = std::min({ r, half.x, half.y, half.z });
	const V3 q(std::abs(p.x) - half.x + r, std::abs(p.y) - half.y + r, std::abs(p.z) - half.z + r);
	const V3 m(std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0));
	return Len(m) + std::min(std::max({ q.x, q.y, q.z }), 0.0) - r;
}

struct Shape
{
	const ErosionParams& p;
	uint64_t seed;
	explicit Shape(const ErosionParams& pp) : p(pp), seed(uint64_t(uint32_t(pp.seed)) * 0x9E37ull + 0x51A7ull) {}

	// ashlar wall: the nearest stone; id = stone hash
	double Wall(const V3& q, uint64_t* id) const
	{
		const double bw = std::max(p.blockWidth, 1e-4), bh = std::max(p.blockHeight, 1e-4), jw = Clamp(p.jointWidth, 0.0, 0.45 * std::min(bw, bh));
		const double hx = 0.5 * p.sizeX, hy = 0.5 * p.sizeY, hz = 0.5 * p.sizeZ;
		const double wallBox = SdRoundBox(q - V3(0, 0, hz), V3(hx, hy + 1.0, hz), 0.0);
		double best = 1e30;
		const int row = int(std::floor(q.z / bh));
		for (int r = row - 1; r <= row + 1; ++r)
		{
			if (r < 0 || r * bh > p.sizeZ) continue;
			const double off = (r & 1) ? 0.5 * bw : 0.0;
			const int col = int(std::floor((q.x + hx - off) / bw));
			for (int c = col - 1; c <= col + 1; ++c)
			{
				const uint64_t hsh = Hash(seed, uint64_t(int64_t(r)), uint64_t(int64_t(c)), 0x57A1);
				const double jy = (Hash01(hsh, 1) - 0.5) * jw * 0.6;
				const V3 cen(-hx + off + (c + 0.5) * bw, jy, (r + 0.5) * bh);
				const V3 half(0.5 * (bw - jw), hy + 0.25 * jw * (Hash01(hsh, 2) - 0.5), 0.5 * (bh - jw));
				const double d = std::max(SdRoundBox(q - cen, half, p.shapeBevel), wallBox);
				if (d < best) { best = d; if (id) *id = hsh; }
			}
		}
		return best;
	}

	// signed distance of the generated shape; label: stone or mortar
	double Sdf(const V3& q, uint8_t* label, uint64_t* id) const
	{
		if (label) *label = kWeathered;
		if (id) *id = 0;
		switch (p.shape)
		{
		case 2: { // column
			const double R = 0.5 * std::min(p.sizeX, p.sizeY), hz = 0.5 * p.sizeZ, r = std::min(p.shapeBevel, 0.5 * std::min(R, hz));
			const double dx = std::hypot(q.x, q.y) - R + r, dz = std::abs(q.z - hz) - hz + r;
			return std::hypot(std::max(dx, 0.0), std::max(dz, 0.0)) + std::min(std::max(dx, dz), 0.0) - r;
		}
		case 3: { // boulder: a lumpy ellipsoid on a flat bed
			const V3 a(0.5 * p.sizeX, 0.5 * p.sizeY, 0.5 * p.sizeZ);
			const double am = std::min({ a.x, a.y, a.z });
			const V3 c = q - V3(0, 0, a.z * 0.92);
			const V3 s(c.x / a.x, c.y / a.y, c.z / a.z);
			const V3 s2(c.x / (a.x * a.x), c.y / (a.y * a.y), c.z / (a.z * a.z));
			const double k0 = Len(s), k1 = std::max(Len(s2), 1e-12);
			double d = k0 * (k0 - 1.0) / k1;
			d += 0.11 * am * Fbm3(q.x / (0.9 * am), q.y / (0.9 * am), q.z / (0.9 * am), seed + 3, 4);
			d += 0.03 * am * Fbm3(q.x / (0.25 * am), q.y / (0.25 * am), q.z / (0.25 * am), seed + 4, 3);
			return std::max(d, -q.z);
		}
		case 4: { // ashlar wall
			const double stone = Wall(q, id);
			const double mortar = SdRoundBox(q - V3(0, 0, 0.5 * p.sizeZ),
				V3(0.5 * p.sizeX, std::max(0.5 * p.sizeY - std::max(p.mortarRecess, 0.0), 0.25 * p.sizeY), 0.5 * p.sizeZ), 0.0);
			if (mortar < stone)
			{
				if (label) *label = kMortar;
				return mortar;
			}
			return stone;
		}
		default: // block
			return SdRoundBox(q - V3(0, 0, 0.5 * p.sizeZ), V3(0.5 * p.sizeX, 0.5 * p.sizeY, 0.5 * p.sizeZ), p.shapeBevel);
		}
	}
	V3 BoxLo() const { return p.shape == 4 ? V3(-0.5 * p.sizeX, -0.5 * p.sizeY - p.jointWidth, 0) : V3(-0.5 * p.sizeX, -0.5 * p.sizeY, 0); }
	V3 BoxHi() const { return p.shape == 4 ? V3(0.5 * p.sizeX, 0.5 * p.sizeY + p.jointWidth, p.sizeZ) : V3(0.5 * p.sizeX, 0.5 * p.sizeY, p.sizeZ); }
};

// ---------------------------------------------------------------------------
// source mesh -> signed distance

double PointTriDist2(const V3& p, const V3& a, const V3& b, const V3& c)
{
	// Ericson, closest point on triangle
	const V3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return Dot(ap, ap);
	const V3 bp = p - b;
	const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return Dot(bp, bp);
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0)
	{
		const V3 q = a + ab * (d1 / (d1 - d3)) - p;
		return Dot(q, q);
	}
	const V3 cp = p - c;
	const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return Dot(cp, cp);
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0)
	{
		const V3 q = a + ac * (d2 / (d2 - d6)) - p;
		return Dot(q, q);
	}
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
	{
		const V3 q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))) - p;
		return Dot(q, q);
	}
	const double den = 1.0 / (va + vb + vc);
	const V3 q = a + ab * (vb * den) + ac * (vc * den) - p;
	return Dot(q, q);
}

void MeshSdf(const Grid& g, const std::vector<V3>& P, const std::vector<std::array<int32_t, 3>>& T, double B, std::vector<float>& d)
{
	const size_t N = g.N();
	std::vector<float> dist(N, float(B));
	std::vector<int32_t> tri(N, -1);
	std::vector<size_t> front, next;
	std::vector<uint8_t> mark(N, 0);
	const double h = g.h;
	auto idx = [&](double x, double o) { return (x - o) / h; };
	for (size_t t = 0; t < T.size(); ++t)
	{
		const V3 &a = P[T[t][0]], &b = P[T[t][1]], &c = P[T[t][2]];
		const V3 nrm = Cross(b - a, c - a);
		const double nl = Len(nrm);
		const V3 n = nl > 1e-20 ? nrm * (1.0 / nl) : V3(0, 0, 0);
		const double m = 1.5 * h;
		const int i0 = std::max(0, int(std::floor(idx(std::min({ a.x, b.x, c.x }) - m, g.o.x)))), i1 = std::min(g.nx - 1, int(std::ceil(idx(std::max({ a.x, b.x, c.x }) + m, g.o.x))));
		const int j0 = std::max(0, int(std::floor(idx(std::min({ a.y, b.y, c.y }) - m, g.o.y)))), j1 = std::min(g.ny - 1, int(std::ceil(idx(std::max({ a.y, b.y, c.y }) + m, g.o.y))));
		const int k0 = std::max(0, int(std::floor(idx(std::min({ a.z, b.z, c.z }) - m, g.o.z)))), k1 = std::min(g.nz - 1, int(std::ceil(idx(std::max({ a.z, b.z, c.z }) + m, g.o.z))));
		for (int k = k0; k <= k1; ++k)
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					const V3 q = g.P(i, j, k);
					if (nl > 1e-20 && std::abs(Dot(q - a, n)) > m) continue;
					const double dd = std::sqrt(PointTriDist2(q, a, b, c));
					const size_t v = g.I(i, j, k);
					if (dd < dist[v])
					{
						dist[v] = float(dd);
						tri[v] = int32_t(t);
						if (!mark[v]) { mark[v] = 1; front.push_back(v); }
					}
				}
	}
	// grow the band: neighbours take the closest triangle of the voxel that reached them
	const int64_t st[3] = { 1, g.nx, int64_t(g.nx) * g.ny };
	while (!front.empty())
	{
		next.clear();
		for (size_t v : front) mark[v] = 0;
		for (size_t v : front)
		{
			const int i = int(v % g.nx), j = int((v / g.nx) % g.ny), k = int(v / (size_t(g.nx) * g.ny));
			const int t = tri[v];
			const V3 &a = P[T[t][0]], &b = P[T[t][1]], &c = P[T[t][2]];
			const int co[3] = { i, j, k }, lim[3] = { g.nx, g.ny, g.nz };
			for (int ax = 0; ax < 3; ++ax)
				for (int s = -1; s <= 1; s += 2)
				{
					const int cc = co[ax] + s;
					if (cc < 0 || cc >= lim[ax]) continue;
					const size_t u = size_t(int64_t(v) + s * st[ax]);
					if (tri[u] == t) continue;
					const V3 q = g.P(ax == 0 ? cc : i, ax == 1 ? cc : j, ax == 2 ? cc : k);
					const double dd = std::sqrt(PointTriDist2(q, a, b, c));
					const float fd = float(dd);   // compare in float: the stored value is rounded
					if (fd < dist[u] && dd < B)
					{
						dist[u] = fd;
						tri[u] = t;
						if (!mark[u]) { mark[u] = 1; next.push_back(u); }
					}
				}
		}
		front.swap(next);
	}
	// inside / outside: winding number along rays on each axis through the grid points, majority vote
	std::vector<uint8_t> votes(N, 0);
	const double jit[3] = { 0.1234567 * h * 1e-3, 0.2718281 * h * 1e-3, 0.1414213 * h * 1e-3 };
	struct Hit { size_t col; double z; int dir; bool operator<(const Hit& o) const { return col != o.col ? col < o.col : z < o.z; } };
	std::vector<Hit> hits;
	for (int ax = 0; ax < 3; ++ax)
	{
		const int b1 = (ax + 1) % 3, b2 = (ax + 2) % 3;
		const int dims[3] = { g.nx, g.ny, g.nz };
		const double org[3] = { g.o.x, g.o.y, g.o.z };
		const int n1 = dims[b1], n2 = dims[b2], na = dims[ax];
		hits.clear();
		for (const auto& t : T)
		{
			const double* A = &P[t[0]].x;
			const double* Bp = &P[t[1]].x;
			const double* C = &P[t[2]].x;
			const double area = (Bp[b1] - A[b1]) * (C[b2] - A[b2]) - (C[b1] - A[b1]) * (Bp[b2] - A[b2]);
			if (std::abs(area) < 1e-30) continue;
			const int u0 = std::max(0, int(std::floor((std::min({ A[b1], Bp[b1], C[b1] }) - org[b1]) / h)));
			const int u1 = std::min(n1 - 1, int(std::ceil((std::max({ A[b1], Bp[b1], C[b1] }) - org[b1]) / h)));
			const int w0 = std::max(0, int(std::floor((std::min({ A[b2], Bp[b2], C[b2] }) - org[b2]) / h)));
			const int w1 = std::min(n2 - 1, int(std::ceil((std::max({ A[b2], Bp[b2], C[b2] }) - org[b2]) / h)));
			for (int w = w0; w <= w1; ++w)
				for (int u = u0; u <= u1; ++u)
				{
					const double x = org[b1] + u * h + jit[b1], y = org[b2] + w * h + jit[b2];
					const double e0 = (Bp[b1] - A[b1]) * (y - A[b2]) - (Bp[b2] - A[b2]) * (x - A[b1]);
					const double e1 = (C[b1] - Bp[b1]) * (y - Bp[b2]) - (C[b2] - Bp[b2]) * (x - Bp[b1]);
					const double e2 = (A[b1] - C[b1]) * (y - C[b2]) - (A[b2] - C[b2]) * (x - C[b1]);
					if (!((e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0))) continue;
					const double l0 = e1 / area, l1 = e2 / area, l2 = e0 / area;   // barycentric of A, B, C
					const double z = l0 * A[ax] + l1 * Bp[ax] + l2 * C[ax];
					hits.push_back({ size_t(w) * n1 + u, z, area > 0 ? 1 : -1 });
				}
		}
		std::sort(hits.begin(), hits.end());
		size_t h0 = 0;
		while (h0 < hits.size())
		{
			size_t h1 = h0;
			while (h1 < hits.size() && hits[h1].col == hits[h0].col) ++h1;
			const int u = int(hits[h0].col % n1), w = int(hits[h0].col / n1);
			size_t q = h0;
			int wind = 0;   // winding number: overlapping closed parts stay inside
			for (int a = 0; a < na; ++a)
			{
				const double x = org[ax] + a * h;
				while (q < h1 && hits[q].z < x) { wind += hits[q].dir; ++q; }
				if (wind != 0)
				{
					int c3[3];
					c3[ax] = a; c3[b1] = u; c3[b2] = w;
					votes[g.I(c3[0], c3[1], c3[2])]++;
				}
			}
			h0 = h1;
		}
	}
	d.resize(N);
	for (size_t v = 0; v < N; ++v) d[v] = votes[v] >= 2 ? -dist[v] : dist[v];
}

// ---------------------------------------------------------------------------
// features

struct Hollow { V3 c, n, t1, t2; double rt, rn; };

double SdEllipsoid(const V3& q, const V3& r)
{
	const V3 s(q.x / r.x, q.y / r.y, q.z / r.z), s2(q.x / (r.x * r.x), q.y / (r.y * r.y), q.z / (r.z * r.z));
	const double k0 = Len(s), k1 = Len(s2);
	if (k1 < 1e-20) return -std::min({ r.x, r.y, r.z });
	return k0 * (k0 - 1.0) / k1;
}

void Frame(const V3& n, V3& t1, V3& t2)
{
	const V3 a = std::abs(n.z) < 0.9 ? V3(0, 0, 1) : V3(1, 0, 0);
	t1 = Norm(Cross(a, n));
	t2 = Cross(n, t1);
}

// a cell grid over the volume: cell (i, j, k) covers [i, i+1) * L in object space (hashes stay put when the volume moves)
struct Cells
{
	double L = 1.0;
	int64_t i0 = 0, j0 = 0, k0 = 0;
	int nx = 0, ny = 0, nz = 0;
	std::vector<int32_t> slot;        // index into the item list, -1: none
	void Init(const Grid& g, double l)
	{
		L = l;
		i0 = int64_t(std::floor(g.o.x / L)) - 1; j0 = int64_t(std::floor(g.o.y / L)) - 1; k0 = int64_t(std::floor(g.o.z / L)) - 1;
		nx = int(int64_t(std::floor((g.o.x + (g.nx - 1) * g.h) / L)) + 2 - i0);
		ny = int(int64_t(std::floor((g.o.y + (g.ny - 1) * g.h) / L)) + 2 - j0);
		nz = int(int64_t(std::floor((g.o.z + (g.nz - 1) * g.h) / L)) + 2 - k0);
		slot.assign(size_t(nx) * ny * nz, -1);
	}
	int32_t At(int64_t i, int64_t j, int64_t k) const
	{
		i -= i0; j -= j0; k -= k0;
		if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return -1;
		return slot[(size_t(k) * ny + j) * nx + i];
	}
	int32_t& Ref(int64_t i, int64_t j, int64_t k) { return slot[(size_t(k - k0) * ny + (j - j0)) * nx + (i - i0)]; }
	template <class F> void ForEach(F&& f) const
	{
		for (int k = 0; k < nz; ++k)
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i) f(i0 + i, j0 + j, k0 + k);
	}
};

V3 CellPoint(uint64_t seed, int64_t i, int64_t j, int64_t k, double L, double jitter = 1.0)
{
	return V3((double(i) + 0.5 + jitter * (Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 1) - 0.5)) * L,
		(double(j) + 0.5 + jitter * (Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 2) - 0.5)) * L,
		(double(k) + 0.5 + jitter * (Hash01(seed, uint64_t(i), uint64_t(j), uint64_t(k) * 4 + 3) - 0.5)) * L);
}

// ---------------------------------------------------------------------------
// manifold surface nets

struct CellVerts { int32_t v[12]; };

const int kEdgeCorner[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };

int EdgeOf(int a, int b)
{
	for (int e = 0; e < 12; ++e)
		if ((kEdgeCorner[e][0] == a && kEdgeCorner[e][1] == b) || (kEdgeCorner[e][0] == b && kEdgeCorner[e][1] == a)) return e;
	return -1;
}

struct FaceDef { int c[4]; int e[4]; };

std::array<FaceDef, 6> MakeFaces()
{
	std::array<FaceDef, 6> F{};
	int n = 0;
	for (int ax = 0; ax < 3; ++ax)
		for (int v = 0; v < 2; ++v)
		{
			const int u = (ax + 1) % 3, w = (ax + 2) % 3;
			const int uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
			FaceDef fd{};
			for (int q = 0; q < 4; ++q) fd.c[q] = (v << ax) | (uv[q][0] << u) | (uv[q][1] << w);
			for (int q = 0; q < 4; ++q) fd.e[q] = EdgeOf(fd.c[q], fd.c[(q + 1) % 4]);
			F[n++] = fd;
		}
	return F;
}

// Every cell with a sign change holds the marching-cubes polygons of its
// crossings, grouped by face arcs (an ambiguous face is resolved by its centre
// value, the same in both cells that share it). Each polygon gets one vertex
// (projected onto the zero level), each crossing one vertex on its grid edge,
// and each face arc one quad: (polygon vertex of the cell below, crossing,
// polygon vertex of the cell above, crossing). Every quad edge joins a polygon
// vertex and a crossing and borders exactly two quads, so the mesh is closed
// and manifold in every case.
void SurfaceNets(const Grid& g, const std::vector<float>& f, GenMesh& m, std::vector<std::array<size_t, 2>>& faceVoxels)
{
	static const std::array<FaceDef, 6> faces = MakeFaces();
	const int cx = g.nx - 1, cy = g.ny - 1, cz = g.nz - 1;
	std::vector<int32_t> cellSlot(size_t(cx) * cy * cz, -1);
	std::vector<CellVerts> cells;
	auto cidx = [&](int i, int j, int k) { return (size_t(k) * cy + j) * cx + i; };
	auto cornerVox = [&](int i, int j, int k, int c) { return g.I(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)); };
	auto cross = [&](float a, float b) { return (a < 0) != (b < 0); };
	// crossing vertices, keyed by grid edge (lower grid point * 3 + axis)
	std::unordered_map<uint64_t, int32_t> crossVert;
	auto crossing = [&](size_t va, size_t vb, int axis) -> int32_t {
		const size_t lo = std::min(va, vb);
		const uint64_t key = uint64_t(lo) * 3 + uint64_t(axis);
		auto it = crossVert.find(key);
		if (it != crossVert.end()) return it->second;
		const size_t hi = std::max(va, vb);
		const int i = int(lo % g.nx), j = int((lo / g.nx) % g.ny), k = int(lo / (size_t(g.nx) * g.ny));
		const double fa = f[lo], fb = f[hi];
		const double t = Clamp(fa / (fa - fb), 0.02, 0.98);
		const V3 p = g.P(i, j, k) + V3(axis == 0 ? t * g.h : 0, axis == 1 ? t * g.h : 0, axis == 2 ? t * g.h : 0);
		const int32_t id = m.AddPoint(p);
		crossVert.emplace(key, id);
		return id;
	};
	// pass 1: polygon vertices per cell
	for (int k = 0; k < cz; ++k)
		for (int j = 0; j < cy; ++j)
			for (int i = 0; i < cx; ++i)
			{
				double val[8];
				int mask = 0;
				for (int c = 0; c < 8; ++c)
				{
					val[c] = f[cornerVox(i, j, k, c)];
					if (val[c] < 0) mask |= 1 << c;
				}
				if (mask == 0 || mask == 255) continue;
				int parent[12];
				for (int e = 0; e < 12; ++e) parent[e] = e;
				auto find = [&](int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
				auto unite = [&](int a, int b) { parent[find(a)] = find(b); };
				bool cr[12];
				for (int e = 0; e < 12; ++e) cr[e] = ((mask >> kEdgeCorner[e][0]) & 1) != ((mask >> kEdgeCorner[e][1]) & 1);
				for (const FaceDef& fd : faces)
				{
					int ce[4], n = 0;
					for (int q = 0; q < 4; ++q)
						if (cr[fd.e[q]]) ce[n++] = fd.e[q];
					if (n == 2) unite(ce[0], ce[1]);
					else if (n == 4)
					{
						const bool insideJoined = 0.25 * (val[fd.c[0]] + val[fd.c[1]] + val[fd.c[2]] + val[fd.c[3]]) < 0;
						for (int q = 0; q < 4; ++q)
							if ((val[fd.c[q]] < 0) != insideJoined) unite(fd.e[(q + 3) % 4], fd.e[q]);
					}
				}
				CellVerts cv;
				int compVert[12];
				for (int e = 0; e < 12; ++e) { cv.v[e] = -1; compVert[e] = -1; }
				const V3 lo = g.P(i, j, k);
				V3 sum[12];
				int cnt[12] = { 0 };
				for (int e = 0; e < 12; ++e)
				{
					if (!cr[e]) continue;
					const int a = kEdgeCorner[e][0], b = kEdgeCorner[e][1];
					const double t = Clamp(val[a] / (val[a] - val[b]), 0.0, 1.0);
					const V3 pa = lo + V3((a & 1) * g.h, ((a >> 1) & 1) * g.h, ((a >> 2) & 1) * g.h);
					const V3 pb = lo + V3((b & 1) * g.h, ((b >> 1) & 1) * g.h, ((b >> 2) & 1) * g.h);
					const int r = find(e);
					sum[r] += pa + (pb - pa) * t;
					cnt[r]++;
				}
				for (int e = 0; e < 12; ++e)
				{
					if (!cr[e]) continue;
					const int r = find(e);
					if (compVert[r] < 0)
					{
						V3 p = sum[r] * (1.0 / cnt[r]);
						const double eps = 0.02 * g.h;
						for (int it = 0; it < 3; ++it)
						{
							const double v = Sample(g, f, p);
							const V3 gr = Gradient(g, f, p);
							const double gl = Dot(gr, gr);
							if (gl < 1e-12) break;
							p = p - gr * (v / gl);
							p.x = Clamp(p.x, lo.x + eps, lo.x + g.h - eps);
							p.y = Clamp(p.y, lo.y + eps, lo.y + g.h - eps);
							p.z = Clamp(p.z, lo.z + eps, lo.z + g.h - eps);
						}
						compVert[r] = m.AddPoint(p);
					}
					cv.v[e] = compVert[r];
				}
				cellSlot[cidx(i, j, k)] = int32_t(cells.size());
				cells.push_back(cv);
			}
	// pass 2: one quad per arc on every inner cell face (each face once: the upper face of the lower cell)
	for (int k = 0; k < cz; ++k)
		for (int j = 0; j < cy; ++j)
			for (int i = 0; i < cx; ++i)
			{
				const int32_t sa = cellSlot[cidx(i, j, k)];
				if (sa < 0) continue;
				for (int ax = 0; ax < 3; ++ax)
				{
					const int bi = i + (ax == 0), bj = j + (ax == 1), bk = k + (ax == 2);
					if (bi >= cx || bj >= cy || bk >= cz) continue;
					const FaceDef& fd = faces[ax * 2 + 1];   // the face at bit ax = 1
					double val[4];
					size_t vox[4];
					bool in[4];
					int n = 0;
					for (int q = 0; q < 4; ++q)
					{
						vox[q] = cornerVox(i, j, k, fd.c[q]);
						val[q] = f[vox[q]];
						in[q] = val[q] < 0;
					}
					for (int q = 0; q < 4; ++q) n += cross(float(val[q]), float(val[(q + 1) % 4]));
					if (n == 0) continue;
					const int32_t sb = cellSlot[cidx(bi, bj, bk)];
					if (sb < 0) continue;   // cannot happen: the face is shared
					// arcs as pairs of face edge slots (q: edge between corner q and q+1)
					int arcs[2][2], cut[2] = { -1, -1 }, na = 0;
					if (n == 2)
					{
						int e2[2], m2 = 0;
						for (int q = 0; q < 4; ++q)
							if (in[q] != in[(q + 1) % 4]) e2[m2++] = q;
						arcs[na][0] = e2[0]; arcs[na][1] = e2[1]; ++na;
					}
					else
					{
						const bool insideJoined = 0.25 * (val[0] + val[1] + val[2] + val[3]) < 0;
						for (int q = 0; q < 4; ++q)
							if (in[q] != insideJoined) { arcs[na][0] = (q + 3) % 4; arcs[na][1] = q; cut[na] = q; ++na; }
					}
					const int u = (ax + 1) % 3, w = (ax + 2) % 3;
					for (int a = 0; a < na; ++a)
					{
						int32_t X[2];
						V3 XP[2];
						size_t inV = 0, outV = 0;
						for (int s = 0; s < 2; ++s)
						{
							const int q = arcs[a][s];
							const size_t v0 = vox[q], v1 = vox[(q + 1) % 4];
							const int c0 = fd.c[q], c1 = fd.c[(q + 1) % 4];
							const int axis = ((c0 ^ c1) & 1) ? 0 : (((c0 ^ c1) & 2) ? 1 : 2);
							X[s] = crossing(v0, v1, axis);
							XP[s] = m.points[X[s]];
							if (s == 0) { inV = in[q] ? v0 : v1; outV = in[q] ? v1 : v0; }
						}
						// orient X0 -> X1 so that the inside lies to the right in the face's (u, w) frame
						auto co = [&](const V3& p, int c) { return c == 0 ? p.x : (c == 1 ? p.y : p.z); };
						const double du = co(XP[1], u) - co(XP[0], u), dw = co(XP[1], w) - co(XP[0], w);
						double best = 0;
						bool rightIsIn = true;
						for (int q = 0; q < 4; ++q)
						{
							if (cut[a] >= 0 && q != cut[a]) continue;   // an ambiguous face: only the cut-off corner is on a pure side
							const V3 cp = g.P(i + (fd.c[q] & 1), j + ((fd.c[q] >> 1) & 1), k + ((fd.c[q] >> 2) & 1));
							const double s = du * (co(cp, w) - co(XP[0], w)) - dw * (co(cp, u) - co(XP[0], u));   // > 0: left
							if (std::abs(s) > best) { best = std::abs(s); rightIsIn = (s < 0) == in[q]; }
						}
						if (!rightIsIn) { std::swap(X[0], X[1]); }
						const int ea = fd.e[arcs[a][0]];
						const int ca0 = kEdgeCorner[ea][0] & ~(1 << ax), ca1 = kEdgeCorner[ea][1] & ~(1 << ax);
						const int32_t pa = cells[sa].v[ea], pb = cells[sb].v[EdgeOf(ca0, ca1)];
						if (pa < 0 || pb < 0) continue;
						m.AddFace({ pa, X[0], pb, X[1] }, 0, {});
						faceVoxels.push_back({ inV, outV });
					}
				}
			}
}
} // namespace

// основной вход: собираем геометрию по переданным настройкам
bool BuildErosion(const ErosionParams& pIn, const std::vector<V3>& srcPoints, const std::vector<std::array<int32_t, 3>>& srcTris,
	const std::vector<ErosionMarker>& markers, ErosionResult& out)
{
	out = ErosionResult();
	ErosionParams p = pIn;
	const Shape shape(p);
	const uint64_t seed = uint64_t(uint32_t(p.seed)) * 0x9E3779B1ull + 0xE205ull;
	const bool fromSource = p.shape == 0;
	if (fromSource && (srcTris.empty() || srcPoints.empty()))
	{
		out.error = "no source mesh";
		return false;
	}
	// bounds
	V3 lo, hi;
	if (fromSource)
	{
		lo = V3(1e300, 1e300, 1e300);
		hi = V3(-1e300, -1e300, -1e300);
		for (const auto& t : srcTris)
			for (int c = 0; c < 3; ++c)
			{
				if (t[c] < 0 || size_t(t[c]) >= srcPoints.size()) { out.error = "bad source triangle"; return false; }
				const V3& q = srcPoints[t[c]];
				lo = V3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
				hi = V3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
			}
	}
	else
	{
		lo = shape.BoxLo();
		hi = shape.BoxHi();
	}
	const V3 ext = hi - lo;
	if (!(ext.x > 0 && ext.y > 0 && ext.z > 0))
	{
		out.error = "the shape is flat";
		return false;
	}
	double h = std::max(p.resolution, 1e-5);
	const double rough = std::max(p.roughness, 0.0);
	const double sigma = std::max(p.edgeRound, 0.0) * 0.9;
	auto blurRadius = [&](double hh) { return sigma > 0.5 * hh ? std::max(1, int(std::lround(std::sqrt(0.25 + sigma * sigma / (hh * hh)) - 0.5))) : 0; };
	Grid g;
	for (int it = 0; it < 12; ++it)
	{
		// room for the blur (3 box passes reach 3 r voxels) so the border never leaks into the faces
		const double pad = std::max(3.0 * h + 3.0 * rough, (3.0 * blurRadius(h) + 2.0) * h);
		g.h = h;
		g.o = lo - V3(pad, pad, pad);
		g.nx = int(std::ceil((ext.x + 2 * pad) / h)) + 1;
		g.ny = int(std::ceil((ext.y + 2 * pad) / h)) + 1;
		g.nz = int(std::ceil((ext.z + 2 * pad) / h)) + 1;
		const double n = double(g.nx) * g.ny * g.nz;
		if (n <= double(std::max(p.maxVoxels, 1000))) break;
		h *= std::cbrt(n / double(std::max(p.maxVoxels, 1000))) * 1.02;
	}
	out.resolution = h;
	out.voxels = int(g.N());
	const size_t N = g.N();

	const double amount = std::max(p.amount, 0.0);
	// rounding blur radius (voxels) and the band the features need
	const int rb = blurRadius(h);
	const double pitR = std::max(p.pitSize, 0.0), cavR = std::max(p.cavitySize, 0.0) * 0.5, stoneR = std::max(p.aggregateSize, 0.0) * 0.5;
	double featBand = 2.0 * h + 3.0 * rough + std::max(p.strata, 0.0) * 2.0 + std::max(p.cementLoss, 0.0) * 2.0;
	if (p.edgeLoss > 0) featBand = std::max(featBand, 1.6 * p.edgeLossSize + 2 * h);
	if (p.pits > 0) featBand = std::max(featBand, 3.0 * pitR + 2 * h);
	if (p.cavities > 0) featBand = std::max(featBand, 2.5 * cavR + 2 * h);
	if (p.aggregate > 0) featBand = std::max(featBand, 2.5 * stoneR + 2 * h);
	const double B = std::max(featBand, 3.0 * rb * h + 3.0 * h) + 2.0 * h;

#ifdef PDGEN_TRACE
	const auto traceT0 = std::chrono::steady_clock::now();
	std::fprintf(stderr, "  grid %d x %d x %d h %.4f\n", g.nx, g.ny, g.nz, g.h);
#endif
	// 1. distance volume
	std::vector<float> d0(N), work(N), f(N), wear(N, 0.0f);
	std::vector<uint8_t> label(N, kWeathered);
	if (fromSource)
	{
		MeshSdf(g, srcPoints, srcTris, B, d0);
	}
	else
	{
		ParallelFor(g.nz, [&](int k) {
			for (int j = 0; j < g.ny; ++j)
				for (int i = 0; i < g.nx; ++i)
				{
					uint8_t lb;
					const double d = shape.Sdf(g.P(i, j, k), &lb, nullptr);
					const size_t v = g.I(i, j, k);
					d0[v] = float(Clamp(d, -B, B));
					label[v] = lb;
				}
		});
	}
	{
		size_t inside = 0;
		for (float v : d0) inside += v < 0;
		if (inside == 0)
		{
			out.error = fromSource ? "the source mesh encloses no volume at this resolution" : "the shape is smaller than the resolution";
			return false;
		}
	}

	TRACE("sdf");
	// 3. wear
	V3 expo = Norm(V3(p.exposureX, p.exposureY, p.exposureZ), V3(0, 0, 1));
	const double vScale = std::max(p.variationScale, 1e-4);
	auto wearAt = [&](const V3& q, const V3& n) {
		double w = amount;
		if (p.variation > 0) w *= 1.0 - p.variation + p.variation * Sat(0.5 + 0.8 * Fbm3(q.x / vScale, q.y / vScale, q.z / vScale, seed + 11, 3));
		if (p.exposure > 0) w *= 1.0 - Sat(p.exposure) + Sat(p.exposure) * Sat(0.5 + 0.5 * Dot(n, expo));
		if (p.shape == 4)
		{
			uint64_t id = 0;
			shape.Wall(q, &id);
			w *= Lerp(1.0, 0.45 + 1.0 * Hash01(id, 7), Sat(p.variation * 1.6));
		}
		for (const auto& mk : markers)
		{
			const double r = std::max(mk.radius, 1e-6);
			const double dd = Len(q - V3(mk.x, mk.y, mk.z));
			if (dd < r) w += mk.strength * Smooth(1.0 - dd / r);
		}
		return Clamp(w, 0.0, 2.0);
	};
	auto gridNormal = [&](const std::vector<float>& v, int i, int j, int k) {
		const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, g.nx - 1), j0 = std::max(j - 1, 0), j1 = std::min(j + 1, g.ny - 1);
		const int k0 = std::max(k - 1, 0), k1 = std::min(k + 1, g.nz - 1);
		return Norm(V3(v[g.I(i1, j, k)] - v[g.I(i0, j, k)], v[g.I(i, j1, k)] - v[g.I(i, j0, k)], v[g.I(i, j, k1)] - v[g.I(i, j, k0)]));
	};
	const double wearBand = std::max(featBand, 3.0 * rb * h + 2.0 * h);
	ParallelFor(g.nz, [&](int k) {
		for (int j = 0; j < g.ny; ++j)
			for (int i = 0; i < g.nx; ++i)
			{
				const size_t v = g.I(i, j, k);
				if (std::abs(d0[v]) > wearBand) continue;
				wear[v] = float(wearAt(g.P(i, j, k), gridNormal(d0, i, j, k)));
			}
	});

	TRACE("wear");
	// 2. rounded edges
	std::copy(d0.begin(), d0.end(), f.begin());
	if (rb > 0)
	{
		std::copy(d0.begin(), d0.end(), work.begin());
		Blur(g, work, rb);
		for (size_t v = 0; v < N; ++v)
		{
			const double w = Sat(wear[v]);
			if (w <= 0) continue;
			f[v] = float(std::max(double(d0[v]), Lerp(double(d0[v]), double(work[v]), w)));
		}
	}
	TRACE("round");
	// dR: the rounded body, kept for the features below
	std::vector<float> dR(f);

	auto inBand = [&](size_t v) { return std::abs(dR[v]) < featBand; };

	// 4a. edge chunks: Voronoi cells containing an edge are cut away
	if (p.edgeLoss > 0 && p.edgeLossSize > 1.5 * h)
	{
		const double L = p.edgeLossSize;
		Cells cells;
		cells.Init(g, L);
		struct Chunk { V3 c; bool on; };
		std::vector<Chunk> pts;
		// sample directions for the edge test (outside share around the point)
		std::vector<V3> dirs;
		for (int a = 0; a < 27; ++a)
		{
			const int x = a % 3 - 1, y = (a / 3) % 3 - 1, z = a / 9 - 1;
			if (x == 0 && y == 0 && z == 0) continue;
			dirs.push_back(Norm(V3(x, y, z)));
		}
		cells.ForEach([&](int64_t i, int64_t j, int64_t k) {
			const V3 c = CellPoint(seed + 101, i, j, k, L, 0.9);
			const double dc = Sample(g, dR, c);
			bool on = false;
			if (std::abs(dc) < 0.6 * L)
			{
				// the share of the space around the nearest surface point that is outside: 0.5 on a face, ~0.75 on a square edge
				const V3 n = Norm(Gradient(g, dR, c));
				const V3 cs = c - n * dc;
				int outside = 0, inside = 0;
				for (const V3& dv : dirs)
				{
					const double s2 = Sample(g, dR, cs + dv * (0.45 * L));
					if (s2 > 0.06 * L) ++outside;
					else if (s2 < -0.06 * L) ++inside;
				}
				const double share = outside + inside > 0 ? double(outside) / double(outside + inside) : 0.5;
				const double edge = Sat((share - 0.55) / 0.1);
				if (edge > 0)
					on = Hash01(seed + 102, uint64_t(i), uint64_t(j), uint64_t(k)) < 1.4 * p.edgeLoss * edge * Sat(0.3 + wearAt(cs, n));
			}
			cells.Ref(i, j, k) = int32_t(pts.size());
			pts.push_back({ c, on });
		});
		ParallelFor(g.nz, [&](int k) {
			for (int j = 0; j < g.ny; ++j)
				for (int i = 0; i < g.nx; ++i)
				{
					const size_t v = g.I(i, j, k);
					if (std::abs(dR[v]) > 1.6 * L) continue;
					const V3 q = g.P(i, j, k);
					const int64_t ci = int64_t(std::floor(q.x / L)), cj = int64_t(std::floor(q.y / L)), ck = int64_t(std::floor(q.z / L));
					int32_t best = -1;
					double bd = 1e300;
					int32_t nb[27];
					int nn = 0;
					for (int64_t c = ck - 1; c <= ck + 1; ++c)
						for (int64_t b = cj - 1; b <= cj + 1; ++b)
							for (int64_t a = ci - 1; a <= ci + 1; ++a)
							{
								const int32_t s = cells.At(a, b, c);
								if (s < 0) continue;
								nb[nn++] = s;
								const V3 dv = q - pts[s].c;
								const double dd = Dot(dv, dv);
								if (dd < bd) { bd = dd; best = s; }
							}
					if (best < 0 || !pts[best].on) continue;
					// distance to the cell border: the nearest bisector plane
					double inner = 1e300;
					const V3 c1 = pts[best].c;
					for (int a = 0; a < nn; ++a)
					{
						if (nb[a] == best) continue;
						const V3 c2 = pts[nb[a]].c;
						const V3 dc = c2 - c1;
						const double l = Len(dc);
						if (l < 1e-12) continue;
						const double t = (Dot(q - c1, dc) - 0.5 * l * l) / l;   // < 0 inside the cell
						inner = std::min(inner, -t);
					}
					// a little unevenness on the break faces
					inner -= 0.12 * L * (0.5 + 0.5 * Fbm3(q.x / (0.35 * L), q.y / (0.35 * L), q.z / (0.35 * L), seed + 103, 2));
					if (inner > f[v])
					{
						f[v] = float(inner);
						if (dR[v] < 0) label[v] = kFresh;
					}
				}
		});
		for (const Chunk& c : pts) out.chunks += c.on;
	}

	TRACE("chunks");
	// 4b. strata and 4c. roughness
	if (p.strata > 0 || rough > 0)
	{
		const double ang = p.strataAngle * kPi / 180.0;
		const V3 axis(0, std::sin(ang), std::cos(ang));
		const double sp = std::max(p.strataSpacing, 2.0 * h);
		const double rs = std::max(p.roughScale, 1e-4);
		ParallelFor(g.nz, [&](int k) {
			for (int j = 0; j < g.ny; ++j)
				for (int i = 0; i < g.nx; ++i)
				{
					const size_t v = g.I(i, j, k);
					if (!inBand(v)) continue;
					const V3 q = g.P(i, j, k);
					const double w = wear[v];
					double add = 0;
					if (p.strata > 0)
					{
						const double s = Dot(q, axis) / sp + 0.3 * Fbm3(q.x / (4 * sp), q.y / (4 * sp), q.z / (4 * sp), seed + 21, 2);
						const double layer = std::floor(s);
						const double fr = s - layer;
						const double groove = Smooth((0.22 - std::min(fr, 1.0 - fr)) / 0.22);
						const double soft = Hash01(seed + 22, uint64_t(int64_t(layer)));
						add += p.strata * Sat(w) * (groove + 0.55 * soft * soft);
					}
					if (rough > 0)
					{
						const double n1 = Fbm3(q.x / rs, q.y / rs, q.z / rs, seed + 31, 4);
						const double n2 = Fbm3(q.x / (0.35 * rs), q.y / (0.35 * rs), q.z / (0.35 * rs), seed + 32, 2);
						add += rough * (0.4 + 0.6 * Sat(w)) * (n1 + 0.35 * n2) * (label[v] == kFresh ? 0.6 : 1.0);
					}
					f[v] = float(f[v] + add);
				}
		});
	}

	TRACE("strata");
	// 4d. hollows and pits: ellipsoids sunk into the rounded surface
	auto hollows = [&](double density, double size, double depth, uint64_t ch, double blend, bool big, int& count) {
		if (density <= 0 || size < 1.2 * h) return;
		const double L = size * (big ? 1.5 : 1.6);
		Cells cells;
		cells.Init(g, L);
		std::vector<Hollow> list;
		cells.ForEach([&](int64_t i, int64_t j, int64_t k) {
			const V3 c0 = CellPoint(seed + ch, i, j, k, L);
			const double dc = Sample(g, dR, c0);
			if (std::abs(dc) > 0.5 * L) return;
			const V3 n = Norm(Gradient(g, dR, c0));
			const V3 cs = c0 - n * dc;
			const double w = wearAt(cs, n);
			// hollows gather in patches (honeycomb), pits are spread out
			const double patch = big ? Sat(0.35 + 1.1 * Fbm3(cs.x / (3.5 * L), cs.y / (3.5 * L), cs.z / (3.5 * L), seed + ch + 1, 2)) * 1.6 : 1.0;
			if (Hash01(seed + ch, uint64_t(i), uint64_t(j), uint64_t(k) * 7 + 5) >= density * Sat(w) * patch) return;
			Hollow H;
			H.rt = 0.5 * size * (0.55 + 0.45 * Hash01(seed + ch, uint64_t(i), uint64_t(j), uint64_t(k) * 7 + 6)) * (0.7 + 0.3 * Sat(w));
			H.rn = H.rt * (big ? 0.3 + 0.45 * Clamp(depth, 0.0, 1.5) : 0.45 + 0.6 * Clamp(depth, 0.0, 1.5));
			H.n = n;
			Frame(n, H.t1, H.t2);
			H.c = cs + n * (H.rn * ((big ? 0.35 : 0.6) - Clamp(depth, 0.0, 1.5)));
			cells.Ref(i, j, k) = int32_t(list.size());
			list.push_back(H);
		});
		count = int(list.size());
		if (list.empty()) return;
		ParallelFor(g.nz, [&](int k) {
			for (int j = 0; j < g.ny; ++j)
				for (int i = 0; i < g.nx; ++i)
				{
					const size_t v = g.I(i, j, k);
					if (std::abs(dR[v]) > 2.0 * size) continue;
					const V3 q = g.P(i, j, k);
					const int64_t ci = int64_t(std::floor(q.x / L)), cj = int64_t(std::floor(q.y / L)), ck = int64_t(std::floor(q.z / L));
					double R = 1e300, rt = 0;
					for (int64_t c = ck - 1; c <= ck + 1; ++c)
						for (int64_t b = cj - 1; b <= cj + 1; ++b)
							for (int64_t a = ci - 1; a <= ci + 1; ++a)
							{
								const int32_t s = cells.At(a, b, c);
								if (s < 0) continue;
								const Hollow& H = list[s];
								const V3 dq = q - H.c;
								const double e = SdEllipsoid(V3(Dot(dq, H.t1), Dot(dq, H.t2), Dot(dq, H.n)), V3(H.rt, H.rt, H.rn));
								if (e < R) { R = e; rt = H.rt; }
							}
					if (R > 1e299) continue;
					if (big) R += 0.22 * rt * Fbm3(q.x / (0.45 * rt), q.y / (0.45 * rt), q.z / (0.45 * rt), seed + ch + 2, 2);   // irregular walls
					f[v] = float(SMax(double(f[v]), -R, blend * rt));
				}
		});
	};
	hollows(Clamp(p.cavities, 0.0, 1.0) * 0.9, std::max(p.cavitySize, 0.0), p.cavityDepth, 41, 0.3, true, out.cavities);
	TRACE("cavities");
	hollows(Clamp(p.pits, 0.0, 1.0) * 0.8, std::max(p.pitSize, 0.0) * 2.0, p.pitDepth, 51, 0.25, false, out.pits);

	TRACE("pits");
	// 4e. exposed aggregate: the cement is worn back, the stones stay
	if (p.aggregate > 0 && stoneR > 1.25 * h)   // smaller stones would be voxel noise
	{
		const double L = 2.0 * stoneR * 1.15;
		Cells cells;
		cells.Init(g, L);
		struct Stone { V3 c, a, b, n; V3 r; };
		std::vector<Stone> list;
		cells.ForEach([&](int64_t i, int64_t j, int64_t k) {
			const V3 c = CellPoint(seed + 61, i, j, k, L, 0.6);
			const double dc = Sample(g, dR, c);
			const double r = stoneR * (0.65 + 0.35 * Hash01(seed + 61, uint64_t(i), uint64_t(j), uint64_t(k) * 9 + 1));
			if (dc > 0.8 * r || dc < -2.5 * r) return;
			if (Hash01(seed + 62, uint64_t(i), uint64_t(j), uint64_t(k)) >= Clamp(p.aggregate, 0.0, 1.0)) return;
			Rng rng(Hash(seed + 63, uint64_t(i), uint64_t(j), uint64_t(k)), 1);
			Stone S;
			S.c = c;
			S.n = Norm(V3(rng.Signed(), rng.Signed(), rng.Signed()));
			Frame(S.n, S.a, S.b);
			S.r = V3(r, r * rng.Range(0.75, 1.0), r * rng.Range(0.55, 0.85));
			cells.Ref(i, j, k) = int32_t(list.size());
			list.push_back(S);
		});
		out.stones = int(list.size());
		const double loss = std::max(p.cementLoss, 0.0);
		ParallelFor(g.nz, [&](int k) {
			for (int j = 0; j < g.ny; ++j)
				for (int i = 0; i < g.nx; ++i)
				{
					const size_t v = g.I(i, j, k);
					if (std::abs(dR[v]) > 3.0 * stoneR + loss) continue;
					if (label[v] == kMortar) continue;
					const V3 q = g.P(i, j, k);
					const double fc = f[v] + loss * (0.3 + 0.7 * Sat(wear[v]));
					const int64_t ci = int64_t(std::floor(q.x / L)), cj = int64_t(std::floor(q.y / L)), ck = int64_t(std::floor(q.z / L));
					double s = 1e300;
					for (int64_t c = ck - 1; c <= ck + 1; ++c)
						for (int64_t b = cj - 1; b <= cj + 1; ++b)
							for (int64_t a = ci - 1; a <= ci + 1; ++a)
							{
								const int32_t id = cells.At(a, b, c);
								if (id < 0) continue;
								const Stone& S = list[id];
								const V3 dq = q - S.c;
								s = std::min(s, SdEllipsoid(V3(Dot(dq, S.a), Dot(dq, S.b), Dot(dq, S.n)), S.r));
							}
					const double st = SMax(s, double(dR[v]), 0.25 * stoneR);
					if (st < fc)
					{
						f[v] = float(st);
						label[v] = kStone;
					}
					else f[v] = float(fc);
				}
		});
	}
	for (int k = 0; k < g.nz; ++k)
		for (int j = 0; j < g.ny; ++j)
			for (int i = 0; i < g.nx; ++i)
				if (i == 0 || j == 0 || k == 0 || i == g.nx - 1 || j == g.ny - 1 || k == g.nz - 1) f[g.I(i, j, k)] = float(B);

	TRACE("aggregate");
	// 5. mesh
	GenMesh& m = out.mesh;
	std::vector<std::array<size_t, 2>> faceVox;
	SurfaceNets(g, f, m, faceVox);
	if (m.faceSizes.empty())
	{
		out.error = "nothing left after the weathering";
		return false;
	}

	TRACE("nets");
	// 6. drop loose bits
	{
		const size_t nv = m.points.size(), nf = m.faceSizes.size();
		std::vector<int32_t> par(nv);
		for (size_t i = 0; i < nv; ++i) par[i] = int32_t(i);
		auto find = [&](int32_t x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
		for (size_t fi = 0; fi < nf; ++fi)
			for (int c = 1; c < 4; ++c) par[find(m.faceVerts[fi * 4 + c])] = find(m.faceVerts[fi * 4]);
		std::unordered_map<int32_t, size_t> faceCount;
		for (size_t fi = 0; fi < nf; ++fi) faceCount[find(m.faceVerts[fi * 4])]++;
		const double minFaces = std::max(8.0, Clamp(p.minPiece, 0.0, 1.0) * double(nf));
		GenMesh k;
		std::vector<int32_t> remap(nv, -1);
		std::vector<std::array<size_t, 2>> keptVox;
		std::unordered_map<int32_t, int> pieceOf;
		for (size_t fi = 0; fi < nf; ++fi)
		{
			const int32_t r = find(m.faceVerts[fi * 4]);
			if (double(faceCount[r]) < minFaces) continue;
			if (!pieceOf.count(r)) pieceOf[r] = int(pieceOf.size());
			int32_t q[4];
			for (int c = 0; c < 4; ++c)
			{
				const int32_t a = m.faceVerts[fi * 4 + c];
				if (remap[a] < 0) remap[a] = k.AddPoint(m.points[a]);
				q[c] = remap[a];
			}
			// a face on a cut-away chunk: the removed voxel outside says so
			const uint8_t lo = label[faceVox[fi][1]];
			const uint8_t lb = lo == kFresh || lo == kStone ? lo : label[faceVox[fi][0]];
			k.AddFace({ q[0], q[1], q[2], q[3] }, int32_t(lb), {});
			keptVox.push_back(faceVox[fi]);
		}
		out.pieces = int(pieceOf.size());
		m = std::move(k);
		faceVox.swap(keptVox);
	}
	if (m.faceSizes.empty())
	{
		out.error = "nothing left after the weathering";
		return false;
	}

	TRACE("cleanup");
	// UVs: box projection by the face normal
	{
		const double us = p.uvScale;
		for (size_t fi = 0; fi < m.faceSizes.size(); ++fi)
		{
			const int32_t* q = &m.faceVerts[fi * 4];
			const V3 n = Cross(m.points[q[2]] - m.points[q[0]], m.points[q[3]] - m.points[q[1]]);
			const double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
			for (int c = 0; c < 4; ++c)
			{
				const V3& v = m.points[q[c]];
				std::array<double, 2> uv;
				if (az >= ax && az >= ay) uv = { (n.z >= 0 ? v.x : -v.x) * us, v.y * us };
				else if (ax >= ay) uv = { (n.x >= 0 ? v.y : -v.y) * us, v.z * us };
				else uv = { (n.y >= 0 ? -v.x : v.x) * us, v.z * us };
				m.loopUv[fi * 4 + c] = uv;
			}
		}
	}

	// attributes
	{
		// curvature at the cavity scale: blur of the final field, < 0 in hollows, > 0 on edges
		std::copy(f.begin(), f.end(), work.begin());
		const double sc = std::max(2.0 * h, 0.5 * std::max(pitR * 2.0, 0.25 * std::max(p.edgeRound, 0.0)));
		const int rc = std::max(1, int(std::lround(sc / h)));
		Blur(g, work, rc);
		const double ref = 0.45 * rc * h;
		const double depthRef = std::max({ p.edgeRound * 0.5, cavR * std::max(p.cavityDepth, 0.2) * (p.cavities > 0 ? 1.0 : 0.0),
			pitR * 1.5, p.edgeLossSize * (p.edgeLoss > 0 ? 0.5 : 0.0), 2.0 * h });
		auto& aW = m.Attr("erosion_wear");
		auto& aD = m.Attr("erosion_depth");
		auto& aC = m.Attr("erosion_cavity");
		auto& aE = m.Attr("erosion_edge");
		for (size_t i = 0; i < m.points.size(); ++i)
		{
			const V3& q = m.points[i];
			aW[i] = float(Sat(Sample(g, wear, q)));
			aD[i] = float(Sat(-Sample(g, d0, q) / depthRef));
			const double c = Sample(g, work, q) / ref;
			aC[i] = float(Sat(-c));
			aE[i] = float(Sat(c));
		}
	}
	TRACE("attrs");
	m.FinishAttrs();
	out.ok = true;
	return true;
}

} // namespace pdgen
