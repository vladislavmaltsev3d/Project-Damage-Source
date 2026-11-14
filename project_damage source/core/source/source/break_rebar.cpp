// прокладываем арматуру и другие внутренние детали через повреждённый объём
#include "rebar_lod.h"
// ProjectDamage break rebar - GPL-3.0-or-later; see LICENSE.
//
// Reinforcement bars exposed by the Boolean (Breaker) damage. The bars are
// laid out in the uncut solid (grid, double grid, rectangular or round cage),
// each bar's centreline is classified against the solid and the break
// cutters, and only the stretches that run through the removed volume (plus
// an embed length into the concrete that is left) are swept into tubes.
// Exposed stretches bow, snap, get trimmed and bend; every resulting piece is
// its own closed island.

#include "manifold/manifold.h"

#include "break_rebar.h"
#include "project_damage_bvh.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <stdexcept>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kPlankType = 1000;                                                       

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

double Smooth01(double t)
{
	t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
	return t * t * (3.0 - 2.0 * t);
}

double Bump(double x)
{
	if (x <= -1.0 || x >= 1.0)
		return 0.0;
	const double t = 1.0 - x * x;
	return t * t;
}

EVec Normalized(const EVec& v, const EVec& fallback)
{
	const double l = m::la::length(v);
	return l > 1e-12 ? v / l : fallback;
}

EVec AnyPerp(const EVec& t)
{
	const EVec a = std::fabs(t.x) < 0.9 ? EVec(1, 0, 0) : EVec(0, 1, 0);
	return Normalized(m::la::cross(t, a), EVec(0, 0, 1));
}

                                                                            
                                                         
class SolidTest
{
public:
	void Build(const m::Manifold& solid, bool unionComponents = false)
	{
        members_.clear();
        if(unionComponents)
        {
            const auto parts=solid.Decompose();
            if(parts.size()>1)
            {
                members_.resize(parts.size());
                for(size_t i=0;i<parts.size();++i)members_[i].Build(parts[i]);
            }
        }
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
        if(!members_.empty())
        {
            for(const auto& member:members_)if(member.Inside(p))return true;
            return false;
        }
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

	std::vector<SolidTest> members_;
	std::vector<EVec> pts_;
	std::vector<ProjectDamagePoly> polys_;
	ProjectDamageBVH bvh_;
	EVec lo_, hi_;
	double eps_ = 1e-7;
	bool valid_ = false;
};

                                                                                 
EVec UpOf(const BreakRebarParams& p)
{
	return Normalized(EVec(p.upX, p.upY, p.upZ), EVec(0, 0, 1));
}

struct Region
{
	SolidTest solid;
	const SurfaceDirectionField* guide = nullptr;
	std::vector<SolidTest> cut;
	SolidTest left;                                                                  
	bool hasLeft = false;
	EVec winLo, winHi;

	bool InWindow(const EVec& p) const
	{
		return p.x >= winLo.x && p.y >= winLo.y && p.z >= winLo.z
			&& p.x <= winHi.x && p.y <= winHi.y && p.z <= winHi.z;
	}
	                                                               
	void State(const EVec& p, bool& inS, bool& inR) const
	{
		if (!InWindow(p)) { inS = true; inR = false; return; }
		inS = solid.Inside(p);
		inR = false;
		if (!inS) return;
		for (const SolidTest& c : cut)
			if (c.Inside(p)) { inR = true; return; }
	}
};

                                    
struct BarPath
{
	std::vector<EVec> pts;
	bool closed = false;
	double radius = 0.6;
	int32_t type = BREAK_REBAR_RIBBED;
	int32_t outKind = 0;                                                                  
	EVec out = EVec(0, 0, 1);
	EVec axisC = EVec(0.0), axisD = EVec(0, 0, 1);
	uint64_t key = 0;
	int32_t family = 0;                                                                         
	int64_t row = 0;                                                                            
	double halfW = 0.0;                                                                        
	double depthCover = -1.0;
	double plankHalfLimit = std::numeric_limits<double>::max();
	double deeper = 0.0;                                                                         
	std::vector<EVec> outs;                                                                                     
};

                                                                                 
EVec NearestOut(const BarPath& bp, const EVec& pos)
{
	size_t best = 0;
	double bd = std::numeric_limits<double>::max();
	for (size_t i = 0; i < bp.pts.size(); i++)
	{
		const EVec d = bp.pts[i] - pos;
		const double d2 = m::la::dot(d, d);
		if (d2 < bd) { bd = d2; best = i; }
	}
	                                                                  
	EVec o = bp.outs[best];
	const size_t n = bp.pts.size();
	const size_t nb[2] = { best == 0 ? (bp.closed ? n - 1 : 0) : best - 1, best + 1 < n ? best + 1 : (bp.closed ? 0 : best) };
	for (size_t k : nb)
	{
		if (k == best) continue;
		const EVec seg = bp.pts[k] - bp.pts[best];
		const double l2 = m::la::dot(seg, seg);
		if (l2 <= 0.0) continue;
		const double t = m::la::dot(pos - bp.pts[best], seg) / l2;
		if (t > 0.0) o = bp.outs[best] * (1.0 - std::min(t, 1.0)) + bp.outs[k] * std::min(t, 1.0);
	}
	return o;
}

struct Polyline
{
	std::vector<EVec> pts;
	std::vector<double> cum;

	void Init(std::vector<EVec> p)
	{
		pts = std::move(p);
		cum.assign(pts.size(), 0.0);
		for (size_t i = 1; i < pts.size(); i++)
			cum[i] = cum[i - 1] + m::la::length(pts[i] - pts[i - 1]);
	}
	double Length() const { return cum.empty() ? 0.0 : cum.back(); }
	size_t Seg(double s) const
	{
		if (pts.size() < 2) return 0;
		const auto it = std::upper_bound(cum.begin(), cum.end(), s);
		size_t i = (size_t)(it - cum.begin());
		i = i == 0 ? 0 : i - 1;
		return std::min(i, pts.size() - 2);
	}
	EVec At(double s) const
	{
		if (pts.size() < 2) return pts.empty() ? EVec(0.0) : pts[0];
		const size_t i = Seg(s);
		const double len = cum[i + 1] - cum[i];
		const double t = len > 0.0 ? (s - cum[i]) / len : 0.0;
		return pts[i] + (pts[i + 1] - pts[i]) * std::clamp(t, 0.0, 1.0);
	}
	EVec Tangent(double s) const
	{
		if (pts.size() < 2) return EVec(0, 0, 1);
		const size_t i = Seg(s);
		return Normalized(pts[i + 1] - pts[i], EVec(0, 0, 1));
	}
};

struct Interval
{
	double a = 0.0, b = 0.0;
	bool naturalA = false, naturalB = false;                                                         
	bool snapA = false, snapB = false;                                      
};

template <typename Pred>
double Refine(const Polyline& pl, double s0, double s1, bool v0, Pred&& pred)
{
	for (int k = 0; k < 12; k++)
	{
		const double mid = 0.5 * (s0 + s1);
		if (pred(pl.At(mid)) == v0) s0 = mid; else s1 = mid;
	}
	return 0.5 * (s0 + s1);
}

template <typename Pred>
std::vector<Interval> Intervals(const Polyline& pl, const std::vector<double>& s,
	const std::vector<char>& v, Pred&& pred)
{
	std::vector<Interval> out;
	bool open = false;
	Interval cur;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (v[i] && !open)
		{
			open = true;
			cur = Interval();
			if (i == 0) { cur.a = s[0]; cur.naturalA = false; }
			else { cur.a = Refine(pl, s[i - 1], s[i], false, pred); cur.naturalA = true; }
		}
		else if (!v[i] && open)
		{
			open = false;
			cur.b = Refine(pl, s[i - 1], s[i], true, pred);
			cur.naturalB = true;
			out.push_back(cur);
		}
	}
	if (open)
	{
		cur.b = s.back();
		cur.naturalB = false;
		out.push_back(cur);
	}
	return out;
}

                                                                     
struct Span
{
	double a = 0.0, b = 0.0;
	bool anchorA = true, anchorB = true;                                          
};

struct Piece
{
	double s0 = 0.0, s1 = 0.0;
	int32_t end0 = 0, end1 = 0;                                                     
	std::vector<Span> spans;
	bool loop = false;                                                                      
	uint64_t key = 0;
};

                                                                                  
                                                              
std::vector<m::vec2> PlankRing(double ht, double hw)
{
	const int nw = std::clamp((int)std::ceil(hw / std::max(ht, 1e-6) * 2.0), 6, 16);
	const int nt = 1;
	std::vector<m::vec2> ring;
	for (int i = 0; i < nw; i++) ring.push_back(m::vec2(ht, -hw + 2.0 * hw * i / nw));
	for (int i = 0; i < nt; i++) ring.push_back(m::vec2(ht - 2.0 * ht * i / nt, hw));
	for (int i = 0; i < nw; i++) ring.push_back(m::vec2(-ht, hw - 2.0 * hw * i / nw));
	for (int i = 0; i < nt; i++) ring.push_back(m::vec2(-ht + 2.0 * ht * i / nt, -hw));
	return ring;
}

struct Builder
{
	const BreakRebarParams& p;
	double sc;
	BreakRebarMesh& out;

	int32_t SidesFor(int32_t type) const
	{
		int32_t n = std::clamp(p.sides, 4, 64);
		if (type == BREAK_REBAR_TWISTED) n = std::max(n, 16);
		if (type == BREAK_REBAR_STRAND) n = std::max(n, 36);
		return RebarLodSides(n, p.rebarResolution);
	}

	                                                                                
	double Profile(int32_t type, double r, double th, double s, double e) const
	{
		const double d = 2.0 * r;
		switch (type)
		{
		case BREAK_REBAR_RIBBED:
		{
			const double h = p.ribHeight * r * e;
			if (h <= 0.0) return r;
			const double pitch = std::max(1e-4, p.ribPitch * d);
			double w = std::fmod(th, 2.0 * kPi);
			if (w < 0.0) w += 2.0 * kPi;
			const int half = w < kPi ? 0 : 1;
			const double alpha = (w - half * kPi) / kPi;
			const double dl = std::min(std::min(w, std::fabs(w - kPi)), 2.0 * kPi - w);
			const double longi = 0.75 * Bump(dl / 0.16);
			double ph = s / pitch + half * 0.5 + (half == 0 ? 0.35 : -0.35) * (alpha - 0.5);
			ph -= std::floor(ph);
			const double cres = std::pow(std::max(0.0, std::sin(kPi * alpha)), 0.6);
			const double trans = cres * Bump((ph - 0.5) / 0.2);
			return r + h * std::max(longi, trans);
		}
		case BREAK_REBAR_HELICAL:
		{
			const double h = p.ribHeight * r * e;
			if (h <= 0.0) return r;
			const double pitch = std::max(1e-4, p.ribPitch * d);
			double ph = s / (2.0 * pitch) + th / kPi;
			ph -= std::floor(ph);
			return r + h * Bump((ph - 0.5) / 0.16);
		}
		case BREAK_REBAR_TWISTED:
		{
			const double rs = 0.886 * r;
			const double phi = 2.0 * kPi * s / std::max(1e-4, p.twistPitch * d);
			const double a = th - phi;
			const double n = 5.0;
			const double q = std::pow(std::pow(std::fabs(std::cos(a)), n) + std::pow(std::fabs(std::sin(a)), n), 1.0 / n);
			return rs / std::max(q, 1e-6);
		}
		case BREAK_REBAR_STRAND:
		{
			const double rw = r / 3.0, dc = 2.0 * rw;
			const double phi = 2.0 * kPi * s / std::max(1e-4, p.twistPitch * d);
			double best = rw;
			for (int k = 0; k < 6; k++)
			{
				const double del = th - (phi + k * kPi / 3.0);
				const double sn = dc * std::sin(del), cs = dc * std::cos(del);
				const double disc = rw * rw - sn * sn;
				if (disc >= 0.0 && cs > 0.0)
					best = std::max(best, cs + std::sqrt(disc));
			}
			return best;
		}
		default:
			return r;
		}
	}

	EVec OutAt(const BarPath& bp, const EVec& pos, const EVec& tan) const
	{
		EVec o = bp.out;
		if (!bp.outs.empty())
			o = NearestOut(bp, pos);
		else if (bp.outKind == 1)
		{
			const EVec rel = pos - bp.axisC;
			o = rel - bp.axisD * m::la::dot(rel, bp.axisD);
		}
		o = o - tan * m::la::dot(o, tan);
		return Normalized(o, AnyPerp(tan));
	}

	                                                     
	void Emit(const BarPath& bp, const Polyline& pl, const Piece& pc)
	{
		const double r = bp.radius;
		const double len = pc.s1 - pc.s0;
		if (len <= 0.25 * r)
			return;
		const double detail = std::clamp(p.detail, 0.05, 4.0);
		const bool ribbed = (bp.type == BREAK_REBAR_RIBBED || bp.type == BREAK_REBAR_HELICAL) && p.ribHeight > 0.0;
		const bool twisted = bp.type == BREAK_REBAR_TWISTED || bp.type == BREAK_REBAR_STRAND;
		double fine = 0.7 * r;
		if (ribbed) fine = std::min(fine, p.ribPitch * 2.0 * r / 5.0);
		if (twisted) fine = std::min(fine, p.twistPitch * 2.0 * r / 16.0);
		fine = std::max(fine / detail, 0.02 * r);
		const double coarse = std::max(fine, 1.5 * r / detail);

		auto exposureAt = [&](double s) -> double
		{
			double e = 0.0;
			for (const Span& sp : pc.spans)
			{
				if (s >= sp.a && s <= sp.b) return 1.0;
				const double dd = s < sp.a ? sp.a - s : s - sp.b;
				e = std::max(e, 1.0 - Smooth01(dd / (3.0 * r)));
			}
			return e;
		};

		                                                                      
		std::vector<double> st;
		{
			double s = pc.s0;
			st.push_back(s);
			while (s < pc.s1)
			{
				const double e = exposureAt(s);
				double step = e > 0.0 ? fine : coarse;
				const double tipDist = std::min(s - pc.s0, pc.s1 - s);
				if (tipDist < 2.0 * r) step = std::min(step, std::max(fine, 0.25 * r));
				for (const Span& sp : pc.spans)
					if (s < sp.a && s + step > sp.a) step = std::max(sp.a - s, 0.05 * r);
				s = std::min(pc.s1, s + step);
				if (pc.s1 - s < 0.2 * fine) s = pc.s1;
				st.push_back(s);
			}
		}
		if (pc.loop && st.size() > 2)
			st.pop_back();
		size_t nr = st.size();
		if (nr < 2)
			return;

		                                       
		const uint64_t key = pc.key;
		const double wob = p.wobble * r;
		std::vector<EVec> c(nr), outv(nr);
		std::vector<double> ex(nr);
		for (size_t i = 0; i < nr; i++)
		{
			const double s = st[i];
			const EVec pos = pl.At(s);
			const EVec tan = pl.Tangent(s);
			const EVec o = OutAt(bp, pos, tan);
			const EVec side = Normalized(m::la::cross(tan, o), AnyPerp(tan));
			const double e = exposureAt(s);
			EVec d(0.0);
			const double ww = wob * (0.3 + 0.7 * e);
			if (ww > 0.0)
			{
				const double f = s / (10.0 * r);
				d += o * (ww * Noise1(f, key ^ 0x51)) + side * (ww * Noise1(f, key ^ 0x93));
			}
			for (size_t k = 0; k < pc.spans.size(); k++)
			{
				const Span& sp = pc.spans[k];
				const double sl = sp.b - sp.a;
				if (sl <= 0.0 || s < sp.a || s > sp.b) continue;
				const uint64_t sk = Key(key, 0x5A, k);
				const double x = (s - sp.a) / sl;
				if (sp.anchorA && sp.anchorB)
				{
					const EVec dir = Normalized(o + side * Range(sk, 3, -0.4, 0.4), o);
					const double amt = p.bow * sl * 0.07 * Range(sk, 1, 0.4, 1.4);
					d += dir * (amt * std::sin(kPi * x));
				}
				else
				{
					const double phi = Range(sk, 4, -1.3, 1.3);
					const EVec dir = Normalized(o * std::cos(phi) + side * std::sin(phi), o);
					const double amt = p.bend * sl * 0.45 * Range(sk, 2, 0.3, 1.0);
					const double xx = sp.anchorA ? x : 1.0 - x;
					d += dir * (amt * xx * xx);
				}
			}
			c[i] = pos + d;
			outv[i] = o;
			ex[i] = e;
		}

		                                                            
        if (bp.type != kPlankType && p.rebarResolution < 1.0) {
            const auto ids=RebarLodIndices(c,r,p.rebarResolution,pc.loop);
            RebarLodSelect(c,ids);RebarLodSelect(outv,ids);RebarLodSelect(ex,ids);RebarLodSelect(st,ids);nr=c.size();
        }
		std::vector<EVec> T(nr), N(nr);
		for (size_t i = 0; i < nr; i++)
		{
			EVec t;
			if (pc.loop) t = c[(i + 1) % nr] - c[(i + nr - 1) % nr];
			else if (i == 0) t = c[1] - c[0];
			else if (i + 1 == nr) t = c[nr - 1] - c[nr - 2];
			else t = c[i + 1] - c[i - 1];
			T[i] = Normalized(t, pl.Tangent(st[i]));
		}
		N[0] = Normalized(outv[0] - T[0] * m::la::dot(outv[0], T[0]), AnyPerp(T[0]));
		for (size_t i = 1; i < nr; i++)
		{
			EVec n = N[i - 1] - T[i] * m::la::dot(N[i - 1], T[i]);
			N[i] = Normalized(n, AnyPerp(T[i]));
		}
		double twistFix = 0.0;
		if (pc.loop)
		{
			EVec n = N[nr - 1] - T[0] * m::la::dot(N[nr - 1], T[0]);
			n = Normalized(n, N[0]);
			const EVec b0 = m::la::cross(T[0], N[0]);
			twistFix = std::atan2(m::la::dot(n, b0), m::la::dot(n, N[0]));
		}

		const bool plank = bp.type == kPlankType;
		const std::vector<m::vec2> ring = plank ? PlankRing(r, std::max(bp.halfW, r)) : std::vector<m::vec2>();
		const int32_t sides = plank ? (int32_t)ring.size() : SidesFor(bp.type);
		const double hw = std::max(bp.halfW, r);
		                                                                 
		auto splinterAt = [&](double b, int e) -> double
		{
			const double x = (b / hw + 1.0) * 2.5;
			const double k0 = std::floor(x), f = x - k0;
			const uint64_t sk = Key(key, 0x5B1, (uint64_t)e);
			const double a0 = U01(sk, (uint64_t)k0), a1 = U01(sk, (uint64_t)k0 + 1);
			const double tooth = a0 + (a1 - a0) * Smooth01(f);
			return std::pow(tooth, 1.5) * (0.6 + 0.4 * std::fabs(std::sin(kPi * f)));
		};
		const int32_t base = (int32_t)out.points.size();
		const int32_t island = out.bars;
		out.points.reserve(out.points.size() + nr * (size_t)sides + 2);
		const double neckLen = 1.6 * r;
		for (size_t i = 0; i < nr; i++)
		{
			const double s = st[i];
			const EVec B = m::la::cross(T[i], N[i]);
			const double rot = pc.loop ? -twistFix * (double)i / (double)nr : 0.0;
			double rs = 1.0, tipShift = 0.0;
			if (!pc.loop && plank)
			{
				if (i == 0 && pc.end0 == 1) tipShift = -1.0;
				if (i + 1 == nr && pc.end1 == 1) tipShift = 1.0;
			}
			else if (!pc.loop)
			{
				const double d0 = s - pc.s0, d1 = pc.s1 - s;
				if (pc.end0 == 1 && d0 < neckLen) rs = std::min(rs, 1.0 - 0.45 * p.neck * std::pow(1.0 - d0 / neckLen, 2.0));
				if (pc.end1 == 1 && d1 < neckLen) rs = std::min(rs, 1.0 - 0.45 * p.neck * std::pow(1.0 - d1 / neckLen, 2.0));
				if (i == 0 && pc.end0 == 1) tipShift = -1.0;
				if (i + 1 == nr && pc.end1 == 1) tipShift = 1.0;
			}
			if (plank)
			{
				const EVec Nr = N[i] * std::cos(rot) + B * std::sin(rot);
				const EVec Br = B * std::cos(rot) - N[i] * std::sin(rot);
				for (int32_t j = 0; j < sides; j++)
				{
					EVec v = c[i] + Nr * ring[(size_t)j].x + Br * ring[(size_t)j].y;
					if (tipShift != 0.0)
						v += T[i] * (tipShift * p.splinter * 3.0 * hw * splinterAt(ring[(size_t)j].y, i == 0 ? 0 : 1));
					out.points.push_back(v);
					out.exposure.push_back((float)ex[i]);
				}
				continue;
			}
			for (int32_t j = 0; j < sides; j++)
			{
				const double th = 2.0 * kPi * (double)j / (double)sides;
				const double a = th + rot;
				const double rr = (p.rebarResolution>=1.0 ? Profile(bp.type,r,th,s,ex[i]) : r + std::clamp(p.rebarResolution,0.0,1.0)*(Profile(bp.type,r,th,s,ex[i])-r)) * rs;
				EVec v = c[i] + (N[i] * std::cos(a) + B * std::sin(a)) * rr;
				if (tipShift != 0.0)
					v += T[i] * (tipShift * 0.35 * r * p.neck * (Noise1(th * 1.7, key ^ (i == 0 ? 0x77 : 0x78)) ));
				out.points.push_back(v);
				out.exposure.push_back((float)ex[i]);
			}
		}
		auto vid = [&](size_t i, int32_t j) { return base + (int32_t)((i % nr) * (size_t)sides + (size_t)(j % sides)); };
		const size_t rings = pc.loop ? nr : nr - 1;
		for (size_t i = 0; i < rings; i++)
			for (int32_t j = 0; j < sides; j++)
			{
				out.polys.push_back({ vid(i, j), vid(i, j + 1), vid(i + 1, j + 1), vid(i + 1, j) });
				out.polyBar.push_back(island);
			}
		if (!pc.loop)
		{
			for (int e = 0; e < 2; e++)
			{
				const size_t i = e == 0 ? 0 : nr - 1;
				EVec cen = c[i];
				const bool snapped = (e == 0 ? pc.end0 : pc.end1) == 1;
				if (snapped && plank)
					cen += T[i] * ((e == 0 ? -1.0 : 1.0) * p.splinter * 3.0 * hw * 0.3);
				else if (snapped)
					cen += T[i] * ((e == 0 ? -1.0 : 1.0) * 0.4 * r * p.neck * Range(key, 40 + e, 0.3, 1.0));
				const int32_t ci = (int32_t)out.points.size();
				out.points.push_back(cen);
				out.exposure.push_back((float)ex[i]);
				for (int32_t j = 0; j < sides; j++)
				{
					if (e == 0) out.polys.push_back({ ci, vid(i, j + 1), vid(i, j), vid(i, j) });
					else        out.polys.push_back({ ci, vid(i, j), vid(i, j + 1), vid(i, j + 1) });
					out.polyBar.push_back(island);
				}
			}
		}
		out.bars++;
	}

	                                                                     
	static bool InConcrete(const Region& rg, const EVec& x)
	{
		if (rg.hasLeft)
			return rg.left.Inside(x);
		if (!rg.solid.Inside(x))
			return false;
		for (const SolidTest& c : rg.cut)
			if (c.Inside(x)) return false;
		return true;
	}

	                                                                       
	                                                                        
	                                                                         
	                                                    
	bool HeldByConcrete(const Polyline& pl, Piece& q, const Region& rg, double r) const
	{
		const double hs = std::clamp(0.5 * r, 0.02, 1.0);
		const double minGrip = std::max(2.0 * r, 0.4 * std::max(0.0, p.embed * sc));
		                                                                     
		                                                                   
		                                 
		{
			const double L = pl.Length();
			const double maxRun = 4.0 * std::max(0.0, p.embed * sc) + 20.0 * r;
			for (int e = 0; e < 2; e++)
			{
				if ((e == 0 ? q.end0 : q.end1) == 1 || q.spans.empty())
					continue;
				const Span& sp = e == 0 ? q.spans.front() : q.spans.back();
				if (!(e == 0 ? sp.anchorA : sp.anchorB))
					continue;
				const double dir = e == 0 ? -1.0 : 1.0;
				double s0 = e == 0 ? q.s0 : q.s1;
				const double edge = e == 0 ? sp.a : sp.b;
				double grip = 0.0;
				for (double t = edge; dir * (t - edge) <= dir * (s0 - edge) + 1e-9; t += dir * hs)
					if (InConcrete(rg, pl.At(t))) grip += hs;
				double t = s0;
				while (grip < minGrip && std::fabs(t - edge) < maxRun)
				{
					const double nt = t + dir * hs;
					if (nt < 0.0 || nt > L || !rg.solid.Inside(pl.At(nt)))
						break;
					t = nt;
					if (InConcrete(rg, pl.At(t))) grip += hs;
				}
				if (e == 0) q.s0 = std::min(q.s0, t); else q.s1 = std::max(q.s1, t);
			}
		}
		const double len = q.s1 - q.s0;
		const size_t n = std::max<size_t>(2, (size_t)std::ceil(len / hs) + 1);
		std::vector<double> s(n);
		std::vector<char> in(n);
		double held = 0.0;
		for (size_t i = 0; i < n; i++)
		{
			s[i] = q.s0 + len * (double)i / (double)(n - 1);
			in[i] = InConcrete(rg, pl.At(s[i]));
			if (in[i]) held += len / (double)(n - 1);
		}
		                                                                    
		                                                          
		bool anchored = false;
		for (size_t k = 0; k < q.spans.size(); k++)
		{
			Span& sp = q.spans[k];
			const double from = k == 0 ? q.s0 - 1e-9 : q.spans[k - 1].b;
			const double to = k + 1 == q.spans.size() ? q.s1 + 1e-9 : q.spans[k + 1].a;
			bool a = false, b = false;
			for (size_t i = 0; i < n; i++)
			{
				if (!in[i]) continue;
				if (s[i] < sp.a && s[i] >= from) a = true;
				if (s[i] > sp.b && s[i] <= to) b = true;
			}
			sp.anchorA = sp.anchorA && a;
			sp.anchorB = sp.anchorB && b;
			anchored |= sp.anchorA || sp.anchorB;
		}
		                                                                         
		const double minHold = std::max(minGrip, 0.08 * len);
		return anchored && held >= minHold;
	}

	                                                                         
	std::vector<Span> ExposedSpans(const Polyline& pl, const Region& rg, bool strokeTest) const
	{
		const double L = pl.Length();
		const double hs = std::clamp(0.6 * std::max(1e-3, p.radius * sc), 0.02, 1.0);
		const size_t ns = (size_t)std::ceil(L / hs) + 1;
		std::vector<double> s(ns);
		std::vector<char> v(ns);
		                                                               
		auto exposed = [&](const EVec& q)
		{
			if (strokeTest)
				return !InConcrete(rg, q);
			bool inS, inR;
			rg.State(q, inS, inR);
			return inR;
		};
		for (size_t i = 0; i < ns; i++)
		{
			s[i] = std::min(L, (double)i * L / (double)std::max<size_t>(1, ns - 1));
			v[i] = exposed(pl.At(s[i]));
		}
		std::vector<Span> out;
		for (const Interval& e : Intervals(pl, s, v, exposed))
			out.push_back({ e.a, e.b, true, true });
		return out;
	}

	                                                                     
	                                                                     
	                                                                      
	                                                                          
	void ProcessWhole(const BarPath& bp, const Region& rg)
	{
		std::vector<EVec> pts = bp.pts;
		const bool trim = p.dropLoose && rg.hasLeft;
		if (trim && bp.closed && pts.size() > 2)
		{
			size_t k = 0;
			while (k < pts.size() && !InConcrete(rg, pts[k])) k++;
			if (k == pts.size())
				return;
			std::rotate(pts.begin(), pts.begin() + k, pts.end());
		}
		if (bp.closed && pts.size() > 2)
			pts.push_back(pts.front());
		Polyline pl;
		pl.Init(pts);
		const double L = pl.Length();
		const double r = bp.radius;
		if (L <= 2.0 * r)
			return;
		if (!trim)
		{
			Piece pc;
			pc.s0 = 0.0; pc.s1 = L; pc.loop = bp.closed; pc.key = Key(bp.key, 0xA1);
			pc.spans = ExposedSpans(pl, rg, false);
			Emit(bp, pl, pc);
			return;
		}
		const double hs = std::clamp(0.5 * r, 0.02, 1.0);
		const size_t n = std::max<size_t>(2, (size_t)std::ceil(L / hs) + 1);
		std::vector<double> s(n);
		std::vector<char> in(n), keep(n);
		bool all = true;
		for (size_t i = 0; i < n; i++)
		{
			s[i] = L * (double)i / (double)(n - 1);
			in[i] = InConcrete(rg, pl.At(s[i]));
			keep[i] = in[i];
			all &= in[i] != 0;
		}
		if (all)
		{
			Piece pc;
			pc.s0 = 0.0; pc.s1 = L; pc.loop = bp.closed; pc.key = Key(bp.key, 0xA1);
			pc.spans = ExposedSpans(pl, rg, false);
			Emit(bp, pl, pc);
			return;
		}
		const double maxRun = std::max(p.reach * sc, 2.0 * std::max(0.0, p.embed * sc) + 10.0 * r);
		for (size_t i = 0; i < n;)
		{
			if (in[i]) { i++; continue; }
			size_t j = i;
			while (j + 1 < n && !in[j + 1]) j++;
			const bool lc = i > 0, rc = j + 1 < n;
			for (size_t k = i; k <= j; k++)
				keep[k] = (lc && rc) || (lc && s[k] - s[i - 1] <= maxRun) || (rc && s[j + 1] - s[k] <= maxRun);
			i = j + 1;
		}
		                                                                       
		Polyline wl;
		const Polyline* use = &pl;
		if (bp.closed)
		{
			size_t m = 0;
			while (m < n && keep[m]) m++;
			if (m == n)
			{
				Piece pc;
				pc.s0 = 0.0; pc.s1 = L; pc.loop = true; pc.key = Key(bp.key, 0xA1);
				pc.spans = ExposedSpans(pl, rg, false);
				Emit(bp, pl, pc);
				return;
			}
			std::vector<EVec> rp;
			std::vector<char> rin, rkeep;
			for (size_t k = 0; k + 1 < n; k++)
			{
				const size_t q = (m + k) % (n - 1);
				rp.push_back(pl.At(s[q])); rin.push_back(in[q]); rkeep.push_back(keep[q]);
			}
			rp.push_back(rp.front()); rin.push_back(rin.front()); rkeep.push_back(rkeep.front());
			wl.Init(rp);
			use = &wl;
			in = rin; keep = rkeep;
			for (size_t k = 0; k < n; k++)
				s[k] = wl.Length() * (double)k / (double)(n - 1);
		}
		const std::vector<Span> exp = ExposedSpans(*use, rg, false);
		const double minGrip = std::max(2.0 * r, 0.4 * std::max(0.0, p.embed * sc));
		uint32_t part = 0;
		for (size_t i = 0; i < n;)
		{
			if (!keep[i]) { i++; continue; }
			size_t j = i;
			double grip = 0.0;
			while (j + 1 < n && keep[j + 1]) { if (in[j]) grip += s[j + 1] - s[j]; j++; }
			Piece pc;
			pc.s0 = s[i]; pc.s1 = s[j]; pc.key = Key(bp.key, 0xA1 + part++);
			for (const Span& sp : exp)
				if (sp.b > pc.s0 && sp.a < pc.s1)
					pc.spans.push_back({ std::max(sp.a, pc.s0), std::min(sp.b, pc.s1), true, true });
			if (grip >= minGrip)
				Emit(bp, *use, pc);
			i = j + 1;
		}
	}

	                                                                          
	                                                                        
	                                                                          
	void ProcessStroke(BarPath bp, const Region& rg, double embedLen)
	{
		if (bp.pts.size() < 2)
			return;
		const double r = bp.radius;
		const double hs = std::clamp(0.5 * r, 0.02, 1.0);
		const double maxRun = 4.0 * std::max(embedLen, 0.0) + 20.0 * r;
		int held = 0;
		for (int e = 0; e < 2; e++)
		{
			const EVec end = e == 0 ? bp.pts.front() : bp.pts.back();
			const EVec prev = e == 0 ? bp.pts[std::min<size_t>(1, bp.pts.size() - 1)] : bp.pts[bp.pts.size() - 2];
			const EVec t = Normalized(end - prev, EVec(0.0));
			if (m::la::dot(t, t) == 0.0)
				continue;
			                                                                        
			double d = -1.0;
			for (double x = 0.0; x <= maxRun; x += hs)
				if (InConcrete(rg, end + t * x)) { d = x; break; }
			if (d < 0.0)
				continue;
			                                                               
			                                          
			double run = d;
			const double target = d + std::max(embedLen, 2.0 * r);
			while (run + hs <= target && rg.solid.Inside(end + t * (run + hs)))
				run += hs;
			double grip = 0.0;
			for (double x = d; x <= run + 1e-9; x += hs)
				if (InConcrete(rg, end + t * x)) grip += hs;
			if (grip < std::max(2.0 * r, 0.25 * embedLen))
				continue;
			held++;
			if (run > 1e-6)
			{
				const EVec ext = end + t * run;
				if (e == 0) bp.pts.insert(bp.pts.begin(), ext); else bp.pts.push_back(ext);
			}
		}
		if (p.dropLoose && held == 0)
			return;
		Polyline pl;
		pl.Init(bp.pts);
		const double L = pl.Length();
		if (L <= 0.5 * r)
			return;
		Piece pc;
		pc.s0 = 0.0; pc.s1 = L; pc.key = Key(bp.key, 0x57);
		pc.spans = ExposedSpans(pl, rg, true);
		Emit(bp, pl, pc);
	}

	                                                
	void Process(const BarPath& bp, const Region& rg)
	{
		if (!bp.closed)
		{
			ProcessOpen(bp, rg, false);
			return;
		}
		const double r = bp.radius;
		Polyline pl;
		std::vector<EVec> pts = bp.pts;
		if (bp.closed && pts.size() > 2)
			pts.push_back(pts.front());
		pl.Init(pts);
		const double L = pl.Length();
		if (L <= 2.0 * r)
			return;
		const double hs = std::clamp(1.2 * r, 0.02, 2.0);
		const size_t ns = (size_t)std::ceil(L / hs) + 1;
		std::vector<double> s(ns);
		std::vector<char> vS(ns), vR(ns);
		bool anyR = false, allR = true;
		for (size_t i = 0; i < ns; i++)
		{
			s[i] = std::min(L, (double)i * L / (double)(ns - 1));
			bool inS, inR;
			rg.State(pl.At(s[i]), inS, inR);
			vS[i] = inS; vR[i] = inR;
			anyR |= inR;
			allR &= inR;
		}
		if (!anyR)
			return;

		{
			if (allR)
			{
				if (p.reach > 0.0 || p.dropLoose)
					return;                                                          
				Piece pc;
				pc.s0 = 0.0; pc.s1 = L; pc.loop = true; pc.key = Key(bp.key, 0xC1);
				pc.spans.push_back({ 0.0, L, true, true });
				Emit(bp, pl, pc);
				return;
			}
			                                                                          
			size_t k0 = 0;
			for (size_t i = 0; i < ns; i++)
				if (vS[i] && !vR[i]) { k0 = i; break; }
			const double sh = s[k0];
			std::vector<EVec> un;
			const size_t steps = std::max<size_t>(8, (size_t)std::ceil(L / std::max(0.25 * r, 1e-3)));
			un.reserve(steps + 1);
			for (size_t i = 0; i <= steps; i++)
				un.push_back(pl.At(std::fmod(sh + L * (double)i / (double)steps, L)));
			BarPath open = bp;
			open.closed = false;
			open.pts = std::move(un);
			ProcessOpen(open, rg, true);
		}
	}

	void ProcessOpen(const BarPath& bp, const Region& rg, bool fromLoop)
	{
		const double r = bp.radius;
		Polyline pl;
		pl.Init(bp.pts);
		const double L = pl.Length();
		const double hs = std::clamp(1.2 * r, 0.02, 2.0);
		const size_t ns = (size_t)std::ceil(L / hs) + 1;
		std::vector<double> s(ns);
		std::vector<char> vS(ns), vR(ns);
		for (size_t i = 0; i < ns; i++)
		{
			s[i] = std::min(L, (double)i * L / (double)(ns - 1));
			bool inS, inR;
			rg.State(pl.At(s[i]), inS, inR);
			vS[i] = inS; vR[i] = inR;
		}
		auto predS = [&rg](const EVec& q) { bool a, b; rg.State(q, a, b); return a; };
		auto predR = [&rg](const EVec& q) { bool a, b; rg.State(q, a, b); return b; };
		const std::vector<Interval> J = Intervals(pl, s, vS, predS);
		std::vector<Interval> E = Intervals(pl, s, vR, predR);
		const double reach = std::max(0.0, p.reach * sc);
		if (reach > 0.0)
		{
			                                                                            
			std::vector<Interval> cut;
			for (const Interval& e : E)
			{
				const double len = e.b - e.a;
				if (!e.naturalA && !e.naturalB)
					continue;
				if (e.naturalA && e.naturalB && len > 2.0 * reach)
				{
					Interval x = e; x.b = e.a + reach; x.naturalB = false; x.snapB = true; cut.push_back(x);
					Interval y = e; y.a = e.b - reach; y.naturalA = false; y.snapA = true; cut.push_back(y);
				}
				else if (e.naturalA && !e.naturalB && len > reach)
				{
					Interval x = e; x.b = e.a + reach; x.snapB = true; cut.push_back(x);
				}
				else if (!e.naturalA && e.naturalB && len > reach)
				{
					Interval y = e; y.a = e.b - reach; y.snapA = true; cut.push_back(y);
				}
				else
					cut.push_back(e);
			}
			E.swap(cut);
		}
		if (E.empty())
			return;

		const double cover = bp.depthCover >= 0.0 ? bp.depthCover : std::max(0.0, p.cover * sc);
		const double embed = std::max(0.0, p.embed * sc);
		const double minExp = std::max(0.0, p.minExposed) * 2.0 * r;

		struct Raw { double s0, s1; Span sp; int32_t end0, end1; };
		std::vector<Raw> raws;
		for (size_t k = 0; k < E.size(); k++)
		{
			const Interval& e = E[k];
			if (e.b - e.a < minExp)
				continue;
			const double mid = 0.5 * (e.a + e.b);
			const Interval* j = nullptr;
			for (const Interval& jj : J)
				if (mid >= jj.a && mid <= jj.b) { j = &jj; break; }
			if (j == nullptr)
				continue;
			const double lo = j->naturalA ? j->a + cover : j->a;
			const double hi = j->naturalB ? j->b - cover : j->b;
			if (hi - lo <= r)
				continue;
			const uint64_t ek = Key(bp.key, 0xE0, k);
			const bool freeA = j->naturalA && e.a - j->a < cover + 2.0 * hs;
			const bool freeB = j->naturalB && j->b - e.b < cover + 2.0 * hs;
			Raw rw;
			rw.sp.a = std::max(lo, e.a);
			rw.sp.b = std::min(hi, e.b);
			rw.sp.anchorA = !freeA;
			rw.sp.anchorB = !freeB;
			rw.s0 = freeA ? lo : std::max(lo, e.a - embed * (p.layout == BREAK_REBAR_CENTRAL ? 1.0 : Range(ek, 1, 0.6, 1.4)));
			rw.s1 = freeB ? hi : std::min(hi, e.b + embed * (p.layout == BREAK_REBAR_CENTRAL ? 1.0 : Range(ek, 2, 0.6, 1.4)));
			rw.end0 = 0; rw.end1 = 0;
			if (e.snapA) { rw.s0 = rw.sp.a; rw.sp.anchorA = false; rw.end0 = 1; }
			if (e.snapB) { rw.s1 = rw.sp.b; rw.sp.anchorB = false; rw.end1 = 1; }
			if (rw.sp.b - rw.sp.a < 0.5 * r)
				continue;
			raws.push_back(rw);
		}
		if (raws.empty())
			return;

		                                                                   
		std::vector<Piece> pieces;
		for (const Raw& rw : raws)
		{
			if (!pieces.empty() && rw.s0 <= pieces.back().s1)
			{
				if (rw.s1 >= pieces.back().s1) pieces.back().end1 = rw.end1;
				pieces.back().s1 = std::max(pieces.back().s1, rw.s1);
				pieces.back().spans.push_back(rw.sp);
			}
			else
			{
				Piece pc;
				pc.s0 = rw.s0; pc.s1 = rw.s1;
				pc.end0 = rw.end0; pc.end1 = rw.end1;
				pc.spans.push_back(rw.sp);
				pieces.push_back(pc);
			}
		}

		uint64_t pieceNo = 0;
		for (Piece& pc : pieces)
		{
			pc.key = Key(bp.key, 0xB0, pieceNo++, fromLoop ? 1 : 0);
			std::vector<Piece> parts;
			                                       
			for (size_t k = 0; k < pc.spans.size(); k++)
			{
				Span& sp = pc.spans[k];
				const uint64_t tk = Key(pc.key, 0x7A, k);
				const double sl = sp.b - sp.a;
				if (p.trim > 0.0 && sp.anchorA != sp.anchorB && sl > minExp + r)
				{
					const double cut = std::min(sl - std::max(minExp, r), sl * p.trim * U01(tk, 1));
					if (cut > 0.25 * r)
					{
						if (!sp.anchorB) { sp.b -= cut; if (k + 1 == pc.spans.size()) { pc.s1 = sp.b; pc.end1 = 1; } }
						else             { sp.a += cut; if (k == 0) { pc.s0 = sp.a; pc.end0 = 1; } }
					}
				}
			}
			                      
			std::vector<std::pair<double, double>> gaps;
			for (size_t k = 0; k < pc.spans.size(); k++)
			{
				const Span& sp = pc.spans[k];
				const uint64_t bk = Key(pc.key, 0xBB, k);
				const double sl = sp.b - sp.a;
				if (bp.type == kPlankType && sp.anchorA && sp.anchorB && sl > 6.0 * r)
				{
					                                                                    
					if (U01(bk, 4) < p.missing)
						gaps.push_back({ sp.a + sl * Range(bk, 5, 0.02, 0.3), sp.b - sl * Range(bk, 6, 0.02, 0.3) });
					else if (U01(bk, 1) < p.broken)
					{
						const double g = sl * Range(bk, 3, 0.2, 0.6);
						const double at = sp.a + 0.5 * g + 0.05 * sl + (0.9 * sl - g) * U01(bk, 2);
						gaps.push_back({ at - 0.5 * g, at + 0.5 * g });
					}
				}
				else if (sp.anchorA && sp.anchorB && sl > 6.0 * r && U01(bk, 1) < p.broken)
				{
					const double at = sp.a + (sp.b - sp.a) * Range(bk, 2, 0.25, 0.75);
					const double g = r * Range(bk, 3, 0.4, 2.5);
					gaps.push_back({ at - 0.5 * g, at + 0.5 * g });
				}
			}
			if (gaps.empty())
			{
				parts.push_back(pc);
			}
			else
			{
				double from = pc.s0;
				int32_t fromEnd = pc.end0;
				for (size_t g = 0; g <= gaps.size(); g++)
				{
					Piece q;
					q.s0 = from; q.end0 = fromEnd;
					q.s1 = g < gaps.size() ? gaps[g].first : pc.s1;
					q.end1 = g < gaps.size() ? 1 : pc.end1;
					q.key = Key(pc.key, 0x9A, g);
					for (const Span& sp : pc.spans)
					{
						Span c2 = sp;
						if (c2.b <= q.s0 || c2.a >= q.s1) continue;
						if (c2.a < q.s0) { c2.a = q.s0; c2.anchorA = false; }
						if (c2.b > q.s1) { c2.b = q.s1; c2.anchorB = false; }
						if (c2.b - c2.a > 0.1 * r) q.spans.push_back(c2);
					}
					if (!q.spans.empty()) parts.push_back(q);
					if (g < gaps.size()) { from = gaps[g].second; fromEnd = 1; }
				}
			}
			for (Piece& q : parts)
			{
				bool anchored = false;
				for (const Span& sp : q.spans)
					anchored |= sp.anchorA || sp.anchorB;
				if (!anchored && p.dropLoose)
					continue;
				if (p.dropLoose && !HeldByConcrete(pl, q, rg, r))
					continue;
				Emit(bp, pl, q);
			}
		}
	}
};

int AxisOf(const EVec& ext, int32_t choice, bool longest)
{
	if (choice >= 1 && choice <= 3)
		return choice - 1;
	int a = 0;
	for (int k = 1; k < 3; k++)
		if (longest ? ext[k] > ext[a] : ext[k] < ext[a]) a = k;
	return a;
}

EVec Unit(int a)
{
	return EVec(a == 0 ? 1.0 : 0.0, a == 1 ? 1.0 : 0.0, a == 2 ? 1.0 : 0.0);
}

void GridPaths(const BreakRebarParams& p, double sc, const EVec& lo, const EVec& hi,
	uint64_t seed, std::vector<BarPath>& paths)
{
	const EVec ext = hi - lo;
	const int na = AxisOf(ext, p.axis, false);
	const EVec n = Unit(na);
	const double ang = p.angle * kPi / 180.0;
	const EVec u0 = Unit((na + 1) % 3), v0 = Unit((na + 2) % 3);
	const EVec u = u0 * std::cos(ang) + v0 * std::sin(ang);
	const EVec v = v0 * std::cos(ang) - u0 * std::sin(ang);
	const EVec cen = (lo + hi) * 0.5;
	const double R = 0.5 * m::la::length(ext) + 1.0;
	const double r = std::max(1e-3, p.radius * sc);
	const double cover = std::max(0.0, p.cover * sc);
	const double gap = 0.04 * r + (p.type == BREAK_REBAR_RIBBED || p.type == BREAK_REBAR_HELICAL ? p.ribHeight * r : 0.0);
	const double nLo = lo[na], nHi = hi[na];

	struct Layer { double outerC, innerC; double outSign; int32_t id; };
	std::vector<Layer> layers;
	const double step = 2.0 * r + gap;
	auto front = [&](int32_t id) { const double c0 = nHi - cover - r - gap; layers.push_back({ c0, c0 - step, 1.0, id }); };
	auto back = [&](int32_t id) { const double c0 = nLo + cover + r + gap; layers.push_back({ c0, c0 + step, -1.0, id }); };
	if (p.layout == BREAK_REBAR_GRID_DOUBLE)
	{
		if (nHi - nLo > 2.0 * (cover + 2.0 * step)) { front(0); back(1); }
		else front(0);
	}
	else if (p.side == 1) back(1);
	else if (p.side == 2)
	{
		const double mid = 0.5 * (nLo + nHi);
		layers.push_back({ mid + 0.5 * step, mid - 0.5 * step, 1.0, 2 });
	}
	else front(0);

	for (const Layer& ly : layers)
	{
		for (int fam = 0; fam < 2; fam++)
		{
			if ((fam == 0 && !p.barsU) || (fam == 1 && !p.barsV))
				continue;
			const bool isOuter = (fam == 0) == (p.outer == 0);
			const double depth = isOuter ? ly.outerC : ly.innerC;
			const EVec runDir = fam == 0 ? u : v;
			const EVec offDir = fam == 0 ? v : u;
			const double sp = std::max(4.0 * r, (fam == 0 ? p.spacingV : p.spacingU) * sc);
			const double c0 = m::la::dot(cen, offDir);
			const double t0 = m::la::dot(cen, runDir);
			const int64_t k0 = (int64_t)std::floor((c0 - R) / sp), k1 = (int64_t)std::ceil((c0 + R) / sp);
			for (int64_t k = k0; k <= k1; k++)
			{
				const uint64_t key = Key(seed, 0x6B1D, (uint64_t)(ly.id * 2 + fam), (uint64_t)k);
				if (U01(key, 1) >= p.amount)
					continue;
				const double off = (double)k * sp + std::clamp(p.jitter, 0.0, 0.45) * sp * Range(key, 2, -1.0, 1.0);
				BarPath bp;
				const EVec basePt = n * depth + offDir * off;
				const EVec a = basePt + runDir * (t0 - R), b = basePt + runDir * (t0 + R);
				bp.pts = { a, b };
				bp.radius = r;
				bp.type = p.type;
				bp.out = n * ly.outSign;
				bp.key = key;
				bp.family = fam; bp.row = k;
				bp.deeper = std::fabs(depth - ly.outerC);
				paths.push_back(std::move(bp));
			}
		}
	}
}

// ограничиваем глубину доски доступной толщиной объекта
void FitPlankDepth(const Region& rg, const BarPath& original, double halfThickness, double surfaceCover, bool centered, bool splitSides,
	std::vector<BarPath>& paths)
{
	const EVec normal = original.out;
	const EVec delta = original.pts.back() - original.pts.front();
	const double length = m::la::length(delta);
	if (length <= 1e-8) return;
	const EVec run = delta / length;
	const EVec lo = rg.solid.Lo(), hi = rg.solid.Hi();
	const EVec center = (lo + hi) * 0.5, half = (hi - lo) * 0.5;
	const double span = std::fabs(normal.x) * half.x + std::fabs(normal.y) * half.y + std::fabs(normal.z) * half.z;
	const double eps = std::max(1e-7, m::la::length(hi - lo) * 1e-7);
	const double rayStart = m::la::dot(center, normal) + span + 4.0 * eps;
	const double rayLength = 2.0 * span + 8.0 * eps;
	const double requested = m::la::dot(original.pts.front(), normal);
	const int samples = std::clamp(int(std::ceil(length / std::max(halfThickness, 0.5))), 8, 128);
	const double step = length / samples;
	struct Range { double lower, upper; int first, last; };
	std::vector<Range> groups;
	bool previousHit = false, fits = true;
	for (int i = 0; i <= samples; ++i)
	{
		const EVec point = original.pts.front() + run * (i * step);
		const EVec origin = point + normal * (rayStart - m::la::dot(point, normal));
		double entry = 0.0, exit = 0.0;
		if (!rg.solid.FirstHit(origin, -normal, rayLength, entry)
			|| !rg.solid.FirstHit(origin - normal * (entry + eps), -normal, rayLength - entry, exit))
		{
			previousHit = false;
			continue;
		}
		const double upper = rayStart - entry;
		double lower = upper - exit - eps;
		if (splitSides) lower = 0.5 * (lower + upper);
		if (upper - lower <= 4.0 * eps) { previousHit = false; continue; }
		const double localRequested = centered ? 0.5 * (lower + upper) : upper - surfaceCover - halfThickness;
		if (std::fabs(localRequested - requested) > eps || requested - halfThickness < lower + eps || requested + halfThickness > upper - eps) fits = false;
		if (previousHit && !groups.empty())
		{
			auto& group = groups.back();
			const double lowerCommon = std::max(group.lower, lower), upperCommon = std::min(group.upper, upper);
			const double minimum = std::min(2.0 * halfThickness, 0.5 * std::min(group.upper - group.lower, upper - lower));
			if (upperCommon - lowerCommon > std::max(minimum, 4.0 * eps))
			{
				group.lower = lowerCommon; group.upper = upperCommon; group.last = i;
				continue;
			}
		}
		groups.push_back({lower, upper, i, i});
		previousHit = true;
	}
	if (groups.empty()) { paths.push_back(original); return; }
	if (fits)
	{
		BarPath path = original;
		if (centered)
		{
			double limit = surfaceCover;
			for (const auto& group : groups) limit = std::min(limit, std::max(0.0, group.upper - requested - halfThickness));
			path.depthCover = limit;
		}
		paths.push_back(std::move(path));
		return;
	}
	for (size_t k = 0; k < groups.size(); ++k)
	{
		const auto& group = groups[k];
		const double thickness = group.upper - group.lower;
		const double radius = std::min(halfThickness, 0.45 * thickness);
		const double localRequested = centered ? 0.5 * (group.lower + group.upper) : group.upper - surfaceCover - radius;
		const double depth = std::clamp(localRequested, group.lower + radius + eps, group.upper - radius - eps);
		const double runCenter = m::la::dot(center - original.pts.front(), run);
		const double runSpan = std::fabs(run.x) * half.x + std::fabs(run.y) * half.y + std::fabs(run.z) * half.z;
		const double a = k == 0 ? runCenter - runSpan : std::max(0.0, (group.first - 0.5) * step);
		const double b = k + 1 == groups.size() ? runCenter + runSpan : std::min(length, (group.last + 0.5) * step);
		if (b - a <= 2.0 * radius) continue;
		BarPath path = original;
		const EVec base = original.pts.front() + normal * (depth - requested);
		path.pts = {base + run * a, base + run * b};
		path.plankHalfLimit = radius;
		path.depthCover = std::max(0.0, group.upper - depth - radius);
		if (groups.size() > 1) path.key = Key(path.key, 0x4445505448ULL, k);
		paths.push_back(std::move(path));
	}
}

void PlankPaths(const BreakRebarParams& p, double sc, const Region& rg,
	const EVec& lo, const EVec& hi, uint64_t seed, std::vector<BarPath>& paths)
{
	double exposed[3][2] = {};
	const auto& points = rg.solid.Points();
	for (const ProjectDamagePoly& face : rg.solid.Tris())
	{
		const EVec a = points[face.a], b = points[face.b], c = points[face.c];
		const EVec faceLo = m::la::min(a, m::la::min(b, c)), faceHi = m::la::max(a, m::la::max(b, c));
		if (faceHi.x < rg.winLo.x || faceHi.y < rg.winLo.y || faceHi.z < rg.winLo.z
			|| faceLo.x > rg.winHi.x || faceLo.y > rg.winHi.y || faceLo.z > rg.winHi.z) continue;
		const EVec cross = m::la::cross(b - a, c - a);
		const double area = m::la::length(cross);
		if (area < 1e-12) continue;
		int axis = 0;
		for (int k = 1; k < 3; ++k) if (std::fabs(cross[k]) > std::fabs(cross[axis])) axis = k;
		exposed[axis][cross[axis] > 0.0 ? 1 : 0] += area;
	}
	const bool multiple = p.fit == BREAK_REBAR_FIT_SHAPE || p.layout == BREAK_REBAR_CAGE || p.layout == BREAK_REBAR_CAGE_ROUND;
	const int selected = AxisOf(hi - lo, p.axis, false);
	const EVec up = UpOf(p);
	const double radius = std::max(1e-3, p.radius * sc);
	const double cover = std::max(0.0, p.cover * sc);
	const double pitch = std::max(1e-3, (p.plankWidth + std::max(0.0, p.plankGap)) * sc);
	const double angle = p.angle * kPi / 180.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		if ((p.axis >= 1 && p.axis <= 3 && axis != p.axis - 1) || (!multiple && axis != selected)) continue;
		for (int sign : {-1, 1})
		{
			if (!p.showAll && exposed[axis][sign > 0 ? 1 : 0] == 0.0) continue;
			const bool bothSides = p.layout != BREAK_REBAR_GRID;
			if (!bothSides && p.side != 2 && sign != (p.side == 1 ? -1 : 1)) continue;
			if (p.side == 2 && !bothSides && sign < 0) continue;
			const EVec normal = Unit(axis) * double(sign);
			EVec horizontal = m::la::cross(up, normal);
			if (m::la::length(horizontal) < 1e-6) horizontal = AnyPerp(normal);
			horizontal = Normalized(horizontal, AnyPerp(normal));
			const EVec vertical = Normalized(m::la::cross(normal, horizontal), up);
			const EVec direction = p.plankDir == 1 ? vertical : horizontal;
			const EVec across = m::la::cross(normal, direction);
			const EVec run = direction * std::cos(angle) + across * std::sin(angle);
			const EVec offset = m::la::cross(normal, run);
			const double depth = p.side == 2 && !bothSides ? 0.5 * (lo[axis] + hi[axis])
				: (sign > 0 ? hi[axis] - cover - radius : lo[axis] + cover + radius);
			double runLo = std::numeric_limits<double>::max(), runHi = -runLo, rowLo = runLo, rowHi = runHi;
			const EVec boundsLo = p.showAll ? lo : rg.winLo, boundsHi = p.showAll ? hi : rg.winHi;
			for (int corner = 0; corner < 8; ++corner)
			{
				const EVec q((corner & 1) ? boundsHi.x : boundsLo.x, (corner & 2) ? boundsHi.y : boundsLo.y, (corner & 4) ? boundsHi.z : boundsLo.z);
				runLo = std::min(runLo, m::la::dot(q, run)); runHi = std::max(runHi, m::la::dot(q, run));
				rowLo = std::min(rowLo, m::la::dot(q, offset)); rowHi = std::max(rowHi, m::la::dot(q, offset));
			}
			const int64_t first = int64_t(std::floor(rowLo / pitch)), last = int64_t(std::ceil(rowHi / pitch));
			for (int64_t row = first; row <= last; ++row)
			{
				BarPath path;
				const EVec base = Unit(axis) * depth + offset * (double(row) * pitch);
				path.pts = {base + run * runLo, base + run * runHi};
				path.radius = radius; path.type = p.type; path.out = normal;
				path.key = Key(seed, 0x504C414E4BULL, uint64_t(axis * 2 + (sign > 0)), uint64_t(row));
				path.family = 0; path.row = row;
				const double halfThickness = std::max(1e-3, 0.5 * p.plankThickness * sc);
				FitPlankDepth(rg, path, halfThickness, std::max(0.0, cover + radius - halfThickness), p.side == 2 && !bothSides, bothSides, paths);
			}
		}
	}
}

void CagePaths(const BreakRebarParams& p, double sc, const EVec& lo, const EVec& hi,
	uint64_t seed, std::vector<BarPath>& paths)
{
	const EVec ext = hi - lo;
	const int na = AxisOf(ext, p.axis, true);
	const EVec a = Unit(na), u = Unit((na + 1) % 3), v = Unit((na + 2) % 3);
	const EVec cen = (lo + hi) * 0.5;
	const double r = std::max(1e-3, p.radius * sc);
	const double rt = std::max(1e-3, p.tieRadius * sc);
	const double cover = std::max(0.0, p.cover * sc);
	const double gap = 0.04 * r + (p.type == BREAK_REBAR_RIBBED || p.type == BREAK_REBAR_HELICAL ? p.ribHeight * r : 0.0);
	const double aLo = lo[na] - 1.0, aHi = hi[na] + 1.0;
	const double hu = 0.5 * ext[(na + 1) % 3], hv = 0.5 * ext[(na + 2) % 3];
	const double cu = cen[(na + 1) % 3], cv = cen[(na + 2) % 3];
	const double ca = cen[na];
	const int32_t tieType = p.type == BREAK_REBAR_STRAND || p.type == BREAK_REBAR_TWISTED ? (int32_t)BREAK_REBAR_SMOOTH : p.type;
	const double tieSp = std::max(4.0 * rt, p.tieSpacing * sc);
	const double sp = std::max(4.0 * r, p.spacingU * sc);
	const EVec axisC = a * ca + u * cu + v * cv;

	auto longBar = [&](double pu, double pv, uint64_t idx)
	{
		const uint64_t key = Key(seed, 0xCA6E, 1, idx);
		BarPath bp;
		const EVec base = u * pu + v * pv;
		bp.pts = { base + a * aLo, base + a * aHi };
		bp.radius = r; bp.type = p.type; bp.outKind = 1; bp.axisC = axisC; bp.axisD = a; bp.key = key;
		bp.family = 0; bp.row = (int64_t)idx; bp.deeper = r + rt + gap;
		paths.push_back(std::move(bp));
	};

	if (p.layout == BREAK_REBAR_CAGE)
	{
		const double ins = cover + 2.0 * rt + r + gap;
		const double bu = hu - ins, bv = hv - ins;
		if (bu <= r || bv <= r)
			return;
		const int nu = std::max(2, (int)std::ceil(2.0 * bu / sp) + 1);
		const int nv = std::max(2, (int)std::ceil(2.0 * bv / sp) + 1);
		uint64_t idx = 0;
		for (int i = 0; i < nu; i++)
			for (int j = 0; j < nv; j++)
			{
				if (i != 0 && i != nu - 1 && j != 0 && j != nv - 1) continue;
				const double pu = cu - bu + 2.0 * bu * (double)i / (double)(nu - 1);
				const double pv = cv - bv + 2.0 * bv * (double)j / (double)(nv - 1);
				const uint64_t key = Key(seed, 0xCA6E, 1, idx);
				const bool corner = (i == 0 || i == nu - 1) && (j == 0 || j == nv - 1);
				if (corner || U01(key, 1) < p.amount) longBar(pu, pv, idx);
				idx++;
			}
		const double d = r + rt + gap;
		const int arcN = 6;
		const int64_t k0 = (int64_t)std::floor(aLo / tieSp), k1 = (int64_t)std::ceil(aHi / tieSp);
		for (int64_t k = k0; k <= k1; k++)
		{
			const double pa = (double)k * tieSp;
			if (pa < lo[na] + cover || pa > hi[na] - cover) continue;
			const uint64_t key = Key(seed, 0xCA6E, 2, (uint64_t)k);
			if (U01(key, 1) >= p.amount) continue;
			BarPath bp;
			const double cx[4] = { cu + bu, cu - bu, cu - bu, cu + bu };
			const double cy[4] = { cv + bv, cv + bv, cv - bv, cv - bv };
			for (int c = 0; c < 4; c++)
				for (int s = 0; s <= arcN; s++)
				{
					const double th = (double)c * 0.5 * kPi + 0.5 * kPi * (double)s / (double)arcN;
					bp.pts.push_back(a * pa + u * (cx[c] + d * std::cos(th)) + v * (cy[c] + d * std::sin(th)));
				}
			bp.closed = true;
			bp.radius = rt; bp.type = tieType; bp.outKind = 1; bp.axisC = axisC; bp.axisD = a; bp.key = key;
			bp.family = 1; bp.row = k;
			paths.push_back(std::move(bp));
		}
	}
	else
	{
		const double rc = std::min(hu, hv) - (cover + 2.0 * rt + r + gap);
		if (rc <= r)
			return;
		const int nb = std::max(4, (int)std::round(2.0 * kPi * rc / sp));
		for (int i = 0; i < nb; i++)
		{
			const uint64_t key = Key(seed, 0xCA6E, 1, (uint64_t)i);
			if (U01(key, 1) >= p.amount && i % std::max(1, nb / 4) != 0) continue;
			const double th = 2.0 * kPi * (double)i / (double)nb;
			longBar(cu + rc * std::cos(th), cv + rc * std::sin(th), (uint64_t)i);
		}
		const double rh = rc + r + rt + gap;
		const int segN = std::max(24, (int)std::ceil(2.0 * kPi * rh / std::max(0.5 * r, 0.05)));
		if (p.spiral)
		{
			BarPath bp;
			const double len = aHi - aLo;
			const int turns = std::max(1, (int)std::ceil(len / tieSp));
			for (int t = 0; t <= turns * segN; t++)
			{
				const double f = (double)t / (double)segN;
				const double th = 2.0 * kPi * f;
				const double pa = std::clamp(lo[na] + cover + f * tieSp, lo[na] + cover, hi[na] - cover);
				bp.pts.push_back(a * pa + u * (cu + rh * std::cos(th)) + v * (cv + rh * std::sin(th)));
				if (pa >= hi[na] - cover) break;
			}
			bp.radius = rt; bp.type = tieType; bp.outKind = 1; bp.axisC = axisC; bp.axisD = a;
			bp.key = Key(seed, 0xCA6E, 3);
			bp.family = 1;
			paths.push_back(std::move(bp));
		}
		else
		{
			const int64_t k0 = (int64_t)std::floor(aLo / tieSp), k1 = (int64_t)std::ceil(aHi / tieSp);
			for (int64_t k = k0; k <= k1; k++)
			{
				const double pa = (double)k * tieSp;
				if (pa < lo[na] + cover || pa > hi[na] - cover) continue;
				const uint64_t key = Key(seed, 0xCA6E, 2, (uint64_t)k);
				if (U01(key, 1) >= p.amount) continue;
				BarPath bp;
				for (int s = 0; s < segN; s++)
				{
					const double th = 2.0 * kPi * (double)s / (double)segN;
					bp.pts.push_back(a * pa + u * (cu + rh * std::cos(th)) + v * (cv + rh * std::sin(th)));
				}
				bp.closed = true;
				bp.radius = rt; bp.type = tieType; bp.outKind = 1; bp.axisC = axisC; bp.axisD = a; bp.key = key;
				bp.family = 1; bp.row = k;
				paths.push_back(std::move(bp));
			}
		}
	}
}


                                                                              
                                                                       

                                                                           
                                                                           
class SurfaceDistance
{
public:
	void Build(const std::vector<EVec>& pts, const std::vector<ProjectDamagePoly>& polys)
	{
		const size_t nt = polys.size();
		std::vector<Tri> tris(nt);
		std::vector<EVec> cen(nt);
		for (size_t t = 0; t < nt; t++)
		{
			tris[t] = { pts[(size_t)polys[t].a], pts[(size_t)polys[t].b], pts[(size_t)polys[t].c] };
			cen[t] = (tris[t].a + tris[t].b + tris[t].c) / 3.0;
		}
		order_.resize(nt);
		for (size_t i = 0; i < nt; i++)
			order_[i] = (int32_t)i;
		nodes_.clear();
		nodes_.reserve(nt / 2 + 1);
		tri_ = std::move(tris);
		if (nt != 0)
			BuildNode(0, (int32_t)nt, cen);
		std::vector<Tri> sorted(nt);
		for (size_t i = 0; i < nt; i++)
			sorted[i] = tri_[(size_t)order_[i]];
		tri_ = std::move(sorted);
		order_.clear();
		order_.shrink_to_fit();
	}

	bool Valid() const { return !nodes_.empty(); }

	                                                                               
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

                                                                           
bool BoxLike(const m::Manifold& solid, const EVec& lo, const EVec& hi)
{
	const EVec e = hi - lo;
	const double box = e.x * e.y * e.z;
	                                                                            
	return box > 0.0 && solid.Volume() >= 0.6 * box;
}

                                                                            
                                                                                 
struct ShapeFrame
{
	EVec a = EVec(0, 0, 1), u = EVec(1, 0, 0), v = EVec(0, 1, 0);
	EVec ToWorld(double x, double y, double z) const { return u * x + v * y + a * z; }
};

ShapeFrame MakeShapeFrame(const std::vector<EVec>& pts, int32_t axisChoice, const EVec& lo, const EVec& hi,
	const EVec& up = EVec(0, 0, 1), bool snapToAxes = true)
{
	ShapeFrame f;
	if (axisChoice == 4)
	{
		                                       
		f.a = Normalized(up, EVec(0, 0, 1));
		int least = 0;
		for (int k = 1; k < 3; k++)
			if (std::fabs(f.a[k]) < std::fabs(f.a[least])) least = k;
		f.u = Normalized(Unit(least) - f.a * m::la::dot(Unit(least), f.a), AnyPerp(f.a));
		f.v = m::la::cross(f.a, f.u);
		return f;
	}
	if (axisChoice >= 1 && axisChoice <= 3)
	{
		const int na = axisChoice - 1;
		f.a = Unit(na); f.u = Unit((na + 1) % 3); f.v = Unit((na + 2) % 3);
		return f;
	}
	EVec mean(0.0);
	for (const EVec& p : pts) mean += p;
	mean = mean / (double)std::max<size_t>(1, pts.size());
	double c[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
	for (const EVec& p : pts)
	{
		const EVec d = p - mean;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				c[i][j] += d[i] * d[j];
	}
	                                                   
	double V[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	for (int sweep = 0; sweep < 24; sweep++)
	{
		const double off = c[0][1] * c[0][1] + c[0][2] * c[0][2] + c[1][2] * c[1][2];
		if (off < 1e-22 * (c[0][0] * c[0][0] + c[1][1] * c[1][1] + c[2][2] * c[2][2] + 1e-300))
			break;
		for (int pI = 0; pI < 2; pI++)
			for (int qI = pI + 1; qI < 3; qI++)
			{
				if (std::fabs(c[pI][qI]) < 1e-300) continue;
				const double th = 0.5 * std::atan2(2.0 * c[pI][qI], c[qI][qI] - c[pI][pI]);
				const double cs = std::cos(th), sn = std::sin(th);
				for (int k = 0; k < 3; k++)
				{
					const double akp = c[k][pI], akq = c[k][qI];
					c[k][pI] = cs * akp - sn * akq;
					c[k][qI] = sn * akp + cs * akq;
				}
				for (int k = 0; k < 3; k++)
				{
					const double apk = c[pI][k], aqk = c[qI][k];
					c[pI][k] = cs * apk - sn * aqk;
					c[qI][k] = sn * apk + cs * aqk;
				}
				for (int k = 0; k < 3; k++)
				{
					const double vkp = V[k][pI], vkq = V[k][qI];
					V[k][pI] = cs * vkp - sn * vkq;
					V[k][qI] = sn * vkp + cs * vkq;
				}
			}
	}
	int best = 0;
	for (int k = 1; k < 3; k++)
		if (c[k][k] > c[best][best]) best = k;
	EVec a(V[0][best], V[1][best], V[2][best]);
	a = Normalized(a, Unit(AxisOf(hi - lo, 0, true)));
	int big = 0;
	for (int k = 1; k < 3; k++)
		if (std::fabs(a[k]) > std::fabs(a[big])) big = k;
	if (a[big] < 0.0) a = -a;
	if (snapToAxes && std::fabs(a[big]) > 0.95)
	{
		f.a = Unit(big); f.u = Unit((big + 1) % 3); f.v = Unit((big + 2) % 3);
		return f;
	}
	f.a = a;
	int least = 0;
	for (int k = 1; k < 3; k++)
		if (std::fabs(a[k]) < std::fabs(a[least])) least = k;
	f.u = Normalized(Unit(least) - a * m::la::dot(Unit(least), a), AnyPerp(a));
	f.v = m::la::cross(a, f.u);
	return f;
}

                                                                            
// ищем пути центральной арматуры внутри отдельных элементов формы
void CentralMemberPaths(const BreakRebarParams& p, double sc, const m::Manifold& source,
    const SolidTest& solid, uint64_t seed, std::vector<BarPath>& paths)
{
    const double radius = std::max(1e-3, p.radius * sc);
    const EVec extent = solid.Hi() - solid.Lo();
    const double span = std::max({extent.x, extent.y, extent.z});
    if (span <= 1e-8) return;
    const double step = std::max(radius * .65, span / 512.0);
    const double epsilon = span * 1e-7;
    SurfaceDistance distance;
    distance.Build(solid.Points(), solid.Tris());
    struct Sample { EVec pos; double clearance; };
    struct Track { std::vector<Sample> samples; };
    struct Route { std::vector<EVec> pts; double radius; bool closed = false; };
    std::vector<Route> routes;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (p.axis >= 1 && p.axis <= 3 && axis != p.axis - 1) continue;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const m::mat3x4 toLocal(EVec(Unit(u).x, Unit(v).x, Unit(axis).x),
            EVec(Unit(u).y, Unit(v).y, Unit(axis).y), EVec(Unit(u).z, Unit(v).z, Unit(axis).z), EVec(0.));
        const m::Manifold local = source.Transform(toLocal);
        const m::Box bounds = local.BoundingBox();
        const double length = bounds.max.z - bounds.min.z;
        const int count = std::max(8, std::min(512, (int)std::ceil(length / step)));
        const double dz = length / count;
        if (dz <= epsilon) continue;
        std::vector<Track> active, complete;
        for (int slice = 0; slice < count; ++slice)
        {
            const double z = bounds.min.z + (slice + .5) * dz;
            const m::Polygons polys = local.Slice(z);
            std::vector<Sample> candidates;
            for (const auto& poly : polys)
            {
                if (poly.size() < 3) continue;
                double area2 = 0.; m::vec2 centre(0.);
                double xx = 0., yy = 0., xy = 0.;
                for (size_t i = 0; i < poly.size(); ++i)
                {
                    const m::vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
                    const double cross = a.x * b.y - b.x * a.y;
                    area2 += cross; centre += (a + b) * cross;
                    xx += (a.x*a.x + a.x*b.x + b.x*b.x) * cross;
                    yy += (a.y*a.y + a.y*b.y + b.y*b.y) * cross;
                    xy += (2*a.x*a.y + a.x*b.y + b.x*a.y + 2*b.x*b.y) * cross;
                }
                if (area2 <= epsilon * epsilon) continue;
                centre = centre / (3. * area2);
                xx = xx / (6.*area2) - centre.x*centre.x;
                yy = yy / (6.*area2) - centre.y*centre.y;
                xy = xy / (12.*area2) - centre.x*centre.y;
                const double discriminant = std::sqrt(std::max(0., (xx-yy)*(xx-yy)+4*xy*xy));
                const double minor = (xx + yy - discriminant)*.5;
                const double major = (xx + yy + discriminant)*.5;
                if (minor <= epsilon*epsilon || major > minor*9.) continue;
                bool inside = false;
                for (const auto& boundary : polys)
                    for (size_t i=0,j=boundary.size()-1; i<boundary.size(); j=i++)
                    {
                        const auto a=boundary[i], b=boundary[j];
                        if ((a.y>centre.y)!=(b.y>centre.y) &&
                            centre.x < (b.x-a.x)*(centre.y-a.y)/(b.y-a.y)+a.x) inside=!inside;
                    }
                if (!inside) continue;
                const EVec pos = Unit(u)*centre.x + Unit(v)*centre.y + Unit(axis)*z;
                double clearance = span;
                for (const auto& boundary : polys)
                    for (size_t i=0;i<boundary.size();++i)
                    {
                        const auto a=boundary[i], b=boundary[(i+1)%boundary.size()];
                        const auto edge=b-a;
                        const double len2=m::la::dot(edge,edge);
                        const double t=len2>1e-20?std::clamp(m::la::dot(centre-a,edge)/len2,0.,1.):0.;
                        clearance=std::min(clearance,m::la::length(centre-a-edge*t));
                    }
                if (clearance < radius*.12 || !solid.Inside(pos)) continue;
                candidates.push_back({pos, clearance});
            }
            struct Match { size_t track, candidate; double cost; };
            std::vector<Match> matches;
            for (size_t t=0;t<active.size();++t)
            {
                const auto& samples=active[t].samples;
                EVec prediction=samples.back().pos + Unit(axis)*dz;
                if (samples.size()>1) prediction=samples.back().pos*2.-samples[samples.size()-2].pos;
                for(size_t c=0;c<candidates.size();++c)
                {
                    const double d=m::la::length(candidates[c].pos-samples.back().pos);
                    if(d > dz*2.1) continue;
                    const EVec middle=(candidates[c].pos+samples.back().pos)*.5;
                    if(!solid.Inside(middle)) continue;
                    matches.push_back({t,c,m::la::length(candidates[c].pos-prediction)});
                }
            }
            std::sort(matches.begin(),matches.end(),[](const Match&a,const Match&b){
                if(a.cost!=b.cost)return a.cost<b.cost;
                return a.track!=b.track?a.track<b.track:a.candidate<b.candidate;
            });
            std::vector<char> taken(candidates.size(),0),used(active.size(),0);
            for(const auto& match:matches)
            {
                if(used[match.track]||taken[match.candidate])continue;
                used[match.track]=1;taken[match.candidate]=1;
                active[match.track].samples.push_back(candidates[match.candidate]);
            }
            std::vector<Track> next;
            for(size_t t=0;t<active.size();++t)
            {
                if(used[t])next.push_back(std::move(active[t]));
                else if(active[t].samples.size()>=3)complete.push_back(std::move(active[t]));
            }
            for(size_t c=0;c<candidates.size();++c)
                if(!taken[c])next.push_back(Track{{candidates[c]}});
            active.swap(next);
        }
        for(auto& track:active)if(track.samples.size()>=3)complete.push_back(std::move(track));
        for(auto& track:complete)
        {
            auto& samples=track.samples;
            for(int iteration=0;iteration<2;++iteration)
            {
                auto smooth=samples;
                for(size_t i=1;i+1<samples.size();++i)
                {
                    const EVec pos=samples[i].pos*.5+(samples[i-1].pos+samples[i+1].pos)*.25;
                    if(solid.Inside(pos))smooth[i].pos=pos;
                }
                samples.swap(smooth);
            }
            Route route;route.radius=radius;
            auto flush=[&](){
                // не отбрасываем короткие опоры; изгиб считаем по всему пути
                double routeLength=0.;
                for(size_t k=1;k<route.pts.size();++k)
                    routeLength+=m::la::length(route.pts[k]-route.pts[k-1]);
                if(route.pts.size()>=3 && routeLength>std::max(1.5*dz,epsilon*4.))
                    routes.push_back(std::move(route));
                route=Route{};route.radius=radius;
            };
            for(size_t i=0;i<samples.size();++i)
            {
                const EVec tangent=samples[std::min(i+1,samples.size()-1)].pos-samples[i?i-1:0].pos;
                int owner=0;
                for(int a=1;a<3;++a)if(std::fabs(tangent[a])>std::fabs(tangent[owner])*1.015)owner=a;
                const bool keep=p.axis!=0||owner==axis;
                if(keep)
                {
                    if(route.pts.empty()&&i)route.pts.push_back(samples[i-1].pos);
                    route.pts.push_back(samples[i].pos);
                    route.radius=std::min(route.radius,samples[i].clearance*.72);
                }
                else if(!route.pts.empty()){route.pts.push_back(samples[i].pos);flush();}
            }
            flush();
        }
    }
    auto clearSegment=[&](const EVec&a,const EVec&b,double r){
        const int n=std::max(2,(int)std::ceil(m::la::length(b-a)/step));
        for(int i=0;i<=n;++i)
        {
            const EVec q=a+(b-a)*(double(i)/n);
            if(!solid.Inside(q)||distance.Distance(q,r)<r*.8)return false;
        }
        return true;
    };
    for(size_t i=0;i<routes.size();++i)
    {
        if(routes[i].pts.empty())continue;
        bool again=true;
        while(again)
        {
            again=false;
            for(size_t j=i+1;j<routes.size()&&!again;++j)
            {
                auto&a=routes[i];auto&b=routes[j];
                if(b.pts.empty())continue;
                for(int endA=0;endA<2&&!again;++endA)for(int endB=0;endB<2&&!again;++endB)
                {
                    const EVec pa=endA?a.pts.back():a.pts.front(), pb=endB?b.pts.back():b.pts.front();
                    const double gap=m::la::length(pa-pb);
                    if(gap>std::max(step*3.,10.*std::max(a.radius,b.radius)))continue;
                    const EVec ta=Normalized(endA?pa-a.pts[a.pts.size()-2]:pa-a.pts[1],Unit(0));
                    const EVec tb=Normalized(endB?pb-b.pts[b.pts.size()-2]:pb-b.pts[1],Unit(0));
                    if(m::la::dot(ta,tb)>-.8)continue;
                    if(gap>step*3. && (m::la::dot(ta,(pb-pa)/gap)<.95 || m::la::dot(tb,(pa-pb)/gap)<.95))continue;
                    if(!clearSegment(pa,pb,std::min(a.radius,b.radius)))continue;
                    if(!endA)std::reverse(a.pts.begin(),a.pts.end());
                    if(endB)std::reverse(b.pts.begin(),b.pts.end());
                    a.pts.back()=(pa+pb)*.5;
                    a.pts.insert(a.pts.end(),b.pts.begin()+1,b.pts.end());
                    a.radius=std::min(a.radius,b.radius);b.pts.clear();again=true;
                }
            }
        }
    }
    uint64_t serial=0;
    for(auto& route:routes)
    {
        if(route.pts.size()<3)continue;
        auto& pts=route.pts;
        if(pts.size()>8 && m::la::length(pts.front()-pts.back())<step*3. &&
            clearSegment(pts.front(),pts.back(),route.radius))
        {
            const EVec first=Normalized(pts[1]-pts.front(),Unit(0));
            const EVec last=Normalized(pts.back()-pts[pts.size()-2],Unit(0));
            if(m::la::dot(first,last)>.8)
            {
                pts.front()=(pts.front()+pts.back())*.5;pts.pop_back();route.closed=true;
            }
        }
        if(!route.closed)
        {
            for(int end=0;end<2;++end)
            {
                const EVec q=end?pts.back():pts.front();
                const EVec d=Normalized(end?q-pts[pts.size()-2]:q-pts[1],Unit(0));
                const EVec extended=q+d*step*.5;
                if(clearSegment(q,extended,route.radius))
                {if(end)pts.back()=extended;else pts.front()=extended;}
            }
        }
        BarPath bar;
        bar.pts=std::move(pts);bar.radius=route.radius;bar.type=p.type;bar.closed=route.closed;
        bar.axisC=bar.pts.front();bar.axisD=Normalized(bar.pts.back()-bar.pts.front(),Unit(2));
        bar.out=AnyPerp(bar.axisD);bar.outKind=0;bar.key=Key(seed,0xCE,serial++);
        paths.push_back(std::move(bar));
    }
}

void CentralPaths(const BreakRebarParams& p, double sc, const m::Manifold& source,
    const SolidTest& solid, uint64_t seed, std::vector<BarPath>& paths)
{
    const auto members=source.Decompose();
    if(members.size()<=1) {CentralMemberPaths(p,sc,source,solid,seed,paths);return;}
    uint64_t serial=0;
    for(const auto& member:members)
    {
        if(member.IsEmpty())continue;
        SolidTest local;local.Build(member);
        CentralMemberPaths(p,sc,member,local,Key(seed,0xCE,serial++),paths);
    }
}

struct SliceGrid
{
	double z = 0.0, x0 = 0.0, y0 = 0.0, h = 1.0;
	int32_t nx = 0, ny = 0;
	std::vector<float> D;                                                       
	m::Polygons polys;

	double At(int32_t i, int32_t j) const { return D[(size_t)j * (size_t)nx + (size_t)i]; }
	double Sample(double x, double y) const
	{
		if (nx < 2 || ny < 2) return -1.0;
		const double fx = (x - x0) / h, fy = (y - y0) / h;
		const int32_t i = (int32_t)std::floor(fx), j = (int32_t)std::floor(fy);
		if (i < 0 || j < 0 || i + 1 >= nx || j + 1 >= ny) return -1.0;
		const double tx = fx - i, ty = fy - j;
		return (At(i, j) * (1 - tx) + At(i + 1, j) * tx) * (1 - ty) + (At(i, j + 1) * (1 - tx) + At(i + 1, j + 1) * tx) * ty;
	}
};

void FillSliceGrid(SliceGrid& g, const ShapeFrame& f, const SurfaceDistance& sd, double cap)
{
	g.nx = g.ny = 0;
	if (g.polys.empty())
		return;
	double minX = std::numeric_limits<double>::max(), minY = minX, maxX = std::numeric_limits<double>::lowest(), maxY = maxX;
	for (const auto& poly : g.polys)
		for (const m::vec2& q : poly)
		{
			minX = std::min(minX, q.x); maxX = std::max(maxX, q.x);
			minY = std::min(minY, q.y); maxY = std::max(maxY, q.y);
		}
	if (maxX <= minX || maxY <= minY)
		return;
	g.x0 = minX - 2.0 * g.h;
	g.y0 = minY - 2.0 * g.h;
	g.nx = (int32_t)std::ceil((maxX - minX) / g.h) + 5;
	g.ny = (int32_t)std::ceil((maxY - minY) / g.h) + 5;
	g.D.assign((size_t)g.nx * (size_t)g.ny, (float)-g.h);
	std::vector<double> xs;
	for (int32_t j = 0; j < g.ny; j++)
	{
		const double y = g.y0 + j * g.h;
		xs.clear();
		for (const auto& poly : g.polys)
		{
			const size_t n = poly.size();
			for (size_t k = 0; k < n; k++)
			{
				const m::vec2& p = poly[k];
				const m::vec2& q = poly[(k + 1) % n];
				if ((p.y > y) != (q.y > y))
					xs.push_back(p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y));
			}
		}
		std::sort(xs.begin(), xs.end());
		for (size_t k = 0; k + 1 < xs.size(); k += 2)
		{
			const int32_t i0 = std::max<int32_t>(0, (int32_t)std::ceil((xs[k] - g.x0) / g.h));
			const int32_t i1 = std::min<int32_t>(g.nx - 1, (int32_t)std::floor((xs[k + 1] - g.x0) / g.h));
			for (int32_t i = i0; i <= i1; i++)
				g.D[(size_t)j * (size_t)g.nx + (size_t)i] = (float)sd.Distance(f.ToWorld(g.x0 + i * g.h, y, g.z), cap);
		}
	}
}

using Loop2 = std::vector<m::vec2>;

double LoopLength(const Loop2& l)
{
	double L = 0.0;
	for (size_t i = 0; i < l.size(); i++)
		L += m::la::length(l[(i + 1) % l.size()] - l[i]);
	return L;
}

                                                                                            
std::vector<Loop2> IsoLoops(const SliceGrid& g, double iso, double ds)
{
	std::vector<Loop2> loops;
	if (g.nx < 3 || g.ny < 3)
		return loops;
	const int32_t nx = g.nx, ny = g.ny;
	auto F = [&](int32_t i, int32_t j) { return g.At(i, j) - iso; };
	auto H = [&](int32_t i, int32_t j) { return (int64_t)((size_t)j * (size_t)nx + (size_t)i) * 2; };
	auto Vt = [&](int32_t i, int32_t j) { return (int64_t)((size_t)j * (size_t)nx + (size_t)i) * 2 + 1; };
	std::unordered_map<int64_t, m::vec2> pos;
	std::unordered_map<int64_t, std::vector<int64_t>> link;
	auto edgePoint = [&](int64_t id, int32_t i0, int32_t j0, int32_t i1, int32_t j1)
	{
		if (pos.count(id)) return;
		const double a = F(i0, j0), b = F(i1, j1);
		const double t = std::clamp(a / (a - b), 0.0, 1.0);
		pos[id] = m::vec2(g.x0 + (i0 + (i1 - i0) * t) * g.h, g.y0 + (j0 + (j1 - j0) * t) * g.h);
	};
	auto seg = [&](int64_t e0, int64_t e1) { link[e0].push_back(e1); link[e1].push_back(e0); };
	for (int32_t j = 0; j + 1 < ny; j++)
		for (int32_t i = 0; i + 1 < nx; i++)
		{
			const bool b0 = F(i, j) > 0, b1 = F(i + 1, j) > 0, b2 = F(i + 1, j + 1) > 0, b3 = F(i, j + 1) > 0;
			const int c = (b0 ? 1 : 0) | (b1 ? 2 : 0) | (b2 ? 4 : 0) | (b3 ? 8 : 0);
			if (c == 0 || c == 15) continue;
			const int64_t e[4] = { H(i, j), Vt(i + 1, j), H(i, j + 1), Vt(i, j) };
			if (b0 != b1) edgePoint(e[0], i, j, i + 1, j);
			if (b1 != b2) edgePoint(e[1], i + 1, j, i + 1, j + 1);
			if (b3 != b2) edgePoint(e[2], i, j + 1, i + 1, j + 1);
			if (b0 != b3) edgePoint(e[3], i, j, i, j + 1);
			const bool centre = (F(i, j) + F(i + 1, j) + F(i + 1, j + 1) + F(i, j + 1)) > 0.0;
			switch (c)
			{
			case 1: case 14: seg(e[3], e[0]); break;
			case 2: case 13: seg(e[0], e[1]); break;
			case 3: case 12: seg(e[3], e[1]); break;
			case 4: case 11: seg(e[1], e[2]); break;
			case 6: case 9:  seg(e[0], e[2]); break;
			case 7: case 8:  seg(e[3], e[2]); break;
			case 5:
				if (centre) { seg(e[0], e[1]); seg(e[2], e[3]); } else { seg(e[3], e[0]); seg(e[1], e[2]); }
				break;
			case 10:
				if (centre) { seg(e[3], e[0]); seg(e[1], e[2]); } else { seg(e[0], e[1]); seg(e[2], e[3]); }
				break;
			default: break;
			}
		}
	std::unordered_map<int64_t, bool> used;
	std::vector<int64_t> keys;
	keys.reserve(link.size());
	for (const auto& kv : link) keys.push_back(kv.first);
	std::sort(keys.begin(), keys.end());
	for (int64_t start : keys)
	{
		if (used[start]) continue;
		Loop2 l;
		int64_t prev = -1, cur = start;
		while (!used[cur])
		{
			used[cur] = true;
			l.push_back(pos[cur]);
			const auto& nb = link[cur];
			int64_t nxt = -1;
			for (int64_t c2 : nb)
				if (c2 != prev && !used[c2]) { nxt = c2; break; }
			if (nxt < 0) break;
			prev = cur;
			cur = nxt;
		}
		if (l.size() < 4) continue;
		                                            
		for (int it = 0; it < 2; it++)
		{
			Loop2 sm(l.size());
			for (size_t k = 0; k < l.size(); k++)
				sm[k] = l[k] * 0.5 + (l[(k + l.size() - 1) % l.size()] + l[(k + 1) % l.size()]) * 0.25;
			l.swap(sm);
		}
		const double L = LoopLength(l);
		const size_t n = std::max<size_t>(8, (size_t)std::ceil(L / std::max(ds, 1e-6)));
		Loop2 rs;
		rs.reserve(n);
		double acc = 0.0;
		size_t k = 0;
		double segLen = m::la::length(l[1 % l.size()] - l[0]);
		for (size_t q = 0; q < n; q++)
		{
			const double target = L * (double)q / (double)n;
			while (acc + segLen < target && k + 1 < l.size() + 1)
			{
				acc += segLen;
				k++;
				segLen = m::la::length(l[(k + 1) % l.size()] - l[k % l.size()]);
			}
			const double t = segLen > 0.0 ? (target - acc) / segLen : 0.0;
			rs.push_back(l[k % l.size()] + (l[(k + 1) % l.size()] - l[k % l.size()]) * std::clamp(t, 0.0, 1.0));
		}
		loops.push_back(std::move(rs));
	}
	return loops;
}

                                                                                  
struct LoopHit { int32_t loop = -1; double s = 0.0; m::vec2 q; double d = std::numeric_limits<double>::max(); };

LoopHit ClosestOnLoops(const std::vector<Loop2>& loops, const std::vector<double>& len, const m::vec2& p)
{
	LoopHit best;
	for (size_t li = 0; li < loops.size(); li++)
	{
		const Loop2& l = loops[li];
		double acc = 0.0;
		for (size_t k = 0; k < l.size(); k++)
		{
			const m::vec2 a = l[k], b = l[(k + 1) % l.size()];
			const m::vec2 ab = b - a;
			const double ll = m::la::dot(ab, ab);
			const double t = ll > 0.0 ? std::clamp(m::la::dot(p - a, ab) / ll, 0.0, 1.0) : 0.0;
			const m::vec2 q = a + ab * t;
			const double d = m::la::length(p - q);
			if (d < best.d) { best.d = d; best.loop = (int32_t)li; best.s = acc + std::sqrt(ll) * t; best.q = q; }
			acc += std::sqrt(ll);
		}
		(void)len;
	}
	return best;
}

m::vec2 PointOnLoop(const Loop2& l, double s)
{
	double acc = 0.0;
	for (size_t k = 0; k < l.size(); k++)
	{
		const m::vec2 a = l[k], b = l[(k + 1) % l.size()];
		const double sl = m::la::length(b - a);
		if (acc + sl >= s || k + 1 == l.size())
			return a + (b - a) * (sl > 0.0 ? std::clamp((s - acc) / sl, 0.0, 1.0) : 0.0);
		acc += sl;
	}
	return l.empty() ? m::vec2(0.0) : l[0];
}

void ChaikinOpen(std::vector<EVec>& pts, int iters)
{
	for (int it = 0; it < iters && pts.size() >= 3; it++)
	{
		std::vector<EVec> o;
		o.reserve(pts.size() * 2);
		o.push_back(pts.front());
		for (size_t i = 0; i + 1 < pts.size(); i++)
		{
			o.push_back(pts[i] * 0.75 + pts[i + 1] * 0.25);
			o.push_back(pts[i] * 0.25 + pts[i + 1] * 0.75);
		}
		o.push_back(pts.back());
		pts.swap(o);
	}
}

                                                                             
                                                                               
                                                                      
void ShapeSkeletonPaths(const BreakRebarParams& p, double sc, const m::Manifold& source,
	const std::vector<EVec>& srcPts, const SurfaceDistance& sd, const EVec& lo, const EVec& hi,
	uint64_t seed, std::vector<BarPath>& paths)
{
	                                                                      
	const ShapeFrame f = MakeShapeFrame(srcPts, p.material != BREAK_FILL_REBAR ? 4 : p.axis, lo, hi, UpOf(p));
	const double r = std::max(1e-3, p.radius * sc);
	const double rt = std::max(1e-3, p.tieRadius * sc);
	const double cover = std::max(0.0, p.cover * sc);
	const double gap = 0.04 * r + (p.type == BREAK_REBAR_RIBBED || p.type == BREAK_REBAR_HELICAL ? p.ribHeight * r : 0.0);
	const double sp = std::max(4.0 * r, p.spacingU * sc);
	const double tieSp = std::max(4.0 * rt, p.tieSpacing * sc);
	const double isoHoop = cover + rt;
	const double isoBar = cover + 2.0 * rt + r + gap;
	const double isoCore = isoBar + 0.75 * sp;
	const int32_t tieType = p.type == BREAK_REBAR_STRAND || p.type == BREAK_REBAR_TWISTED ? (int32_t)BREAK_REBAR_SMOOTH : p.type;

	                                                                
	const m::mat3x4 toLocal(EVec(f.u.x, f.v.x, f.a.x), EVec(f.u.y, f.v.y, f.a.y), EVec(f.u.z, f.v.z, f.a.z), EVec(0.0));
	const m::Manifold local = source.Transform(toLocal);
	const m::Box lb = local.BoundingBox();
	const double zLo = lb.min.z + cover + r, zHi = lb.max.z - cover - r;
	if (zHi <= zLo)
		return;
	const double extXY = std::max(lb.max.x - lb.min.x, lb.max.y - lb.min.y);
	double h = std::max(0.5 * r, std::min(sp / 6.0, (cover + r) / 1.5));
	h = std::max(h, extXY / 300.0);
	const double dz = std::clamp(0.5 * std::min(sp, tieSp), 2.0 * r, std::max(2.0 * r, (zHi - zLo) / 4.0));
	const int32_t nLong = std::min<int32_t>(2000, (int32_t)std::floor((zHi - zLo) / dz) + 1);

	std::vector<SliceGrid> grids;
	for (int32_t k = 0; k < nLong; k++)
	{
		SliceGrid g;
		g.z = zLo + dz * k;
		g.h = h;
		grids.push_back(std::move(g));
	}
	const size_t firstTie = grids.size();
	std::vector<int64_t> tieIdx;
	if (p.barsV)
	{
		const int64_t k0 = (int64_t)std::ceil((lb.min.z + cover + rt) / tieSp), k1 = (int64_t)std::floor((lb.max.z - cover - rt) / tieSp);
		for (int64_t k = k0; k <= k1 && grids.size() < 4000; k++)
		{
			SliceGrid g;
			g.z = (double)k * tieSp;
			g.h = h;
			grids.push_back(std::move(g));
			tieIdx.push_back(k);
		}
	}
	for (SliceGrid& g : grids)
		g.polys = local.Slice(g.z);
	const double cap = (p.core ? isoCore : std::max(isoBar, isoHoop)) + 2.0 * h;
	ProjectDamageParallelFor(0, (int64_t)grids.size(), [&](int64_t i) { FillSliceGrid(grids[(size_t)i], f, sd, cap); });

	const double ds = std::max(0.25 * r, std::min(h * 0.5, 0.5 * r + 0.1 * sp));

	        
	for (size_t gi = firstTie; gi < grids.size(); gi++)
	{
		const SliceGrid& g = grids[gi];
		const std::vector<Loop2> loops = IsoLoops(g, isoHoop, ds);
		for (const Loop2& l : loops)
		{
			const double L = LoopLength(l);
			if (L < 8.0 * r)
				continue;
			m::vec2 c(0.0);
			for (const m::vec2& q : l) c += q;
			c = c / (double)l.size();
			const uint64_t key = Key(Key(seed, 0x5E1E, 2, (uint64_t)tieIdx[gi - firstTie]),
				(uint64_t)(int64_t)std::llround(c.x / sp) * 7919ULL + (uint64_t)(int64_t)std::llround(c.y / sp));
			if (U01(key, 1) >= p.amount)
				continue;
			BarPath bp;
			for (const m::vec2& q : l)
				bp.pts.push_back(f.ToWorld(q.x, q.y, g.z));
			bp.closed = true;
			bp.radius = rt; bp.type = tieType; bp.outKind = 1;
			bp.axisC = f.ToWorld(c.x, c.y, g.z); bp.axisD = f.a; bp.key = key;
			bp.family = 1; bp.row = (int64_t)tieIdx[gi - firstTie];
			paths.push_back(std::move(bp));
		}
	}

	                                                            
	if (p.barsU)
	{
		struct Track { std::vector<EVec> pts; m::vec2 last; m::vec2 centre; uint64_t serial; };
		std::vector<Track> active, done;
		uint64_t serial = 0;
		const double maxStep = dz + 0.35 * sp;
		const double minSep = 0.6 * sp;
		const double cov = 0.75 * sp;
		for (int32_t k = 0; k < nLong; k++)
		{
			const SliceGrid& g = grids[(size_t)k];
			const std::vector<Loop2> loops = IsoLoops(g, isoBar, ds);
			std::vector<double> len(loops.size());
			std::vector<m::vec2> cen(loops.size(), m::vec2(0.0));
			for (size_t li = 0; li < loops.size(); li++)
			{
				len[li] = LoopLength(loops[li]);
				for (const m::vec2& q : loops[li]) cen[li] += q;
				cen[li] = cen[li] / (double)std::max<size_t>(1, loops[li].size());
			}
			struct Cand { size_t t; LoopHit hit; };
			std::vector<Cand> cands;
			std::vector<char> ended(active.size(), 1);
			for (size_t t = 0; t < active.size(); t++)
			{
				const LoopHit hit = ClosestOnLoops(loops, len, active[t].last);
				if (hit.loop >= 0 && hit.d <= maxStep)
					cands.push_back({ t, hit });
			}
			std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y)
				{ return x.hit.d < y.hit.d || (x.hit.d == y.hit.d && x.t < y.t); });
			std::vector<LoopHit> taken;
			for (const Cand& c : cands)
			{
				bool clash = false;
				for (const LoopHit& o : taken)
					if (m::la::length(o.q - c.hit.q) < minSep) { clash = true; break; }
				if (clash) continue;
				taken.push_back(c.hit);
				Track& tr = active[c.t];
				tr.pts.push_back(f.ToWorld(c.hit.q.x, c.hit.q.y, g.z));
				tr.last = c.hit.q;
				tr.centre = cen[(size_t)c.hit.loop];
				ended[c.t] = 0;
			}
			std::vector<Track> next;
			for (size_t t = 0; t < active.size(); t++)
			{
				if (ended[t]) { if (active[t].pts.size() >= 2) done.push_back(std::move(active[t])); }
				else next.push_back(std::move(active[t]));
			}
			                                                         
			for (size_t li = 0; li < loops.size(); li++)
			{
				const double L = len[li];
				if (L < 2.0 * r)
					continue;
				std::vector<double> at;
				for (const LoopHit& o : taken)
					if (o.loop == (int32_t)li) at.push_back(o.s);
				std::vector<double> seeds;
				if (at.empty())
				{
					const int32_t n = std::max<int32_t>(1, (int32_t)std::llround(L / sp));
					                                                                                          
					size_t kmax = 0;
					for (size_t q = 1; q < loops[li].size(); q++)
						if (loops[li][q].x > loops[li][kmax].x) kmax = q;
					double s0 = 0.0;
					for (size_t q = 0; q < kmax; q++)
						s0 += m::la::length(loops[li][q + 1] - loops[li][q]);
					for (int32_t q = 0; q < n; q++)
						seeds.push_back(std::fmod(s0 + L * (double)q / (double)n, L));
				}
				else
				{
					std::sort(at.begin(), at.end());
					for (size_t q = 0; q < at.size(); q++)
					{
						const double a0 = at[q] + cov;
						const double a1 = (q + 1 < at.size() ? at[q + 1] : at[0] + L) - cov;
						const double Lr = a1 - a0;
						if (Lr <= 0.0) continue;
						const int32_t n = (int32_t)std::floor(Lr / sp + 0.35);
						for (int32_t i = 0; i < n; i++)
							seeds.push_back(std::fmod(a0 + Lr * ((double)i + 0.5) / (double)n, L));
					}
				}
				for (double s : seeds)
				{
					Track tr;
					tr.last = PointOnLoop(loops[li], s);
					tr.centre = cen[li];
					tr.pts.push_back(f.ToWorld(tr.last.x, tr.last.y, g.z));
					tr.serial = serial++;
					next.push_back(std::move(tr));
				}
			}
			active.swap(next);
		}
		for (Track& tr : active)
			if (tr.pts.size() >= 2) done.push_back(std::move(tr));
		for (Track& tr : done)
		{
			double len = 0.0;
			for (size_t i = 1; i < tr.pts.size(); i++)
				len += m::la::length(tr.pts[i] - tr.pts[i - 1]);
			if (tr.pts.size() < 3 || len < std::max(sp, 6.0 * r))
				continue;
			const uint64_t key = Key(seed, 0x5E1E, 1, tr.serial);
			if (U01(key, 1) >= p.amount)
				continue;
			for (int it = 0; it < 2; it++)
			{
				std::vector<EVec> sm = tr.pts;
				for (size_t i = 1; i + 1 < tr.pts.size(); i++)
					sm[i] = tr.pts[i] * 0.5 + (tr.pts[i - 1] + tr.pts[i + 1]) * 0.25;
				tr.pts.swap(sm);
			}
			ChaikinOpen(tr.pts, 2);
			BarPath bp;
			bp.pts = std::move(tr.pts);
			bp.radius = r; bp.type = p.type; bp.outKind = 1;
			bp.axisC = f.ToWorld(tr.centre.x, tr.centre.y, 0.0); bp.axisD = f.a; bp.key = key;
			bp.family = 0; bp.row = (int64_t)tr.serial; bp.deeper = isoBar - isoHoop;
			paths.push_back(std::move(bp));
		}
	}

	                                             
	if (p.core)
	{
		const double x0 = lb.min.x, x1 = lb.max.x, y0 = lb.min.y, y1 = lb.max.y;
		const int64_t i0 = (int64_t)std::floor(x0 / sp), i1 = (int64_t)std::ceil(x1 / sp);
		const int64_t j0 = (int64_t)std::floor(y0 / sp), j1 = (int64_t)std::ceil(y1 / sp);
		for (int64_t i = i0; i <= i1; i++)
			for (int64_t j = j0; j <= j1; j++)
			{
				const double x = (double)i * sp, y = (double)j * sp;
				int32_t run0 = -1;
				for (int32_t k = 0; k <= nLong; k++)
				{
					const bool in = k < nLong && grids[(size_t)k].Sample(x, y) >= isoCore;
					if (in && run0 < 0) run0 = k;
					if (!in && run0 >= 0)
					{
						if (k - run0 >= 2)
						{
							const uint64_t key = Key(Key(seed, 0x5E1E, 3, (uint64_t)(i * 7919 + j)), (uint64_t)run0);
							if (U01(key, 1) < p.amount)
							{
								BarPath bp;
								bp.pts = { f.ToWorld(x, y, grids[(size_t)run0].z), f.ToWorld(x, y, grids[(size_t)(k - 1)].z) };
								bp.radius = r; bp.type = p.type; bp.outKind = 1;
								bp.axisC = f.ToWorld(0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.0); bp.axisD = f.a; bp.key = key;
								paths.push_back(std::move(bp));
							}
						}
						run0 = -1;
					}
				}
			}
	}
}

                                                                             
                                                                                 
                                                                       
                                                                            
                                                                
void SurfaceGridPaths(const BreakRebarParams& p, double sc, const Region& rg, const SurfaceDistance& sd,
	uint64_t seed, std::vector<BarPath>& paths, std::vector<std::pair<EVec, EVec>>& through)
{
	const EVec lo = rg.solid.Lo(), hi = rg.solid.Hi();
	const double r = std::max(1e-3, p.radius * sc);
	const double cover = std::max(0.0, p.cover * sc);
	const double gap = 0.04 * r + (p.type == BREAK_REBAR_RIBBED || p.type == BREAK_REBAR_HELICAL ? p.ribHeight * r : 0.0);
	const double step = 2.0 * r + gap;
	const double spU = std::max(4.0 * r, p.spacingU * sc), spV = std::max(4.0 * r, p.spacingV * sc);
	const double embed = std::max(0.0, p.embed * sc);
	const double ang = p.angle * kPi / 180.0;

	                                         
	const size_t nc = rg.cut.size();
	std::vector<EVec> bLo(nc), bHi(nc);
	std::vector<size_t> parent(nc);
	for (size_t i = 0; i < nc; i++)
	{
		bLo[i] = m::la::max(rg.cut[i].Lo(), lo);
		bHi[i] = m::la::min(rg.cut[i].Hi(), hi);
		parent[i] = i;
	}
	auto root = [&](size_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
	const double pad = std::max(spU, spV);
	for (size_t i = 0; i < nc; i++)
		for (size_t j = i + 1; j < nc; j++)
		{
			bool ov = true;
			for (int k = 0; k < 3; k++)
				ov &= bLo[i][k] - pad <= bHi[j][k] && bLo[j][k] - pad <= bHi[i][k];
			if (ov) parent[root(i)] = root(j);
		}

	const std::vector<EVec>& P = rg.solid.Points();
	const std::vector<ProjectDamagePoly>& T = rg.solid.Tris();
	for (size_t g = 0; g < nc; g++)
	{
		if (root(g) != g)
			continue;
		std::vector<size_t> members;
		EVec gLo(std::numeric_limits<double>::max()), gHi(std::numeric_limits<double>::lowest());
		for (size_t i = 0; i < nc; i++)
			if (root(i) == g)
			{
				members.push_back(i);
				gLo = m::la::min(gLo, bLo[i]);
				gHi = m::la::max(gHi, bHi[i]);
			}
		if (gLo.x > gHi.x || gLo.y > gHi.y || gLo.z > gHi.z)
			continue;
		                                                              
		EVec nSum(0.0), cSum(0.0);
		double area = 0.0;
		for (const ProjectDamagePoly& t : T)
		{
			const EVec a = P[(size_t)t.a], b = P[(size_t)t.b], c = P[(size_t)t.c];
			const EVec cen = (a + b + c) / 3.0;
			if (cen.x < gLo.x || cen.y < gLo.y || cen.z < gLo.z || cen.x > gHi.x || cen.y > gHi.y || cen.z > gHi.z)
				continue;
			bool cut = false;
			for (size_t i : members)
				if (rg.cut[i].Inside(cen)) { cut = true; break; }
			if (!cut) continue;
			const EVec nn = m::la::cross(b - a, c - a) * 0.5;
			const double ar = m::la::length(nn);
			nSum += rg.guide && ar>1e-20 ? rg.guide->Near(cen,nn/ar)*ar : nn;
			cSum += cen * ar;
			area += ar;
		}
		if (area <= 0.0)
		{
			                                                                   
			                                                                    
			                                                                   
			for (const ProjectDamagePoly& t : T)
			{
				const EVec a=P[(size_t)t.a], b=P[(size_t)t.b], d=P[(size_t)t.c];
				const EVec tLo=m::la::min(a,m::la::min(b,d)), tHi=m::la::max(a,m::la::max(b,d));
				if (tHi.x<gLo.x || tHi.y<gLo.y || tHi.z<gLo.z || tLo.x>gHi.x || tLo.y>gHi.y || tLo.z>gHi.z) continue;
				std::vector<EVec> patch{a,b,d};
				for (int axis=0; axis<3 && patch.size()>=3; ++axis)
					for (int side=0; side<2 && patch.size()>=3; ++side)
					{
						const double bound=side ? gHi[axis] : gLo[axis];
						std::vector<EVec> clipped;
						EVec prev=patch.back(); bool inPrev=side ? prev[axis]<=bound : prev[axis]>=bound;
						for (const EVec& cur : patch)
						{
							const bool inCur=side ? cur[axis]<=bound : cur[axis]>=bound;
							if (inCur!=inPrev)
							{
								const double f=(bound-prev[axis])/(cur[axis]-prev[axis]);
								clipped.push_back(prev+(cur-prev)*f);
							}
							if (inCur) clipped.push_back(cur);
							prev=cur;inPrev=inCur;
						}
						patch.swap(clipped);
					}
				for (size_t k=1; k+1<patch.size(); ++k)
				{
					const EVec cen=(patch[0]+patch[k]+patch[k+1])/3.0;
					bool cut=false;
					for (size_t i : members) if (rg.cut[i].Inside(cen)) {cut=true;break;}
					if (!cut) continue;
					const EVec nn=m::la::cross(patch[k]-patch[0],patch[k+1]-patch[0])*0.5;
					const double ar=m::la::length(nn);
					if (ar<=1e-20) continue;
					nSum+=rg.guide ? rg.guide->Near(cen,nn/ar)*ar : nn;
					cSum+=cen*ar;area+=ar;
				}
			}
		}
		if (area <= 0.0)
			continue;
		const EVec c = cSum / area;
		const double margin = 1.5 * embed + pad;
		const EVec wLo = m::la::max(gLo - EVec(margin), lo), wHi = m::la::min(gHi + EVec(margin), hi);
		                                                                         
		                                                                            
		double removed = 0.0;
		{
			const int samples = 4096;
			int hits = 0;
			const EVec ext = gHi - gLo;
			for (int i = 0; i < samples; i++)
			{
				const uint64_t sk = Key(seed, 0xD7, (uint64_t)g, (uint64_t)i);
				const EVec x = gLo + EVec(ext.x * U01(sk, 1), ext.y * U01(sk, 2), ext.z * U01(sk, 3));
				bool inCut = false;
				for (size_t m2 : members)
					if (rg.cut[m2].Inside(x)) { inCut = true; break; }
				if (inCut && rg.solid.Inside(x)) hits++;
			}
			removed = ext.x * ext.y * ext.z * (double)hits / (double)samples;
		}
		const double meanDepth = removed / area;
		if (m::la::length(nSum) < 0.35 * area || meanDepth > cover + 2.0 * pad)
		{
			through.push_back({ wLo, wHi });
			continue;
		}
		const EVec n = m::la::normalize(nSum);
		int least = 0;
		for (int k = 1; k < 3; k++)
			if (std::fabs(n[k]) < std::fabs(n[least])) least = k;
		EVec u0 = Normalized(Unit(least) - n * m::la::dot(Unit(least), n), AnyPerp(n));
		if (p.material != BREAK_FILL_REBAR)
		{
			                                                 
			const EVec lv = m::la::cross(UpOf(p), n);
			if (m::la::length(lv) > 0.2) u0 = m::la::normalize(lv);
		}
		const EVec v0 = m::la::cross(n, u0);
		const EVec u = u0 * std::cos(ang) + v0 * std::sin(ang);
		const EVec v = v0 * std::cos(ang) - u0 * std::sin(ang);

		const double H = m::la::length(hi - lo) + 1.0;
		double uMin = std::numeric_limits<double>::max(), uMax = std::numeric_limits<double>::lowest();
		double vMin = uMin, vMax = uMax;
		for (int k = 0; k < 8; k++)
		{
			const EVec q((k & 1) ? wHi.x : wLo.x, (k & 2) ? wHi.y : wLo.y, (k & 4) ? wHi.z : wLo.z);
			uMin = std::min(uMin, m::la::dot(q - c, u)); uMax = std::max(uMax, m::la::dot(q - c, u));
			vMin = std::min(vMin, m::la::dot(q - c, v)); vMax = std::max(vMax, m::la::dot(q - c, v));
		}

		struct Layer { int32_t id; double sign; bool centred; };
		std::vector<Layer> layers;
		if (p.layout == BREAK_REBAR_GRID_DOUBLE) { layers.push_back({ 0, 1.0, false }); layers.push_back({ 1, -1.0, false }); }
		else if (p.side == 1) layers.push_back({ 1, -1.0, false });
		else if (p.side == 2) layers.push_back({ 2, 1.0, true });
		else layers.push_back({ 0, 1.0, false });

		const double hs = std::max(0.5 * r, std::min(std::min(spU, spV) / 5.0, 2.0 * r));
		const double maxJump = 2.0 * hs + 0.5 * cover;
		const double minDepth = 0.7 * (cover + r);
		for (const Layer& ly : layers)
			for (int fam = 0; fam < 2; fam++)
			{
				if ((fam == 0 && !p.barsU) || (fam == 1 && !p.barsV))
					continue;
				const bool isOuter = (fam == 0) == (p.outer == 0);
				const double depth = cover + r + gap + (isOuter ? 0.0 : step);
				const EVec runDir = fam == 0 ? u : v;
				const EVec offDir = fam == 0 ? v : u;
				const double sp = fam == 0 ? spV : spU;
				const double tMin = fam == 0 ? uMin : vMin, tMax = fam == 0 ? uMax : vMax;
				const double oMin = fam == 0 ? vMin : uMin, oMax = fam == 0 ? vMax : uMax;
				const double cO = m::la::dot(c, offDir);
				const int64_t k0 = (int64_t)std::floor((cO + oMin) / sp), k1 = (int64_t)std::ceil((cO + oMax) / sp);
				const size_t ns = (size_t)std::ceil((tMax - tMin) / hs) + 1;
				for (int64_t k = k0; k <= k1; k++)
				{
					const uint64_t key = Key(seed, 0x5F17, (uint64_t)(ly.id * 2 + fam), (uint64_t)k);
					if (U01(key, 1) >= p.amount)
						continue;
					const double off = (double)k * sp + std::clamp(p.jitter, 0.0, 0.45) * sp * Range(key, 2, -1.0, 1.0) - cO;
					const EVec base = c + offDir * off;
					                                                     
					std::vector<double> th(ns), th2(ns);
					std::vector<char> hit(ns);
					const EVec dir = n * -ly.sign;
					for (size_t i = 0; i < ns; i++)
					{
						const EVec q = base + runDir * (tMin + (double)i * hs);
						double t = 0.0, t2 = 0.0;
						hit[i] = rg.solid.FirstHit(q - dir * H, dir, 2.0 * H, t);
						th[i] = t;
						if (hit[i] && ly.centred)
						{
							hit[i] = rg.solid.FirstHit(q + dir * H, -dir, 2.0 * H, t2);
							th2[i] = t2;
						}
					}
					                                                                                
					const int w = std::max(1, (int)std::llround(std::max(2.0 * r, sp / 3.0) / hs));
					std::vector<EVec> pt(ns);
					std::vector<char> ok(ns, 0);
					size_t a = 0;
					while (a < ns)
					{
						if (!hit[a]) { a++; continue; }
						size_t b = a + 1;
						while (b < ns && hit[b] && std::fabs(th[b] - th[b - 1]) <= maxJump
							&& (!ly.centred || std::fabs(th2[b] - th2[b - 1]) <= maxJump)) b++;
						for (size_t i = a; i < b; i++)
						{
							double s1 = 0.0, s2 = 0.0;
							int cnt = 0;
							for (int d = -w; d <= w; d++)
							{
								const int64_t ii = (int64_t)i + d;
								if (ii < (int64_t)a || ii >= (int64_t)b) continue;
								s1 += th[(size_t)ii]; s2 += th2[(size_t)ii]; cnt++;
							}
							s1 /= cnt; s2 /= cnt;
							const EVec q = base + runDir * (tMin + (double)i * hs);
							EVec x;
							if (ly.centred)
							{
								const EVec f1 = q - dir * H + dir * s1, f2 = q + dir * H - dir * s2;
								if (m::la::dot(f2 - f1, dir) < 2.0 * depth) continue;
								x = (f1 + f2) * 0.5 + n * (isOuter ? 0.5 * step : -0.5 * step);
							}
							else
								x = q - dir * H + dir * (s1 + depth);
							if (!rg.solid.Inside(x) || sd.Distance(x, minDepth) < minDepth)
								continue;
							pt[i] = x;
							ok[i] = 1;
						}
						a = b;
					}
					uint64_t run = 0;
					for (size_t i = 0; i < ns;)
					{
						if (!ok[i]) { i++; continue; }
						size_t j = i;
						std::vector<EVec> pts;
						while (j < ns && ok[j]) pts.push_back(pt[j++]);
						i = j;
						if (pts.size() < 3)
							continue;
						BarPath bp;
						bp.pts = std::move(pts);
						bp.radius = r;
						bp.type = p.type;
						bp.out = n * ly.sign;
						bp.key = Key(key, 0x52, run++, (uint64_t)g);
						bp.family = fam; bp.row = k;
						bp.deeper = isOuter ? 0.0 : step;
						paths.push_back(std::move(bp));
					}
				}
			}
	}
}

                                                                          
                                                    
                                                                           
struct SkeletonCache
{
	std::mutex lock;
	uint64_t key = 0;
	bool valid = false;
	std::vector<BarPath> paths;
};

constexpr int kSkeletonSlots = 4;

SkeletonCache& TheSkeletonCache(uint64_t key)
{
	static SkeletonCache cache[kSkeletonSlots];
	static std::mutex pick;
	static int next = 0;
	std::lock_guard<std::mutex> guard(pick);
	for (SkeletonCache& c : cache)
		if (c.valid && c.key == key)
			return c;
	for (SkeletonCache& c : cache)
		if (!c.valid)
			return c;
	SkeletonCache& c = cache[next];
	next = (next + 1) % kSkeletonSlots;
	return c;
}

                                                                              
                                                                        

bool ConcreteAt(const Region& rg, const EVec& x)
{
	if (rg.hasLeft)
		return rg.left.Inside(x);
	if (!rg.solid.Inside(x))
		return false;
	for (const SolidTest& c : rg.cut)
		if (c.Inside(x)) return false;
	return true;
}

bool RemovedAt(const Region& rg, const EVec& x)
{
	if (rg.hasLeft)
		return rg.solid.Inside(x) && !rg.left.Inside(x);
	bool inS, inR;
	rg.State(x, inS, inR);
	return inR;
}

struct Brick
{
	EVec c, t, h, o;                                                                   
	double a = 0.0, b = 0.0, e = 0.0;                                        
	double pitch = 0.0, course = 0.0, mort = 0.0;
	uint64_t key = 0;
	double fc = 0.0, fe = 0.0;                                                                   
	EVec toConcrete = EVec(0.0);                                                                 
};

EVec OutOf(const BarPath& bp, const EVec& pos, const EVec& tan)
{
	EVec o = bp.out;
	if (!bp.outs.empty())
		o = NearestOut(bp, pos);
	else if (bp.outKind == 1)
	{
		const EVec rel = pos - bp.axisC;
		o = rel - bp.axisD * m::la::dot(rel, bp.axisD);
	}
	o = o - tan * m::la::dot(o, tan);
	return Normalized(o, AnyPerp(tan));
}

EVec Rotate(const EVec& v, const EVec& axis, double ang)
{
	return v * std::cos(ang) + m::la::cross(axis, v) * std::sin(ang) + axis * (m::la::dot(axis, v) * (1.0 - std::cos(ang)));
}

                                                                         
                                                                       
EVec StrokeOut(const Region& rg, const BarPath& bp)
{
	const EVec t = Normalized(bp.pts.back() - bp.pts.front(), EVec(0, 0, 1));
	const EVec u = AnyPerp(t), v = m::la::cross(t, u);
	const double far = m::la::length(rg.solid.Hi() - rg.solid.Lo()) + 1.0;
	EVec acc(0.0);
	const EVec probes[3] = { bp.pts.front(), bp.pts.back(), bp.pts[bp.pts.size() / 2] };
	for (const EVec& x : probes)
	{
		if (!rg.solid.Inside(x)) continue;
		double best = std::numeric_limits<double>::max();
		EVec dir(0.0);
		for (int k = 0; k < 24; k++)
		{
			const double th = 2.0 * kPi * k / 24.0;
			const EVec d = u * std::cos(th) + v * std::sin(th);
			double hit = 0.0;
			if (rg.solid.FirstHit(x, d, far, hit) && hit < best) { best = hit; dir = d; }
		}
		acc += dir;
	}
	return Normalized(acc, u);
}

void LayCourse(const BarPath& bp, const BreakRebarParams& p, double sc, const Region& rg, bool all,
	std::vector<Brick>& out)
{
	const double len = std::max(0.1, p.brickLength * sc);
	const double hgt = std::max(0.1, p.brickHeight * sc);
	const double dep = std::max(0.1, p.brickDepth * sc);
	const double mort = std::max(0.0, p.mortar * sc);
	std::vector<EVec> pts = bp.pts;
	if (bp.closed && pts.size() > 2)
		pts.push_back(pts.front());
	Polyline pl;
	pl.Init(pts);
	const double L = pl.Length();
	double pitch = len + mort;
	if (L < 0.6 * pitch)
		return;
	std::vector<double> centres;
	const double shift = (bp.row & 1) ? 0.5 : 0.0;
	if (bp.closed)
	{
		const int n = std::max(3, (int)std::llround(L / pitch));
		pitch = L / n;
		for (int i = 0; i < n; i++)
			centres.push_back(((double)i + 0.5 + shift) * pitch);
	}
	else
	{
		const double bl = pitch - mort;
		const int64_t k0 = (int64_t)std::ceil((0.5 * bl) / pitch - 0.5 - shift);
		for (int64_t k = k0;; k++)
		{
			const double s = ((double)k + 0.5 + shift) * pitch;
			if (s + 0.5 * bl > L + 1e-9) break;
			if (s - 0.5 * bl >= -1e-9) centres.push_back(s);
		}
	}
	const double bl = std::max(0.1 * pitch, pitch - mort);
	const double jit = std::clamp(p.brickJitter, 0.0, 2.0);
	for (size_t i = 0; i < centres.size(); i++)
	{
		double s = centres[i];
		if (bp.closed) s = std::fmod(s, L);
		const EVec A = pl.At(std::max(0.0, s - 0.5 * bl)), B = pl.At(std::min(L, s + 0.5 * bl));
		Brick bk;
		bk.key = Key(bp.key, 0xB71C, (uint64_t)i);
		bk.c = pl.At(s) * 0.5 + (A + B) * 0.25;
		if (!all && !rg.InWindow(bk.c))
			continue;
		bk.t = Normalized(B - A, pl.Tangent(s));
		                                                                         
		const EVec up = UpOf(p);
		const EVec tl = bk.t - up * m::la::dot(bk.t, up);
		if (m::la::length(tl) > 0.3) bk.t = m::la::normalize(tl);
		bk.o = OutOf(bp, bk.c, bk.t);
		bk.h = Normalized(m::la::cross(bk.o, bk.t), AnyPerp(bk.o));
		if (m::la::dot(bk.h, up) < 0.0) { bk.t = -bk.t; bk.h = -bk.h; }
		               
		const double turn = jit * 0.05 * Range(bk.key, 1, -1.0, 1.0);
		bk.t = Rotate(bk.t, bk.o, turn); bk.h = Rotate(bk.h, bk.o, turn);
		const double tilt = jit * 0.03 * Range(bk.key, 2, -1.0, 1.0);
		bk.t = Rotate(bk.t, bk.h, tilt); bk.o = Rotate(bk.o, bk.h, tilt);
		bk.c += bk.o * (jit * 0.06 * dep * Range(bk.key, 3, -1.0, 1.0)) + bk.t * (jit * 0.04 * bl * Range(bk.key, 4, -1.0, 1.0));
		bk.a = 0.5 * bl * (1.0 + 0.015 * jit * Range(bk.key, 5, -1.0, 1.0));
		bk.b = 0.5 * hgt * (1.0 + 0.02 * jit * Range(bk.key, 6, -1.0, 1.0));
		bk.e = 0.5 * dep;
		bk.pitch = pitch; bk.course = hgt + mort; bk.mort = mort;
		                                                            
		int nc = 0, nr = 0, nt = 0;
		EVec sc0(0.0), sr0(0.0);
		for (int x = -1; x <= 1; x++)
			for (int y = -1; y <= 1; y++)
				for (int z = -1; z <= 1; z++)
				{
					const EVec q = bk.c + bk.t * (0.8 * bk.a * x) + bk.h * (0.75 * bk.b * y) + bk.o * (0.75 * bk.e * z);
					nt++;
					if (ConcreteAt(rg, q)) { nc++; sc0 += q; }
					else if (RemovedAt(rg, q)) { nr++; sr0 += q; }
				}
		bk.fc = (double)nc / nt;
		bk.fe = (double)nr / nt;
		if (nc == 0 && nr == 0 && !all)
			continue;
		if (nc > 0 && nr > 0)
			bk.toConcrete = Normalized(sc0 / (double)nc - sr0 / (double)nr, -bk.o);
		out.push_back(bk);
	}
}

                                                                             
                                                   
void EmitBrick(const Brick& bk, const BreakRebarParams& p, const Region& rg, bool broken, const EVec& keep,
	BreakRebarMesh& out)
{
	const double cmin = std::min(bk.a, std::min(bk.b, bk.e));
	const double ch = std::clamp(p.chamfer, 0.0, 0.45) * cmin;
	std::vector<m::vec3> pts;
	pts.reserve(24);
	int idx = 0;
	for (int sx = -1; sx <= 1; sx += 2)
		for (int sy = -1; sy <= 1; sy += 2)
			for (int sz = -1; sz <= 1; sz += 2)
				for (int k = 0; k < 3; k++)
				{
					const double w = ch * (0.6 + 0.8 * U01(bk.key, 20 + idx++));
					m::vec3 q(sx * (bk.a - (k != 0 ? w : 0.0)), sy * (bk.b - (k != 1 ? w : 0.0)), sz * (bk.e - (k != 2 ? w : 0.0)));
					pts.push_back(q);
				}
	m::Manifold brick = m::Manifold::Hull(pts);
	                                                                           
	                                                                        
	                                                                   
	m::Manifold mortar;
	const double mj = bk.mort;
	if (p.mortarJoints && !p.brick3D && mj > 1e-4)                          
	{
		const double recess = std::clamp(p.mortarRecess, 0.0, 0.9) * 2.0 * bk.e;
		const double zf = bk.e - recess * Range(bk.key, 60, 0.6, 1.4), zb = -0.92 * bk.e;
		if (zf > zb)
		{
			const double ov = 0.05 * mj;                                                       
			if (U01(bk.key, 61) >= 0.12)
			{
				const m::vec3 lo(-bk.a - 0.5 * mj, -bk.b - mj, zb), hi(bk.a + 0.5 * mj, -bk.b + ov, zf);
				mortar = mortar + m::Manifold::Cube(hi - lo).Translate(lo);
			}
			if (U01(bk.key, 62) >= 0.2)
			{
				const m::vec3 lo(bk.a - ov, -bk.b, zb), hi(bk.a + mj, bk.b, zf - 0.3 * recess * U01(bk.key, 63));
				mortar = mortar + m::Manifold::Cube(hi - lo).Translate(lo);
			}
		}
	}
	double cut[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	if (broken)
	{
		EVec n(m::la::dot(keep, bk.t), m::la::dot(keep, bk.h), m::la::dot(keep, bk.o));
		n = Normalized(n, EVec(1, 0, 0));
		for (int k = 0; k < 2; k++)
		{
			const uint64_t kk = Key(bk.key, 0xB4E, (uint64_t)k);
			EVec nk = Normalized(n + EVec(Range(kk, 1, -0.45, 0.45), Range(kk, 2, -0.45, 0.45), Range(kk, 3, -0.3, 0.3)), n);
			const double ext = bk.a * std::fabs(nk.x) + bk.b * std::fabs(nk.y) + bk.e * std::fabs(nk.z);
			const m::vec3 nv(nk.x, nk.y, nk.z);
			const double off = -ext * Range(kk, 4, 0.0, 0.45);
			brick = brick.TrimByPlane(nv, off);
			if (!mortar.IsEmpty()) mortar = mortar.TrimByPlane(nv, off);
			                                                                    
			cut[4 * k + 0] = nk.x * 2.0 * bk.a;
			cut[4 * k + 1] = nk.y * 2.0 * bk.b;
			cut[4 * k + 2] = nk.z * 2.0 * bk.e;
			cut[4 * k + 3] = off;
		}
	}
	if (brick.NumTri() == 0 || brick.Volume() < 0.05 * 8.0 * bk.a * bk.b * bk.e)
		return;
	if (p.brick3D)
	{
		                                                           
		const EVec cols[4] = { bk.t * (2.0 * bk.a), bk.h * (2.0 * bk.b), bk.o * (2.0 * bk.e), bk.c };
		for (const EVec& c : cols) { out.brickFrame.push_back(c.x); out.brickFrame.push_back(c.y); out.brickFrame.push_back(c.z); }
		out.brickKey.push_back((Int32)(bk.key & 0x7fffffffULL));
		out.brickCut.insert(out.brickCut.end(), cut, cut + 8);
		return;
	}
	auto append = [&](const m::Manifold& piece, Int32 part)
	{
		const m::MeshGL g = piece.GetMeshGL();
		const int32_t base = (int32_t)out.points.size();
		const size_t np = g.NumVert();
		for (size_t i = 0; i < np; i++)
		{
			const double x = g.vertProperties[i * g.numProp], y = g.vertProperties[i * g.numProp + 1], z = g.vertProperties[i * g.numProp + 2];
			const EVec w = bk.c + bk.t * x + bk.h * y + bk.o * z;
			out.points.push_back(w);
			out.exposure.push_back(ConcreteAt(rg, w) ? 0.0f : 1.0f);
		}
		for (size_t f = 0; f + 2 < g.triVerts.size(); f += 3)
		{
			const Int32 a = base + (Int32)g.triVerts[f], b = base + (Int32)g.triVerts[f + 1], c = base + (Int32)g.triVerts[f + 2];
			out.polys.push_back({ a, b, c, c });
			out.polyBar.push_back(out.bars);
			out.polyPart.push_back(part);
		}
		out.bars++;
	};
	append(brick, 0);
	                                                                            
	if (!mortar.IsEmpty())
		for (const m::Manifold& piece : mortar.Decompose())
			if (piece.Volume() > 0.02 * mj * bk.b * bk.e)
				append(piece, 1);
}

                                                                          
                                                                             
                                    
void BuildBricks(std::vector<Brick>& bricks, const BreakRebarParams& p, const Region& rg, bool all,
	BreakRebarMesh& out)
{
	const size_t n = bricks.size();
	if (n == 0)
		return;
	double cell = 0.0;
	for (const Brick& b : bricks) cell = std::max(cell, std::max(b.pitch, 2.0 * b.course));
	cell = std::max(cell, 1e-3);
	auto cellOf = [&](const EVec& x) { return std::array<int64_t, 3>{ (int64_t)std::floor(x.x / cell), (int64_t)std::floor(x.y / cell), (int64_t)std::floor(x.z / cell) }; };
	auto hashOf = [](const std::array<int64_t, 3>& c) { return (uint64_t)c[0] * 73856093ULL ^ (uint64_t)c[1] * 19349663ULL ^ (uint64_t)c[2] * 83492791ULL; };
	std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
	for (size_t i = 0; i < n; i++)
		grid[hashOf(cellOf(bricks[i].c))].push_back((uint32_t)i);
	std::vector<std::vector<uint32_t>> nb(n);
	for (size_t i = 0; i < n; i++)
	{
		const Brick& bi = bricks[i];
		const auto c = cellOf(bi.c);
		for (int dx = -1; dx <= 1; dx++)
			for (int dy = -1; dy <= 1; dy++)
				for (int dz = -1; dz <= 1; dz++)
				{
					const auto it = grid.find(hashOf({ c[0] + dx, c[1] + dy, c[2] + dz }));
					if (it == grid.end()) continue;
					for (uint32_t j : it->second)
					{
						if (j == i) continue;
						const EVec rel = bricks[j].c - bi.c;
						const double dt = std::fabs(m::la::dot(rel, bi.t));
						const double dh = std::fabs(m::la::dot(rel, bi.h));
						const double dout = std::fabs(m::la::dot(rel, bi.o));
						                                                                               
						const Brick& bj = bricks[j];
						const double tolG = 1.5 * bi.mort + 0.1 * std::min(bi.a, bi.b);
						const double gapT = dt - (bi.a + bj.a), gapH = dh - (bi.b + bj.b), gapO = dout - (bi.e + bj.e);
						if (gapO > -0.6 * std::min(bi.e, bj.e)) continue;
						const bool same = gapT <= tolG && gapH <= -0.8 * std::min(bi.b, bj.b);
						const bool next = gapH <= tolG && gapT <= -0.3 * std::min(bi.a, bj.a);
						if ((same || next) && std::find(nb[i].begin(), nb[i].end(), j) == nb[i].end())
						{
							nb[i].push_back(j);
							nb[j].push_back((uint32_t)i);
						}
					}
				}
	}
	const double minHeld = 0.15;
	std::vector<char> held(n);
	for (size_t i = 0; i < n; i++)
		held[i] = bricks[i].fc >= minHeld;
	auto bfs = [&](const std::vector<char>& use, std::vector<int32_t>& dist, std::vector<int32_t>& parent)
	{
		dist.assign(n, -1);
		parent.assign(n, -1);
		std::vector<uint32_t> queue;
		for (size_t i = 0; i < n; i++)
			if (held[i] && use[i]) { dist[i] = 0; queue.push_back((uint32_t)i); }
		for (size_t qi = 0; qi < queue.size(); qi++)
		{
			const uint32_t i = queue[qi];
			for (uint32_t j : nb[i])
				if (use[j] && dist[j] < 0) { dist[j] = dist[i] + 1; parent[j] = (int32_t)i; queue.push_back(j); }
		}
	};
	std::vector<char> any(n, 1), keep(n, 1);
	std::vector<int32_t> dist, parent;
	bfs(any, dist, parent);
	const double miss = std::clamp(p.missing, 0.0, 1.0);
	for (size_t i = 0; i < n; i++)
	{
		if (held[i])
		{
			                                                                  
			if (bricks[i].fe >= 0.5)
				keep[i] = U01(bricks[i].key, 7) >= miss * bricks[i].fe;
			continue;
		}
		if (dist[i] < 0)
		{
			keep[i] = !p.dropLoose && U01(bricks[i].key, 7) >= miss;
			continue;
		}
		const double pm = 1.0 - std::pow(1.0 - miss, (double)dist[i]);
		keep[i] = U01(bricks[i].key, 7) >= pm;
	}
	bfs(keep, dist, parent);
	if (p.dropLoose)
		for (size_t i = 0; i < n; i++)
			if (dist[i] < 0) keep[i] = 0;
	                                                                      
	                                                                         
	                                                                           
	                                                                        
	                                                                         
	                                                                   
	const EVec up = UpOf(p);
	std::vector<char> upright(n, 0);
	for (size_t i = 0; i < n; i++)
		upright[i] = std::fabs(m::la::dot(bricks[i].h, up)) >= 0.5;
	enum { kBelow = 1, kAbove = 2, kSide = 3 };
	auto relOf = [&](size_t i, size_t j)
	{
		const EVec rel = bricks[j].c - bricks[i].c;
		const double dv = m::la::dot(rel, up);
		if (std::fabs(dv) < 0.5 * bricks[i].b) return (int)kSide;
		return dv < 0.0 ? (int)kBelow : (int)kAbove;
	};
	                                                                                                 
	auto spans = [&](size_t i, int rel, const std::vector<char>& set)
	{
		const Brick& bi = bricks[i];
		double lo = 1e30, hi = -1e30;
		for (uint32_t j : nb[i])
		{
			if (!set[j] || relOf(i, j) != rel) continue;
			const double sj = m::la::dot(bricks[j].c - bi.c, bi.t);
			lo = std::min(lo, std::max(-bi.a, sj - bricks[j].a));
			hi = std::max(hi, std::min(bi.a, sj + bricks[j].a));
		}
		const double mg = 0.1 * bi.a;
		return lo <= -mg && hi >= mg;
	};
	std::vector<char> solid(n, 0), hang(n, 0);
	if (p.dropLoose)
	{
		for (size_t i = 0; i < n; i++)
			solid[i] = keep[i] && (held[i] || !upright[i]);
		                                                                   
		for (bool grew = true; grew;)
		{
			grew = false;
			for (size_t i = 0; i < n; i++)
				if (keep[i] && !solid[i] && spans(i, kBelow, solid)) { solid[i] = 1; grew = true; }
		}
		for (size_t i = 0; i < n; i++)
		{
			if (!keep[i] || solid[i]) continue;
			if (spans(i, kAbove, solid)) { hang[i] = 1; continue; }
			int l = 0, r = 0;
			for (uint32_t j : nb[i])
				if (solid[j] && relOf(i, j) == kSide)
					(m::la::dot(bricks[j].c - bricks[i].c, bricks[i].t) < 0.0 ? l : r)++;
			hang[i] = l > 0 && r > 0;
		}
		for (size_t i = 0; i < n; i++)
			if (upright[i] && !held[i]) keep[i] = solid[i] || hang[i];
	}
	                                                             
	std::vector<int32_t> children(n, 0);
	std::vector<EVec> leanDir(n, EVec(0.0));
	for (size_t i = 0; i < n; i++)
	{
		if (!keep[i] || held[i]) continue;
		const bool grav = p.dropLoose && upright[i];
		const int want = !grav ? 0 : solid[i] ? (int)kBelow : spans(i, kAbove, solid) ? (int)kAbove : (int)kSide;
		EVec acc(0.0);
		for (uint32_t j : nb[i])
		{
			if (!keep[j]) continue;
			if (!grav) { if ((int32_t)j != parent[i]) continue; }
			else if (relOf(i, j) != want || !solid[j]) continue;
			children[j]++;
			acc += bricks[j].c - bricks[i].c;
		}
		if (!grav && parent[i] >= 0) acc = bricks[(size_t)parent[i]].c - bricks[i].c;
		leanDir[i] = acc;
	}
	                                                                                 
	const double brk = std::clamp(p.brickBroken, 0.0, 1.0);
	std::vector<char> broken(n, 0);
	std::vector<EVec> keepDir(n, EVec(0.0));
	for (size_t i = 0; i < n; i++)
	{
		if (!keep[i] || children[i] != 0 || U01(bricks[i].key, 8) >= brk) continue;
		if (held[i])
		{
			if (bricks[i].fe > 0.2 && m::la::dot(bricks[i].toConcrete, bricks[i].toConcrete) > 0.0)
			{ broken[i] = 1; keepDir[i] = bricks[i].toConcrete; }
		}
		else if (m::la::dot(leanDir[i], leanDir[i]) > 0.0)
		{
			broken[i] = 1;
			keepDir[i] = Normalized(leanDir[i], bricks[i].t);
		}
	}
	std::vector<uint32_t> emit;
	for (size_t i = 0; i < n; i++)
		if (keep[i] && (all || bricks[i].fe > 0.0)) emit.push_back((uint32_t)i);
	std::vector<BreakRebarMesh> parts(emit.size());
	ProjectDamageParallelFor(0, (int64_t)emit.size(), [&](int64_t k)
	{
		const uint32_t i = emit[(size_t)k];
		EmitBrick(bricks[i], p, rg, broken[i] != 0, keepDir[i], parts[(size_t)k]);
	});
	for (const BreakRebarMesh& part : parts)
	{
		const Int32 vo = (Int32)out.points.size();
		const Int32 bo = out.bars;
		out.points.insert(out.points.end(), part.points.begin(), part.points.end());
		out.exposure.insert(out.exposure.end(), part.exposure.begin(), part.exposure.end());
		for (const ProjectDamagePoly& qq : part.polys)
			out.polys.push_back({ qq.a + vo, qq.b + vo, qq.c + vo, qq.d + vo });
		for (Int32 id : part.polyBar)
			out.polyBar.push_back(id + bo);
		out.polyPart.insert(out.polyPart.end(), part.polyPart.begin(), part.polyPart.end());
		out.bars += part.bars;
		                                               
		out.brickFrame.insert(out.brickFrame.end(), part.brickFrame.begin(), part.brickFrame.end());
		out.brickKey.insert(out.brickKey.end(), part.brickKey.begin(), part.brickKey.end());
		out.brickCut.insert(out.brickCut.end(), part.brickCut.begin(), part.brickCut.end());
	}
}

#include "break_volume_fill.h"

uint64_t SkeletonKey(const BreakRebarParams& p, double sc, const std::vector<EVec>& pts, size_t ntri, uint64_t seed)
{
	uint64_t h = Key(seed, (uint64_t)ntri, (uint64_t)pts.size(), 0x5CE1);
	auto mixD = [&](double d) { uint64_t b; std::memcpy(&b, &d, sizeof b); h = Mix(h ^ b); };
	for (const EVec& q : pts) { mixD(q.x); mixD(q.y); mixD(q.z); }
	mixD(sc); mixD(p.radius); mixD(p.tieRadius); mixD(p.cover); mixD(p.spacingU); mixD(p.tieSpacing);
	mixD(p.amount); mixD(p.ribHeight); mixD(p.upX); mixD(p.upY); mixD(p.upZ);
	h = Mix(h ^ (uint64_t)(uint32_t)p.material);
	h = Mix(h ^ (uint64_t)(uint32_t)p.axis ^ ((uint64_t)(uint32_t)p.type << 8) ^ ((uint64_t)p.core << 16)
		^ ((uint64_t)p.barsU << 17) ^ ((uint64_t)p.barsV << 18));
	return h;
}

}

// собираем профиль арматуры вдоль уже заданного пути
void BuildRebarSurfacePath(const BreakRebarParams& p, const std::vector<EVec>& points,
    const std::vector<EVec>& normals, bool freeStart, bool freeEnd, BreakRebarMesh& out)
{
    out=BreakRebarMesh();
    if(points.size()<2 || points.size()!=normals.size() || points.size()>100000)
        throw std::invalid_argument("Invalid rebar surface path");
    for(double value:{p.radius,p.ribHeight,p.ribPitch,p.twistPitch,p.detail,p.rebarResolution,p.wobble,p.neck})
        if(!std::isfinite(value))throw std::invalid_argument("Rebar settings must be finite");
    if(p.radius<=0.0 || p.type<0 || p.type>4 || p.ribPitch<.01 || p.twistPitch<.01)
        throw std::invalid_argument("Invalid rebar profile");
    BarPath bp;bp.radius=p.radius;bp.type=p.type;bp.key=Key(p.seed,0x53555246);
    for(size_t i=0;i<points.size();++i){
        for(int axis=0;axis<3;++axis)if(!std::isfinite(points[i][axis]) || !std::isfinite(normals[i][axis]))
            throw std::invalid_argument("Rebar path must be finite");
        if(!bp.pts.empty() && m::la::length(points[i]-bp.pts.back())<1e-8)continue;
        bp.pts.push_back(points[i]);bp.outs.push_back(Normalized(normals[i],EVec(0,0,1)));
    }
    if(bp.pts.size()<2)return;
    bp.out=bp.outs.front();Polyline line;line.Init(bp.pts);
    double step=.7*p.radius;
    if((p.type==BREAK_REBAR_RIBBED || p.type==BREAK_REBAR_HELICAL) && p.ribHeight>0.)step=std::min(step,p.ribPitch*2.*p.radius/5.);
    if(p.type==BREAK_REBAR_TWISTED || p.type==BREAK_REBAR_STRAND)step=std::min(step,p.twistPitch*2.*p.radius/16.);
    step=std::max(step/std::clamp(p.detail,.05,4.),.02*p.radius);
    if(line.Length()/step*std::max(p.sides,36)>1500000.)throw std::invalid_argument("Too many rebar polygons; reduce Resolution or increase Radius");
    BreakRebarParams settings=p;settings.bow=0.;settings.bend=0.;
    Piece piece;piece.s0=0.;piece.s1=line.Length();piece.end0=freeStart?1:0;piece.end1=freeEnd?1:0;piece.key=bp.key;
    piece.spans.push_back({0.,line.Length(),!freeStart,!freeEnd});
    Builder builder{settings,1.,out};builder.Emit(bp,line,piece);
}

static void BuildBreakRebarOne(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, const m::Manifold* left, const SurfaceDirectionField* guide)
{
	out = BreakRebarMesh();
	const size_t nStrokes = p.strokeCount.size();
	if (p.dropLoose && left != nullptr && left->NumTri() == 0) return;
	if (!p.enable || source.NumTri() == 0 || p.radius <= 0.0)
		return;
	if (cutters.empty() && nStrokes == 0 && !p.showAll)
		return;
	const double sc = std::clamp(p.scale, 0.01, 100.0);
	const uint64_t seed = Key((uint64_t)(uint32_t)mainSeed, (uint64_t)(uint32_t)p.seed, 0x4EBA);

	Region rg;
	rg.guide = guide;
	rg.solid.Build(source, p.material == BREAK_FILL_REBAR && p.layout == BREAK_REBAR_CENTRAL);
	if (!rg.solid.Valid())
		return;
	const EVec lo = rg.solid.Lo(), hi = rg.solid.Hi();
	EVec cLo(std::numeric_limits<double>::max()), cHi(std::numeric_limits<double>::lowest());
	for (const m::Manifold& c : cutters)
	{
		if (c.NumTri() == 0) continue;
		rg.cut.emplace_back();
		rg.cut.back().Build(c);
		if (!rg.cut.back().Valid()) { rg.cut.pop_back(); continue; }
		cLo = m::la::min(cLo, rg.cut.back().Lo());
		cHi = m::la::max(cHi, rg.cut.back().Hi());
	}
	if (left != nullptr && left->NumTri() != 0 && p.dropLoose)
	{
		rg.left.Build(*left, p.material == BREAK_FILL_REBAR && p.layout == BREAK_REBAR_CENTRAL);
		rg.hasLeft = rg.left.Valid();
	}
	                                                                             
	const int32_t mat = p.material == BREAK_FILL_BRICKS || p.material == BREAK_FILL_PLANKS ? p.material : (int32_t)BREAK_FILL_REBAR;
	double margin = 1.5 * p.embed * sc + 6.0 * p.radius * sc + p.cover * sc;
	if (mat == BREAK_FILL_BRICKS) margin += (p.brickLength + p.brickDepth) * sc;
	if (mat == BREAK_FILL_PLANKS) margin += (p.plankWidth + p.plankThickness) * sc;
	rg.winLo = m::la::max(cLo - EVec(margin), lo - EVec(1e-6));
	rg.winHi = m::la::min(cHi + EVec(margin), hi + EVec(1e-6));
	const bool anyCut = !rg.cut.empty()
		&& rg.winLo.x <= rg.winHi.x && rg.winLo.y <= rg.winHi.y && rg.winLo.z <= rg.winHi.z;

	                                       
	BreakRebarParams q = p;
	if (p.mode == BREAK_REBAR_BUILT_IN)
	{
		const double k = std::clamp(p.deform, 0.0, 1.0);
		q.wobble *= k; q.bow *= k; q.bend *= k; q.broken *= k; q.trim *= k;
	}

	                                                                           
	                                                                            
	BreakRebarParams lp = p;
	double layerHalf = 0.0;
	if (mat != BREAK_FILL_REBAR)
	{
		const double pitch = mat == BREAK_FILL_BRICKS ? p.brickHeight + std::max(0.0, p.mortar)
			: p.plankWidth + std::max(0.0, p.plankGap);
		layerHalf = 0.5 * (mat == BREAK_FILL_BRICKS ? p.brickDepth : p.plankThickness);
		const double rl = std::max(1e-3, std::min(layerHalf, 0.25 * pitch));
		lp.radius = rl; lp.tieRadius = rl;
		lp.cover = std::max(0.0, p.cover) + layerHalf - rl;
		lp.spacingU = lp.spacingV = lp.tieSpacing = std::max(0.05, pitch);
		lp.type = BREAK_REBAR_SMOOTH; lp.ribHeight = 0.0;
		lp.amount = 1.0; lp.jitter = 0.0; lp.core = false; lp.spiral = false;
		lp.barsU = lp.barsV = true; lp.outer = 0;
		if (mat == BREAK_FILL_BRICKS) lp.angle = 0.0;
		q.bow *= 0.35; q.bend *= 0.3; q.wobble *= 0.3;
		q.broken = std::max(q.broken, p.mode == BREAK_REBAR_BUILT_IN ? q.broken : 0.7);
	}

	std::vector<BarPath> paths;
	if (mat == BREAK_FILL_REBAR && p.layout == BREAK_REBAR_CENTRAL && (anyCut || p.showAll))
	{
        uint64_t key=Key(SkeletonKey(lp,sc,rg.solid.Points(),rg.solid.Tris().size(),seed),0xCE123);
        for(const auto& tri:rg.solid.Tris()) key=Key(key,tri.a,tri.b,tri.c);
        SkeletonCache& cache=TheSkeletonCache(key);
        std::lock_guard<std::mutex> guard(cache.lock);
        if(!cache.valid || cache.key!=key)
        {
            cache.paths.clear();
            CentralPaths(lp,sc,source,rg.solid,seed,cache.paths);
            cache.key=key;cache.valid=true;
        }
        paths=cache.paths;
	}
	else if (mat == BREAK_FILL_PLANKS && (anyCut || p.showAll))
	{
		PlankPaths(lp, sc, rg, lo, hi, seed, paths);
		q.wobble = 0.0; q.bend = 0.0;
	}
	else if (anyCut || p.showAll)
	{
		const bool cage = lp.layout == BREAK_REBAR_CAGE || p.layout == BREAK_REBAR_CAGE_ROUND;
		const bool shape = p.fit == BREAK_REBAR_FIT_SHAPE || (p.fit == BREAK_REBAR_FIT_AUTO && !BoxLike(source, lo, hi));
		if (!shape)
		{
			if (cage) CagePaths(lp, sc, lo, hi, seed, paths);
			else GridPaths(lp, sc, lo, hi, seed, paths);
		}
		else
		{
			SurfaceDistance sd;
			bool sdBuilt = false;
			auto distance = [&]() -> const SurfaceDistance&
			{
				if (!sdBuilt) { sd.Build(rg.solid.Points(), rg.solid.Tris()); sdBuilt = true; }
				return sd;
			};
			std::vector<std::pair<EVec, EVec>> through;
			const bool grid = p.mode == BREAK_REBAR_AROUND && !cage;
			if (grid && anyCut)
				SurfaceGridPaths(lp, sc, rg, distance(), seed, paths, through);
			if (!grid || !through.empty())
			{
				const uint64_t key = SkeletonKey(lp, sc, rg.solid.Points(), rg.solid.Tris().size(), seed);
				SkeletonCache& cache = TheSkeletonCache(key);
				std::lock_guard<std::mutex> guard(cache.lock);
				if (!cache.valid || cache.key != key)
				{
					cache.paths.clear();
					ShapeSkeletonPaths(lp, sc, source, rg.solid.Points(), distance(), lo, hi, seed, cache.paths);
					cache.key = key;
					cache.valid = true;
				}
				for (const BarPath& bp : cache.paths)
				{
					bool keep = !grid;
					for (size_t w = 0; w < through.size() && !keep; w++)
						for (const EVec& x : bp.pts)
							if (x.x >= through[w].first.x && x.y >= through[w].first.y && x.z >= through[w].first.z
								&& x.x <= through[w].second.x && x.y <= through[w].second.y && x.z <= through[w].second.z)
							{ keep = true; break; }
					if (keep) paths.push_back(bp);
				}
			}
		}
	}

	                                                                     
	if (mat != BREAK_FILL_REBAR && !paths.empty())
	{
		const EVec up = UpOf(p);
		double hz[2] = { 0.0, 0.0 }, tot[2] = { 0.0, 0.0 };
		for (const BarPath& bp : paths)
			for (size_t i = 1; i < bp.pts.size(); i++)
			{
				const EVec d = bp.pts[i] - bp.pts[i - 1];
				const double l = m::la::length(d);
				const int f = bp.family != 0 ? 1 : 0;
				hz[f] += l - std::fabs(m::la::dot(d, up));
				tot[f] += l;
			}
		const double h0 = tot[0] > 0.0 ? hz[0] / tot[0] : -1.0, h1 = tot[1] > 0.0 ? hz[1] / tot[1] : -1.0;
		const int course = h1 > h0 ? 1 : 0;
		int want = course;
		if (mat == BREAK_FILL_PLANKS && p.plankDir == 1 && tot[1 - course] > 0.0)
			want = 1 - course;
		std::vector<BarPath> kept;
		for (BarPath& bp : paths)
		{
			if ((bp.family != 0 ? 1 : 0) != want)
				continue;
			if (bp.deeper > 0.0)
			{
				Polyline pl;
				pl.Init(bp.pts);
				std::vector<EVec> lifted(bp.pts.size());
				for (size_t i = 0; i < bp.pts.size(); i++)
				{
					const EVec tan = pl.Tangent(pl.cum[i] - (i + 1 == bp.pts.size() ? 1e-9 : 0.0));
					lifted[i] = bp.pts[i] + OutOf(bp, bp.pts[i], tan) * bp.deeper;
				}
				bp.pts.swap(lifted);
				bp.deeper = 0.0;
			}
			kept.push_back(std::move(bp));
		}
		paths.swap(kept);
		                                                                    
		                                                                      
		                                             
		if (mat != BREAK_FILL_PLANKS)
		{
			SurfaceDistance sdm;
			sdm.Build(rg.solid.Points(), rg.solid.Tris());
			const double hstep = std::max(0.05, 0.5 * layerHalf * sc);
			const double cap = 4.0 * (p.cover + 2.0 * layerHalf) * sc + 4.0 * hstep;
			ProjectDamageParallelFor(0, (int64_t)paths.size(), [&](int64_t pi)
			{
				BarPath& bp = paths[(size_t)pi];
				std::vector<EVec> outs(bp.pts.size());
				Polyline pl;
				pl.Init(bp.pts);
				for (size_t i = 0; i < bp.pts.size(); i++)
				{
					const EVec x = bp.pts[i];
					EVec g(0.0);
					for (int k = 0; k < 3; k++)
					{
						EVec d(0.0);
						d[k] = hstep;
						g[k] = sdm.Distance(x + d, cap) - sdm.Distance(x - d, cap);
					}
					const EVec tan = pl.Tangent(pl.cum[i] - (i + 1 == bp.pts.size() ? 1e-9 : 0.0));
					const EVec fallback = OutOf(bp, x, tan);
					                                                                      
					EVec o = m::la::length(g) > 0.2 * hstep ? Normalized(-g, fallback) : fallback;
					if (!rg.solid.Inside(x)) o = fallback;
					outs[i] = o;
				}
				bp.outs.swap(outs);
			});
		}
		for (BarPath& bp : paths)
		{
			bp.radius = std::min(layerHalf * sc, bp.plankHalfLimit);
			if (mat == BREAK_FILL_PLANKS)
			{
				bp.type = kPlankType;
				bp.halfW = 0.5 * p.plankWidth * sc;
			}
		}
	}

	                  
	std::vector<BarPath> strokes;
	{
		size_t at = 0;
		const double rs = std::max(1e-3, p.radius * sc * std::max(0.05, p.strokeRadius));
		for (size_t i = 0; i < nStrokes; i++)
		{
			const size_t n = (size_t)std::max<int32_t>(0, p.strokeCount[i]);
			if ((at + n) * 3 > p.strokePts.size())
				break;
			BarPath bp;
			for (size_t k = 0; k < n; k++)
			{
				const EVec v(p.strokePts[(at + k) * 3], p.strokePts[(at + k) * 3 + 1], p.strokePts[(at + k) * 3 + 2]);
				if (bp.pts.empty() || m::la::length(v - bp.pts.back()) > 0.05 * rs)
					bp.pts.push_back(v);
			}
			at += n;
			if (bp.pts.size() < 2)
				continue;
			bp.radius = rs;
			bp.type = p.type;
			bp.out = AnyPerp(Normalized(bp.pts.back() - bp.pts.front(), EVec(0, 0, 1)));
			bp.key = Key(seed, 0x57E0, (uint64_t)i);
			bp.row = (int64_t)i;
			if (mat != BREAK_FILL_REBAR)
			{
				                                                    
				bp.out = StrokeOut(rg, bp);
				bp.radius = layerHalf * sc;
				if (mat == BREAK_FILL_PLANKS)
				{
					bp.type = kPlankType;
					bp.halfW = 0.5 * p.plankWidth * sc * std::max(0.05, p.strokeRadius);
				}
			}
			strokes.push_back(std::move(bp));
		}
	}
	BreakRebarParams qs = p;
	qs.bow = 0.0; qs.bend = 0.0; qs.broken = 0.0; qs.trim = 0.0;
	const double strokeEmbed = std::max(0.0, p.embed * sc * p.strokeEmbed);

	if (mat == BREAK_FILL_BRICKS)
	{
		std::vector<std::vector<Brick>> courses(paths.size() + strokes.size());
		ProjectDamageParallelFor(0, (int64_t)courses.size(), [&](int64_t i)
		{
			const size_t k = (size_t)i;
			LayCourse(k < paths.size() ? paths[k] : strokes[k - paths.size()], p, sc, rg, p.showAll, courses[k]);
		});
		std::vector<Brick> bricks;
		for (const std::vector<Brick>& c : courses)
			bricks.insert(bricks.end(), c.begin(), c.end());
		BuildBricks(bricks, p, rg, p.showAll, out);
		return;
	}

	                                                                                 
	const size_t np = paths.size();
	std::vector<BreakRebarMesh> parts(np + strokes.size());
	ProjectDamageParallelFor(0, (int64_t)parts.size(), [&](int64_t i)
	{
		const size_t k = (size_t)i;
		if (k < np)
		{
			Builder b{ q, sc, parts[k] };
			if (p.showAll) b.ProcessWhole(paths[k], rg);
			else b.Process(paths[k], rg);
		}
		else
		{
			Builder b{ qs, sc, parts[k] };
			b.ProcessStroke(strokes[k - np], rg, strokeEmbed);
		}
	});
	for (const BreakRebarMesh& part : parts)
	{
		const Int32 vo = (Int32)out.points.size();
		const Int32 bo = out.bars;
		out.points.insert(out.points.end(), part.points.begin(), part.points.end());
		out.exposure.insert(out.exposure.end(), part.exposure.begin(), part.exposure.end());
		for (const ProjectDamagePoly& qq : part.polys)
			out.polys.push_back({ qq.a + vo, qq.b + vo, qq.c + vo, qq.d + vo });
		for (Int32 id : part.polyBar)
			out.polyBar.push_back(id + bo);
		out.polyPart.insert(out.polyPart.end(), part.polyPart.begin(), part.polyPart.end());
		out.bars += part.bars;
		                                               
		out.brickFrame.insert(out.brickFrame.end(), part.brickFrame.begin(), part.brickFrame.end());
		out.brickKey.insert(out.brickKey.end(), part.brickKey.begin(), part.brickKey.end());
		out.brickCut.insert(out.brickCut.end(), part.brickCut.begin(), part.brickCut.end());
	}
}

                                                                             
                                                                              
                                                                              
                                 
// собираем внутреннее заполнение для всех подходящих частей объекта
void BuildBreakRebar(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, const m::Manifold* left, const SurfaceDirectionField* guide)
{
    if (p.stableVolume && p.material == BREAK_FILL_BRICKS)
    {
        BuildStableVolumeFill(source, cutters, p, mainSeed, out, left);
        return;
    }
	const int32_t mask = p.axisMask & 7;
	int axes[3], n = 0;
	for (int a = 0; a < 3; a++)
		if (mask & (1 << a)) axes[n++] = a;
	if (n == 0)
	{
		BuildBreakRebarOne(source, cutters, p, mainSeed, out, left, guide);
		return;
	}
	BreakRebarParams q = p;
	q.axisMask = 0;
	q.axis = axes[0] + 1;
	                                                                     
	                                             
	if (n > 1 && p.material != BREAK_FILL_REBAR && p.material != BREAK_FILL_PLANKS && source.NumTri() != 0)
	{
		const m::Box box = source.BoundingBox();
		const EVec lo(box.min.x, box.min.y, box.min.z), hi(box.max.x, box.max.y, box.max.z);
		if (p.fit == BREAK_REBAR_FIT_SHAPE || (p.fit == BREAK_REBAR_FIT_AUTO && !BoxLike(source, lo, hi)))
			n = 1;
	}
	BuildBreakRebarOne(source, cutters, q, mainSeed, out, left, guide);
	for (int i = 1; i < n; i++)
	{
		q.axis = axes[i] + 1;
		q.seed = p.seed + 7919 * (axes[i] + 1);
		q.strokePts.clear();
		q.strokeCount.clear();
		BreakRebarMesh part;
		BuildBreakRebarOne(source, cutters, q, mainSeed, part, left, guide);
		if (part.polys.empty() && part.brickKey.empty())
			continue;
		const Int32 vo = (Int32)out.points.size();
		const Int32 bo = out.bars;
		const bool parts = !out.polyPart.empty() || !part.polyPart.empty();
		if (parts) out.polyPart.resize(out.polys.size(), 0);
		out.points.insert(out.points.end(), part.points.begin(), part.points.end());
		out.exposure.insert(out.exposure.end(), part.exposure.begin(), part.exposure.end());
		for (const ProjectDamagePoly& qq : part.polys)
			out.polys.push_back({ qq.a + vo, qq.b + vo, qq.c + vo, qq.d + vo });
		for (Int32 id : part.polyBar)
			out.polyBar.push_back(id + bo);
		if (parts)
		{
			out.polyPart.insert(out.polyPart.end(), part.polyPart.begin(), part.polyPart.end());
			out.polyPart.resize(out.polys.size(), 0);
		}
		out.bars += part.bars;
		out.brickFrame.insert(out.brickFrame.end(), part.brickFrame.begin(), part.brickFrame.end());
		out.brickKey.insert(out.brickKey.end(), part.brickKey.begin(), part.brickKey.end());
		out.brickCut.insert(out.brickCut.end(), part.brickCut.begin(), part.brickCut.end());
	}
}

PROJECT_DAMAGE_NS_MACRO_END
