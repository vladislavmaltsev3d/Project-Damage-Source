// раскалываем деревянный слой у края пробоя
// ProjectDamage break chips - GPL-3.0-or-later; see LICENSE.
//
// Wooden Chips fill of the Boolean (Breaker) damage: an impact on plywood.
//
//  1. Frame. The stack runs along the object's thinnest side (or the chosen
//     axis). z = 0 is the impact face and the plies go down to negative z.
//  2. Ply outlines. Each ply slices the cutters at its mid depth and widens
//     that outline by Crater, most at the impact face and not at all at the
//     last ply, so the plies step down like a funnel.
//  3. Tears. In the ply's grain frame the outline is scanned in fibre bundles
//     (strips as wide as a splinter). Every bundle crossing the hole tears on
//     past both ends of the crossing by a heavy-tailed length and ends in a
//     point. The union of the bundles is the ply's torn outline. Neighbouring
//     plies run across each other, so a tear can run on hidden under the next
//     ply, as a delamination does.
//  4. Carve. One prism per ply is returned for the caller to subtract.
//  5. Splinters. The fibres of a tear stay attached at its far end: a shard
//     rooted there runs back along the tear and bends out of the panel once
//     the plies above (or below) leave it room. Shards fork, and thin fibres
//     lie on the terrace floors.

#include "manifold/manifold.h"
#include "manifold/cross_section.h"

#include "break_chips.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t Mix(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

uint64_t Key(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0)
{
	return Mix(Mix(Mix(Mix(a) ^ b) ^ c) ^ d);
}

double U01(uint64_t key, uint64_t channel)
{
	return (double)(Mix(key ^ (channel * 0xD1B54A32D192ED03ull)) >> 11) * (1.0 / 9007199254740992.0);
}

double Range(uint64_t key, uint64_t channel, double lo, double hi)
{
	return lo + (hi - lo) * U01(key, channel);
}

double Noise1(double x, uint64_t key)
{
	const double fl = std::floor(x);
	const int64_t i = (int64_t)fl;
	const double f = x - fl;
	const double u = f * f * (3.0 - 2.0 * f);
	const double a = U01(key, (uint64_t)i) * 2.0 - 1.0;
	const double b = U01(key, (uint64_t)(i + 1)) * 2.0 - 1.0;
	return a + (b - a) * u;
}

double SmoothStep(double a, double b, double x)
{
	if (b <= a) return x >= b ? 1.0 : 0.0;
	const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// The crossings of the horizontal line y with even-odd polygons, in pairs.
void Crossings(const m::Polygons& polys, double y, std::vector<double>& xs)
{
	xs.clear();
	for (const m::SimplePolygon& poly : polys)
	{
		const size_t n = poly.size();
		for (size_t i = 0; i < n; i++)
		{
			const m::vec2& a = poly[i];
			const m::vec2& b = poly[(i + 1) % n];
			if ((a.y <= y) != (b.y <= y))
				xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
		}
	}
	std::sort(xs.begin(), xs.end());
	if (xs.size() % 2 != 0) xs.pop_back();
}

// A raster of a polygon set, so the shard march needs no polygon walk.
struct Raster
{
	double x0 = 0.0, y0 = 0.0, h = 1.0;
	int nx = 0, ny = 0;
	std::vector<uint8_t> v;

	void Build(const m::Polygons& polys, const m::vec2& lo, const m::vec2& hi, double cell)
	{
		x0 = lo.x;
		y0 = lo.y;
		h = cell;
		nx = std::max(1, (int)std::ceil((hi.x - lo.x) / h));
		ny = std::max(1, (int)std::ceil((hi.y - lo.y) / h));
		v.assign((size_t)nx * (size_t)ny, 0);
		std::vector<double> xs;
		for (int j = 0; j < ny; j++)
		{
			Crossings(polys, y0 + (j + 0.5) * h, xs);
			for (size_t k = 0; k + 1 < xs.size(); k += 2)
			{
				const int a = std::max(0, (int)std::ceil((xs[k] - x0) / h - 0.5));
				const int b = std::min(nx - 1, (int)std::floor((xs[k + 1] - x0) / h - 0.5));
				for (int i = a; i <= b; i++)
					v[(size_t)j * (size_t)nx + (size_t)i] = 1;
			}
		}
	}

	bool At(double x, double y) const
	{
		if (v.empty()) return false;
		const int i = (int)std::floor((x - x0) / h), j = (int)std::floor((y - y0) / h);
		if (i < 0 || j < 0 || i >= nx || j >= ny) return false;
		return v[(size_t)j * (size_t)nx + (size_t)i] != 0;
	}
};

struct Tear
{
	m::vec2 tip;       // the far end of the tear, panel xy
	m::vec2 dir;       // back along the tear, towards the hole
	double reach = 0;  // tear length
	double width = 0;  // fibre bundle width
	uint64_t key = 0;
};

struct Ply
{
	bool open = false;
	double theta = 0.0;
	double top = 0.0, bottom = 0.0;
	Raster voids;            // torn outline of this ply
	Raster solid;            // the object at this ply's depth
	std::vector<Tear> tears;
};

struct Stack
{
	std::vector<Ply> plies;
	double t = 1.0;
	bool passesBack = false;

	// Is this panel point free of wood? Above the face and below the stack it
	// is free when the object is; below the stack the object stays solid.
	bool Free(const m::vec3& q) const
	{
		if (q.z >= 0.0) return true;
		const int k = (int)std::floor(-q.z / t);
		if (k >= (int)plies.size()) return passesBack;
		const Ply& ply = plies[(size_t)k];
		return ply.voids.At(q.x, q.y) || !ply.solid.At(q.x, q.y);
	}
};

struct Shard
{
	m::vec3 root;
	m::vec2 dir;
	double length = 0.0;
	double halfW = 0.0, halfT = 0.0;
	double bendMax = 0.0;     // radians
	double bendSign = 1.0;    // +1 out of the impact face, -1 out of the back
	double bendStart = 0.15;  // share of the length that stays in the ply
	double checkFrom = 0.0;   // share of the length before the first room check
	double sharp = 0.8;
	double rough = 1.0;
	double wander = 0.0;
	int rings = 8;
	uint64_t key = 0;
};

// March the shard from its root, bending only where the wood leaves room, and
// skin it: a flat irregular hexagon that tapers to a single point.
void AddShard(const Shard& sh, const Stack& stack, const m::mat3x4& toWorld, BreakRebarMesh& out)
{
	if (sh.length <= 0.0 || sh.rings < 2) return;
	const int rings = sh.rings;
	const double ds = sh.length / (double)rings;
	const m::vec3 d0(sh.dir.x, sh.dir.y, 0.0);
	const m::vec3 side(-sh.dir.y, sh.dir.x, 0.0);
	std::vector<m::vec3> cen;
	std::vector<m::vec3> tan;
	cen.reserve((size_t)rings + 1);
	tan.reserve((size_t)rings + 1);
	m::vec3 q = sh.root;
	cen.push_back(q);
	const double yaw = Range(sh.key, 1, -1.0, 1.0) * 0.14;
	const double wanderPhase = Range(sh.key, 2, 0.0, 100.0);
	double bend = 0.0;
	const double bendRate = sh.bendMax * 5.0 / (double)rings;
	for (int i = 1; i <= rings; i++)
	{
		const double s = (double)i / (double)rings;
		const double target = sh.bendMax * SmoothStep(sh.bendStart, sh.bendStart + 0.3, s);
		bend = std::min(target, bend + bendRate);
		const double wob = yaw + sh.wander * Noise1(wanderPhase + s * 2.5, sh.key ^ 0x31);
		const m::vec3 plan = d0 * std::cos(wob) + side * std::sin(wob);
		m::vec3 step = (plan * std::cos(bend) + m::vec3(0.0, 0.0, sh.bendSign) * std::sin(bend)) * ds;
		m::vec3 next = q + step;
		// Lying in its own ply it fills its own tear, so that is always allowed;
		// it may leave the ply only where the wood above or below gives way.
		const m::vec3 ahead = next + plan * ds;
		const m::vec3 edge = side * sh.halfW;
		if (s > sh.checkFrom && bend > 0.0 && (!stack.Free(next) || !stack.Free(ahead) || !stack.Free(next + edge) || !stack.Free(next - edge)))
		{
			bend = 0.0;
			step = plan * ds;
			next = q + step;
		}
		tan.push_back(m::la::normalize(step));
		q = next;
		cen.push_back(q);
	}
	if (cen.size() < 3) return;
	tan.push_back(tan.back());

	const int nr = (int)cen.size() - 1;     // the last center is the apex
	static constexpr int kSides = 6;
	const Int32 vo = (Int32)out.points.size();
	const double total = ds * nr;
	const double power = 0.3 + 0.9 * sh.sharp;
	const double gu = Range(sh.key, 3, -50.0, 50.0), gv = Range(sh.key, 4, -18.0, 18.0), gk = Range(sh.key, 5, -18.0, 18.0);
	const double twist = Range(sh.key, 6, -0.6, 0.6);
	double jitter[kSides];
	for (int k = 0; k < kSides; k++) jitter[k] = Range(sh.key, 10 + (uint64_t)k, 0.78, 1.18);

	auto emit = [&](const m::vec3& local, double along, double lat, double up, double exposure)
	{
		out.points.push_back(EVec(toWorld * m::vec4(local, 1.0)));
		out.exposure.push_back((float)exposure);
		out.grain.push_back((float)(along + gu));
		out.grain.push_back((float)(lat + gv));
		out.grain.push_back((float)(up + gk));
	};

	for (int j = 0; j < nr; j++)
	{
		const double s = (double)j / (double)nr;
		const double taper = std::pow(1.0 - s, power);
		const double edge = 1.0 + sh.rough * 0.28 * Noise1(s * 7.0 + 3.0, sh.key ^ 0x77);
		const double hw = sh.halfW * taper * edge;
		const double ht = sh.halfT * std::pow(1.0 - s, 0.5 * power);
		const m::vec3 T = tan[(size_t)j];
		m::vec3 S = side - T * m::la::dot(side, T);
		const double sl = m::la::length(S);
		S = sl > 1e-9 ? S / sl : side;
		const m::vec3 U = m::la::cross(T, S);
		const double rot = twist * s;
		const m::vec3 S2 = S * std::cos(rot) + U * std::sin(rot);
		const m::vec3 U2 = U * std::cos(rot) - S * std::sin(rot);
		for (int k = 0; k < kSides; k++)
		{
			const double a = 2.0 * kPi * (double)k / (double)kSides;
			const double fibre = 1.0 + sh.rough * 0.16 * Noise1(s * 11.0 + (double)k * 7.3, sh.key ^ 0xA3);
			const double lat = std::cos(a) * hw * jitter[k] * fibre;
			const double up = std::sin(a) * ht * jitter[k] * fibre;
			emit(cen[(size_t)j] + S2 * lat + U2 * up, s * total, lat, up, s);
		}
	}
	const Int32 apex = (Int32)out.points.size();
	emit(cen.back(), total, 0.0, 0.0, 1.0);
	const Int32 base = (Int32)out.points.size();
	emit(cen.front(), 0.0, 0.0, 0.0, 0.0);

	auto idx = [&](int j, int k) { return vo + (Int32)(j * kSides + ((k + kSides) % kSides)); };
	const Int32 bar = out.bars++;
	auto poly = [&](Int32 a, Int32 b, Int32 c, Int32 d)
	{
		out.polys.push_back({ a, b, c, d });
		out.polyBar.push_back(bar);
	};
	for (int k = 0; k < kSides; k++)
	{
		poly(base, idx(0, k + 1), idx(0, k), idx(0, k));
		for (int j = 0; j + 1 < nr; j++)
			poly(idx(j, k), idx(j, k + 1), idx(j + 1, k + 1), idx(j + 1, k));
		poly(idx(nr - 1, k), idx(nr - 1, k + 1), apex, apex);
	}
}

}  // namespace

// формируем деревянные сколы вокруг вырезов
void BuildBreakChips(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, m::Manifold* carve)
{
	out = BreakRebarMesh();
	if (carve) *carve = m::Manifold();
	if (!p.enable || source.NumTri() == 0) return;

	const m::Box box = source.BoundingBox();
	const m::vec3 size = box.max - box.min;
	if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) return;

	std::vector<m::Manifold> live;
	m::vec3 cLo(std::numeric_limits<double>::max()), cHi(std::numeric_limits<double>::lowest());
	for (const m::Manifold& c : cutters)
	{
		if (c.NumTri() == 0) continue;
		const m::Box b = c.BoundingBox();
		if (b.max.x < box.min.x || b.max.y < box.min.y || b.max.z < box.min.z
			|| b.min.x > box.max.x || b.min.y > box.max.y || b.min.z > box.max.z) continue;
		live.push_back(c);
		cLo = m::la::min(cLo, b.min);
		cHi = m::la::max(cHi, b.max);
	}
	if (live.empty()) return;

	const uint64_t seed = Key((uint64_t)(uint32_t)mainSeed, (uint64_t)(uint32_t)p.seed, 0xC41B);
	const double sc = std::max(1e-3, p.scale);
	const double tearLength = std::max(0.02, p.chipLength * sc);
	double bundle = std::max(0.01, p.chipWidth * sc);
	const double sharp = std::clamp(p.chipSharp, 0.0, 1.0);
	const double lift = std::clamp(p.chipLift, 0.0, 1.5);
	const double amount = std::max(0.0, p.chipAmount);
	const double crater = std::max(0.0, p.chipCrater * sc);
	const double detail = std::clamp(p.detail, 0.05, 8.0);

	// Frame: x, y in the panel, +z out of the impact face.
	int ax = p.axis >= 1 && p.axis <= 3 ? p.axis - 1 : (size.x <= size.y && size.x <= size.z ? 0 : size.y <= size.z ? 1 : 2);
	const double thick = size[ax];
	double sign = 1.0;
	if (p.chipFace == 1) sign = 1.0;
	else if (p.chipFace == 2) sign = -1.0;
	else
	{
		// Auto: the face the cutters sit nearest; a cutter centred on the panel
		// counts as coming from the + face.
		const double cm = 0.5 * (std::max(cLo[ax], box.min[ax]) + std::min(cHi[ax], box.max[ax]));
		const double bm = 0.5 * (box.min[ax] + box.max[ax]);
		sign = cm < bm - 1e-3 * thick ? -1.0 : 1.0;
	}
	m::vec3 n(0.0), u(0.0);
	n[ax] = sign;
	u[(ax + 1) % 3] = 1.0;
	const m::vec3 v = m::la::cross(n, u);
	const double face = sign > 0.0 ? box.max[ax] : -box.min[ax];  // n . (a point of the face)
	const m::mat3x4 toLocal(m::vec3(u.x, v.x, n.x), m::vec3(u.y, v.y, n.y), m::vec3(u.z, v.z, n.z), m::vec3(0.0, 0.0, -face));
	const m::mat3x4 toWorld(u, v, n, n * face);

	const m::Manifold localSource = source.Transform(toLocal);
	const m::Box lb = localSource.BoundingBox();
	std::vector<m::Manifold> localCut;
	localCut.reserve(live.size());
	for (const m::Manifold& c : live) localCut.push_back(c.Transform(toLocal));

	// Stack: Ply Thickness 0 divides the object evenly; a stack thinner than the
	// object leaves the rest of it whole, a thicker one is cut down to fit.
	int count = std::clamp<int>(p.chipPlies, 1, 32);
	double t = thick / (double)count;
	if (p.chipPlyThickness > 0.0)
	{
		const double want = std::max(1e-3, p.chipPlyThickness * sc);
		if (want * count > thick) count = std::max(1, (int)std::floor(thick / want + 1e-6));
		t = std::min(want, thick);
	}
	Stack stack;
	stack.t = t;
	stack.plies.resize((size_t)count);
	const bool reachesBack = t * count >= thick * (1.0 - 1e-6);

	// Raster cell: a fibre bundle resolved in 3 cells, 4096 cells at most.
	const double reachAll = crater + 1.6 * tearLength + 2.0 * bundle;
	m::vec2 winLo(lb.max.x, lb.max.y), winHi(lb.min.x, lb.min.y);
	for (const m::Manifold& c : localCut)
	{
		const m::Box cb = c.BoundingBox();
		winLo = m::la::min(winLo, m::vec2(cb.min.x, cb.min.y));
		winHi = m::la::max(winHi, m::vec2(cb.max.x, cb.max.y));
	}
	winLo = m::la::max(winLo - m::vec2(reachAll), m::vec2(lb.min.x, lb.min.y) - m::vec2(bundle));
	winHi = m::la::min(winHi + m::vec2(reachAll), m::vec2(lb.max.x, lb.max.y) + m::vec2(bundle));
	if (winLo.x >= winHi.x || winLo.y >= winHi.y) return;
	const double extent = std::max(winHi.x - winLo.x, winHi.y - winLo.y);
	const double cell = std::max(bundle / 3.0, extent / 4096.0);
	// A hole far wider than the bundles: coarsen them rather than scan forever.
	bundle = std::max(bundle, extent / 6000.0);

	const m::SimplePolygon window = { m::vec2(winLo.x, winLo.y), m::vec2(winHi.x, winLo.y), m::vec2(winHi.x, winHi.y), m::vec2(winLo.x, winHi.y) };
	const m::CrossSection clip(m::Polygons{ window }, m::CrossSection::FillRule::Positive);

	const double grainBase = p.angle * kPi / 180.0;
	const double plyTurn = p.chipPlyAngle * kPi / 180.0;
	std::vector<m::Manifold> prisms;
	bool through = false;

	for (int i = 0; i < count; i++)
	{
		Ply& ply = stack.plies[(size_t)i];
		const uint64_t plyKey = Key(seed, 0x51A7, (uint64_t)i);
		ply.top = -(double)i * t;
		ply.bottom = -(double)(i + 1) * t;
		const double zMid = 0.5 * (ply.top + ply.bottom);
		ply.theta = grainBase + plyTurn * (double)i + Range(plyKey, 1, -0.05, 0.05);
		ply.solid.Build(localSource.Slice(zMid), winLo, winHi, cell);

		m::CrossSection hole;
		for (const m::Manifold& c : localCut)
		{
			const m::Box cb = c.BoundingBox();
			if (cb.min.z > zMid || cb.max.z < zMid) continue;
			hole = hole + m::CrossSection(c.Slice(zMid), m::CrossSection::FillRule::Positive);
		}
		hole = hole ^ clip;
		if (hole.IsEmpty()) continue;

		// the funnel: widest at the impact face, none at the last ply
		const double depth = count > 1 ? (double)i / (double)(count - 1) : 0.0;
		const double widen = crater * (1.0 - depth) * Range(plyKey, 2, 0.75, 1.2);
		const m::CrossSection wide = widen > 0.0
			? hole.Simplify(0.1 * bundle).Offset(widen, m::CrossSection::JoinType::Round, 2.0, 12) : hole.Simplify(0.1 * bundle);
		if (wide.IsEmpty()) continue;

		// scan in the grain frame: x along the fibres, y across
		const double ct = std::cos(ply.theta), st = std::sin(ply.theta);
		auto toGrain = [&](const m::vec2& q) { return m::vec2(ct * q.x + st * q.y, -st * q.x + ct * q.y); };
		auto toPanel = [&](const m::vec2& q) { return m::vec2(ct * q.x - st * q.y, st * q.x + ct * q.y); };
		m::Polygons grainPolys = wide.ToPolygons();
		double yMin = std::numeric_limits<double>::max(), yMax = std::numeric_limits<double>::lowest();
		for (m::SimplePolygon& poly : grainPolys)
			for (m::vec2& q : poly)
			{
				q = toGrain(q);
				yMin = std::min(yMin, q.y);
				yMax = std::max(yMax, q.y);
			}

		m::Polygons torn;
		std::vector<double> xs;
		const double plyLength = tearLength * (1.0 - 0.3 * depth);
		int strip = 0;
		double y = yMin;
		while (y < yMax)
		{
			const uint64_t sk = Key(plyKey, 0x7E41, (uint64_t)strip++);
			const double w = bundle * (U01(sk, 7) < 0.15 ? Range(sk, 1, 1.4, 2.4) : Range(sk, 1, 0.35, 1.3));
			const double yc = y + 0.5 * w;
			y += w;
			Crossings(grainPolys, yc, xs);
			for (size_t k = 0; k + 1 < xs.size(); k += 2)
			{
				const uint64_t ik = Key(sk, (uint64_t)k);
				const double a = xs[k], b = xs[k + 1];
				if (b - a < 1e-6) continue;
				// fibres tear in groups: slow noise across the grain, heavy tail along it
				const double group = 0.5 + 0.5 * Noise1(yc / (2.5 * bundle), plyKey ^ 0xB0);
				const double eL = plyLength * (0.06 + 0.94 * std::pow(U01(ik, 1), 2.2)) * (0.45 + 1.1 * group);
				const double eR = plyLength * (0.06 + 0.94 * std::pow(U01(ik, 2), 2.2)) * (0.45 + 1.1 * group);
				const double half = 0.52 * w;
				const double tipHalf = (1.0 - sharp) * 0.7 * half;
				const double midL = half * (sharp > 0.5 ? Range(ik, 3, 0.3, 0.6) : Range(ik, 3, 0.6, 0.95));
				const double midR = half * (sharp > 0.5 ? Range(ik, 4, 0.3, 0.6) : Range(ik, 4, 0.6, 0.95));
				const double jA = Range(ik, 5, -0.35, 0.35) * half, jB = Range(ik, 6, -0.35, 0.35) * half;
				const m::vec2 g[12] = {
					{ a - eL, yc + jA - tipHalf }, { a - 0.5 * eL, yc - midL }, { a, yc - half }, { b, yc - half },
					{ b + 0.5 * eR, yc - midR }, { b + eR, yc + jB - tipHalf }, { b + eR, yc + jB + tipHalf },
					{ b + 0.5 * eR, yc + midR }, { b, yc + half }, { a, yc + half }, { a - 0.5 * eL, yc + midL },
					{ a - eL, yc + jA + tipHalf } };
				m::SimplePolygon shape;
				shape.reserve(12);
				for (const m::vec2& q : g) shape.push_back(toPanel(q));
				torn.push_back(std::move(shape));

				Tear te;
				te.width = w;
				te.key = Key(ik, 0x70);
				te.tip = toPanel(m::vec2(a - eL, yc + jA));
				te.dir = toPanel(m::vec2(1.0, 0.0));
				te.reach = eL;
				ply.tears.push_back(te);
				te.key = Key(ik, 0x71);
				te.tip = toPanel(m::vec2(b + eR, yc + jB));
				te.dir = toPanel(m::vec2(-1.0, 0.0));
				te.reach = eR;
				ply.tears.push_back(te);
			}
		}
		if (torn.empty()) continue;
		const m::CrossSection outline = m::CrossSection(torn, m::CrossSection::FillRule::Positive).Simplify(0.01 * bundle);
		if (outline.IsEmpty()) continue;
		const m::Polygons outlinePolys = outline.ToPolygons();
		ply.voids.Build(outlinePolys, winLo, winHi, cell);
		ply.open = true;
		if (i == count - 1 && reachesBack) through = true;

		if (carve)
		{
			const bool last = i == count - 1 && reachesBack;
			const double zt = i == 0 ? std::max(1.0, t) : ply.top + 0.002 * t;
			const double zb = last ? ply.bottom - std::max(1.0, t) : ply.bottom;
			m::Manifold prism = m::Manifold::Extrude(outlinePolys, zt - zb).Translate(m::vec3(0.0, 0.0, zb));
			if (prism.Status() == m::Manifold::Error::NoError && !prism.IsEmpty())
				prisms.push_back(prism.Transform(toWorld));
		}
	}
	stack.passesBack = through;
	if (carve && !prisms.empty())
		*carve = m::Manifold::BatchBoolean(prisms, m::OpType::Add);

	// Splinters
	out.grain.clear();
	const double stepOut = std::max(cell, 0.1 * bundle);
	const int ringsBase = std::max(3, (int)std::round(9.0 * detail));
	for (int i = 0; i < count; i++)
	{
		const Ply& ply = stack.plies[(size_t)i];
		if (!ply.open) continue;
		const double zMid = 0.5 * (ply.top + ply.bottom);
		for (const Tear& te : ply.tears)
		{
			if (te.reach < 0.04 * tearLength) continue;
			const double chance = amount * 0.9;
			int spawns = (int)std::floor(chance);
			if (U01(te.key, 20) < chance - (double)spawns) spawns++;
			for (int sp = 0; sp < spawns; sp++)
			{
				const uint64_t sk = Key(te.key, 0x5A, (uint64_t)sp);
				// the root: walk past the end of the tear into wood that holds it
				m::vec2 root = te.tip;
				double walked = 0.0;
				const double walkMax = 0.75 * tearLength + 4.0 * bundle;
				while (walked < walkMax && ply.voids.At(root.x, root.y))
				{
					root -= te.dir * stepOut;
					walked += stepOut;
				}
				if (ply.voids.At(root.x, root.y) || !ply.solid.At(root.x, root.y)) continue;
				const double embed = std::max(2.0 * stepOut, 0.08 * tearLength);
				root -= te.dir * embed;
				if (ply.voids.At(root.x, root.y) || !ply.solid.At(root.x, root.y))
					root += te.dir * embed;

				// plies near the impact face bend out of it, the far ones are
				// pushed out of the back when the hole goes through
				bool up = (double)i < 0.6 * (double)count;
				if (U01(sk, 1) < 0.12) up = !up;
				if (!through) up = true;

				Shard sh;
				sh.key = sk;
				sh.root = m::vec3(root.x, root.y, zMid + Range(sk, 2, -0.15, 0.15) * t);
				sh.dir = te.dir;
				// short tears still leave a chip standing at the rim
				sh.length = std::max(te.reach + walked + embed, tearLength * Range(sk, 14, 0.2, 0.5)) * Range(sk, 3, 0.75, 1.3);
				sh.length = std::min(sh.length, 1.6 * tearLength);
				// mostly slivers, now and then a broad strip of veneer
				const double broad = U01(sk, 15) < 0.14 ? 2.0 : 1.0;
				sh.halfW = std::max(0.04 * bundle, broad * te.width * Range(sk, 4, 0.3, 0.52));
				sh.halfT = t * Range(sk, 5, 0.18, 0.46);
				sh.bendMax = lift * Range(sk, 6, 0.3, 1.15) * 75.0 * kPi / 180.0;
				sh.bendSign = up ? 1.0 : -1.0;
				sh.bendStart = std::clamp((walked + embed) / std::max(1e-6, sh.length), 0.05, 0.6);
				sh.checkFrom = sh.bendStart * 0.6;
				sh.sharp = sharp;
				sh.rough = 1.0;
				sh.wander = 0.06;
				sh.rings = std::clamp((int)std::round(ringsBase * std::sqrt(sh.length / tearLength)), 3, 48);
				AddShard(sh, stack, toWorld, out);

				// splintered: a thinner fibre peels off the same root
				if (U01(sk, 7) < 0.25 + 0.35 * sharp)
				{
					Shard fork = sh;
					fork.key = Key(sk, 0xF0);
					const double spread = Range(sk, 8, -0.22, 0.22);
					const double cs = std::cos(spread), sn = std::sin(spread);
					fork.dir = m::vec2(te.dir.x * cs - te.dir.y * sn, te.dir.x * sn + te.dir.y * cs);
					fork.root += m::vec3(-te.dir.y, te.dir.x, 0.0) * (Range(sk, 9, -0.6, 0.6) * te.width);
					fork.length *= Range(sk, 10, 0.5, 0.95);
					fork.halfW *= Range(sk, 11, 0.3, 0.6);
					fork.halfT *= Range(sk, 12, 0.4, 0.8);
					fork.bendMax = lift * Range(sk, 13, 0.1, 1.3) * 75.0 * kPi / 180.0;
					fork.rings = std::max(3, (int)std::round(0.75 * sh.rings));
					AddShard(fork, stack, toWorld, out);
				}
			}
		}

		// Thin fibres on the terrace floor this ply leaves for the one below,
		// so the steps read as torn veneer rather than as machined shelves.
		if (i + 1 >= count || amount <= 0.0) continue;
		const Ply& below = stack.plies[(size_t)i + 1];
		const uint64_t fk = Key(seed, 0xF1B3, (uint64_t)i);
		const double floorZ = ply.bottom;
		int columns = (int)std::ceil((winHi.x - winLo.x) / (2.0 * bundle));
		int rows = (int)std::ceil((winHi.y - winLo.y) / (2.0 * bundle));
		columns = std::clamp(columns, 1, 2048);
		rows = std::clamp(rows, 1, 2048);
		const double fibreChance = std::min(1.0, 0.45 * amount);
		const double ct = std::cos(below.theta), st = std::sin(below.theta);
		const m::vec2 grain(ct, st);
		for (int r = 0; r < rows; r++)
			for (int c = 0; c < columns; c++)
			{
				const uint64_t ck = Key(fk, (uint64_t)r, (uint64_t)c);
				if (U01(ck, 1) > fibreChance) continue;
				const m::vec2 at(winLo.x + (winHi.x - winLo.x) * ((double)c + U01(ck, 2)) / (double)columns,
					winLo.y + (winHi.y - winLo.y) * ((double)r + U01(ck, 3)) / (double)rows);
				if (!ply.voids.At(at.x, at.y) || below.voids.At(at.x, at.y) || !below.solid.At(at.x, at.y)) continue;
				const double dirSign = U01(ck, 4) < 0.5 ? 1.0 : -1.0;
				Shard sh;
				sh.key = Key(ck, 0xF2);
				sh.root = m::vec3(at.x, at.y, floorZ - 0.1 * t);
				sh.dir = grain * dirSign;
				sh.length = tearLength * Range(ck, 5, 0.06, 0.32);
				sh.halfW = bundle * Range(ck, 6, 0.1, 0.32);
				sh.halfT = t * Range(ck, 7, 0.1, 0.24);
				sh.bendMax = lift * Range(ck, 8, 0.0, 0.3);
				sh.bendSign = 1.0;
				sh.bendStart = 0.2;
				sh.checkFrom = 0.45;
				sh.sharp = sharp;
				sh.wander = 0.05;
				sh.rings = std::max(3, (int)std::round(0.5 * ringsBase));
				AddShard(sh, stack, toWorld, out);
			}
	}
}

PROJECT_DAMAGE_NS_MACRO_END
