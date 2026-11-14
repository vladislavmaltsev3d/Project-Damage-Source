// разрываем металлическую поверхность вокруг точек пробоя
// ProjectDamage metal tear generator - GPL-3.0-or-later; see LICENSE.
// Recipe "metal-tear-v1". See metal_tear.h and METAL_TEAR.md.
#include "gen_metal_tear.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace pdgen {
namespace {

struct Petal
{
	double a0 = 0, a1 = 0, ac = 0, half = 0;   // boundary angles, centre, half span
	double root = 0, len = 0;                  // root line distance from the centre, petal length
	double curl = 0, twist = 0, cup = 0;
	uint64_t seed = 0;
	std::vector<double> ta, tb;                // rolled profile: (along, out) at s = i * step
	double step = 0;
	V3 eIn, eLat, eN;                          // frame at the root centre (smooth sheet, no crumple)
};

struct Blast
{
	V2 c;
	double R = 0.1, holeR = 0.03;
	int side = 1;
	bool puncture = false;
	std::vector<double> bound;                 // n + 1 angles, bound[n] = bound[0] + 2 pi
	std::vector<double> slitEnd;               // n, per boundary
	std::vector<Petal> petals;
	double rootMean = 0, gapHalf = 0;
	uint64_t seed = 0;
};

struct Dent { V2 c; double a = 0.1, depth = 0.02; int side = -1; int lobes = 6; double rot = 0; uint64_t seed = 0; };
struct Tear { V2 a, b; double halfW = 0.01; int side = 1; uint64_t seed = 0; };
struct Bite { V2 c; double r = 0.1; uint64_t seed = 0; };

double WrapPi(double a)
{
	while (a > kPi) a -= 2 * kPi;
	while (a <= -kPi) a += 2 * kPi;
	return a;
}

class MetalBuilder
{
public:
	MetalBuilder(const MetalTearParams& p, double h) : P(p), h(h), seed(uint64_t(uint32_t(p.seed)) * 0x9E37ull + 0x4D45ull) {}

	const MetalTearParams& P;
	double h;
	uint64_t seed;
	std::vector<Blast> blasts;
	std::vector<Dent> dents;
	std::vector<Tear> tears;
	std::vector<Bite> bites;

	double HW() const { return 0.5 * P.width; }
	double HH() const { return 0.5 * P.height; }

	// ---------------------------------------------------------------- layout
	void MakeBlast(V2 c, double R, int side, int n, bool puncture, uint64_t s)
	{
		Blast b;
		b.c = c;
		b.R = R;
		b.side = side >= 0 ? 1 : -1;
		b.puncture = puncture;
		b.seed = s;
		Rng r(s, 11);
		b.holeR = R * Clamp((puncture ? 0.45 : 1.0) * P.holeSize * (1.0 + 0.3 * r.Signed()), 0.02, 0.9);
		const double a0 = r.Range(0, 2 * kPi);
		const double rnd = Sat(P.slitRandom);
		b.bound.resize(n + 1);
		for (int k = 0; k < n; ++k) b.bound[k] = a0 + 2 * kPi * (k + 0.5 * rnd * r.Signed() * 0.8) / n;
		std::sort(b.bound.begin(), b.bound.begin() + n);
		b.bound[n] = b.bound[0] + 2 * kPi;
		b.slitEnd.resize(n);
		for (int k = 0; k < n; ++k) b.slitEnd[k] = R * Clamp(1.0 + 0.45 * rnd * r.Signed(), 0.45, 1.6);
		// slit opening at the hole: share of the petal arc there
		b.gapHalf = 0.5 * Sat(P.petalGap) * b.holeR * (2 * kPi / n) * 0.9;
		b.petals.resize(n);
		double rootSum = 0;
		for (int k = 0; k < n; ++k)
		{
			Petal& pt = b.petals[k];
			pt.a0 = b.bound[k];
			pt.a1 = b.bound[k + 1];
			pt.ac = 0.5 * (pt.a0 + pt.a1);
			pt.half = 0.5 * (pt.a1 - pt.a0);
			const double e0 = b.slitEnd[k], e1 = b.slitEnd[(k + 1) % n];
			pt.root = std::min(e0, e1) * std::cos(std::min(pt.half, 1.4));
			pt.len = std::max(pt.root - 0.6 * b.holeR, 0.25 * pt.root);
			pt.seed = Hash(s, 101, uint64_t(k));
			Rng pr(pt.seed, 1);
			const double cv = Sat(P.curlRandom);
			pt.curl = (puncture ? 0.75 : 1.0) * P.petalCurl * kPi * Clamp(1.0 + cv * 0.8 * pr.Signed(), 0.15, 1.9);
			pt.twist = P.petalTwist * 0.9 * pr.Signed();
			pt.cup = 0.35 * P.petalCurl * (0.5 + pr.Next());
			// rolled centre line: hinge near the root + a curl that tightens toward the tip
			const int N = 96;
			const double sMax = pt.root + 2.0 * h;
			pt.step = sMax / N;
			pt.ta.assign(N + 1, 0.0);
			pt.tb.assign(N + 1, 0.0);
			double aa = 0, bb = 0;
			for (int i = 1; i <= N; ++i)
			{
				const double sm = (i - 0.5) * pt.step;
				const double x = sm / pt.len;
				const double phi = pt.curl * (0.32 * Smooth(x / 0.28) + 0.68 * std::pow(Sat(x), 1.7) + 0.25 * std::max(x - 1.0, 0.0));
				aa += std::cos(phi) * pt.step;
				bb += std::sin(phi) * pt.step;
				pt.ta[i] = aa;
				pt.tb[i] = bb;
			}
			rootSum += pt.root;
		}
		b.rootMean = rootSum / n;
		blasts.push_back(std::move(b));
	}

	bool FreeSpot(V2 c, double rad, double gap) const
	{
		for (const Blast& b : blasts)
			if (Len(c - b.c) < rad + b.R * 1.15 + gap) return false;
		return true;
	}

	void Layout(const std::vector<MetalBlastMarker>& markers)
	{
		Rng r(seed, 1);
		int mk = 0;
		for (const MetalBlastMarker& m : markers)
		{
			const int n = m.petals > 2 ? std::min(m.petals, 24) : r.Int(std::max(P.petalsMin, 3), std::max(P.petalsMax, std::max(P.petalsMin, 3)));
			MakeBlast(V2(m.x, m.y), std::max(m.radius, 3 * h), m.side, n, false, Hash(seed, 7, uint64_t(mk++)));
		}
		for (int i = 0; i < P.blasts; ++i)
			for (int tries = 0; tries < 40; ++tries)
			{
				const double R = r.Range(std::min(P.blastSizeMin, P.blastSizeMax), std::max(P.blastSizeMin, P.blastSizeMax));
				const double mx = std::max(HW() - R * 1.1, 0.0), my = std::max(HH() - R * 1.1, 0.0);
				const V2 c(r.Range(-mx, mx), r.Range(-my, my));
				if (!FreeSpot(c, R, 0.0) && tries < 39) continue;
				const int n = r.Int(std::max(P.petalsMin, 3), std::max(P.petalsMax, std::max(P.petalsMin, 3)));
				MakeBlast(c, std::max(R, 3 * h), P.blastSide, n, false, Hash(seed, 8, uint64_t(i)));
				break;
			}
		for (int i = 0; i < P.punctures; ++i)
			for (int tries = 0; tries < 30; ++tries)
			{
				const double R = std::max(r.Range(std::min(P.punctureSizeMin, P.punctureSizeMax), std::max(P.punctureSizeMin, P.punctureSizeMax)), 3.5 * h);
				const V2 c(r.Range(-HW() + 2 * R, HW() - 2 * R), r.Range(-HH() + 2 * R, HH() - 2 * R));
				if (!FreeSpot(c, R, R) && tries < 29) continue;
				MakeBlast(c, R, P.blastSide, r.Int(3, 5), true, Hash(seed, 9, uint64_t(i)));
				break;
			}
		for (int i = 0; i < P.dents; ++i)
			for (int tries = 0; tries < 30; ++tries)
			{
				Dent d;
				d.a = r.Range(std::min(P.dentSizeMin, P.dentSizeMax), std::max(P.dentSizeMin, P.dentSizeMax));
				d.c = V2(r.Range(-HW(), HW()), r.Range(-HH(), HH()));
				if (!FreeSpot(d.c, d.a * 0.7, 0.0) && tries < 29) continue;
				d.depth = P.dentDepth * d.a * (0.6 + 0.8 * r.Next());
				d.side = r.Chance(P.dentBack) ? 1 : -1;
				d.lobes = r.Int(5, 9);
				d.rot = r.Range(0, 2 * kPi);
				d.seed = Hash(seed, 21, uint64_t(i));
				dents.push_back(d);
				break;
			}
		for (int i = 0; i < P.tears; ++i)
			for (int tries = 0; tries < 30; ++tries)
			{
				Tear t;
				const double L = r.Range(std::min(P.tearLengthMin, P.tearLengthMax), std::max(P.tearLengthMin, P.tearLengthMax));
				const double ang = r.Range(0, kPi);
				const V2 d(std::cos(ang) * 0.5 * L, std::sin(ang) * 0.5 * L);
				const V2 c(r.Range(-HW() + 0.3 * L, HW() - 0.3 * L), r.Range(-HH() + 0.3 * L, HH() - 0.3 * L));
				if (!FreeSpot(c, 0.5 * L, 0.0) && tries < 29) continue;
				t.a = c - d;
				t.b = c + d;
				t.halfW = 0.5 * P.tearWidth * (0.6 + 0.8 * r.Next());
				t.side = r.Chance(0.5) ? 1 : -1;
				t.seed = Hash(seed, 31, uint64_t(i));
				tears.push_back(t);
				break;
			}
		for (int i = 0; i < P.edgeBites; ++i)
		{
			Bite b;
			b.r = P.biteSize * (0.6 + 0.8 * r.Next());
			const int sideIx = r.Int(0, 3);
			const double t = r.Range(-1, 1);
			const double dip = b.r * r.Range(0.1, 0.6);
			if (sideIx == 0) b.c = V2(t * HW(), -HH() - dip);
			else if (sideIx == 1) b.c = V2(t * HW(), HH() + dip);
			else if (sideIx == 2) b.c = V2(-HW() - dip, t * HH());
			else b.c = V2(HW() + dip, t * HH());
			b.seed = Hash(seed, 41, uint64_t(i));
			bites.push_back(b);
		}
	}

	// ---------------------------------------------------------------- fields
	double JagScale() const { return std::max(3.0 * h, 0.004); }

	// sheet border (positive outside), torn
	double BorderField(double u, double v) const
	{
		double f = std::max(std::abs(u) - HW(), std::abs(v) - HH());
		if (P.edgeTear > 0.0)
		{
			const double s = std::max(P.edgeTearScale, 2 * h);
			const double lobes = 0.55 + 0.45 * Fbm2(u / s, v / s, seed + 3, 3);
			const double teeth = 0.25 * Sat(P.jagged + 0.2) * Noise2(u / JagScale(), v / JagScale(), seed + 4);
			f += P.edgeTear * (lobes + teeth);
		}
		for (const Bite& b : bites)
		{
			const V2 q(u, v);
			const double rr = b.r * (1.0 + 0.3 * Fbm2(u / (b.r * 0.5), v / (b.r * 0.5), b.seed, 3)) +
				0.2 * P.jagged * JagScale() * Noise2(u / JagScale(), v / JagScale(), b.seed + 1);
			f = std::max(f, rr - Len(q - b.c));
		}
		return f;
	}

	// removed by blasts and tears (negative inside the removed area)
	double HoleField(double u, double v) const
	{
		const V2 q(u, v);
		double g = 1e30;
		const double js = JagScale();
		for (const Blast& b : blasts)
		{
			const V2 d = q - b.c;
			const double r = Len(d);
			if (r > b.R * 1.8 + 4 * h) { g = std::min(g, r - b.R * 1.8); continue; }
			const double rough = 1.0 + 0.35 * P.jagged * Fbm2(u / (b.holeR * 0.7), v / (b.holeR * 0.7), b.seed + 2, 3);
			double gb = r - b.holeR * rough + 0.25 * P.jagged * js * Noise2(u / js, v / js, b.seed + 3);
			const int n = int(b.slitEnd.size());
			const double minHalf = 0.75 * h;
			for (int k = 0; k < n; ++k)
			{
				const V2 dir(std::cos(b.bound[k]), std::sin(b.bound[k]));
				// slits bend slightly so the tears are not ruler-straight
				const double along = Dot(d, dir);
				if (along < 0.0 || along > b.slitEnd[k] + 4 * h) continue;
				const V2 nrm(-dir.y, dir.x);
				const double wob = 0.05 * b.R * Sat(P.slitRandom + 0.2) * Noise2(along / (0.35 * b.R), double(k) * 7.31, b.seed + 5);
				const double across = std::abs(Dot(d, nrm) - wob * Sat(along / b.slitEnd[k]));
				const double t = Sat(along / b.slitEnd[k]);
				const double wHole = std::max(b.gapHalf * (b.holeR > 0 ? along / std::max(b.holeR, 1e-9) : 1.0), minHalf);
				double half = std::max(Lerp(wHole, minHalf, std::pow(t, 0.7)), minHalf);
				half *= 1.0 + 0.6 * P.jagged * Noise2(u / js, v / js, b.seed + 6 + uint64_t(k));
				const double ext = along - b.slitEnd[k];
				const double gs = ext <= 0.0 ? across - half : std::hypot(across, ext) - minHalf;
				gb = std::min(gb, gs);
			}
			g = std::min(g, gb);
		}
		for (const Tear& t : tears)
		{
			double s;
			const double dist = SegDist(q, t.a, t.b, &s);
			const double env = std::sqrt(std::max(0.0, 1.0 - (2 * s - 1) * (2 * s - 1)));
			double half = std::max(t.halfW * env, 0.7 * h) * (1.0 + 0.6 * P.jagged * Noise2(u / js, v / js, t.seed));
			g = std::min(g, dist - half);
		}
		return g;
	}

	double Field(double u, double v) const { return std::max(BorderField(u, v), -HoleField(u, v)); }

	// ---------------------------------------------------------------- deformation
	struct Masks { double dent = 0, scorch = 0, crumple = 0; };

	// height of the flat sheet (before the bend), with masks
	double Height(double u, double v, Masks* m = nullptr, bool smooth = false) const
	{
		const V2 q(u, v);
		double z = 0;
		if (P.corrugation > 0.0 && P.corrugationPeriod > 0.0)
		{
			const double ph = 2 * kPi * u / P.corrugationPeriod;
			double w = std::sin(ph);
			if (P.corrugationShape == 1) w = Clamp(1.7 * w, -1.0, 1.0);
			z += 0.5 * P.corrugation * w;
		}
		if (P.warp > 0.0) z += P.warp * Fbm2(u / std::max(P.warpScale, 1e-3), v / std::max(P.warpScale, 1e-3), seed + 11, 3);
		double dentMask = 0, crumpleMask = 0, scorch = 0;
		for (const Dent& d : dents)
		{
			const V2 e = q - d.c;
			const double r = Len(e) / d.a;
			if (r > 1.4) continue;
			const double ang = std::atan2(e.y, e.x) + d.rot;
			const double rr = r * (1.0 + 0.12 * Noise2(std::cos(ang) * 2.0, std::sin(ang) * 2.0, d.seed));
			const double bowl = rr < 1 ? (1 - rr * rr) * (1 - rr * rr) : 0.0;
			const double cone = rr < 1 ? std::pow(1 - rr, 1.15) * Smooth(rr / 0.12 + 0.35) + (rr < 0.12 ? 0.0 : 0.0) : 0.0;
			double prof = Lerp(bowl, cone, Sat(P.dentSharpness));
			prof -= 0.07 * std::exp(-((rr - 1.05) / 0.13) * ((rr - 1.05) / 0.13));   // raised rim
			const double crease = 1.0 - std::abs(std::sin(0.5 * d.lobes * ang + 1.7 * Noise2(rr * 2.0, 0.0, d.seed + 1)));
			prof *= 1.0 + P.dentCreases * 0.35 * (crease * crease - 0.4) * Smooth(rr / 0.35) * (rr < 1 ? 1.0 : 0.0);
			z += d.side * d.depth * prof;
			dentMask = std::max(dentMask, Sat(prof));
			crumpleMask = std::max(crumpleMask, rr < 1.1 ? 0.3 * Smooth((1.1 - rr) / 0.4) : 0.0);
		}
		for (const Tear& t : tears)
		{
			double s;
			const double dist = SegDist(q, t.a, t.b, &s);
			const double env = std::pow(std::max(std::sin(kPi * s), 0.0), 0.7);
			const double reach = std::max(t.halfW * 3.0, 4 * h);
			z += t.side * P.tearOpen * t.halfW * 2.2 * env * std::exp(-dist / reach);
			crumpleMask = std::max(crumpleMask, env * std::exp(-dist / (2 * reach)));
		}
		for (const Blast& b : blasts)
		{
			const double r = Len(q - b.c);
			const double Rb = std::max(P.bulgeRadius, 1.05) * b.rootMean;
			if (r < Rb) z += b.side * P.bulge * b.R * (b.puncture ? 0.4 : 1.0) * (1.0 - SmoothStep(b.rootMean * 0.9, Rb, r));
			if (r < Rb * 1.2 && r > 1e-9 && !smooth)
			{
				// radial buckles: the stretched ring around the hole folds into spokes
				const double ang = std::atan2(v - b.c.y, u - b.c.x);
				const double k = std::max(6.0, std::round(2 * kPi * b.rootMean / std::max(3.0 * P.crumpleSize, 4 * h)));
				const double ph = k * ang + 2.5 * Noise2(r / (0.5 * b.R), ang * 1.3, b.seed + 21);
				const double spoke = 1.0 - 2.0 * std::abs(std::sin(0.5 * ph));   // triangle-ish wave, sharp crests
				const double env = Smooth((r - b.rootMean * 0.85) / (0.25 * b.R)) * (1.0 - SmoothStep(b.rootMean, Rb * 1.2, r));
				z += b.side * P.crumple * P.crumpleSize * 0.35 * spoke * env * (b.puncture ? 0.3 : 1.0);
			}
			crumpleMask = std::max(crumpleMask, 0.4 * (1.0 - SmoothStep(b.R * 0.8, Rb * 1.1, r)));
			scorch = std::max(scorch, std::exp(-(r / (1.3 * b.R)) * (r / (1.3 * b.R))) * (0.75 + 0.25 * Fbm2(u / (0.3 * b.R), v / (0.3 * b.R), b.seed + 9, 3)));
		}
		// torn border bent out of plane
		const double fb = BorderField(u, v);
		if (P.edgeCurl != 0.0 && P.edgeCurlWidth > 0.0)
		{
			const double db = std::max(-fb, 0.0);
			if (db < P.edgeCurlWidth)
			{
				const double t = 1.0 - db / P.edgeCurlWidth;
				const double side = Fbm2(u / (P.edgeCurlWidth * 4), v / (P.edgeCurlWidth * 4), seed + 13, 2);
				z += P.edgeCurl * t * t * Clamp(side * 1.6, -1.0, 1.0);
				crumpleMask = std::max(crumpleMask, t * 0.7);
			}
		}
		if (P.crumple > 0.0 && crumpleMask > 0.0 && !smooth)
		{
			const double cs = std::max(P.crumpleSize, 2 * h);
			z += P.crumple * cs * 0.25 * crumpleMask * (Ridged2(u / cs, v / cs, seed + 17, 3) - 0.55) * 2.0;
		}
		if (m) { m->dent = dentMask; m->crumple = crumpleMask; m->scorch = scorch; }
		return z;
	}

	// flat (u, v, z) -> bent sheet
	V3 BendMap(double u, double v, double z) const
	{
		if (std::abs(P.bend) < 1e-9) return V3(u, v, z);
		const double a = P.bendAngle * kPi / 180.0;
		const double ca = std::cos(a), sa = std::sin(a);
		const double x = u * ca + v * sa, y = -u * sa + v * ca;
		const double R = 1.0 / P.bend;
		const double th = x / R;
		const double X = (R - z) * std::sin(th), Z = R - (R - z) * std::cos(th);
		return V3(X * ca - y * sa, X * sa + y * ca, Z);
	}

	V3 G(double u, double v, Masks* m = nullptr) const { return BendMap(u, v, Height(u, v, m)); }
	V3 GSmooth(double u, double v) const { return BendMap(u, v, Height(u, v, nullptr, true)); }

	// one frame per petal at its root centre, from the smooth sheet
	void PetalFrames()
	{
		for (Blast& b : blasts)
			for (Petal& pt : b.petals)
			{
				const V2 rc(std::cos(pt.ac), std::sin(pt.ac)), tc(-rc.y, rc.x);
				const V2 Q = b.c + rc * pt.root;
				const double eps = std::max(0.03 * b.R, h);
				const V3 g0 = GSmooth(Q.x, Q.y);
				pt.eIn = Norm(GSmooth(Q.x - rc.x * eps, Q.y - rc.y * eps) - g0, V3(-rc.x, -rc.y, 0));
				const V3 lat = Norm(GSmooth(Q.x + tc.x * eps, Q.y + tc.y * eps) - GSmooth(Q.x - tc.x * eps, Q.y - tc.y * eps), V3(tc.x, tc.y, 0));
				pt.eN = Norm(Cross(lat, pt.eIn), V3(0, 0, 1));   // (tc x -rc) = +Z on a flat sheet
				pt.eLat = Norm(Cross(pt.eIn, pt.eN), lat);
			}
	}

	// a petal point: returns false when (u,v) is not on a petal
	bool PetalPoint(double u, double v, V3& out, double& petalMask) const
	{
		const V2 q(u, v);
		for (const Blast& b : blasts)
		{
			const V2 d = q - b.c;
			const double r = Len(d);
			if (r > b.R * 1.7) continue;
			const double ang = std::atan2(d.y, d.x);
			for (const Petal& pt : b.petals)
			{
				const double D = WrapPi(ang - pt.ac);
				if (std::abs(D) > pt.half + 1e-9) continue;
				const double s = pt.root - r * std::cos(D);
				if (s <= 0.0) return false;
				const double w = r * std::sin(D);
				const V2 rc(std::cos(pt.ac), std::sin(pt.ac)), tc(-rc.y, rc.x);
				const V2 Q = b.c + rc * pt.root + tc * w;
				const V3 g0 = G(Q.x, Q.y);
				const V3& eIn = pt.eIn;
				const V3& eLat = pt.eLat;
				const V3& eN = pt.eN;
				// rolled profile
				const double fi = s / pt.step;
				const int i0 = std::min(int(fi), int(pt.ta.size()) - 2);
				const double fr = Clamp(fi - i0, 0.0, 1.0);
				const double a = Lerp(pt.ta[i0], pt.ta[i0 + 1], fr) + std::max(fi - (pt.ta.size() - 1), 0.0) * pt.step;
				double bz = Lerp(pt.tb[i0], pt.tb[i0 + 1], fr);
				const double x = s / pt.len;
				bz += pt.cup * w * w / std::max(pt.len, 1e-9) * Smooth(x / 0.5);
				{
					// folds running along the petal from root to tip, crumple near the torn tip
					const double fw = std::max(pt.len * 0.12, 3 * h);
					const double ph = w / fw + 0.8 * Noise2(s / (pt.len * 0.5), 0.0, pt.seed + 3);
					const double fold = 1.0 - 2.0 * std::abs(ph - std::floor(ph) - 0.5) * 2.0;
					bz += P.petalCrumple * fw * 0.35 * fold * Smooth(x * 1.5);
					bz += P.petalCrumple * pt.len * 0.04 * Fbm2(s / (pt.len * 0.2), w / (pt.len * 0.2), pt.seed, 3) * Smooth((x - 0.4) * 2.0);
				}
				const double tw = pt.twist * x;
				const double side = double(b.side);
				out = g0 + eIn * a + eN * (side * bz) + (eLat * (std::cos(tw) - 1.0) + eN * (side * std::sin(tw))) * w;
				petalMask = 1.0;
				return true;
			}
		}
		return false;
	}
};

} // namespace

bool BuildMetalTear(const MetalTearParams& pin, const std::vector<MetalBlastMarker>& markers, MetalTearResult& out)
{
	out = MetalTearResult();
	MetalTearParams p = pin;
	p.width = std::max(p.width, 1e-4);
	p.height = std::max(p.height, 1e-4);
	double h = std::max(p.resolution, 1e-5);
	const double cap = std::max(p.maxVerts, 1000) / (p.thickness > 0 ? 2.15 : 1.05);
	if (p.width * p.height / (h * h) > cap) h = std::sqrt(p.width * p.height / cap);
	out.resolution = h;
	MetalBuilder B(p, h);
	B.Layout(markers);
	B.PetalFrames();
	Domain2D dom;
	const double m = 2 * h;
	MeshDomain2D([&](double u, double v) { return B.Field(u, v); }, -B.HW() - m, B.HW() + m, -B.HH() - m, B.HH() + m, h, dom,
		std::max(p.minPiece, 4 * h * h));
	if (dom.tris.empty()) return false;
	const size_t n = dom.rest.size();
	std::vector<V3> mid(n);
	std::vector<float> dent(n), scorch(n), petal(n), torn(n);
	for (size_t i = 0; i < n; ++i)
	{
		const double u = dom.rest[i].x, v = dom.rest[i].y;
		MetalBuilder::Masks mk;
		double pm = 0.0;
		V3 pp;
		if (B.PetalPoint(u, v, pp, pm)) { mid[i] = pp; B.Height(u, v, &mk); }
		else mid[i] = B.G(u, v, &mk);
		dent[i] = float(mk.dent);
		scorch[i] = float(Sat(mk.scorch));
		petal[i] = float(pm);
		// torn mask: the distance to a torn border (the straight border counts when it is torn)
		double fd = -B.HoleField(u, v);
		for (const Bite& b : B.bites) fd = std::max(fd, b.r - Len(V2(u, v) - b.c));
		if (p.edgeTear > 0.0) fd = std::max(fd, B.BorderField(u, v));
		torn[i] = float(1.0 - Sat(-fd / std::max(p.tornBand, 1e-6)));
	}
	SheetOptions so;
	so.thickness = std::max(p.thickness, 0.0);
	so.uvScale = p.uvScale;
	so.uvU0 = -B.HW();
	so.uvV0 = -B.HH();
	SolidifySheet(dom, mid, so, out.mesh);
	// bend mask from the turning of the normals between neighbours
	std::vector<float> bendv(n, 0.0f);
	{
		const std::vector<V3> nrm = VertexNormals(mid, dom.tris);
		for (const auto& t : dom.tris)
			for (int e = 0; e < 3; ++e)
			{
				const int a = t[e], b = t[(e + 1) % 3];
				const double len = Len(mid[a] - mid[b]);
				if (len <= 0) continue;
				const double ang = std::acos(Clamp(Dot(nrm[a], nrm[b]), -1.0, 1.0)) / len * h;   // radians per grid step
				const float v = float(Sat(ang / 0.35));
				bendv[a] = std::max(bendv[a], v);
				bendv[b] = std::max(bendv[b], v);
			}
	}
	const size_t copies = so.thickness > 0 ? 2 : 1;
	auto put = [&](const char* name, const std::vector<float>& src)
	{
		std::vector<float>& a = out.mesh.Attr(name);
		for (size_t c = 0; c < copies; ++c)
			for (size_t i = 0; i < n; ++i) a[c * n + i] = src[i];
	};
	put("metal_torn", torn);
	put("metal_dent", dent);
	put("metal_scorch", scorch);
	put("metal_bend", bendv);
	put("metal_petal", petal);
	out.mesh.FinishAttrs();
	for (const Blast& b : B.blasts)
	{
		if (b.puncture) out.punctures++;
		else out.blasts++;
		out.petals += int(b.petals.size());
	}
	out.tears = int(B.tears.size());
	out.dents = int(B.dents.size());
	out.bites = int(B.bites.size());
	out.ok = true;
	return true;
}

// ===========================================================================
// on an object

namespace {

struct MeshSite
{
	int kind = 0;                       // 0 blast, 1 puncture, 2 dent, 3 tear
	V3 P0, N, T, B;
	double size = 0.1, reach = 0.1;
	int side = 1, petals = 0;
	uint64_t seed = 0;
	std::unique_ptr<MetalTearParams> prm;
	std::unique_ptr<MetalBuilder> b;
};

struct VSite { int32_t s; double u, v, g; };

// every site works on an endless flat sheet of its own: no border effects
MetalTearParams SiteParams(const MetalTearParams& p)
{
	MetalTearParams q = p;
	q.width = q.height = 1e6;
	q.edgeTear = 0; q.edgeBites = 0; q.edgeCurl = 0; q.warp = 0; q.corrugation = 0; q.bend = 0;
	q.blasts = q.punctures = q.dents = q.tears = 0;
	return q;
}

void BuildSite(MeshSite& st, const MetalTearParams& p, double h)
{
	Rng fr(st.seed, 3);
	const double ang = fr.Range(0, 2 * kPi);
	const V3 ax = std::abs(st.N.x) < 0.9 ? V3(1, 0, 0) : V3(0, 1, 0);
	const V3 t0 = Norm(Cross(st.N, ax)), b0 = Cross(st.N, t0);
	st.T = t0 * std::cos(ang) + b0 * std::sin(ang);
	st.B = Cross(st.N, st.T);
	st.prm = std::make_unique<MetalTearParams>(SiteParams(p));
	st.b = std::make_unique<MetalBuilder>(*st.prm, h);
	MetalBuilder& B = *st.b;
	Rng r(st.seed, 5);
	if (st.kind <= 1)
	{
		B.MakeBlast(V2(0, 0), std::max(st.size, 3 * h), st.side, st.petals, st.kind == 1, st.seed);
		const Blast& b = B.blasts.back();
		const double Rb = std::max(p.bulgeRadius, 1.05) * b.rootMean;
		st.reach = std::max(1.85 * b.R, 1.25 * Rb) + 3 * h;
	}
	else if (st.kind == 2)
	{
		Dent d;
		d.a = st.size;
		d.depth = p.dentDepth * d.a * (0.6 + 0.8 * r.Next());
		d.side = st.side;
		d.lobes = r.Int(5, 9);
		d.rot = r.Range(0, 2 * kPi);
		d.seed = Hash(st.seed, 21);
		B.dents.push_back(d);
		st.reach = 1.45 * d.a + 2 * h;
	}
	else
	{
		Tear t;
		t.a = V2(-0.5 * st.size, 0);
		t.b = V2(0.5 * st.size, 0);
		t.halfW = 0.5 * p.tearWidth * (0.6 + 0.8 * r.Next());
		t.side = st.side;
		t.seed = Hash(st.seed, 31);
		B.tears.push_back(t);
		st.reach = 0.5 * st.size + 5 * std::max(t.halfW * 3.0, 4 * h) + 2 * h;
	}
	B.PetalFrames();
}

} // namespace

// основной вход: собираем геометрию по переданным настройкам
bool BuildMetalTearOnMesh(const MetalTearParams& pin, const MeshInput& in, const std::vector<MetalMeshMarker>& markers,
	int32_t sourceMaterials, MetalTearResult& out)
{
	out = MetalTearResult();
	if (in.points.empty() || in.faceSizes.empty()) return false;
	MetalTearParams p = pin;
	double h = std::max(p.resolution, 1e-5);
	const int32_t matBack = std::max(sourceMaterials, 1), matSide = matBack + 1;
	const uint64_t seed = uint64_t(uint32_t(p.seed)) * 0x9E37ull + 0x4D4Full;
	const SurfaceTris surf = MakeSurfaceTris(in);
	if (surf.total <= 0.0) return false;
	const std::vector<V3>& P = in.points;
	std::vector<V3> cen(surf.t.size());
	std::vector<double> triRad(surf.t.size());
	for (size_t i = 0; i < surf.t.size(); ++i)
	{
		cen[i] = (P[surf.t[i][0]] + P[surf.t[i][1]] + P[surf.t[i][2]]) * (1.0 / 3.0);
		triRad[i] = std::max({ Len(P[surf.t[i][0]] - cen[i]), Len(P[surf.t[i][1]] - cen[i]), Len(P[surf.t[i][2]] - cen[i]) });
	}
	// surface normal around a point and how flat it is there (smallest cosine to the mean)
	auto frameAt = [&](const V3& q, double rad, V3& n) -> double
	{
		// the triangles reaching into the disc, weighted by about how much of them is in it
		V3 acc;
		std::vector<size_t> near;
		for (size_t i = 0; i < surf.t.size(); ++i)
		{
			if (surf.area[i] <= 0 || Len(cen[i] - q) > rad + triRad[i]) continue;
			const V3 c = ClosestOnTriangle(q, P[surf.t[i][0]], P[surf.t[i][1]], P[surf.t[i][2]]);
			if (Len(c - q) >= rad) continue;
			near.push_back(i);
			acc += surf.normal[i] * std::min(surf.area[i], kPi * rad * rad);
		}
		n = Norm(acc, V3(0, 0, 0));
		if (Len(n) < 0.5) return -1.0;
		double mn = 1.0;
		for (size_t i : near) mn = std::min(mn, Dot(surf.normal[i], n));
		return mn;
	};
	auto closest = [&](const V3& q, V3& best, V3& nrm)
	{
		double bd = 1e300;
		for (size_t i = 0; i < surf.t.size(); ++i)
		{
			const V3 c = ClosestOnTriangle(q, P[surf.t[i][0]], P[surf.t[i][1]], P[surf.t[i][2]]);
			const double d = Len(c - q);
			if (d < bd) { bd = d; best = c; nrm = surf.normal[i]; }
		}
	};
	std::vector<double> cum(surf.t.size());
	{
		double a = 0;
		for (size_t i = 0; i < surf.t.size(); ++i) { a += surf.area[i]; cum[i] = a; }
	}
	auto sample = [&](Rng& r) -> V3
	{
		const double x = r.Next() * surf.total;
		const size_t i = std::min(size_t(std::lower_bound(cum.begin(), cum.end(), x) - cum.begin()), surf.t.size() - 1);
		double a = r.Next(), b = r.Next();
		if (a + b > 1) { a = 1 - a; b = 1 - b; }
		const auto& t = surf.t[i];
		return P[t[0]] + (P[t[1]] - P[t[0]]) * a + (P[t[2]] - P[t[0]]) * b;
	};
	// ---- sites: markers, then random ones where the surface is flat enough
	std::vector<MeshSite> sites;
	Rng r(seed, 1);
	int mk = 0;
	for (const MetalMeshMarker& m : markers)
	{
		if (sites.size() >= 256) break;
		MeshSite st;
		st.kind = 0;
		V3 nrm;
		closest(m.p, st.P0, nrm);
		V3 n2;
		st.N = frameAt(st.P0, std::max(0.4 * m.radius, 2 * h), n2) > -0.5 && Len(n2) > 0.5 ? n2 : nrm;
		st.size = std::max(m.radius, 3 * h);
		st.side = m.side != 0 ? (m.side > 0 ? 1 : -1) : (Dot(m.p - st.P0, st.N) > 0 ? -1 : 1);
		st.petals = m.petals > 2 ? std::min(m.petals, 24) : r.Int(std::max(p.petalsMin, 3), std::max(p.petalsMax, std::max(p.petalsMin, 3)));
		st.seed = Hash(seed, 7, uint64_t(mk++));
		sites.push_back(std::move(st));
	}
	auto roughReach = [&](int kind, double size) { return kind <= 1 ? 2.2 * size : (kind == 2 ? 1.45 * size : 0.5 * size + 0.05 * size); };
	auto place = [&](int kind, double size, uint64_t s) -> bool
	{
		const double rr = roughReach(kind, size);
		V3 bestP, bestN;
		double bestFlat = -2.0;
		for (int tries = 0; tries < 24; ++tries)
		{
			const V3 q = sample(r);
			bool free = true;
			for (const MeshSite& o : sites)
				if (Len(o.P0 - q) < 0.55 * (rr + roughReach(o.kind, o.size))) { free = false; break; }
			if (!free) continue;
			V3 n;
			const double flat = frameAt(q, std::max(kind == 3 ? 0.5 * size : 0.8 * size, 2 * h), n);
			if (flat > bestFlat) { bestFlat = flat; bestP = q; bestN = n; }
			if (flat > 0.8) break;
		}
		if (bestFlat < 0.75) return false;   // no holes across an edge or a sharp bend
		MeshSite st;
		st.kind = kind;
		st.P0 = bestP;
		st.N = bestN;
		st.size = size;
		st.seed = s;
		Rng sr(s, 2);
		if (kind <= 1) { st.side = p.blastSide >= 0 ? 1 : -1; st.petals = kind == 1 ? sr.Int(3, 5) : sr.Int(std::max(p.petalsMin, 3), std::max(p.petalsMax, std::max(p.petalsMin, 3))); }
		else if (kind == 2) st.side = sr.Chance(p.dentBack) ? 1 : -1;
		else st.side = sr.Chance(0.5) ? 1 : -1;
		sites.push_back(std::move(st));
		return true;
	};
	auto rng = [&](double a, double b) { return r.Range(std::min(a, b), std::max(a, b)); };
	for (int i = 0; i < std::min(p.blasts, 64); ++i) place(0, rng(p.blastSizeMin, p.blastSizeMax), Hash(seed, 8, uint64_t(i)));
	for (int i = 0; i < std::min(p.punctures, 1024); ++i) place(1, std::max(rng(p.punctureSizeMin, p.punctureSizeMax), 3.5 * h), Hash(seed, 9, uint64_t(i)));
	for (int i = 0; i < std::min(p.dents, 256); ++i) place(2, rng(p.dentSizeMin, p.dentSizeMax), Hash(seed, 21, uint64_t(i)));
	for (int i = 0; i < std::min(p.tears, 128); ++i) place(3, rng(p.tearLengthMin, p.tearLengthMax), Hash(seed, 31, uint64_t(i)));
	for (MeshSite& st : sites) BuildSite(st, p, h);
	// vertex cap: the refined area is about the sites' discs
	{
		double area = 0;
		for (const MeshSite& st : sites) area += kPi * st.reach * st.reach;
		area = std::min(area, surf.total);
		const double cap = std::max(p.maxVerts, 1000) / (p.thickness > 0 ? 2.2 : 1.1);
		if (area / (0.6 * h * h) > cap)
		{
			h = std::sqrt(area / (0.6 * cap));
			for (MeshSite& st : sites) BuildSite(st, p, h);
		}
	}
	out.resolution = h;
	// ---- the faces near a site are edited, the others kept
	EditMesh m = MakeEditMesh(in, [&](const std::vector<int32_t>& idx)
	{
		V3 c;
		for (int32_t v : idx) c += P[v];
		c = c * (1.0 / idx.size());
		double rad = 0;
		for (int32_t v : idx) rad = std::max(rad, Len(P[v] - c));
		for (const MeshSite& st : sites)
			if (Len(c - st.P0) < st.reach + rad + h) return true;
		return false;
	});
	Refine(m, [&](int32_t a, int32_t b)
	{
		const double len = Len(m.p[a] - m.p[b]);
		if (len <= 1.3 * h) return false;
		const V3 c = (m.p[a] + m.p[b]) * 0.5;
		for (const MeshSite& st : sites)
			if (Len(c - st.P0) < st.reach + 0.5 * len) return true;
		return false;
	}, size_t(std::max(p.maxVerts, 1000)) * 2 + m.tris.size());
	// ---- local coordinates of each site: geodesic polar (u, v)
	const size_t n0 = m.p.size();
	std::vector<std::vector<VSite>> loc(n0);
	{
		const auto vt = VertexTriangles(m);
		for (size_t s = 0; s < sites.size(); ++s)
		{
			const MeshSite& st = sites[s];
			// the distance over the surface only decides what belongs to the site (not the
			// other side of a thin wall); the coordinates use the straight distance, which
			// stays round on any triangulation
			const std::vector<double> d = SurfaceDistance(m, vt, st.P0, st.reach * 1.4);
			for (size_t v = 0; v < n0; ++v)
			{
				if (d[v] > st.reach * 1.35) continue;
				const V3 q = m.p[v] - st.P0;
				const double e = Len(q);
				if (e > st.reach) continue;
				const double x = Dot(q, st.T), y = Dot(q, st.B), l = std::hypot(x, y);
				const double k = l > 1e-12 ? e / l : 0.0;
				loc[v].push_back({ int32_t(s), x * k, y * k, e });
			}
		}
	}
	auto lerpLoc = [](const std::vector<VSite>& a, const std::vector<VSite>& b, double t)
	{
		std::vector<VSite> o;
		for (const VSite& x : a)
			for (const VSite& y : b)
				if (x.s == y.s) { o.push_back({ x.s, x.u + (y.u - x.u) * t, x.v + (y.v - x.v) * t, x.g + (y.g - x.g) * t }); break; }
		if (o.empty()) o = t < 0.5 ? a : b;
		return o;
	};
	auto holeAt = [&](const std::vector<VSite>& L) -> double
	{
		double f = 1e30;
		for (const VSite& e : L)
		{
			const MeshSite& st = sites[e.s];
			if (st.kind == 2) continue;
			f = std::min(f, st.b->HoleField(e.u, e.v));
		}
		return f;
	};
	std::vector<double> f(n0);
	for (size_t v = 0; v < n0; ++v) f[v] = holeAt(loc[v]);
	auto crossing = [&](int32_t a, int32_t b) -> double
	{
		const auto& A = loc[a];
		const auto& Bv = loc[b];
		return RootOnEdge([&](double t) { return holeAt(lerpLoc(A, Bv, t)); }, f[a], f[b]);
	};
	{
		const auto moves = SnapToContour(m, f, crossing);
		std::vector<std::vector<VSite>> snap(moves.size());
		for (size_t k = 0; k < moves.size(); ++k) snap[k] = lerpLoc(loc[moves[k].v], loc[moves[k].toward], moves[k].t);
		for (size_t k = 0; k < moves.size(); ++k) loc[moves[k].v] = snap[k];
	}
	EditMesh cm = ClipSide(m, f, +1, crossing);
	loc.resize(cm.p.size());
	f.resize(cm.p.size(), 0.0);
	for (size_t v = n0; v < cm.p.size(); ++v)
	{
		const auto& o = cm.origin[v];
		loc[v] = lerpLoc(loc[o.a], loc[o.b], o.t);
		f[v] = 0.0;
	}
	DropSmallPieces(cm, std::max(p.minPiece, 4 * h * h), true);
	// ---- deform
	const size_t n = cm.p.size();
	const std::vector<V3> nrm0 = EditNormals(cm);
	std::vector<float> dent(n, 0.0f), scorch(n, 0.0f), petal(n, 0.0f), torn(n, 0.0f);
	std::vector<V3> disp(n);
	for (size_t v = 0; v < n; ++v)
	{
		if (loc[v].empty()) continue;
		for (const VSite& e : loc[v])
		{
			const MeshSite& st = sites[e.s];
			const double w = 1.0 - SmoothStep(0.8 * st.reach, st.reach, e.g);
			if (w <= 0.0) continue;
			MetalBuilder::Masks mkk;
			V3 pp;
			double pm = 0.0;
			if (st.b->PetalPoint(e.u, e.v, pp, pm))
			{
				const V3 d = pp - V3(e.u, e.v, 0.0);
				disp[v] += st.T * d.x + st.B * d.y + st.N * d.z;
				st.b->Height(e.u, e.v, &mkk);
			}
			else
			{
				const double z = st.b->Height(e.u, e.v, &mkk);
				const V3 nv = Len(nrm0[v]) > 0.5 ? nrm0[v] : st.N;
				disp[v] += nv * (z * w);
			}
			dent[v] = std::max(dent[v], float(mkk.dent * w));
			scorch[v] = std::max(scorch[v], float(Sat(mkk.scorch) * w));
			petal[v] = std::max(petal[v], float(pm));
		}
		torn[v] = float(1.0 - Sat(f[v] / std::max(p.tornBand, 1e-6)));
	}
	for (size_t v = 0; v < n; ++v) cm.p[v] += disp[v];
	// bend mask from the turning of the normals along the edited edges
	std::vector<float> bendv(n, 0.0f);
	{
		const std::vector<V3> nr = EditNormals(cm);
		for (const EditTri& t : cm.tris)
			for (int e = 0; e < 3; ++e)
			{
				const int32_t a = t.v[e], b = t.v[(e + 1) % 3];
				const double len = Len(cm.p[a] - cm.p[b]);
				if (len <= 0) continue;
				const double ang = std::acos(Clamp(Dot(nr[a], nr[b]), -1.0, 1.0)) / len * h;
				const float val = float(Sat(ang / 0.35));
				bendv[a] = std::max(bendv[a], val);
				bendv[b] = std::max(bendv[b], val);
			}
	}
	// ---- output: the outer surface, then the inner skin and the walls
	const std::vector<int32_t> map = EmitEditMesh(cm, out.mesh);
	std::vector<int32_t> back(out.mesh.points.size(), -1);
	for (size_t v = 0; v < n; ++v)
		if (map[v] >= 0) back[map[v]] = int32_t(v);
	for (int32_t c : SplitPinchVertices(out.mesh)) back.push_back(back[c]);
	const size_t no = out.mesh.points.size();
	if (p.thickness > 0.0)
	{
		const std::vector<V3> nr = EditNormals(cm);
		for (size_t i = 0; i < no; ++i) out.mesh.points.push_back(out.mesh.points[i] - nr[back[i]] * p.thickness);
		const size_t nf = out.mesh.faceSizes.size();
		std::vector<size_t> start(nf);
		{
			size_t L = 0;
			for (size_t fi = 0; fi < nf; ++fi) { start[fi] = L; L += size_t(out.mesh.faceSizes[fi]); }
		}
		std::unordered_map<uint64_t, int> cnt;
		for (size_t fi = 0; fi < nf; ++fi)
		{
			const int sz = out.mesh.faceSizes[fi];
			for (int k = 0; k < sz; ++k)
				cnt[EdgeKey(out.mesh.faceVerts[start[fi] + k], out.mesh.faceVerts[start[fi] + (k + 1) % sz])]++;
		}
		std::vector<int32_t> fv;
		std::vector<UV2> fu;
		const double us = p.uvScale;
		for (size_t fi = 0; fi < nf; ++fi)
		{
			const int sz = out.mesh.faceSizes[fi];
			fv.resize(sz);
			fu.resize(sz);
			for (int k = 0; k < sz; ++k)
			{
				fv[k] = out.mesh.faceVerts[start[fi] + sz - 1 - k] + int32_t(no);
				fu[k] = out.mesh.loopUv[start[fi] + sz - 1 - k];
			}
			out.mesh.AddFaceV(fv, matBack, fu);
			for (int k = 0; k < sz; ++k)
			{
				const int32_t a = out.mesh.faceVerts[start[fi] + k], b = out.mesh.faceVerts[start[fi] + (k + 1) % sz];
				if (cnt[EdgeKey(a, b)] != 1) continue;
				const V3& pa = out.mesh.points[a];
				const V3& pb = out.mesh.points[b];
				const double s0 = (pa.x + pa.y + pa.z) * us, s1 = s0 + Len(pb - pa) * us, th = p.thickness * us;
				out.mesh.AddFace({ b, a, a + int32_t(no), b + int32_t(no) }, matSide, { { s1, th }, { s0, th }, { s0, 0.0 }, { s1, 0.0 } });
			}
		}
	}
	auto put = [&](const char* name, const std::vector<float>& src)
	{
		std::vector<float>& a = out.mesh.Attr(name);
		for (size_t i = 0; i < out.mesh.points.size(); ++i) a[i] = src[back[i % no]];
	};
	put("metal_torn", torn);
	put("metal_dent", dent);
	put("metal_scorch", scorch);
	put("metal_bend", bendv);
	put("metal_petal", petal);
	out.mesh.FinishAttrs();
	for (const MeshSite& st : sites)
	{
		if (st.kind == 0) { out.blasts++; out.petals += int(st.b->blasts.back().petals.size()); }
		else if (st.kind == 1) { out.punctures++; out.petals += int(st.b->blasts.back().petals.size()); }
		else if (st.kind == 2) out.dents++;
		else out.tears++;
	}
	out.ok = !out.mesh.faceSizes.empty();
	return out.ok;
}

} // namespace pdgen
