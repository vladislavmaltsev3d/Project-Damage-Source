// строим кабели вдоль линий и укладываем их с учётом препятствий
// ProjectDamage cable generator — GPL-3.0-or-later; see LICENSE.
//
// Cables along curves, or dropped around spawn points, optionally settled by
// a rope simulation onto colliders.
//
// 1. Clump centre lines. Curve: every spline is resampled by arc length
//    (control points kept: the anchors) and, without simulation, hung between
//    anchors by Sag (a parabola along world down taken square to the span's
//    chord). A rotation-minimising frame runs along it (closed for cyclic
//    splines), rolled by the curve tilt. The Amount of cables is split into
//    Clumps, spread over Radius around the curve (in a row for Side by Side,
//    else over a disc). Spawn point: every clump starts somewhere within
//    Radius of the point, with its own heading and length; Noisiness bends
//    its heading along the way, so a dropped clump coils.
// 2. Cables of a clump sit Side by Side, as a round Bundle (rings of 1, 6,
//    12 ..) or Twisted. Noisiness also meanders every clump and (when loose)
//    every cable; without simulation loose cables get Spread (each its own sag).
// 3. Rope simulation (Gravity > 0): position based dynamics, 60 steps per
//    second with substeps: gravity, inextensible segments (plus Slack between
//    pinned points), bending resistance (Stiffness), collisions with the
//    collider triangles (an AABB tree; swept test against tunnelling), an
//    optional floor, cable against cable (spatial hash), friction. Pinned:
//    control points (hanging between them), ends, the start, or nothing. A
//    tied clump is simulated as one thick rope and its cables put back around
//    it afterwards.
// 4. Every cable is one closed tube with the cross-section of its type
//    (round, flat twin & earth, single wire, armored with a spiral ridge,
//    data). An open end is Capped, Stripped (sheath cut back, insulated cores
//    fanning out and ending in bare copper), Torn (ragged sheath, uneven cores)
//    or ends in a Connector (plug with pins, RJ45 for data, ferrule for a
//    single wire). Rat damage: inside a damage zone (or everywhere) holes are
//    bitten through the sheath with a jagged rim and a wall thickness; the
//    cores show through, some bitten to bare copper with strands sticking out
//    of the hole. Ties are bands around a clump with a lock head, clips a
//    clamp at every control point. UVs: U around, V along.
#include "cable_generator.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = EVec;
constexpr double kPi = 3.14159265358979323846;
double Dot(const V& a, const V& b) { return m::la::dot(a, b); }
V Cross(const V& a, const V& b) { return m::la::cross(a, b); }
double Len(const V& v) { return m::la::length(v); }
V Unit(const V& v, const V& fb) { const double l = Len(v); return l > 1e-12 ? v / l : fb; }
double Smooth(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
V Vmin(const V& a, const V& b) { return V(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)); }
V Vmax(const V& a, const V& b) { return V(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)); }

struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL) { U(); }
	double U()
	{
		s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
		return double((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
	}
	double R(double a, double b) { return a + (b - a) * U(); }
	V Dir() { for (;;) { V v(U() * 2 - 1, U() * 2 - 1, U() * 2 - 1); const double l = Len(v); if (l > 1e-3 && l <= 1.0) return v / l; } }
};

uint64_t Hash(uint64_t x)
{
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
	return x;
}

                                   
double Noise(uint64_t key, double x)
{
	const double f = std::floor(x);
	const int64_t i = (int64_t)f;
	auto h = [&](int64_t k) { return double(Hash(key * 0x9e3779b97f4a7c15ULL + (uint64_t)k) >> 11) * (2.0 / 9007199254740992.0) - 1.0; };
	return h(i) + (h(i + 1) - h(i)) * Smooth(x - f);
}

                                                                           
                                                                             
                                                                       
enum { COPPER = 1, BROWN = 2, BLUE, GY, BLACK, GREY, RED, ORANGE, GREEN, TIE = 10, CLIP = 11, PLUG = 12, PIN = 13 };
struct Core { double a, b, r; int slot; bool insulated; };
struct TypeInfo { double aspect, square, fray, helix, sheath; std::vector<Core> cores; };

std::vector<Core> Ring3(double r, int s0, int s1, int s2, double share)
{
	const double a[3] = { kPi / 2, kPi * 7 / 6, kPi * 11 / 6 };
	const int s[3] = { s0, s1, s2 };
	std::vector<Core> c;
	for (int i = 0; i < 3; ++i) c.push_back({ r * std::cos(a[i]), r * std::sin(a[i]), share, s[i], true });
	return c;
}

TypeInfo TypeOf(int type)
{
	switch (type)
	{
	case 1: return { 0.42, 3.0, 0.4, 0.0, 0.16, { { 0.0, -0.56, 0.70, BROWN, true }, { 0.0, 0.56, 0.70, BLUE, true }, { 0.0, 0.0, 0.30, COPPER, false } } };
	case 2: return { 1.0, 2.0, 0.15, 0.0, 0.42, { { 0.0, 0.0, 0.52, COPPER, false } } };
	case 3: return { 1.0, 2.0, 1.0, 1.0, 0.12, Ring3(0.42, BLACK, GREY, GREEN, 0.36) };
	case 4:
	{
		const double a[4] = { 0.4, 2.0, 3.5, 5.1 };
		const int s[4] = { ORANGE, GREEN, BLUE, BROWN };
		std::vector<Core> c;
		for (int i = 0; i < 4; ++i) c.push_back({ 0.45 * std::cos(a[i]), 0.45 * std::sin(a[i]), 0.34, s[i], true });
		return { 1.0, 2.0, 0.3, 0.0, 0.14, c };
	}
	default: return { 1.0, 2.0, 0.5, 0.0, 0.13, Ring3(0.45, BROWN, BLUE, GY, 0.40) };
	}
}
const int kWireSlots[6] = { BROWN, BLUE, GY, BLACK, RED, GREY };

                                                                          
struct Out
{
	CableGenResult& r;
	int maxVerts;
	int32_t Add(const V& p)
	{
		if ((int)r.points.size() >= maxVerts) throw std::length_error("Cables: more vertices than allowed (raise Segment Length, lower Amount or Sides)");
		r.points.push_back(p);
		return (int32_t)r.points.size() - 1;
	}
	void Face(std::initializer_list<int32_t> v, std::initializer_list<std::array<double, 2>> uv, int mat, int cable)
	{
		r.faceVerts.insert(r.faceVerts.end(), v);
		r.loopUv.insert(r.loopUv.end(), uv);
		r.faceSizes.push_back((int32_t)v.size());
		r.faceMat.push_back(mat);
		r.faceCable.push_back(cable);
	}
};

                                                                   
void Section(int K, double square, std::vector<double>& ca, std::vector<double>& cb)
{
	ca.resize(K); cb.resize(K);
	const double e = 2.0 / std::max(square, 2.0);
	for (int k = 0; k < K; ++k)
	{
		const double phi = 2.0 * kPi * k / K, c = std::cos(phi), s = std::sin(phi);
		ca[k] = (c < 0 ? -1.0 : 1.0) * std::pow(std::abs(c), e);
		cb[k] = (s < 0 ? -1.0 : 1.0) * std::pow(std::abs(s), e);
	}
}

std::vector<double> Cum(const std::vector<V>& C, bool closed = false)
{
	std::vector<double> s(C.size() + (closed ? 1 : 0), 0.0);
	for (size_t i = 1; i < C.size(); ++i) s[i] = s[i - 1] + Len(C[i] - C[i - 1]);
	if (closed && C.size() > 1) s.back() = s[C.size() - 1] + Len(C.front() - C.back());
	return s;
}

std::vector<V> Tangents(const std::vector<V>& C, bool closed)
{
	const size_t J = C.size();
	std::vector<V> T(J);
	for (size_t j = 0; j < J; ++j)
	{
		V d;
		if (closed) d = C[(j + 1) % J] - C[(j + J - 1) % J];
		else d = C[std::min(j + 1, J - 1)] - C[j > 0 ? j - 1 : 0];
		T[j] = Unit(d, j > 0 ? T[j - 1] : V(1, 0, 0));
	}
	return T;
}

V Perp(const V& t, const V& want)
{
	V n = want - t * Dot(want, t);
	if (Len(n) < 1e-6)
	{
		const V alt = std::abs(t.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
		n = alt - t * Dot(alt, t);
	}
	return Unit(n, V(0, 0, 1));
}

                                                                            
                               
void Frames(const std::vector<V>& C, const std::vector<V>& T, const V& n0, bool closed, std::vector<V>& N)
{
	const size_t J = C.size();
	N.assign(J, V(0.0));
	N[0] = Perp(T[0], n0);
	auto step = [&](const V& c0, const V& t0, const V& r0, const V& c1, const V& t1) {
		const V v1 = c1 - c0;
		const double q1 = Dot(v1, v1);
		if (q1 < 1e-24) return Perp(t1, r0);
		const V rL = r0 - v1 * (2.0 / q1 * Dot(v1, r0)), tL = t0 - v1 * (2.0 / q1 * Dot(v1, t0));
		const V v2 = t1 - tL;
		const double q2 = Dot(v2, v2);
		const V r1 = q2 < 1e-24 ? rL : rL - v2 * (2.0 / q2 * Dot(v2, rL));
		return Perp(t1, r1);
	};
	for (size_t j = 1; j < J; ++j) N[j] = step(C[j - 1], T[j - 1], N[j - 1], C[j], T[j]);
	if (closed && J > 2)
	{
		const V back = step(C[J - 1], T[J - 1], N[J - 1], C[0], T[0]);
		const double ang = std::atan2(Dot(Cross(back, N[0]), T[0]), Dot(back, N[0]));
		const std::vector<double> s = Cum(C, true);
		for (size_t j = 1; j < J; ++j)
		{
			const double a = ang * s[j] / std::max(s.back(), 1e-12);
			const V b = Cross(T[j], N[j]);
			N[j] = Unit(N[j] * std::cos(a) + b * std::sin(a), N[j]);
		}
	}
}

template <typename A>
A Lerp(const std::vector<double>& s, const std::vector<A>& arr, double u)
{
	if (u <= s.front()) return arr.front();
	if (u >= s.back()) return arr.back();
	const size_t i = std::upper_bound(s.begin(), s.end(), u) - s.begin();
	const double d = s[i] - s[i - 1], t = d > 1e-15 ? (u - s[i - 1]) / d : 0.0;
	return arr[i - 1] + (arr[i] - arr[i - 1]) * t;
}

                                                                         
struct Hole { double s, len, phi, half; uint64_t key; };

struct TubeIn
{
	std::vector<V> C, N, B;
	std::vector<double> ha, hb, s;                                              
	std::vector<std::vector<double>> rmod;                                  
	std::vector<double> pull0, pull1;                                         
	bool closed = false;
};

double AngDiff(double a, double b)
{
	double d = std::fmod(a - b, 2.0 * kPi);
	if (d > kPi) d -= 2.0 * kPi;
	if (d < -kPi) d += 2.0 * kPi;
	return d;
}

                                                                              
void Tube(Out& o, const TubeIn& t, int K, double square, int mat, int cable, double vPer,
	const std::vector<Hole>* holes = nullptr, double thick = 0.0)
{
	const int J = (int)t.C.size();
	if (J < 2) return;
	std::vector<double> ca, cb;
	Section(K, square, ca, cb);
	std::vector<int32_t> ring(J * K);
	const int rows = t.closed ? J : J - 1;
	                                                                      
	                                                                         
	                                                                       
	                                   
	std::vector<uint8_t> cut;
	std::unordered_map<int, std::array<double, 2>> moved;                                           
	auto outline = [](const Hole& h, double th) {
		double w = 0.0;
		for (int mm = 2; mm <= 3; ++mm)
		{
			const uint64_t hk = Hash(h.key + mm);
			w += (0.5 + 0.5 * double(hk & 1023) / 1023.0) / mm * std::sin(mm * th + double((hk >> 10) & 1023) * 0.00614);
		}
		return 1.0 + 0.2 * w;
	};
	if (holes && !holes->empty() && !t.closed)
	{
		std::vector<uint8_t> inside((size_t)J * K, 0);
		std::vector<int> owner((size_t)J * K, -1);
		std::vector<double> best((size_t)J * K, 1e9);
		for (int j = 0; j < J; ++j)
			for (size_t hi = 0; hi < holes->size(); ++hi)
			{
				const Hole& h = (*holes)[hi];
				if (std::abs(t.s[j] - h.s) > 0.8 * h.len) continue;
				for (int k = 0; k < K; ++k)
				{
					const double du = (t.s[j] - h.s) / (0.5 * h.len), dv = AngDiff(2.0 * kPi * k / K, h.phi) / h.half;
					const double f = std::hypot(du, dv) / outline(h, std::atan2(dv, du));
					const size_t v = (size_t)j * K + k;
					if (f < best[v]) { best[v] = f; owner[v] = (int)hi; }
					if (f < 1.0) inside[v] = 1;
				}
			}
		auto in = [&](int j, int k) { return inside[(size_t)j * K + ((k + K) % K)] != 0; };
		cut.assign((size_t)rows * K, 0);
		for (int j = 0; j < rows; ++j)
			for (int k = 0; k < K; ++k)
				if (in(j, k) && in(j, k + 1) && in(j + 1, k) && in(j + 1, k + 1)) cut[(size_t)j * K + k] = 1;
		auto cutAt = [&](int j, int k) { return j >= 0 && j < rows && cut[(size_t)j * K + ((k + K) % K)]; };
		for (int j = 0; j < J; ++j)
			for (int k = 0; k < K; ++k)
			{
				const size_t v = (size_t)j * K + k;
				if (!inside[v] || owner[v] < 0) continue;
				if (!(cutAt(j - 1, k - 1) || cutAt(j - 1, k) || cutAt(j, k - 1) || cutAt(j, k))) continue;
				const Hole& h = (*holes)[owner[v]];
				const double du = (t.s[j] - h.s) / (0.5 * h.len), dv = AngDiff(2.0 * kPi * k / K, h.phi) / h.half;
				const double th = std::atan2(dv, du), r = outline(h, th);
				moved[(int)v] = { h.s + r * std::cos(th) * 0.5 * h.len, h.phi + r * std::sin(th) * h.half };
			}
	}
	const double ex = 2.0 / std::max(square, 2.0);
	auto ringPos = [&](int j, int k, double shrink) {
		auto it = moved.find(j * K + k);
		if (it != moved.end())
		{
			const double u = std::clamp(it->second[0], t.s.front(), t.s[J - 1]), phi = it->second[1];
			const V C = Lerp(t.s, t.C, u), N = Unit(Lerp(t.s, t.N, u), t.N[j]), B = Unit(Lerp(t.s, t.B, u), t.B[j]);
			const double c = std::cos(phi), s = std::sin(phi);
			const double a = (c < 0 ? -1.0 : 1.0) * std::pow(std::abs(c), ex), b = (s < 0 ? -1.0 : 1.0) * std::pow(std::abs(s), ex);
			return C + N * (Lerp(t.s, t.ha, u) * a * (1.0 - shrink)) + B * (Lerp(t.s, t.hb, u) * b * (1.0 - shrink));
		}
		const double rm = (t.rmod.empty() ? 1.0 : t.rmod[j][k]) * (1.0 - shrink);
		return t.C[j] + t.N[j] * (t.ha[j] * ca[k] * rm) + t.B[j] * (t.hb[j] * cb[k] * rm);
	};
	for (int j = 0; j < J; ++j)
	{
		const V T = Unit(j + 1 < J ? t.C[j + 1] - t.C[j] : t.C[j] - t.C[j - 1], V(1, 0, 0));
		for (int k = 0; k < K; ++k)
		{
			V p = ringPos(j, k, 0.0);
			if (!t.closed && j == 0 && !t.pull0.empty()) p = p + T * t.pull0[k];
			if (!t.closed && j == J - 1 && !t.pull1.empty()) p = p - T * t.pull1[k];
			ring[j * K + k] = o.Add(p);
		}
	}
	auto isCut = [&](int j, int k) { return !cut.empty() && j >= 0 && j < rows && cut[(size_t)j * K + ((k + K) % K)]; };
	for (int j = 0; j < rows; ++j)
	{
		const int j1 = (j + 1) % J;
		const double v0 = t.s[j] * vPer, v1 = (t.closed && j1 == 0 ? t.s[J] : t.s[j1]) * vPer;
		for (int k = 0; k < K; ++k)
		{
			if (isCut(j, k)) continue;
			const int k1 = (k + 1) % K;
			const double u0 = double(k) / K, u1 = double(k + 1) / K;
			o.Face({ ring[j * K + k], ring[j * K + k1], ring[j1 * K + k1], ring[j1 * K + k] },
				{ { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } }, mat, cable);
		}
	}
	if (!cut.empty())
	{
		                                                                   
		                              
		std::vector<uint8_t> near((size_t)rows, 0);
		for (int j = 0; j < rows; ++j)
			for (int k = 0; k < K; ++k)
				if (cut[(size_t)j * K + k])
					for (int d = -2; d <= 2; ++d) if (j + d >= 0 && j + d < rows) near[j + d] = 1;
		std::unordered_map<int, int32_t> inner;
		auto in = [&](int j, int k) {
			k = (k + K) % K;
			const int key = j * K + k;
			auto it = inner.find(key);
			if (it != inner.end()) return it->second;
			const int32_t id = o.Add(ringPos(j, k, thick));
			inner[key] = id;
			return id;
		};
		for (int j = 0; j < rows; ++j)
		{
			if (!near[j]) continue;
			const double v0 = t.s[j] * vPer, v1 = t.s[j + 1] * vPer;
			for (int k = 0; k < K; ++k)
			{
				const int k1 = (k + 1) % K;
				const double u0 = double(k) / K, u1 = double(k + 1) / K;
				if (!isCut(j, k))
				{
					o.Face({ in(j, k), in(j + 1, k), in(j + 1, k1), in(j, k1) }, { { u0, v0 }, { u0, v1 }, { u1, v1 }, { u1, v0 } }, mat, cable);
					continue;
				}
				                                              
				auto rim = [&](int ja, int ka, int jb, int kb) {
					o.Face({ ring[ja * K + (ka % K)], ring[jb * K + (kb % K)], in(jb, kb), in(ja, ka) },
						{ { 0.0, 0.0 }, { 0.05, 0.0 }, { 0.05, 0.02 }, { 0.0, 0.02 } }, mat, cable);
				};
				if (!isCut(j, k + 1)) rim(j + 1, k1, j, k1);
				if (!isCut(j, k - 1)) rim(j, k, j + 1, k);
				if (j == 0 || !isCut(j - 1, k)) rim(j, k1, j, k);
				if (j == rows - 1 || !isCut(j + 1, k)) rim(j + 1, k, j + 1, k1);
			}
		}
	}
	if (t.closed) return;
	                                                         
	for (int end = 0; end < 2; ++end)
	{
		const int j = end ? J - 1 : 0;
		const std::vector<double>& pull = end ? t.pull1 : t.pull0;
		double mean = 0.0;
		for (double v : pull) mean += v / K;
		const V T = Unit(end ? t.C[J - 1] - t.C[J - 2] : t.C[1] - t.C[0], V(1, 0, 0));
		const int32_t c = o.Add(t.C[j] + T * (end ? -mean : mean));
		for (int k = 0; k < K; ++k)
		{
			const int k1 = (k + 1) % K;
			const std::array<double, 2> uc{ 0.5, 0.5 }, uk{ 0.5 + 0.5 * cb[k], 0.5 + 0.5 * ca[k] }, uk1{ 0.5 + 0.5 * cb[k1], 0.5 + 0.5 * ca[k1] };
			if (end) o.Face({ c, ring[j * K + k], ring[j * K + k1] }, { uc, uk, uk1 }, mat, cable);
			else o.Face({ c, ring[j * K + k1], ring[j * K + k] }, { uc, uk1, uk }, mat, cable);
		}
	}
}

                                                                      
void Lathe(Out& o, const V& P, const V& T, const V& N, const V& B, const std::vector<std::array<double, 2>>& prof,
	int K, double square, int mat, int cable, double ra = 1.0, double rb = 1.0)
{
	TubeIn t;
	for (const auto& q : prof)
	{
		t.C.push_back(P + T * q[0]); t.N.push_back(N); t.B.push_back(B);
		t.ha.push_back(q[1] * ra); t.hb.push_back(q[1] * rb); t.s.push_back(q[0]);
	}
	Tube(o, t, K, square, mat, cable, 1.0 / std::max(prof.back()[0], 1e-6));
}

                                                          
void Box(Out& o, const V& c, const V& ax, const V& ay, double hx, double hy, double hz, int mat, int cable)
{
	const V az = Cross(ax, ay);
	int32_t id[8];
	for (int i = 0; i < 8; ++i)
		id[i] = o.Add(c + ax * ((i & 1) ? hx : -hx) + ay * ((i & 2) ? hy : -hy) + az * ((i & 4) ? hz : -hz));
	const int quads[6][4] = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
	for (const auto& q : quads)
		o.Face({ id[q[0]], id[q[1]], id[q[2]], id[q[3]] }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, mat, cable);
}

                                                                            
                                                                        
                                                                           
void Band(Out& o, const V& c, const V& T, const V& N, const V& B, double hn, double hb, double square, double th, double w,
	int K, int mat, int cable, bool clip)
{
	std::vector<double> ca, cb;
	Section(K, square, ca, cb);
	std::vector<int32_t> v(K * 4);
	for (int k = 0; k < K; ++k)
	{
		const V dir = Unit(N * (ca[k] / std::max(hn, 1e-9)) + B * (cb[k] / std::max(hb, 1e-9)), N);
		const V p0 = c + N * (hn * ca[k]) + B * (hb * cb[k]) + dir * (0.05 * th);
		const V p1 = p0 + dir * th;
		v[k * 4 + 0] = o.Add(p0 - T * (0.5 * w));
		v[k * 4 + 1] = o.Add(p1 - T * (0.5 * w));
		v[k * 4 + 2] = o.Add(p1 + T * (0.5 * w));
		v[k * 4 + 3] = o.Add(p0 + T * (0.5 * w));
	}
	for (int k = 0; k < K; ++k)
	{
		const int k1 = (k + 1) % K;
		const double u0 = double(k) / K, u1 = double(k + 1) / K;
		for (int q = 0; q < 4; ++q)
		{
			const int q1 = (q + 1) % 4;
			o.Face({ v[k * 4 + q], v[k1 * 4 + q], v[k1 * 4 + q1], v[k * 4 + q1] },
				{ { u0, q * 0.25 }, { u1, q * 0.25 }, { u1, (q + 1) * 0.25 }, { u0, (q + 1) * 0.25 } }, mat, cable);
		}
	}
	const V out = clip ? -N : B;
	const double h = clip ? hn : hb;
	const double bh = clip ? 0.8 * th : 1.2 * th, bl = clip ? 1.3 * std::max(hb, hn) : 0.8 * w;
	const V side = clip ? B : N;
	Box(o, c + out * (h + th + bh), T, side, (clip ? 0.5 : 0.38) * w, bl, bh, mat, cable);
}

                                                                             
struct Bvh
{
	struct Node { V lo, hi; int left = -1, right = -1, start = 0, count = 0; };
	std::vector<V> a, b, c, n;
	std::vector<int> ids;
	std::vector<Node> nodes;
	bool Empty() const { return a.empty(); }
	void Build(const CableGenScene& sc)
	{
		for (const auto& t : sc.colliderTris)
		{
			const V A = sc.colliderPts[t[0]], B = sc.colliderPts[t[1]], C = sc.colliderPts[t[2]];
			const V nn = Cross(B - A, C - A);
			if (Len(nn) < 1e-18) continue;
			a.push_back(A); b.push_back(B); c.push_back(C); n.push_back(Unit(nn, V(0, 0, 1)));
		}
		ids.resize(a.size());
		for (size_t i = 0; i < ids.size(); ++i) ids[i] = (int)i;
		if (!a.empty()) Split(0, (int)a.size());
	}
	int Split(int s, int e)
	{
		Node nd;
		nd.lo = V(1e300); nd.hi = V(-1e300);
		for (int i = s; i < e; ++i) { const int t = ids[i]; nd.lo = Vmin(nd.lo, Vmin(a[t], Vmin(b[t], c[t]))); nd.hi = Vmax(nd.hi, Vmax(a[t], Vmax(b[t], c[t]))); }
		const int me = (int)nodes.size();
		nodes.push_back(nd);
		if (e - s <= 4) { nodes[me].start = s; nodes[me].count = e - s; return me; }
		const V ext = nd.hi - nd.lo;
		const int ax = ext.x > ext.y && ext.x > ext.z ? 0 : (ext.y > ext.z ? 1 : 2);
		const int mid = (s + e) / 2;
		std::nth_element(ids.begin() + s, ids.begin() + mid, ids.begin() + e, [&](int i, int j) {
			const V ci = a[i] + b[i] + c[i], cj = a[j] + b[j] + c[j];
			return ci[ax] < cj[ax];
		});
		const int l = Split(s, mid), r = Split(mid, e);
		nodes[me].left = l; nodes[me].right = r;
		return me;
	}
	void Query(const V& lo, const V& hi, std::vector<int>& outIds) const
	{
		if (nodes.empty()) return;
		int stack[128], sp = 0;
		stack[sp++] = 0;
		while (sp)
		{
			const Node& nd = nodes[stack[--sp]];
			if (nd.hi.x < lo.x || nd.lo.x > hi.x || nd.hi.y < lo.y || nd.lo.y > hi.y || nd.hi.z < lo.z || nd.lo.z > hi.z) continue;
			if (nd.left < 0) { for (int i = 0; i < nd.count; ++i) outIds.push_back(ids[nd.start + i]); continue; }
			if (sp < 126) { stack[sp++] = nd.left; stack[sp++] = nd.right; }
		}
	}
};

                                                                              
V ClosestOnTri(const V& p, const V& a, const V& b, const V& c)
{
	const V ab = b - a, ac = c - a, ap = p - a;
	const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const V bp = p - b;
	const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	const V cp = p - c;
	const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

                                                                   
bool SegTri(const V& p, const V& q, const V& a, const V& b, const V& c, double& t)
{
	const V d = q - p, e1 = b - a, e2 = c - a, h = Cross(d, e2);
	const double det = Dot(e1, h);
	if (std::abs(det) < 1e-18) return false;
	const double f = 1.0 / det;
	const V s = p - a;
	const double u = f * Dot(s, h);
	if (u < 0.0 || u > 1.0) return false;
	const V qv = Cross(s, e1);
	const double v = f * Dot(d, qv);
	if (v < 0.0 || u + v > 1.0) return false;
	t = f * Dot(e2, qv);
	return t >= 0.0 && t <= 1.0;
}

                                                                         
struct Path
{
	std::vector<V> X, N;                                            
	std::vector<double> sc;                           
	std::vector<uint8_t> pin;
	std::vector<int> span;                                                                     
	std::vector<double> tt;                                   
	bool closed = false;
	double radius = 0.0;                                               
	double radius0 = 0.0, radius1 = 0.0;                                                  
	int clump = -1, cable = -1;
};

                                                                    
Path Resample(const Path& in, double h)
{
	Path out = in;
	const size_t n = in.X.size();
	if (n < 2) return out;
	std::vector<V> X = in.X, N = in.N;
	std::vector<double> sc = in.sc;
	std::vector<uint8_t> pin = in.pin;
	std::vector<int> span = in.span;
	if (in.closed) { X.push_back(X.front()); N.push_back(N.front()); sc.push_back(sc.front()); pin.push_back(pin.front()); span.push_back(span.front()); }
	const std::vector<double> s = Cum(X);
	const double total = s.back();
	if (total < 1e-12) return out;
	std::vector<double> keep{ 0.0 };
	for (size_t i = 1; i + 1 < X.size(); ++i) if (pin[i] && s[i] > keep.back() + 1e-9) keep.push_back(s[i]);
	if (total > keep.back() + 1e-9) keep.push_back(total);
	out.X.clear(); out.N.clear(); out.sc.clear(); out.pin.clear(); out.span.clear(); out.tt.clear();
	for (size_t k = 0; k + 1 < keep.size(); ++k)
	{
		const double len = keep[k + 1] - keep[k];
		const int m = std::max(1, (int)std::ceil(len / h - 1e-9));
		for (int j = 0; j < m; ++j)
		{
			const double u = keep[k] + len * j / m;
			out.X.push_back(Lerp(s, X, u));
			out.N.push_back(Lerp(s, N, u));
			out.sc.push_back(Lerp(s, sc, u));
			const size_t i = std::lower_bound(s.begin(), s.end(), u - 1e-9) - s.begin();
			out.pin.push_back(j == 0 && i < pin.size() && std::abs(s[i] - u) < 1e-8 ? pin[i] : 0);
			const size_t near = std::min<size_t>(std::upper_bound(s.begin(), s.end(), u) - s.begin(), s.size()) - 1;
			out.span.push_back(span[near] == -2 || (near + 1 < span.size() && span[near + 1] == -2) ? -2 : -1);
			out.tt.push_back(0.0);
		}
	}
	if (!in.closed)
	{
		out.X.push_back(X.back()); out.N.push_back(N.back()); out.sc.push_back(sc.back()); out.pin.push_back(pin.back());
		out.span.push_back(-1); out.tt.push_back(0.0);
	}
	return out;
}

struct Spline
{
	std::vector<V> P;
	std::vector<double> tilt, rad, t;
	std::vector<int> span;
	std::vector<V> chord;
	std::vector<double> chordLen;
	bool closed = false;
};

Spline ResampleCurve(const CableGenCurve& in, double h, int budget)
{
	Spline L;
	L.closed = in.cyclic && in.pts.size() >= 3;
	std::vector<V> P = in.pts;
	std::vector<double> tl = in.tilt, rd = in.radius;
	std::vector<uint8_t> an = in.anchor;
	if (L.closed) { P.push_back(P.front()); tl.push_back(tl.front()); rd.push_back(rd.front()); an.push_back(1); }
	const std::vector<double> s = Cum(P);
	const double total = s.back();
	if (total < 1e-9) return L;
	std::vector<double> A;
	for (size_t i = 0; i < P.size(); ++i)
		if (an[i] || i == 0 || i + 1 == P.size())
			if (A.empty() || s[i] > A.back() + 1e-9) A.push_back(s[i]);
	if (A.back() < total - 1e-9) A.push_back(total);
	h = std::max(h, total / std::max(budget, 8));
	std::vector<double> us;
	for (size_t k = 0; k + 1 < A.size(); ++k)
	{
		const double len = A[k + 1] - A[k];
		const int n = std::max(1, (int)std::ceil(len / h - 1e-9));
		for (int j = 0; j < n; ++j) { us.push_back(A[k] + len * j / n); L.span.push_back((int)k); L.t.push_back(double(j) / n); }
		const V a = Lerp(s, P, A[k]), b = Lerp(s, P, A[k + 1]);
		L.chord.push_back(b - a);
		L.chordLen.push_back(Len(b - a));
	}
	if (!L.closed) { us.push_back(total); L.span.push_back((int)A.size() - 2); L.t.push_back(1.0); }
	for (double u : us)
	{
		L.P.push_back(Lerp(s, P, u));
		L.tilt.push_back(Lerp(s, tl, u));
		L.rad.push_back(std::max(Lerp(s, rd, u), 0.0));
	}
	return L;
}

                                                                         
void Layout(int n, int arrangement, double gap, double Dw, double Dt, std::vector<std::array<double, 2>>& off,
	double& hullN, double& hullB, double& square)
{
	n = std::max(1, n);
	off.clear();
	if (arrangement == 0)
	{
		const double pitch = Dw + gap;
		for (int i = 0; i < n; ++i) off.push_back({ 0.0, (i - 0.5 * (n - 1)) * pitch });
		hullN = 0.5 * Dt;
		hullB = 0.5 * (n - 1) * pitch + 0.5 * Dw;
		square = n > 1 ? 6.0 : 2.0;
		return;
	}
	const double D = std::max(Dw, Dt) + gap;
	if (n == 1) off.push_back({ 0.0, 0.0 });
	else if (n <= 6)
	{
		const double r = n == 2 ? 0.5 * D : 0.5 * D / std::sin(kPi / n);
		for (int i = 0; i < n; ++i) off.push_back({ r * std::cos(2.0 * kPi * i / n + kPi / 2), r * std::sin(2.0 * kPi * i / n + kPi / 2) });
	}
	else
	{
		off.push_back({ 0.0, 0.0 });
		int left = n - 1;
		for (int ring = 1; left > 0; ++ring)
		{
			const int m = std::min(left, 6 * ring);
			for (int i = 0; i < m; ++i)
			{
				const double a = 2.0 * kPi * i / m + 0.5 * ring;
				off.push_back({ ring * D * std::cos(a), ring * D * std::sin(a) });
			}
			left -= m;
		}
	}
	double rmax = 0.0;
	for (const auto& q : off) rmax = std::max(rmax, std::hypot(q[0], q[1]));
	hullN = hullB = rmax + 0.5 * std::max(Dw, Dt);
	square = 2.0;
}

struct Clump
{
	Path center;
	std::vector<std::array<double, 2>> off;
	double hullN = 0, hullB = 0, square = 2.0, phase = 0.0;
	std::vector<double> anchorU;                                                        
	std::vector<V> spanDir;                                                                  
	std::vector<double> spanSag;
	uint64_t key = 0;
	int firstCable = 0;
	bool tied = false;
};

                                               
                                                                     
                                                                        
void InsertCoil(Path& q, int ji, double Rc, int turns, double pitch, double step, const V& upIn, bool lying = false)
{
	const int J = (int)q.X.size();
	const V P0 = q.X[ji];
	const V T = Unit(q.X[std::min(ji + 1, J - 1)] - q.X[std::max(ji - 1, 0)], V(1, 0, 0));
	const V Tq0 = Unit(T - upIn * Dot(T, upIn), Perp(upIn, V(1, 0, 0)));
	const V side = Cross(Tq0, upIn);
	const V up = lying ? side : upIn;                                              
	const V Tq = Tq0;
	const V Bq = lying ? upIn : side;                                           
	                                                                        
	                                                                  
	const uint64_t key = Hash((uint64_t)(P0.x * 1e6) ^ ((uint64_t)(P0.y * 1e6) << 21) ^ ((uint64_t)(P0.z * 1e6) << 42) ^ (uint64_t)ji);
	const int n = std::clamp((int)std::ceil(2.4 * kPi * Rc * turns / std::max(step, 1e-6)), 12 * turns, 400 * turns);
	std::vector<V> X, N;
	for (int i = 1; i < n; ++i)
	{
		const double a = 2.0 * kPi * turns * i / n, u = a / (2.0 * kPi);
		const double r = Rc * (1.0 + 0.3 * Noise(key, u * 1.3) + 0.08 * Noise(key + 1, u * 7.0));
		const double drop = lying ? 1.0 : 1.0 + 0.45 * (0.5 - 0.5 * std::cos(a));                            
		const double wide = lying ? 1.0 + 0.25 * Noise(key + 2, u * 0.7) : 0.8;
		const double lean = 0.35 * Noise(key + 3, u * 0.9);                                                   
		const V down2 = -up * (r * (1.0 - std::cos(a)) * drop);
		const V across = Tq * (r * wide * std::sin(a));
		const V out = Bq * (pitch * u + r * std::sin(lean) * (1.0 - std::cos(a)) * 0.5);
		X.push_back(P0 + down2 + across + out + (Tq * Noise(key + 4, u * 5.0) + Bq * Noise(key + 5, u * 5.0)) * (0.05 * r));
		N.push_back(Bq);
	}
	                                                                     
	const V shift = Bq * (pitch * turns);
	const std::vector<double> s = Cum(q.X);
	const double fade = std::max(8.0 * pitch, 6.0 * Rc);
	for (int j = ji; j < J; ++j)
	{
		if (q.pin[j]) break;
		const double w = 1.0 - Smooth((s[j] - s[ji]) / fade);
		q.X[j] = q.X[j] + shift * w;
	}
	q.X.insert(q.X.begin() + ji, X.begin(), X.end());
	q.N.insert(q.N.begin() + ji, N.begin(), N.end());
	q.sc.insert(q.sc.begin() + ji, X.size(), q.sc[ji]);
	q.pin.insert(q.pin.begin() + ji, X.size(), (uint8_t)0);
	q.span.insert(q.span.begin() + ji, X.size(), -2);                                         
	q.tt.insert(q.tt.begin() + ji, X.size(), 0.0);
}

                                                                                
void Dangle(Path& q, bool atEnd, double len, double step)
{
	const int J = (int)q.X.size();
	if (J < 2) return;
	if (!atEnd)
	{
		std::reverse(q.X.begin(), q.X.end()); std::reverse(q.N.begin(), q.N.end()); std::reverse(q.sc.begin(), q.sc.end());
		std::reverse(q.pin.begin(), q.pin.end()); std::reverse(q.span.begin(), q.span.end()); std::reverse(q.tt.begin(), q.tt.end());
	}
	const V T = Unit(q.X[J - 1] - q.X[J - 2], V(1, 0, 0));
	q.pin[J - 1] = 0;
	const int n = std::clamp((int)std::ceil(len / std::max(step, 1e-6)), 1, 2000);
	for (int i = 1; i <= n; ++i)
	{
		q.X.push_back(q.X[J - 1] + T * (len * i / n));
		q.N.push_back(q.N[J - 1]); q.sc.push_back(q.sc[J - 1]); q.pin.push_back(0); q.span.push_back(-1); q.tt.push_back(0.0);
	}
	if (!atEnd)
	{
		std::reverse(q.X.begin(), q.X.end()); std::reverse(q.N.begin(), q.N.end()); std::reverse(q.sc.begin(), q.sc.end());
		std::reverse(q.pin.begin(), q.pin.end()); std::reverse(q.span.begin(), q.span.end()); std::reverse(q.tt.begin(), q.tt.end());
	}
}

std::vector<Path> Expand(const Clump& cl, const CableGenParams& p, double Dw, bool sim, const V& up)
{
	std::vector<Path> cables;
	const Path& c = cl.center;
	const int J = (int)c.X.size();
	const std::vector<V> T = Tangents(c.X, c.closed);
	const std::vector<double> s = Cum(c.X, c.closed);
	const bool loose = cl.off.size() == 1 || !cl.tied;
	for (size_t m = 0; m < cl.off.size(); ++m)
	{
		Rng rng(Hash(cl.key * 31 + m * 977 + 5));
		const uint64_t kN = Hash(rng.s + 1), kB = Hash(rng.s + 2);
		std::vector<double> cableSag(cl.spanSag.size());
		for (auto& v : cableSag) v = (rng.U() - 0.5) * 2.0;
		Path q = c;
		q.cable = cl.firstCable + (int)m;
		for (int j = 0; j < J; ++j)
		{
			const V N = Perp(T[j], c.N[j]), B = Cross(T[j], N);
			const double th = p.arrangement == 2 ? 2.0 * kPi * p.twist * s[j] + cl.phase : 0.0;
			const double cs = std::cos(th), sn = std::sin(th);
			const V n = N * cs + B * sn, bb = B * cs - N * sn;
			V x = c.X[j] + (n * cl.off[m][0] + bb * cl.off[m][1]) * c.sc[j];
			if (loose)
			{
				const bool held = c.pin[j] != 0;
				const double env = c.span[j] >= 0 ? std::sqrt(std::max(std::sin(kPi * c.tt[j]), 0.0)) : 1.0;
				if (!sim && c.span[j] >= 0 && !cl.spanSag.empty())
				{
					const int k = c.span[j];
					x = x + cl.spanDir[k] * (cl.spanSag[k] * p.spread * 0.6 * cableSag[k] * 4.0 * c.tt[j] * (1.0 - c.tt[j]));
				}
				if (p.noise > 0.0 && !held && cl.off.size() > 1)
				{
					const double u = s[j] / std::max(p.noiseLength, 1e-4);
					x = x + (n * Noise(kN, u * 1.7) + bb * Noise(kB, u * 1.7 + 17.0)) * (p.noise * 1.2 * Dw * env);
				}
			}
			q.X[j] = x;
			q.N[j] = n;
		}
		if (loose)
		{
			                                                                
			                                                                
			Rng xr(Hash(cl.key * 131 + m * 7 + 1));
			const double f = 1.0 - 0.7 * p.diameterRandom * xr.U();
			for (auto& v : q.sc) v *= f;
			const double L = s[J - 1];
			if (p.fanOut > 0.0 && !c.closed && L > 0.0)
			{
				const double reach = std::min(4.0 * p.fanOut, 0.45 * L);
				for (int end = 0; end < 2; ++end)
				{
					const double a = std::atan2(cl.off[m][1], cl.off[m][0]);
					const bool centre = std::hypot(cl.off[m][0], cl.off[m][1]) < 1e-9;
					const double ang = (centre ? xr.R(0.0, 2.0 * kPi) : a) + xr.R(-0.9, 0.9);
					const double amt = p.fanOut * xr.R(0.4, 1.2);
					for (int j = 0; j < J; ++j)
					{
						const double d = end ? L - s[j] : s[j];
						if (d >= reach) continue;
						const V N = Perp(T[j], c.N[j]), B = Cross(T[j], N);
						const double w = 1.0 - d / reach;
						q.X[j] = q.X[j] + (N * std::cos(ang) + B * std::sin(ang)) * (amt * w * w);
					}
				}
			}
			if (!sim && p.coils > 0.0 && xr.U() < p.coils && J > 8)
			{
				int ji = (int)xr.R(0.2 * J, 0.8 * J);
				while (ji < J - 2 && q.pin[ji]) ++ji;
				if (!q.pin[ji])
				{
					const int turns = std::max(1, (int)std::round(p.coilTurns * xr.R(0.6, 1.4)));
					InsertCoil(q, ji, std::max(p.coilRadius * xr.R(0.6, 1.4), 2.0 * Dw), turns, 1.4 * Dw * f,
						std::max(p.segment, 0.5 * Dw), up);
				}
			}
			if (sim && !c.closed && p.dangle > 0.0 && xr.U() < p.dangle)
				Dangle(q, xr.U() < 0.6, p.dangleLength * xr.R(0.5, 1.5), std::max(p.segment, 0.5 * Dw));
		}
		cables.push_back(std::move(q));
	}
	return cables;
}

                                                                              
struct Sim
{
	std::vector<V> x, xp;
	std::vector<double> w, r;
	std::vector<int> cable, idx;
	struct Range { int b = 0, e = 0; bool closed = false; std::vector<double> rest, rest2; std::vector<int> pinA, pinB; std::vector<double> arc;
		std::vector<uint8_t> curl; std::vector<std::array<int, 2>> shape; std::vector<double> shapeRest; };
	std::vector<Range> ranges;
};

void Simulate(std::vector<Path*>& paths, const Bvh& bvh, const CableGenParams& p, const V& down, CableGenResult& stats)
{
	Sim S;
	for (size_t ci = 0; ci < paths.size(); ++ci)
	{
		Path& P = *paths[ci];
		const int n = (int)P.X.size();
		if (n < 2) continue;
		Sim::Range R;
		R.b = (int)S.x.size(); R.e = R.b + n; R.closed = P.closed;
		int pins = 0;
		for (int i = 0; i < n; ++i)
		{
			S.x.push_back(P.X[i]); S.xp.push_back(P.X[i]);
			S.w.push_back(P.pin[i] ? 0.0 : 1.0);
			S.r.push_back(std::max(P.radius * P.sc[i], i < 3 ? P.radius0 : (i >= n - 3 ? P.radius1 : 0.0)));
			S.cable.push_back((int)ci); S.idx.push_back(i);
			pins += P.pin[i] ? 1 : 0;
		}
		const double slack = pins >= 2 ? 1.0 + p.slack : 1.0;
		const int segs = P.closed ? n : n - 1;
		for (int i = 0; i < segs; ++i) R.rest.push_back(Len(P.X[(i + 1) % n] - P.X[i]) * slack);
		for (int i = 0; i + 2 < n; ++i)
		{
			const bool curl = P.span[i] == -2 || P.span[i + 1] == -2 || P.span[i + 2] == -2;
			R.rest2.push_back(Len(P.X[i + 2] - P.X[i]) * (curl ? 1.0 : slack));
			R.curl.push_back(curl ? 1 : 0);
		}
		                                                                            
		for (int i = 0; i < n; ++i)
		{
			if (P.span[i] != -2) continue;
			for (int off : { 3, 6, 11, 17 })
				if (i + off < n && P.span[i + off] == -2)
				{
					R.shape.push_back({ i, i + off });
					R.shapeRest.push_back(Len(P.X[i + off] - P.X[i]));
				}
		}
		                                                                    
		R.arc.assign(n, 0.0);
		for (int i = 1; i < n; ++i) R.arc[i] = R.arc[i - 1] + (i - 1 < (int)R.rest.size() ? R.rest[i - 1] : 0.0);
		R.pinA.assign(n, -1); R.pinB.assign(n, -1);
		if (!P.closed)
		{
			for (int i = 0, last = -1; i < n; ++i) { if (P.pin[i]) last = i; R.pinA[i] = last; }
			for (int i = n - 1, last = -1; i >= 0; --i) { if (P.pin[i]) last = i; R.pinB[i] = last; }
		}
		S.ranges.push_back(std::move(R));
	}
	const int np = (int)S.x.size();
	stats.particles = np;
	if (np == 0) return;
	const V up = -down;
	const double sub = 1;
	const double dt = 1.0 / 60.0 / sub;
	const V g = down * (9.81 * p.metre * p.gravity * dt * dt);
	const double kb = 0.05 + 0.6 * p.stiffness * p.stiffness;
	double rmax = 0.0;
	for (double v : S.r) rmax = std::max(rmax, v);
	std::vector<int> candStart(np + 1), cand;
	std::vector<int> buf;
	std::vector<V> contactN(np);
	std::vector<uint8_t> contact(np);
	std::vector<std::pair<int64_t, int>> grid(np);
	std::unordered_map<int64_t, std::pair<int, int>> cells;
	cells.reserve(np * 2);
	const double cell = std::max(2.2 * rmax, 1e-6);
	auto hkey = [](int64_t ix, int64_t iy, int64_t iz) { return (ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791); };
	auto selfCollide = [&]() {
		for (int i = 0; i < np; ++i)
			grid[i] = { hkey((int64_t)std::floor(S.x[i].x / cell), (int64_t)std::floor(S.x[i].y / cell), (int64_t)std::floor(S.x[i].z / cell)), i };
		std::sort(grid.begin(), grid.end());
		cells.clear();
		for (int i = 0; i < np;)
		{
			int j = i;
			while (j < np && grid[j].first == grid[i].first) ++j;
			cells[grid[i].first] = { i, j };
			i = j;
		}
		for (int i = 0; i < np; ++i)
		{
			const V& xi = S.x[i];
			const int64_t bx = (int64_t)std::floor(xi.x / cell), by = (int64_t)std::floor(xi.y / cell), bz = (int64_t)std::floor(xi.z / cell);
			for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz)
			{
				auto cit = cells.find(hkey(bx + dx, by + dy, bz + dz));
				if (cit == cells.end()) continue;
				for (int g = cit->second.first; g < cit->second.second; ++g)
				{
					const int j = grid[g].second;
					if (j <= i) continue;
					if (S.cable[i] == S.cable[j] && std::abs(S.idx[i] - S.idx[j]) <= 2) continue;
					const double wi = S.w[i], wj = S.w[j];
					if (wi + wj <= 0.0) continue;
					const V d = S.x[j] - S.x[i];
					const double dist = Len(d), rr = S.r[i] + S.r[j];
					if (dist >= rr || dist < 1e-12) continue;
					const V corr = d * ((rr - dist) / dist / (wi + wj));
					S.x[i] = S.x[i] - corr * wi;
					S.x[j] = S.x[j] + corr * wj;
				}
			}
		}
	};
	const int steps = std::clamp(p.steps, 1, 5000);
	int calm = 0, done = 0;
	for (int step = 0; step < steps * (int)sub; ++step)
	{
		for (int i = 0; i < np; ++i)
		{
			if (S.w[i] <= 0.0) continue;
			const V v = (S.x[i] - S.xp[i]) * 0.985;
			S.xp[i] = S.x[i];
			S.x[i] = S.x[i] + v + g;
		}
		                                                  
		cand.clear();
		for (int i = 0; i < np; ++i)
		{
			candStart[i] = (int)cand.size();
			if (bvh.Empty() || S.w[i] <= 0.0) continue;
			const double m = S.r[i] * 1.5;
			buf.clear();
			bvh.Query(Vmin(S.x[i], S.xp[i]) - V(m), Vmax(S.x[i], S.xp[i]) + V(m), buf);
			                                                           
			const double reach = m + Len(S.x[i] - S.xp[i]);
			const V mid = (S.x[i] + S.xp[i]) * 0.5;
			for (int tri : buf)
			{
				const double plane = std::abs(Dot(mid - bvh.a[tri], bvh.n[tri]));
				if (plane > reach) continue;
				if (Len(ClosestOnTri(mid, bvh.a[tri], bvh.b[tri], bvh.c[tri]) - mid) <= reach) cand.push_back(tri);
			}
		}
		candStart[np] = (int)cand.size();
		                                            
		for (int i = 0; i < np; ++i)
		{
			contact[i] = 0;
			double best = 2.0;
			int bt = -1;
			for (int c = candStart[i]; c < candStart[i + 1]; ++c)
			{
				const int t = cand[c];
				double u;
				if (SegTri(S.xp[i], S.x[i], bvh.a[t], bvh.b[t], bvh.c[t], u) && u < best) { best = u; bt = t; }
			}
			if (bt >= 0)
			{
				V nn = bvh.n[bt];
				if (Dot(S.xp[i] - bvh.a[bt], nn) < 0.0) nn = -nn;
				S.x[i] = S.xp[i] + (S.x[i] - S.xp[i]) * best + nn * S.r[i];
				contact[i] = 1; contactN[i] = nn;
			}
		}
		const int iters = 8;
		for (int it = 0; it < iters; ++it)
		{
			for (const Sim::Range& R : S.ranges)
			{
				const int n = R.e - R.b;
				const bool fwd = (it & 1) == 0;
				for (int q = 0; q < (int)R.rest.size(); ++q)
				{
					const int k = fwd ? q : (int)R.rest.size() - 1 - q;
					const int i = R.b + k, j = R.b + (k + 1) % n;
					const double wi = S.w[i], wj = S.w[j];
					if (wi + wj <= 0.0) continue;
					const V d = S.x[j] - S.x[i];
					const double L = Len(d);
					if (L < 1e-12) continue;
					const V corr = d * ((L - R.rest[k]) / L / (wi + wj));
					S.x[i] = S.x[i] + corr * wi;
					S.x[j] = S.x[j] - corr * wj;
				}
				for (size_t k = 0; k < R.shape.size(); ++k)
				{
					const int i = R.b + R.shape[k][0], j = R.b + R.shape[k][1];
					const double wi = S.w[i], wj = S.w[j];
					if (wi + wj <= 0.0) continue;
					const V d = S.x[j] - S.x[i];
					const double L = Len(d);
					if (L < 1e-12) continue;
					const V corr = d * (0.5 * (L - R.shapeRest[k]) / L / (wi + wj));
					S.x[i] = S.x[i] + corr * wi;
					S.x[j] = S.x[j] - corr * wj;
				}
				for (int k = 0; k < (int)R.rest2.size(); ++k)
				{
					const int i = R.b + k, j = R.b + k + 2;
					const double wi = S.w[i], wj = S.w[j];
					if (wi + wj <= 0.0) continue;
					const V d = S.x[j] - S.x[i];
					const double L = Len(d);
					                                                                            
					if ((L >= R.rest2[k] && !R.curl[k]) || L < 1e-12) continue;
					const V corr = d * ((R.curl[k] ? 0.6 : kb) * (L - R.rest2[k]) / L / (wi + wj));
					S.x[i] = S.x[i] + corr * wi;
					S.x[j] = S.x[j] - corr * wj;
				}
			}
			for (const Sim::Range& R : S.ranges)
				for (int k = 0; k < R.e - R.b; ++k)
				{
					const int i = R.b + k;
					if (S.w[i] <= 0.0) continue;
					for (int pk : { R.pinA[k], R.pinB[k] })
					{
						if (pk < 0) continue;
						const V d = S.x[i] - S.x[R.b + pk];
						const double L = Len(d), lim = std::abs(R.arc[k] - R.arc[pk]);
						if (L > lim && L > 1e-12) S.x[i] = S.x[R.b + pk] + d * (lim / L);
					}
				}
			for (int i = 0; i < np; ++i)
			{
				if (S.w[i] <= 0.0) continue;
				for (int c = candStart[i]; c < candStart[i + 1]; ++c)
				{
					const int t = cand[c];
					                                                            
					V ns = bvh.n[t];
					if (Dot(S.xp[i] - bvh.a[t], ns) < 0.0) ns = -ns;
					const V q = ClosestOnTri(S.x[i], bvh.a[t], bvh.b[t], bvh.c[t]);
					const V d = S.x[i] - q;
					const double dist = Len(d);
					if (Dot(d, ns) < 0.0)
					{
						                                                                  
						if (Dot(S.x[i] - bvh.a[t], ns) < -4.0 * S.r[i]) continue;                              
						S.x[i] = q + ns * S.r[i];
						contact[i] = 1; contactN[i] = ns;
						continue;
					}
					if (dist >= S.r[i]) continue;
					const V nn = dist > 1e-9 ? d / dist : ns;
					S.x[i] = q + nn * S.r[i];
					contact[i] = 1; contactN[i] = nn;
				}
				if (p.useFloor)
				{
					const double h = Dot(S.x[i], up) - p.floorHeight;
					if (h < S.r[i]) { S.x[i] = S.x[i] + up * (S.r[i] - h); contact[i] = 1; contactN[i] = up; }
				}
			}
			if (it == iters / 2) selfCollide();
		}
		                                                                  
		double maxMove = 0.0;
		for (int i = 0; i < np; ++i)
		{
			if (S.w[i] <= 0.0) continue;
			if (contact[i])
			{
				const V d = S.x[i] - S.xp[i];
				const V dn = contactN[i] * Dot(d, contactN[i]);
				S.x[i] = S.xp[i] + dn + (d - dn) * (1.0 - 0.85 * p.friction);
			}
			maxMove = std::max(maxMove, Len(S.x[i] - S.xp[i]));
		}
		done = step + 1;
		calm = maxMove < 1e-4 * p.metre ? calm + 1 : 0;
		if (step > 60 && calm > 20) break;
	}
	stats.simSteps = (int)(done / sub);
	for (size_t ci = 0, r = 0; ci < paths.size(); ++ci)
	{
		Path& P = *paths[ci];
		if (P.X.size() < 2) continue;
		const Sim::Range& R = S.ranges[r++];
		for (int i = R.b; i < R.e; ++i) P.X[i - R.b] = S.x[i];
	}
}

                                                       
void SmoothPath(Path& P, int passes)
{
	const int n = (int)P.X.size();
	for (int pass = 0; pass < passes; ++pass)
	{
		std::vector<V> X = P.X;
		for (int i = 0; i < n; ++i)
		{
			if (P.pin[i]) continue;
			if (!P.closed && (i == 0 || i == n - 1)) continue;
			const V a = P.X[(i + n - 1) % n], b = P.X[(i + 1) % n];
			X[i] = P.X[i] * 0.5 + (a + b) * 0.25;
		}
		P.X.swap(X);
	}
}

                                                                             
struct Cable
{
	std::vector<V> C, N, B;
	std::vector<double> s, scale;
};

void Cores(Out& o, const CableGenParams& p, const TypeInfo& ti, const Cable& c, double sSheath, bool torn,
	Rng& rng, const V& down, double hw, double ht, int K, int cable, double vPer)
{
	const double sEnd = c.s.back(), strip = sEnd - sSheath;
	if (strip <= 1e-6) return;
	const double sc = c.scale.back();
	const double start = std::max(sSheath - 0.8 * 2.0 * ht * sc, c.s.front());
	for (const Core& core : ti.cores)
	{
		const double rc = core.r * ht * sc;
		const double trim = torn ? rng.R(0.0, 0.35) * strip : 0.0;
		const double e = sEnd - trim;
		if (e - start < 2.0 * rc) continue;
		const double step = std::max(0.5 * rc, 1e-4);
		const int n = std::max(3, (int)std::ceil((e - start) / step) + 1);
		std::vector<V> pos(n), Nc(n);
		const double jx = rng.R(-1, 1), jy = rng.R(-1, 1);
		for (int i = 0; i < n; ++i)
		{
			const double u = start + (e - start) * i / (n - 1);
			const V C = Lerp(c.s, c.C, u), N = Unit(Lerp(c.s, c.N, u), V(0, 0, 1)), B = Unit(Lerp(c.s, c.B, u), V(0, 1, 0));
			const double f = Smooth((u - sSheath) / std::max(strip, 1e-9));
			const double spread = 1.0 + p.splay * (torn ? 2.2 : 1.4) * f;
			V q = C + N * (core.a * ht * sc * spread) + B * (core.b * hw * sc * spread);
			if (core.a == 0.0 && core.b == 0.0)
				q = q + Unit(N * jx + B * jy, N) * (p.splay * 0.6 * ht * sc * f);
			if (torn) q = q + down * (p.splay * 0.25 * strip * f * f);
			pos[i] = q;
			Nc[i] = N;
		}
		Cable k;
		k.C = pos;
		k.s = Cum(pos);
		const std::vector<V> T = Tangents(pos, false);
		Frames(pos, T, Nc[0], false, k.N);
		const int Kc = std::max(6, K / 2 + 2);
		const double len = k.s.back();
		auto piece = [&](double a, double b, double r, int mat) {
			TubeIn t;
			std::vector<double> sa{ a };
			for (double v : k.s) if (v > a + 1e-9 && v < b - 1e-9) sa.push_back(v);
			sa.push_back(b);
			if (b - a < 1e-7) return;
			for (double u : sa)
			{
				const V C = Lerp(k.s, k.C, u), T2 = Unit(Lerp(k.s, T, u), V(1, 0, 0));
				const V N = Perp(T2, Lerp(k.s, k.N, u));
				t.C.push_back(C); t.N.push_back(N); t.B.push_back(Cross(T2, N));
				t.ha.push_back(r); t.hb.push_back(r); t.s.push_back(u);
			}
			Tube(o, t, Kc, 2.0, mat, cable, vPer);
		};
		if (core.insulated)
		{
			const double bare = std::min(torn ? rng.R(0.25, 0.6) * strip : 0.3 * strip, std::max(len - 2.0 * rc, 0.0));
			const double cut = len - bare;
			if (cut > 1e-6) piece(0.0, cut, rc, core.slot);
			piece(std::max(cut - 0.5 * rc, 0.0), len, rc * 0.55, COPPER);
		}
		else piece(0.0, len, rc, COPPER);
	}
}

                                             
void Strand(Out& o, Rng& rng, const V& p, V d, double len, double r, int cable, const V& down)
{
	const int n = 7;
	std::vector<V> X{ p };
	for (int i = 1; i < n; ++i)
	{
		d = Unit(d + rng.Dir() * 0.45 + down * 0.08, d);
		X.push_back(X.back() + d * (len / (n - 1)));
	}
	TubeIn t;
	t.C = X;
	const std::vector<V> T = Tangents(X, false);
	Frames(X, T, rng.Dir(), false, t.N);
	t.s = Cum(X);
	for (int i = 0; i < n; ++i)
	{
		t.B.push_back(Cross(T[i], t.N[i]));
		const double f = 1.0 - 0.5 * double(i) / (n - 1);
		t.ha.push_back(r * f); t.hb.push_back(r * f);
	}
	Tube(o, t, 4, 2.0, COPPER, cable, 1.0 / std::max(len, 1e-6));
}

                                                                          
                                  
void HoleInsides(Out& o, const CableGenParams& p, const TypeInfo& ti, const Cable& c, const Hole& h, double a0, double a1,
	double hw, double ht, double D, int K, int cable, double vPer, const V& down, CableGenResult& out)
{
	Rng rng(Hash(h.key + 99));
	const double s0 = std::max(h.s - 0.5 * h.len - 0.6 * D, a0 + 1e-6), s1 = std::min(h.s + 0.5 * h.len + 0.6 * D, a1 - 1e-6);
	if (s1 - s0 < 1e-6) return;
	const int n = std::max(4, (int)std::ceil((s1 - s0) / (h.len / 6.0)) + 1);
	const double sc = Lerp(c.s, c.scale, h.s);
	const int Kc = std::max(6, K / 2 + 2);
	for (const Core& core : ti.cores)
	{
		const double rc = core.r * ht * sc;
		const double cphi = std::atan2(core.b, core.a);
		const bool facing = (core.a == 0.0 && core.b == 0.0) || std::abs(AngDiff(cphi, h.phi)) < h.half + 0.8;
		const bool bitten = facing && (!core.insulated || rng.U() < 0.45 + 0.5 * p.strands);
		const double g0 = h.s - 0.32 * h.len * rng.R(0.5, 1.0), g1 = h.s + 0.32 * h.len * rng.R(0.5, 1.0);
		std::vector<V> pos(n);
		std::vector<double> us(n);
		for (int i = 0; i < n; ++i)
		{
			const double u = s0 + (s1 - s0) * i / (n - 1);
			const V C = Lerp(c.s, c.C, u), N = Unit(Lerp(c.s, c.N, u), V(0, 0, 1)), B = Unit(Lerp(c.s, c.B, u), V(0, 1, 0));
			pos[i] = C + N * (core.a * ht * sc) + B * (core.b * hw * sc);
			us[i] = u;
		}
		auto piece = [&](double a, double b, double r, int mat) {
			if (b - a < 1e-7) return;
			TubeIn t;
			std::vector<double> sa{ a };
			for (double v : us) if (v > a + 1e-9 && v < b - 1e-9) sa.push_back(v);
			sa.push_back(b);
			for (double u : sa)
			{
				const V C = Lerp(us, pos, u), N = Unit(Lerp(c.s, c.N, u), V(0, 0, 1));
				const V Tt = Unit(Lerp(us, pos, std::min(u + 1e-4, s1)) - Lerp(us, pos, std::max(u - 1e-4, s0)), V(1, 0, 0));
				const V Np = Perp(Tt, N);
				t.C.push_back(C); t.N.push_back(Np); t.B.push_back(Cross(Tt, Np));
				t.ha.push_back(r); t.hb.push_back(r); t.s.push_back(u);
			}
			Tube(o, t, Kc, 2.0, mat, cable, vPer);
		};
		if (core.insulated && bitten)
		{
			piece(s0, g0, rc, core.slot);
			piece(g1, s1, rc, core.slot);
			piece(g0 - 0.3 * rc, g1 + 0.3 * rc, 0.55 * rc, COPPER);
		}
		else if (core.insulated) piece(s0, s1, rc, core.slot);
		else piece(s0, s1, rc * (ti.cores.size() == 1 ? 1.0 : 0.9), COPPER);
		if (!bitten || p.strands <= 0.0) continue;
		const int ns = (int)std::round(p.strands * rng.R(3.0, 9.0));
		const double rCu = core.insulated ? 0.55 * rc : rc;
		for (int k = 0; k < ns; ++k)
		{
			const double u = rng.R(std::max(g0, s0), std::min(g1, s1));
			const V C = Lerp(c.s, c.C, u), N = Unit(Lerp(c.s, c.N, u), V(0, 0, 1)), B = Unit(Lerp(c.s, c.B, u), V(0, 1, 0));
			const V T = Unit(Lerp(c.s, c.C, std::min(u + 1e-3, c.s.back())) - Lerp(c.s, c.C, std::max(u - 1e-3, c.s.front())), V(1, 0, 0));
			const V outDir = Unit(N * std::cos(h.phi) + B * std::sin(h.phi), N);
			const V base = C + N * (core.a * ht * sc) + B * (core.b * hw * sc) + outDir * (0.6 * rCu);
			const V d0 = Unit(outDir * rng.R(0.5, 1.0) + T * rng.R(-0.8, 0.8) + rng.Dir() * 0.3, outDir);
			Strand(o, rng, base, d0, p.strandLength * D * rng.R(0.4, 1.2), std::max(0.13 * rCu, 4e-5), cable, down);
		}
	}
	++out.holes;
}

                                                            
void Connector(Out& o, int type, const V& P, const V& T, const V& N, const V& B, double hw, double ht, double D, int K, int cable)
{
	const double re = std::max(hw, ht);
	if (type == 4)
	{
		                                                             
		const double l = 3.5 * D, w = 1.95 * D, h = 1.35 * D;
		Box(o, P + T * (0.5 * l), T, B, 0.5 * l, 0.5 * w, 0.5 * h, PLUG, cable);
		for (int i = 0; i < 8; ++i)
			Box(o, P + T * (l - 0.28 * D) + B * ((i - 3.5) * 0.2 * D) - N * (0.5 * h - 0.05 * D), T, B, 0.24 * D, 0.045 * D, 0.07 * D, PIN, cable);
		const double tilt = 0.2;
		const V lt = Unit(T * std::cos(tilt) + N * std::sin(tilt), T);
		Box(o, P + T * (0.62 * l) + N * (0.5 * h + 0.18 * D), lt, B, 0.33 * l, 0.3 * D, 0.05 * D, PLUG, cable);
		return;
	}
	if (type == 2)
	{
		                                                      
		const double r = re;
		Lathe(o, P - T * (0.4 * D), T, N, B, { { 0.0, 1.55 * r }, { 0.15 * D, 1.75 * r }, { 1.0 * D, 1.75 * r }, { 1.2 * D, 1.3 * r } }, K, 2.0, PLUG, cable);
		Lathe(o, P + T * (1.1 * D), T, N, B, { { 0.0, 1.15 * r }, { 2.4 * D, 1.15 * r }, { 2.6 * D, 0.9 * r } }, K, 2.0, PIN, cable);
		return;
	}
	                                                            
	const double rh = 1.75 * re + 0.15 * D, L = std::max(3.4 * re, 2.2 * D);
	Lathe(o, P, T, N, B, { { 0.0, 0.8 * rh }, { 0.06 * L, rh }, { 0.3 * L, rh }, { 0.33 * L, 0.94 * rh }, { 0.36 * L, rh },
		{ 0.48 * L, rh }, { 0.51 * L, 0.94 * rh }, { 0.54 * L, rh }, { 0.92 * L, rh }, { L, 0.9 * rh } }, std::max(K, 16), 2.4, PLUG, cable);
	const double rp = 0.13 * rh, lp = 1.1 * rh;
	for (int sgn : { -1, 1 })
		Lathe(o, P + T * (0.98 * L) + B * (sgn * 0.45 * rh), T, N, B, { { 0.0, rp }, { 0.85 * lp, rp }, { lp, 0.6 * rp } }, 8, 2.0, PIN, cable);
	if (type == 0 || type == 3)
		Lathe(o, P + T * (0.98 * L) + N * (0.5 * rh), T, N, B, { { 0.0, 0.8 * rp }, { 0.6 * lp, 0.8 * rp }, { 0.7 * lp, 0.5 * rp } }, 8, 2.0, PIN, cable);
}

}             

// основной вход: собираем геометрию по переданным настройкам
bool BuildCableCurves(const std::vector<CableGenCurve>& curves, const CableGenScene& scene, const CableGenParams& pIn,
	CableGenResult& out)
{
	CableGenParams p = pIn;
	p.count = std::clamp(p.count, 1, 256);
	p.clumps = std::clamp(p.clumps, 1, std::min(64, p.count));
	p.sides = std::clamp(p.sides, 4, 48);
	p.segment = std::max(p.segment, 1e-4);
	p.metre = std::max(p.metre, 1e-6);
	const TypeInfo ti = TypeOf(p.type);
	const double Dw = std::max(p.diameter, 1e-5), Dt = Dw * ti.aspect, hw = 0.5 * Dw, ht = 0.5 * Dt;
	const double D = std::max(Dw, Dt);
	const V down = Unit(V(p.downX, p.downY, p.downZ), V(0, 0, -1)), up = -down;
	const bool sim = p.gravity > 0.0;
	Out o{ out, std::max(1000, p.maxVerts) };
	const int K0 = p.sides;
	const double vPer = p.uvScale / (kPi * Dw);
	const int nsrc = (int)curves.size() + (int)scene.spawns.size();
	const int budget = std::max(16, p.maxVerts / std::max(1, nsrc) / std::max(1, p.count * (K0 + 2)));
	                                   
	std::vector<int> members(p.clumps);
	for (int c = 0; c < p.clumps; ++c) members[c] = p.count / p.clumps + (c < p.count % p.clumps ? 1 : 0);
	const bool tiesOn = p.tieSpacing > 0.0;

	                             
	std::vector<Clump> clumps;
	int cableCount = 0;
	auto clumpLayout = [&](Clump& cl, int c) {
		Layout(members[c], p.arrangement, p.gap, Dw, Dt, cl.off, cl.hullN, cl.hullB, cl.square);
		cl.tied = tiesOn && members[c] > 1;
		cl.firstCable = cableCount;
		cableCount += members[c];
	};
	                                                                         
	double hullMax = 0.0;
	{
		std::vector<std::array<double, 2>> off;
		double hn, hb, sq;
		Layout(members[0], p.arrangement, p.gap, Dw, Dt, off, hn, hb, sq);
		hullMax = std::max(hn, hb);
	}
	auto clumpOffsets = [&](Rng& rng) {
		std::vector<std::array<double, 2>> co(p.clumps, { 0.0, 0.0 });
		if (p.clumps == 1) return co;
		const double minGap = 2.0 * hullMax + p.gap + 0.5 * D;
		if (p.arrangement == 0)
		{
			const double pitch = std::max(2.0 * p.radius / (p.clumps - 1), minGap);
			for (int c = 0; c < p.clumps; ++c) co[c] = { 0.0, (c - 0.5 * (p.clumps - 1)) * pitch + rng.R(-0.15, 0.15) * p.radius / p.clumps };
			return co;
		}
		const double R = std::max(p.radius, minGap * 0.6 * std::sqrt((double)p.clumps));
		for (int c = 0; c < p.clumps; ++c)
		{
			const double r = R * std::sqrt((c + 0.5) / p.clumps), a = c * 2.39996 + rng.R(-0.3, 0.3);
			co[c] = { r * std::cos(a), r * std::sin(a) };
		}
		return co;
	};
	for (size_t ci = 0; ci < curves.size(); ++ci)
	{
		const CableGenCurve& in = curves[ci];
		if (in.pts.size() < 2) continue;
		Spline L = ResampleCurve(in, p.segment, budget);
		const int J = (int)L.P.size();
		if (J < 2) continue;
		Rng rng(Hash(uint64_t(p.seed) * 7919 + ci * 104729 + 11));
		std::vector<double> spanSag(L.chord.size());
		std::vector<V> spanDir(L.chord.size());
		for (size_t k = 0; k < L.chord.size(); ++k)
		{
			const V cu = Unit(L.chord[k], V(1, 0, 0));
			spanDir[k] = down - cu * Dot(down, cu);
			spanSag[k] = sim ? 0.0 : p.sag * L.chordLen[k] * (1.0 + p.sagRandom * (rng.U() - 0.5) * 1.2);
		}
		std::vector<V> BC(J);
		for (int j = 0; j < J; ++j) BC[j] = L.P[j] + spanDir[L.span[j]] * (spanSag[L.span[j]] * 4.0 * L.t[j] * (1.0 - L.t[j]));
		const std::vector<V> T = Tangents(BC, L.closed);
		std::vector<V> N;
		Frames(BC, T, up, L.closed, N);
		std::vector<double> sc(J, 1.0);
		for (int j = 0; j < J; ++j)
		{
			if (p.useTilt && L.tilt[j] != 0.0)
			{
				const V b = Cross(T[j], N[j]);
				N[j] = N[j] * std::cos(L.tilt[j]) + b * std::sin(L.tilt[j]);
			}
			if (p.useRadius) sc[j] = std::max(L.rad[j], 1e-3);
		}
		const std::vector<double> s = Cum(BC, L.closed);
		const double total = s[L.closed ? J : J - 1];
		std::vector<double> anchorU;
		for (int j = 0; j < J; ++j) if (L.t[j] == 0.0 || (!L.closed && j == J - 1)) anchorU.push_back(s[j] / std::max(total, 1e-12));
		const auto co = clumpOffsets(rng);
		for (int c = 0; c < p.clumps; ++c)
		{
			Clump cl;
			cl.key = Hash(uint64_t(p.seed) * 1000003 + ci * 7919 + c * 31 + 17);
			clumpLayout(cl, c);
			cl.phase = Rng(cl.key).R(0.0, 2.0 * kPi);
			cl.anchorU = anchorU;
			cl.spanDir = spanDir;
			cl.spanSag = spanSag;
			Path& P = cl.center;
			P.closed = L.closed;
			P.clump = (int)clumps.size();
			const uint64_t kN = Hash(cl.key + 3), kB = Hash(cl.key + 4);
			for (int j = 0; j < J; ++j)
			{
				const V Bv = Cross(T[j], N[j]);
				const bool anchor = L.t[j] == 0.0 || (!L.closed && j == J - 1);
				const bool pinned = sim && (p.pin == 0 ? anchor : p.pin == 1 ? (!L.closed && (j == 0 || j == J - 1))
					: p.pin == 2 ? j == 0 : false);
				const bool held = pinned || (!sim && (p.sag > 0.0 || p.clips) && anchor);
				const double env = held || (sim && p.pin == 0) ? std::sqrt(std::max(std::sin(kPi * L.t[j]), 0.0)) : 1.0;
				V x = BC[j] + (N[j] * co[c][0] + Bv * co[c][1]) * sc[j];
				if (p.noise > 0.0 && !pinned)
				{
					const double u = s[j] / std::max(p.noiseLength, 1e-4);
					const double amp = p.noise * (3.0 * D + 0.25 * p.radius) * env;
					x = x + (N[j] * Noise(kN, u) + Bv * Noise(kB, u + 31.0)) * amp;
				}
				P.X.push_back(x);
				P.N.push_back(N[j]);
				P.sc.push_back(sc[j]);
				P.pin.push_back(pinned ? 1 : 0);
				P.span.push_back(L.span[j]);
				P.tt.push_back(L.t[j]);
			}
			clumps.push_back(std::move(cl));
		}
	}
	for (size_t si = 0; si < scene.spawns.size(); ++si)
	{
		const V o0 = scene.spawns[si];
		Rng rng(Hash(uint64_t(p.seed) * 6151 + si * 92821 + 3));
		const V e1 = Perp(up, V(1, 0, 0)), e2 = Cross(up, e1);
		const auto co = clumpOffsets(rng);
		for (int c = 0; c < p.clumps; ++c)
		{
			Clump cl;
			cl.key = Hash(uint64_t(p.seed) * 2000003 + si * 7919 + c * 31 + 23);
			clumpLayout(cl, c);
			Rng cr(cl.key);
			cl.phase = cr.R(0.0, 2.0 * kPi);
			const double heading = cr.R(0.0, 2.0 * kPi);
			const double len = cr.R(std::min(p.lengthMin, p.lengthMax), std::max(p.lengthMin, p.lengthMax));
			                                                                       
			                                        
			V x = o0 + e1 * co[c][0] + e2 * co[c][1] + up * (c * 2.4 * hullMax);
			x = x - (e1 * std::cos(heading) + e2 * std::sin(heading)) * (0.5 * len * (1.0 - 0.6 * p.noise));
			const int n = std::max(2, std::min(budget, (int)std::ceil(len / p.segment)) + 1);
			const double h = len / (n - 1);
			const uint64_t kH = Hash(cl.key + 5), kZ = Hash(cl.key + 6);
			Path& P = cl.center;
			P.clump = (int)clumps.size();
			for (int j = 0; j < n; ++j)
			{
				const double s = j * h, u = s / std::max(p.noiseLength, 1e-4);
				const double ang = heading + p.noise * 3.2 * Noise(kH, u * 0.6) + p.noise * 1.4 * Noise(kH + 1, u * 2.0);
				const V dir = e1 * std::cos(ang) + e2 * std::sin(ang);
				if (j > 0) x = x + dir * h + up * (p.noise * 0.15 * h * Noise(kZ, u));
				const V T = dir;
				P.X.push_back(x);
				P.N.push_back(Perp(T, up));
				P.sc.push_back(1.0);
				const bool pinned = sim && (p.pin == 0 || p.pin == 2 ? j == 0 : p.pin == 1 ? (j == 0 || j == n - 1) : false);
				P.pin.push_back(pinned ? 1 : 0);
				P.span.push_back(-1);
				P.tt.push_back(0.0);
			}
			clumps.push_back(std::move(cl));
		}
	}

	                                
	std::vector<Path> cables;
	if (!sim)
	{
		for (const Clump& cl : clumps)
			for (Path& q : Expand(cl, p, Dw, false, up)) cables.push_back(std::move(q));
	}
	else
	{
		Bvh bvh;
		bvh.Build(scene);
		double total = 0.0;
		for (const Clump& cl : clumps) total += Cum(cl.center.X).back() * (cl.tied ? 1.0 : (double)cl.off.size());
		const double hs = std::max({ p.segment, 0.75 * D, 0.02 * p.metre, total / 30000.0 });
		std::vector<Path> units;
		std::vector<int> unitClump;
		for (size_t ci = 0; ci < clumps.size(); ++ci)
		{
			const Clump& cl = clumps[ci];
			if (cl.tied)
			{
				Path c = Resample(cl.center, hs);
				c.radius = std::max(cl.hullN, cl.hullB) + 0.15 * D;
				units.push_back(std::move(c));
				unitClump.push_back((int)ci);
			}
			else
				for (Path& q : Expand(cl, p, Dw, true, up))
				{
					Path r = Resample(q, hs);
					r.radius = 0.5 * (0.5 * (Dw + Dt)) + 0.05 * D;
					const double plug = p.type == 4 ? 0.7 * D : p.type == 2 ? 0.9 * D : 0.95 * D + 0.15 * D;
					if (p.endStart == 3) r.radius0 = plug;
					if (p.endEnd == 3) r.radius1 = plug;
					units.push_back(std::move(r));
					unitClump.push_back(-1);
				}
		}
		std::vector<Path*> ptrs;
		for (Path& u : units) ptrs.push_back(&u);
		Simulate(ptrs, bvh, p, down, out);
		for (size_t u = 0; u < units.size(); ++u)
		{
			SmoothPath(units[u], 2);
			Path r = Resample(units[u], p.segment);
			if (unitClump[u] >= 0)
			{
				Clump cl = clumps[unitClump[u]];
				cl.center = r;
				cl.spanSag.clear();
				for (Path& q : Expand(cl, p, Dw, true, up)) cables.push_back(std::move(q));
				clumps[unitClump[u]].center = r;
			}
			else
			{
				                                                                     
				                                                
				Rng xr(Hash(uint64_t(p.seed) * 3571 + r.cable * 7 + 2));
				const int J = (int)r.X.size();
				if (p.coils > 0.0 && xr.U() < p.coils && J > 8)
				{
					int ji = (int)xr.R(0.2 * J, 0.8 * J);
					while (ji < J - 2 && r.pin[ji]) ++ji;
					const double rr = units[u].radius * 2.5;
					bool lying = p.useFloor && Dot(r.X[ji], up) - p.floorHeight < rr;
					if (!lying && !bvh.Empty())
					{
						std::vector<int> near;
						bvh.Query(r.X[ji] - V(rr), r.X[ji] + V(rr), near);
						for (int tri : near)
							if (Len(ClosestOnTri(r.X[ji], bvh.a[tri], bvh.b[tri], bvh.c[tri]) - r.X[ji]) < rr) { lying = true; break; }
					}
					const int turns = std::max(1, (int)std::round(p.coilTurns * xr.R(0.6, 1.4)));
					const double f = r.sc[ji];
					InsertCoil(r, ji, std::max(p.coilRadius * xr.R(0.6, 1.4), 2.0 * Dw), turns, 1.4 * Dw * f,
						std::max(p.segment, 0.5 * Dw), up, lying);
				}
				cables.push_back(std::move(r));
			}
		}
	}

	                 
	const bool damageOn = p.damage > 0.0 && (p.damageEverywhere || !scene.zones.empty());
	std::vector<std::vector<int>> clumpCables(clumps.size());
	for (size_t c = 0; c < cables.size(); ++c)
	{
		Path& P = cables[c];
		if (P.clump >= 0) clumpCables[P.clump].push_back((int)c);
		const bool closed = P.closed;
		const int J = (int)P.X.size();
		if (J < 2) continue;
		Rng rng(Hash(uint64_t(p.seed) * 424243 + P.cable * 31 + 7));
		Cable cb;
		cb.C = P.X; cb.scale = P.sc;
		const std::vector<V> Tc = Tangents(cb.C, closed);
		cb.N.resize(J); cb.B.resize(J);
		for (int j = 0; j < J; ++j) { cb.N[j] = Perp(Tc[j], P.N[j]); cb.B[j] = Cross(Tc[j], cb.N[j]); }
		cb.s = Cum(cb.C, closed);
		const double total = cb.s[closed ? J : J - 1];
		if (total < 0.5 * Dw) continue;
		const int slot = p.type == 2 ? kWireSlots[(P.cable * 7) % 6] : 0;
		const int mode0 = closed ? 0 : p.endStart, mode1 = closed ? 0 : p.endEnd;
		double strip0 = 0.0, strip1 = 0.0;
		if (mode0 == 1 || mode0 == 2) strip0 = std::min(p.strip * (mode0 == 2 ? rng.R(0.6, 1.3) : 1.0), 0.45 * total);
		if (mode1 == 1 || mode1 == 2) strip1 = std::min(p.strip * (mode1 == 2 ? rng.R(0.6, 1.3) : 1.0), 0.45 * total);
		if (total - strip0 - strip1 < Dw) strip0 = strip1 = 0.0;
		                                                    
		const double boot = 1.3 * D;
		const double a = strip0, b = total - strip1;
		            
		std::vector<Hole> holes;
		if (damageOn && !closed)
		{
			std::vector<std::array<double, 2>> zones;
			if (p.damageEverywhere) zones.push_back({ a, b });
			else
			{
				double z0 = -1.0;
				for (int j = 0; j < J; ++j)
				{
					bool inside = false;
					for (const auto& z : scene.zones) if (Len(cb.C[j] - V(z[0], z[1], z[2])) < z[3]) { inside = true; break; }
					if (inside && z0 < 0.0) z0 = cb.s[j];
					if ((!inside || j == J - 1) && z0 >= 0.0) { zones.push_back({ z0, cb.s[j] }); z0 = -1.0; }
				}
			}
			const double margin = std::max(1.5 * D, 0.0) + (mode0 == 3 || mode1 == 3 ? boot : 0.0);
			for (const auto& z : zones)
			{
				const double lo = std::max(z[0], a + margin), hi = std::min(z[1], b - margin);
				if (hi <= lo) continue;
				const double want = p.damage * (hi - lo) + rng.U();
				const int nh = std::min((int)want, 400);
				for (int k = 0; k < nh; ++k)
				{
					Hole h;
					h.len = std::max(p.damageSize * D * rng.R(0.6, 1.4), 0.3 * D);
					h.s = rng.R(lo, hi);
					h.phi = rng.R(0.0, 2.0 * kPi);
					h.half = std::clamp(rng.R(0.3, 0.7) * kPi * std::min(p.damageSize, 1.5), 0.25 * kPi, 0.9 * kPi);
					h.key = Hash(rng.s + k);
					bool clash = false;
					for (const Hole& o2 : holes) if (std::abs(o2.s - h.s) < 0.5 * (o2.len + h.len) + 0.3 * D) clash = true;
					if (!clash) holes.push_back(h);
				}
			}
		}
		const int K = holes.empty() ? K0 : std::max(K0, 32);
		TubeIn t;
		t.closed = closed;
		if (closed)
		{
			t.C = cb.C; t.N = cb.N; t.B = cb.B; t.s = cb.s;
			for (int j = 0; j < J; ++j) { t.ha.push_back(ht * cb.scale[j]); t.hb.push_back(hw * cb.scale[j]); }
		}
		else
		{
			std::vector<double> sa{ a };
			for (int j = 0; j < J; ++j) if (cb.s[j] > a + 1e-9 && cb.s[j] < b - 1e-9) sa.push_back(cb.s[j]);
			sa.push_back(b);
			                                                                     
			for (const Hole& h : holes)
			{
				const double st = h.len / 20.0;
				for (double u = h.s - 0.8 * h.len; u <= h.s + 0.8 * h.len; u += st) if (u > a + 1e-9 && u < b - 1e-9) sa.push_back(u);
			}
			std::sort(sa.begin(), sa.end());
			sa.erase(std::unique(sa.begin(), sa.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), sa.end());
			std::vector<double> sj(cb.s.begin(), cb.s.begin() + J);
			for (double u : sa)
			{
				const V C = Lerp(sj, cb.C, u), Tl = Unit(Lerp(sj, Tc, u), V(1, 0, 0));
				const V Nn = Perp(Tl, Lerp(sj, cb.N, u));
				const double k = Lerp(sj, cb.scale, u);
				t.C.push_back(C); t.N.push_back(Nn); t.B.push_back(Cross(Tl, Nn));
				t.ha.push_back(ht * k); t.hb.push_back(hw * k); t.s.push_back(u);
			}
			auto ragged = [&](std::vector<double>& pull, double seg) {
				const double amt = std::min(p.fray * ti.fray * 0.6 * Dw, 0.6 * seg);
				pull.assign(K, 0.0);
				if (amt <= 0.0) return;
				double lo = 1e30, hi = -1e30;
				const double w2 = rng.R(0.3, 1.0), w3 = rng.R(0.3, 1.0), w5 = rng.R(0.3, 1.0);
				const double o2 = rng.R(0, 6.283), o3 = rng.R(0, 6.283), o5 = rng.R(0, 6.283);
				for (int k = 0; k < K; ++k)
				{
					const double phi = 2.0 * kPi * k / K;
					pull[k] = w2 / 2 * std::sin(2 * phi + o2) + w3 / 3 * std::sin(3 * phi + o3) + w5 / 5 * std::sin(5 * phi + o5);
					lo = std::min(lo, pull[k]); hi = std::max(hi, pull[k]);
				}
				for (int k = 0; k < K; ++k)
				{
					double w = (pull[k] - lo) / std::max(hi - lo, 1e-9);
					if (ti.helix > 0.0 && rng.U() < 0.3) w = std::min(1.0, w + 0.6);
					pull[k] = w * amt;
				}
			};
			const size_t n = t.C.size();
			if (n >= 2)
			{
				if (mode0 == 2 && strip0 > 0.0) ragged(t.pull0, Len(t.C[1] - t.C[0]));
				if (mode1 == 2 && strip1 > 0.0) ragged(t.pull1, Len(t.C[n - 1] - t.C[n - 2]));
			}
		}
		if (ti.helix > 0.0)
		{
			const double pitch = 0.45 * Dw;
			t.rmod.resize(t.C.size());
			for (size_t j = 0; j < t.C.size(); ++j)
			{
				t.rmod[j].resize(K);
				for (int k = 0; k < K; ++k) t.rmod[j][k] = 1.0 + 0.07 * std::sin(2.0 * kPi * t.s[j] / pitch - 2.0 * kPi * k / K);
			}
		}
		Tube(o, t, K, ti.square, slot, P.cable, vPer, &holes, std::clamp(ti.sheath, 0.05, 0.6));
		++out.cables;
		if (closed) continue;
		Cable fwd;
		fwd.C.assign(cb.C.begin(), cb.C.begin() + J);
		fwd.N.assign(cb.N.begin(), cb.N.begin() + J);
		fwd.B.assign(cb.B.begin(), cb.B.begin() + J);
		fwd.s.assign(cb.s.begin(), cb.s.begin() + J);
		fwd.scale = cb.scale;
		for (const Hole& h : holes) HoleInsides(o, p, ti, fwd, h, a, b, hw, ht, D, K, P.cable, vPer, down, out);
		Cable rev;
		rev.C.assign(fwd.C.rbegin(), fwd.C.rend());
		rev.N.assign(fwd.N.rbegin(), fwd.N.rend());
		rev.B.resize(J);
		rev.scale.assign(fwd.scale.rbegin(), fwd.scale.rend());
		for (int j = 0; j < J; ++j) rev.B[j] = -fwd.B[J - 1 - j];
		rev.s = Cum(rev.C);
		if ((mode1 == 1 || mode1 == 2) && strip1 > 0.0) Cores(o, p, ti, fwd, total - strip1, mode1 == 2, rng, down, hw, ht, K, P.cable, vPer);
		if ((mode0 == 1 || mode0 == 2) && strip0 > 0.0) Cores(o, p, ti, rev, total - strip0, mode0 == 2, rng, down, hw, ht, K, P.cable, vPer);
		for (int end = 0; end < 2; ++end)
		{
			if ((end ? mode1 : mode0) != 3) continue;
			const Cable& e = end ? fwd : rev;
			const double L = e.s.back();
			const double k = e.scale.back();
			                                         
			TubeIn bt;
			const double b0 = std::max(L - boot, 0.0);
			std::vector<double> sa{ b0 };
			for (double v : e.s) if (v > b0 + 1e-9 && v < L - 1e-9) sa.push_back(v);
			sa.push_back(L);
			for (double u : sa)
			{
				const V C = Lerp(e.s, e.C, u);
				const V Tl = Unit(Lerp(e.s, e.C, std::min(u + 1e-4, L)) - Lerp(e.s, e.C, std::max(u - 1e-4, 0.0)), V(1, 0, 0));
				const V Nn = Perp(Tl, Lerp(e.s, e.N, u));
				const double f = (u - b0) / std::max(L - b0, 1e-9);
				const double grow = p.type == 2 ? 1.08 + 0.4 * f : 1.12 + 0.55 * f * f;
				bt.C.push_back(C); bt.N.push_back(Nn); bt.B.push_back(Cross(Tl, Nn));
				bt.ha.push_back(std::max(ht, 0.75 * hw) * k * grow); bt.hb.push_back(hw * k * grow); bt.s.push_back(u);
			}
			Tube(o, bt, K, 2.0, PLUG, P.cable, vPer);
			const V Te = Unit(e.C.back() - e.C[e.C.size() - 2], V(1, 0, 0));
			const V Ne = Perp(Te, e.N.back());
			Connector(o, p.type, e.C.back(), Te, Ne, Cross(Te, Ne), hw * k, ht * k, D * k, K, P.cable);
			++out.connectors;
		}
	}

	                                                                  
	const int Kb = std::max(12, K0 + 4);
	for (size_t ci = 0; ci < clumps.size(); ++ci)
	{
		const Clump& cl = clumps[ci];
		const std::vector<int>& ids = clumpCables[ci];
		if (ids.empty()) continue;
		std::vector<std::vector<double>> cs;
		for (int id : ids) cs.push_back(Cum(cables[id].X, cables[id].closed));
		auto at = [&](double u, V& c, V& t, V& n, V& b, double& k) {
			c = V(0.0); n = V(0.0); k = 0.0;
			V tsum(0.0);
			for (size_t m = 0; m < ids.size(); ++m)
			{
				const Path& P = cables[ids[m]];
				std::vector<V> X = P.X, N = P.N;
				std::vector<double> sc = P.sc;
				if (P.closed) { X.push_back(X.front()); N.push_back(N.front()); sc.push_back(sc.front()); }
				const double s = u * cs[m].back();
				c = c + Lerp(cs[m], X, s);
				n = n + Lerp(cs[m], N, s);
				k += Lerp(cs[m], sc, s);
				tsum = tsum + Lerp(cs[m], X, std::min(s + 1e-3, cs[m].back())) - Lerp(cs[m], X, std::max(s - 1e-3, 0.0));
			}
			const double inv = 1.0 / ids.size();
			c = c * inv; k *= inv;
			t = Unit(tsum, V(1, 0, 0));
			n = Perp(t, n);
			b = Cross(t, n);
		};
		auto hull = [&](double u, const V& c, const V& n, const V& b, double k, double& hn, double& hb) {
			hn = cl.hullN * k; hb = cl.hullB * k;
			for (size_t m = 0; m < ids.size(); ++m)
			{
				const Path& P = cables[ids[m]];
				std::vector<V> X = P.X;
				if (P.closed) X.push_back(X.front());
				const V d = Lerp(cs[m], X, u * cs[m].back()) - c;
				hn = std::max(hn, std::abs(Dot(d, n)) + ht * k);
				hb = std::max(hb, std::abs(Dot(d, b)) + hw * k);
			}
		};
		const double len = cs[0].back();
		if (cl.tied && len > 0.0)
		{
			const int nt = std::min((int)std::floor(len / p.tieSpacing), 100000);
			const double start = 0.5 * (len - (nt - 1) * p.tieSpacing);
			for (int i = 0; i < nt; ++i)
			{
				const double u = (start + i * p.tieSpacing) / len;
				V c, t, n, b; double k, hn, hb;
				at(u, c, t, n, b, k);
				hull(u, c, n, b, k, hn, hb);
				Band(o, c, t, n, b, hn, hb, cl.square, std::max(0.12 * Dt, 0.0006), p.tieWidth, Kb, TIE, -1, false);
				++out.ties;
			}
		}
		if (p.clips)
		{
			for (size_t ai = 0; ai < cl.anchorU.size(); ++ai)
			{
				const double u = cl.anchorU[ai];
				if (!cables[ids[0]].closed && ((u <= 1e-9 && p.endStart) || (u >= 1.0 - 1e-9 && p.endEnd))) continue;
				V c, t, n, b; double k, hn, hb;
				at(std::clamp(u, 0.0, 1.0), c, t, n, b, k);
				hull(u, c, n, b, k, hn, hb);
				Band(o, c, t, n, b, hn, hb, cl.square, std::max(0.2 * Dt, 0.001), std::max(0.9 * Dw, 0.006), Kb, CLIP, -1, true);
				++out.clips;
			}
		}
	}
	return !out.faceSizes.empty();
}
PROJECT_DAMAGE_NS_MACRO_END
