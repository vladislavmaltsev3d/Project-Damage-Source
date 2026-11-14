// раскладываем ткань и рассчитываем её провисание и столкновения
// ProjectDamage cloth sheets — GPL-3.0-or-later; see LICENSE.
//
// Recipe cloth-sheets-v1.
//   sheets    rectangular grids of particles (Cell Size apart), Scale +/- 30 %,
//             curtains longer than wide. Types:
//               Curtain   top edge pinned every Pin Spacing along the curve,
//                         the cloth gathered (shorter span than its width):
//                         scallops between the pins
//               Swag      pinned at its two top corners (and maybe the middle),
//                         a short span: deep swags
//               Corner    one top corner pinned, and one side point further
//                         along the curve: a diagonal hang
//               Draped    laid over the curve (a rope / beam of Curve Radius,
//                         also a collider): it folds over and hangs both sides
//               Crumpled  folded over itself once or twice, wrinkled, dropped
//                         from a little height: it settles as a heap
//             Mixed picks per sheet from what the mode allows.
//   clumps    sheets are grouped; a clump drops its sheets together from one
//             spot, stacked and folded tighter (Clump Tightness)
//   damage    ragged edges (noise erosion, deeper along the bottom edge),
//             holes with ragged rims; then the torn edge is cleaned: no lone
//             particles, one-cell slivers or spikes, and small islands go
//   solver    position-based dynamics: Verlet at 1/120 s, gravity x Gravity,
//             8 Gauss-Seidel passes over stretch (1.0), shear (0.8) and
//             bend (Stiffness) distance constraints; collisions with the
//             collider triangles (continuous against the plane, Thickness
//             offset, Friction), the Draped capsule, a fallback floor; layers
//             repel each other (no cloth through cloth); stops when settled
//   self      Self Collision: every particle against the nearby cloth
//             triangles (other sheets, far parts of its own): the side it was on
//             at the start of the step is kept, at Self Distance; hanging sheets
//             are laid along the curve without overlap, extra rows in front
//   wind      pressure along the cloth normal (sails, not a sideways drift),
//             turbulent in space and time, easing to a third by the end: the
//             irregular billows and folds freeze into the shape
//   wrinkles  fine noise along the normal (Noisiness)
// Output: quads / triangles, UV in metres of cloth, per point: sheet (random
// per sheet), edge (1 at a cut or torn edge, fading over 4 cells).
#include "cloth_sheets.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pd_cloth {
namespace {

constexpr double kPi = 3.14159265358979323846;
inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3& operator+=(V3& a, const V3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline V3& operator-=(V3& a, const V3& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Unit(const V3& a, const V3& fb) { const double l = Len(a); return l > 1e-12 ? a * (1.0 / l) : fb; }

struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL) {}
	double U() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0); }
	double R(double a, double b) { return a + (b - a) * U(); }
};
double H3(int64_t x, int64_t y, int64_t z, uint64_t seed)
{
	uint64_t h = seed ^ (uint64_t)x * 73856093ULL ^ (uint64_t)y * 19349663ULL ^ (uint64_t)z * 83492791ULL;
	h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
	return double(h >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
}
double Noise3(const V3& p, uint64_t seed)
{
	const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
	const double tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
	const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty), sz = tz * tz * (3 - 2 * tz);
	const int64_t ix = (int64_t)fx, iy = (int64_t)fy, iz = (int64_t)fz;
	auto L = [](double a, double b, double t) { return a + (b - a) * t; };
	return L(L(L(H3(ix, iy, iz, seed), H3(ix + 1, iy, iz, seed), sx), L(H3(ix, iy + 1, iz, seed), H3(ix + 1, iy + 1, iz, seed), sx), sy),
		L(L(H3(ix, iy, iz + 1, seed), H3(ix + 1, iy, iz + 1, seed), sx), L(H3(ix, iy + 1, iz + 1, seed), H3(ix + 1, iy + 1, iz + 1, seed), sx), sy), sz);
}
double Fbm(const V3& p, uint64_t seed, int oct = 3)
{
	double s = 0, a = 0.5, f = 1;
	for (int i = 0; i < oct; ++i) { s += a * Noise3(p * f, seed + (uint64_t)i * 1013); a *= 0.5; f *= 2.03; }
	return s;
}

                                                                             
struct Tri { V3 a, b, c, n; };
struct Grid
{
	std::vector<Tri> tris;
	double cell = 0.1;
	std::unordered_map<uint64_t, std::vector<int>> bins;
	static uint64_t Key(int64_t x, int64_t y, int64_t z)
	{
		return ((uint64_t)(x + (1 << 20)) << 42) ^ ((uint64_t)(y + (1 << 20)) << 21) ^ (uint64_t)(z + (1 << 20));
	}
	void Add(const V3& a, const V3& b, const V3& c)
	{
		const V3 n = Cross(b - a, c - a);
		if (Len(n) < 1e-14) return;
		tris.push_back({ a, b, c, Unit(n, V3(0, 0, 1)) });
	}
	void Build(double cs)
	{
		cell = std::max(1e-4, cs);
		bins.clear();
		for (size_t t = 0; t < tris.size(); ++t)
		{
			const Tri& r = tris[t];
			const int64_t x0 = (int64_t)std::floor(std::min({ r.a.x, r.b.x, r.c.x }) / cell), x1 = (int64_t)std::floor(std::max({ r.a.x, r.b.x, r.c.x }) / cell);
			const int64_t y0 = (int64_t)std::floor(std::min({ r.a.y, r.b.y, r.c.y }) / cell), y1 = (int64_t)std::floor(std::max({ r.a.y, r.b.y, r.c.y }) / cell);
			const int64_t z0 = (int64_t)std::floor(std::min({ r.a.z, r.b.z, r.c.z }) / cell), z1 = (int64_t)std::floor(std::max({ r.a.z, r.b.z, r.c.z }) / cell);
			if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 200000) continue;                                  
			for (int64_t x = x0; x <= x1; ++x) for (int64_t y = y0; y <= y1; ++y) for (int64_t z = z0; z <= z1; ++z)
				bins[Key(x, y, z)].push_back((int)t);
		}
		big.clear();
		for (size_t t = 0; t < tris.size(); ++t)
		{
			const Tri& r = tris[t];
			const double ext = std::max({ std::max({ r.a.x, r.b.x, r.c.x }) - std::min({ r.a.x, r.b.x, r.c.x }),
				std::max({ r.a.y, r.b.y, r.c.y }) - std::min({ r.a.y, r.b.y, r.c.y }), std::max({ r.a.z, r.b.z, r.c.z }) - std::min({ r.a.z, r.b.z, r.c.z }) });
			if (std::pow(ext / cell + 1, 3) > 200000) big.push_back((int)t);
		}
	}
	std::vector<int> big;
	template <class F> void Query(const V3& lo, const V3& hi, std::vector<int>& stamp, int& tick, F&& f) const
	{
		++tick;
		const int64_t x0 = (int64_t)std::floor(lo.x / cell), x1 = (int64_t)std::floor(hi.x / cell);
		const int64_t y0 = (int64_t)std::floor(lo.y / cell), y1 = (int64_t)std::floor(hi.y / cell);
		const int64_t z0 = (int64_t)std::floor(lo.z / cell), z1 = (int64_t)std::floor(hi.z / cell);
		for (int64_t x = x0; x <= x1; ++x) for (int64_t y = y0; y <= y1; ++y) for (int64_t z = z0; z <= z1; ++z)
		{
			auto it = bins.find(Key(x, y, z));
			if (it == bins.end()) continue;
			for (int t : it->second) if (stamp[(size_t)t] != tick) { stamp[(size_t)t] = tick; f(tris[(size_t)t]); }
		}
		for (int t : big) if (stamp[(size_t)t] != tick) { stamp[(size_t)t] = tick; f(tris[(size_t)t]); }
	}
};
                                                                         
bool InsideTri(const Tri& t, const V3& p, double tol)
{
	const V3 v0 = t.b - t.a, v1 = t.c - t.a, v2 = p - t.a;
	const double d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
	const double den = d00 * d11 - d01 * d01;
	if (std::abs(den) < 1e-20) return false;
	const double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1 - v - w;
	return u >= -tol && v >= -tol && w >= -tol;
}

                                                                         
                                                                           
bool SupportsPatch(const Tri& tri, const V3& projected, double radius)
{
    if (InsideTri(tri,projected,1e-8)) return true;
    if (radius <= 0.0) return false;
    auto nearSegment = [&](const V3& a, const V3& b) {
        const V3 edge=b-a;
        const double t=std::clamp(Dot(projected-a,edge)/std::max(1e-20,Dot(edge,edge)),0.0,1.0);
        const V3 distance=projected-(a+edge*t);
        return Dot(distance,distance)<=radius*radius;
    };
    return nearSegment(tri.a,tri.b) || nearSegment(tri.b,tri.c) || nearSegment(tri.c,tri.a);
}

                                                                              
                                                                              
bool SweepContact(const Grid& grid, std::vector<int>& stamp, int& tick, const V3& from,
                  V3& target, double margin, double patchRadius, V3& normal, const V3* layDirection = nullptr)
{
    V3 start = from;
    bool hit = false;
    const double eps = margin * 1e-6;
    const double reach = margin + patchRadius;
    for (int contact = 0; contact < 4; ++contact)
    {
        const V3 delta = target - start;
        const V3 lo(std::min(start.x,target.x)-reach, std::min(start.y,target.y)-reach, std::min(start.z,target.z)-reach);
        const V3 hi(std::max(start.x,target.x)+reach, std::max(start.y,target.y)+reach, std::max(start.z,target.z)+reach);
        double first = 2.0;
        V3 faceNormal, point;
        grid.Query(lo, hi, stamp, tick, [&](const Tri& t) {
            const double signedStart = Dot(start-t.a,t.n);
                                                                                  
            const double side = std::abs(signedStart) > eps ? (signedStart > 0 ? 1.0 : -1.0)
                                                            : (Dot(delta,t.n) > 0 ? -1.0 : 1.0);
            const V3 n = t.n * side;
            const double a = Dot(start-t.a,n), b = Dot(target-t.a,n);
            if (b >= margin-eps) return;
            const double time = a > margin ? (a-margin)/(a-b) : 0.0;
            if (time >= first) return;
            const V3 at = start + delta*time;
            const V3 surface = at - n*Dot(at-t.a,n);
            if (!SupportsPatch(t,surface,patchRadius)) return;
            first = time;
            faceNormal = n;
            point = surface + n*margin;
        });
        if (first > 1.0) return hit;
        hit = true;
        normal = faceNormal;
        const V3 rest = delta*(1.0-first);
        const double inward = std::min(0.0,Dot(rest,normal));
        V3 tangent = rest-normal*inward;
        if (layDirection && inward < 0.0)
        {
                                                                          
                                                                             
            const V3 along = *layDirection-normal*Dot(*layDirection,normal);
            if (Len(along)>1e-9) tangent += Unit(along,V3())*(-inward);
            if (Len(tangent)>1e-9) tangent=Unit(tangent,V3())*Len(rest);
        }
        start=point;
        target=point+tangent;
    }
    target=start;                                                 
    return hit;
}

                                                                            
                                                                         
void SupportFromAbove(const Grid& grid, std::vector<int>& stamp, int& tick,
                      const V3& up, double ceiling, double margin, double footprint, V3& p)
{
    const double span=ceiling-Dot(p,up)+margin;
    if (span<=0) return;
    const V3 end=p+up*span;
    const double pad=footprint+margin;
    const V3 lo(std::min(p.x,end.x)-pad,std::min(p.y,end.y)-pad,std::min(p.z,end.z)-pad);
    const V3 hi(std::max(p.x,end.x)+pad,std::max(p.y,end.y)+pad,std::max(p.z,end.z)+pad);
    double lift=0;
    grid.Query(lo,hi,stamp,tick,[&](const Tri& t){
        V3 n=t.n;
        if (Dot(n,up)<0) n=n*(-1.);
        const double cosine=Dot(n,up);
        if (cosine<0.25) return;
        const double rise=(margin-Dot(p-t.a,n))/cosine;
        if (rise<=lift || rise>span) return;
        if (SupportsPatch(t,p+up*rise-n*margin,footprint)) lift=rise;
    });
    p+=up*lift;
}

                                                                          
struct Curve
{
	std::vector<V3> p;
	std::vector<double> s;                  
	double L = 0;
	void Init(const std::vector<V3>& pts)
	{
		p = pts; s.assign(p.size(), 0.0);
		for (size_t i = 1; i < p.size(); ++i) s[i] = s[i - 1] + Len(p[i] - p[i - 1]);
		L = s.empty() ? 0.0 : s.back();
	}
	V3 At(double t, V3* tangent = nullptr) const
	{
		t = std::clamp(t, 0.0, L);
		size_t i = (size_t)(std::upper_bound(s.begin(), s.end(), t) - s.begin());
		i = std::clamp<size_t>(i, 1, p.size() - 1);
		const double seg = std::max(1e-12, s[i] - s[i - 1]), f = (t - s[i - 1]) / seg;
		if (tangent) *tangent = Unit(p[i] - p[i - 1], V3(1, 0, 0));
		return p[i - 1] + (p[i] - p[i - 1]) * f;
	}
};

struct Constraint { int a, b; double rest, k; };

}               

// основной вход: собираем геометрию по переданным настройкам
bool BuildClothSheets(const ClothInput& in, const ClothParams& prm, ClothResult& out)
{
	out = ClothResult();
	const V3 U = Unit(V3(prm.upX, prm.upY, prm.upZ), V3(0, 0, 1));
	Rng rng((uint64_t)prm.seed + 7);
	const uint64_t nseed = 0xC10755EEDULL ^ ((uint64_t)prm.seed * 0x9E3779B97F4A7C15ULL);
	std::vector<Curve> curves;
	for (const auto& c : in.curves) if (c.size() >= 2) { Curve cv; cv.Init(c); if (cv.L > 1e-6) curves.push_back(cv); }
	const bool haveCurve = !curves.empty();
	int mode = prm.mode;
	if ((mode == AlongCurve || mode == HangingFromCurve) && !haveCurve) mode = AroundPoints;
	if (mode == AroundPoints && in.points.empty()) { if (!haveCurve) return false; mode = AlongCurve; }
	const double h = std::max(0.005, prm.cellSize);
	const double thick = std::max(1e-4, prm.thickness);

	                                 
	Grid grid;
	for (size_t i = 0; i + 2 < in.colliderTris.size(); i += 3) grid.Add(in.colliderTris[i], in.colliderTris[i + 1], in.colliderTris[i + 2]);
	if (grid.tris.empty() && prm.groundFallback)
	{
		double lowest = 1e300; V3 centre(0, 0, 0); int n = 0;
		for (const V3& q : in.points) { lowest = std::min(lowest, Dot(q, U)); centre += q; ++n; }
		for (const Curve& c : curves) for (const V3& q : c.p) { lowest = std::min(lowest, Dot(q, U) - (mode == HangingFromCurve ? prm.scale * 1.1 : 0.0)); centre += q; ++n; }
		centre = centre * (1.0 / std::max(1, n));
		const V3 a = Unit(Cross(U, std::abs(U.z) < 0.9 ? V3(0, 0, 1) : V3(1, 0, 0)), V3(1, 0, 0)), b = Cross(U, a);
		const V3 o = centre + U * (lowest - Dot(centre, U));
		const double R = 1000.0;
		const V3 p0 = o - a * R - b * R, p1 = o + a * R - b * R, p2 = o + a * R + b * R, p3 = o - a * R + b * R;
		grid.Add(p0, p1, p2); grid.Add(p0, p2, p3);
	}
	grid.Build(std::max(h * 2.0, thick * 6.0));
	std::vector<int> stamp(grid.tris.size(), 0);
	int tick = 0;
    const bool ground = mode != HangingFromCurve && !grid.tris.empty() && !in.colliderTris.empty();
    const double patchRadius = !in.colliderTris.empty() ? h : 0.0;

	              
	struct Sheet { int nx, ny, type, first; std::vector<int> pid; float var; };
	std::vector<Sheet> sheets;
	std::vector<V3> pos, prev;
	std::vector<double> inv;                        
	std::vector<int> owner, gi, gj;
	std::vector<V3> pinAt;
	std::vector<Constraint> cons;
	std::vector<std::pair<V3, V3>> capsules;                                 
	                                                                      
	                                                                     
	struct Ball { V3 c; double r; };
	std::vector<Ball> balls;
	std::vector<int> clumpBall;
	const int nSheets = std::max(1, prm.amount);
	const int nClumps = std::max(0, std::min(prm.clumps, nSheets));
	                                            
	std::vector<V3> clumpAt;
	auto spawnPoint = [&]() -> V3 {
		if (mode == AroundPoints)
		{
			const V3 c = in.points[(size_t)(rng.U() * in.points.size()) % in.points.size()];
			const double a = rng.U() * 2 * kPi, r = prm.radius * std::sqrt(rng.U());
			const V3 ax = Unit(Cross(U, std::abs(U.z) < 0.9 ? V3(0, 0, 1) : V3(1, 0, 0)), V3(1, 0, 0)), ay = Cross(U, ax);
			return c + ax * (r * std::cos(a)) + ay * (r * std::sin(a));
		}
		const Curve& cv = curves[(size_t)(rng.U() * curves.size()) % curves.size()];
		V3 t;
		const V3 c = cv.At(rng.U() * cv.L, &t);
		const V3 side = Unit(Cross(U, t), V3(1, 0, 0));
		return c + side * ((rng.U() * 2 - 1) * prm.radius * 0.5);
	};
	const double ballR = prm.scale * (0.26 + 0.14 * (1.0 - std::clamp(prm.clumpTightness, 0.0, 1.0)));
	                                                                                   
	const bool mayCrumple = prm.type == Crumpled || (prm.type == Mixed && mode != HangingFromCurve);
	for (int k = 0; k < (mayCrumple ? nClumps : 0); ++k)
	{
		clumpAt.push_back(spawnPoint());
		balls.push_back({ clumpAt.back() + U * (ballR * 0.75), ballR * 1.1 });
		clumpBall.push_back((int)balls.size() - 1);
		                                                                              
		const double a = rng.U() * 2 * kPi;
		const V3 bx = Unit(Cross(U, std::abs(U.z) < 0.9 ? V3(0, 0, 1) : V3(1, 0, 0)), V3(1, 0, 0)), by = Cross(U, bx);
		balls.push_back({ clumpAt.back() + (bx * std::cos(a) + by * std::sin(a)) * (ballR * 0.9) + U * (ballR * 0.45), ballR * 0.7 });
	}
	std::vector<int> clumpHeight(clumpAt.size(), 0);

	                                                                           
	                                                                         
	std::vector<double> hangCursor(curves.size(), 0.0);
	std::vector<int> hangLayer(curves.size(), 0);
	const double selfD = std::max(thick * 2.0, prm.selfDistance);
	for (int si = 0; si < nSheets; ++si)
	{
		       
		int type = prm.type;
		if (type == Mixed)
		{
			const double r = rng.U();
			if (mode == HangingFromCurve) type = r < 0.45 ? Curtain : (r < 0.8 ? Swag : Corner);
			else if (mode == AlongCurve) type = r < 0.5 ? Draped : Crumpled;
			else type = Crumpled;
		}
		if ((type == Curtain || type == Swag || type == Corner) && mode != HangingFromCurve) type = (mode == AlongCurve ? Draped : Crumpled);
		if (type == Draped && !haveCurve) type = Crumpled;
		       
		const double W = prm.scale * rng.R(0.7, 1.3);
		const double Hh = (type == Curtain ? W * rng.R(1.2, 2.0) : W * rng.R(0.6, 1.2));
		Sheet sh;
		sh.type = type;
		sh.var = (float)rng.U();
		sh.nx = std::clamp((int)std::round(W / h) + 1, 4, 160);
		sh.ny = std::clamp((int)std::round(Hh / h) + 1, 4, 200);
		const int nx = sh.nx, ny = sh.ny;
		const bool hanging = (type == Curtain || type == Swag || type == Corner);
		                   
		std::vector<char> alive((size_t)nx * ny, 1);
		auto A = [&](int i, int j) -> char& { return alive[(size_t)j * nx + i]; };
		const uint64_t sseed = nseed + (uint64_t)si * 7919;
		if (prm.damaged)
		{
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					                                                                  
					const double dl = i, dr = nx - 1 - i, dt = j, db = ny - 1 - j;
					const double n = 0.5 + 0.5 * Fbm(V3(i * 0.22, j * 0.22, 0.5), sseed);
					const double depth = prm.tear * (1.0 + 7.0 * n * n);
					bool cut = false;
					if (db < depth * 1.6) cut = true;                                                           
					if (dl < depth * 0.8 || dr < depth * 0.8) cut = true;
					if (!hanging && dt < depth * 0.8) cut = true;
					if (hanging && dt < 2.5) cut = false;
					if (cut) A(i, j) = 0;
				}
			const int holes = (int)std::round(prm.holes * W * Hh * rng.R(0.5, 1.5));
			for (int k = 0; k < holes; ++k)
			{
				const double cx = rng.R(0.15, 0.85) * (nx - 1), cy = rng.R(hanging ? 0.2 : 0.1, 0.9) * (ny - 1);
				const double r = rng.R(0.03, 0.10) * prm.scale / h;
				for (int j = std::max(0, (int)(cy - 2 * r)); j <= std::min(ny - 1, (int)(cy + 2 * r)); ++j)
					for (int i = std::max(0, (int)(cx - 2 * r)); i <= std::min(nx - 1, (int)(cx + 2 * r)); ++i)
					{
						const double d = std::hypot(i - cx, j - cy);
						const double rr = r * (1.0 + 0.45 * Fbm(V3(i * 0.4, j * 0.4, 7.0 + k), sseed));
						if (d < rr && !(hanging && j < 3)) A(i, j) = 0;
					}
			}
		}
		                                                                            
		                                                                        
		if (prm.damaged)
			for (int pass = 0; pass < 4; ++pass)
			{
				bool changed = false;
				for (int j = 0; j < ny; ++j)
					for (int i = 0; i < nx; ++i)
					{
						if (!A(i, j)) continue;
						int n = 0;
						if (i > 0 && A(i - 1, j)) ++n;
						if (i + 1 < nx && A(i + 1, j)) ++n;
						if (j > 0 && A(i, j - 1)) ++n;
						if (j + 1 < ny && A(i, j + 1)) ++n;
						                                                                      
						bool inQuad = false;
						for (int di = -1; di <= 0 && !inQuad; ++di)
							for (int dj = -1; dj <= 0 && !inQuad; ++dj)
							{
								const int i0 = i + di, j0 = j + dj;
								if (i0 < 0 || j0 < 0 || i0 + 1 >= nx || j0 + 1 >= ny) continue;
								inQuad = A(i0, j0) && A(i0 + 1, j0) && A(i0, j0 + 1) && A(i0 + 1, j0 + 1);
							}
						if (n < 2 || !inQuad) { A(i, j) = 0; changed = true; }
					}
				if (!changed) break;
			}
		                                                                                 
		std::vector<V3> P((size_t)nx * ny);
		std::vector<char> pin((size_t)nx * ny, 0);
		std::vector<V3> pinPos((size_t)nx * ny);
		if (hanging)
		{
			const size_t ci = (size_t)(rng.U() * curves.size()) % curves.size();
			const Curve& cv = curves[ci];
			const double gather = type == Curtain ? rng.R(0.6, 0.85) : (type == Swag ? rng.R(0.45, 0.7) : rng.R(0.55, 0.8));
			const double span = std::min(cv.L, W * gather);
			double s0;
			int layer = 0;
			if (prm.selfCollision)
			{
				double gap = rng.R(0.0, 0.25) * span;
				if (hangCursor[ci] + gap + span > cv.L + 1e-9 && hangCursor[ci] > 0.0)
				{
					hangCursor[ci] = 0.0; ++hangLayer[ci];
					gap = rng.R(0.0, 0.5) * span;
				}
				s0 = std::min(hangCursor[ci] + gap, std::max(0.0, cv.L - span));
				hangCursor[ci] = s0 + span;
				layer = hangLayer[ci];
			}
			else
				s0 = rng.U() * std::max(0.0, cv.L - span);
			V3 t;
			cv.At(s0 + span * 0.5, &t);
			const V3 side = Unit(Cross(U, t), V3(0, 1, 0));
			                                                    
			const double layerOff = layer * (h * 3.5 * (0.3 + prm.noisiness) + 3.0 * selfD);
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const double f = (double)i / (nx - 1);
					const V3 top = cv.At(s0 + span * f) - U * (thick * 2.0);
					                                                                 
					const double wave = (std::sin(f * kPi * 5.0 + j * 0.05) + Fbm(V3(f * 4.0, j * h * 2.0, 0.0), sseed + 11)) * h * 1.2 * (0.3 + prm.noisiness);
					P[(size_t)j * nx + i] = top - U * (j * h) + side * (wave + layerOff);
				}
			if (!in.colliderTris.empty())
            {
                                                                                     
                                                                                     
                for (int i = 0; i < nx; ++i)
                {
                    const double f = double(i)/(nx-1);
                    V3 rest = cv.At(s0+span*f);
                    V3 safe = rest;
                    V3 contactNormal;
                    for (int j=0; j<ny; ++j)
                    {
                        V3& point=P[size_t(j)*nx+i];
                        V3 next=safe+(point-rest);
                        SweepContact(grid,stamp,tick,safe,next,thick,patchRadius,contactNormal,&side);
                        rest=point;
                        safe=next;
                        point=safe;
                    }
                }
            }
			auto pinIt = [&](int i, int j) { const size_t q = (size_t)j * nx + i; if (alive[q]) { pin[q] = 1; pinPos[q] = P[q]; } };
			if (type == Curtain)
			{
				const int every = std::max(1, (int)std::round(prm.pinSpacing / std::max(1e-9, span / (nx - 1))));
				for (int i = 0; i < nx; i += every) pinIt(i, 0);
				pinIt(nx - 1, 0);
			}
			else if (type == Swag)
			{
				pinIt(0, 0); pinIt(nx - 1, 0);
				if (rng.U() < 0.5) pinIt(nx / 2, 0);
			}
			else                                                                        
			{
				const int jr = (int)(rng.R(0.25, 0.6) * (ny - 1));
				const size_t q0 = 0, q1 = (size_t)jr * nx + (nx - 1);
				if (alive[q0]) { pin[q0] = 1; pinPos[q0] = P[q0]; }
				if (alive[q1]) { pin[q1] = 1; pinPos[q1] = cv.At(s0 + span) - U * (thick * 2.0) + side * layerOff; }
			}
		}
		else
		{
			                                                                             
			V3 centre, ax, ay;
			double lift;
			const Curve* dcv = nullptr;
			double dsc = 0.0;
			if (type == Draped)
			{
				const Curve& cv = curves[(size_t)(rng.U() * curves.size()) % curves.size()];
				V3 t;
				dcv = &cv;
				dsc = cv.L * rng.R(0.15, 0.85);
				centre = cv.At(dsc, &t);
				ax = Unit(t - U * Dot(t, U), V3(1, 0, 0));
				ay = Unit(Cross(U, ax), V3(0, 1, 0));
				lift = prm.curveRadius + thick * 3.0;
			}
			else
			{
				const int haveClumps = (int)clumpAt.size();
				const bool inClump = haveClumps > 0 && rng.U() < 0.85;
				const int ck = inClump ? (int)(rng.U() * haveClumps) % haveClumps : -1;
				Ball ball;
				if (inClump) ball = balls[(size_t)clumpBall[(size_t)ck]];
				else { const V3 at = spawnPoint(); ball = { at + U * (ballR * 0.6), ballR }; balls.push_back(ball); }
				centre = ball.c + U * (ball.r) + V3(rng.R(-1, 1), rng.R(-1, 1), 0.0) * (ball.r * 0.4);
				const double yaw = rng.U() * 2 * kPi;
				const V3 bx = Unit(Cross(U, std::abs(U.z) < 0.9 ? V3(0, 0, 1) : V3(1, 0, 0)), V3(1, 0, 0)), by = Cross(U, bx);
				ax = bx * std::cos(yaw) + by * std::sin(yaw);
				ay = Cross(U, ax);
				lift = thick * 4.0 + (ck >= 0 ? clumpHeight[(size_t)ck]++ * prm.scale * 0.12 : 0.0);
			}
			const V3 O = centre + U * lift - ax * (0.5 * (nx - 1) * h) - ay * (0.5 * (ny - 1) * h);
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
					P[(size_t)j * nx + i] = O + ax * (i * h) + ay * (j * h);
			if (type == Crumpled)
			{
				                                                                               
				const int folds = rng.U() < 0.5 ? 1 : 0;
				for (int f = 0; f < folds; ++f)
				{
					V3 c(0, 0, 0); int n = 0;
					for (const V3& q : P) { c += q; ++n; }
					c = c * (1.0 / n);
					const double a = rng.U() * kPi;
					const V3 d = ax * std::cos(a) + ay * std::sin(a);                                 
					const V3 nrm = Cross(U, d);                                                                
					const double off = rng.R(-0.2, 0.2) * W;
					for (V3& q : P)
					{
						const double s = Dot(q - c, nrm) - off;
						if (s > 0)
						{
							                                                                     
							const double rad = thick * 4.0 * (f + 1);
							const double ang = std::min(kPi, s / rad);
							const double rem = std::max(0.0, s - rad * kPi);
							const V3 base = q - nrm * s;
							q = base + nrm * (rad * std::sin(ang) - rem) + U * (rad * (1 - std::cos(ang)));
						}
					}
				}
				                                                             
				for (size_t q = 0; q < P.size(); ++q)
					P[q] += U * (Fbm(P[q] * (1.5 / prm.scale), sseed + 3) * prm.scale * 0.05 * (0.5 + prm.noisiness));
			}
			if (type == Draped)
			{
				                                                                     
				                                                          
				const int jr = (int)std::round(rng.R(0.3, 0.7) * (ny - 1));
				for (int i = 0; i < nx; ++i)
				{
					const size_t q = (size_t)jr * nx + i;
					P[q] = dcv->At(dsc + (i - 0.5 * (nx - 1)) * h) + U * (prm.curveRadius + thick);
					for (int j = 0; j < ny; ++j)
						P[(size_t)j * nx + i] = P[q] + ay * ((j - jr) * h);
					if (!ground && alive[q]) { pin[q] = 1; pinPos[q] = P[q]; }
				}
				                                                           
				if (!ground && capsules.empty())
					for (const Curve& c : curves)
						for (size_t k = 1; k < c.p.size(); ++k) capsules.push_back({ c.p[k - 1], c.p[k] });
			}
		}
                                                                         
                                                                                
        if (ground)
        {
            V3 n;
            for (V3& point : P)
                SweepContact(grid,stamp,tick,point+U*(prm.scale*0.5+h),point,thick,patchRadius,n);
        }
		                                                         
		{
			std::vector<int> comp((size_t)nx * ny, -1);
			std::vector<int> size;
			std::vector<char> hasPin;
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const size_t q = (size_t)j * nx + i;
					if (!alive[q] || comp[q] >= 0) continue;
					const int c = (int)size.size();
					size.push_back(0); hasPin.push_back(0);
					std::vector<size_t> st = { q };
					comp[q] = c;
					while (!st.empty())
					{
						const size_t u = st.back(); st.pop_back();
						++size[(size_t)c];
						if (pin[u]) hasPin[(size_t)c] = 1;
						const int ui = (int)(u % nx), uj = (int)(u / nx);
						const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
						for (int k = 0; k < 4; ++k)
						{
							const int vi = ui + di[k], vj = uj + dj[k];
							if (vi < 0 || vj < 0 || vi >= nx || vj >= ny) continue;
							const size_t v = (size_t)vj * nx + vi;
							if (alive[v] && comp[v] < 0) { comp[v] = c; st.push_back(v); }
						}
					}
				}
			int best = -1;
			for (size_t c = 0; c < size.size(); ++c) if (best < 0 || size[c] > size[(size_t)best]) best = (int)c;
			for (size_t q = 0; q < alive.size(); ++q)
				if (alive[q] && comp[q] != best && !(hasPin[(size_t)comp[q]] && size[(size_t)comp[q]] > 12)) alive[q] = 0;
			if (best < 0) continue;
		}
		            
		sh.first = (int)pos.size();
		sh.pid.assign((size_t)nx * ny, -1);
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				const size_t q = (size_t)j * nx + i;
				if (!alive[q]) continue;
				sh.pid[q] = (int)pos.size();
				pos.push_back(P[q]); prev.push_back(P[q]);
				inv.push_back(pin[q] ? 0.0 : 1.0);
				pinAt.push_back(pin[q] ? pinPos[q] : P[q]);
				owner.push_back((int)sheets.size()); gi.push_back(i); gj.push_back(j);
			}
		auto link = [&](int i0, int j0, int i1, int j1, double rest, double k) {
			if (i1 < 0 || j1 < 0 || i1 >= nx || j1 >= ny) return;
			const int a = sh.pid[(size_t)j0 * nx + i0], b = sh.pid[(size_t)j1 * nx + i1];
			if (a < 0 || b < 0) return;
			cons.push_back({ a, b, rest, k });
		};
		const double bend = std::clamp(prm.stiffness, 0.0, 1.0);
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				if (sh.pid[(size_t)j * nx + i] < 0) continue;
				link(i, j, i + 1, j, h, 1.0);
				link(i, j, i, j + 1, h, 1.0);
				link(i, j, i + 1, j + 1, h * std::sqrt(2.0), 0.8);
				link(i, j, i - 1, j + 1, h * std::sqrt(2.0), 0.8);
				if (bend > 0.0) { link(i, j, i + 2, j, 2 * h, bend * 0.6); link(i, j, i, j + 2, 2 * h, bend * 0.6); }
			}
		sheets.push_back(std::move(sh));
	}
	if (pos.empty()) return false;
	for (size_t q = 0; q < pos.size(); ++q) if (inv[q] == 0.0) { pos[q] = pinAt[q]; prev[q] = pinAt[q]; }

    std::vector<double> spawnTop(sheets.size(),-1e300);
    if (ground)
        for (size_t q=0; q<pos.size(); ++q)
            spawnTop[owner[q]]=std::max(spawnTop[owner[q]],Dot(pos[q],U)+h);
    auto supportGround = [&](size_t q) {
        SupportFromAbove(grid,stamp,tick,U,spawnTop[owner[q]],thick,h*1.5,pos[q]);
    };
	                                                           
	std::vector<std::array<int, 3>> ctris;
	if (prm.selfCollision)
		for (const Sheet& sh : sheets)
			for (int j = 0; j + 1 < sh.ny; ++j)
				for (int i = 0; i + 1 < sh.nx; ++i)
				{
					const int a = sh.pid[(size_t)j * sh.nx + i], b = sh.pid[(size_t)j * sh.nx + i + 1];
					const int c = sh.pid[(size_t)(j + 1) * sh.nx + i + 1], d = sh.pid[(size_t)(j + 1) * sh.nx + i];
					if (a >= 0 && b >= 0 && c >= 0) ctris.push_back({ a, b, c });
					if (a >= 0 && c >= 0 && d >= 0) ctris.push_back({ a, c, d });
					else if (a >= 0 && b >= 0 && d >= 0 && c < 0) ctris.push_back({ a, b, d });
				}
	std::unordered_map<uint64_t, std::vector<int>> thash;
	const double tcell = std::max(h * 1.5, selfD * 3.0);
	             
	const double dt = 1.0 / 120.0;
	const V3 G = U * (-9.81 * std::max(0.0, prm.gravity) * dt * dt);
	const double damp = 0.02;
	const double rmin = std::max(2.2 * thick, 0.25 * h);
	std::vector<V3> start(pos.size());
	std::unordered_map<uint64_t, std::vector<int>> phash;
	const double pcell = rmin * 1.01;
	int quiet = 0;
	const int nSteps = std::max(1, prm.steps) + (balls.empty() ? 0 : std::max(1, prm.steps) / 2);
	double ballScale = 1.0;
	                                                                            
	const V3 windDir = Unit(V3(prm.windX, prm.windY, prm.windZ), V3(1, 0, 0));
	const bool windy = prm.wind > 1e-6;
	std::vector<V3> pnrm(pos.size(), U);
	auto refreshNormals = [&]() {
		for (const Sheet& sh : sheets)
		{
			const int nx = sh.nx, ny = sh.ny;
			auto id = [&](int i, int j) { return (i < 0 || j < 0 || i >= nx || j >= ny) ? -1 : sh.pid[(size_t)j * nx + i]; };
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const int c = id(i, j);
					if (c < 0) continue;
					const int r = id(i + 1, j) >= 0 ? id(i + 1, j) : c, l = id(i - 1, j) >= 0 ? id(i - 1, j) : c;
					const int d = id(i, j + 1) >= 0 ? id(i, j + 1) : c, u = id(i, j - 1) >= 0 ? id(i, j - 1) : c;
					pnrm[(size_t)c] = Unit(Cross(pos[(size_t)r] - pos[(size_t)l], pos[(size_t)d] - pos[(size_t)u]), pnrm[(size_t)c]);
				}
		}
	};
	for (int step = 0; step < nSteps; ++step)
	{
		const double ph = (double)step / nSteps;
		ballScale = ph < 0.45 ? 1.0 : std::max(0.0, 1.0 - (ph - 0.45) / 0.3);
		if (windy && step % 4 == 0) refreshNormals();
		const double windNow = windy ? prm.wind * (1.0 - 0.67 * ph) : 0.0;
		const double tsec = step * dt;
		for (size_t q = 0; q < pos.size(); ++q)
		{
			start[q] = pos[q];
			if (inv[q] == 0.0) continue;
			const V3 v = (pos[q] - prev[q]) * (1.0 - damp);
			prev[q] = pos[q];
			V3 a = G;
			if (windNow > 0.0)
			{
				                                                         
				const V3 sp = pos[q] * (1.0 / std::max(0.2, prm.scale)) + V3(tsec * 0.7, tsec * 0.4, 0.0);
				const V3 gust(Noise3(sp, nseed + 71), Noise3(sp + V3(17.3, 0, 0), nseed + 72), Noise3(sp + V3(0, 31.7, 0), nseed + 73));
				const V3 w = windDir * (1.0 + prm.turbulence * Noise3(sp * 0.5, nseed + 74)) + gust * prm.turbulence;
				                                                                 
				const V3 n = pnrm[q];
				a += n * (Dot(w, n) * windNow * 9.81 * dt * dt);
			}
			pos[q] += v + a;
		}
		for (int it = 0; it < 8; ++it)
			for (const Constraint& c : cons)
			{
				const double wa = inv[(size_t)c.a], wb = inv[(size_t)c.b], w = wa + wb;
				if (w == 0.0) continue;
				const V3 d = pos[(size_t)c.b] - pos[(size_t)c.a];
				const double l = Len(d);
				if (l < 1e-12) continue;
				const V3 corr = d * (c.k * (l - c.rest) / (l * w));
				pos[(size_t)c.a] += corr * wa;
				pos[(size_t)c.b] -= corr * wb;
			}
		                                                                             
		for (size_t q = 0; q < pos.size(); ++q)
		{
			if (inv[q] == 0.0) continue;
			V3& p = pos[q];
			const V3 s = start[q];
            V3 hn;
            bool hit = SweepContact(grid,stamp,tick,s,p,thick,patchRadius,hn);
			for (const Ball& b : balls)
			{
				const double rr = b.r * ballScale + thick;
				if (rr <= thick) continue;
				const V3 d = p - b.c;
				const double l = Len(d);
				if (l < rr) { const V3 n = Unit(d, U); p = b.c + n * rr; hit = true; hn = n; }
			}
			for (const auto& cap : capsules)
			{
				const V3 ab = cap.second - cap.first;
				const double tt = std::clamp(Dot(p - cap.first, ab) / std::max(1e-12, Dot(ab, ab)), 0.0, 1.0);
				const V3 c = cap.first + ab * tt;
				const V3 d = p - c;
				const double l = Len(d), r = prm.curveRadius + thick;
				if (l < r) { const V3 n = Unit(d, U); p = c + n * r; hit = true; hn = n; }
			}
			if (hit)
			{
				                                                                      
				                                                         
				const V3 v = p - prev[q];
				const V3 vt = v - hn * Dot(v, hn);
				const double fr = std::clamp(prm.friction, 0.0, 1.0);
				if (Len(vt) < h * 0.08 * fr) { p = p - vt; prev[q] = p; }
				else prev[q] = p - vt * (1.0 - fr);
			}
		}
		                                                                           
		                             
		if (!capsules.empty())
			for (const Constraint& c : cons)
			{
				if (c.k < 1.0) continue;                                                      
				V3& pa = pos[(size_t)c.a];
				V3& pb = pos[(size_t)c.b];
				for (const auto& cap : capsules)
				{
					                                                       
					const V3 d1 = pb - pa, d2 = cap.second - cap.first, r = pa - cap.first;
					const double a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r), cc = Dot(d1, r), b = Dot(d1, d2);
					const double den = a * e - b * b;
					double sx = den > 1e-14 ? std::clamp((b * f - cc * e) / den, 0.0, 1.0) : 0.0;
					double tx = e > 1e-14 ? std::clamp((b * sx + f) / e, 0.0, 1.0) : 0.0;
					sx = a > 1e-14 ? std::clamp((b * tx - cc) / a, 0.0, 1.0) : 0.0;
					const V3 c1 = pa + d1 * sx, c2 = cap.first + d2 * tx;
					const V3 dd = c1 - c2;
					const double l = Len(dd), rr = prm.curveRadius + thick;
					if (l >= rr) continue;
					const V3 push = Unit(dd, U) * (rr - l);
					const double wa = inv[(size_t)c.a] * (1.0 - sx), wb = inv[(size_t)c.b] * sx, w = wa * wa + wb * wb;
					if (w < 1e-12) continue;
					pa += push * (wa / w);
					pb += push * (wb / w);
					                                                         
					const double g = std::clamp(prm.friction, 0.0, 1.0);
					prev[(size_t)c.a] = prev[(size_t)c.a] + (pa - prev[(size_t)c.a]) * g;
					prev[(size_t)c.b] = prev[(size_t)c.b] + (pb - prev[(size_t)c.b]) * g;
				}
			}
		                                                                            
		                                                                 
		if (prm.selfCollision && !ctris.empty())
		{
			thash.clear();
			auto cellOf = [&](double x) { return (int64_t)std::floor(x / tcell); };
			for (size_t t = 0; t < ctris.size(); ++t)
			{
				const V3& a = pos[(size_t)ctris[t][0]]; const V3& b = pos[(size_t)ctris[t][1]]; const V3& c = pos[(size_t)ctris[t][2]];
				const int64_t x0 = cellOf(std::min({ a.x, b.x, c.x }) - selfD), x1 = cellOf(std::max({ a.x, b.x, c.x }) + selfD);
				const int64_t y0 = cellOf(std::min({ a.y, b.y, c.y }) - selfD), y1 = cellOf(std::max({ a.y, b.y, c.y }) + selfD);
				const int64_t z0 = cellOf(std::min({ a.z, b.z, c.z }) - selfD), z1 = cellOf(std::max({ a.z, b.z, c.z }) + selfD);
				if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 64) continue;                            
				for (int64_t x = x0; x <= x1; ++x) for (int64_t y = y0; y <= y1; ++y) for (int64_t z = z0; z <= z1; ++z)
					thash[Grid::Key(x, y, z)].push_back((int)t);
			}
			for (size_t q = 0; q < pos.size(); ++q)
			{
				auto it = thash.find(Grid::Key(cellOf(pos[q].x), cellOf(pos[q].y), cellOf(pos[q].z)));
				if (it == thash.end()) continue;
				for (int t : it->second)
				{
					const auto& tr = ctris[(size_t)t];
					if (tr[0] == (int)q || tr[1] == (int)q || tr[2] == (int)q) continue;
					V3& pa = pos[(size_t)tr[0]]; V3& pb = pos[(size_t)tr[1]]; V3& pc = pos[(size_t)tr[2]];
					V3 n = Cross(pb - pa, pc - pa);
					const double ln = Len(n);
					if (ln < 1e-14) continue;
					n = n * (1.0 / ln);
					const double sNow = Dot(pos[q] - pa, n);
					if (std::abs(sNow) > selfD * 2.0) continue;
					                                    
					const V3 n0 = Unit(Cross(start[(size_t)tr[1]] - start[(size_t)tr[0]], start[(size_t)tr[2]] - start[(size_t)tr[0]]), n);
					const double sPrev = Dot(start[q] - start[(size_t)tr[0]], n0);
					const double side = (std::abs(sPrev) > 1e-9 ? sPrev : sNow) >= 0.0 ? 1.0 : -1.0;
					if (side * sNow >= selfD) continue;
					                                   
					const V3 pp = pos[q] - n * sNow;
					const V3 v0 = pb - pa, v1 = pc - pa, v2 = pp - pa;
					const double d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1), d20 = Dot(v2, v0), d21 = Dot(v2, v1);
					const double den = d00 * d11 - d01 * d01;
					if (std::abs(den) < 1e-20) continue;
					const double bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den, bu = 1.0 - bv - bw;
					if (bu < -0.02 || bv < -0.02 || bw < -0.02) continue;
					                                                            
					const double delta = side * selfD - sNow;
					const double wq = inv[q];
					const double wt = inv[(size_t)tr[0]] * bu * bu + inv[(size_t)tr[1]] * bv * bv + inv[(size_t)tr[2]] * bw * bw;
					const double wsum = wq + wt;
					                                                                   
					                                                    
					if (wsum < 0.05) continue;
					const double k = delta / wsum;
					                                                                     
					auto capped = [&](double m) { return std::clamp(m, -2.0 * selfD, 2.0 * selfD); };
					pos[q] += n * capped(k * wq);
					pa -= n * capped(k * inv[(size_t)tr[0]] * bu);
					pb -= n * capped(k * inv[(size_t)tr[1]] * bv);
					pc -= n * capped(k * inv[(size_t)tr[2]] * bw);
				}
			}
		}
		                                                                                    
		if (prm.selfCollision && (step & 1) == 0)
		{
			phash.clear();
			auto key = [&](const V3& p) {
				return Grid::Key((int64_t)std::floor(p.x / pcell), (int64_t)std::floor(p.y / pcell), (int64_t)std::floor(p.z / pcell));
			};
			for (size_t q = 0; q < pos.size(); ++q) phash[key(pos[q])].push_back((int)q);
			for (size_t q = 0; q < pos.size(); ++q)
			{
				const int64_t cx = (int64_t)std::floor(pos[q].x / pcell), cy = (int64_t)std::floor(pos[q].y / pcell), cz = (int64_t)std::floor(pos[q].z / pcell);
				for (int64_t x = cx - 1; x <= cx + 1; ++x) for (int64_t y = cy - 1; y <= cy + 1; ++y) for (int64_t z = cz - 1; z <= cz + 1; ++z)
				{
					auto it = phash.find(Grid::Key(x, y, z));
					if (it == phash.end()) continue;
					for (int o : it->second)
					{
						if ((size_t)o <= q) continue;
						if (owner[(size_t)o] == owner[q] && std::abs(gi[(size_t)o] - gi[q]) <= 2 && std::abs(gj[(size_t)o] - gj[q]) <= 2) continue;
						const V3 d = pos[(size_t)o] - pos[q];
						const double l = Len(d);
						if (l >= rmin || l < 1e-12) continue;
						const double w = inv[q] + inv[(size_t)o];
						if (w == 0.0) continue;
						const V3 corr = d * ((rmin - l) / (l * w));
						pos[q] -= corr * inv[q];
						pos[(size_t)o] += corr * inv[(size_t)o];
					}
				}
			}
		}
                                                                         
                                                                            
        if (!in.colliderTris.empty() && (prm.selfCollision || !capsules.empty() || !balls.empty()))
            for (size_t q=0; q<pos.size(); ++q)
            {
                V3 normal;
                if (inv[q] && SweepContact(grid,stamp,tick,start[q],pos[q],thick,patchRadius,normal))
                {
                    const double incoming=Dot(pos[q]-prev[q],normal);
                    if (incoming<0.0) prev[q]+=normal*incoming;
                }
            }
        if (ground)
            for (size_t q=0; q<pos.size(); ++q) supportGround(q);
		           
		double mx = 0.0;
		for (size_t q = 0; q < pos.size(); ++q) mx = std::max(mx, Len(pos[q] - start[q]));
		quiet = mx < h * 0.002 ? quiet + 1 : 0;
		if (quiet > 30 && ballScale == 0.0) break;
	}

                                                                                
    const bool guardedHem = !in.colliderTris.empty();
    const std::vector<V3> settled = guardedHem ? pos : std::vector<V3>();
	                                                                                
	if (prm.damaged)
		for (const Sheet& sh : sheets)
		{
			const int nx = sh.nx, ny = sh.ny;
			auto id = [&](int i, int j) { return (i < 0 || j < 0 || i >= nx || j >= ny) ? -1 : sh.pid[(size_t)j * nx + i]; };
			for (int pass = 0; pass < 2; ++pass)
			{
				std::vector<std::pair<int, V3>> moves;
				for (int j = 0; j < ny; ++j)
					for (int i = 0; i < nx; ++i)
					{
						const int c = id(i, j);
						if (c < 0 || inv[(size_t)c] == 0.0) continue;
						bool boundary = false;
						V3 sum(0, 0, 0); int n = 0;
						for (int dj = -1; dj <= 1; ++dj)
							for (int di = -1; di <= 1; ++di)
							{
								if (!di && !dj) continue;
								const int o = id(i + di, j + dj);
								if (o < 0) { boundary = true; continue; }
								sum += pos[(size_t)o]; ++n;
							}
						if (boundary && n >= 2) moves.push_back({ c, pos[(size_t)c] * 0.6 + sum * (0.4 / n) });
					}
				for (const auto& mv : moves) pos[(size_t)mv.first] = mv.second;
			}
		}
	                                                
	for (const Sheet& sh : sheets)
	{
		const int nx = sh.nx, ny = sh.ny;
		auto id = [&](int i, int j) { return (i < 0 || j < 0 || i >= nx || j >= ny) ? -1 : sh.pid[(size_t)j * nx + i]; };
		std::vector<V3> nrm(pos.size());
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				const int c = id(i, j);
				if (c < 0) continue;
				const int r = id(i + 1, j) >= 0 ? id(i + 1, j) : c, l = id(i - 1, j) >= 0 ? id(i - 1, j) : c;
				const int d = id(i, j + 1) >= 0 ? id(i, j + 1) : c, u = id(i, j - 1) >= 0 ? id(i, j - 1) : c;
				nrm[(size_t)c] = Unit(Cross(pos[(size_t)r] - pos[(size_t)l], pos[(size_t)d] - pos[(size_t)u]), U);
			}
		const uint64_t ws = nseed + 991 + (uint64_t)sh.first;
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				const int c = id(i, j);
				if (c < 0 || inv[(size_t)c] == 0.0) continue;
				const V3 rest(i * h, j * h, 0);
				const double wr = Fbm(rest * (1.0 / (h * 4.0)), ws) * 0.6 + Fbm(V3(rest.x * (1.0 / (h * 1.5)), rest.y * (1.0 / (h * 9.0)), 3.0), ws + 5) * 0.4;
				pos[(size_t)c] += nrm[(size_t)c] * (wr * prm.noisiness * h * 0.35);
			}
	}
    if (guardedHem)
        for (size_t q=0; q<pos.size(); ++q)
        {
            V3 normal;
            SweepContact(grid,stamp,tick,settled[q],pos[q],thick,patchRadius,normal);
        }
    if (ground)
        for (size_t q=0; q<pos.size(); ++q) supportGround(q);
	         
	std::vector<int> outId(pos.size(), -1);
	for (const Sheet& sh : sheets)
	{
		const int nx = sh.nx, ny = sh.ny;
		auto id = [&](int i, int j) { return (i < 0 || j < 0 || i >= nx || j >= ny) ? -1 : sh.pid[(size_t)j * nx + i]; };
		                                                   
		std::vector<int> dist((size_t)nx * ny, 99);
		std::vector<size_t> queue;
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				if (id(i, j) < 0) continue;
				if (id(i + 1, j) < 0 || id(i - 1, j) < 0 || id(i, j + 1) < 0 || id(i, j - 1) < 0) { dist[(size_t)j * nx + i] = 0; queue.push_back((size_t)j * nx + i); }
			}
		for (size_t qi = 0; qi < queue.size(); ++qi)
		{
			const size_t u = queue[qi];
			const int ui = (int)(u % nx), uj = (int)(u / nx);
			const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
			for (int k = 0; k < 4; ++k)
			{
				const int vi = ui + di[k], vj = uj + dj[k];
				if (id(vi, vj) < 0) continue;
				const size_t v = (size_t)vj * nx + vi;
				if (dist[v] > dist[u] + 1) { dist[v] = dist[u] + 1; queue.push_back(v); }
			}
		}
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				const int c = id(i, j);
				if (c < 0) continue;
				outId[(size_t)c] = (int)out.points.size();
				out.points.push_back(pos[(size_t)c]);
				out.uvs.push_back(i * h); out.uvs.push_back(-j * h);
				out.sheet.push_back(sh.var);
				out.edge.push_back((float)std::max(0.0, 1.0 - dist[(size_t)j * nx + i] / 4.0));
			}
		for (int j = 0; j + 1 < ny; ++j)
			for (int i = 0; i + 1 < nx; ++i)
			{
				const int a = id(i, j), b = id(i + 1, j), c = id(i + 1, j + 1), d = id(i, j + 1);
				const int n = (a >= 0) + (b >= 0) + (c >= 0) + (d >= 0);
				if (n == 4) out.polys.push_back({ outId[(size_t)a], outId[(size_t)d], outId[(size_t)c], outId[(size_t)b] });
				else if (n == 3)
				{
					                                                                              
					int v[3], k = 0;
					for (int x : { a, d, c, b }) if (x >= 0) v[k++] = outId[(size_t)x];
					out.polys.push_back({ v[0], v[1], v[2], v[2] });
				}
			}
		++out.sheets;
	}
	                                                         
	if (!guardedHem)
	{
		const double reach = prm.scale * 0.15;
		for (V3& p : out.points)
		{
			const V3 lo(p.x - reach, p.y - reach, p.z - reach), hi(p.x + reach, p.y + reach, p.z + reach);
			grid.Query(lo, hi, stamp, tick, [&](const Tri& t) {
				if (Dot(t.n, U) < -0.5) return;                                                                 
				const double d = Dot(p - t.a, t.n);
				if (d < thick && d > -reach)
				{
					const V3 proj = p - t.n * d;
					if (InsideTri(t, proj, 1e-6)) p = proj + t.n * thick;                                           
				}
			});
		}
	}
	return out.sheets > 0 && !out.polys.empty();
}

}                       
