// строим плиточное покрытие с локальным разрушением
// ProjectDamage broken tile generator - GPL-3.0-or-later; see LICENSE.
// Recipe "tile-damage-v1". See tile_damage.h and DAMAGE_GENERATORS.md.
#include "tile_damage.h"

#include <algorithm>
#include <cmath>

namespace pdgen {
namespace {

struct Chip { V2 c; double r = 0.01; bool through = false; uint64_t seed = 0; };

struct Tile
{
	V2 c;                   // centre
	double a = 0, b = 0;    // half sizes
	uint64_t seed = 0;
	double rnd = 0, dmg = 0;
	bool missing = false, leftover = false, loose = false, cracked = false;
	std::vector<Chip> chips;
	std::vector<int> crackSegs;   // indices into the crack segment list
	// own breakage of a knocked-out tile that leaves pieces
	std::vector<std::array<V2, 2>> ownCracks;
	V2 away;                // direction away from the impact
};

struct Seg { V2 a, b; double halfW = 0.001; uint64_t seed = 0; };

struct Zone { V2 c; double R = 0.3, s = 1.0; };

double RoundBox(const V2& p, double a, double b, double r)
{
	r = std::min(r, std::min(a, b) * 0.9);
	const double qx = std::abs(p.x) - a + r, qy = std::abs(p.y) - b + r;
	return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

V3 Rotate(const V3& v, const V3& axis, double ang)
{
	const V3 k = Norm(axis, V3(1, 0, 0));
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1 - c));
}

class TileBuilder
{
public:
	TileBuilder(const TileDamageParams& p, double h) : P(p), h(h), seed(uint64_t(uint32_t(p.seed)) * 0x9E37ull + 0x7113ull) {}
	const TileDamageParams& P;
	double h;
	uint64_t seed;
	std::vector<Tile> tiles;
	std::vector<Seg> segs;
	std::vector<Zone> zones;
	double px = 0, py = 0;

	double HW() const { return 0.5 * P.width; }
	double HH() const { return 0.5 * P.height; }
	double AreaBox(const V2& p) const { return std::max(std::abs(p.x) - HW(), std::abs(p.y) - HH()); }

	double Damage(const V2& p) const
	{
		double d = 0;
		for (const Zone& z : zones)
		{
			const double r = Len(p - z.c) / std::max(z.R, 1e-6);
			if (r < 1.0) d = std::max(d, z.s * std::pow(1.0 - r * r, 1.2));
		}
		if (d > 0) d *= 0.85 + 0.3 * Fbm2(p.x / 0.15, p.y / 0.15, seed + 3, 2);
		return Clamp(d, 0.0, 1.5);
	}

	// x of the vertical joint lines in the row that contains y (pattern dependent)
	double RowOffset(int j) const { return P.pattern == 1 && (j & 1) ? P.rowOffset * px : 0.0; }
	double ColOffset(int i) const { return P.pattern == 2 && (i & 1) ? P.rowOffset * py : 0.0; }

	// distance to the nearest joint centre line and the joint direction there
	double JointDist(const V2& p, V2& dir) const
	{
		double best = 1e30;
		if (P.pattern != 2)
		{
			const int j = int(std::floor(p.y / py + 0.5));
			const double yc = j * py;   // tile row centre
			// horizontal joints at yc +- py/2
			const double dh = std::abs(std::abs(p.y - yc) - 0.5 * py);
			if (dh < best) { best = dh; dir = V2(1, 0); }
			const double off = RowOffset(j);
			const double xr = p.x - off;
			const double dv = std::abs(std::abs(xr - std::floor(xr / px + 0.5) * px) - 0.5 * px);
			if (dv < best) { best = dv; dir = V2(0, 1); }
		}
		else
		{
			const int i = int(std::floor(p.x / px + 0.5));
			const double xc = i * px;
			const double dv = std::abs(std::abs(p.x - xc) - 0.5 * px);
			if (dv < best) { best = dv; dir = V2(0, 1); }
			const double yr = p.y - ColOffset(i);
			const double dh = std::abs(std::abs(yr - std::floor(yr / py + 0.5) * py) - 0.5 * py);
			if (dh < best) { best = dh; dir = V2(1, 0); }
		}
		return best;
	}

	// the nearest point on the nearest joint line
	V2 SnapToJoint(const V2& p) const
	{
		V2 dir;
		JointDist(p, dir);
		if (P.pattern != 2)
		{
			const int j = int(std::floor(p.y / py + 0.5));
			const double yc = j * py;
			if (dir.x != 0) return V2(p.x, yc + (p.y >= yc ? 0.5 : -0.5) * py);
			const double off = RowOffset(j);
			const double xr = p.x - off, xc = std::floor(xr / px + 0.5) * px;
			return V2(off + xc + (xr >= xc ? 0.5 : -0.5) * px, p.y);
		}
		const int i = int(std::floor(p.x / px + 0.5));
		const double xc = i * px;
		if (dir.y != 0) return V2(xc + (p.x >= xc ? 0.5 : -0.5) * px, p.y);
		const double yr = p.y - ColOffset(i), yc = std::floor(yr / py + 0.5) * py;
		return V2(p.x, ColOffset(i) + yc + (yr >= yc ? 0.5 : -0.5) * py);
	}

	void Layout(const std::vector<TileImpactMarker>& markers)
	{
		px = P.tileWidth + P.groutWidth;
		py = P.tileHeight + P.groutWidth;
		Rng r(seed, 1);
		for (const TileImpactMarker& m : markers) zones.push_back({ V2(m.x, m.y), std::max(m.radius, 1e-3), Clamp(m.strength, 0.0, 2.0) });
		for (int i = 0; i < P.impacts; ++i)
			zones.push_back({ V2(r.Range(-0.7, 0.7) * HW(), r.Range(-0.7, 0.7) * HH()), P.impactRadius * r.Range(0.7, 1.2), 1.0 });
		for (Zone& z : zones) z.s *= P.damage / 0.6;
		// tiles
		const double a = 0.5 * P.tileWidth, b = 0.5 * P.tileHeight;
		const int nj = int(std::ceil(HH() / py)) + 1, ni = int(std::ceil(HW() / px)) + 2;
		for (int j = -nj; j <= nj; ++j)
			for (int i = -ni; i <= ni; ++i)
			{
				Tile t;
				t.c = V2(i * px + RowOffset(j), j * py + ColOffset(i));
				t.a = a;
				t.b = b;
				if (std::abs(t.c.x) - a > HW() || std::abs(t.c.y) - b > HH()) continue;
				t.seed = Hash(seed, 100, uint64_t(int64_t(i) + 100000), uint64_t(int64_t(j) + 100000));
				tiles.push_back(t);
			}
		// tile states
		int k = 0;
		for (Tile& t : tiles)
		{
			Rng tr(t.seed, 1);
			t.rnd = tr.Next();
			t.dmg = Damage(t.c);
			const double pMiss = P.missing * Smooth((t.dmg - 0.3) / 0.55);
			t.missing = tr.Next() < pMiss;
			if (t.missing) t.leftover = tr.Next() < P.leftovers;
			t.loose = !t.missing && tr.Next() < P.loose * Smooth(t.dmg / 0.5);
			// direction away from the strongest zone
			double best = -1;
			for (const Zone& z : zones)
			{
				const double w = z.s / (Len(t.c - z.c) + 0.05);
				if (w > best) { best = w; const V2 d = t.c - z.c; const double l = Len(d); t.away = l > 1e-9 ? d * (1.0 / l) : V2(1, 0); }
			}
			if (best < 0) t.away = V2(1, 0);
			// chips
			const double rate = P.chips * t.dmg * 3.0 + P.scatterChips;
			int nc = int(std::floor(rate + tr.Next()));
			if (t.missing && !t.leftover) nc = 0;
			for (int c = 0; c < std::min(nc, 8); ++c)
			{
				Chip ch;
				ch.seed = Hash(t.seed, 7, uint64_t(c));
				ch.r = P.chipSize * tr.Range(0.45, 1.3);
				if (tr.Chance(0.4))
				{
					const double sx = tr.Chance(0.5) ? 1 : -1, sy = tr.Chance(0.5) ? 1 : -1;
					ch.c = t.c + V2(sx * a, sy * b);
					ch.through = tr.Chance(0.6);
				}
				else
				{
					const int side = tr.Int(0, 3);
					const double u = tr.Range(-0.85, 0.85);
					ch.c = t.c + (side == 0 ? V2(u * a, -b) : side == 1 ? V2(u * a, b) : side == 2 ? V2(-a, u * b) : V2(a, u * b));
					ch.through = tr.Chance(0.25);
				}
				t.chips.push_back(ch);
			}
			// a knocked-out tile that leaves pieces: its own cracks
			if (t.leftover)
				for (int c = 0; c < 2 + int(tr.Next() * 2); ++c)
				{
					const double ang = tr.Range(0, kPi);
					const V2 d(std::cos(ang), std::sin(ang));
					const V2 o = t.c + V2(tr.Range(-0.4, 0.4) * a, tr.Range(-0.4, 0.4) * b);
					t.ownCracks.push_back({ o - d * (3 * (a + b)), o + d * (3 * (a + b)) });
				}
			++k;
		}
		// crack paths
		const double step = 0.3 * std::min(P.tileWidth, P.tileHeight);
		int segId = 0;
		std::function<void(V2, V2, double, int)> walk = [&](V2 p, V2 dir, double len, int depth)
		{
			Rng cr(Hash(seed, 300, uint64_t(segId), uint64_t(depth)), 1);
			bool follow = false;
			V2 jdir;
			double run = 0;
			while (run < len)
			{
				V2 nd = dir;
				const double jd = JointDist(p, jdir);
				if (follow)
				{
					nd = Dot(jdir, dir) >= 0 ? jdir : jdir * -1.0;
					if (cr.Chance(0.12 * (1.0 - P.followGrout) + 0.04))
					{
						follow = false;
						const double ang = cr.Range(0.35, 0.8) * (cr.Chance(0.5) ? 1 : -1);
						nd = V2(nd.x * std::cos(ang) - nd.y * std::sin(ang), nd.x * std::sin(ang) + nd.y * std::cos(ang));
					}
				}
				else if (jd < 0.3 * std::min(P.tileWidth, P.tileHeight) && cr.Chance(P.followGrout * 0.8))
				{
					follow = true;
					const V2 q = SnapToJoint(p);
					if (Len(q - p) > 1e-9)
					{
						segs.push_back({ p, q, 0.5 * P.crackWidth, Hash(seed, 301, uint64_t(segId++)) });
						run += Len(q - p);
						p = q;
					}
					JointDist(p, jdir);
					nd = Dot(jdir, dir) >= 0 ? jdir : jdir * -1.0;
				}
				else
				{
					const double ang = 0.3 * cr.Normal();
					nd = V2(dir.x * std::cos(ang) - dir.y * std::sin(ang), dir.x * std::sin(ang) + dir.y * std::cos(ang));
				}
				const double l = Len(nd);
				nd = l > 1e-9 ? nd * (1.0 / l) : dir;
				V2 q = p + nd * step;
				if (follow) q = SnapToJoint(q);
				if (AreaBox(q) > 0.02) break;
				segs.push_back({ p, q, 0.5 * P.crackWidth, Hash(seed, 301, uint64_t(segId++)) });
				run += Len(q - p);
				p = q;
				dir = nd;
				if (depth < 2 && cr.Chance(0.07))
				{
					const double ang = cr.Range(0.4, 1.0) * (cr.Chance(0.5) ? 1 : -1);
					walk(p, V2(dir.x * std::cos(ang) - dir.y * std::sin(ang), dir.x * std::sin(ang) + dir.y * std::cos(ang)),
						(len - run) * cr.Range(0.3, 0.7), depth + 1);
				}
			}
		};
		for (size_t zi = 0; zi < zones.size(); ++zi)
		{
			Rng zr(seed, 400 + zi);
			const int n = int(std::round(P.cracks * std::min(zones[zi].s, 1.5)));
			for (int c = 0; c < n; ++c)
			{
				const double ang = 2 * kPi * (c + zr.Range(-0.35, 0.35)) / std::max(n, 1);
				const V2 dir(std::cos(ang), std::sin(ang));
				walk(zones[zi].c + dir * (zones[zi].R * 0.15), dir, P.crackLength * zr.Range(0.5, 1.2) * std::min(zones[zi].s, 1.3), 0);
			}
		}
		// which crack segments touch which tile
		for (size_t s = 0; s < segs.size(); ++s)
		{
			const Seg& sg = segs[s];
			const double x0 = std::min(sg.a.x, sg.b.x), x1 = std::max(sg.a.x, sg.b.x);
			const double y0 = std::min(sg.a.y, sg.b.y), y1 = std::max(sg.a.y, sg.b.y);
			for (Tile& t : tiles)
				if (x1 >= t.c.x - t.a - 0.01 && x0 <= t.c.x + t.a + 0.01 && y1 >= t.c.y - t.b - 0.01 && y0 <= t.c.y + t.b + 0.01)
					t.crackSegs.push_back(int(s));
		}
	}

	double CrackDist(const Tile& t, const V2& p, double* jagW = nullptr) const
	{
		double d = 1e30;
		for (int s : t.crackSegs)
		{
			const Seg& sg = segs[s];
			d = std::min(d, SegDist(p, sg.a, sg.b));
		}
		if (jagW) *jagW = 0;
		return d;
	}

	double CrackField(const Tile& t, const V2& p) const   // negative inside a crack
	{
		double g = 1e30;
		const double minHalf = 0.55 * h;
		for (int s : t.crackSegs)
		{
			const Seg& sg = segs[s];
			const double wob = P.crackJag * 0.6 * P.crackWidth * Noise2(p.x / 0.006, p.y / 0.006, sg.seed);
			g = std::min(g, SegDist(p, sg.a, sg.b) + wob - std::max(sg.halfW, minHalf));
		}
		for (const auto& oc : t.ownCracks)
		{
			const double wob = P.crackJag * 0.6 * P.crackWidth * Noise2(p.x / 0.006, p.y / 0.006, t.seed + 5);
			g = std::min(g, SegDist(p, oc[0], oc[1]) + wob + 0.002 * Noise2(p.x / 0.03, p.y / 0.03, t.seed + 6) - std::max(0.5 * P.crackWidth, minHalf));
		}
		return g;
	}

	double TileField(const Tile& t, const V2& p) const
	{
		double f = std::max(RoundBox(p - t.c, t.a, t.b, P.cornerRadius), AreaBox(p));
		for (const Chip& ch : t.chips)
			if (ch.through)
				f = std::max(f, ch.r * (1.0 + 0.3 * Fbm2(p.x / (ch.r * 0.6), p.y / (ch.r * 0.6), ch.seed, 2)) - Len(p - ch.c));
		return std::max(f, -CrackField(t, p));
	}

	// shallow chips: depth below the tile face
	double ChipDepth(const Tile& t, const V2& p) const
	{
		double d = 0;
		for (const Chip& ch : t.chips)
		{
			if (ch.through) continue;
			const double r = Len(p - ch.c) / (ch.r * (1.0 + 0.35 * Fbm2(p.x / (ch.r * 0.5), p.y / (ch.r * 0.5), ch.seed + 1, 2)));
			if (r < 1.0) d = std::max(d, P.chipDepth * P.tileThickness * (1.0 - r * r) * (0.8 + 0.2 * Noise2(p.x / 0.002, p.y / 0.002, ch.seed + 2)));
		}
		return d;
	}

	double TopZ(const Tile& t, const V2& p, double* chip) const
	{
		const double T = P.tileThickness;
		double z = T;
		const double dEdge = -std::max(RoundBox(p - t.c, t.a, t.b, P.cornerRadius), AreaBox(p) < 0 ? -1e30 : AreaBox(p));
		const double bv = std::min(P.bevel, 0.45 * T);
		if (bv > 0 && dEdge < bv)
		{
			const double q = bv - std::max(dEdge, 0.0);
			z -= bv - std::sqrt(std::max(bv * bv - q * q, 0.0));
		}
		z += P.tileWarp * Noise2((p.x - t.c.x) / (t.a * 1.5), (p.y - t.c.y) / (t.b * 1.5), t.seed + 9);
		const double cd = ChipDepth(t, p);
		z -= cd;
		if (chip) *chip = Sat(cd / std::max(0.12 * T, 1e-9));
		return std::max(z, 0.2 * T);
	}

	// grout / adhesive
	double MissingField(const V2& p) const   // negative inside the footprint of a missing tile
	{
		double f = 1e30;
		for (const Tile& t : tiles)
		{
			if (!t.missing) continue;
			if (std::abs(p.x - t.c.x) > t.a + 0.03 || std::abs(p.y - t.c.y) > t.b + 0.03) continue;
			f = std::min(f, RoundBox(p - t.c, t.a, t.b, P.cornerRadius));
		}
		return f;
	}

	double GroutCrackDist(const V2& p) const
	{
		double d = 1e30;
		for (const Seg& s : segs)
		{
			const double x0 = std::min(s.a.x, s.b.x) - 0.02, x1 = std::max(s.a.x, s.b.x) + 0.02;
			if (p.x < x0 || p.x > x1) continue;
			const double y0 = std::min(s.a.y, s.b.y) - 0.02, y1 = std::max(s.a.y, s.b.y) + 0.02;
			if (p.y < y0 || p.y > y1) continue;
			d = std::min(d, SegDist(p, s.a, s.b));
		}
		return d;
	}
};

} // namespace

// основной вход: собираем геометрию по переданным настройкам
bool BuildTileDamage(const TileDamageParams& pin, const std::vector<TileImpactMarker>& markers, TileDamageResult& out)
{
	out = TileDamageResult();
	TileDamageParams p = pin;
	p.width = std::max(p.width, 1e-3);
	p.height = std::max(p.height, 1e-3);
	p.tileWidth = std::max(p.tileWidth, 1e-3);
	p.tileHeight = std::max(p.tileHeight, 1e-3);
	p.tileThickness = std::max(p.tileThickness, 1e-5);
	p.groutWidth = std::max(p.groutWidth, 0.0);
	double h = std::max(p.resolution, 1e-5);
	const double cap = std::max(p.maxVerts, 1000) / 2.6;
	if (p.width * p.height / (h * h) > cap) h = std::sqrt(p.width * p.height / cap);
	out.resolution = h;
	TileBuilder B(p, h);
	B.Layout(markers);
	GenMesh& M = out.mesh;
	const double T = p.tileThickness;
	// ---------------------------------------------------------------- tiles
	int tileIx = 0;
	for (const Tile& t : B.tiles)
	{
		++tileIx;
		out.tiles++;
		if (t.missing) out.missing++;
		if (t.missing && !t.leftover) continue;
		const bool damaged = t.dmg > 0.05 || !t.chips.empty() || !t.crackSegs.empty() || t.leftover;
		const double ht = damaged ? h : std::max(h * 2.0, std::min(t.a, t.b) / 6.0);
		Domain2D dom;
		const double m = 2 * ht;
		MeshDomain2D([&](double u, double v) { return B.TileField(t, V2(u, v)); }, t.c.x - t.a - m, t.c.x + t.a + m, t.c.y - t.b - m,
			t.c.y + t.b + m, ht, dom, 4 * ht * ht);
		if (dom.tris.empty()) continue;
		std::vector<int32_t> triPiece;
		const int np = DomainPieces(dom, triPiece);
		// piece centroids (rest) and areas
		std::vector<V2> cen(np);
		std::vector<double> area(np, 0.0);
		for (size_t i = 0; i < dom.tris.size(); ++i)
		{
			const auto& tr = dom.tris[i];
			const V2 e1 = dom.rest[tr[1]] - dom.rest[tr[0]], e2 = dom.rest[tr[2]] - dom.rest[tr[0]];
			const double ar = 0.5 * (e1.x * e2.y - e1.y * e2.x);
			const V2 cc = (dom.rest[tr[0]] + dom.rest[tr[1]] + dom.rest[tr[2]]) * (1.0 / 3.0);
			cen[triPiece[i]] = cen[triPiece[i]] + cc * ar;
			area[triPiece[i]] += ar;
		}
		for (int k = 0; k < np; ++k) cen[k] = area[k] > 0 ? cen[k] * (1.0 / area[k]) : t.c;
		if (np > 1 && !t.leftover) out.cracked++;
		if (t.loose) out.loose++;
		out.chips += int(t.chips.size());
		// top / bottom points before the piece transforms
		const size_t n = dom.rest.size();
		std::vector<V3> top(n), bot(n);
		std::vector<float> chip(n), crack(n);
		for (size_t i = 0; i < n; ++i)
		{
			const V2 q = dom.rest[i];
			double cv = 0;
			top[i] = V3(q.x, q.y, B.TopZ(t, q, &cv));
			bot[i] = V3(q.x, q.y, 0.0);
			chip[i] = float(cv);
			crack[i] = float(1.0 - Sat(std::min(B.CrackDist(t, q), 1e9) / std::max(3.0 * h, 0.004)));
		}
		Rng tr(t.seed, 3);
		// loose tile: tilt about one edge
		V3 looseAxis(0, 0, 0), loosePivot;
		double looseAng = 0;
		if (t.loose)
		{
			const double ang = tr.Range(0, 2 * kPi);
			const V2 d(std::cos(ang), std::sin(ang));
			looseAxis = V3(-d.y, d.x, 0);
			loosePivot = V3(t.c.x - d.x * t.a, t.c.y - d.y * t.b, 0.0);
			looseAng = p.tiltAngle * kPi / 180.0 * tr.Range(0.4, 1.0);
		}
		for (int k = 0; k < np; ++k)
		{
			// leftovers: keep the pieces on the side away from the impact
			if (t.leftover)
			{
				const V2 d = cen[k] - t.c;
				const double side = Dot(d, t.away) / std::max(std::min(t.a, t.b), 1e-9);
				Rng pr(Hash(t.seed, 11, uint64_t(k)), 1);
				if (side + 0.5 * pr.Signed() < 0.15 || area[k] < 0.08 * 4 * t.a * t.b * 0.25) continue;
			}
			std::vector<V3> tk(top), bk(bot);
			// piece transform: apart from the tile centre, sink, tilt
			Rng pr(Hash(t.seed, 12, uint64_t(k)), 1);
			V3 shift(0, 0, 0), axis(0, 0, 0);
			double ang = 0;
			const V3 pivot(cen[k].x, cen[k].y, 0.0);
			if (np > 1 || t.leftover)
			{
				V2 d = cen[k] - t.c;
				const double l = Len(d);
				d = l > 1e-9 ? d * (1.0 / l) : V2(1, 0);
				const double mag = p.pieceShift * pr.Range(0.3, 1.0) * (0.5 + t.dmg);
				shift = V3(d.x * mag, d.y * mag, -mag * pr.Range(0.0, 0.8));
				const double a2 = pr.Range(0, 2 * kPi);
				axis = V3(std::cos(a2), std::sin(a2), 0);
				ang = p.pieceTilt * kPi / 180.0 * pr.Range(0.2, 1.0) * (0.5 + t.dmg);
			}
			for (size_t i = 0; i < n; ++i)
			{
				auto xf = [&](V3 v)
				{
					if (ang != 0) v = pivot + Rotate(v - pivot, axis, ang);
					v = v + shift;
					if (looseAng != 0) v = loosePivot + Rotate(v - loosePivot, looseAxis, looseAng);
					return v;
				};
				tk[i] = xf(tk[i]);
				bk[i] = xf(bk[i]);
			}
			ExtrudeOptions eo;
			eo.topMat = [](size_t) { return 0; };
			eo.matBottom = 1;
			eo.matSide = 1;
			eo.uvScale = p.uvScale;
			eo.uvU0 = t.c.x - t.a;
			eo.uvV0 = t.c.y - t.b;
			eo.part = tileIx - 1;
			eo.triPiece = &triPiece;
			eo.piece = k;
			const int32_t base = int32_t(M.points.size());
			const std::vector<int32_t> map = ExtrudeDomain(dom, tk, bk, eo, M);
			int32_t used = 0;
			for (int32_t v : map) used += v >= 0;
			auto& aRnd = M.Attr("tile_random");
			auto& aChip = M.Attr("tile_chip");
			auto& aCrack = M.Attr("tile_crack");
			auto& aDmg = M.Attr("tile_damage");
			for (size_t i = 0; i < n; ++i)
			{
				if (map[i] < 0) continue;
				for (int c = 0; c < 2; ++c)
				{
					const size_t ix = size_t(base + map[i] + c * used);
					aRnd[ix] = float(t.rnd);
					aChip[ix] = chip[i];
					aCrack[ix] = crack[i];
					aDmg[ix] = float(Sat(t.dmg));
				}
			}
			out.pieces++;
		}
	}
	// ---------------------------------------------------------------- grout
	const double hg = std::max(h * 1.5, 0.6 * p.groutWidth);
	const double margin = 2.5 * hg;
	{
		// speed: bucket the tiles on a grid
		const double cell = std::max(B.px, B.py);
		const int gx = int(std::ceil(p.width / cell)) + 2, gy = int(std::ceil(p.height / cell)) + 2;
		std::vector<std::vector<int>> bucket(size_t(gx) * gy);
		for (size_t i = 0; i < B.tiles.size(); ++i)
		{
			const Tile& t = B.tiles[i];
			const int x0 = int(std::floor((t.c.x - t.a + B.HW()) / cell)) + 1, x1 = int(std::floor((t.c.x + t.a + B.HW()) / cell)) + 1;
			const int y0 = int(std::floor((t.c.y - t.b + B.HH()) / cell)) + 1, y1 = int(std::floor((t.c.y + t.b + B.HH()) / cell)) + 1;
			for (int y = std::max(y0, 0); y <= std::min(y1, gy - 1); ++y)
				for (int x = std::max(x0, 0); x <= std::min(x1, gx - 1); ++x) bucket[size_t(y) * gx + x].push_back(int(i));
		}
		auto bucketOf = [&](const V2& q) -> const std::vector<int>&
		{
			const int x = Clamp(int(std::floor((q.x + B.HW()) / cell)) + 1, 0, gx - 1), y = Clamp(int(std::floor((q.y + B.HH()) / cell)) + 1, 0, gy - 1);
			return bucket[size_t(y) * gx + x];
		};
		auto groutField = [&](double u, double v) -> double
		{
			const V2 q(u, v);
			double f = B.AreaBox(q);
			double miss = 1e30, hide = 1e30;
			for (int i : bucketOf(q))
			{
				const Tile& t = B.tiles[i];
				if (t.missing) miss = std::min(miss, RoundBox(q - t.c, t.a, t.b, p.cornerRadius));
				else hide = std::min(hide, RoundBox(q - t.c, t.a - margin, t.b - margin, 0.0));
			}
			// grout broken away at the edge of a missing tile
			if (miss < 0.02)
				miss -= p.groutCrumble * p.groutWidth * (0.6 + 0.8 * Noise2(u / 0.004, v / 0.004, B.seed + 21));
			f = std::max(f, -miss);
			f = std::max(f, -hide);
			return f;
		};
		Domain2D gd;
		MeshDomain2D(groutField, -B.HW() - 2 * hg, B.HW() + 2 * hg, -B.HH() - 2 * hg, B.HH() + 2 * hg, hg, gd, 4 * hg * hg);
		if (!gd.tris.empty())
		{
			const size_t n = gd.rest.size();
			std::vector<V3> top(n), bot(n);
			std::vector<float> crack(n), dmg(n);
			const double gt = std::max(T - p.groutRecess, 0.1 * T);
			for (size_t i = 0; i < n; ++i)
			{
				const V2 q = gd.rest[i];
				const double dc = B.GroutCrackDist(q);
				double z = gt;
				const double c1 = Sat(1.0 - dc / std::max(1.5 * p.groutWidth + p.crackWidth, 2 * hg));
				z -= p.groutCrumble * gt * 0.8 * c1 * (0.5 + 0.5 * Noise2(q.x / 0.01, q.y / 0.01, B.seed + 22));
				// crumbled next to missing tiles
				double miss = 1e30;
				for (int ti : bucketOf(q))
					if (B.tiles[ti].missing) miss = std::min(miss, RoundBox(q - B.tiles[ti].c, B.tiles[ti].a, B.tiles[ti].b, p.cornerRadius));
				if (miss < 3 * p.groutWidth)
					z -= p.groutCrumble * gt * 0.7 * Sat(1.0 - miss / (3 * p.groutWidth + 1e-9)) * (0.4 + 0.6 * Noise2(q.x / 0.006, q.y / 0.006, B.seed + 23) * 0.5 + 0.3);
				top[i] = V3(q.x, q.y, std::max(z, 0.08 * T));
				bot[i] = V3(q.x, q.y, 0.0);
				crack[i] = float(c1);
				dmg[i] = float(Sat(B.Damage(q)));
			}
			ExtrudeOptions eo;
			eo.topMat = [](size_t) { return 2; };
			eo.matBottom = 2;
			eo.matSide = 2;
			eo.uvScale = p.uvScale;
			eo.uvU0 = -B.HW();
			eo.uvV0 = -B.HH();
			const int32_t base = int32_t(M.points.size());
			const auto map = ExtrudeDomain(gd, top, bot, eo, M);
			int32_t used = 0;
			for (int32_t v : map) used += v >= 0;
			auto& aCrack = M.Attr("tile_crack");
			auto& aDmg = M.Attr("tile_damage");
			for (size_t i = 0; i < n; ++i)
				if (map[i] >= 0)
					for (int c = 0; c < 2; ++c) { aCrack[size_t(base + map[i] + c * used)] = crack[i]; aDmg[size_t(base + map[i] + c * used)] = dmg[i]; }
		}
		// ---------------------------------------------------------------- adhesive on the bed of missing tiles
		if (p.adhesive > 0 && out.missing > 0)
		{
			auto adhField = [&](double u, double v) -> double
			{
				const V2 q(u, v);
				double miss = 1e30;
				for (int i : bucketOf(q))
					if (B.tiles[i].missing) miss = std::min(miss, RoundBox(q - B.tiles[i].c, B.tiles[i].a - 0.3 * hg, B.tiles[i].b - 0.3 * hg, p.cornerRadius));
				// patches torn off with the tile
				const double patch = 0.0035 * (Fbm2(u / 0.035, v / 0.035, B.seed + 31, 3) + 1.0 - 2.0 * Sat(p.adhesive));
				return std::max(std::max(miss, B.AreaBox(q)), patch);
			};
			Domain2D ad;
			MeshDomain2D(adhField, -B.HW() - 2 * h, B.HW() + 2 * h, -B.HH() - 2 * h, B.HH() + 2 * h, h, ad, 4 * h * h);
			if (!ad.tris.empty())
			{
				const size_t n = ad.rest.size();
				std::vector<V3> top(n), bot(n);
				for (size_t i = 0; i < n; ++i)
				{
					const V2 q = ad.rest[i];
					// notched trowel ridges, each tile its own direction
					double dirA = 0;
					for (int ti : bucketOf(q))
						if (B.tiles[ti].missing && std::abs(q.x - B.tiles[ti].c.x) <= B.tiles[ti].a + 1e-9 && std::abs(q.y - B.tiles[ti].c.y) <= B.tiles[ti].b + 1e-9)
							dirA = Hash01(B.tiles[ti].seed, 41) * kPi;
					const double s = q.x * std::cos(dirA) + q.y * std::sin(dirA);
					const double ridge = 0.5 + 0.5 * std::sin(2 * kPi * s / std::max(p.adhesiveSpacing, 1e-4) + 0.6 * Noise2(q.x / 0.02, q.y / 0.02, B.seed + 32));
					const double hgt = p.adhesiveHeight * (0.25 + 0.75 * std::pow(ridge, 1.5)) * (0.8 + 0.4 * Noise2(q.x / 0.004, q.y / 0.004, B.seed + 33));
					const double fe = adhField(q.x, q.y);   // thin out at the patch border
					top[i] = V3(q.x, q.y, std::max(hgt * Sat(-fe / 0.002 + 0.15), 0.00015));
					bot[i] = V3(q.x, q.y, 0.0);
				}
				ExtrudeOptions eo;
				eo.topMat = [](size_t) { return 3; };
				eo.matBottom = 3;
				eo.matSide = 3;
				eo.uvScale = p.uvScale;
				ExtrudeDomain(ad, top, bot, eo, M);
			}
		}
	}
	// ---------------------------------------------------------------- bed
	if (p.bedThickness > 0)
	{
		Domain2D bd;
		const double hb = std::min(p.width, p.height) / 4.0;
		MeshDomain2D([&](double u, double v) { return B.AreaBox(V2(u, v)); }, -B.HW() - hb, B.HW() + hb, -B.HH() - hb, B.HH() + hb, hb, bd);
		const size_t n = bd.rest.size();
		std::vector<V3> top(n), bot(n);
		for (size_t i = 0; i < n; ++i)
		{
			top[i] = V3(bd.rest[i].x, bd.rest[i].y, 0.0);
			bot[i] = V3(bd.rest[i].x, bd.rest[i].y, -p.bedThickness);
		}
		ExtrudeOptions eo;
		eo.topMat = [](size_t) { return 3; };
		eo.matBottom = 3;
		eo.matSide = 3;
		eo.uvScale = p.uvScale;
		eo.uvU0 = -B.HW();
		eo.uvV0 = -B.HH();
		ExtrudeDomain(bd, top, bot, eo, M);
	}
	M.FinishAttrs();
	out.ok = !M.points.empty();
	return out.ok;
}

} // namespace pdgen
