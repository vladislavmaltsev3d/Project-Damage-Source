// строим каркас внутри выреза, сохраняя размеры досок и креплений
// ProjectDamage break framing - GPL-3.0-or-later; see LICENSE.
//
// Recipe break-framing-v1. How the frame is laid out:
//
//  1. Faces. The source's triangles near the breaks are grown into flat
//     panels (neighbours within `facetAngle` of the seed). A panel steeper
//     than 45 degrees is a wall (members run up its slope: studs, rafters),
//     a flatter one a deck, floor or ceiling (members run across its shorter
//     span: joists). Panels too narrow to hold a member behind both skins
//     (the top and the ends of a thin wall) are skipped; the panels on either
//     side frame them.
//  2. Members per panel, in the panel's own 2D frame, `skin` below its
//     surface: plates / rims / headers / jack studs along every edge of the
//     panel and of its openings (top plates and headers doubled), studs or
//     joists at `studSpacing` in the area left, noggins / blocking in rows
//     between them, staggered so each can be nailed. Stud positions are
//     anchored at the object origin, so the two faces of a wall put theirs at
//     the same places; a member that would leave a thin gap to the far skin
//     runs through to it, and the copy from the far face is dropped.
//  3. Breaks. A member crossing a hole snaps (`woodBroken`) with splintered
//     ends; pieces nothing holds are dropped. Nails stick out of the members'
//     faces where the board came off.
//  4. Carve. The cavity (the source inset by `skin`, `cavityReach` around the
//     breaks) and a torn band of face paper around each hole are returned for
//     the caller to subtract from the damaged solid.

#include "manifold/manifold.h"
#include "manifold/cross_section.h"

#include "break_framing.h"
#include "project_damage_bvh.h"
#include "framing_nail_anchor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
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

uint64_t KeyOf(double v)
{
	uint64_t b;
	std::memcpy(&b, &v, sizeof b);
	return b;
}

EVec Normalized(const EVec& v, const EVec& fallback)
{
	const double l = m::la::length(v);
	return l > 1e-12 ? v / l : fallback;
}

                                                                                     

                                                   
class SolidTest
{
public:
	void Build(const m::Manifold& solid)
	{
		const m::MeshGL64 mesh = solid.GetMeshGL64();
		const size_t nv = mesh.NumVert();
		pts_.resize(nv);
		lo_ = EVec(std::numeric_limits<double>::max());
		hi_ = EVec(std::numeric_limits<double>::lowest());
		for (size_t i = 0; i < nv; i++)
		{
			pts_[i] = EVec(mesh.vertProperties[i * mesh.numProp + 0],
				mesh.vertProperties[i * mesh.numProp + 1], mesh.vertProperties[i * mesh.numProp + 2]);
			lo_ = m::la::min(lo_, pts_[i]);
			hi_ = m::la::max(hi_, pts_[i]);
		}
		polys_.resize(mesh.NumTri());
		for (size_t t = 0; t < polys_.size(); t++)
		{
			const int32_t a = (int32_t)mesh.triVerts[t * 3], b = (int32_t)mesh.triVerts[t * 3 + 1],
				c = (int32_t)mesh.triVerts[t * 3 + 2];
			polys_[t] = { a, b, c, c };
		}
		bvh_.Build(MeshView{ pts_.data(), (int32_t)pts_.size(), polys_.data(), (int32_t)polys_.size() });
		eps_ = 1e-7 * std::max(1.0, m::la::length(hi_ - lo_));
		valid_ = !polys_.empty();
	}

	bool Valid() const { return valid_; }
	const EVec& Lo() const { return lo_; }
	const EVec& Hi() const { return hi_; }
	const std::vector<EVec>& Points() const { return pts_; }
	const std::vector<ProjectDamagePoly>& Tris() const { return polys_; }

	bool FirstHit(const EVec& org, const EVec& dir, double maxDist, double& t) const
	{
		return valid_ && bvh_.Raycast(org, dir, maxDist, &t);
	}

	bool Inside(const EVec& p) const
	{
		if (!valid_ || p.x < lo_.x || p.y < lo_.y || p.z < lo_.z || p.x > hi_.x || p.y > hi_.y || p.z > hi_.z)
			return false;
		static const EVec dirs[3] = {
			Normalized(EVec(0.5773, 0.5779, 0.5768), EVec(0, 0, 1)),
			Normalized(EVec(-0.6211, 0.3352, 0.7083), EVec(0, 0, 1)),
			Normalized(EVec(0.2917, -0.8102, -0.5084), EVec(0, 0, 1)) };
		int votes = 0;
		for (int k = 0; k < 3; k++)
		{
			votes += Parity(p, dirs[k]);
			if (votes >= 2) return true;
			if (votes + (2 - k) < 2) return false;
		}
		return votes >= 2;
	}

private:
	int Parity(const EVec& p, const EVec& d) const
	{
		int count = 0;
		EVec org = p;
		for (int i = 0; i < 256; i++)
		{
			double t = 0.0;
			if (!bvh_.Raycast(org, d, 1e30, &t))
				break;
			count++;
			org = org + d * (t + eps_);
		}
		return count & 1;
	}

	std::vector<EVec> pts_;
	std::vector<ProjectDamagePoly> polys_;
	ProjectDamageBVH bvh_;
	EVec lo_, hi_;
	double eps_ = 1e-7;
	bool valid_ = false;
};

                                                                                     
bool ExitAlong(const SolidTest& solid, const EVec& q, const EVec& dir, double& dist)
{
	bool inside = solid.Inside(q);
	const double eps = 1e-6 * std::max(1.0, m::la::length(solid.Hi() - solid.Lo()));
	double at = 0.0;
	EVec org = q;
	for (int i = 0; i < 64; i++)
	{
		double t = 0.0;
		if (!solid.FirstHit(org, dir, 1e30, t)) return false;
		at += t;
		if (inside) { dist = at; return true; }
		inside = true;
		org = q + dir * (at + eps);
		at += eps;
	}
	return false;
}

                                                           
class SurfaceDistance
{
public:
	void Build(const std::vector<EVec>& pts, const std::vector<ProjectDamagePoly>& polys)
	{
		const size_t nt = polys.size();
		tri_.resize(nt);
		std::vector<EVec> cen(nt);
		for (size_t t = 0; t < nt; t++)
		{
			tri_[t] = { pts[(size_t)polys[t].a], pts[(size_t)polys[t].b], pts[(size_t)polys[t].c] };
			cen[t] = (tri_[t].a + tri_[t].b + tri_[t].c) / 3.0;
		}
		order_.resize(nt);
		for (size_t i = 0; i < nt; i++)
			order_[i] = (int32_t)i;
		nodes_.clear();
		nodes_.reserve(nt / 2 + 1);
		if (nt != 0)
			BuildNode(0, (int32_t)nt, cen);
		std::vector<Tri> sorted(nt);
		for (size_t i = 0; i < nt; i++)
			sorted[i] = tri_[(size_t)order_[i]];
		tri_ = std::move(sorted);
		order_.clear();
	}

	double Distance(const EVec& p, double cap) const
	{
		if (nodes_.empty())
			return cap;
		double best2 = cap * cap;
		int32_t stack[128];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0)
		{
			const Node& nd = nodes_[(size_t)stack[--sp]];
			if (BoxDist2(nd, p) >= best2)
				continue;
			if (nd.left < 0)
			{
				for (int32_t i = nd.start; i < nd.start + nd.count; i++)
					best2 = std::min(best2, TriDist2(tri_[(size_t)i], p));
				continue;
			}
			const double dl = BoxDist2(nodes_[(size_t)nd.left], p), dr = BoxDist2(nodes_[(size_t)nd.right], p);
			if (sp + 2 > 128)
				continue;
			if (dl < dr) { stack[sp++] = nd.right; stack[sp++] = nd.left; }
			else         { stack[sp++] = nd.left;  stack[sp++] = nd.right; }
		}
		return std::sqrt(best2);
	}

private:
	struct Tri { EVec a, b, c; };
	struct Node
	{
		EVec lo, hi;
		int32_t left = -1, right = -1, start = 0, count = 0;
	};

	int32_t BuildNode(int32_t start, int32_t end, const std::vector<EVec>& cen)
	{
		const int32_t id = (int32_t)nodes_.size();
		nodes_.push_back(Node());
		Node nd;
		nd.lo = EVec(std::numeric_limits<double>::max());
		nd.hi = EVec(std::numeric_limits<double>::lowest());
		EVec clo = nd.lo, chi = nd.hi;
		for (int32_t i = start; i < end; i++)
		{
			const size_t t = (size_t)order_[(size_t)i];
			const Tri& tr = tri_[t];
			nd.lo = m::la::min(nd.lo, m::la::min(tr.a, m::la::min(tr.b, tr.c)));
			nd.hi = m::la::max(nd.hi, m::la::max(tr.a, m::la::max(tr.b, tr.c)));
			clo = m::la::min(clo, cen[t]);
			chi = m::la::max(chi, cen[t]);
		}
		if (end - start <= 6)
		{
			nd.start = start;
			nd.count = end - start;
			nodes_[(size_t)id] = nd;
			return id;
		}
		const EVec ext = chi - clo;
		const int ax = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
		const int32_t mid = (start + end) / 2;
		std::nth_element(order_.begin() + start, order_.begin() + mid, order_.begin() + end,
			[&](int32_t x, int32_t y) { return cen[(size_t)x][ax] < cen[(size_t)y][ax]; });
		nd.left = BuildNode(start, mid, cen);
		nd.right = BuildNode(mid, end, cen);
		nodes_[(size_t)id] = nd;
		return id;
	}

	static double BoxDist2(const Node& nd, const EVec& p)
	{
		double d2 = 0.0;
		for (int k = 0; k < 3; k++)
		{
			const double e = p[k] < nd.lo[k] ? nd.lo[k] - p[k] : (p[k] > nd.hi[k] ? p[k] - nd.hi[k] : 0.0);
			d2 += e * e;
		}
		return d2;
	}

	                                                                              
	static double TriDist2(const Tri& t, const EVec& p)
	{
		const EVec ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
		const double d1 = m::la::dot(ab, ap), d2 = m::la::dot(ac, ap);
		auto sq = [](const EVec& v) { return m::la::dot(v, v); };
		if (d1 <= 0.0 && d2 <= 0.0) return sq(ap);
		const EVec bp = p - t.b;
		const double d3 = m::la::dot(ab, bp), d4 = m::la::dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3) return sq(bp);
		const double vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
			return sq(p - (t.a + ab * (d1 / (d1 - d3))));
		const EVec cp = p - t.c;
		const double d5 = m::la::dot(ab, cp), d6 = m::la::dot(ac, cp);
		if (d6 >= 0.0 && d5 <= d6) return sq(cp);
		const double vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
			return sq(p - (t.a + ac * (d2 / (d2 - d6))));
		const double va = d3 * d6 - d5 * d4;
		if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
			return sq(p - (t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
		const double den = 1.0 / (va + vb + vc);
		return sq(p - (t.a + ab * (vb * den) + ac * (vc * den)));
	}

	std::vector<Tri> tri_;
	std::vector<int32_t> order_;
	std::vector<Node> nodes_;
};

                                                                                   

struct Sizes
{
	double w, d, joist, spacing, noggin, skin, reach, tear, paper, nailLen, broken, nails, facetCos;
	bool doublePlate;
	EVec up;
};

Sizes SizesOf(const BreakRebarParams& p)
{
	const double sc = std::clamp(p.scale, 0.01, 100.0);
	Sizes s;
	s.w = std::max(0.2, p.studWidth * sc);
	s.d = std::max(0.2, p.studDepth * sc);
	s.joist = std::max(0.2, p.joistDepth * sc);
	s.spacing = std::max(2.5 * s.w, p.studSpacing * sc);
	s.noggin = p.nogginSpacing > 0.0 ? std::max(3.0 * s.w, p.nogginSpacing * sc) : 0.0;
	s.skin = std::max(0.05, p.skin * sc);
	s.reach = std::max(0.0, p.cavityReach * sc);
	s.tear = std::max(0.0, p.paperTear * sc);
	s.paper = std::clamp(p.paperDepth * sc, 0.0, 0.8 * s.skin);
	// Preserve the default 12.5 mm drywall look independently of the current skin.
	s.nailLen = std::max(0.0, p.nailLength) * 1.25 * sc;
	s.broken = std::clamp(p.woodBroken, 0.0, 1.0);
	s.nails = std::clamp(p.nails, 0.0, 1.0);
	s.facetCos = std::cos(0.5 * std::clamp(p.facetAngle, 1.0, 90.0) * kPi / 180.0);                                   
	s.doublePlate = p.doublePlate;
	s.up = Normalized(EVec(p.upX, p.upY, p.upZ), EVec(0, 0, 1));
	return s;
}

struct Window
{
	EVec lo, hi;
	bool Hits(const EVec& a, const EVec& b) const                                   
	{
		return a.x <= hi.x && a.y <= hi.y && a.z <= hi.z && b.x >= lo.x && b.y >= lo.y && b.z >= lo.z;
	}
	bool Has(const EVec& q) const
	{
		return q.x >= lo.x && q.y >= lo.y && q.z >= lo.z && q.x <= hi.x && q.y <= hi.y && q.z <= hi.z;
	}
};

                                                                                       

struct Panel
{
	EVec n, A, B;                                                                                      
	double c = 0.0;                                                           
	m::CrossSection poly;                                    
	bool deck = false;                              
	double bsign = 1.0;                                                                                    
	uint64_t key = 0;
};

m::vec2 To2(const Panel& pn, const EVec& x) { return m::vec2(m::la::dot(x, pn.B), m::la::dot(x, pn.A)); }
EVec To3(const Panel& pn, const m::vec2& q, double depthFromPlane)
{
	return pn.B * q.x + pn.A * q.y + pn.n * (pn.c - depthFromPlane);
}

// ищем участки поверхности, на которых можно разместить каркас
std::vector<Panel> FindPanels(const m::Manifold& source, const SolidTest& solid, const Window& grow, const Sizes& sz, uint64_t seed)
{
	std::vector<Panel> panels;
	const m::MeshGL64 mesh = source.GetMeshGL64();
	const size_t nv = mesh.NumVert(), nt = mesh.NumTri();
	if (nt == 0) return panels;
	std::vector<EVec> pts(nv);
	for (size_t i = 0; i < nv; i++)
		pts[i] = EVec(mesh.vertProperties[i * mesh.numProp], mesh.vertProperties[i * mesh.numProp + 1],
			mesh.vertProperties[i * mesh.numProp + 2]);
	                                                                         
	std::vector<int32_t> weld(nv);
	{
		EVec lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
		for (const EVec& q : pts) { lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
		const double cell = std::max(1e-9, 1e-6 * m::la::length(hi - lo));
		std::unordered_map<uint64_t, int32_t> seen;
		seen.reserve(nv * 2);
		for (size_t i = 0; i < nv; i++)
		{
			const int64_t qx = (int64_t)std::llround(pts[i].x / cell), qy = (int64_t)std::llround(pts[i].y / cell),
				qz = (int64_t)std::llround(pts[i].z / cell);
			const uint64_t h = Key((uint64_t)qx, (uint64_t)qy, (uint64_t)qz);
			auto it = seen.find(h);
			if (it == seen.end()) { seen.emplace(h, (int32_t)i); weld[i] = (int32_t)i; }
			else weld[i] = it->second;
		}
	}
	std::vector<std::array<int32_t, 3>> tri(nt);
	std::vector<EVec> tn(nt);
	std::vector<double> ta(nt);
	std::vector<char> use(nt, 0);
	for (size_t t = 0; t < nt; t++)
	{
		for (int k = 0; k < 3; k++) tri[t][k] = (int32_t)mesh.triVerts[t * 3 + k];
		const EVec& a = pts[(size_t)tri[t][0]]; const EVec& b = pts[(size_t)tri[t][1]]; const EVec& c = pts[(size_t)tri[t][2]];
		const EVec cr = m::la::cross(b - a, c - a);
		ta[t] = 0.5 * m::la::length(cr);
		tn[t] = Normalized(cr, EVec(0, 0, 1));
		const EVec lo = m::la::min(a, m::la::min(b, c)), hi = m::la::max(a, m::la::max(b, c));
		use[t] = ta[t] > 1e-12 && grow.Hits(lo, hi);
	}
	std::unordered_map<uint64_t, std::vector<int32_t>> edges;
	edges.reserve(nt * 2);
	for (size_t t = 0; t < nt; t++)
	{
		if (!use[t]) continue;
		for (int k = 0; k < 3; k++)
		{
			int32_t a = weld[(size_t)tri[t][k]], b = weld[(size_t)tri[t][(k + 1) % 3]];
			if (a > b) std::swap(a, b);
			edges[((uint64_t)(uint32_t)a << 32) | (uint32_t)b].push_back((int32_t)t);
		}
	}
	std::vector<int32_t> order;
	for (size_t t = 0; t < nt; t++) if (use[t]) order.push_back((int32_t)t);
	std::stable_sort(order.begin(), order.end(), [&](int32_t x, int32_t y) { return ta[(size_t)x] > ta[(size_t)y]; });
	std::vector<int32_t> owner(nt, -1);
	const EVec up = sz.up;
	                                                                              
	EVec h1 = std::fabs(up.x) < 0.9 ? EVec(1, 0, 0) : EVec(0, 1, 0);
	h1 = Normalized(h1 - up * m::la::dot(h1, up), EVec(1, 0, 0));
	const EVec h2 = m::la::cross(up, h1);
	for (int32_t seedTri : order)
	{
		if (owner[(size_t)seedTri] >= 0) continue;
		const int32_t id = (int32_t)panels.size();
		const EVec sn = tn[(size_t)seedTri];
		std::vector<int32_t> members{ seedTri }, stack{ seedTri };
		owner[(size_t)seedTri] = id;
		while (!stack.empty())
		{
			const int32_t t = stack.back();
			stack.pop_back();
			for (int k = 0; k < 3; k++)
			{
				int32_t a = weld[(size_t)tri[(size_t)t][k]], b = weld[(size_t)tri[(size_t)t][(k + 1) % 3]];
				if (a > b) std::swap(a, b);
				auto it = edges.find(((uint64_t)(uint32_t)a << 32) | (uint32_t)b);
				if (it == edges.end()) continue;
				for (int32_t o : it->second)
					if (owner[(size_t)o] < 0 && m::la::dot(tn[(size_t)o], sn) >= sz.facetCos)
					{
						owner[(size_t)o] = id;
						members.push_back(o);
						stack.push_back(o);
					}
			}
		}
		EVec nsum(0.0);
		double area = 0.0;
		for (int32_t t : members) { nsum += tn[(size_t)t] * ta[(size_t)t]; area += ta[(size_t)t]; }
		                                                                            
		                                                                            
		{
			int votes = 0, asked = 0;
			for (size_t q = 0; q < members.size() && asked < 5; q += std::max<size_t>(1, members.size() / 5), asked++)
			{
				const int32_t t = members[q];
				const EVec cen = (pts[(size_t)tri[(size_t)t][0]] + pts[(size_t)tri[(size_t)t][1]] + pts[(size_t)tri[(size_t)t][2]]) / 3.0;
				votes += solid.Inside(cen + tn[(size_t)t] * (0.5 * sz.skin)) ? 1 : 0;
			}
			if (2 * votes > asked)
			{
				for (int32_t t : members) owner[(size_t)t] = -2;
				continue;
			}
		}
		Panel pn;
		pn.n = Normalized(nsum, sn);
		pn.deck = std::fabs(m::la::dot(pn.n, up)) >= 0.7071;
		if (!pn.deck)
		{
			pn.B = Normalized(m::la::cross(up, pn.n), h1);
			pn.A = m::la::cross(pn.n, pn.B);
		}
		else
		{
			double e1lo = 1e300, e1hi = -1e300, e2lo = 1e300, e2hi = -1e300;
			for (int32_t t : members)
				for (int k = 0; k < 3; k++)
				{
					const EVec& q = pts[(size_t)tri[(size_t)t][k]];
					const double u1 = m::la::dot(q, h1), u2 = m::la::dot(q, h2);
					e1lo = std::min(e1lo, u1); e1hi = std::max(e1hi, u1);
					e2lo = std::min(e2lo, u2); e2hi = std::max(e2hi, u2);
				}
			const EVec j = (e1hi - e1lo) <= (e2hi - e2lo) + 1e-6 ? h1 : h2;
			pn.A = Normalized(j - pn.n * m::la::dot(j, pn.n), m::la::cross(pn.n, h2));
			pn.B = m::la::cross(pn.A, pn.n);
		}
		{
			const double d1 = m::la::dot(pn.B, h1), d2 = m::la::dot(pn.B, h2), d3 = m::la::dot(pn.B, up);
			const double big = std::fabs(d1) >= std::fabs(d2) && std::fabs(d1) >= std::fabs(d3) ? d1
				: (std::fabs(d2) >= std::fabs(d3) ? d2 : d3);
			pn.bsign = big < 0.0 ? -1.0 : 1.0;
		}
		pn.c = std::numeric_limits<double>::max();
		m::Polygons polys;
		polys.reserve(members.size());
		for (int32_t t : members)
		{
			m::SimplePolygon sp(3);
			for (int k = 0; k < 3; k++)
			{
				const EVec& q = pts[(size_t)tri[(size_t)t][k]];
				pn.c = std::min(pn.c, m::la::dot(q, pn.n));
				sp[(size_t)k] = To2(pn, q);
			}
			                                                                     
			                                                               
			const double cr = (sp[1].x - sp[0].x) * (sp[2].y - sp[0].y) - (sp[1].y - sp[0].y) * (sp[2].x - sp[0].x);
			if (cr < 0.0) std::swap(sp[1], sp[2]);
			polys.push_back(std::move(sp));
		}
		pn.poly = m::CrossSection(polys, m::CrossSection::FillRule::NonZero).Simplify(0.05 * sz.w);
		pn.key = Key(seed, 0xFACE, (uint64_t)std::llround(pn.n.x * 1000.0), (uint64_t)std::llround(pn.n.y * 1000.0)
			^ ((uint64_t)std::llround(pn.n.z * 1000.0) << 20) ^ ((uint64_t)std::llround(pn.c * 10.0) << 40));
		if (area > 0.0 && !pn.poly.IsEmpty())
			panels.push_back(std::move(pn));
		else
			for (int32_t t : members) owner[(size_t)t] = -2;
	}
	return panels;
}

                                                                                       

enum MemberKind : int32_t { kPlate = 0, kEdge = 1, kStud = 2, kNoggin = 3 };

struct Member
{
	EVec o;                                                                                 
	EVec u, v, k;                                                     
	double len = 0.0, w = 0.0, d = 0.0;
	int32_t kind = kStud;
	int32_t panel = -1;
	uint64_t key = 0;
	double splA = 0.0, splB = 0.0;                                               
	double hair = 0.0;                                                                                
	bool loose = false;

	EVec Corner(int i) const
	{
		return o + u * ((i & 1) ? len : 0.0) + v * ((i & 2) ? 0.5 * w : -0.5 * w) + k * ((i & 4) ? d : 0.0);
	}
	EVec Mid(double s) const { return o + u * s + k * (0.5 * d); }
};

struct Seg2 { m::vec2 a, b; };

                                                                                 
std::vector<std::pair<double, double>> LineSpans(const m::Polygons& area, const m::vec2& org, const m::vec2& dir)
{
	std::vector<double> ts;
	const m::vec2 nrm(-dir.y, dir.x);
	for (const m::SimplePolygon& poly : area)
		for (size_t i = 0; i < poly.size(); i++)
		{
			const m::vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
			const double da = m::la::dot(a - org, nrm), db = m::la::dot(b - org, nrm);
			if ((da < 0.0) == (db < 0.0)) continue;
			const double f = da / (da - db);
			ts.push_back(m::la::dot(a + (b - a) * f - org, dir));
		}
	std::sort(ts.begin(), ts.end());
	std::vector<std::pair<double, double>> spans;
	for (size_t i = 0; i + 1 < ts.size(); i += 2)
		if (ts[i + 1] - ts[i] > 1e-9)
			spans.push_back({ ts[i], ts[i + 1] });
	return spans;
}

std::vector<std::pair<double, double>> Intersect(const std::vector<std::pair<double, double>>& x,
	const std::vector<std::pair<double, double>>& y)
{
	std::vector<std::pair<double, double>> r;
	size_t i = 0, j = 0;
	while (i < x.size() && j < y.size())
	{
		const double a = std::max(x[i].first, y[j].first), b = std::min(x[i].second, y[j].second);
		if (b > a) r.push_back({ a, b });
		if (x[i].second < y[j].second) i++; else j++;
	}
	return r;
}

                                                              
std::vector<std::pair<double, double>> StripSpans(const m::Polygons& area, const m::vec2& org, const m::vec2& dir, double w)
{
	const m::vec2 nrm(-dir.y, dir.x);
	std::vector<std::pair<double, double>> r = LineSpans(area, org, dir);
	r = Intersect(r, LineSpans(area, org + nrm * (0.49 * w), dir));
	r = Intersect(r, LineSpans(area, org - nrm * (0.49 * w), dir));
	return r;
}

m::SimplePolygon Rect2(const m::vec2& a, const m::vec2& b, double w)
{
	const m::vec2 t = m::la::normalize(b - a), nrm(-t.y, t.x);
	const double h = 0.5 * w;
	return { a - nrm * h, b - nrm * h, b + nrm * h, a + nrm * h };
}

struct Member2
{
	m::vec2 a, b;                              
	int32_t kind;
	double depth;                                    
	int32_t stack;                                                                     
};

                                                                               
                                                                    
bool SoftSeam(const Panel& pn, const SolidTest& solid, const m::vec2& mid, const m::vec2& in, const Sizes& sz)
{
	const double step = sz.skin + 2.0 * sz.w, lift = 2.0 * step;
	const EVec outward = -(pn.B * in.x + pn.A * in.y);
	auto height = [&](const EVec& onPlane, double& h)
	{
		double t = 0.0;
		if (!solid.FirstHit(onPlane + pn.n * lift, -pn.n, 2.0 * lift, t)) return false;
		h = lift - t;
		return true;
	};
	const EVec at = To3(pn, mid, 0.0);
	double h0 = 0.0, h1 = 0.0;
	if (!height(at - outward * (0.5 * step), h0) || !height(at + outward * step, h1)) return false;
	return std::fabs(h1 - h0) <= 1.5 * step * 0.6;                             
}

// размещаем стойки, перемычки и обвязку выбранного участка
void PanelMembers(const Panel& pn, int32_t panelIdx, const Sizes& sz, const Window& win, const SolidTest& solid, std::vector<Member>& out)
{
	const double w = sz.w;
	const double depth = pn.deck ? sz.joist : sz.d;
	                                                                          
	const m::CrossSection inner = pn.poly.Offset(-sz.skin, m::CrossSection::JoinType::Miter, 4.0);
	if (inner.IsEmpty()) return;
	                                                                                   
	if (pn.poly.Offset(-(sz.skin + sz.d), m::CrossSection::JoinType::Miter, 4.0).IsEmpty()) return;
	const m::Rect bb = inner.Bounds();
	                                                                        
	{
		double lo0 = 1e300, hi0 = -1e300, lo1 = 1e300, hi1 = -1e300;
		for (int i = 0; i < 8; i++)
		{
			const EVec q((i & 1) ? win.hi.x : win.lo.x, (i & 2) ? win.hi.y : win.lo.y, (i & 4) ? win.hi.z : win.lo.z);
			const m::vec2 p2 = To2(pn, q);
			lo0 = std::min(lo0, p2.x); hi0 = std::max(hi0, p2.x);
			lo1 = std::min(lo1, p2.y); hi1 = std::max(hi1, p2.y);
		}
		if (bb.max.x < lo0 || bb.min.x > hi0 || bb.max.y < lo1 || bb.min.y > hi1) return;
	}
	std::vector<Member2> ms;
	m::Polygons edgeRects;
	                                                                        
	const bool tall = bb.max.y - bb.min.y > 10.0 * w;
	const m::Polygons contours = inner.ToPolygons();
	for (const m::SimplePolygon& poly : contours)
	{
		const size_t n = poly.size();
		if (n < 3) continue;
		double area2 = 0.0;
		for (size_t i = 0; i < n; i++)
		{
			const m::vec2 a = poly[i], b = poly[(i + 1) % n];
			area2 += a.x * b.y - a.y * b.x;
		}
		const bool hole = area2 < 0.0;                                                           
		for (size_t i = 0; i < n; i++)
		{
			const m::vec2 p0 = poly[(i + n - 1) % n], a = poly[i], b = poly[(i + 1) % n], p3 = poly[(i + 2) % n];
			const double len = m::la::length(b - a);
			if (len < w) continue;
			const m::vec2 t = (b - a) / len, in(-t.y, t.x);                               
			if (SoftSeam(pn, solid, 0.5 * (a + b), in, sz)) continue;
			                                                        
			const m::vec2 tp = m::la::normalize(a - p0), tn = m::la::normalize(p3 - b);
			const double ea = (tp.x * t.y - tp.y * t.x) < -1e-6 ? w : 0.0;
			const double eb = (t.x * tn.y - t.y * tn.x) < -1e-6 ? w : 0.0;
			Member2 e;
			e.a = a - t * ea + in * (0.5 * w);
			e.b = b + t * eb + in * (0.5 * w);
			const bool level = pn.deck || std::fabs(t.y) < 0.38;                                          
			e.kind = level ? kPlate : kEdge;
			e.depth = depth;
			e.stack = 0;
			ms.push_back(e);
			edgeRects.push_back(Rect2(e.a - t * 0.0, e.b, w));
			                                     
			if (sz.doublePlate && !pn.deck && level && tall && ((!hole && in.y < -0.7) || (hole && in.y > 0.7)))
			{
				Member2 e2 = e;
				e2.a = e.a + in * w;
				e2.b = e.b + in * w;
				e2.stack = 1;
				ms.push_back(e2);
				edgeRects.push_back(Rect2(e2.a, e2.b, w));
			}
		}
	}
	m::CrossSection freeArea = inner - m::CrossSection(edgeRects, m::CrossSection::FillRule::NonZero).Offset(0.02 * w, m::CrossSection::JoinType::Miter, 4.0);
	const m::Polygons freePolys = freeArea.ToPolygons();
	                                                        
	m::Polygons studRects;
	const double s = sz.spacing;
	const m::Rect fb = freeArea.Bounds();
	if (!freeArea.IsEmpty())
	{
		const int64_t k0 = (int64_t)std::floor(fb.min.x / s), k1 = (int64_t)std::ceil(fb.max.x / s);
		for (int64_t kk = k0; kk <= k1 && kk - k0 < 100000; kk++)
		{
			const double bx = (double)kk * s;
			if (bx < fb.min.x - w || bx > fb.max.x + w) continue;
			const auto spans = StripSpans(freePolys, m::vec2(bx, 0.0), m::vec2(0.0, 1.0), w);
			for (const auto& sp : spans)
			{
				if (sp.second - sp.first < 3.0 * w) continue;
				Member2 st;
				st.a = m::vec2(bx, sp.first);
				st.b = m::vec2(bx, sp.second);
				st.kind = kStud;
				st.depth = depth;
				st.stack = 0;
				ms.push_back(st);
				studRects.push_back(Rect2(st.a, st.b, w));
			}
		}
	}
	                                                    
	if (sz.noggin > 0.0 && (tall || pn.deck) && !freeArea.IsEmpty())
	{
		const m::CrossSection bays = studRects.empty() ? freeArea
			: freeArea - m::CrossSection(studRects, m::CrossSection::FillRule::NonZero).Offset(0.02 * w, m::CrossSection::JoinType::Miter, 4.0);
		const m::Polygons bayPolys = bays.ToPolygons();
		const double r = sz.noggin;
		const int64_t r0 = (int64_t)std::floor(fb.min.y / r - 0.5), r1 = (int64_t)std::ceil(fb.max.y / r + 0.5);
		for (int64_t rr = r0; rr <= r1 && rr - r0 < 100000; rr++)
		{
			const double ay = ((double)rr + 0.5) * r;
			const auto spans = LineSpans(bayPolys, m::vec2(0.0, ay), m::vec2(1.0, 0.0));
			for (const auto& sp : spans)
			{
				const double mid = 0.5 * (sp.first + sp.second);
				const int64_t bay = (int64_t)std::floor(pn.bsign * mid / s);                                 
				const double shift = ((bay & 1) ? 0.6 : -0.6) * w;
				auto fit = StripSpans(bayPolys, m::vec2(0.0, ay + shift), m::vec2(1.0, 0.0), w);
				fit = Intersect(fit, { { sp.first - 1e-6, sp.second + 1e-6 } });
				for (const auto& f : fit)
				{
					if (f.second - f.first < 1.5 * w) continue;
					                                                                            
					if (f.second - f.first > 2.2 * s) continue;
					Member2 ng;
					ng.a = m::vec2(f.first, ay + shift);
					ng.b = m::vec2(f.second, ay + shift);
					ng.kind = kNoggin;
					ng.depth = depth;
					ng.stack = 0;
					ms.push_back(ng);
				}
			}
		}
	}
	                                                                                                    
	for (size_t i = 0; i < ms.size(); i++)
	{
		const Member2& e = ms[i];
		const m::vec2 t2 = e.b - e.a;
		const double len = m::la::length(t2);
		if (len < 1e-6) continue;
		Member mb;
		const double hair = 0.004 * w * (double)(e.kind + 1);
		mb.o = To3(pn, e.a, sz.skin + hair);
		mb.u = Normalized(pn.B * t2.x + pn.A * t2.y, pn.A);
		mb.k = -pn.n;
		mb.v = m::la::cross(mb.k, mb.u);
		mb.len = len;
		mb.w = w;
		mb.d = e.depth - hair;
		mb.hair = hair;
		mb.kind = e.kind;
		mb.panel = panelIdx;
		mb.key = Key(pn.key, (uint64_t)std::llround(e.a.x * 8.0), (uint64_t)std::llround(e.a.y * 8.0),
			(uint64_t)e.kind | ((uint64_t)e.stack << 8));
		out.push_back(mb);
	}
}

                                                                                           

struct PieceMesh
{
	std::vector<EVec> pts;
	std::vector<ProjectDamagePoly> polys;
	std::vector<float> grain;                 
	int32_t part = 0;
};

void Orient(PieceMesh& pm)
{
	double vol = 0.0;
	for (const ProjectDamagePoly& q : pm.polys)
	{
		vol += m::la::dot(pm.pts[(size_t)q.a], m::la::cross(pm.pts[(size_t)q.b], pm.pts[(size_t)q.c]));
		if (q.c != q.d)
			vol += m::la::dot(pm.pts[(size_t)q.a], m::la::cross(pm.pts[(size_t)q.c], pm.pts[(size_t)q.d]));
	}
	if (vol < 0.0)
		for (ProjectDamagePoly& q : pm.polys)
		{
			if (q.c == q.d) { std::swap(q.b, q.c); q.d = q.c; }
			else std::swap(q.b, q.d);
		}
}

                                                                                          
PieceMesh LumberMesh(const Member& mb)
{
	PieceMesh pm;
	const bool jag = mb.splA > 0.0 || mb.splB > 0.0;
	const int nT = jag ? 3 : 1, nR = jag ? 6 : 1;
	const int gw = nT + 1, gh = nR + 1;
	auto gidx = [&](int j, int i) { return j + gw * i; };
	std::vector<int> ring;
	for (int j = 0; j < nT; j++) ring.push_back(gidx(j, 0));
	for (int i = 0; i < nR; i++) ring.push_back(gidx(nT, i));
	for (int j = nT; j > 0; j--) ring.push_back(gidx(j, nR));
	for (int i = nR; i > 0; i--) ring.push_back(gidx(0, i));
	const double gu = Range(mb.key, 40, -50.0, 50.0), gv = Range(mb.key, 41, -18.0, 18.0), gk = Range(mb.key, 42, -18.0, 18.0);
	for (int end = 0; end < 2; end++)
	{
		const double spl = end == 0 ? mb.splA : mb.splB;
		const uint64_t ek = Key(mb.key, 0x5911, (uint64_t)end);
		const double slant = Range(ek, 1, -1.0, 1.0);
		for (int i = 0; i < gh; i++)
			for (int j = 0; j < gw; j++)
			{
				const double t = -0.5 * mb.w + mb.w * (double)j / (double)nT;
				const double r = mb.d * (double)i / (double)nR;
				double h = 0.0;
				if (spl > 0.0)
				{
					                                                     
					const double across = 0.5 + 0.5 * slant * ((double)i / (double)nR * 2.0 - 1.0);
					const double fibre = U01(ek, 10 + (uint64_t)gidx(j, i));
					h = spl * (0.15 + 0.45 * across + 0.6 * fibre * fibre * fibre);
				}
				const double sAlong = end == 0 ? -h : mb.len + h;
				pm.pts.push_back(mb.o + mb.u * sAlong + mb.v * t + mb.k * r);
				pm.grain.push_back((float)(sAlong + gu));
				pm.grain.push_back((float)(t + gv));
				pm.grain.push_back((float)(r + gk));
			}
	}
	const int32_t off1 = gw * gh;
	auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d, bool split)
	{
		if (split) { pm.polys.push_back({ a, b, c, c }); pm.polys.push_back({ a, c, d, d }); }
		else pm.polys.push_back({ a, b, c, d });
	};
	const size_t nr = ring.size();
	for (size_t q = 0; q < nr; q++)
	{
		const int32_t a = ring[q], b = ring[(q + 1) % nr];
		quad(a, b, off1 + b, off1 + a, jag);
	}
	for (int i = 0; i < nR; i++)
		for (int j = 0; j < nT; j++)
		{
			const int32_t a = gidx(j, i), b = gidx(j + 1, i), c = gidx(j + 1, i + 1), d = gidx(j, i + 1);
			quad(a, d, c, b, mb.splA > 0.0);
			quad(off1 + a, off1 + b, off1 + c, off1 + d, mb.splB > 0.0);
		}
	Orient(pm);
	return pm;
}

                                                                              
// строим гвоздь с выступающей и заглублённой частями
PieceMesh NailMesh(const EVec& base, const EVec& out, double stick, double rs, double timberDepth, uint64_t key)
{
	PieceMesh pm;
	pm.part = 1;
	const int segs = 6;
	EVec dir = out;
	const EVec side = Normalized(m::la::cross(out, std::fabs(out.z) < 0.9 ? EVec(0, 0, 1) : EVec(1, 0, 0)), EVec(1, 0, 0));
	const EVec side2 = m::la::cross(out, side);
	const double lean = Range(key, 1, 0.0, 0.18), la = Range(key, 2, 0.0, 2.0 * kPi);
	dir = Normalized(out + (side * std::cos(la) + side2 * std::sin(la)) * lean, out);
	const bool bent = U01(key, 3) < 0.3;
	const double bendAt = Range(key, 4, 0.35, 0.8) * stick, bendAng = bent ? Range(key, 5, 0.3, 1.1) : 0.0;
	const EVec bendAxis = Normalized(m::la::cross(dir, side * std::cos(la + 1.3) + side2 * std::sin(la + 1.3)), side);
	const double embed = std::min(.75 * timberDepth, std::max(0.8, 6.0 * rs));
	const double rh = rs * 2.6, th = rs * 0.8;
	const double hs[6] = { -embed, -embed, stick, stick, stick + th, stick + th };
	const double rr[6] = { 0.0, rs, rs, rh, rh, 0.0 };
	const EVec u1 = Normalized(m::la::cross(dir, side2), side), u2 = m::la::cross(dir, u1);
	const EVec pivot = base + dir * bendAt;
	auto bendP = [&](const EVec& q, double h)
	{
		if (h <= bendAt || bendAng == 0.0) return q;
		const double c = std::cos(bendAng), s = std::sin(bendAng);
		const EVec v = q - pivot;
		return pivot + v * c + m::la::cross(bendAxis, v) * s + bendAxis * (m::la::dot(bendAxis, v) * (1.0 - c));
	};
	                      
	pm.pts.push_back(bendP(base + dir * hs[0], hs[0]));
	for (int r = 1; r < 5; r++)
		for (int k = 0; k < segs; k++)
		{
			const double a = 2.0 * kPi * (double)k / (double)segs;
			const EVec q = base + dir * hs[r] + (u1 * std::cos(a) + u2 * std::sin(a)) * rr[r];
			pm.pts.push_back(bendP(q, hs[r]));
		}
	pm.pts.push_back(bendP(base + dir * hs[5], hs[5]));
	const int32_t top = (int32_t)pm.pts.size() - 1;
	for (int k = 0; k < segs; k++)
	{
		const int32_t k1 = (k + 1) % segs;
		pm.polys.push_back({ 0, 1 + k1, 1 + k, 1 + k });
		for (int r = 0; r < 3; r++)
			pm.polys.push_back({ 1 + r * segs + k, 1 + r * segs + k1, 1 + (r + 1) * segs + k1, 1 + (r + 1) * segs + k });
		pm.polys.push_back({ 1 + 3 * segs + k, 1 + 3 * segs + k1, top, top });
	}
	for (size_t i = 0; i < pm.pts.size(); i++)
	{
		const EVec q = pm.pts[i] - base;
		pm.grain.push_back((float)m::la::dot(q, dir));
		pm.grain.push_back((float)m::la::dot(q, u1));
		pm.grain.push_back((float)m::la::dot(q, u2));
	}
	Orient(pm);
	return pm;
}

                                                                                           
bool ClipToSource(PieceMesh& pm, const m::Manifold& source, m::Manifold& timber)
{
	m::MeshGL64 mg;
	mg.numProp = 3;
	for (const EVec& q : pm.pts) { mg.vertProperties.push_back(q.x); mg.vertProperties.push_back(q.y); mg.vertProperties.push_back(q.z); }
	for (const ProjectDamagePoly& q : pm.polys)
	{
		mg.triVerts.push_back((uint64_t)q.a); mg.triVerts.push_back((uint64_t)q.b); mg.triVerts.push_back((uint64_t)q.c);
		if (q.c != q.d) { mg.triVerts.push_back((uint64_t)q.a); mg.triVerts.push_back((uint64_t)q.c); mg.triVerts.push_back((uint64_t)q.d); }
	}
	mg.Merge();
	const m::Manifold piece(mg);
	if (piece.Status() != m::Manifold::Error::NoError) return false;
	const m::Manifold kept = piece ^ source;
	if (kept.Status() != m::Manifold::Error::NoError || kept.NumTri() == 0) { pm.pts.clear(); pm.polys.clear(); return true; }
	const m::MeshGL64 r = kept.GetMeshGL64();
	PieceMesh np;
	np.part = pm.part;
	                                                                                                               
	for (size_t i = 0; i < r.NumVert(); i++)
	{
		const EVec q(r.vertProperties[i * r.numProp], r.vertProperties[i * r.numProp + 1], r.vertProperties[i * r.numProp + 2]);
		np.pts.push_back(q);
		size_t best = 0;
		double bd = 1e300;
		for (size_t j = 0; j < pm.pts.size(); j++)
		{
			const double dd = m::la::length2(pm.pts[j] - q);
			if (dd < bd) { bd = dd; best = j; }
		}
		for (int c = 0; c < 3; c++) np.grain.push_back(pm.grain[best * 3 + (size_t)c]);
	}
	for (size_t t = 0; t < r.NumTri(); t++)
		np.polys.push_back({ (int32_t)r.triVerts[t * 3], (int32_t)r.triVerts[t * 3 + 1], (int32_t)r.triVerts[t * 3 + 2], (int32_t)r.triVerts[t * 3 + 2] });
	pm = std::move(np);
	timber = kept;
	return true;
}

                                                                                          

bool IsBox(const m::Manifold& source)
{
	const m::Box b = source.BoundingBox();
	const EVec e = b.max - b.min;
	const double bv = e.x * e.y * e.z;
	return bv > 0.0 && source.Volume() >= 0.999 * bv;
}

m::Manifold BoxSolid(const EVec& lo, const EVec& hi)
{
	const EVec e = hi - lo;
	if (e.x <= 0.0 || e.y <= 0.0 || e.z <= 0.0) return m::Manifold();
	return m::Manifold::Cube(e).Translate(lo);
}

m::Manifold Cavity(const m::Manifold& source, const SolidTest& solid, const Window& reach, const Sizes& sz)
{
	const m::Box sb = source.BoundingBox();
	const EVec lo = m::la::max(reach.lo, sb.min + EVec(sz.skin)), hi = m::la::min(reach.hi, sb.max - EVec(sz.skin));
	if (lo.x >= hi.x || lo.y >= hi.y || lo.z >= hi.z) return m::Manifold();
	if (IsBox(source))
		return BoxSolid(lo, hi);
	SurfaceDistance sd;
	sd.Build(solid.Points(), solid.Tris());
	const double cell = std::clamp(0.5 * sz.w + 0.3 * sz.skin, 0.6, 3.0);
	const double cap = sz.skin + 3.0 * cell;
	auto field = [&](m::vec3 x) -> double
	{
		                           
		const double box = std::min(std::min(std::min(x.x - lo.x, hi.x - x.x), std::min(x.y - lo.y, hi.y - x.y)),
			std::min(x.z - lo.z, hi.z - x.z));
		if (box < -cell) return box;
		                                                                                     
		const double d = sd.Distance(x, cap);
		const double f = d < sz.skin ? d - sz.skin : (solid.Inside(x) ? d - sz.skin : -d - sz.skin);
		return std::min(f, box);
	};
	m::Box bounds;
	bounds.min = lo - EVec(2.0 * cell);
	bounds.max = hi + EVec(2.0 * cell);
	m::Manifold cav = m::Manifold::LevelSet(field, bounds, cell, 0.0, -1.0, true);
	if (cav.Status() != m::Manifold::Error::NoError) return m::Manifold();
	                                                                     
	return cav.Simplify(0.12 * cell);
}

                                                                                
m::Manifold PaperBand(const Panel& pn, const std::vector<m::Manifold>& cutters, const Window& win, const Sizes& sz, uint64_t seed)
{
	if (sz.tear <= 0.0 || sz.paper <= 0.0) return m::Manifold();
	                                                
	const m::mat3x4 toLocal(m::vec3(pn.B.x, pn.A.x, pn.n.x), m::vec3(pn.B.y, pn.A.y, pn.n.y),
		m::vec3(pn.B.z, pn.A.z, pn.n.z), m::vec3(0.0, 0.0, -pn.c));
	const m::mat3x4 toWorld(pn.B, pn.A, pn.n, pn.n * pn.c);
	m::CrossSection foot;
	for (const m::Manifold& c : cutters)
	{
		const m::Box cb = c.BoundingBox();
		if (!win.Hits(cb.min, cb.max)) continue;
		const m::Manifold loc = c.Transform(toLocal);
		const m::Box lb = loc.BoundingBox();
		const double z = -0.5 * sz.paper;
		if (lb.min.z > z || lb.max.z < z) continue;
		foot = foot + m::CrossSection(loc.Slice(z), m::CrossSection::FillRule::Positive);
	}
	if (foot.IsEmpty()) return m::Manifold();
	                                                                                   
	const double step = std::max(0.25, 0.12 * sz.tear);
	m::Polygons torn;
	const uint64_t tk = Key(seed, pn.key, 0x7EA2);
	for (const m::SimplePolygon& poly : foot.Simplify(0.2 * step).ToPolygons())
	{
		const size_t n = poly.size();
		if (n < 3) continue;
		m::SimplePolygon dense;
		for (size_t i = 0; i < n; i++)
		{
			const m::vec2 a = poly[i], b = poly[(i + 1) % n];
			const int k = std::max(1, (int)std::ceil(m::la::length(b - a) / step));
			for (int j = 0; j < k; j++) dense.push_back(a + (b - a) * ((double)j / (double)k));
		}
		const size_t dn = dense.size();
		m::SimplePolygon out(dn);
		double s = 0.0;
		for (size_t i = 0; i < dn; i++)
		{
			const m::vec2 prev = dense[(i + dn - 1) % dn], next = dense[(i + 1) % dn];
			const EVec t3 = Normalized(EVec(next.x - prev.x, next.y - prev.y, 0.0), EVec(1, 0, 0));
			const m::vec2 t(t3.x, t3.y);
			const m::vec2 outN(t.y, -t.x);                                                              
			if (i > 0) s += m::la::length(dense[i] - dense[i - 1]);
			const double big = 0.5 + 0.5 * Noise1(s / (2.2 * sz.tear), tk);
			const double mid = 0.5 + 0.5 * Noise1(s / (0.6 * sz.tear), tk ^ 0x51);
			const double fine = Noise1(s / (0.18 * sz.tear), tk ^ 0xA7);
			double off = sz.tear * (0.2 + 0.85 * big + 0.45 * mid * big) + 0.15 * sz.tear * fine;
			                                                              
			if (U01(tk, (uint64_t)std::floor(s / (1.5 * sz.tear))) < 0.12) off *= 1.8;
			out[i] = dense[i] + outN * std::max(0.0, off);
		}
		torn.push_back(std::move(out));
	}
	m::CrossSection band = m::CrossSection(torn, m::CrossSection::FillRule::Positive) + foot;
	                                                            
	band = band ^ pn.poly;
	if (band.IsEmpty()) return m::Manifold();
	m::Manifold slab = m::Manifold::Extrude(band.ToPolygons(), sz.paper + 1.0).Translate(m::vec3(0.0, 0.0, -sz.paper));
	return slab.Transform(toWorld);
}

}              

// собираем деревянный каркас, открытый повреждением
void BuildBreakFraming(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, m::Manifold* carve)
{
	out = BreakRebarMesh();
	if (carve) *carve = m::Manifold();
	if (!p.enable || source.NumTri() == 0) return;
	const Sizes sz = SizesOf(p);
	const uint64_t seed = Key((uint64_t)(uint32_t)mainSeed, (uint64_t)(uint32_t)p.seed, 0xF4A3);

	SolidTest solid;
	solid.Build(source);
	if (!solid.Valid()) return;
	std::vector<SolidTest> cut;
	EVec cLo(std::numeric_limits<double>::max()), cHi(std::numeric_limits<double>::lowest());
	std::vector<m::Manifold> live;
	for (const m::Manifold& c : cutters)
	{
		if (c.NumTri() == 0) continue;
		cut.emplace_back();
		cut.back().Build(c);
		if (!cut.back().Valid()) { cut.pop_back(); continue; }
		cLo = m::la::min(cLo, cut.back().Lo());
		cHi = m::la::max(cHi, cut.back().Hi());
		live.push_back(c);
	}
	const bool all = p.showAll;
	if (cut.empty() && !all) return;
	                                                                            
	Window win;
	if (all) { win.lo = solid.Lo() - EVec(1.0); win.hi = solid.Hi() + EVec(1.0); }
	else
	{
		const double margin = 0.75 * sz.spacing + sz.d;
		win.lo = m::la::max(cLo - EVec(margin), solid.Lo() - EVec(1.0));
		win.hi = m::la::min(cHi + EVec(margin), solid.Hi() + EVec(1.0));
	}
	Window grow = win;
	grow.lo -= EVec(sz.spacing + 2.0 * sz.d);
	grow.hi += EVec(sz.spacing + 2.0 * sz.d);
	auto inCut = [&](const EVec& q)
	{
		for (const SolidTest& c : cut) if (c.Inside(q)) return true;
		return false;
	};

	const std::vector<Panel> panels = FindPanels(source, solid, grow, sz, seed);

	                                                        
	if (carve && !cut.empty())
	{
		                                                                                               
		std::vector<Window> reach;
		for (const SolidTest& ct : cut)
		{
			Window w;
			w.lo = ct.Lo() - EVec(sz.reach);
			w.hi = ct.Hi() + EVec(sz.reach);
			reach.push_back(w);
		}
		for (bool merged = true; merged;)
		{
			merged = false;
			for (size_t i = 0; i < reach.size() && !merged; i++)
				for (size_t j = i + 1; j < reach.size() && !merged; j++)
					if (reach[i].Hits(reach[j].lo, reach[j].hi))
					{
						reach[i].lo = m::la::min(reach[i].lo, reach[j].lo);
						reach[i].hi = m::la::max(reach[i].hi, reach[j].hi);
						reach.erase(reach.begin() + (std::ptrdiff_t)j);
						merged = true;
					}
		}
		m::Manifold c;
		for (const Window& w : reach)
		{
			const m::Manifold one = Cavity(source, solid, w, sz);
			if (one.NumTri() != 0) c = c + one;
		}
		for (const Panel& pn : panels)
		{
			const m::Manifold band = PaperBand(pn, live, win, sz, seed);
			if (band.NumTri() != 0) c = c + band;
		}
		if (c.Status() == m::Manifold::Error::NoError) *carve = c;
	}

	                          
	std::vector<std::vector<Member>> perPanel(panels.size());
	ProjectDamageParallelFor(0, (int64_t)panels.size(), [&](int64_t i)
	{
		PanelMembers(panels[(size_t)i], (int32_t)i, sz, win, solid, perPanel[(size_t)i]);
	});
	std::vector<Member> members;
	for (auto& v : perPanel) for (Member& mb : v) members.push_back(mb);
	                                
	{
		std::vector<Member> kept;
		for (const Member& mb : members)
		{
			EVec lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
			for (int c = 0; c < 8; c++) { const EVec q = mb.Corner(c); lo = m::la::min(lo, q); hi = m::la::max(hi, q); }
			if (win.Hits(lo, hi)) kept.push_back(mb);
		}
		members.swap(kept);
	}
	                                                                          
	ProjectDamageParallelFor(0, (int64_t)members.size(), [&](int64_t i)
	{
		Member& mb = members[(size_t)i];
		                                                                    
		                                                                     
		                      
		{
			double gap = std::numeric_limits<double>::max();
			for (int sIdx = 0; sIdx <= 4; sIdx++)
				for (int side = -1; side <= 1; side += 2)
				{
					const EVec q = mb.o + mb.u * (mb.len * (0.02 + 0.24 * (double)sIdx)) + mb.v * (0.45 * mb.w * (double)side);
					double g = 0.0;
					if (ExitAlong(solid, q, -mb.k, g)) gap = std::min(gap, g);
				}
			if (gap < std::numeric_limits<double>::max())
			{
				const double shift = (sz.skin + mb.hair) - gap;
				if (std::fabs(shift) > 1e-4) mb.o += mb.k * shift;
			}
		}
		double far = std::numeric_limits<double>::max();
		bool any = false;
		for (int sIdx = 0; sIdx < 3; sIdx++)
		{
			const double sAlong = mb.len * (0.1 + 0.4 * (double)sIdx);
			for (int side = -1; side <= 1; side += 2)
			{
				const EVec org = mb.o + mb.u * sAlong + mb.v * (0.4 * mb.w * (double)side) + mb.k * 1e-3;
				double t = 0.0;
				if (solid.FirstHit(org, mb.k, 1e30, t)) { far = std::min(far, t); any = true; }
			}
		}
		if (!any) return;
		const double room = far - sz.skin;                                         
		if (room < mb.d + 0.6 * mb.d) mb.d = std::max(0.2 * mb.w, room - 0.004 * mb.w * (double)(mb.kind + 1));
	});
	                                                                               
	{
		std::vector<char> drop(members.size(), 0);
		for (size_t i = 0; i < members.size(); i++)
		{
			if (drop[i]) continue;
			const Member& a = members[i];
			for (size_t j = i + 1; j < members.size(); j++)
			{
				if (drop[j]) continue;
				const Member& b = members[j];
				if (std::fabs(m::la::dot(a.u, b.u)) < 0.995 || std::fabs(m::la::dot(a.k, b.k)) < 0.995) continue;
				EVec lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
				for (int c = 0; c < 8; c++)
				{
					const EVec q = b.Corner(c) - a.o;
					const EVec l(m::la::dot(q, a.u), m::la::dot(q, a.v), m::la::dot(q, a.k));
					lo = m::la::min(lo, l); hi = m::la::max(hi, l);
				}
				const double ox = std::min(hi.x, a.len) - std::max(lo.x, 0.0);
				const double oy = std::min(hi.y, 0.5 * a.w) - std::max(lo.y, -0.5 * a.w);
				const double oz = std::min(hi.z, a.d) - std::max(lo.z, 0.0);
				if (ox <= 0.0 || oy <= 0.0 || oz <= 0.0) continue;
				const double ov = ox * oy * oz;
				const double va = a.len * a.w * a.d, vb = b.len * b.w * b.d;
				if (ov > 0.5 * std::min(va, vb))
				{
					if (vb > va * 1.05) { drop[i] = 1; break; }
					drop[j] = 1;
				}
			}
		}
		std::vector<Member> kept;
		for (size_t i = 0; i < members.size(); i++) if (!drop[i]) kept.push_back(members[i]);
		members.swap(kept);
	}
	                                                                        
	std::vector<Member> pieces;
	for (const Member& mb : members)
	{
		const double step = std::max(0.5, std::min(2.0, 0.5 * mb.w));
		const int ns = std::max(2, (int)std::ceil(mb.len / step) + 1);
		std::vector<char> in(ns);
		int inside = 0;
		for (int i = 0; i < ns; i++)
		{
			in[(size_t)i] = cut.empty() ? 0 : (char)inCut(mb.Mid(mb.len * (double)i / (double)(ns - 1)));
			inside += in[(size_t)i];
		}
		if (inside == ns && !all)
		{
			                                                  
			if (U01(mb.key, 0xB1) < sz.broken * (mb.kind == kNoggin ? 1.5 : 0.6)) continue;
			pieces.push_back(mb);
			continue;
		}
		                                 
		int bestA = -1, bestB = -1;
		for (int i = 0; i < ns;)
		{
			if (!in[(size_t)i]) { i++; continue; }
			int j = i;
			while (j + 1 < ns && in[(size_t)(j + 1)]) j++;
			if (bestA < 0 || j - i > bestB - bestA) { bestA = i; bestB = j; }
			i = j + 1;
		}
		const double runA = bestA < 0 ? 0.0 : mb.len * (double)bestA / (double)(ns - 1);
		const double runB = bestA < 0 ? 0.0 : mb.len * (double)bestB / (double)(ns - 1);
		if (bestA < 0 || runB - runA < 3.0 * mb.w || U01(mb.key, 0xB2) >= sz.broken)
		{
			pieces.push_back(mb);
			continue;
		}
		const double at = runA + (runB - runA) * Range(mb.key, 0xB3, 0.25, 0.75);
		const double gap = mb.w * Range(mb.key, 0xB4, 0.4, 2.5);
		const double spl = mb.w * Range(mb.key, 0xB5, 0.8, 2.6);
		Member first = mb, second = mb;
		first.len = at - 0.5 * gap - 0.5 * spl;
		first.splB = spl;
		second.o = mb.o + mb.u * (at + 0.5 * gap + 0.5 * spl);
		second.len = mb.len - (at + 0.5 * gap + 0.5 * spl);
		second.splA = spl;
		second.key = Key(mb.key, 0x2D2);
		for (Member* pc : { &first, &second })
		{
			if (pc->len < 0.5 * pc->w) continue;
			                                                     
			bool held = false;
			for (int i = 0; i <= 8 && !held; i++)
				held = !inCut(pc->Mid(pc->len * (double)i / 8.0));
			if (held) pieces.push_back(*pc);
		}
	}
	                                              
	std::vector<PieceMesh> meshes(pieces.size());
	std::vector<std::vector<PieceMesh>> nails(pieces.size());
	ProjectDamageParallelFor(0, (int64_t)pieces.size(), [&](int64_t i)
	{
		const Member& mb = pieces[(size_t)i];
		PieceMesh pm = LumberMesh(mb);
		bool outside = false;
		for (int c = 0; c < 8 && !outside; c++)
		{
			const EVec q = mb.Corner(c);
			const EVec mid = mb.o + mb.u * (0.5 * mb.len) + mb.k * (0.5 * mb.d);
			outside = !solid.Inside(q + (mid - q) * 0.02);
		}
		m::Manifold clippedTimber;
		if (outside) ClipToSource(pm, source, clippedTimber);
		const bool hasTimber = !pm.polys.empty();
		meshes[(size_t)i] = std::move(pm);
		if (!hasTimber || mb.kind == kNoggin || sz.nails <= 0.0 || cut.empty()) return;
		const double nailStep = mb.kind == kStud ? 20.0 * std::clamp(p.scale, 0.01, 100.0) : 30.0 * std::clamp(p.scale, 0.01, 100.0);
		const double rs = std::max(0.05, 0.05 * mb.w);
		for (double sAlong = Range(mb.key, 0xC0, 3.0, nailStep); sAlong < mb.len - 2.0; sAlong += nailStep * Range(mb.key, (uint64_t)(0xC1 + sAlong), 0.8, 1.2))
		{
			const uint64_t nk = Key(mb.key, 0x4A11, (uint64_t)std::llround(sAlong * 10.0));
			const double tAcross = Range(nk, 1, -0.2, 0.2) * mb.w;
			EVec face = mb.o + mb.u * sAlong + mb.v * tAcross;
			double timberDepth = mb.d;
			if (outside)
			{
				const auto support = FindFramingNailSupport(clippedTimber, face, -mb.k, mb.d + 1.0);
				if (!support) continue;
				face = support->front;
				timberDepth = support->depth;
			}
			                                                                   
			if (!inCut(face - mb.k * (0.5 * sz.skin))) continue;
			if (U01(nk, 2) >= sz.nails) continue;
			const double stick = sz.nailLen * Range(nk, 3, 1.6, 3.0);
			nails[(size_t)i].push_back(NailMesh(face, -mb.k, stick, rs, timberDepth, nk));
		}
	});
	auto append = [&](const PieceMesh& pm)
	{
		if (pm.polys.empty()) return;
		const Int32 vo = (Int32)out.points.size();
		out.points.insert(out.points.end(), pm.pts.begin(), pm.pts.end());
		out.exposure.insert(out.exposure.end(), pm.pts.size(), 1.0f);
		out.grain.insert(out.grain.end(), pm.grain.begin(), pm.grain.end());
		for (const ProjectDamagePoly& q : pm.polys)
		{
			out.polys.push_back({ q.a + vo, q.b + vo, q.c + vo, q.d + vo });
			out.polyBar.push_back(out.bars);
			out.polyPart.push_back(pm.part);
		}
		out.bars++;
	};
	for (size_t i = 0; i < meshes.size(); i++)
	{
		append(meshes[i]);
		for (const PieceMesh& nm : nails[i]) append(nm);
	}
}

PROJECT_DAMAGE_NS_MACRO_END
