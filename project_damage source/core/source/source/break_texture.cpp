// переносим шум или текстуру на поверхность каттера и среза
// ProjectDamage break texture surfaces (EXPERIMENTAL) — GPL-3.0-or-later; see LICENSE.
//
// Breaker surface mode 1 ("Texture", recipe break-texture-v1): instead of the
// procedural height field on a fixed grid (pole-pinched UV sphere, stretched
// cells on steep noise), the break surface is
//   1. a UNIFORM base: an icosphere (sphere mode) or an even grid (plane
//      mode), plus the procedural macro shape x texMacro (no fine noise);
//   2. refined where it matters (conforming red-green splits): every
//      triangle with a corner / edge midpoint / centre inside the object to
//      the target edge (texEdge, auto: texScale / 60), and the triangles the
//      object's surface crosses - the exit line of the break - a further
//      texExit times finer; outside the object it stays coarse (that part
//      cuts nothing);
//   3. displaced along its normal by a grayscale texture (stb_image, any
//      JPG/PNG), sampled TRIPLANAR in object (mesh) space at
//      (p - texOffset) / texScale: continuous across breakers, no stretching
//      on steep parts; displacement = (h - 0.5) x texStrength x strength;
//   4. closed into the cutter: the plane sheet gets a skirt from its
//      boundary down (or up) to a cap beyond the object, the sphere is closed.
// Strip and custom breakers keep the procedural surface.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FALLBACK
#include "stb_image.h"

#include "manifold/manifold.h"
#include "break_texture.h"
#include "surface_noise.h"
#include "adaptive_refine.h"
#include "project_damage_bvh.h"
#include "project_damage_internal.h"
#include "obreaker.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace m = manifold;
using namespace project_damage;                                                       
PROJECT_DAMAGE_NS_MACRO_BEGIN
namespace {

std::mutex gImgMutex;
std::map<std::string, std::shared_ptr<DispImage>> gImgCache;

double Px(const DispImage& im, int x, int y)
{
	x %= im.w; if (x < 0) x += im.w;
	y %= im.h; if (y < 0) y += im.h;
	return im.v[(size_t)y * (size_t)im.w + (size_t)x];
}

double Bilinear(const DispImage& im, double u, double v)
{
	const double x = u * im.w - 0.5, y = v * im.h - 0.5;
	const double fx = std::floor(x), fy = std::floor(y);
	const int x0 = (int)fx, y0 = (int)fy;
	const double tx = x - fx, ty = y - fy;
	return (Px(im, x0, y0) * (1 - tx) + Px(im, x0 + 1, y0) * tx) * (1 - ty)
		+ (Px(im, x0, y0 + 1) * (1 - tx) + Px(im, x0 + 1, y0 + 1) * tx) * ty;
}

}               

// читаем карту высот и повторно используем уже загруженную
std::shared_ptr<const DispImage> LoadDispImage(const std::string& path)
{
	std::lock_guard<std::mutex> lock(gImgMutex);
	auto it = gImgCache.find(path);
	if (it != gImgCache.end()) return it->second;
	auto img = std::make_shared<DispImage>();
	int w = 0, h = 0, c = 0;
	unsigned char* data = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &c, 1);
	if (data != nullptr && w > 0 && h > 0)
	{
		img->w = w; img->h = h;
		img->v.resize((size_t)w * (size_t)h);
		for (size_t i = 0; i < img->v.size(); i++) img->v[i] = data[i] / 255.0f;
		img->ok = true;
	}
	if (data) stbi_image_free(data);
	gImgCache[path] = img;
	return img;
}

double SampleTriplanar(const DispImage& im, const EVec& p, const EVec& n, const EVec& offset, double scale)
{
	if (!im.ok) return 0.5;
	const EVec q = (p - offset) / std::max(1e-9, scale);
	double wx = std::pow(std::abs(n.x), 4.0), wy = std::pow(std::abs(n.y), 4.0), wz = std::pow(std::abs(n.z), 4.0);
	const double ws = wx + wy + wz;
	if (ws < 1e-12) { wx = wy = wz = 1.0 / 3.0; }
	else { wx /= ws; wy /= ws; wz /= ws; }
	return wx * Bilinear(im, q.y, q.z) + wy * Bilinear(im, q.x, q.z) + wz * Bilinear(im, q.x, q.y);
}

namespace {

struct Sheet { std::vector<EVec> p; std::vector<std::array<Int32, 3>> t; };

                                                                             
                                                                           
template <class Project, class Want>
void Refine(Sheet& s, Project project, Want want, int maxLevels, size_t maxTris)
{
	for (int level = 0; level < maxLevels && s.t.size() < maxTris; ++level)
	{
		std::vector<char> red(s.t.size(), 0);
		size_t n = 0;
		for (size_t i = 0; i < s.t.size(); ++i) if (want(s.t[i])) { red[i] = 1; ++n; }
		if (n == 0 || s.t.size() + 3 * n > maxTris) break;
		auto key = [](Int32 a, Int32 b) { return a < b ? (uint64_t(a) << 32) | uint32_t(b) : (uint64_t(b) << 32) | uint32_t(a); };
		std::unordered_map<uint64_t, Int32> mark;                                                  
		for (size_t i = 0; i < s.t.size(); ++i)
			if (red[i]) for (int k = 0; k < 3; ++k) mark[key(s.t[i][k], s.t[i][(k + 1) % 3])] = -1;
		for (bool changed = true; changed;)
		{
			changed = false;
			for (size_t i = 0; i < s.t.size(); ++i)
			{
				if (red[i]) continue;
				int c = 0;
				for (int k = 0; k < 3; ++k) c += mark.count(key(s.t[i][k], s.t[i][(k + 1) % 3])) ? 1 : 0;
				if (c >= 2)
				{
					red[i] = 1; changed = true;
					for (int k = 0; k < 3; ++k) mark[key(s.t[i][k], s.t[i][(k + 1) % 3])] = -1;
				}
			}
		}
		auto mid = [&](Int32 a, Int32 b) {
			Int32& m_ = mark[key(a, b)];
			if (m_ < 0) { m_ = (Int32)s.p.size(); s.p.push_back(project((s.p[(size_t)a] + s.p[(size_t)b]) * 0.5)); }
			return m_;
		};
		std::vector<std::array<Int32, 3>> next;
		                                                                          
		size_t predicted = 0;
		for (const auto& tri : s.t) {
			int edges = 0;
			for (int k = 0; k < 3; ++k) edges += mark.count(key(tri[k], tri[(k+1)%3])) ? 1 : 0;
			predicted += edges == 3 ? 4 : edges == 0 ? 1 : 2;
		}
		if (predicted > maxTris) break;
		next.reserve(s.t.size() + 3 * n);
		for (size_t i = 0; i < s.t.size(); ++i)
		{
			const auto T = s.t[i];
			bool e[3]; int c = 0;
			for (int k = 0; k < 3; ++k) { e[k] = mark.count(key(T[k], T[(k + 1) % 3])) > 0; c += e[k]; }
			if (c == 0) { next.push_back(T); continue; }
			if (c == 3)
			{
				const Int32 m0 = mid(T[0], T[1]), m1 = mid(T[1], T[2]), m2 = mid(T[2], T[0]);
				next.push_back({ T[0], m0, m2 }); next.push_back({ m0, T[1], m1 });
				next.push_back({ m2, m1, T[2] }); next.push_back({ m0, m1, m2 });
				continue;
			}
			int k = 0; while (!e[k]) ++k;
			const Int32 a = T[k], b = T[(k + 1) % 3], c2 = T[(k + 2) % 3], mm = mid(a, b);
			next.push_back({ a, mm, c2 }); next.push_back({ mm, b, c2 });
		}
		s.t.swap(next);
	}
}

double Longest(const Sheet& s, const std::array<Int32, 3>& t)
{
	const EVec& a = s.p[(size_t)t[0]]; const EVec& b = s.p[(size_t)t[1]]; const EVec& c = s.p[(size_t)t[2]];
	return std::max({ m::la::length(b - a), m::la::length(c - b), m::la::length(a - c) });
}

}               

// формируем каттер по карте высот с нужной плотностью сетки
m::Manifold BuildTextureBreakCutter(const BreakerSpec& s, Float ampScale, const EVec& bbMin, const EVec& bbMax,
	const std::function<bool(const EVec&)>& insideMesh, std::string* log)
{
	const bool sphere = s.mode == BREAKER_MODE_SPHERE;
	if (!sphere && s.mode != BREAKER_MODE_PLANE) return m::Manifold();
	const std::shared_ptr<const DispImage> img = LoadDispImage(s.texPath);
	if (log && !img->ok) *log = "[ProjectDamage] breaker texture not readable: " + s.texPath + " (flat surface)";
	const double scale = std::max(1e-6, (double)s.texScale);
	const double target = s.texEdge > 0.0 ? (double)s.texEdge : scale / 60.0;
	const double exitF = std::max(1.0, (double)s.texExit);
	const double L = sphere ? 2.0 * s.radius : std::max(s.length, s.width);
	const double objDiag = m::la::length(bbMax - bbMin);
	const double H0 = std::max(target, std::min(L / 24.0, objDiag / 16.0));
	const EMat inv = InverseAffine(s.rel);
	const m::mat3 Ainv{ m::vec3(inv[0]), m::vec3(inv[1]), m::vec3(inv[2]) };
	const m::mat3 Nm = m::la::transpose(Ainv);                                        

	                  
	Sheet sh;
	auto onSphere = [&](const EVec& q) { const double l = m::la::length(q); return l > 1e-12 ? q * (s.radius / l) : q; };
	auto onPlane = [&](const EVec& q) { return EVec(q.x, 0.0, q.z); };
	if (sphere)
	{
		const double t = (1.0 + std::sqrt(5.0)) / 2.0;
		const EVec v[12] = { {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
			{0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1} };
		for (const EVec& q : v) sh.p.push_back(onSphere(q));
		const int f[20][3] = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
			{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
		for (const auto& q : f) sh.t.push_back({ q[0], q[1], q[2] });
		Refine(sh, onSphere, [&](const std::array<Int32, 3>& tr) { return Longest(sh, tr) > H0; }, 12, 4000000);
	}
	else
	{
		const int nx = (int)std::clamp(std::ceil(s.length / H0), 2.0, 512.0), nz = (int)std::clamp(std::ceil(s.width / H0), 2.0, 512.0);
		for (int i = 0; i <= nx; i++)
			for (int j = 0; j <= nz; j++)
				sh.p.push_back(EVec(-0.5 * s.length + s.length * i / nx, 0.0, -0.5 * s.width + s.width * j / nz));
		auto vid = [nz](int i, int j) { return (Int32)(i * (nz + 1) + j); };
		for (int i = 0; i < nx; i++)
			for (int j = 0; j < nz; j++)
			{
				                                                             
				if (((i + j) & 1) == 0)
				{
					sh.t.push_back({ vid(i, j), vid(i, j + 1), vid(i + 1, j) });
					sh.t.push_back({ vid(i + 1, j), vid(i, j + 1), vid(i + 1, j + 1) });
				}
				else
				{
					sh.t.push_back({ vid(i, j), vid(i + 1, j + 1), vid(i + 1, j) });
					sh.t.push_back({ vid(i, j), vid(i, j + 1), vid(i + 1, j + 1) });
				}
			}
	}

	                                                                   
	std::vector<signed char> in;                                                 
	auto insideV = [&](Int32 v) {
		if ((size_t)v >= in.size()) in.resize(sh.p.size(), -1);
		if (in[(size_t)v] < 0) in[(size_t)v] = insideMesh(MulPoint(s.rel, sh.p[(size_t)v])) ? 1 : 0;
		return in[(size_t)v] == 1;
	};
	auto insideP = [&](const EVec& q) { return insideMesh(MulPoint(s.rel, q)); };
	auto project = [&](const EVec& q) { return sphere ? onSphere(q) : onPlane(q); };
	const size_t maxTris = 1500000;
	for (int pass = 0; pass < 2; ++pass)
	{
		const double want = pass == 0 ? target : target / exitF;
		Refine(sh, project, [&](const std::array<Int32, 3>& tr) {
			const double e = Longest(sh, tr);
			if (e <= want) return false;
			int nin = 0;
			for (int k = 0; k < 3; ++k) nin += insideV(tr[k]) ? 1 : 0;
			const bool crossing = nin > 0 && nin < 3;
			if (pass == 1) return crossing;
			if (nin > 0) return true;
			                                                                
			const EVec& a = sh.p[(size_t)tr[0]]; const EVec& b = sh.p[(size_t)tr[1]]; const EVec& c = sh.p[(size_t)tr[2]];
			return insideP(project((a + b + c) / 3.0)) || insideP(project((a + b) * 0.5))
				|| insideP(project((b + c) * 0.5)) || insideP(project((c + a) * 0.5));
		}, 16, maxTris);
	}

	                                                                        
	                                                                           
	                                                                    
	                                                                   
	                     
	const double ns = s.noiseScale;
	const Int64 sd = (Int64)s.seed;
	const double macroK = std::clamp((double)s.texMacro, 0.0, 4.0) * ampScale;
	const bool classic = BrClassicSurface(s);
	for (EVec& q : sh.p)
	{
		if (macroK <= 0.0) break;
		if (sphere)
		{
			const EVec d = q / std::max(1e-12, (double)m::la::length(q));
			const EVec pp = d * s.radius;
			if (!classic)
			{
				q = d * (s.radius * (1.0 + BrSurface3(pp, s, L, sd) * macroK));
				continue;
			}
			const double turb = BrTurbulence3(pp, 0.18 * L * ns, sd, 3) * s.turbH;
			const double macro = ((BrValueNoise3(pp, 0.50 * L * ns, sd + 50) - 0.5) * 2.0) * s.macroH;
			const double rough = BrFbm3(pp, 0.08 * L * ns, sd + 100, 4) * s.fineH;
			q = d * (s.radius * (1.0 + (macro + turb + rough) * macroK));
		}
		else
		{
			double turb = 0.0, macro = 0.0, rough = 0.0;
			if (classic)
			{
				turb = BrTurbulence(q.x, q.z, 0.18 * L * ns, sd, 3) * s.turbH * L;
				macro = ((BrValueNoise(q.x, q.z, 0.50 * L * ns, sd + 50) - 0.5) * 2.0) * s.macroH * L;
				rough = BrFbm(q.x, q.z, 0.08 * L * ns, sd + 100, 4) * s.fineH * L;
			}
			else
				macro = BrSurface2(q.x, q.z, s, L, sd) * L;
			const double distIn = std::min(s.length * 0.5 - std::abs(q.x), s.width * 0.5 - std::abs(q.z));
			const double t = std::clamp(distIn / (0.15 * L), 0.0, 1.0);
			const double wgt = 1.0 - t * t * (3.0 - 2.0 * t);
			const double rim = ((BrValueNoise(q.x, q.z, 0.28 * L * s.rimScale, sd + 200) - 0.5) * 2.0) * s.rimAmp * L * wgt;
			q.y = (macro + turb + rough + rim) * macroK;
		}
	}
	std::vector<EVec> nrm(sh.p.size(), EVec(0.0));
	for (const auto& tr : sh.t)
	{
		const EVec fn = m::la::cross(sh.p[(size_t)tr[1]] - sh.p[(size_t)tr[0]], sh.p[(size_t)tr[2]] - sh.p[(size_t)tr[0]]);
		for (int k = 0; k < 3; ++k) nrm[(size_t)tr[k]] += fn;
	}
	const double strength = (double)s.texStrength * ampScale;
	const EVec off(s.texOffX, s.texOffY, s.texOffZ);
	for (size_t i = 0; i < sh.p.size(); ++i)
	{
		EVec nl = nrm[i];
		const double l = m::la::length(nl);
		if (l < 1e-20) continue;
		nl = nl / l;
		                                                                    
		                                                                     
		                                                     
		if (sphere) { if (m::la::dot(nl, sh.p[i]) > 0.0) nl = nl * -1.0; }
		else if (s.flip) nl = nl * -1.0;
		const EVec pw = MulPoint(s.rel, sh.p[i]);
		EVec nw = Nm * nl;
		nw = nw / std::max(1e-20, (double)m::la::length(nw));
		const double h = SampleTriplanar(*img, pw, nw, off, scale);
		const EVec dw = nw * ((h - 0.5) * strength);                
		sh.p[i] = sh.p[i] + Ainv * dw;                                                
	}

	                     
	std::vector<EVec> verts = sh.p;
	std::vector<Int32> tris;
	tris.reserve(sh.t.size() * 3 + 64);
	for (const auto& tr : sh.t) { tris.push_back(tr[0]); tris.push_back(tr[1]); tris.push_back(tr[2]); }
	if (!sphere)
	{
		                                                                             
		std::unordered_map<uint64_t, int> use;
		auto ekey = [](Int32 a, Int32 b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
		for (const auto& tr : sh.t) for (int k = 0; k < 3; ++k) use[ekey(tr[k], tr[(k + 1) % 3])]++;
		std::unordered_map<Int32, Int32> nextOf;
		for (const auto& tr : sh.t)
			for (int k = 0; k < 3; ++k)
			{
				const Int32 a = tr[k], b = tr[(k + 1) % 3];
				if (!use.count(ekey(b, a))) nextOf[a] = b;
			}
		if (nextOf.empty()) return m::Manifold();
		std::vector<Int32> loop;
		Int32 start = nextOf.begin()->first, v = start;
		for (size_t guard = 0; guard <= nextOf.size(); ++guard)
		{
			loop.push_back(v);
			auto it = nextOf.find(v);
			if (it == nextOf.end()) return m::Manifold();
			v = it->second;
			if (v == start) break;
		}
		                                                                
		double minY = 1e300, maxY = -1e300, sMin = 1e300, sMax = -1e300;
		for (int c = 0; c < 8; c++)
		{
			const EVec corner((c & 1) ? bbMax.x : bbMin.x, (c & 2) ? bbMax.y : bbMin.y, (c & 4) ? bbMax.z : bbMin.z);
			const double ly = MulPoint(inv, corner).y;
			minY = std::min(minY, ly); maxY = std::max(maxY, ly);
		}
		for (const EVec& q : sh.p) { sMin = std::min(sMin, (double)q.y); sMax = std::max(sMax, (double)q.y); }
		const bool capAbove = !s.flip;
		double capY = capAbove ? maxY + 0.1 * L : minY - 0.1 * L;
		if (capAbove && capY < sMax) capY = sMax + 0.1 * L;
		if (!capAbove && capY > sMin) capY = sMin - 0.1 * L;
		const Int32 base = (Int32)verts.size();
		EVec cen(0.0);
		for (Int32 b : loop) { verts.push_back(EVec(sh.p[(size_t)b].x, capY, sh.p[(size_t)b].z)); cen += sh.p[(size_t)b]; }
		cen = cen / (double)loop.size();
		const Int32 capC = (Int32)verts.size();
		verts.push_back(EVec(cen.x, capY, cen.z));
		const size_t nb = loop.size();
		for (size_t k = 0; k < nb; ++k)
		{
			const Int32 a = loop[k], b = loop[(k + 1) % nb];
			const Int32 a2 = base + (Int32)k, b2 = base + (Int32)((k + 1) % nb);
			                                                                    
			tris.push_back(b); tris.push_back(a); tris.push_back(a2);
			tris.push_back(b); tris.push_back(a2); tris.push_back(b2);
			          
			tris.push_back(b2); tris.push_back(a2); tris.push_back(capC);
		}
	}
	                                         
	double vol = 0.0;
	for (size_t i = 0; i + 2 < tris.size(); i += 3)
		vol += m::la::dot(verts[(size_t)tris[i]], m::la::cross(verts[(size_t)tris[i + 1]], verts[(size_t)tris[i + 2]]));
	if (vol < 0.0)
		for (size_t i = 0; i + 2 < tris.size(); i += 3) std::swap(tris[i + 1], tris[i + 2]);
	m::MeshGL64 mesh;
	mesh.numProp = 3;
	for (const EVec& q : verts)
	{
		const EVec g = MulPoint(s.rel, q);
		mesh.vertProperties.push_back(g.x); mesh.vertProperties.push_back(g.y); mesh.vertProperties.push_back(g.z);
	}
	for (Int32 t : tris) mesh.triVerts.push_back((uint64_t)t);
	m::Manifold out(mesh);
	if (out.Status() != m::Manifold::Error::NoError) return m::Manifold();
	return out;
}

                                                                          
                                                                            
                                                                        
                                                                          
                                                                    
std::function<bool(const EVec&)> MakeInsideTest(const MeshView& view, std::shared_ptr<ProjectDamageBVH>& bvh)
{
	bvh = std::make_shared<ProjectDamageBVH>();
	bvh->Build(view);
	EVec lo(1e300), hi(-1e300);
	for (Int32 i = 0; i < view.npts; ++i) { lo = m::la::min(lo, view.pts[i]); hi = m::la::max(hi, view.pts[i]); }
	const double eps = 1e-7 * m::la::length(hi - lo) + 1e-9;
	std::shared_ptr<ProjectDamageBVH> keep = bvh;
	return [keep, eps](const EVec& p) {
		const EVec skew = m::la::normalize(EVec(0.5773, 0.6123, 0.5401));
		EVec o = p;
		int hits = 0;
		for (int guard = 0; guard < 256; ++guard)
		{
			double d = 0.0;
			int32_t poly = -1;
			if (!keep->Raycast(o, skew, 1e30, &d, &poly)) break;
			++hits;
			o = o + skew * (d + eps);
		}
		return (hits & 1) == 1;
	};
}

                                                                            
                            
static double SampleSpec(const DispImage& im, const TexSpec& t, const EVec& p, const EVec& n, const EVec& offset, double scale)
{
	if (t.proc) return pd_surface_noise::Height(p, offset, scale, t.detail, t.roughness, t.distortion);
	return SampleTriplanar(im, p, n, offset, scale);
}

m::Manifold TextureDisplaceSolid(const m::Manifold& solid, const TexSpec& t, std::string* log,
	const std::function<bool(const EVec&)>* inside)
{
	if (solid.IsEmpty() || t.depth <= 0.0) return solid;
	const auto image = LoadDispImage(t.proc ? std::string() : t.path);
	if (!t.proc && !image->ok)
	{
		if (log) *log = "[ProjectDamage] displacement texture not readable: " + t.path;
		return solid;
	}
	const double scale = std::max(1e-6, t.scale);
	const double target = (t.edge > 0.0 ? t.edge : t.proc ? scale / 16.0 : scale / 60.0) / std::max(1.0, t.exit);
	const auto bounds = solid.BoundingBox();
	const double diagonal = m::la::length(bounds.Size());
	                                                            
	const double floor = diagonal * std::sqrt(std::max<size_t>(1, solid.NumTri()) / 1000000.0);
	m::Manifold refined = solid.RefineToLength(std::max({ target, floor, 1e-6 }));
	m::MeshGL64 mesh = refined.GetMeshGL64();
	const size_t np = mesh.numProp;
	                                                                             
	std::vector<uint64_t> root(mesh.NumVert());
	for (size_t v = 0; v < root.size(); ++v) root[v] = v;
	for (size_t i = 0; i < mesh.mergeFromVert.size(); ++i) root[(size_t)mesh.mergeFromVert[i]] = mesh.mergeToVert[i];
	auto rootOf = [&](uint64_t v) { for (int g = 0; g < 64 && root[(size_t)v] != v; ++g) v = root[(size_t)v]; return v; };
	auto point = [&](uint64_t i) { return EVec(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]); };
	std::vector<EVec> normals(mesh.NumVert(), EVec(0.0));
	for (size_t k = 0; k < mesh.triVerts.size(); k += 3)
	{
		const uint64_t a = mesh.triVerts[k], b = mesh.triVerts[k + 1], c = mesh.triVerts[k + 2];
		const EVec n = m::la::cross(point(b) - point(a), point(c) - point(a));
		normals[(size_t)rootOf(a)] += n; normals[(size_t)rootOf(b)] += n; normals[(size_t)rootOf(c)] += n;
	}
	const EVec offset(t.offX, t.offY, t.offZ);
	std::vector<EVec> moved(mesh.NumVert());
	for (size_t i = 0; i < mesh.NumVert(); ++i)
	{
		const uint64_t r = rootOf(i);
		if ((size_t)r != i) continue;
		const EVec p = point(i);
		const double len = m::la::length(normals[i]);
		if (len < 1e-20 || (inside != nullptr && !(*inside)(p))) { moved[i] = p; continue; }
		const EVec n = normals[i] / len;             
		const double h = SampleSpec(*image, t, p, n, offset, scale);
		moved[i] = p - n * ((h - 0.5) * t.depth);
	}
	for (size_t i = 0; i < mesh.NumVert(); ++i)
	{
		const EVec q = moved[(size_t)rootOf(i)];
		for (int k = 0; k < 3; ++k) mesh.vertProperties[i * np + (size_t)k] = q[k];
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0 || result.Volume() <= 0)
	{
		if (log) *log = "[ProjectDamage] textured cutter invalid; reduce the texture depth. Original cutter retained.";
		return solid;
	}
	return result;
}

                                                                             
                                                                           
                                                                          
                                                                             
                                                                            
                                                                          
                                                                           
                                                                           
                                                                             
                                                                             
                                                                            
                                                                           
                                                                            
m::Manifold FlareBreakCutter(const m::Manifold& solid, const MeshView& view, double W, double B, std::string* log)
{
	if (solid.IsEmpty() || !(W > 0.0) || !(B > 0.0)) return solid;
	std::shared_ptr<ProjectDamageBVH> bvh;
	const auto inside = MakeInsideTest(view, bvh);
	                                                                                 
	const double floor = std::sqrt(std::max(0.0, solid.SurfaceArea()) / (0.433 * 800000.0));
	m::Manifold refined = solid.RefineToLength(std::max({ B / 8.0, floor, 1e-6 }));
	m::MeshGL64 mesh = refined.GetMeshGL64();
	const size_t np = mesh.numProp;
	std::vector<uint64_t> root(mesh.NumVert());
	for (size_t v = 0; v < root.size(); ++v) root[v] = v;
	for (size_t i = 0; i < mesh.mergeFromVert.size(); ++i) root[(size_t)mesh.mergeFromVert[i]] = mesh.mergeToVert[i];
	auto rootOf = [&](uint64_t v) { for (int g = 0; g < 64 && root[(size_t)v] != v; ++g) v = root[(size_t)v]; return v; };
	auto point = [&](uint64_t i) { return EVec(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]); };
	std::vector<EVec> normals(mesh.NumVert(), EVec(0.0));
	for (size_t k = 0; k < mesh.triVerts.size(); k += 3)
	{
		const uint64_t a = mesh.triVerts[k], b = mesh.triVerts[k + 1], c = mesh.triVerts[k + 2];
		const EVec n = m::la::cross(point(b) - point(a), point(c) - point(a));
		normals[(size_t)rootOf(a)] += n; normals[(size_t)rootOf(b)] += n; normals[(size_t)rootOf(c)] += n;
	}
	static const double s3 = 0.57735026919;
	const EVec dirs[14] = { EVec(1, 0, 0), EVec(-1, 0, 0), EVec(0, 1, 0), EVec(0, -1, 0), EVec(0, 0, 1), EVec(0, 0, -1),
		EVec(s3, s3, s3), EVec(-s3, s3, s3), EVec(s3, -s3, s3), EVec(s3, s3, -s3), EVec(-s3, -s3, s3), EVec(-s3, s3, -s3), EVec(s3, -s3, -s3), EVec(-s3, -s3, -s3) };
	                                                              
	std::vector<EVec> fn((size_t)view.npolys);
	for (Int32 k = 0; k < view.npolys; ++k)
	{
		const ProjectDamagePoly& q = view.polys[k];
		EVec c = m::la::cross(view.pts[q.b] - view.pts[q.a], view.pts[q.c] - view.pts[q.a]);
		if (q.c != q.d) c += m::la::cross(view.pts[q.c] - view.pts[q.a], view.pts[q.d] - view.pts[q.a]);
		const double l = m::la::length(c);
		fn[(size_t)k] = l > 1e-20 ? c / l : EVec(0, 0, 1);
	}
	std::vector<EVec> moved(mesh.NumVert());
	for (size_t i = 0; i < mesh.NumVert(); ++i)
	{
		if ((size_t)rootOf(i) != i) continue;
		const EVec p = point(i);
		moved[i] = p;
		const double len = m::la::length(normals[i]);
		if (len < 1e-20) continue;
		const EVec nc = normals[i] / len;
		                                                                 
		double near = 2.0 * B;
		int32_t nearPoly = -1;
		for (const EVec& d : dirs)
		{
			double dist = 0.0;
			int32_t poly = -1;
			if (bvh->Raycast(p, d, near, &dist, &poly)) { near = dist; nearPoly = poly; }
		}
		if (nearPoly < 0 || (size_t)nearPoly >= fn.size()) continue;                                     
		                                                                       
		                                                                
		                                                                         
		                                                                    
		                                                               
		double off = 0.0;
		if (inside(p))
		{
			const double f = 1.0 - std::min(near, B) / B;
			off = W * std::pow(f, 2.5);
		}
		else if (near <= B) off = W;
		else
		{
			const double x = std::clamp((near - B) / B, 0.0, 1.0);
			off = W * (1.0 - x * x * (3.0 - 2.0 * x));
		}
		if (off <= 0.0) continue;
		                                              
		                                                                      
		                                                                      
		                                                               
		const EVec ns = fn[(size_t)nearPoly];
		const EVec t = nc - ns * m::la::dot(nc, ns);
		moved[i] = p + t * off;
	}
	for (size_t i = 0; i < mesh.NumVert(); ++i)
	{
		const EVec q = moved[(size_t)rootOf(i)];
		for (int k = 0; k < 3; ++k) mesh.vertProperties[i * np + (size_t)k] = q[k];
	}
	m::Manifold result(mesh);
	if (result.Status() != m::Manifold::Error::NoError || result.NumTri() == 0 || result.Volume() <= 0)
	{
		if (log) *log = "[ProjectDamage] smooth slope made the cutter invalid; lower Smooth Slope. Original cutter retained.";
		return solid;
	}
	return result;
}

// добавляем рельеф только выбранным поверхностям результата
m::Manifold TextureDisplaceClassFaces(const m::Manifold& result, const std::vector<Int32>& ids,
	const TexSpec& t, std::string* log)
{
	if (result.IsEmpty() || ids.empty() || t.depth <= 0.0) return result;
	const auto image = t.proc ? std::make_shared<DispImage>() : LoadDispImage(t.path);
	if (!t.proc && !image->ok)
	{
		if (log) *log = "[ProjectDamage] displacement texture not readable: " + t.path;
		return result;
	}
	const double scale = std::max(1e-6, t.scale);
	                                                                     
	                                                                           
	                                                                    
	constexpr size_t triangleBudget = 48000;
	const std::unordered_set<Int32> selected(ids.begin(), ids.end());
	double target = (t.edge > 0.0 ? t.edge : scale / (t.proc ? 5.0 : 12.0)) / std::max(1.0, t.exit);
	{
		const m::MeshGL64 pre = result.GetMeshGL64();
		const size_t npp = pre.numProp;
		double area = 0.0;
		for (size_t r = 0; r + 1 < pre.runIndex.size() && r < pre.runOriginalID.size(); ++r)
		{
			if (!selected.count((Int32)pre.runOriginalID[r])) continue;
			for (size_t tri = pre.runIndex[r] / 3; tri < pre.runIndex[r + 1] / 3; ++tri)
			{
				EVec q[3];
				for (int k = 0; k < 3; ++k)
				{
					const size_t vi = (size_t)pre.triVerts[3 * tri + (size_t)k] * npp;
					q[k] = EVec(pre.vertProperties[vi], pre.vertProperties[vi + 1], pre.vertProperties[vi + 2]);
				}
				area += 0.5 * m::la::length(m::la::cross(q[1] - q[0], q[2] - q[0]));
			}
		}
		if (area <= 0.0) return result;
		// Leave room for conforming triangles and elongated triangles at patch borders.
		target = std::max(target, std::sqrt(area / (0.433 * (triangleBudget / 4.0))));
	}
	const m::Manifold refined = RefineNearDamage(
		result, {}, target, 0.0, 16, size_t(result.NumTri()) + triangleBudget, &ids);
	m::MeshGL64 mesh = refined.GetMeshGL64();
	const size_t np = mesh.numProp;
	std::vector<char> mark(mesh.NumTri(), 0);
	for (size_t r = 0; r + 1 < mesh.runIndex.size() && r < mesh.runOriginalID.size(); ++r)
		if (selected.count((Int32)mesh.runOriginalID[r]))
			for (size_t tri = mesh.runIndex[r] / 3; tri < mesh.runIndex[r + 1] / 3; ++tri) mark[tri] = 1;
	std::vector<uint64_t> root(mesh.NumVert());
	for (size_t v = 0; v < root.size(); ++v) root[v] = v;
	for (size_t i = 0; i < mesh.mergeFromVert.size(); ++i) root[(size_t)mesh.mergeFromVert[i]] = mesh.mergeToVert[i];
	auto rootOf = [&](uint64_t v) { for (int g = 0; g < 64 && root[(size_t)v] != v; ++g) v = root[(size_t)v]; return v; };
	auto point = [&](uint64_t i) { return EVec(mesh.vertProperties[i * np], mesh.vertProperties[i * np + 1], mesh.vertProperties[i * np + 2]); };
	std::vector<EVec> nrm(mesh.NumVert(), EVec(0.0));
	std::vector<char> other(mesh.NumVert(), 0), any(mesh.NumVert(), 0);
	for (size_t tri = 0; tri < mesh.NumTri(); ++tri)
	{
		const uint64_t a = mesh.triVerts[3 * tri], b = mesh.triVerts[3 * tri + 1], c = mesh.triVerts[3 * tri + 2];
		const uint64_t ra = rootOf(a), rb = rootOf(b), rc = rootOf(c);
		if (!mark[tri]) { other[(size_t)ra] = other[(size_t)rb] = other[(size_t)rc] = 1; continue; }
		const EVec n = m::la::cross(point(b) - point(a), point(c) - point(a));
		for (uint64_t r : { ra, rb, rc }) { nrm[(size_t)r] += n; any[(size_t)r] = 1; }
	}
	const EVec off(t.offX, t.offY, t.offZ);
	                                                                        
	                                                                      
	                                                                  
	std::vector<EVec> vp(mesh.NumVert());
	for (size_t v = 0; v < mesh.NumVert(); ++v) vp[v] = point(v);
	std::vector<ProjectDamagePoly> vf(mesh.NumTri());
	for (size_t tri = 0; tri < mesh.NumTri(); ++tri)
		vf[tri] = { (Int32)mesh.triVerts[3 * tri], (Int32)mesh.triVerts[3 * tri + 1], (Int32)mesh.triVerts[3 * tri + 2], (Int32)mesh.triVerts[3 * tri + 2] };
	ProjectDamageBVH bvh;
	bvh.Build(MeshView{ vp.data(), (Int32)vp.size(), vf.data(), (Int32)vf.size() });
	const double room = 2.0 * t.depth;
	std::vector<EVec> moved(mesh.NumVert());
	std::vector<char> move(mesh.NumVert(), 0);
	size_t count = 0;
	for (size_t v = 0; v < mesh.NumVert(); ++v)
	{
		if (rootOf(v) != v || !any[v] || other[v]) continue;
		const double l = m::la::length(nrm[v]);
		if (l < 1e-20) continue;
		const EVec n = nrm[v] / l;                         
		const EVec p = point(v);
		double d = 0.0;
		int32_t hitPoly = -1;
		double fade = 1.0;
		if (bvh.Raycast(p + n * (1e-4 * t.depth + 1e-7), n, room, &d, &hitPoly)) fade = std::clamp(d / room, 0.0, 1.0);
		fade = fade * fade * (3.0 - 2.0 * fade);
		if (fade <= 0.0) continue;
		moved[v] = p + n * ((SampleSpec(*image, t, p, n, off, scale) - 0.5) * t.depth * fade);
		move[v] = 1;
		++count;
	}
	if (count == 0) return result;
	for (size_t v = 0; v < mesh.NumVert(); ++v)
	{
		const uint64_t r = rootOf(v);
		if (!move[(size_t)r]) continue;
		for (int k = 0; k < 3; ++k) mesh.vertProperties[v * np + (size_t)k] = moved[(size_t)r][k];
	}
	m::Manifold out(mesh);
	if (out.Status() != m::Manifold::Error::NoError || out.IsEmpty())
	{
		if (log) *log = "[ProjectDamage] textured back faces invalid; texture skipped";
		return result;
	}
	return out;
}

TexSpec TexSpecOf(const BreakerSpec& s, Float ampScale)
{
	TexSpec t;
	t.path = s.texPath;
	t.scale = s.texScale;
	t.depth = (double)s.texStrength * ampScale;
	t.offX = s.texOffX; t.offY = s.texOffY; t.offZ = s.texOffZ;
	t.edge = s.texEdge;
	t.exit = s.texExit;
	return t;
}

m::Manifold TextureDisplaceCutter(const m::Manifold& cutter, const BreakerSpec& s, Float ampScale, std::string* log)
{
	if (ampScale <= 0 || s.texStrength <= 0) return cutter;
	return TextureDisplaceSolid(cutter, TexSpecOf(s, ampScale), log);
}

PROJECT_DAMAGE_NS_MACRO_END
