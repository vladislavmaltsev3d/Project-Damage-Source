// строим расщеплённые деревянные детали
// ProjectDamage split wood generator - GPL-3.0-or-later; see LICENSE.
// Recipe "wood-split-v1". See wood_split.h and DAMAGE_GENERATORS.md.
#include "wood_split.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pdgen {
namespace {

struct Bundle
{
	double e0 = 0, e1 = 0, c = 0, a = 0;   // edges across the grain, centre, half width
	double d = 0;                          // signed reach past the break line (+: left piece, -: right piece)
	double pw = 1;                         // tip profile power
	double lift = 0, sway = 0;
};

struct Crack { double e = 0, zs = 0, ze = 0, cw = 0; };   // between bundle j and j+1

struct Layer
{
	std::vector<Bundle> b;
	std::vector<Crack> cr;                 // cr[j] between b[j] and b[j+1]
	double z0 = 0, z1 = 0, shift = 0, liftScale = 1;
	uint64_t seed = 0;
};

struct Split
{
	double vs = 0, tip = 0, end = 0, open = 0, wob = 0, halfW = 0;
	uint64_t seed = 0;
};

class WoodBuilder
{
public:
	WoodBuilder(const WoodSplitParams& p, double hu, double hv) : P(p), hu(hu), hv(hv), seed(uint64_t(uint32_t(p.seed)) * 0x9E37ull + 0x574Full) {}

	const WoodSplitParams& P;
	double hu, hv;
	uint64_t seed;
	double xb = 0;                         // break line
	bool broken = false, twoPieces = false;
	std::vector<Layer> layers;
	std::vector<Split> splits;

	double HL() const { return 0.5 * P.length; }
	double HW() const { return 0.5 * P.width; }

	void Layout()
	{
		broken = P.breakMode != 0;
		twoPieces = P.breakMode == 1;
		xb = Clamp(P.breakPos, -0.4, 0.4) * P.length;
		const int L = std::max(P.layers, 1);
		const double T = P.thickness;
		const double wmin = std::max(std::min(P.splinterWidthMin, P.splinterWidthMax), 2.5 * hv);
		const double wmax = std::max(std::max(P.splinterWidthMin, P.splinterWidthMax), wmin);
		const double lmin = std::max(std::min(P.splinterLengthMin, P.splinterLengthMax), 0.0);
		const double lmax = std::max(P.splinterLengthMin, P.splinterLengthMax);
		const double cw = std::max(0.5 * P.splinterGap, 0.55 * hv);
		layers.resize(L);
		for (int k = 0; k < L; ++k)
		{
			Layer& ly = layers[k];
			ly.z0 = -0.5 * T + T * k / L;
			ly.z1 = -0.5 * T + T * (k + 1) / L;
			ly.seed = Hash(seed, 50, P.layerShift < 0.05 ? 0 : uint64_t(k));
			Rng r(ly.seed, 1);
			Rng rl(Hash(seed, 51, uint64_t(k)), 1);
			ly.shift = P.layerShift * rl.Signed() * std::max(lmin * 2.0, 0.01 * P.length);
			// outer layers peel the most, the top up and the bottom down
			const double m = L > 1 ? (2.0 * k / (L - 1) - 1.0) : 1.0;
			ly.liftScale = L > 1 ? m * (0.35 + 0.65 * std::abs(m)) : 1.0;
			double v = -HW();
			while (v < HW() - 1e-9)
			{
				Bundle bd;
				double w = r.Range(wmin, wmax);
				if (HW() - (v + w) < 0.5 * wmin) w = HW() - v;
				bd.e0 = v;
				bd.e1 = v + w;
				bd.c = v + 0.5 * w;
				bd.a = 0.5 * w;
				const bool longOne = r.Chance(P.longShare);
				const double len = longOne ? r.Range(std::max(lmin, 0.4 * lmax), lmax) : r.Range(0.0, std::max(lmin, 1e-6));
				bd.d = (r.Chance(0.5) ? 1.0 : -1.0) * len;
				if (P.breakMode == 2 && bd.d < 0 && r.Chance(0.7)) bd.d = -bd.d;   // a broken end keeps most splinters
				bd.pw = Lerp(0.35, 1.7, Sat(P.tipShape)) * r.Range(0.75, 1.3);
				bd.lift = P.splinterLift * r.Range(0.2, 1.0) * (r.Chance(0.15) ? -0.5 : 1.0);
				bd.sway = P.splinterSway * r.Signed();
				ly.b.push_back(bd);
				v += w;
			}
			for (size_t j = 0; j + 1 < ly.b.size(); ++j)
			{
				Crack c;
				c.e = ly.b[j].e1;
				const double dlo = std::min({ 0.0, ly.b[j].d, ly.b[j + 1].d }), dhi = std::max({ 0.0, ly.b[j].d, ly.b[j + 1].d });
				const double depth = P.crackDepth * r.Range(0.1, 1.0) * (r.Chance(0.3) ? 0.15 : 1.0);
				c.zs = xb + ly.shift + dlo - depth;
				c.ze = xb + ly.shift + dhi + depth;
				c.cw = cw;
				ly.cr.push_back(c);
			}
		}
		Rng rs(seed, 60);
		const double leftEnd = broken ? xb : HL();
		const double leftLen = leftEnd + HL();
		for (int i = 0; i < P.splits; ++i)
		{
			Split s;
			s.seed = Hash(seed, 61, uint64_t(i));
			s.vs = HW() * rs.Range(-0.6, 0.6);
			s.end = leftEnd + 0.5 * P.splinterLengthMax;
			s.tip = leftEnd - leftLen * Clamp(P.splitLength * rs.Range(0.6, 1.2), 0.02, 0.95);
			s.open = P.splitOpen * rs.Range(0.6, 1.3) * (rs.Chance(0.5) ? 1.0 : 1.0);
			s.wob = 0.06 * P.width * rs.Range(0.3, 1.0);
			s.halfW = std::max(0.6 * hv, 0.0004);
			splits.push_back(s);
		}
	}

	const Bundle& BundleAt(const Layer& ly, double v, int* idx = nullptr) const
	{
		size_t lo = 0, hi = ly.b.size() - 1;
		while (lo < hi)
		{
			const size_t mid = (lo + hi) / 2;
			if (v < ly.b[mid].e1) hi = mid;
			else lo = mid + 1;
		}
		if (idx) *idx = int(lo);
		return ly.b[lo];
	}

	double Rough(double u, double v, uint64_t s) const
	{
		return P.grainRough * 1.1 * hv * Noise2(u / (2.5 * hu + 0.004), v / (1.3 * hv), s);
	}

	// break line of a layer: material of the left piece is u < BreakLine(v)
	double BreakLine(const Layer& ly, double v) const
	{
		const Bundle& b = BundleAt(ly, v);
		const double t = std::abs(v - b.c) / std::max(b.a, 1e-12);
		const double base = P.breakJag * Noise2(v / (3.0 * std::max(P.splinterWidthMax, 4 * hv)), 0.37, ly.seed + 7);
		return xb + ly.shift + base + b.d * std::pow(std::max(1.0 - t, 0.0), b.pw);
	}

	double CrackField(const Layer& ly, double u, double v) const   // negative inside a crack
	{
		if (!broken) return 1e30;
		double g = 1e30;
		int i;
		BundleAt(ly, v, &i);
		for (int j = std::max(i - 1, 0); j <= std::min(i, int(ly.cr.size()) - 1); ++j)
		{
			const Crack& c = ly.cr[j];
			const double cw = c.cw * (1.0 + 0.5 * P.grainRough * Noise2(u / (3 * hu + 0.006), double(j) * 3.1, ly.seed + 9));
			const double dx = std::max(c.zs - u, u - c.ze), dy = std::abs(v - c.e) - cw;
			const double o = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0)) + std::min(std::max(dx, dy), 0.0);
			g = std::min(g, o);
		}
		return g;
	}

	double SplitCenter(const Split& s, double u) const
	{
		return s.vs + s.wob * Noise2(u / (0.25 * P.length + 1e-6), 0.5, s.seed) + 0.3 * s.wob * Noise2(u / 0.03, 1.5, s.seed + 1);
	}

	double SplitField(double u, double v) const
	{
		double g = 1e30;
		for (const Split& s : splits)
		{
			const double half = s.halfW * (1.0 + 0.8 * P.splitJag * std::abs(Noise2(u / (3 * hu + 0.008), v / (2 * hv), s.seed + 3)));
			const double dy = std::abs(v - SplitCenter(s, u)) - half;
			const double dx = std::max(s.tip - u, u - s.end);
			g = std::min(g, std::hypot(std::max(dx, 0.0), std::max(dy, 0.0)) + std::min(std::max(dx, dy), 0.0));
		}
		return g;
	}

	// piece 0: left (or the only piece), piece 1: right
	double Field(int piece, const Layer& ly, double u, double v) const
	{
		double f = std::abs(v) - HW();
		if (piece == 0)
		{
			f = std::max(f, -HL() - u);
			if (broken) f = std::max(f, u - BreakLine(ly, v) + Rough(u, v, ly.seed + 11));
			else f = std::max(f, u - HL());
			f = std::max(f, -SplitField(u, v));
		}
		else
		{
			f = std::max(f, u - HL());
			f = std::max(f, BreakLine(ly, v) - u + Rough(u, v, ly.seed + 13));
		}
		if (broken) f = std::max(f, -CrackField(ly, u, v));
		return f;
	}

	struct Out { V3 p; float splinter = 0, brk = 0; };

	Out Deform(int piece, const Layer& ly, double u, double v) const
	{
		Out o;
		double x = u, y = v, z = 0.5 * (ly.z0 + ly.z1);
		const double Ls = std::max(P.splinterLengthMax, 1e-4);
		double brkDist = 1e30;
		if (broken)
		{
			int i;
			const Bundle& b = BundleAt(ly, v, &i);
			const double lo = i > 0 ? (piece == 0 ? ly.cr[i - 1].zs : ly.cr[i - 1].ze) : (piece == 0 ? -1e30 : 1e30);
			const double hi = i < int(ly.cr.size()) ? (piece == 0 ? ly.cr[i].zs : ly.cr[i].ze) : (piece == 0 ? -1e30 : 1e30);
			double s;
			if (piece == 0)
			{
				const double start = std::max(lo, hi);
				s = start > -1e29 ? u - start : 0.0;
				if (start <= -1e29) s = u - (xb + ly.shift - P.crackDepth);   // edge bundle: free on the board side
			}
			else
			{
				const double end = std::min(lo, hi);
				s = end < 1e29 ? end - u : 0.0;
				if (end >= 1e29) s = (xb + ly.shift + P.crackDepth) - u;
			}
			if (s > 0)
			{
				z += ly.liftScale * b.lift * s * s / Ls;
				y += b.sway * s * s / Ls;
				o.splinter = float(Sat(s / std::max(std::abs(b.d) + P.crackDepth, 1e-4)));
			}
			brkDist = std::abs(u - BreakLine(ly, v));
			brkDist = std::min(brkDist, std::abs(CrackField(ly, u, v)));
		}
		if (piece == 0)
			for (const Split& s : splits)
			{
				if (u <= s.tip) continue;
				const double c = SplitCenter(s, u);
				const double side = v >= c ? 1.0 : -1.0;
				const double t = Sat((u - s.tip) / std::max(s.end - s.tip, 1e-6));
				y += side * 0.5 * s.open * std::pow(t, 1.4);
				z += side * 0.15 * s.open * std::pow(t, 2.0) * Noise2(0.3, double(s.seed % 97), s.seed);
				brkDist = std::min(brkDist, std::abs(v - c));
			}
		if (P.bow != 0.0) z += P.bow * (1.0 - (2 * u / P.length) * (2 * u / P.length));
		// bend at the break and pull the pieces apart
		if (broken)
		{
			const double a = 0.5 * P.breakAngle * kPi / 180.0;
			const double phi = piece == 0 ? a : -a;
			const double zb = -0.5 * P.thickness;
			const double dx = x - xb, dz = z - zb;
			x = xb + dx * std::cos(phi) + dz * std::sin(phi);
			z = zb - dx * std::sin(phi) + dz * std::cos(phi);
			// the far end stays where it was, the break sags
			const double far = piece == 0 ? (xb + HL()) : (HL() - xb);
			z -= far * std::sin(std::abs(phi)) * (P.breakAngle >= 0 ? 1.0 : -1.0);
			x += (twoPieces ? 0.5 : 0.0) * P.breakGap * (piece == 0 ? -1.0 : 1.0);
		}
		o.p = V3(x, y, z);
		o.brk = float(1.0 - Sat(brkDist / std::max(4.0 * hv, 0.004)));
		return o;
	}
};

} // namespace

bool BuildWoodSplit(const WoodSplitParams& pin, WoodSplitResult& out)
{
	out = WoodSplitResult();
	WoodSplitParams p = pin;
	p.length = std::max(p.length, 1e-3);
	p.width = std::max(p.width, 1e-4);
	p.thickness = std::max(p.thickness, 1e-5);
	p.layers = std::max(1, std::min(p.layers, 32));
	double hv = std::max(p.resolution, 1e-5);
	double stretch = Clamp(p.grainStretch, 1.0, 20.0);
	// cap: two faces per layer per grid vertex
	const double cap = std::max(p.maxVerts, 1000) / (2.3 * p.layers);
	const double cells = p.length * p.width / (hv * hv * stretch);
	if (cells > cap) hv *= std::sqrt(cells / cap);
	const double hu = hv * stretch;
	out.resolution = hv;
	// layers thinner than about three mesh edges cannot be followed
	p.layers = std::max(1, std::min(p.layers, int(p.thickness / (3.0 * hv))));
	WoodBuilder B(p, hu, hv);
	B.Layout();
	const int L = int(B.layers.size());
	const int pieces = B.twoPieces ? 2 : 1;
	const double reach = std::max(p.splinterLengthMin, p.splinterLengthMax) + p.crackDepth + p.breakJag + 2 * hu;
	int part = 0;
	for (int piece = 0; piece < pieces; ++piece)
		for (int k = 0; k < L; ++k, ++part)
		{
			const Layer& ly = B.layers[k];
			double u0 = -B.HL(), u1 = B.HL();
			if (B.broken)
			{
				if (piece == 0) u1 = std::min(B.HL(), B.xb + std::abs(ly.shift) + reach);
				else u0 = std::max(-B.HL(), B.xb - std::abs(ly.shift) - reach);
			}
			Domain2D dom;
			const double m = 2 * hu;
			MeshDomain2D([&](double u, double v) { return B.Field(piece, ly, u, v); }, u0 - m, u1 + m, -B.HW() - 2 * hv, B.HW() + 2 * hv, hu,
				dom, -1.0, hv);   // only the board itself: loose chips cut off by cracks are dropped
			if (dom.tris.empty()) continue;
			const size_t n = dom.rest.size();
			std::vector<V3> mid(n);
			std::vector<float> spl(n), brk(n);
			for (size_t i = 0; i < n; ++i)
			{
				const auto o = B.Deform(piece, ly, dom.rest[i].x, dom.rest[i].y);
				mid[i] = o.p;
				spl[i] = o.splinter;
				brk[i] = o.brk;
			}
			SheetOptions so;
			so.thickness = ly.z1 - ly.z0;
			so.uvScale = p.uvScale;
			so.uvU0 = -B.HL();
			so.uvV0 = -B.HW();
			so.matFront = k == L - 1 ? 0 : 1;
			so.matBack = k == 0 ? 0 : 1;
			so.part = part;
			so.sideV0 = ly.z0 + 0.5 * p.thickness;
			const double tolU = 0.3 * hu, tolV = 0.3 * hv;
			const double HL = B.HL(), HW = B.HW();
			so.sideMat = [=](const V2& q) -> int32_t
			{
				return (std::abs(q.y) > HW - tolV || q.x < -HL + tolU || q.x > HL - tolU) ? 0 : 1;
			};
			GenMesh piecesMesh;
			SolidifySheet(dom, mid, so, piecesMesh);
			auto put = [&](const char* name, const std::vector<float>& src)
			{
				std::vector<float>& a = piecesMesh.Attr(name);
				for (size_t c = 0; c < 2; ++c)
					for (size_t i = 0; i < n; ++i) a[c * n + i] = src[i];
			};
			put("wood_splinter", spl);
			put("wood_break", brk);
			std::vector<float> lay(n, L > 1 ? float(k) / float(L - 1) : 1.0f);
			put("wood_layer", lay);
			out.mesh.Append(piecesMesh);
		}
	if (out.mesh.points.empty()) return false;
	out.mesh.FinishAttrs();
	out.pieces = pieces;
	for (const Layer& ly : B.layers)
		for (const Bundle& b : ly.b)
			if (std::abs(b.d) >= std::max(p.splinterLengthMin, 1e-6)) out.splinters++;
	if (!B.broken) out.splinters = 0;
	out.splits = int(B.splits.size());
	out.ok = true;
	return true;
}

// ===========================================================================
// on an object

namespace {

// 2D loops (y, z) for the cap: distance to the segments, inside by even-odd
struct Loops2D
{
	std::vector<std::pair<V2, V2>> seg;
	double Signed(const V2& q) const
	{
		double d = 1e300;
		bool in = false;
		for (const auto& s : seg)
		{
			const V2 a = s.first, b = s.second;
			d = std::min(d, SegDist(q, a, b));
			if ((a.y > q.y) != (b.y > q.y))
			{
				const double x = a.x + (q.y - a.y) * (b.x - a.x) / (b.y - a.y);
				if (x > q.x) in = !in;
			}
		}
		return in ? -d : d;
	}
};

double LoopArea(const std::vector<V2>& q)
{
	double a = 0;
	for (size_t i = 0; i < q.size(); ++i)
	{
		const V2& p0 = q[i];
		const V2& p1 = q[(i + 1) % q.size()];
		a += p0.x * p1.y - p1.x * p0.y;
	}
	return 0.5 * a;
}

} // namespace

// основной вход: собираем геометрию по переданным настройкам
bool BuildWoodSplitOnMesh(const WoodSplitParams& pin, const MeshInput& in, int32_t axis, int32_t sourceMaterials, WoodSplitResult& out)
{
	out = WoodSplitResult();
	if (in.points.empty() || in.faceSizes.empty()) return false;
	const int32_t matFresh = std::max(sourceMaterials, 1);
	// ---- frame: x along the grain, z through the layers
	V3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
	for (const V3& q : in.points)
	{
		lo = V3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
		hi = V3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
	}
	const double dim[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
	int g = axis >= 0 && axis <= 2 ? axis : (dim[0] >= dim[1] && dim[0] >= dim[2] ? 0 : (dim[1] >= dim[2] ? 1 : 2));
	const int o1 = (g + 1) % 3, o2 = (g + 2) % 3;
	const int ia = dim[o1] >= dim[o2] ? o1 : o2, it = ia == o1 ? o2 : o1;   // width, thickness axes
	auto unit = [](int k) { return V3(k == 0, k == 1, k == 2); };
	const V3 ex = unit(g), ez = unit(it);
	V3 ey = unit(ia);
	if (Dot(Cross(ex, ey), ez) < 0) ey = ey * -1.0;
	const V3 c0 = (lo + hi) * 0.5;
	auto toF = [&](const V3& q) { const V3 d = q - c0; return V3(Dot(d, ex), Dot(d, ey), Dot(d, ez)); };
	auto fromF = [&](const V3& q) { return c0 + ex * q.x + ey * q.y + ez * q.z; };
	WoodSplitParams p = pin;
	p.length = std::max(dim[g], 1e-3);
	p.width = std::max(dim[ia], 1e-4) * 1.002;
	p.thickness = std::max(dim[it], 1e-5);
	p.layers = std::max(1, std::min(p.layers, 32));
	p.splits = 0;
	p.bow = 0.0;
	if (p.breakMode == 0)
	{
		// nothing to break: the object as it is
		EditMesh m = MakeEditMesh(in, [](const std::vector<int32_t>&) { return false; });
		EmitEditMesh(m, out.mesh);
		out.mesh.FinishAttrs();
		out.pieces = 1;
		out.ok = true;
		return true;
	}
	double hv = std::max(p.resolution, 1e-5);
	const double stretch = Clamp(p.grainStretch, 1.0, 20.0);
	const double reach0 = std::max(p.splinterLengthMin, p.splinterLengthMax) + p.crackDepth + p.breakJag;
	{
		// the refined band: the surface around the break and two caps
		const double perim = 2.0 * (p.width + p.thickness);
		const double area = perim * 2.0 * (reach0 + p.layerShift * std::max(p.splinterLengthMin * 2.0, 0.01 * p.length)) + 2.5 * p.width * p.thickness;
		const double cells = area / (hv * hv * stretch);
		const double cap = std::max(p.maxVerts, 1000) / 2.4;
		if (cells > cap) hv *= std::sqrt(cells / cap);
	}
	const double hu = hv * stretch;
	out.resolution = hv;
	// layers thinner than about three mesh edges cannot be followed
	p.layers = std::max(1, std::min(p.layers, int(p.thickness / (3.0 * hv))));
	WoodBuilder B(p, hu, hv);
	B.Layout();
	const int L = int(B.layers.size());
	double shiftMax = 0;
	for (const Layer& ly : B.layers) shiftMax = std::max(shiftMax, std::abs(ly.shift));
	const double reach = reach0 + shiftMax + 2 * hu;
	const double T = p.thickness;
	const double band = std::min(0.6 * hv, 0.25 * T / L);
	auto layerOf = [&](double z) { return std::min(std::max(int(std::floor((z + 0.5 * T) / T * L)), 0), L - 1); };
	// each layer's break line sampled across the width and filtered: a morphological
	// opening and closing over about one mesh edge takes off the needle tips narrower
	// than the mesh can follow (they would come apart into loose bits and pinholes)
	const double dyS = 0.25 * hv;
	const int nS = std::max(2, int(std::ceil(2 * B.HW() / dyS)) + 1);
	std::vector<std::vector<double>> blT(L);
	{
		const int w = std::max(1, int(std::round(1.0 * hv / dyS)));
		auto filt = [&](const std::vector<double>& a, bool mx)
		{
			std::vector<double> o(a.size());
			for (int i = 0; i < int(a.size()); ++i)
			{
				double v = a[i];
				for (int j = std::max(0, i - w); j <= std::min(int(a.size()) - 1, i + w); ++j) v = mx ? std::max(v, a[j]) : std::min(v, a[j]);
				o[i] = v;
			}
			return o;
		};
		for (int k = 0; k < L; ++k)
		{
			std::vector<double> a(nS);
			for (int i = 0; i < nS; ++i) a[i] = B.BreakLine(B.layers[k], Clamp(-B.HW() + i * dyS, -B.HW(), B.HW()));
			a = filt(filt(a, false), true);   // opening: thin peaks
			a = filt(filt(a, true), false);   // closing: thin valleys
			blT[k] = a;
		}
	}
	auto BL = [&](int k, double y)
	{
		const double fi = Clamp((y + B.HW()) / dyS, 0.0, double(nS - 1));
		const int i0 = std::min(int(fi), nS - 2);
		return Lerp(blT[k][i0], blT[k][i0 + 1], fi - i0);
	};
	// the break surface x = Hx(y, z): each layer's break line, blended over a thin band between layers
	auto Hx = [&](double y, double z) -> double
	{
		const int k = layerOf(z);
		const double zb0 = -0.5 * T + T * k / L, zb1 = -0.5 * T + T * (k + 1) / L;
		if (k > 0 && z - zb0 < band) return Lerp(BL(k - 1, y), BL(k, y), SmoothStep(zb0 - band, zb0 + band, z));
		if (k < L - 1 && zb1 - z < band) return Lerp(BL(k, y), BL(k + 1, y), SmoothStep(zb1 - band, zb1 + band, z));
		return BL(k, y);
	};
	auto field = [&](const V3& q) { const V3 f = toF(q); return f.x - Hx(f.y, f.z); };
	// ---- edit the faces near the break
	EditMesh m = MakeEditMesh(in, [&](const std::vector<int32_t>& idx)
	{
		double x0 = 1e300, x1 = -1e300;
		for (int32_t v : idx) { const double x = toF(in.points[v]).x; x0 = std::min(x0, x); x1 = std::max(x1, x); }
		return x0 <= B.xb + reach + hu && x1 >= B.xb - reach - hu;
	});
	auto metric = [&](int32_t a, int32_t b)
	{
		const V3 fa = toF(m.p[a]), fb = toF(m.p[b]);
		const V3 d = fb - fa;
		const double xm = 0.5 * (fa.x + fb.x);
		if (std::abs(xm - B.xb) > reach + 0.5 * std::abs(d.x)) return false;
		return (d.x / hu) * (d.x / hu) + (d.y / hv) * (d.y / hv) + (d.z / hv) * (d.z / hv) > 1.3 * 1.3;
	};
	const size_t maxTris = size_t(std::max(p.maxVerts, 1000)) * 2 + m.tris.size();
	Refine(m, metric, maxTris);
	const bool trace = std::getenv("PDGEN_TRACE") != nullptr;
	if (trace) std::fprintf(stderr, "wood mesh: refined %zu tris %zu verts, reach %.3f hu %.4f hv %.4f\n", m.tris.size(), m.p.size(), reach, hu, hv);
	const size_t n0 = m.p.size();
	std::vector<double> f(n0);
	for (size_t v = 0; v < n0; ++v) f[v] = field(m.p[v]);
	auto crossing = [&](int32_t a, int32_t b) -> double
	{
		const V3 pa = m.p[a], pb = m.p[b];
		return RootOnEdge([&](double t) { return field(pa + (pb - pa) * t); }, f[a], f[b], 20);
	};
	SnapToContour(m, f, crossing);
	const bool two = p.breakMode == 1;
	const int pieces = two ? 2 : 1;
	const double Ls = std::max(p.splinterLengthMax, 1e-4);
	int part = 0;
	for (int piece = 0; piece < pieces; ++piece)
	{
		EditMesh pm = ClipSide(m, f, piece == 0 ? -1 : +1, crossing);
		// spike tips the cut severed from their piece are dropped: a piece is kept when it
		// reaches past the broken zone on its own side
		KeepAnchoredPieces(pm, [&](int32_t v)
		{
			const double x = toF(pm.p[v]).x;
			return piece == 0 ? x < B.xb - reach0 - shiftMax : x > B.xb + reach0 + shiftMax;
		});
		// ---- the cap: the break surface inside the cut loops
		const auto loops = BoundaryLoops(pm, true);
		std::vector<char> matched(loops.size(), 0);
		if (!loops.empty())
		{
			Loops2D L2;
			double y0 = 1e300, y1 = -1e300, z0 = 1e300, z1 = -1e300;
			std::vector<std::vector<V2>> loop2(loops.size());
			for (size_t i = 0; i < loops.size(); ++i)
			{
				for (int32_t v : loops[i])
				{
					const V3 q = toF(pm.p[v]);
					loop2[i].push_back(V2(q.y, q.z));
					y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); z0 = std::min(z0, q.z); z1 = std::max(z1, q.z);
				}
				for (size_t k = 0; k < loop2[i].size(); ++k) L2.seg.push_back({ loop2[i][k], loop2[i][(k + 1) % loop2[i].size()] });
			}
			const double inset = 0.5 * hv;
			Domain2D dom;
			MeshDomain2D([&](double y, double z) { return L2.Signed(V2(y, z)) + inset; }, y0 - 2 * hv, y1 + 2 * hv, z0 - 2 * hv, z1 + 2 * hv, hv, dom, 0.0);
			if (!dom.tris.empty())
			{
				// cap boundary loops (domain on the left)
				std::unordered_map<int32_t, int32_t> nxt;
				for (const auto& e : BoundaryEdges(dom.tris)) nxt[e[0]] = e[1];
				std::vector<std::vector<int32_t>> capLoops;
				std::unordered_map<int32_t, char> seen;
				for (const auto& kv : nxt)
				{
					if (seen.count(kv.first)) continue;
					std::vector<int32_t> lp;
					int32_t v = kv.first;
					while (!seen.count(v))
					{
						seen[v] = 1;
						lp.push_back(v);
						auto itn = nxt.find(v);
						if (itn == nxt.end()) break;
						v = itn->second;
					}
					if (lp.size() >= 3) capLoops.push_back(lp);
				}
				// match each side loop with the nearest cap loop
				std::vector<int> capFor(loops.size(), -1);
				std::vector<char> capUsed(capLoops.size(), 0);
				for (size_t i = 0; i < loops.size(); ++i)
				{
					double best = 3.0 * hv;
					int bj = -1;
					for (size_t j = 0; j < capLoops.size(); ++j)
					{
						if (capUsed[j]) continue;
						double worst = 0;
						const size_t step = std::max<size_t>(1, capLoops[j].size() / 12);
						for (size_t k = 0; k < capLoops[j].size(); k += step)
						{
							double dmin = 1e300;
							for (const V2& q : loop2[i]) dmin = std::min(dmin, Len(q - dom.rest[capLoops[j][k]]));
							worst = std::max(worst, dmin);
						}
						if (worst < best) { best = worst; bj = int(j); }
					}
					if (bj >= 0) { capFor[i] = bj; capUsed[bj] = 1; }
				}
				// the side faces run each loop one way; the cap must run it the other way
				bool flip = false;
				for (size_t i = 0; i < loops.size(); ++i)
				{
					if (capFor[i] < 0) continue;
					std::vector<V2> rev(loop2[i].rbegin(), loop2[i].rend());
					std::vector<V2> cq;
					for (int32_t v : capLoops[capFor[i]]) cq.push_back(dom.rest[v]);
					flip = (LoopArea(rev) > 0) != (LoopArea(cq) > 0);
					break;
				}
				const int32_t base = int32_t(pm.p.size());
				for (size_t k = 0; k < dom.rest.size(); ++k)
				{
					const double y = dom.rest[k].x, z = dom.rest[k].y;
					pm.AddVertex(fromF(V3(Hx(y, z), y, z)));
				}
				const double us = p.uvScale;
				auto uvOf = [&](int32_t v) { const V3 q = toF(pm.p[v]); return UV2{ q.y * us, q.z * us }; };
				auto addTri = [&](int32_t a, int32_t b, int32_t c)
				{
					if (flip) std::swap(b, c);
					if (a == b || b == c || a == c) return;
					EditTri e;
					e.v = { a, b, c };
					e.uv = { uvOf(a), uvOf(b), uvOf(c) };
					e.mat = matFresh;
					e.tag = 1;
					pm.tris.push_back(e);
				};
				for (const auto& t : dom.tris) addTri(base + t[0], base + t[1], base + t[2]);
				// zip each side loop to its cap loop (both with the domain on the left)
				for (size_t i = 0; i < loops.size(); ++i)
				{
					if (capFor[i] < 0) continue;
					matched[i] = 1;
					std::vector<int32_t> O(loops[i].rbegin(), loops[i].rend());
					std::vector<V2> O2(loop2[i].rbegin(), loop2[i].rend());
					std::vector<int32_t> I = capLoops[capFor[i]];
					if (flip) { std::reverse(O.begin(), O.end()); std::reverse(O2.begin(), O2.end()); }
					// O runs like the cap loop: flip keeps the side's direction reversed
					size_t j0 = 0;
					double bd = 1e300;
					for (size_t j = 0; j < I.size(); ++j)
					{
						const double d = Len(dom.rest[I[j]] - O2[0]);
						if (d < bd) { bd = d; j0 = j; }
					}
					std::rotate(I.begin(), I.begin() + j0, I.end());
					const size_t nO = O.size(), nI = I.size();
					size_t a = 0, b = 0;
					while (a < nO || b < nI)
					{
						const int32_t o0 = O[a % nO], o1 = O[(a + 1) % nO];
						const int32_t c0 = base + I[b % nI], c1 = base + I[(b + 1) % nI];
						bool advO;
						if (a >= nO) advO = false;
						else if (b >= nI) advO = true;
						else advO = Len(O2[(a + 1) % nO] - dom.rest[I[b % nI]]) < Len(O2[a % nO] - dom.rest[I[(b + 1) % nI]]);
						// in the cap's own orientation (addTri flips them with the cap)
						if (advO) { addTri(o0, o1, c0); ++a; }
						else { addTri(o0, c1, c0); ++b; }
					}
				}
			}
			// a loop without a cap (a thin part the grid missed): closed flat
			for (size_t i = 0; i < loops.size(); ++i)
			{
				if (matched[i]) continue;
				// a pinhole in the surface (or a loop the cap missed): closed flat with the
				// surface's own material and UVs
				std::vector<int32_t> rv(loops[i].rbegin(), loops[i].rend());
				std::unordered_map<int32_t, std::pair<UV2, int32_t>> at;
				for (const EditTri& t : pm.tris)
					for (int k = 0; k < 3; ++k)
						if (t.tag == 0) at.emplace(t.v[k], std::make_pair(t.uv[k], t.mat));
				std::vector<std::array<int32_t, 3>> tr;
				TriangulatePolygon(pm.p, rv, tr);
				for (const auto& t : tr)
				{
					EditTri e;
					e.v = { rv[t[0]], rv[t[1]], rv[t[2]] };
					e.mat = matFresh;
					for (int k = 0; k < 3; ++k)
					{
						auto ia2 = at.find(e.v[k]);
						if (ia2 != at.end()) { e.uv[k] = ia2->second.first; e.mat = ia2->second.second; }
					}
					e.tag = 2;
					pm.tris.push_back(e);
				}
			}
		}
		if (trace)
		{
			std::fprintf(stderr, "  piece %d: %zu tris before cap refine, %zu loops\n", piece, pm.tris.size(), loops.size());
			for (size_t i = 0; i < loops.size(); ++i)
			{
				V3 a(1e9, 1e9, 1e9), b(-1e9, -1e9, -1e9);
				for (int32_t v : loops[i]) { const V3 q = toF(pm.p[v]); a = V3(std::min(a.x, q.x), std::min(a.y, q.y), std::min(a.z, q.z)); b = V3(std::max(b.x, q.x), std::max(b.y, q.y), std::max(b.z, q.z)); }
				std::fprintf(stderr, "    loop %zu: %zu verts x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f matched %d\n", i, loops[i].size(), a.x, b.x, a.y, b.y, a.z, b.z, int(matched[i]));
			}
		}
		Refine(pm, [&](int32_t a, int32_t b)
		{
			const V3 fa = toF(pm.p[a]), fb = toF(pm.p[b]);
			const V3 d = fb - fa;
			const double xm = 0.5 * (fa.x + fb.x);
			if (std::abs(xm - B.xb) > reach + 0.5 * std::abs(d.x)) return false;
			return (d.x / hu) * (d.x / hu) + (d.y / hv) * (d.y / hv) + (d.z / hv) * (d.z / hv) > 1.3 * 1.3;
		}, maxTris);
		if (trace) std::fprintf(stderr, "  piece %d: %zu tris after\n", piece, pm.tris.size());
		// ---- deform: splinters peel, the pieces bend at the break and part
		const size_t n = pm.p.size();
		std::vector<float> spl(n, 0.0f), brk(n, 0.0f), lay(n, 0.0f);
		const double a = 0.5 * p.breakAngle * kPi / 180.0;
		const double phi = piece == 0 ? a : -a;
		const double zb = -0.5 * T;
		const double far = piece == 0 ? (B.xb + B.HL()) : (B.HL() - B.xb);
		for (size_t v = 0; v < n; ++v)
		{
			V3 q = toF(pm.p[v]);
			int k = layerOf(q.z);
			// a point in the band between two layers belongs to the one that reaches further
			{
				const double zb0 = -0.5 * T + T * k / L, zb1 = -0.5 * T + T * (k + 1) / L;
				int k2 = -1;
				if (k > 0 && q.z - zb0 < band) k2 = k - 1;
				else if (k < L - 1 && zb1 - q.z < band) k2 = k + 1;
				if (k2 >= 0)
				{
					const double h1 = BL(k, q.y), h2 = BL(k2, q.y);
					if (piece == 0 && q.x > std::min(h1, h2) + 0.1 * hv) k = h1 >= h2 ? k : k2;
					if (piece == 1 && q.x < std::max(h1, h2) - 0.1 * hv) k = h1 <= h2 ? k : k2;
				}
			}
			const Layer& ly = B.layers[k];
			lay[v] = L > 1 ? float(k) / float(L - 1) : 1.0f;
			brk[v] = float(1.0 - Sat(std::abs(q.x - Hx(q.y, q.z)) / std::max(4.0 * hv, 0.004)));
			{
				int i;
				const Bundle& bd = B.BundleAt(ly, Clamp(q.y, -B.HW(), B.HW()), &i);
				double s;
				if (piece == 0)
				{
					const double l0 = i > 0 ? ly.cr[i - 1].zs : -1e30, h0 = i < int(ly.cr.size()) ? ly.cr[i].zs : -1e30;
					const double start = std::max(l0, h0);
					s = start > -1e29 ? q.x - start : q.x - (B.xb + ly.shift - p.crackDepth);
				}
				else
				{
					const double l0 = i > 0 ? ly.cr[i - 1].ze : 1e30, h0 = i < int(ly.cr.size()) ? ly.cr[i].ze : 1e30;
					const double end = std::min(l0, h0);
					s = end < 1e29 ? end - q.x : (B.xb + ly.shift + p.crackDepth) - q.x;
				}
				if (s > 0)
				{
					q.z += ly.liftScale * bd.lift * s * s / Ls;
					q.y += bd.sway * s * s / Ls;
					spl[v] = float(Sat(s / std::max(std::abs(bd.d) + p.crackDepth, 1e-4)));
				}
			}
			if (two)
			{
				const double dx = q.x - B.xb, dz = q.z - zb;
				q.x = B.xb + dx * std::cos(phi) + dz * std::sin(phi);
				q.z = zb - dx * std::sin(phi) + dz * std::cos(phi);
				q.z -= far * std::sin(std::abs(phi)) * (p.breakAngle >= 0 ? 1.0 : -1.0);
				q.x += 0.5 * p.breakGap * (piece == 0 ? -1.0 : 1.0);
			}
			pm.p[v] = fromF(q);
		}
		GenMesh gm;
		const std::vector<int32_t> map = EmitEditMesh(pm, gm, nullptr, part);
		std::vector<int32_t> back(gm.points.size(), -1);
		for (size_t v = 0; v < n; ++v)
			if (map[v] >= 0) back[map[v]] = int32_t(v);
		for (int32_t cpy : SplitPinchVertices(gm)) back.push_back(back[cpy]);
		auto put = [&](const char* name, const std::vector<float>& src)
		{
			std::vector<float>& at = gm.Attr(name);
			for (size_t i = 0; i < gm.points.size(); ++i) at[i] = src[back[i]];
		};
		put("wood_splinter", spl);
		put("wood_break", brk);
		put("wood_layer", lay);
		out.mesh.Append(gm);
		++part;
	}
	out.mesh.FinishAttrs();
	out.pieces = pieces;
	for (const Layer& ly : B.layers)
		for (const Bundle& bd : ly.b)
			if (std::abs(bd.d) >= std::max(p.splinterLengthMin, 1e-6)) out.splinters++;
	out.ok = !out.mesh.faceSizes.empty();
	return out.ok;
}

} // namespace pdgen
