// прокладываем царапины вдоль нарисованных штрихов
// ProjectDamage scratches draw — GPL-3.0-or-later; see LICENSE.
//
// Recipe: scratches-draw-v1. Scratches scattered around grease pencil strokes:
// Single mode (mode 1) skips steps 1-3 and 5: every stroke becomes exactly
// one scratch along its own path, snapped point by point to the surface,
// with the style's groove (step 4) and optional scuff band (step 6).
//   1. strokes: every stroke is resampled by arc length. It gets about
//      density x length scratches (all strokes together are capped at
//      maxScratches, shared out by length). A dot (a stroke with no length)
//      gets a small ring of randomly turned scratches;
//   2. placement: a scratch picks a point on its stroke, snaps it to the
//      surface, and walks sideways over the surface by up to `spread`
//      (crowded toward the stroke by `falloff`). Its heading is the stroke
//      direction projected on the surface, turned by a Gaussian angle of
//      `angleJitter` degrees, or a random angle for `crossShare` of them;
//   3. path: the scratch is walked over the surface from its centre both
//      ways, half its length each. Every step is re-snapped with a ray down
//      the last normal, so it follows curved faces. It stops where the
//      surface turns by more than `wrapAngle`, at an avoided polygon, or on
//      a miss. The heading bends by `curve` and wavers by `wobble`;
//   4. groove: a closed slab along the path with a V or U cross section.
//      Width and depth taper in at both ends, a gouge fades from a deep
//      start, and the depth fades in and out along `gaps` noise. Where the
//      depth fades out the slab floor lifts above the surface, so a scratch
//      stops and restarts without any separate pieces;
//   5. bundles: some scratches are several parallel fine lines, each offset
//      sideways on the surface, trimmed at its own ends;
//   6. scuff band (optional): a paper-thin flat strip under each scratch
//      that only carries its own selection.
// The centre lines and half widths come back as ScratchLine for the per-
// vertex mask (ScratchMaskWeights) after the Boolean.
//
// Channel map, per scratch key: 0-2 placement, 3-5 angle, 6-9 size, 10-12
// noise offset, 13-14 bundle, 15 along, 16 bend, 100 + b*8 bundle line b.
#include "manifold/manifold.h"
#include "scratches_draw.h"
#include "project_damage_bvh.h"
#include "project_damage_host.h"
#include "project_damage_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace m = manifold;
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

using V = m::vec3;

double Random01(uint64_t seed, uint64_t candidate, uint64_t channel)
{
	uint64_t z = seed ^ (candidate * 0x9e3779b97f4a7c15ULL)
		^ ((channel + 1) * 0xd1b54a32d192ed03ULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return double(z >> 11) * (1.0 / 9007199254740992.0);
}

                                                                    
uint64_t Mix(uint64_t seed, uint64_t a, uint64_t b)
{
	uint64_t z = seed ^ (a * 0x9e3779b97f4a7c15ULL) ^ ((b + 1) * 0xc2b2ae3d27d4eb4fULL);
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

double Gauss(double u1, double u2)
{
	return std::sqrt(-2.0 * std::log(std::max(1e-12, u1))) * std::cos(2.0 * m::kPi * u2);
}

V Unit(const V& v, const V& fallback)
{
	const double len = m::la::length(v);
	return len > 1e-12 ? v / len : fallback;
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double ClampD(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double Smooth(double e0, double e1, double x)
{
	if (e1 <= e0) return x >= e0 ? 1.0 : 0.0;
	const double t = ClampD((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double Fbm(V q, int octaves)
{
	double sum = 0.0, amp = 1.0, norm = 0.0;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * (2.0 * ProjectDamageNoise(q) - 1.0);
		norm += amp;
		amp *= 0.5;
		q = q * 2.03 + V(17.13, -9.31, 5.77);
	}
	return sum / norm;
}

V AnyPerp(const V& n)
{
	const V a = std::abs(n.x) < 0.7 ? V(1, 0, 0) : V(0, 1, 0);
	return Unit(m::la::cross(n, a), V(0, 0, 1));
}

                                                     
V Rotate(const V& v, const V& n, double a)
{
	const double c = std::cos(a), s = std::sin(a);
	return v * c + m::la::cross(n, v) * s + n * (m::la::dot(n, v) * (1.0 - c));
}

V Tangential(const V& d, const V& n, const V& fallback)
{
	return Unit(d - n * m::la::dot(d, n), fallback);
}

                                                                       
m::Manifold Orient(m::MeshGL& mesh)
{
	m::Manifold solid(mesh);
	if (solid.Status() == m::Manifold::Error::NoError && solid.Volume() < 0.0) {
		for (size_t k = 0; k < mesh.triVerts.size(); k += 3) std::swap(mesh.triVerts[k + 1], mesh.triVerts[k + 2]);
		solid = m::Manifold(mesh);
	}
	if (solid.Status() != m::Manifold::Error::NoError) return m::Manifold();
	return solid;
}

void PushV(m::MeshGL& mesh, const V& q)
{
	mesh.vertProperties.push_back(float(q.x));
	mesh.vertProperties.push_back(float(q.y));
	mesh.vertProperties.push_back(float(q.z));
}

void Quad(m::MeshGL& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	mesh.triVerts.insert(mesh.triVerts.end(), {a, b, c, a, c, d});
}

                                                                        
                                                                         
                                                         
m::Manifold SlabSolid(const std::vector<V>& top, const std::vector<V>& bot, int ns, int nw)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.reserve(size_t(ns + 1) * size_t(nw + 1) * 6);
	for (int i = 0; i <= ns; ++i)
		for (int j = 0; j <= nw; ++j) {
			PushV(mesh, bot[size_t(i * (nw + 1) + j)]);
			PushV(mesh, top[size_t(i * (nw + 1) + j)]);
		}
	auto B = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j)); };
	auto T = [&](int i, int j) { return uint32_t(2 * (i * (nw + 1) + j) + 1); };
	for (int i = 0; i < ns; ++i)
		for (int j = 0; j < nw; ++j) {
			Quad(mesh, T(i, j), T(i + 1, j), T(i + 1, j + 1), T(i, j + 1));
			Quad(mesh, B(i, j), B(i, j + 1), B(i + 1, j + 1), B(i + 1, j));
		}
	for (int i = 0; i < ns; ++i) {
		Quad(mesh, B(i, 0), B(i + 1, 0), T(i + 1, 0), T(i, 0));
		Quad(mesh, B(i, nw), T(i, nw), T(i + 1, nw), B(i + 1, nw));
	}
	for (int j = 0; j < nw; ++j) {
		Quad(mesh, B(0, j), T(0, j), T(0, j + 1), B(0, j + 1));
		Quad(mesh, B(ns, j), B(ns, j + 1), T(ns, j + 1), T(ns, j));
	}
	return Orient(mesh);
}

                                                                                  

struct Surf {
	const MeshView* v = nullptr;
	std::vector<V> polyN;
	ProjectDamageBVH bvh;
	double diag = 1.0;
	const std::vector<bool>* avoid = nullptr;

	bool Avoided(int32_t poly) const
	{
		return avoid && poly >= 0 && size_t(poly) < avoid->size() && (*avoid)[size_t(poly)];
	}
	bool Ray(const V& P, const V& dir, double maxDist, double& d, int32_t& poly) const
	{
		poly = -1;
		return bvh.Raycast(P, dir, maxDist, &d, &poly) && poly >= 0;
	}
	                                                                         
	                                                                         
	                             
	bool NormalNear(const V& P, double snap, V& N, V& S, int32_t& hitPoly) const
	{
		static const V dirs[13] = {
			V(1, 0, 0), V(0, 1, 0), V(0, 0, 1),
			V(1, 1, 0), V(1, -1, 0), V(1, 0, 1), V(1, 0, -1), V(0, 1, 1), V(0, 1, -1),
			V(1, 1, 1), V(1, 1, -1), V(1, -1, 1), V(-1, 1, 1)};
		double best = 1e300;
		bool ok = false;
		for (const V& d0 : dirs) {
			const V d = Unit(d0, V(0, 0, 1));
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				const V dir = d * double(sgn);
				double t = 0.0;
				int32_t poly = -1;
				if (!Ray(P - dir * snap, dir, 2.0 * snap, t, poly)) continue;
				const V& pn = polyN[size_t(poly)];
				const double graze = std::abs(m::la::dot(pn, dir));
				if (graze < 0.15) continue;
				const double score = std::abs(t - snap) / graze;
				if (score < best) { best = score; N = pn; S = P - dir * snap + dir * t; hitPoly = poly; ok = true; }
			}
		}
		return ok && best <= 1.5 * snap;
	}
	                                                                       
	                                                         
	bool Drop(const V& P, const V& n, double rise, double cosWrap, V& S, V& N, int32_t& poly) const
	{
		double t = 0.0;
		if (!Ray(P + n * rise, -n, 2.0 * rise, t, poly)) return false;
		const V& pn = polyN[size_t(poly)];
		if (m::la::dot(pn, n) < cosWrap) return false;
		S = P + n * rise - n * t;
		N = pn;
		return true;
	}
};

// готовим поверхность для поиска места повреждения
void BuildSurf(const CoreInput& in, bool useAvoid, Surf& s)
{
	const MeshView& v = in.view;
	s.v = &v;
	s.polyN.assign(size_t(v.npolys), V(0, 0, 1));
	for (int32_t i = 0; i < v.npolys; ++i) {
		const ProjectDamagePoly& q = v.polys[i];
		V cr = m::la::cross(v.pts[q.b] - v.pts[q.a], v.pts[q.c] - v.pts[q.a]);
		if (q.c != q.d) cr = cr + m::la::cross(v.pts[q.c] - v.pts[q.a], v.pts[q.d] - v.pts[q.a]);
		s.polyN[size_t(i)] = GuidedNormal(v, i, Unit(cr, V(0, 0, 1)));
	}
	s.bvh.Build(v);
	V lo(1e300), hi(-1e300);
	for (int32_t i = 0; i < v.npts; ++i) { lo = m::la::min(lo, v.pts[i]); hi = m::la::max(hi, v.pts[i]); }
	s.diag = std::max(1e-6, m::la::length(hi - lo));
	s.avoid = useAvoid ? in.avoidPoly : nullptr;
}

                                                                                      

struct WalkPt { V P, N; };

                                                                           
                                                                         
                                                                          
                                                             
template <typename TurnFn>
double Walk(const Surf& s, V S, V N, V d, double len, double step, double cosWrap, TurnFn turn,
	std::vector<WalkPt>& out)
{
	double walked = 0.0;
	d = Tangential(d, N, AnyPerp(N));
	while (walked < len - 1e-9) {
		const double h = std::min(step, len - walked);
		d = Rotate(d, N, turn(walked) * h);
		V S2, N2;
		int32_t poly = -1;
		if (!s.Drop(S + d * h, N, 1.5 * h + 1e-4, cosWrap, S2, N2, poly)) break;
		if (s.Avoided(poly)) break;
		const double moved = m::la::length(S2 - S);
		if (!(moved > 0.2 * h) || moved > 3.0 * h) break;
		walked += h;
		d = Tangential(S2 - S, N2, d);
		S = S2;
		N = N2;
		out.push_back({S, N});
	}
	return walked;
}

                                                                                         

                                                                         
                                                                           
                                                           
struct Row { V S, N, side; double hw, c, h1; };

void GrooveSolid(const std::vector<Row>& rows, double e, double top, std::vector<m::Manifold>& out)
{
	if (rows.size() < 2) return;
	const int ns = int(rows.size()) - 1, nw = 4;
	std::vector<V> topv, botv;
	topv.reserve(rows.size() * 5);
	botv.reserve(rows.size() * 5);
	static const double us[5] = {-1.0, -0.5, 0.0, 0.5, 1.0};
	for (const Row& r : rows) {
		const double hs[5] = {e, r.h1, r.c, r.h1, e};
		for (int j = 0; j <= nw; ++j) {
			const V L = r.S + r.side * (us[j] * r.hw);
			botv.push_back(L + r.N * hs[j]);
			topv.push_back(L + r.N * top);
		}
	}
	m::Manifold c = SlabSolid(topv, botv, ns, nw);
	if (!c.IsEmpty()) out.push_back(std::move(c));
}

struct Scratch {
	uint64_t key = 0;
	V S, N, dir;
	double len = 1.0, width = 0.05, depth = 0.02, bend = 0.0;
};

struct Ctx {
	const ScratchDrawParams* p = nullptr;
	const Surf* s = nullptr;
	double cosWrap = 0.8;
};

struct Built { std::vector<m::Manifold> grooves, scuffs; std::vector<ScratchLine> lines; };

                                                                       
double TipEnv(double t, double taper)
{
	if (taper <= 1e-6) return 1.0;
	return std::pow(Smooth(0.0, taper, t) * Smooth(0.0, taper, 1.0 - t), 0.6);
}

void CarvePath(const Ctx& cx, const Scratch& K, const std::vector<WalkPt>& path, bool single, Built& out);

void BuildScratch(const Ctx& cx, const Scratch& K, Built& out)
{
	const ScratchDrawParams& p = *cx.p;
	const Surf& s = *cx.s;
	auto R = [&](uint64_t ch) { return Random01(K.key, 0, ch); };

	                                                                                
	const double step = ClampD(K.len / (28.0 * p.detail), 0.01, 0.5);
	const V noff(R(10) * 1000.0, R(11) * 1000.0, R(12) * 1000.0);
	const double wob = p.wobble * 6.0 / K.len;
	auto turnF = [&](double sgn) {
		return [&, sgn](double a) {
			const double sArc = 0.5 * K.len + sgn * a;                                 
			return sgn * (K.bend / K.len + wob * Fbm(noff + V(sArc / K.len * 3.0, 0.3, 0.0), 2));
		};
	};
	std::vector<WalkPt> fwd, bwd;
	Walk(s, K.S, K.N, K.dir, 0.5 * K.len, step, cx.cosWrap, turnF(1.0), fwd);
	Walk(s, K.S, K.N, -K.dir, 0.5 * K.len, step, cx.cosWrap, turnF(-1.0), bwd);
	std::vector<WalkPt> path;
	path.reserve(fwd.size() + bwd.size() + 1);
	for (size_t i = bwd.size(); i-- > 0;) path.push_back(bwd[i]);
	path.push_back({K.S, K.N});
	for (const WalkPt& w : fwd) path.push_back(w);
	CarvePath(cx, K, path, false, out);
}

                                                                            
                              
void CarvePath(const Ctx& cx, const Scratch& K, const std::vector<WalkPt>& path, bool single, Built& out)
{
	const ScratchDrawParams& p = *cx.p;
	const Surf& s = *cx.s;
	auto R = [&](uint64_t ch) { return Random01(K.key, 0, ch); };
	const V noff(R(10) * 1000.0, R(11) * 1000.0, R(12) * 1000.0);
	if (path.size() < 3) return;

	std::vector<double> arc(path.size(), 0.0);
	for (size_t i = 1; i < path.size(); ++i) arc[i] = arc[i - 1] + m::la::length(path[i].P - path[i - 1].P);
	const double total = arc.back();
	if (!(total > 2.0 * K.width)) return;

	std::vector<V> side(path.size());
	for (size_t i = 0; i < path.size(); ++i) {
		const V a = path[i > 0 ? i - 1 : i].P, b = path[i + 1 < path.size() ? i + 1 : i].P;
		const V t = Tangential(b - a, path[i].N, AnyPerp(path[i].N));
		side[i] = Unit(m::la::cross(path[i].N, t), AnyPerp(path[i].N));
	}

	                                                                                     
	int nLines = 1;
	if (!single && R(13) < p.bundleChance) {
		const int lo = std::max(2, p.bundleMin), hi = std::max(lo, p.bundleMax);
		nLines = lo + int(std::floor(R(14) * (hi - lo + 1 - 1e-9)));
	}
	const double taper = ClampD(p.taper, 0.0, 0.5);
	const double gouge = ClampD(p.gouge, 0.0, 1.0);
	const double gaps = ClampD(p.gaps, 0.0, 1.0);

	for (int b = 0; b < nLines; ++b) {
		auto RB = [&](uint64_t ch) { return Random01(K.key, 1 + uint64_t(b), 100 + ch); };
		double off = 0.0, wS = 1.0, dS = 1.0, t0 = 0.0, t1 = 1.0;
		if (nLines > 1) {
			off = (double(b) - 0.5 * (nLines - 1)) * p.bundleGap * Lerp(0.75, 1.25, RB(0));
			wS = p.bundleThin * Lerp(0.6, 1.2, RB(1));
			dS = p.bundleThin * Lerp(0.5, 1.2, RB(2));
			t0 = 0.25 * RB(3);
			t1 = 1.0 - 0.25 * RB(4);
		}
		const double hwN = 0.5 * K.width * wS;                          
		const double dN = K.depth * dS;                             
		const double e = 0.15 * dN + 0.05 * hwN;                                    
		const double lift = 0.2 * dN;                                                         
		const double top = e + 2.0 * hwN + dN;
		const V goff = noff + V(37.1 * (b + 1), 11.3, -5.9 * b);
		const double gScale = std::max(4.0 * hwN, 0.12 * K.len);

		std::vector<Row> rows;
		ScratchLine line;
		auto flush = [&]() {
			GrooveSolid(rows, e, top, out.grooves);
			if (p.scuffEnable && rows.size() >= 2) {
				std::vector<Row> band = rows;
				const double sd = std::max(1e-5, p.scuffDepth);
				for (Row& r : band) {
					const double f = ClampD(r.hw / std::max(1e-9, hwN), 0.0, 1.0);
					r.hw = std::max(1e-4, r.hw * std::max(1.0, p.scuffWidth));
					r.c = -sd * f + 0.5 * sd * (1.0 - f);
					r.h1 = r.c;
				}
				GrooveSolid(band, 0.5 * sd, 0.5 * sd + 3.0 * sd + 0.25 * hwN, out.scuffs);
			}
			if (line.pts.size() >= 2) out.lines.push_back(std::move(line));
			rows.clear();
			line = ScratchLine();
		};
		for (size_t i = 0; i < path.size(); ++i) {
			const double tg = arc[i] / total;
			if (tg < t0 || tg > t1) continue;
			const double t = (tg - t0) / std::max(1e-9, t1 - t0);
			V S = path[i].P, N = path[i].N;
			if (off != 0.0) {
				int32_t poly = -1;
				V S2, N2;
				if (!s.Drop(S + side[i] * off, N, std::abs(off) + 4.0 * hwN + 1e-4, cx.cosWrap, S2, N2, poly)
					|| s.Avoided(poly)) {
					flush();
					continue;
				}
				S = S2;
				N = N2;
			}
			                                                                     
			double f = TipEnv(t, taper) * (1.0 - gouge * Smooth(0.15, 1.0, t) * 0.85);
			if (gaps > 0.0) {
				const double g = Fbm(goff + V(arc[i] / gScale, 0.0, 0.0), 2);
				const double thr = -0.75 + 1.3 * gaps;
				f *= Smooth(thr - 0.1, thr + 0.1, g);
			}
			const double rough = 1.0 + 0.2 * Fbm(goff + V(arc[i] / (3.0 * hwN + 1e-9), 4.1, 0.0), 2);
			const double hw = std::max(1e-4, hwN * (0.35 + 0.65 * std::sqrt(f)) * rough);
			const double c = -dN * f * rough + lift * (1.0 - f);
			const double h1 = p.profile == 1 ? Lerp(c, e, 0.12) : 0.5 * (c + e);
			rows.push_back({S, N, side[i], hw, c, h1});
			if (f > 0.05) {
				line.pts.push_back(S);
				line.halfWidth.push_back(hw * std::min(1.0, 1.5 * f));
			} else if (!line.pts.empty()) {
				out.lines.push_back(std::move(line));
				line = ScratchLine();
			}
		}
		flush();
	}
}

                                      
struct Stroke {
	std::vector<V> pts;
	std::vector<double> cum;
	double len = 0.0;
	V At(double a) const
	{
		if (pts.size() == 1) return pts[0];
		a = ClampD(a, 0.0, len);
		const size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), a) - cum.begin());
		const size_t k = std::min(std::max<size_t>(i, 1), pts.size() - 1);
		const double seg = cum[k] - cum[k - 1];
		const double f = seg > 1e-12 ? (a - cum[k - 1]) / seg : 0.0;
		return pts[k - 1] + (pts[k] - pts[k - 1]) * f;
	}
};

}             

void PlaceScratches(const CoreInput& in, const ScratchDrawParams& pIn, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* strokes,
	std::vector<m::Manifold>& grooves, std::vector<m::Manifold>& scuffs,
	std::vector<ScratchLine>* lines)
{
	if (!pIn.enable || !strokes || strokes->empty()) return;
	if (!in.view.pts || in.view.npts < 3 || !in.view.polys || in.view.npolys < 1) return;

	ScratchDrawParams p = pIn;
	ScratchDrawLook(p, p.style);                                                           
	const double sz = ClampD(std::isfinite(p.sizeScale) ? p.sizeScale : 1.0, 0.01, 100.0);
	p.lengthMin *= sz;
	p.lengthMax *= sz;
	p.width *= sz;
	p.depth *= sz;
	p.bundleGap *= sz;
	p.scuffDepth *= sz;
	p.detail = ClampD(p.detail, 0.05, 4.0);
	p.density = std::max(0.0, p.density);
	p.maxScratches = std::max(0, std::min(p.maxScratches, 20000));
	p.spread = std::max(0.0, p.spread);
	p.falloff = ClampD(p.falloff, 0.2, 8.0);
	p.lengthMin = std::max(1e-3, p.lengthMin);
	p.lengthMax = std::max(p.lengthMin, p.lengthMax);
	p.width = std::max(1e-4, p.width);
	p.depth = std::max(1e-5, p.depth);
	p.paintSnap = std::max(1e-4, p.paintSnap);
	p.bundleMin = std::max(2, std::min(p.bundleMin, 16));
	p.bundleMax = std::max(p.bundleMin, std::min(p.bundleMax, 16));
	p.bundleGap = std::max(0.0, p.bundleGap);
	p.bundleThin = ClampD(p.bundleThin, 0.05, 2.0);
	const bool singleMode = p.mode == kScratchSingle;
	if (!singleMode && (p.density <= 0.0 || p.maxScratches == 0)) return;

	Surf s;
	BuildSurf(in, p.avoid, s);
	Ctx cx;
	cx.p = &p;
	cx.s = &s;
	cx.cosWrap = std::cos(ClampD(p.wrapAngle, 1.0, 89.0) * m::kPi / 180.0);

	const uint64_t seed = (uint64_t(uint32_t(mainSeed)) << 32) ^ uint64_t(uint32_t(p.seed)) ^ 0x5C2A7C4E5ULL;

	                                                                                         
	std::vector<Stroke> sk;
	for (const auto& poly : *strokes) {
		Stroke st;
		for (const EVec& q : poly)
			if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)) st.pts.push_back(q);
		if (st.pts.empty()) { sk.push_back(st); continue; }
		st.cum.assign(st.pts.size(), 0.0);
		for (size_t i = 1; i < st.pts.size(); ++i) st.cum[i] = st.cum[i - 1] + m::la::length(st.pts[i] - st.pts[i - 1]);
		st.len = st.cum.back();
		sk.push_back(std::move(st));
	}

	                                                                                   
	if (singleMode) {
		std::vector<Built> built(sk.size());
		ProjectDamageParallelFor(0, int64_t(sk.size()), [&](int64_t i) {
			const Stroke& st = sk[size_t(i)];
			if (st.pts.size() < 2 || !(st.len > 2.0 * p.width)) return;
			Scratch K;
			K.key = Mix(seed, uint64_t(i) + 1, 0x51A6);
			auto R = [&](uint64_t ch) { return Random01(K.key, 0, ch); };
			double ms = 1.0;
			if (mask) {
				V Nm, Sm;
				int32_t pm = -1;
				if (s.NormalNear(st.At(0.5 * st.len), p.paintSnap, Nm, Sm, pm)) {
					if (!mask->Keep(Sm, int32_t(i & 0x7fffffff), 0x5D)) return;
					ms = mask->SizeScale(Sm);
				}
				if (!(ms > 0.0)) return;
			}
			K.len = st.len;
			K.width = p.width * (1.0 + ClampD(p.widthVar, 0.0, 1.0) * (2.0 * R(7) - 1.0) * 0.3) * ms;
			K.depth = p.depth * (1.0 + ClampD(p.depthVar, 0.0, 1.0) * (2.0 * R(8) - 1.0) * 0.3) * ms;
			const double step = ClampD(st.len / (28.0 * p.detail), 0.01, 0.25 / std::sqrt(p.detail));
			const int n = std::max(2, int(std::ceil(st.len / step)));
			const double h = st.len / n;
			std::vector<WalkPt> path;
			auto flush = [&]() {
				if (path.size() >= 3) CarvePath(cx, K, path, true, built[size_t(i)]);
				path.clear();
			};
			for (int k = 0; k <= n; ++k) {
				V N, S;
				int32_t poly = -1;
				if (!s.NormalNear(st.At(h * k), p.paintSnap, N, S, poly) || s.Avoided(poly)) {
					flush();
					continue;
				}
				                                                                    
				if (!path.empty() && (m::la::dot(N, path.back().N) < cx.cosWrap
					|| m::la::length(S - path.back().P) > 3.0 * h + p.paintSnap * 0.1))
					flush();
				path.push_back({S, N});
			}
			flush();
		});
		for (Built& b : built) {
			for (auto& g : b.grooves) grooves.push_back(std::move(g));
			for (auto& g : b.scuffs) scuffs.push_back(std::move(g));
			if (lines)
				for (auto& l : b.lines) lines->push_back(std::move(l));
		}
		return;
	}
	std::vector<int64_t> count(sk.size(), 0);
	int64_t total = 0;
	for (size_t i = 0; i < sk.size(); ++i) {
		if (sk[i].pts.empty()) continue;
		                                                                  
		const double L = sk[i].len > 1e-6 ? sk[i].len : std::max(0.5, 2.0 * m::kPi * p.spread * 0.5);
		count[i] = std::max<int64_t>(1, int64_t(std::llround(p.density * L)));
		total += count[i];
	}
	if (total > p.maxScratches) {
		const double k = double(p.maxScratches) / double(total);
		for (auto& c : count) c = c > 0 ? std::max<int64_t>(1, int64_t(std::floor(double(c) * k))) : 0;
	}

	std::vector<std::pair<int32_t, int32_t>> jobs;                     
	for (size_t i = 0; i < sk.size(); ++i)
		for (int64_t j = 0; j < count[i]; ++j) jobs.push_back({int32_t(i), int32_t(j)});

	std::vector<Built> built(jobs.size());
	const double jit = std::max(0.0, p.angleJitter) * m::kPi / 180.0;
	const double bias = ClampD(p.lengthBias, 0.0, 1.0);
	const double lenExp = std::pow(4.0, 2.0 * bias - 1.0);                              

	ProjectDamageParallelFor(0, int64_t(jobs.size()), [&](int64_t q) {
		const Stroke& st = sk[size_t(jobs[size_t(q)].first)];
		Scratch K;
		K.key = Mix(seed, uint64_t(jobs[size_t(q)].first) + 1, uint64_t(jobs[size_t(q)].second));
		auto R = [&](uint64_t ch) { return Random01(K.key, 0, ch); };

		                             
		const bool dot = st.len <= 1e-6;
		const double a = R(0) * st.len;
		const V Q = st.At(a);
		V T(0.0);
		if (!dot) {
			const double h = ClampD(0.02 * st.len, std::min(0.25, 0.5 * st.len), std::max(0.5, p.spread));
			T = st.At(a + h) - st.At(a - h);
		}
		V N, S;
		int32_t poly = -1;
		if (!s.NormalNear(Q, p.paintSnap, N, S, poly) || s.Avoided(poly)) return;
		const V T0 = Tangential(T, N, AnyPerp(N));
		const bool onLine = !dot && R(15) < ClampD(p.along, 0.0, 1.0);

		                                 
		if (!onLine && p.spread > 0.0) {
			const V side = m::la::cross(N, T0) * (R(2) < 0.5 ? -1.0 : 1.0);
			const double dist = p.spread * std::pow(R(1), p.falloff);
			if (dot) {
				                              
				const V dd = Rotate(T0, N, 2.0 * m::kPi * R(2));
				std::vector<WalkPt> w;
				Walk(s, S, N, dd, std::max(dist, 0.3 * p.spread), std::max(1e-3, p.spread / 10.0), cx.cosWrap,
					[](double) { return 0.0; }, w);
				if (!w.empty()) { S = w.back().P; N = w.back().N; }
			} else if (dist > 1e-6) {
				std::vector<WalkPt> w;
				Walk(s, S, N, side, dist, std::max(1e-3, p.spread / 10.0), cx.cosWrap, [](double) { return 0.0; }, w);
				if (!w.empty()) { S = w.back().P; N = w.back().N; }
			}
		}
		const uint64_t maskIdx = uint64_t(q);
		if (mask && !mask->Keep(S, int32_t(maskIdx & 0x7fffffff), 0x5C)) return;
		const double ms = mask ? mask->SizeScale(S) : 1.0;
		if (!(ms > 0.0)) return;

		          
		double ang = 0.0;
		if (dot || R(3) < ClampD(p.crossShare, 0.0, 1.0)) ang = m::kPi * (2.0 * R(4) - 1.0);
		else if (!onLine) ang = ClampD(Gauss(R(4), R(5)) * jit, -m::kPi, m::kPi);
		K.dir = Rotate(Tangential(T0, N, AnyPerp(N)), N, ang);
		K.S = S;
		K.N = N;

		       
		K.len = Lerp(p.lengthMin, p.lengthMax, std::pow(R(6), lenExp)) * ms;
		K.width = p.width * (1.0 + ClampD(p.widthVar, 0.0, 1.0) * (2.0 * R(7) - 1.0) * 0.7) * ms;
		K.depth = p.depth * (1.0 + ClampD(p.depthVar, 0.0, 1.0) * (2.0 * R(8) - 1.0) * 0.7) * ms;
		K.bend = p.curve * (2.0 * R(16) - 1.0);
		if (onLine) K.len = std::min(K.len * 1.5, std::max(p.lengthMax, 0.5 * st.len));
		BuildScratch(cx, K, built[size_t(q)]);
	});

	for (Built& b : built) {
		for (auto& g : b.grooves) grooves.push_back(std::move(g));
		for (auto& g : b.scuffs) scuffs.push_back(std::move(g));
		if (lines)
			for (auto& l : b.lines) lines->push_back(std::move(l));
	}
}

void ScratchMaskWeights(const std::vector<ScratchLine>& lines, const EVec* pts, int32_t npts,
	double falloff, std::vector<double>& out)
{
	out.assign(size_t(std::max(0, npts)), 0.0);
	if (!pts || npts <= 0 || lines.empty()) return;
	falloff = std::max(1e-6, falloff);

	struct Seg { V a, b; double ha, hb; };
	std::vector<Seg> segs;
	double maxH = 0.0, segSum = 0.0;
	V lo(1e300), hi(-1e300);
	for (const ScratchLine& l : lines)
		for (size_t i = 0; i + 1 < l.pts.size() && i + 1 < l.halfWidth.size(); ++i) {
			segs.push_back({l.pts[i], l.pts[i + 1], l.halfWidth[i], l.halfWidth[i + 1]});
			maxH = std::max(maxH, std::max(l.halfWidth[i], l.halfWidth[i + 1]));
			segSum += m::la::length(l.pts[i + 1] - l.pts[i]);
			lo = m::la::min(lo, m::la::min(l.pts[i], l.pts[i + 1]));
			hi = m::la::max(hi, m::la::max(l.pts[i], l.pts[i + 1]));
		}
	if (segs.empty()) return;
	const double reach = maxH + falloff;
	lo = lo - V(reach);
	hi = hi + V(reach);
	double cell = std::max(reach, segSum / double(segs.size()));
	const V ext = hi - lo;
	                               
	while ((ext.x / cell + 1.0) * (ext.y / cell + 1.0) * (ext.z / cell + 1.0) > 4.0e6) cell *= 1.5;
	const int nx = int(ext.x / cell) + 1, ny = int(ext.y / cell) + 1, nz = int(ext.z / cell) + 1;
	auto CellOf = [&](const V& q, int& x, int& y, int& z) {
		x = int(std::floor((q.x - lo.x) / cell));
		y = int(std::floor((q.y - lo.y) / cell));
		z = int(std::floor((q.z - lo.z) / cell));
	};
	std::vector<std::vector<int32_t>> grid(size_t(nx) * size_t(ny) * size_t(nz));
	for (size_t k = 0; k < segs.size(); ++k) {
		const V a = m::la::min(segs[k].a, segs[k].b) - V(reach), b = m::la::max(segs[k].a, segs[k].b) + V(reach);
		int x0, y0, z0, x1, y1, z1;
		CellOf(a, x0, y0, z0);
		CellOf(b, x1, y1, z1);
		x0 = std::max(0, x0); y0 = std::max(0, y0); z0 = std::max(0, z0);
		x1 = std::min(nx - 1, x1); y1 = std::min(ny - 1, y1); z1 = std::min(nz - 1, z1);
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					grid[(size_t(z) * ny + size_t(y)) * nx + size_t(x)].push_back(int32_t(k));
	}
	ProjectDamageParallelFor(0, int64_t(npts), [&](int64_t i) {
		const V P = pts[i];
		int x, y, z;
		CellOf(P, x, y, z);
		if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) return;
		double best = 1e300;
		for (int32_t k : grid[(size_t(z) * ny + size_t(y)) * nx + size_t(x)]) {
			const Seg& g = segs[size_t(k)];
			const V ab = g.b - g.a;
			const double l2 = m::la::dot(ab, ab);
			const double t = l2 > 1e-18 ? ClampD(m::la::dot(P - g.a, ab) / l2, 0.0, 1.0) : 0.0;
			const double d = m::la::length(P - (g.a + ab * t)) - Lerp(g.ha, g.hb, t);
			best = std::min(best, d);
		}
		out[size_t(i)] = best <= 0.0 ? 1.0 : 1.0 - Smooth(0.0, falloff, best);
	});
}

PROJECT_DAMAGE_NS_MACRO_END
