// строим воронку и выброшенные вокруг неё части
// ProjectDamage blast crater generator - GPL-3.0-or-later; see LICENSE.
//
// 1. craters: the main one at the origin plus host markers; each has a
//    ragged outline (noise around the circle), a bowl with a flat rubbly
//    floor, a rim, and a blanket of thrown-out soil that thins as
//    (1 + d / reach)^-3, with streaks (rays) and a stronger downrange side
// 2. ground: a closed slab, its top the sum of the craters, gentle terrain
//    and clods (Worley bumps where the blanket is thick)
// 3. pavement: a 2D domain = the patch minus the holes over the craters
//    minus cracks (Worley cells in a log-polar-scaled space: small plates at
//    the rim, larger further out; long radial cracks; concrete slab joints);
//    every connected plate is extruded: small plates rigid (a plane fitted to
//    the ground, pushed up and tilted at the rim), large ones draped
// 4. debris: stones, soil clods and pavement chunks thrown out (more along
//    the streaks and downrange) or fallen back into the bowl, resting on the
//    surface
#include "blast_crater.h"

namespace pdgen {
namespace {

struct Crater
{
	V2 c;
	double R = 1, D = 0.5, rim = 0.2, dir = 0, asym = 0;
	uint64_t seed = 0;
	std::vector<double> rays;
	std::vector<std::vector<V2>> cracks;   // long radial pavement cracks (polylines)
};

double WrapAngle(double a)
{
	while (a > kPi) a -= 2 * kPi;
	while (a < -kPi) a += 2 * kPi;
	return a;
}

struct Field
{
	const BlastCraterParams& p;
	std::vector<Crater> craters;
	uint64_t seed;
	explicit Field(const BlastCraterParams& pp) : p(pp), seed(uint64_t(uint32_t(pp.seed)) * 0x9E3779B1ull + 0xC4A7ull) {}

	// the crater's own radius in direction a (ragged outline)
	double Outline(const Crater& C, double a) const
	{
		const double n = Noise2(std::cos(a) * 1.7 + 3.1, std::sin(a) * 1.7, C.seed + 1) + 0.45 * Noise2(std::cos(a) * 4.3, std::sin(a) * 4.3 + 7.7, C.seed + 2);
		return C.R * (1.0 + Clamp(p.irregularity, 0.0, 2.0) * 0.11 * n);
	}

	// height contribution of one crater; ejecta 0..1, inside 0..1
	// base: only the uplifted ground under the thrown-out blanket (what the pavement lies on)
	double CraterHeight(const Crater& C, double x, double y, double* ej, double* inside, double* sOut, bool base = false) const
	{
		const double dx = x - C.c.x, dy = y - C.c.y;
		const double r = std::hypot(dx, dy);
		const double a = std::atan2(dy, dx);
		const double Rr = Outline(C, a);
		const double s = r / Rr;
		if (sOut) *sOut = r / C.R;
		const double down = std::cos(a - C.dir);
		const double af = std::max(0.2, 1.0 + C.asym * down);
		const double irr = Clamp(p.irregularity, 0.0, 2.0);
		double z;
		if (s < 1.0)
		{
			const double flat = Clamp(p.floorFlat, 0.0, 0.9);
			const double t = Sat((s - flat) / (1.0 - flat));
			z = -C.D * (1.0 - t * t) * (1.0 - 0.25 * C.asym * down);
			const double lip = Smooth((s - 0.6) / 0.4);
			z += C.rim * af * lip * lip;
			// rubble on the floor
			z += irr * 0.1 * C.D * (1.0 - s) * Fbm2(x / (0.22 * C.R), y / (0.22 * C.R), C.seed + 3, 3);
			if (inside) *inside = Sat(-z / std::max(C.D, 1e-9));
			if (ej) *ej = Sat(lip);
		}
		else
		{
			const double reach = std::max(Clamp(p.ejecta, 0.0, 5.0) * af, 0.05);
			const double uplift = 0.4 * C.rim * af * std::pow(1.0 + (s - 1.0) / (0.5 * reach), -3.0);
			if (base)
			{
				if (ej) *ej = 0.0;
				if (inside) *inside = 0.0;
				return uplift;
			}
			double e = std::pow(1.0 + (s - 1.0) / reach, -3.0);
			if (p.rays > 0 && !C.rays.empty())
			{
				double streak = 0;
				for (double ra : C.rays) streak = std::max(streak, std::exp(-std::pow(WrapAngle(a - ra) / 0.09, 2)));
				e += Clamp(p.rays, 0.0, 2.0) * 0.6 * streak * std::pow(1.0 + (s - 1.0) / (2.4 * reach), -2.0) * Smooth((s - 1.0) / 0.25);
			}
			e *= 0.75 + 0.5 * Sat(0.5 + 0.8 * Fbm2(x / (0.35 * C.R), y / (0.35 * C.R), C.seed + 4, 3));
			z = uplift + 0.6 * C.rim * af * e;
			if (inside) *inside = 0.0;
			if (ej) *ej = Sat(e);
		}
		return z;
	}

	double Height(double x, double y, double* ejOut = nullptr, double* inOut = nullptr, bool base = false) const
	{
		double z = 0, ej = 0, in = 0;
		if (p.terrain > 0)
		{
			const double ts = std::max(p.terrainScale, 1e-3);
			z += p.terrain * Fbm2(x / ts, y / ts, seed + 5, 4);
		}
		for (const Crater& C : craters)
		{
			double e = 0, i = 0;
			z += CraterHeight(C, x, y, &e, &i, nullptr, base);
			ej = std::max(ej, e);
			in = std::max(in, i);
		}
		if (!base && p.clods > 0 && p.clodSize > 0 && ej > 0.02)
		{
			const double cs = p.clodSize;
			const Cell2 w = Worley2(x / cs, y / cs, seed + 6, 0.9);
			const double bump = Sat(1.0 - w.f1 / 0.62);
			z += p.clods * cs * 0.45 * bump * bump * Sat(ej * 1.6) * (0.6 + 0.8 * Hash01(w.id, 1));
			z += p.clods * cs * 0.12 * Fbm2(x / (0.4 * cs), y / (0.4 * cs), seed + 7, 2) * Sat(ej * 2.0);
		}
		if (ejOut) *ejOut = ej;
		if (inOut) *inOut = in;
		return z;
	}

	double Scorch(double x, double y) const
	{
		double sc = 0;
		for (const Crater& C : craters)
		{
			const double s = std::hypot(x - C.c.x, y - C.c.y) / C.R;
			sc = std::max(sc, Sat(1.5 - 0.55 * s + 0.35 * Fbm2(x / (0.3 * C.R), y / (0.3 * C.R), C.seed + 8, 3)));
		}
		return Sat(sc * Clamp(p.scorch, 0.0, 2.0));
	}

	// pavement: > 0 where there is none (holes, cracks, joints)
	// the edge of a crater's broken zone (meshed finer): < 0 inside
	double ZoneEdge(const Crater& C, double x, double y) const
	{
		const double a = std::atan2(y - C.c.y, x - C.c.x);
		const double rd = C.R * (std::max(p.crackReach, p.holeSize) + 0.1) * (1.0 + 0.08 * Noise2(std::cos(a) * 1.5, std::sin(a) * 1.5 + 2.0, C.seed + 13));
		const double jag = 0.06 * C.R * Noise2(x * 2.5 / C.R, y * 2.5 / C.R, C.seed + 14) + 0.012 * C.R * Noise2(x * 12.0 / C.R, y * 12.0 / C.R, C.seed + 15);
		return std::hypot(x - C.c.x, y - C.c.y) - rd + jag;
	}

	// minHw: the narrowest crack half width the mesh can show (coarse regions)
	double PavementGap(double x, double y, double minHw = 0.0) const
	{
		double g = -1e30;
		const double hw0 = p.crackWidth > 0 ? 0.5 * p.crackWidth : 0.0;
		for (const Crater& C : craters)
		{
			const double dx = x - C.c.x, dy = y - C.c.y;
			const double r = std::hypot(dx, dy);
			const double a = std::atan2(dy, dx);
			const double hole = Outline(C, a) * std::max(p.holeSize, 0.0) * (1.0 + 0.06 * Noise2(x * 3.0 / C.R, y * 3.0 / C.R, C.seed + 9));
			g = std::max(g, hole - r);
			if (hw0 <= 0) continue;
			const double s = r / C.R;
			const double reach = std::max(p.crackReach, p.holeSize) * (1.0 + 0.15 * Noise2(std::cos(a) * 2.0, std::sin(a) * 2.0 + 5.0, C.seed + 10));
			const double fade = 1.0 - SmoothStep(0.7 * reach, reach, s);
			const double jag = 0.6 + 0.8 * Sat(0.5 + Noise2(x * 6.0, y * 6.0, C.seed + 11));
			if (fade > 0 && r > 1e-6)
			{
				// Worley plates in a space scaled by ln(1 + r / R): small near the rim, larger outward
				const double ps = std::max(p.plateSize, 1e-3) * 0.5;
				const double L = C.R * std::log1p(r / C.R) / ps;
				const Cell2 w = Worley2(dx / r * L + 13.0, dy / r * L - 7.0, C.seed + 12, 0.95);
				const double dist = 0.5 * (w.f2 - w.f1) * ps * (1.0 + r / C.R);
				const double hw = fade > 0.05 ? std::max(hw0 * jag * (0.4 + 0.6 * fade), minHw) : hw0 * jag * fade * 8.0;
				g = std::max(g, hw - dist);
			}
			if (s > std::max(p.crackReach, p.holeSize) * 1.2 + std::max(p.crackLength, 0.0) + 0.5) continue;   // beyond every long crack
			for (const auto& line : C.cracks)
			{
				const double len = double(line.size() - 1);
				for (size_t k = 0; k + 1 < line.size(); ++k)
				{
					double t;
					const double d = SegDist(V2(x, y), line[k], line[k + 1], &t);
					const double along = (double(k) + t) / len;
					g = std::max(g, (along < 0.92 ? std::max(hw0 * jag * (1.0 - along), minHw) : hw0 * jag * (1.0 - along)) - d);
				}
			}
		}
		if (p.pavement == 2 && p.slabSize > 0)
		{
			const double S = p.slabSize;
			const double jx = std::abs(x / S - std::round(x / S)) * S, jy = std::abs(y / S - std::round(y / S)) * S;
			g = std::max(g, std::max({ hw0, 0.004, minHw }) - std::min(jx, jy));
		}
		return g;
	}
};

// a closed lumpy blob: subdivided octahedron, displaced; level 1 is angular, 2 rounder
void Blob(GenMesh& m, const V3& c, const V3& radii, const V3 axes[3], int level, double rough, uint64_t seed, int32_t mat, int32_t part, double uvScale)
{
	std::vector<V3> P = { V3(1, 0, 0), V3(-1, 0, 0), V3(0, 1, 0), V3(0, -1, 0), V3(0, 0, 1), V3(0, 0, -1) };
	std::vector<std::array<int32_t, 3>> T = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
	for (int l = 0; l < level; ++l)
	{
		std::unordered_map<uint64_t, int32_t> mid;
		auto midpoint = [&](int32_t a, int32_t b) {
			const uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
			auto it = mid.find(key);
			if (it != mid.end()) return it->second;
			P.push_back(Norm(P[a] + P[b]));
			mid.emplace(key, int32_t(P.size() - 1));
			return int32_t(P.size() - 1);
		};
		std::vector<std::array<int32_t, 3>> T2;
		for (const auto& t : T)
		{
			const int32_t a = midpoint(t[0], t[1]), b = midpoint(t[1], t[2]), cc = midpoint(t[2], t[0]);
			T2.push_back({ t[0], a, cc });
			T2.push_back({ t[1], b, a });
			T2.push_back({ t[2], cc, b });
			T2.push_back({ a, b, cc });
		}
		T.swap(T2);
	}
	const int32_t base = int32_t(m.points.size());
	for (const V3& d : P)
	{
		const double k = 1.0 + rough * Noise3(d.x * 1.3, d.y * 1.3, d.z * 1.3, seed) + 0.4 * rough * Noise3(d.x * 3.1, d.y * 3.1, d.z * 3.1, seed + 1);
		const V3 q(d.x * radii.x * k, d.y * radii.y * k, d.z * radii.z * k);
		m.AddPoint(c + axes[0] * q.x + axes[1] * q.y + axes[2] * q.z);
	}
	for (const auto& t : T)
	{
		std::array<double, 2> uv[3];
		for (int i = 0; i < 3; ++i)
		{
			const V3& d = P[t[i]];
			uv[i] = { (d.x * radii.x + d.y * radii.y) * uvScale, d.z * radii.z * uvScale };
		}
		m.AddFace({ base + t[0], base + t[1], base + t[2] }, mat, { uv[0], uv[1], uv[2] }, part);
	}
}

// a flat polygonal chunk (pavement), closed
void Chunk(GenMesh& m, const std::vector<V2>& poly, double thick, const V3& c, const V3 axes[3], int32_t part, double uvScale)
{
	const int32_t n = int32_t(poly.size()), base = int32_t(m.points.size());
	for (const V2& q : poly) m.AddPoint(c + axes[0] * q.x + axes[1] * q.y + axes[2] * thick);
	for (const V2& q : poly) m.AddPoint(c + axes[0] * q.x + axes[1] * q.y);
	std::vector<int32_t> top, bot;
	std::vector<std::array<double, 2>> ut, ub;
	for (int32_t i = 0; i < n; ++i)
	{
		top.push_back(base + i);
		ut.push_back({ poly[i].x * uvScale, poly[i].y * uvScale });
		bot.push_back(base + n + (n - 1 - i));
		ub.push_back({ -poly[n - 1 - i].x * uvScale, poly[n - 1 - i].y * uvScale });
	}
	m.AddFaceV(top, 2, ut, part);
	m.AddFaceV(bot, 3, ub, part);
	double s = 0;
	for (int32_t i = 0; i < n; ++i)
	{
		const int32_t j = (i + 1) % n;
		const double l = Len(poly[j] - poly[i]) * uvScale;
		m.AddFace({ base + j, base + i, base + n + i, base + n + j }, 3, { { s + l, thick * uvScale }, { s, thick * uvScale }, { s, 0.0 }, { s + l, 0.0 } }, part);
		s += l;
	}
}

} // namespace

// создаём воронки по заданным точкам удара
bool BuildBlastCrater(const BlastCraterParams& pIn, const std::vector<CraterMarker>& markers, BlastCraterResult& out)
{
	out = BlastCraterResult();
	const BlastCraterParams& p = pIn;
	Field F(p);
	const uint64_t seed = F.seed;
	const double W = std::max(p.width, 0.01), H = std::max(p.height, 0.01);
	// craters
	auto addCrater = [&](double x, double y, double R, double depthScale, uint64_t s) {
		if (R <= 0) return;
		Crater C;
		C.c = V2(x, y);
		C.R = R;
		const double scale = R / std::max(p.radius, 1e-6);
		C.D = std::max(p.depth, 0.0) * scale * depthScale;
		C.rim = std::max(p.rimHeight, 0.0) * scale * std::min(depthScale, 1.5);
		C.dir = (p.direction + 25.0 * (Hash01(s, 1) - 0.5) * (s != seed + 100)) * kPi / 180.0;
		C.asym = Clamp(p.asymmetry, 0.0, 0.95);
		C.seed = s;
		Rng rng(s, 2);
		const int nr = std::max(0, std::min(p.rayCount, 64));
		for (int i = 0; i < nr; ++i) C.rays.push_back(2 * kPi * (i + 0.5 * rng.Signed() * 0.8) / std::max(nr, 1));
		// long radial cracks in the pavement
		if (p.pavement > 0 && p.crackWidth > 0)
		{
			const int nc = std::max(0, std::min(p.radialCracks, 64));
			for (int i = 0; i < nc; ++i)
			{
				double a = 2 * kPi * (i + rng.Range(0.1, 0.9)) / std::max(nc, 1);
				const double r0 = R * std::max(p.holeSize, 0.3) * 0.95;
				const double r1 = R * (std::max(p.crackReach, p.holeSize) * 0.85 + std::max(p.crackLength, 0.0) * rng.Range(0.5, 1.0));
				std::vector<V2> line;
				V2 q(x + std::cos(a) * r0, y + std::sin(a) * r0);
				line.push_back(q);
				const double step = std::max(0.15 * R, 4 * p.resolution);
				for (double r = r0; r < r1; r += step)
				{
					a += rng.Signed() * 0.18 * step / r;
					q = q + V2(std::cos(a) * step, std::sin(a) * step) + V2(rng.Signed(), rng.Signed()) * (0.12 * step);
					line.push_back(q);
				}
				if (line.size() >= 2) C.cracks.push_back(line);
			}
		}
		F.craters.push_back(C);
	};
	if (p.mainCrater) addCrater(0, 0, p.radius, 1.0, seed + 100);
	for (size_t i = 0; i < markers.size(); ++i)
		addCrater(markers[i].x, markers[i].y, markers[i].radius, std::max(markers[i].depth, 0.0), seed + 200 + i * 31);
	out.craters = int(F.craters.size());

	// resolution: the ground slab (top + bottom) and the pavement stay below maxVerts
	double h = std::max(p.resolution, 1e-4);
	{
		const double layers = (p.groundThickness > 0 ? 2.0 : 1.0) + (p.pavement > 0 ? 2.0 : 0.0);
		const double est = layers * (W / h + 1) * (H / h + 1);
		const double lim = 0.9 * std::max(p.maxVerts, 1000);
		if (est > lim) h *= std::sqrt(est / lim) * 1.01;
	}
	out.resolution = h;
	GenMesh& m = out.mesh;
	const double us = p.uvScale;

	// ground
	{
		Domain2D d;
		MeshDomain2D([&](double x, double y) { return std::max(std::abs(x) - 0.5 * W, std::abs(y) - 0.5 * H); }, -0.5 * W, 0.5 * W, -0.5 * H, 0.5 * H, h, d);
		std::vector<V3> top(d.rest.size()), bot(d.rest.size());
		const double T = std::max(p.groundThickness, 0.0);
		for (size_t i = 0; i < d.rest.size(); ++i)
		{
			const double x = d.rest[i].x, y = d.rest[i].y;
			top[i] = V3(x, y, F.Height(x, y));
		}
		// the slab bottom sits below the deepest bowl
		double zmin = 0;
		for (const V3& q : top) zmin = std::min(zmin, q.z);
		for (size_t i = 0; i < d.rest.size(); ++i) bot[i] = V3(d.rest[i].x, d.rest[i].y, std::min(-T, zmin - 0.25 * T));
		if (T > 0)
		{
			ExtrudeOptions o;
			o.topMat = [](size_t) { return 0; };
			o.matBottom = 1;
			o.matSide = 1;
			o.uvScale = us;
			ExtrudeDomain(d, top, bot, o, m);
		}
		else
		{
			const int32_t base = int32_t(m.points.size());
			for (const V3& q : top) m.AddPoint(q);
			for (const auto& t : d.tris)
				m.AddFace({ base + t[0], base + t[1], base + t[2] }, 0,
					{ std::array<double, 2>{ d.rest[t[0]].x * us, d.rest[t[0]].y * us }, std::array<double, 2>{ d.rest[t[1]].x * us, d.rest[t[1]].y * us },
						std::array<double, 2>{ d.rest[t[2]].x * us, d.rest[t[2]].y * us } });
		}
	}

	// pavement
	const double PT = std::max(p.pavementThickness, 0.0);
	if (p.pavement > 0 && PT > 0)
	{
		// regions: a finer mesh over every crater's broken zone (its cracks are narrow), the rest coarse
		struct Region { double x0, x1, y0, y1, hh; int idx; };
		std::vector<Region> regions;
		double hf = std::min(h, std::max(0.8 * std::max(p.crackWidth, 0.0), h / 2.5));
		{
			double area = 0;
			for (const Crater& C : F.craters) area += std::pow(2.0 * C.R * (std::max(p.crackReach, p.holeSize) + 0.1) * 1.1, 2);
			hf = std::min(h, std::max(hf, std::sqrt(2.0 * area / (0.45 * std::max(p.maxVerts, 1000)))));
		}
		for (size_t i = 0; i < F.craters.size(); ++i)
		{
			const Crater& C = F.craters[i];
			const double rd = C.R * (std::max(p.crackReach, p.holeSize) + 0.1) * 1.1;
			Region rg{ std::max(-0.5 * W, C.c.x - rd), std::min(0.5 * W, C.c.x + rd), std::max(-0.5 * H, C.c.y - rd), std::min(0.5 * H, C.c.y + rd), hf, int(i) };
			if (rg.x1 > rg.x0 && rg.y1 > rg.y0) regions.push_back(rg);
		}
		regions.push_back({ -0.5 * W, 0.5 * W, -0.5 * H, 0.5 * H, h, -1 });
		int plates = 0, partBase = 0;
		for (const Region& rg : regions)
		{
			Domain2D d;
			const double hh = rg.hh;
			auto field = [&](double x, double y) {
				double f = std::max(std::max(std::abs(x) - 0.5 * W, std::abs(y) - 0.5 * H), F.PavementGap(x, y, rg.idx < 0 ? 0.6 * h : 0.75 * hh));
				const double gb = 0.25 * std::max(p.crackWidth, 1.2 * h);   // the zone edge is a crack too
				if (rg.idx >= 0)
				{
					f = std::max(f, F.ZoneEdge(F.craters[size_t(rg.idx)], x, y) + gb);
					for (int j = 0; j < rg.idx; ++j) f = std::max(f, gb - F.ZoneEdge(F.craters[size_t(j)], x, y));
				}
				else
					for (const Crater& C : F.craters) f = std::max(f, gb - F.ZoneEdge(C, x, y));
				return f;
			};
			MeshDomain2D(field, rg.x0, rg.x1, rg.y0, rg.y1, hh, d, 6.0 * hh * hh);
			std::vector<int32_t> triPiece;
			const int np = d.tris.empty() ? 0 : DomainPieces(d, triPiece);
			std::vector<std::vector<int32_t>> verts(np);
			{
				std::vector<int32_t> seen(d.rest.size(), -1);
				for (size_t t = 0; t < d.tris.size(); ++t)
					for (int32_t v : d.tris[t])
						if (seen[v] != triPiece[t]) { seen[v] = triPiece[t]; verts[triPiece[t]].push_back(v); }
			}
			std::vector<V3> top(d.rest.size()), bot(d.rest.size());
			for (size_t i = 0; i < d.rest.size(); ++i) bot[i] = V3(d.rest[i].x, d.rest[i].y, F.Height(d.rest[i].x, d.rest[i].y, nullptr, nullptr, true));
			for (int pc = 0; pc < np; ++pc)
			{
				const auto& vs = verts[pc];
				if (vs.size() < 3) continue;
				V2 c(0, 0);
				bool border = false;
				for (int32_t v : vs)
				{
					c = c + d.rest[v];
					if (std::abs(d.rest[v].x) > 0.5 * W - 1.5 * hh || std::abs(d.rest[v].y) > 0.5 * H - 1.5 * hh) border = true;
				}
				c = V2(c.x / vs.size(), c.y / vs.size());
				double ext = 0;
				for (int32_t v : vs) ext = std::max(ext, Len(d.rest[v] - c));
				// nearest crater
				const Crater* nc = nullptr;
				double best = 1e300;
				for (const Crater& C : F.craters)
				{
					const double s = Len(c - C.c) / C.R;
					if (s < best) { best = s; nc = &C; }
				}
				const bool rigid = !border && nc != nullptr && ext < 2.5 * nc->R;
				if (rigid)
				{
					// least squares plane through the ground under the plate
					double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sz = 0, sxz = 0, syz = 0;
					const double n = double(vs.size());
					for (int32_t v : vs)
					{
						const double x = d.rest[v].x - c.x, y = d.rest[v].y - c.y, z = bot[v].z;
						sxx += x * x; sxy += x * y; syy += y * y; sx += x; sy += y; sz += z; sxz += x * z; syz += y * z;
					}
					const double det = sxx * syy - sxy * sxy;
					double a = sz / n, b = 0, cc = 0;
					if (std::abs(det) > 1e-18)
					{
						const double bx = sxz - sx * a, by = syz - sy * a;
						b = (bx * syy - by * sxy) / det;
						cc = (by * sxx - bx * sxy) / det;
					}
					Rng rng(Hash(seed + 300, uint64_t(partBase + pc)), 1);
					const V2 rd = Len(c - nc->c) > 1e-9 ? (c - nc->c) * (1.0 / Len(c - nc->c)) : V2(1, 0);
					const double push = Smooth(1.0 - (best - std::max(p.holeSize, 0.3)) / 1.0);
					const double ang = (p.plateTilt * push * rng.Range(0.35, 1.0) + rng.Range(0.0, 1.2)) * kPi / 180.0;
					double rmax = -1e300;
					for (int32_t v : vs) rmax = std::max(rmax, Dot(d.rest[v] - nc->c, rd));
					const double rx = rng.Signed(), ry = rng.Signed();   // a small random tilt too
					const double lift = push * rng.Range(0.0, 0.5) * PT;
					double pen = 0;
					std::vector<double> z(vs.size());
					for (size_t k = 0; k < vs.size(); ++k)
					{
						const int32_t v = vs[k];
						const double x = d.rest[v].x - c.x, y = d.rest[v].y - c.y;
						z[k] = a + b * x + cc * y + std::tan(ang) * (rmax - Dot(d.rest[v] - nc->c, rd)) + 0.008 * (rx * x + ry * y) + lift;
						pen = std::max(pen, bot[v].z - z[k]);
					}
					for (size_t k = 0; k < vs.size(); ++k) bot[vs[k]].z = z[k] + 0.6 * pen;
				}
				for (int32_t v : vs) top[v] = bot[v] + V3(0, 0, PT);
				ExtrudeOptions o;
				o.topMat = [](size_t) { return 2; };
				o.matBottom = 3;
				o.matSide = 3;
				o.uvScale = us;
				o.part = partBase + pc;
				o.triPiece = &triPiece;
				o.piece = pc;
				ExtrudeDomain(d, top, bot, o, m);
				++plates;
			}
			partBase += np;
		}
		out.plates = plates;
	}

	// debris
	if (p.debris > 0 && p.debrisSize > 0 && !F.craters.empty())
	{
		Rng rng(seed + 400, 1);
		double wsum = 0;
		for (const Crater& C : F.craters) wsum += C.R * C.R;
		int made = 0;
		for (int n = 0; n < std::min(p.debris, 20000); ++n)
		{
			// crater, weighted by area
			double pick = rng.Next() * wsum;
			const Crater* C = &F.craters[0];
			for (const Crater& k : F.craters)
			{
				pick -= k.R * k.R;
				if (pick <= 0) { C = &k; break; }
			}
			const bool inside = rng.Chance(Clamp(p.debrisInside, 0.0, 1.0));
			double a;
			if (!inside && !C->rays.empty() && rng.Chance(Clamp(p.rays, 0.0, 1.0) * 0.6))
				a = C->rays[size_t(rng.Int(0, int(C->rays.size()) - 1))] + 0.06 * rng.Normal();
			else
			{
				do a = rng.Range(-kPi, kPi);
				while (rng.Next() * (1.0 + C->asym) > 1.0 + C->asym * std::cos(a - C->dir));
			}
			const double u = rng.Next();
			const double r = inside ? C->R * 0.8 * std::sqrt(u) : C->R * (0.95 + std::max(p.debrisSpread, 0.0) * -std::log(std::max(1.0 - u, 1e-9)));
			const V2 pos = C->c + V2(std::cos(a) * r, std::sin(a) * r);
			double size = p.debrisSize * (0.25 + 0.75 * std::pow(rng.Next(), 2.0));
			if (!inside) size *= 1.0 - 0.45 * Sat((r / C->R - 1.0) / (3.0 * std::max(p.debrisSpread, 0.1)));
			if (std::abs(pos.x) > 0.5 * W - size || std::abs(pos.y) > 0.5 * H - size) continue;
			const double sOf = Len(pos - C->c) / C->R;
			const bool onPavement = p.pavement > 0 && PT > 0 && F.PavementGap(pos.x, pos.y) < 0;
			const double ground = onPavement ? std::max(F.Height(pos.x, pos.y), F.Height(pos.x, pos.y, nullptr, nullptr, true) + PT) : F.Height(pos.x, pos.y);
			const int part = 100000 + made;
			const double yaw = rng.Range(-kPi, kPi);
			const double kind = rng.Next();
			if (p.pavement > 0 && PT > 0 && kind < 0.35 && sOf > 0.3)
			{
				// a pavement chunk lying tilted
				const int k = rng.Int(4, 7);
				std::vector<double> angs;
				for (int i = 0; i < k; ++i) angs.push_back(rng.Range(0.0, 2 * kPi));
				std::sort(angs.begin(), angs.end());
				std::vector<V2> poly;
				for (double t : angs)
				{
					const double rr = size * rng.Range(0.6, 1.0);
					poly.push_back(V2(std::cos(t) * rr, std::sin(t) * rr));
				}
				// keep it convex enough: drop vertices that make a reflex corner
				bool changed = true;
				while (changed && poly.size() > 3)
				{
					changed = false;
					for (size_t i = 0; i < poly.size(); ++i)
					{
						const V2 &a0 = poly[(i + poly.size() - 1) % poly.size()], &b0 = poly[i], &c0 = poly[(i + 1) % poly.size()];
						if ((b0.x - a0.x) * (c0.y - b0.y) - (b0.y - a0.y) * (c0.x - b0.x) <= 1e-12) { poly.erase(poly.begin() + long(i)); changed = true; break; }
					}
				}
				if (poly.size() < 3) continue;
				const double tilt = rng.Range(0.0, 0.6);
				const double ta = rng.Range(-kPi, kPi);
				V3 ax[3];
				ax[2] = Norm(V3(std::sin(tilt) * std::cos(ta), std::sin(tilt) * std::sin(ta), std::cos(tilt)));
				{
					const V3 ref(std::cos(yaw), std::sin(yaw), 0);
					ax[0] = Norm(ref - ax[2] * Dot(ref, ax[2]));
					ax[1] = Cross(ax[2], ax[0]);
				}
				const double lift = std::sin(tilt) * size * 0.7;
				Chunk(m, poly, PT, V3(pos.x, pos.y, ground - 0.2 * PT + lift), ax, part, us);
			}
			else
			{
				const bool clod = kind < 0.7;
				Rng r2(Hash(seed + 401, uint64_t(n)), 3);
				const V3 radii = clod ? V3(size, size * r2.Range(0.7, 1.0), size * r2.Range(0.45, 0.7))
									  : V3(size, size * r2.Range(0.55, 0.95), size * r2.Range(0.35, 0.75));
				V3 ax[3];
				const V3 nz = Norm(V3(r2.Signed() * 0.35, r2.Signed() * 0.35, 1.0));
				const V3 ref(std::cos(yaw), std::sin(yaw), 0);
				ax[2] = nz;
				ax[0] = Norm(ref - nz * Dot(ref, nz));
				ax[1] = Cross(ax[2], ax[0]);
				Blob(m, V3(pos.x, pos.y, ground + radii.z * 0.55), radii, ax, clod ? 2 : 1, clod ? 0.35 : 0.5, Hash(seed + 402, uint64_t(n)),
					clod ? 1 : 4, part, us);
			}
			++made;
		}
		out.debris = made;
	}
	m.FinishAttrs();

	// attributes
	{
		auto& sc = m.Attr("crater_scorch");
		auto& ej = m.Attr("crater_ejecta");
		auto& dp = m.Attr("crater_depth");
		for (size_t i = 0; i < m.points.size(); ++i)
		{
			const V3& q = m.points[i];
			double e = 0, in = 0;
			F.Height(q.x, q.y, &e, &in);
			sc[i] = float(F.Scorch(q.x, q.y));
			ej[i] = float(e);
			dp[i] = float(in);
		}
	}
	m.FinishAttrs();
	out.ok = !m.faceSizes.empty();
	return out.ok;
}

} // namespace pdgen
