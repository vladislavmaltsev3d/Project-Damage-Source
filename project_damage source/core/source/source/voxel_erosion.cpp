// разрушаем объём и собираем новую поверхность по оставшейся форме
// ProjectDamage voxel erosion — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Recipe: voxel-erosion-v1. Broken, weathered edges on any closed mesh.
// Reimplements, as a signed-distance pipeline, the "remesh -> round ->
// noise displace -> intersect with the original" damage setup:
//
//   1. pieces: the input polygons are split into connected pieces; each piece
//      is eroded on its own (its own grid, its own noise offset), so two
//      bricks that touch never protect or erase each other;
//   2. reference space: a piece (or p.refSize) is mapped so its largest
//      dimension is 2; the voxel size, the rounding, the displacement and
//      the noise all live in that space, so the look is size independent;
//   3. signed distance S on a voxel grid (positive inside): exact sign by
//      ray parity along +X, exact distance by a closest-triangle BVH inside
//      a band, clamped beyond it;
//   4. rounding: S is Gaussian-blurred. sigma reproduces the original's mesh
//      smoothing (5 full passes + 100 passes weighted by Damage); the blur
//      radius is limited by the local thickness, so thin plates round to a
//      bullnose instead of vanishing (as a mesh smoother would behave);
//   5. displacement along the normal == adding to the field:
//        K = Ks + k * 0.25 * (T(p) + offset - 0.5)
//      T is one of four domain-warped noises, brightened by Contrast and
//      clamped at 1 (clamped = intact). Image mask: + k*0.5*m*(0.215+adj);
//   6. Post Smooth: K is blurred again; Thickness Cap: K >= S - cap*2*M;
//      zone / avoid / field-mask weight w: K = w*K + (1-w)*(S + margin);
//      clip: K <= S + margin (the kept shape never bulges into neighbours);
//   7. Manifold::LevelSet(K) per piece -> eroded solid E_i, always a clean
//      2-manifold (no self-intersections, unlike displaced meshes);
//   8. one cutter = box around the object - union(E_i).
//
// Pure function of its inputs; every piece seeds its own noise offset from
// (mainSeed, p.seed, piece index), pieces ordered by their first vertex.
#include "voxel_erosion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

inline double Clamp01d(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline double SmoothStep01(double x) { x = Clamp01d(x); return x * x * (3.0 - 2.0 * x); }
inline double SmootherStep01(double x) { x = Clamp01d(x); return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
inline double Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V Cross(const V& a, const V& b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline V Mul(const V& a, const V& b) { return V(a.x * b.x, a.y * b.y, a.z * b.z); }
inline V Lerp3(const V& a, const V& b, double t) { return a + (b - a) * t; }

                                                                              
                                                                           

inline uint64_t Mix64(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}
inline uint64_t Hash3(int x, int y, int z, uint64_t salt)
{
	return Mix64((uint64_t)(uint32_t)x ^ Mix64((uint64_t)(uint32_t)y ^ Mix64((uint64_t)(uint32_t)z ^ Mix64(salt))));
}
inline double U01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }

inline double GradDot(uint64_t h, double x, double y, double z)
{
	switch ((int)(h % 12u)) {
	case 0: return x + y;  case 1: return -x + y; case 2: return x - y;  case 3: return -x - y;
	case 4: return x + z;  case 5: return -x + z; case 6: return x - z;  case 7: return -x - z;
	case 8: return y + z;  case 9: return -y + z; case 10: return y - z; default: return -y - z;
	}
}
inline double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

                                          
double Perlin(const V& p, uint64_t salt)
{
	const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const int X = (int)fx, Y = (int)fy, Z = (int)fz;
	const double x = p.x - fx, y = p.y - fy, z = p.z - fz;
	const double u = Fade(x), v = Fade(y), w = Fade(z);
	auto g = [&](int i, int j, int k) { return GradDot(Hash3(X + i, Y + j, Z + k, salt), x - i, y - j, z - k); };
	const double x00 = g(0, 0, 0) + u * (g(1, 0, 0) - g(0, 0, 0));
	const double x10 = g(0, 1, 0) + u * (g(1, 1, 0) - g(0, 1, 0));
	const double x01 = g(0, 0, 1) + u * (g(1, 0, 1) - g(0, 0, 1));
	const double x11 = g(0, 1, 1) + u * (g(1, 1, 1) - g(0, 1, 1));
	const double y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
	return (y0 + w * (y1 - y0)) * 0.97;
}

                                          
double Fbm(V p, double detail, double rough, double lac, uint64_t salt)
{
	double amp = 1.0, maxAmp = 0.0, sum = 0.0;
	const int n = (int)std::floor(detail);
	for (int i = 0; i <= n; ++i) { sum += Perlin(p, salt + (uint64_t)i) * amp; maxAmp += amp; amp *= rough; p = p * lac; }
	const double rmd = detail - std::floor(detail);
	double out = 0.5 + 0.5 * sum / maxAmp;
	if (rmd > 1e-9) {
		const double s2 = sum + Perlin(p, salt + (uint64_t)(n + 1)) * amp;
		out = out + rmd * ((0.5 + 0.5 * s2 / (maxAmp + amp)) - out);
	}
	return out;
}

                                                                       
                                                 
double Hetero(V p, double detail, double rough, double lac, uint64_t salt)
{
	double pwr = rough, value = Perlin(p, salt);
	p = p * lac;
	const int n = (int)std::floor(detail);
	for (int i = 1; i <= n; ++i) {
		value += Perlin(p, salt + (uint64_t)i) * pwr * value;
		pwr *= rough; p = p * lac;
	}
	return value;
}

inline V CellSite(int x, int y, int z, double randomness)
{
	const uint64_t h = Hash3(x, y, z, 0x51D3A9ull);
	return V(U01(h), U01(Mix64(h + 1)), U01(Mix64(h + 2))) * randomness;
}

double VoronoiF1Raw(const V& p, double randomness)
{
	const V c(std::floor(p.x), std::floor(p.y), std::floor(p.z)), l = p - c;
	double best = 1e30;
	for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
		const V d = V(i, j, k) + CellSite((int)c.x + i, (int)c.y + j, (int)c.z + k, randomness) - l;
		best = std::min(best, Dot(d, d));
	}
	return std::sqrt(best);
}

double VoronoiSmoothF1Raw(const V& p, double smoothness, double randomness)
{
	const V c(std::floor(p.x), std::floor(p.y), std::floor(p.z)), l = p - c;
	const double s = std::max(1e-4, Clamp01d(smoothness * 0.5) );
	double sd = 8.0;
	for (int k = -2; k <= 2; ++k) for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
		const V d = V(i, j, k) + CellSite((int)c.x + i, (int)c.y + j, (int)c.z + k, randomness) - l;
		const double dist = std::sqrt(Dot(d, d));
		const double h = SmoothStep01(0.5 + 0.5 * (sd - dist) / s);
		const double corr = s * h * (1.0 - h);
		sd = sd + (dist - sd) * h - corr;
	}
	return sd;
}

double VoronoiEdgeRaw(const V& p, double randomness)
{
	const V c(std::floor(p.x), std::floor(p.y), std::floor(p.z)), l = p - c;
	V toClosest(0.0); double best = 1e30;
	for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
		const V d = V(i, j, k) + CellSite((int)c.x + i, (int)c.y + j, (int)c.z + k, randomness) - l;
		const double dd = Dot(d, d);
		if (dd < best) { best = dd; toClosest = d; }
	}
	double minEdge = 8.0;
	for (int k = -1; k <= 1; ++k) for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
		const V d = V(i, j, k) + CellSite((int)c.x + i, (int)c.y + j, (int)c.z + k, randomness) - l;
		const V perp = d - toClosest;
		const double pl = Dot(perp, perp);
		if (pl > 1e-8) minEdge = std::min(minEdge, Dot((toClosest + d) * 0.5, perp / std::sqrt(pl)));
	}
	return std::max(0.0, minEdge);
}

                                                                        
                                                                          
double FractalF1(V p, double detail, double rough, double lac, double randomness, bool smooth, double smoothness)
{
	const double maxD = std::sqrt(3.0) * (0.5 + 0.5 * randomness);
	double amp = 1.0, maxAmp = 0.0, sum = 0.0, scale = 1.0;
	for (int i = 0; i <= (int)std::floor(detail); ++i) {
		const V q = p * scale;
		const double d = smooth ? VoronoiSmoothF1Raw(q, smoothness, randomness) : VoronoiF1Raw(q, randomness);
		sum += d * amp; maxAmp += amp; amp *= rough; scale *= lac;
	}
	return Clamp01d(sum / (maxAmp * std::max(1e-6, maxD)));
}

double FractalEdge(V p, double detail, double lac, double randomness)
{
	double best = 8.0, scale = 1.0;
	for (int i = 0; i <= (int)std::floor(detail); ++i) {
		best = std::min(best, VoronoiEdgeRaw(p * scale, randomness) / scale);
		scale *= lac;
	}
	return best;
}

                                                                              
                                

struct Tri { V a, b, c; int32_t id; };

                                                                             
                                          
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c, double& u, double& v, double& w)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) { u = 1; v = 0; w = 0; return a; }
	const V bp = p - b; const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) { u = 0; v = 1; w = 0; return b; }
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) { const double t = d1 / (d1 - d3); u = 1 - t; v = t; w = 0; return a + ab * t; }
	const V cp = p - c; const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) { u = 0; v = 0; w = 1; return c; }
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) { const double t = d2 / (d2 - d6); u = 1 - t; v = 0; w = t; return a + ac * t; }
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); u = 0; v = 1 - t; w = t; return b + (c - b) * t;
	}
	const double den = 1.0 / (va + vb + vc);
	v = vb * den; w = vc * den; u = 1.0 - v - w;
	return a + ab * v + ac * w;
}

struct Bvh
{
	struct Node { V lo, hi; int32_t left = -1, right = -1, first = 0, count = 0; };
	std::vector<Node> nodes;
	std::vector<int32_t> order;
	const std::vector<Tri>* tris = nullptr;

	void Build(const std::vector<Tri>& t)
	{
		tris = &t;
		order.resize(t.size());
		std::iota(order.begin(), order.end(), 0);
		std::vector<V> cen(t.size());
		for (size_t i = 0; i < t.size(); ++i) cen[i] = (t[i].a + t[i].b + t[i].c) * (1.0 / 3.0);
		nodes.clear(); nodes.reserve(t.size() * 2 + 1);
		if (!t.empty()) BuildRec(0, (int32_t)t.size(), cen);
	}
	int32_t BuildRec(int32_t first, int32_t count, const std::vector<V>& cen)
	{
		const int32_t id = (int32_t)nodes.size();
		nodes.emplace_back();
		V lo(1e300), hi(-1e300);
		for (int32_t i = first; i < first + count; ++i) {
			const Tri& t = (*tris)[(size_t)order[(size_t)i]];
			for (const V* q : { &t.a, &t.b, &t.c }) {
				lo = V(std::min(lo.x, q->x), std::min(lo.y, q->y), std::min(lo.z, q->z));
				hi = V(std::max(hi.x, q->x), std::max(hi.y, q->y), std::max(hi.z, q->z));
			}
		}
		nodes[(size_t)id].lo = lo; nodes[(size_t)id].hi = hi;
		if (count <= 4) { nodes[(size_t)id].first = first; nodes[(size_t)id].count = count; return id; }
		const V e = hi - lo;
		const int ax = e.x >= e.y && e.x >= e.z ? 0 : (e.y >= e.z ? 1 : 2);
		const int32_t mid = first + count / 2;
		std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
			[&](int32_t a, int32_t b) { return cen[(size_t)a][ax] < cen[(size_t)b][ax]; });
		const int32_t l = BuildRec(first, mid - first, cen);
		const int32_t r = BuildRec(mid, first + count - mid, cen);
		nodes[(size_t)id].left = l; nodes[(size_t)id].right = r;
		return id;
	}
	static double BoxD2(const Node& n, const V& p)
	{
		const double dx = std::max({ n.lo.x - p.x, 0.0, p.x - n.hi.x });
		const double dy = std::max({ n.lo.y - p.y, 0.0, p.y - n.hi.y });
		const double dz = std::max({ n.lo.z - p.z, 0.0, p.z - n.hi.z });
		return dx * dx + dy * dy + dz * dz;
	}
	                                                                       
	                                                                    
	template <class F>
	int32_t Closest(const V& p, double& best2, double& bu, double& bv, double& bw, const F* accept) const
	{
		if (nodes.empty()) return -1;
		int32_t stack[128]; int sp = 0; int32_t hit = -1;
		stack[sp++] = 0;
		while (sp > 0) {
			const Node& n = nodes[(size_t)stack[--sp]];
			if (BoxD2(n, p) >= best2) continue;
			if (n.left < 0) {
				for (int32_t i = n.first; i < n.first + n.count; ++i) {
					const int32_t ti = order[(size_t)i];
					const Tri& t = (*tris)[(size_t)ti];
					if (accept && !(*accept)(ti)) continue;
					double u, v, w;
					const V q = ClosestOnTri(p, t.a, t.b, t.c, u, v, w);
					const V d = q - p; const double d2 = Dot(d, d);
					if (d2 < best2) { best2 = d2; hit = ti; bu = u; bv = v; bw = w; }
				}
				continue;
			}
			const Node& L = nodes[(size_t)n.left]; const Node& R = nodes[(size_t)n.right];
			const double dl = BoxD2(L, p), dr = BoxD2(R, p);
			if (sp > 124) continue;                                                
			if (dl < dr) { if (dr < best2) stack[sp++] = n.right; if (dl < best2) stack[sp++] = n.left; }
			else         { if (dl < best2) stack[sp++] = n.left;  if (dr < best2) stack[sp++] = n.right; }
		}
		return hit;
	}
};
using AcceptAll = bool (*)(int32_t);

                                                                              
               

struct Grid
{
	int nx = 0, ny = 0, nz = 0;
	V lo{ 0.0 };
	double h = 1.0;
	size_t Size() const { return (size_t)nx * (size_t)ny * (size_t)nz; }
	size_t Idx(int i, int j, int k) const { return (size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k); }
	V Pos(int i, int j, int k) const { return lo + V(i, j, k) * h; }
};

                                                           
void Blur(const Grid& g, std::vector<float>& f, double sigma)
{
	if (sigma < 0.3) return;
	const int r = std::max(1, (int)std::ceil(3.0 * sigma));
	std::vector<double> ker((size_t)(2 * r + 1));
	double s = 0.0;
	for (int i = -r; i <= r; ++i) { ker[(size_t)(i + r)] = std::exp(-0.5 * (i * i) / (sigma * sigma)); s += ker[(size_t)(i + r)]; }
	for (double& k : ker) k /= s;
	std::vector<float> tmp(f.size());
	const int dims[3] = { g.nx, g.ny, g.nz };
	for (int axis = 0; axis < 3; ++axis) {
		const int n = dims[axis];
		const int a1 = axis == 0 ? 1 : 0, a2 = axis == 2 ? 1 : 2;
		const int n1 = dims[a1], n2 = dims[a2];
		const size_t stride = axis == 0 ? 1 : (axis == 1 ? (size_t)g.nx : (size_t)g.nx * (size_t)g.ny);
		ProjectDamageParallelFor(0, (int64_t)n1 * n2, [&](int64_t line) {
			int c[3] = { 0, 0, 0 };
			c[a1] = (int)(line % n1); c[a2] = (int)(line / n1); c[axis] = 0;
			const size_t base = g.Idx(c[0], c[1], c[2]);
			for (int i = 0; i < n; ++i) {
				double acc = 0.0;
				for (int t = -r; t <= r; ++t) {
					const int q = std::min(n - 1, std::max(0, i + t));
					acc += ker[(size_t)(t + r)] * f[base + (size_t)q * stride];
				}
				tmp[base + (size_t)i * stride] = (float)acc;
			}
		});
		f.swap(tmp);
	}
}

                                                              
void MaxFilter(const Grid& g, std::vector<float>& f, int r)
{
	if (r <= 0) return;
	std::vector<float> tmp(f.size());
	const int dims[3] = { g.nx, g.ny, g.nz };
	for (int axis = 0; axis < 3; ++axis) {
		const int n = dims[axis];
		const int a1 = axis == 0 ? 1 : 0, a2 = axis == 2 ? 1 : 2;
		const int n1 = dims[a1], n2 = dims[a2];
		const size_t stride = axis == 0 ? 1 : (axis == 1 ? (size_t)g.nx : (size_t)g.nx * (size_t)g.ny);
		ProjectDamageParallelFor(0, (int64_t)n1 * n2, [&](int64_t line) {
			int c[3] = { 0, 0, 0 };
			c[a1] = (int)(line % n1); c[a2] = (int)(line / n1);
			const size_t base = g.Idx(c[0], c[1], c[2]);
			std::deque<int> dq;
			int next = 0;
			for (int i = 0; i < n; ++i) {
				const int hiI = std::min(n - 1, i + r);
				while (next <= hiI) {
					while (!dq.empty() && f[base + (size_t)dq.back() * stride] <= f[base + (size_t)next * stride]) dq.pop_back();
					dq.push_back(next++);
				}
				while (dq.front() < i - r) dq.pop_front();
				tmp[base + (size_t)i * stride] = f[base + (size_t)dq.front() * stride];
			}
		});
		f.swap(tmp);
	}
}

                                                                              
                           

struct NoiseSetup
{
	int type = 2;
	double scale = 1.0, dodge = 0.0;
	V rnd{ 0.0 }, pos{ 0.0 }, scl{ 1.0 };
	m::mat3 rot = m::mat3(V(1, 0, 0), V(0, 1, 0), V(0, 0, 1));
};

inline V Rotate(const NoiseSetup& n, const V& v) { return m::la::mul(n.rot, v); }
                                                                             
inline V Transform(const NoiseSetup& n, const V& u, const V& warp, double mod)
{
	return Mul(Rotate(n, Lerp3(u, warp, mod)) - n.pos, n.scl);
}
inline double Dodge(double a, double f)
{
	if (a <= 0.0) return 0.0;
	const double t = 1.0 - f;
	return t <= 0.0 ? 1.0 : std::min(1.0, a / t);
}

                          
double ErosionNoise(const NoiseSetup& n, const V& u)
{
	const V q0 = Transform(n, u, V(0.0), 0.0);
	switch (n.type) {
	case 0: {                                                   
		const double f1 = FractalF1(q0 * n.scale, 2.0, 0.5, 2.0, 0.75, false, 0.0);
		const V q = (Transform(n, u, V(f1), 0.25) + n.rnd) * n.scale;
		double acc = 0.0;
		for (int c = 0; c < 3; ++c)
			acc += Dodge(Hetero(q + V(c * 31.7, c * 17.3, c * 9.1), 2.0, 0.5, 4.0, 101u + (uint64_t)c) + 0.6, n.dodge);
		return acc / 3.0;
	}
	case 1: {               
		const V q = (q0 + n.rnd) * n.scale;
		double acc = 0.0;
		for (int c = 0; c < 3; ++c)
			acc += Dodge(Fbm(q + V(c * 31.7, c * 17.3, c * 9.1), 2.0, 0.5, 4.0, 211u + (uint64_t)c), n.dodge);
		return acc / 3.0;
	}
	default: {
		const V w(Fbm(q0 * 2.0, 2.0, 0.5, 2.0, 307u), Fbm(q0 * 2.0 + V(13.1, 7.7, 3.3), 2.0, 0.5, 2.0, 311u),
			Fbm(q0 * 2.0 + V(5.9, 21.4, 11.8), 2.0, 0.5, 2.0, 313u));
		if (n.type == 2) {                                        
			const V q = (Transform(n, u, w, 0.25) + n.rnd) * n.scale;
			return Dodge(FractalEdge(q, 2.0, 2.0, 0.75) + 0.41, n.dodge);
		}
		const V q = (Transform(n, u, w, 0.15) + n.rnd) * n.scale;                      
		return Dodge(FractalF1(q, 1.0, 0.25, 2.0, 1.0, true, 1.0) + 0.2, n.dodge);
	}
	}
}

m::mat3 EulerXYZ(double x, double y, double z)
{
	const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
	                             
	return m::mat3(V(cy * cz, cy * sz, -sy),
		V(sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy),
		V(cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy));
}

                                                                              
            

struct PieceTri { int32_t poly; int32_t p[3]; int32_t corner[3]; };

struct SharedInputs
{
	const CoreInput* in = nullptr;
	const VoxelErosionParams* p = nullptr;
	std::vector<double> pointWeight;                   
	bool image = false;
	bool boxUv = true;
};

double SampleImage(const VoxelErosionParams& p, double u, double v)
{
	const int W = p.maskWidth, H = p.maskHeight;
	const double x = (u - std::floor(u)) * W - 0.5, y = (v - std::floor(v)) * H - 0.5;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const double fx = x - x0, fy = y - y0;
	auto px = [&](int i, int j) {
		i = ((i % W) + W) % W; j = ((j % H) + H) % H;
		return (double)p.maskPixels[(size_t)j * (size_t)W + (size_t)i];
	};
	const double a = px(x0, y0) + fx * (px(x0 + 1, y0) - px(x0, y0));
	const double b = px(x0, y0 + 1) + fx * (px(x0 + 1, y0 + 1) - px(x0, y0 + 1));
	return Clamp01d(a + fy * (b - a));
}

bool ErodePiece(const SharedInputs& S, const std::vector<PieceTri>& ptris, int pieceIndex, Int32 mainSeed,
	m::Manifold& eroded, bool& coarsened)
{
	const CoreInput& in = *S.in;
	const VoxelErosionParams& p = *S.p;
	const EVec* P = in.view.pts;

	std::vector<Tri> tris(ptris.size());
	V lo(1e300), hi(-1e300);
	double vol = 0.0;
	for (size_t i = 0; i < ptris.size(); ++i) {
		const PieceTri& t = ptris[i];
		tris[i] = Tri{ P[t.p[0]], P[t.p[1]], P[t.p[2]], (int32_t)i };
		vol += Dot(tris[i].a, Cross(tris[i].b, tris[i].c)) / 6.0;
		for (int c = 0; c < 3; ++c) {
			const V& q = P[t.p[c]];
			lo = V(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
			hi = V(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
		}
	}
	const V ext = hi - lo;
	const double dmax = std::max({ ext.x, ext.y, ext.z });
	if (!(vol > 1e-12 * dmax * dmax * dmax) || !(dmax > 0.0)) return false;                                      

	                                                                                   
	const double R = p.refSize > 0.0 ? p.refSize : dmax;
	const double k = 0.5 * R;                                                          
	const double d = Clamp01d(p.damage / 100.0);
	auto lam = [](double w) { return 6.0 * w / (1.0 + 6.0 * w); };                             
	const double s0 = 5.0 * lam(1.0) * 0.5;
	const double s1 = p.roundFirst ? 100.0 * lam(0.0015 + 0.9985 * SmoothStep01(d)) * 0.5 : 0.0;
	const double sigma = std::sqrt(s0 + s1);                    
	const double sigmaPost = std::sqrt(20.0 * lam(0.15 * SmoothStep01(p.postSmooth / 100.0)) * 0.5);
	const double offset = p.roundFirst ? 0.3 + (0.025 - 0.3) * SmootherStep01(d) : 0.15 * (1.0 - d);
	const double adj = std::max(-1.0, std::min(1.0, p.maskAdjust));

	double h = std::max(1e-4, p.voxelSize / 10.0) * k;
	Grid g;
	int padVox = 0;
	for (int guard = 0; guard < 60; ++guard) {
		padVox = (int)std::ceil(1.5 + 3.0 * sigma + 3.0 * sigmaPost + 3.0);
		g.h = h;
		g.lo = lo - V(padVox * h);
		g.nx = (int)std::ceil(ext.x / h) + 1 + 2 * padVox;
		g.ny = (int)std::ceil(ext.y / h) + 1 + 2 * padVox;
		g.nz = (int)std::ceil(ext.z / h) + 1 + 2 * padVox;
		if ((double)g.nx * g.ny * g.nz <= (double)std::max<int32_t>(100000, p.maxVoxels)) break;
		h *= 1.08; coarsened = true;
	}
	const double margin = 1.5 * h;
	const double B = (4.5 * sigma + 3.0 * sigmaPost + 4.0) * h + 0.15 * k + margin;
	const size_t N = g.Size();

	Bvh bvh; bvh.Build(tris);

	                                                                                       
	std::vector<uint8_t> inside(N, 0);
	{
		const int bs = 4;
		const int by = g.ny / bs + 1, bz = g.nz / bs + 1;
		std::vector<std::vector<int32_t>> bins((size_t)by * (size_t)bz);
		for (size_t t = 0; t < tris.size(); ++t) {
			const Tri& T = tris[t];
			const double y0 = std::min({ T.a.y, T.b.y, T.c.y }), y1 = std::max({ T.a.y, T.b.y, T.c.y });
			const double z0 = std::min({ T.a.z, T.b.z, T.c.z }), z1 = std::max({ T.a.z, T.b.z, T.c.z });
			const int j0 = std::max(0, (int)std::floor((y0 - g.lo.y) / h) / bs), j1 = std::min(by - 1, (int)std::ceil((y1 - g.lo.y) / h) / bs);
			const int k0 = std::max(0, (int)std::floor((z0 - g.lo.z) / h) / bs), k1 = std::min(bz - 1, (int)std::ceil((z1 - g.lo.z) / h) / bs);
			for (int kk = k0; kk <= k1; ++kk) for (int jj = j0; jj <= j1; ++jj) bins[(size_t)kk * by + jj].push_back((int32_t)t);
		}
		ProjectDamageParallelFor(0, (int64_t)g.ny * g.nz, [&](int64_t row) {
			const int j = (int)(row % g.ny), kk = (int)(row / g.ny);
			const double y = g.lo.y + j * h + h * 1.2345671e-4, z = g.lo.z + kk * h + h * 7.6543217e-5;
			std::vector<double> xs;
			for (int32_t t : bins[(size_t)(kk / bs) * by + (size_t)(j / bs)]) {
				const Tri& T = tris[(size_t)t];
				const double e0 = (T.b.y - T.a.y) * (z - T.a.z) - (T.b.z - T.a.z) * (y - T.a.y);
				const double e1 = (T.c.y - T.b.y) * (z - T.b.z) - (T.c.z - T.b.z) * (y - T.b.y);
				const double e2 = (T.a.y - T.c.y) * (z - T.c.z) - (T.a.z - T.c.z) * (y - T.c.y);
				if (!((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))) continue;
				const double s = e0 + e1 + e2;
				if (std::fabs(s) < 1e-30) continue;
				                                                 
				xs.push_back((e1 * T.a.x + e2 * T.b.x + e0 * T.c.x) / s);
			}
			std::sort(xs.begin(), xs.end());
			size_t c = 0;
			for (int i = 0; i < g.nx; ++i) {
				const double x = g.lo.x + i * h;
				while (c < xs.size() && xs[c] < x) ++c;
				inside[g.Idx(i, j, kk)] = (uint8_t)(c & 1u);
			}
		});
	}

	                                                                                       
	const bool wantW = !S.pointWeight.empty() || (p.avoid && in.avoidPoly != nullptr);
	std::vector<float> Sd(N), W(wantW ? N : 0, 1.0f), Mimg(S.image ? N : 0, 0.0f);
	const double B2 = B * B;
	auto toRef = [&](const V& x) {
		V w = x;
		if (p.globalCoords && p.world.size() >= 12) {
			const double* M = p.world.data();
			w = V(M[0] * x.x + M[1] * x.y + M[2] * x.z + M[3], M[4] * x.x + M[5] * x.y + M[6] * x.z + M[7],
				M[8] * x.x + M[9] * x.y + M[10] * x.z + M[11]);
		}
		return w * (1.0 / k);
	};
	ProjectDamageParallelFor(0, (int64_t)g.nz * g.ny, [&](int64_t row) {
		const int j = (int)(row % g.ny), kk = (int)(row / g.ny);
		for (int i = 0; i < g.nx; ++i) {
			const size_t id = g.Idx(i, j, kk);
			const V x = g.Pos(i, j, kk);
			double best2 = B2, bu = 0, bv = 0, bw = 0;
			const int32_t t = bvh.Closest(x, best2, bu, bv, bw, (const AcceptAll*)nullptr);
			const double dist = t >= 0 ? std::sqrt(best2) : B;
			Sd[id] = (float)(inside[id] ? dist : -dist);
			if (t < 0) continue;
			const PieceTri& pt = ptris[(size_t)t];
			if (wantW) {
				double w = 1.0;
				if (!S.pointWeight.empty())
					w = bu * S.pointWeight[(size_t)pt.p[0]] + bv * S.pointWeight[(size_t)pt.p[1]] + bw * S.pointWeight[(size_t)pt.p[2]];
				if (p.avoid && in.avoidPoly && (size_t)pt.poly < in.avoidPoly->size() && (*in.avoidPoly)[(size_t)pt.poly]) w = 0.0;
				W[id] = (float)Clamp01d(w);
			}
			if (S.image) {
				double u, v;
				if (!S.boxUv) {
					const double* uv = p.maskUvs.data() + (size_t)pt.poly * 8;
					u = bu * uv[pt.corner[0] * 2] + bv * uv[pt.corner[1] * 2] + bw * uv[pt.corner[2] * 2];
					v = bu * uv[pt.corner[0] * 2 + 1] + bv * uv[pt.corner[1] * 2 + 1] + bw * uv[pt.corner[2] * 2 + 1];
				} else {
					const Tri& T = tris[(size_t)t];
					const V nrm = Cross(T.b - T.a, T.c - T.a);
					const V q = toRef(T.a * bu + T.b * bv + T.c * bw) * 0.5 + V(0.5);
					const double ax = std::fabs(nrm.x), ay = std::fabs(nrm.y), az = std::fabs(nrm.z);
					if (ax >= ay && ax >= az) { u = q.y; v = q.z; } else if (ay >= az) { u = q.x; v = q.z; } else { u = q.x; v = q.y; }
				}
				const double c = SampleImage(p, u, v);
				Mimg[id] = (float)(p.maskInvert ? 1.0 - c : c);
			}
		}
	});
	std::vector<uint8_t>().swap(inside);

	                                                                                        
	                                                                       
	                                                                           
	                                                                          
	                                                               
	                                                    
	std::vector<float> M(N);
	for (size_t i = 0; i < N; ++i) M[i] = std::max(0.0f, Sd[i]);
	MaxFilter(g, M, (int)std::ceil(3.0 * sigma));
	std::vector<float> K(N, 0.0f), lev(N);
	ProjectDamageParallelFor(0, (int64_t)N, [&](int64_t ii) {
		const double allowed = std::max(1e-3, 0.6 * M[(size_t)ii] / h);
		lev[(size_t)ii] = (float)std::max(0.0, std::min(4.0, std::log2(sigma / allowed)));
	});
	for (int l = 0; l <= 4; ++l) {
		std::vector<float> b = Sd;
		Blur(g, b, sigma / std::pow(2.0, l));
		ProjectDamageParallelFor(0, (int64_t)N, [&](int64_t ii) {
			const double w = std::max(0.0, 1.0 - std::fabs((double)lev[(size_t)ii] - l));
			if (w > 0.0) K[(size_t)ii] += (float)(w * b[(size_t)ii]);
		});
	}
	std::vector<float>().swap(lev);
	if (S.image) Blur(g, Mimg, 1.5);

	                                                                                       
	NoiseSetup ns;
	ns.type = std::max(0, std::min(3, (int)p.noiseType));
	ns.scale = std::max(1e-4, p.noiseScale);
	ns.dodge = 0.25 * Clamp01d(p.noiseContrast / 100.0);
	ns.pos = V(p.posX, p.posY, p.posZ);
	ns.scl = V(p.sclX, p.sclY, p.sclZ);
	ns.rot = EulerXYZ(p.rotX, p.rotY, p.rotZ);
	{
		const uint64_t h0 = Mix64(((uint64_t)(uint32_t)mainSeed << 32) ^ (uint64_t)(uint32_t)p.seed ^ Mix64((uint64_t)pieceIndex + 0x7E05ull));
		ns.rnd = V(U01(h0), U01(Mix64(h0 + 1)), U01(Mix64(h0 + 2))) * 100.0;
	}
	const double imgPush = S.image ? 0.5 * (0.215 + adj) : 0.0;
	const double band = k * (0.25 * (0.6 + offset) + std::fabs(imgPush)) + (3.0 * sigmaPost + 3.0) * h;
	const double cap = Clamp01d(p.thicknessCap);
	ProjectDamageParallelFor(0, (int64_t)g.nz * g.ny, [&](int64_t row) {
		const int j = (int)(row % g.ny), kk = (int)(row / g.ny);
		for (int i = 0; i < g.nx; ++i) {
			const size_t id = g.Idx(i, j, kk);
			const double ks = K[id];
			double disp;
			if (std::fabs(ks) < band) {
				const double T = ErosionNoise(ns, toRef(g.Pos(i, j, kk)));
				disp = k * 0.25 * (T + offset - 0.5);
			} else disp = k * 0.25 * offset;                                                                   
			if (S.image) disp += k * imgPush * Mimg[id];
			K[id] = (float)(ks + disp);
		}
	});
	Blur(g, K, sigmaPost);
	ProjectDamageParallelFor(0, (int64_t)N, [&](int64_t ii) {
		const size_t id = (size_t)ii;
		double kv = K[id];
		const double s = Sd[id];
		if (cap > 0.0) kv = std::max(kv, s - cap * 2.0 * M[id]);
		if (wantW) { const double w = W[id]; kv = w * kv + (1.0 - w) * (s + margin); }
		K[id] = (float)std::min(kv, s + margin);
	});
	const double edgeWall = std::clamp(p.edgeWallDepth, 0.0, 25.0) * 0.01 * R;
	if (edgeWall > 0.0) {
		std::vector<float> wallNormals = Sd;
		Blur(g, wallNormals, std::max(1.0, std::min(edgeWall, 0.1*dmax)/h));
		auto sample = [&](const V& x) {
			const V f = (x - g.lo) * (1.0 / h);
			const int i = (int)std::floor(f.x), j = (int)std::floor(f.y), z = (int)std::floor(f.z);
			if (i < 0 || j < 0 || z < 0 || i >= g.nx-1 || j >= g.ny-1 || z >= g.nz-1) return -B;
			const double tx = f.x-i, ty = f.y-j, tz = f.z-z;
			double value = 0.0;
			for (int c=0;c<2;++c) for (int b=0;b<2;++b) for (int a=0;a<2;++a)
				value += K[g.Idx(i+a,j+b,z+c)] * (a?tx:1-tx) * (b?ty:1-ty) * (c?tz:1-tz);
			return value;
		};
		std::vector<float> edged = K;
		ProjectDamageParallelFor(1, (int64_t)g.nz-1, [&](int64_t zz) {
			const int z = (int)zz;
			for (int j=1;j<g.ny-1;++j) for (int i=1;i<g.nx-1;++i) {
				const size_t id = g.Idx(i,j,z);
				const double s = Sd[id];
				const double depth = std::min(edgeWall, 0.35 * (double)M[id]);
				if (depth <= 1e-12 || s < -2*h || s > 4*depth) continue;
				if (wantW && W[id] <= 0.0f) continue;
				V n(wallNormals[g.Idx(i+1,j,z)]-wallNormals[g.Idx(i-1,j,z)],
				    wallNormals[g.Idx(i,j+1,z)]-wallNormals[g.Idx(i,j-1,z)],
				    wallNormals[g.Idx(i,j,z+1)]-wallNormals[g.Idx(i,j,z-1)]);
				const double len = std::sqrt(Dot(n,n));
				if (len < 1e-12) continue;
				const double fade = 1.0-SmoothStep01((s-depth)/(3*depth));
				const double medial = SmoothStep01((len/(2*h)-0.05)/0.2);
				const double shift = std::min(s,depth)*fade*medial;
				double kv = sample(g.Pos(i,j,z)-n*(shift/len));
				if (cap > 0.0) kv = std::max(kv,s-cap*2.0*M[id]);
				edged[id] = (float)std::min(kv,s+margin);
			}
		});
		K.swap(edged);
	}
	std::vector<float>().swap(M); std::vector<float>().swap(W); std::vector<float>().swap(Mimg);

	                                                                                         
	const Grid gg = g;
	const std::vector<float>& KF = K;
	const double outside = -B;
	auto field = [&gg, &KF, outside](V x) -> double {
		const V f = (x - gg.lo) * (1.0 / gg.h);
		const int i = (int)std::floor(f.x), j = (int)std::floor(f.y), kk = (int)std::floor(f.z);
		if (i < 0 || j < 0 || kk < 0 || i >= gg.nx - 1 || j >= gg.ny - 1 || kk >= gg.nz - 1) return outside;
		const double tx = f.x - i, ty = f.y - j, tz = f.z - kk;
		auto at = [&](int a, int b, int c) { return (double)KF[gg.Idx(i + a, j + b, kk + c)]; };
		const double c00 = at(0, 0, 0) + tx * (at(1, 0, 0) - at(0, 0, 0));
		const double c10 = at(0, 1, 0) + tx * (at(1, 1, 0) - at(0, 1, 0));
		const double c01 = at(0, 0, 1) + tx * (at(1, 0, 1) - at(0, 0, 1));
		const double c11 = at(0, 1, 1) + tx * (at(1, 1, 1) - at(0, 1, 1));
		const double c0 = c00 + ty * (c10 - c00), c1 = c01 + ty * (c11 - c01);
		return c0 + tz * (c1 - c0);
	};
	const V glo = g.lo + V(h), ghi = g.lo + V((g.nx - 2) * h, (g.ny - 2) * h, (g.nz - 2) * h);
	eroded = m::Manifold::LevelSet(field, m::Box(glo, ghi), h, 0.0, -1.0, true);
	return eroded.Status() == m::Manifold::Error::NoError;
}

}             

void PlaceVoxelErosion(const CoreInput& in, const VoxelErosionParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::string* log)
{
	if (!p.enable || in.view.pts == nullptr || in.view.polys == nullptr || in.view.npolys <= 0) return;
	const int32_t np = in.view.npts, nf = in.view.npolys;

	SharedInputs S; S.in = &in; S.p = &p;
	                                                       
	const bool zones = p.zoneWeights.size() == (size_t)np;
	const bool fieldMask = mask != nullptr && mask->Active();
	if (zones || fieldMask) {
		S.pointWeight.assign((size_t)np, 1.0);
		for (int32_t i = 0; i < np; ++i) {
			double w = zones ? Clamp01d(p.zoneWeights[(size_t)i]) : 1.0;
			if (fieldMask) w *= Clamp01d(mask->Weight(in.view.pts[i]));
			S.pointWeight[(size_t)i] = w;
		}
	}
	S.image = p.maskWidth > 0 && p.maskHeight > 0 && p.maskPixels.size() >= (size_t)p.maskWidth * (size_t)p.maskHeight;
	S.boxUv = p.maskBox || p.maskUvs.size() < (size_t)nf * 8;

	                                        
	std::vector<int32_t> parent((size_t)np);
	std::iota(parent.begin(), parent.end(), 0);
	auto find = [&](int32_t x) { while (parent[(size_t)x] != x) { parent[(size_t)x] = parent[(size_t)parent[(size_t)x]]; x = parent[(size_t)x]; } return x; };
	auto unite = [&](int32_t a, int32_t b) { a = find(a); b = find(b); if (a != b) parent[(size_t)std::max(a, b)] = std::min(a, b); };
	for (int32_t f = 0; f < nf; ++f) {
		const ProjectDamagePoly& q = in.view.polys[f];
		unite(q.a, q.b); unite(q.b, q.c); unite(q.c, q.d);
	}
	std::vector<int32_t> pieceOf((size_t)np, -1);
	std::vector<std::vector<PieceTri>> pieces;
	for (int32_t f = 0; f < nf; ++f) {
		const ProjectDamagePoly& q = in.view.polys[f];
		const int32_t r = find(q.a);
		if (pieceOf[(size_t)r] < 0) { pieceOf[(size_t)r] = (int32_t)pieces.size(); pieces.emplace_back(); }
		auto& tl = pieces[(size_t)pieceOf[(size_t)r]];
		tl.push_back(PieceTri{ f, { q.a, q.b, q.c }, { 0, 1, 2 } });
		if (q.c != q.d) tl.push_back(PieceTri{ f, { q.a, q.c, q.d }, { 0, 2, 3 } });
	}

	std::vector<m::Manifold> kept(pieces.size());
	std::vector<uint8_t> ok(pieces.size(), 0), coarse(pieces.size(), 0);
	for (size_t i = 0; i < pieces.size(); ++i) {                                   
		bool c = false;
		ok[i] = ErodePiece(S, pieces[i], (int)i, mainSeed, kept[i], c) ? 1 : 0;
		coarse[i] = c ? 1 : 0;
	}
	std::vector<m::Manifold> parts;
	for (size_t i = 0; i < kept.size(); ++i) if (ok[i] && kept[i].NumTri() > 0) parts.push_back(std::move(kept[i]));

	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < np; ++i) {
		const V& q = in.view.pts[i];
		lo = V(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
		hi = V(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
	}
	const V ext = hi - lo;
	const double pad = 0.1 * std::max({ ext.x, ext.y, ext.z, 1e-3 });
	const m::Manifold box = m::Manifold::Cube(ext + V(2.0 * pad)).Translate(lo - V(pad));
	const m::Manifold keep = parts.empty() ? m::Manifold() : m::Manifold::BatchBoolean(parts, m::OpType::Add);
	m::Manifold cutter = box - keep;
	if (cutter.Status() == m::Manifold::Error::NoError && cutter.NumTri() > 0) cutters.push_back(std::move(cutter));

	if (log && std::find(coarse.begin(), coarse.end(), (uint8_t)1) != coarse.end())
		*log = "[ProjectDamage] Voxel Erosion: the voxel budget made the grid coarser on at least one piece "
		       "(raise Max Voxels or Voxel Size for the intended detail)";
}

void TransferSurfaceUvs(const std::vector<EVec>& srcPts, const std::vector<int32_t>& srcTris,
	const std::vector<double>& srcUvs, const std::vector<EVec>& qPts,
	const std::vector<int32_t>& qFace, std::vector<double>& outUv)
{
	const size_t nt = srcTris.size() / 3;
	outUv.assign(qPts.size() * 2, 0.0);
	if (nt == 0 || srcUvs.size() < nt * 6 || qFace.size() != qPts.size()) return;
	std::vector<Tri> tris(nt);
	for (size_t t = 0; t < nt; ++t)
		tris[t] = Tri{ srcPts[(size_t)srcTris[t * 3]], srcPts[(size_t)srcTris[t * 3 + 1]], srcPts[(size_t)srcTris[t * 3 + 2]], (int32_t)t };
	Bvh bvh; bvh.Build(tris);

	                                                                       
	std::vector<int32_t> par(nt); std::iota(par.begin(), par.end(), 0);
	auto find = [&](int32_t x) { while (par[(size_t)x] != x) { par[(size_t)x] = par[(size_t)par[(size_t)x]]; x = par[(size_t)x]; } return x; };
	struct EdgeRec { int32_t t, c0, c1; };
	std::vector<std::pair<uint64_t, EdgeRec>> edges; edges.reserve(nt * 3);
	for (size_t t = 0; t < nt; ++t) for (int e = 0; e < 3; ++e) {
		const int32_t a = srcTris[t * 3 + (size_t)e], b = srcTris[t * 3 + (size_t)((e + 1) % 3)];
		const uint64_t key = ((uint64_t)(uint32_t)std::min(a, b) << 32) | (uint32_t)std::max(a, b);
		edges.push_back({ key, EdgeRec{ (int32_t)t, a < b ? e : (e + 1) % 3, a < b ? (e + 1) % 3 : e } });
	}
	std::sort(edges.begin(), edges.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
	auto uvAt = [&](int32_t t, int c) { return std::make_pair(srcUvs[(size_t)t * 6 + (size_t)c * 2], srcUvs[(size_t)t * 6 + (size_t)c * 2 + 1]); };
	auto same = [](std::pair<double, double> a, std::pair<double, double> b) { return std::fabs(a.first - b.first) < 1e-6 && std::fabs(a.second - b.second) < 1e-6; };
	for (size_t i = 0; i + 1 < edges.size(); ++i) {
		if (edges[i].first != edges[i + 1].first) continue;
		const EdgeRec& A = edges[i].second; const EdgeRec& Bk = edges[i + 1].second;
		if (same(uvAt(A.t, A.c0), uvAt(Bk.t, Bk.c0)) && same(uvAt(A.t, A.c1), uvAt(Bk.t, Bk.c1))) {
			const int32_t ra = find(A.t), rb = find(Bk.t);
			if (ra != rb) par[(size_t)std::max(ra, rb)] = std::min(ra, rb);
		}
	}
	std::vector<int32_t> island(nt);
	for (size_t t = 0; t < nt; ++t) island[t] = find((int32_t)t);

	                         
	int32_t nFaces = 0;
	for (int32_t f : qFace) nFaces = std::max(nFaces, f + 1);
	std::vector<V> cen((size_t)nFaces, V(0.0)); std::vector<int32_t> cnt((size_t)nFaces, 0);
	for (size_t i = 0; i < qPts.size(); ++i) if (qFace[i] >= 0) { cen[(size_t)qFace[i]] += qPts[i]; cnt[(size_t)qFace[i]]++; }
	std::vector<int32_t> faceIsland((size_t)nFaces, -1);
	ProjectDamageParallelFor(0, (int64_t)nFaces, [&](int64_t f) {
		if (cnt[(size_t)f] == 0) return;
		double b2 = 1e300, u, v, w;
		const int32_t t = bvh.Closest(cen[(size_t)f] / (double)cnt[(size_t)f], b2, u, v, w, (const AcceptAll*)nullptr);
		faceIsland[(size_t)f] = t >= 0 ? island[(size_t)t] : -1;
	});
	ProjectDamageParallelFor(0, (int64_t)qPts.size(), [&](int64_t qi) {
		const int32_t f = qFace[(size_t)qi];
		const int32_t isl = f >= 0 ? faceIsland[(size_t)f] : -1;
		auto accept = [&](int32_t t) { return isl < 0 || island[(size_t)t] == isl; };
		double b2 = 1e300, u = 1, v = 0, w = 0;
		int32_t t = bvh.Closest(qPts[(size_t)qi], b2, u, v, w, &accept);
		if (t < 0) { b2 = 1e300; t = bvh.Closest(qPts[(size_t)qi], b2, u, v, w, (const AcceptAll*)nullptr); }
		if (t < 0) return;
		const double* uv = srcUvs.data() + (size_t)t * 6;
		outUv[(size_t)qi * 2] = u * uv[0] + v * uv[2] + w * uv[4];
		outUv[(size_t)qi * 2 + 1] = u * uv[1] + v * uv[3] + w * uv[5];
	});
}

PROJECT_DAMAGE_NS_MACRO_END
