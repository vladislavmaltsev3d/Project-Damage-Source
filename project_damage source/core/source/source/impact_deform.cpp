// деформируем меш вокруг точки удара с учётом защищённых мест
// ProjectDamage impact deformation — GPL-3.0-or-later; see LICENSE.
//
// Port of the Deformable Mesh Component hit deformation (Debugging Delight
// Studios, UE). Per impact (centre, radius R, strength S):
//   groups      the points are bucketed once into a groupsX x Y x Z grid of
//               the original bounding box; a group whose box meets the
//               impact sphere contributes its points (GetGroupVerticesInSphere)
//   in sphere   with the CURRENT (already deformed) positions: d = |p - c| / R;
//               deformation = curve(d) (default (1 - d^2) * 0.8) plus three
//               1D Perlin layers of d - frequency 8 * clamp(1 - R / 100) *
//               clamp(1 - 1.2 d), x1 / x1.5 / x2, amplitudes clamp(1 - d^2, 0,
//               .5), clamp(1 - d^3, 0, .3), clamp(1 - d^4, 0, .2) x Total
//               Amplitude, all x Noise Factor (GetVerticesInSphere)
//   move        relative to the deformation origin: q' = q - q * (def * S *
//               MaxDeform); if |q'|^2 <= |q0|^2 (1 - MaxDeform) the point is
//               put at q0 (1 - MaxDeform) instead (q0 = its original
//               position) - the UE version adds FVector(1) there, an
//               axis-dependent 1-unit nudge, dropped here; a point whose
//               ORIGINAL position is in a protection box stays at its original
//               position, a move INTO a box is undone
//               (UpdateVertexPositionsAndColors); amount += def * S * MaxDeform.
// Additions over the UE version (all neutral at their defaults):
//   noise scale x the noise frequency
//   origin      an impact may carry its own origin (the add-on's Auto origin
//               puts one behind the surface under every impact)
//   protection  painted weights per point: 1 never moves, 0.5 moves half way
//   smoothness  afterwards the displacement field (and the amount) is
//               diffused along the mesh edges, coincident points welded, over
//               about Smoothness x half the mean impact radius; protected
//               points hold zero, so painted zones get a soft border
#include "impact_deform.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace {

                                                                                             
const unsigned char kPerm[256] = {
151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180};
double Grad1(int h, double x) { return (h & 1) ? -x : x; }
double Perlin1D(double x)
{
	const double f = std::floor(x);
	const int xi = (int)f & 255;
	const double t = x - f;
	const double u = t * t * t * (t * (t * 6 - 15) + 10);
	const double g0 = Grad1(kPerm[xi], t), g1 = Grad1(kPerm[(xi + 1) & 255], t - 1.0);
	return 2.0 * (g0 + u * (g1 - g0));
}
double Clamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }

                                                                            
                                                                           
                                                                              
                                                                             
template <class InBox, class WeightOf>
void SmoothDisplacement(double* xyz, const std::vector<double>& orig, int n, const ImpactDeformParams& p,
                        const int32_t* quads, size_t nquads, std::vector<char>& moved,
                        std::vector<float>* amount, const InBox& inBox, const WeightOf& weightOf)
{
	const double sm = Clamp(p.smoothness, 0.0, 1.0);
	double meanR = 0.0;
	for (const DeformImpact& im : p.impacts) meanR += std::max(1e-9, im.radius);
	meanR /= (double)p.impacts.size();
	const double reach = sm * 0.5 * meanR;
	if (reach <= 0.0) return;
	                                                               
	std::vector<char> region((size_t)n, 0);
	bool any = false;
	for (int i = 0; i < n; ++i)
	{
		const double* o = &orig[3 * (size_t)i];
		for (const DeformImpact& im : p.impacts)
		{
			const double r = std::max(1e-9, im.radius) + reach;
			const double dx = o[0] - im.x, dy = o[1] - im.y, dz = o[2] - im.z;
			if (dx * dx + dy * dy + dz * dz <= r * r) { region[(size_t)i] = 1; any = true; break; }
		}
	}
	if (!any) return;
	                                                         
	std::vector<int> nodeOf((size_t)n, -1);
	std::map<std::array<double, 3>, int> weld;
	std::vector<int> rep;                                            
	std::vector<char> fixed;
	auto node = [&](int i) {
		if (nodeOf[(size_t)i] >= 0) return nodeOf[(size_t)i];
		const double* o = &orig[3 * (size_t)i];
		const auto it = weld.emplace(std::array<double, 3>{ o[0], o[1], o[2] }, (int)rep.size());
		if (it.second) { rep.push_back(i); fixed.push_back(0); }
		const int k = it.first->second;
		nodeOf[(size_t)i] = k;
		if (!region[(size_t)i] || inBox(o) || weightOf(i) >= 1.0) fixed[(size_t)k] = 1;
		return k;
	};
	std::vector<std::pair<int, int>> edges;
	for (size_t f = 0; f < nquads; ++f)
	{
		const int32_t* q = quads + 4 * f;
		for (int e = 0; e < 4; ++e)
		{
			const int a = q[e], b = q[(e + 1) & 3];
			if (a == b || a < 0 || b < 0 || a >= n || b >= n) continue;
			if (!region[(size_t)a] && !region[(size_t)b]) continue;
			const int na = node(a), nb = node(b);
			if (na == nb) continue;
			edges.emplace_back(std::min(na, nb), std::max(na, nb));
		}
	}
	if (edges.empty()) return;
	std::sort(edges.begin(), edges.end());
	edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
	const size_t nn = rep.size();
	std::vector<int> start(nn + 1, 0), adj(edges.size() * 2);
	double hSum = 0.0;
	for (const auto& e : edges)
	{
		++start[(size_t)e.first + 1]; ++start[(size_t)e.second + 1];
		const double* a = &orig[3 * (size_t)rep[(size_t)e.first]];
		const double* b = &orig[3 * (size_t)rep[(size_t)e.second]];
		hSum += std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
	}
	for (size_t k = 0; k < nn; ++k) start[k + 1] += start[k];
	{
		std::vector<int> fill(start.begin(), start.end() - 1);
		for (const auto& e : edges) { adj[(size_t)fill[(size_t)e.first]++] = e.second; adj[(size_t)fill[(size_t)e.second]++] = e.first; }
	}
	const double h = std::max(1e-12, hSum / (double)edges.size());
	                                                                       
	const int steps = (int)Clamp(std::round(2.0 * (reach / h) * (reach / h)), 1.0, 1000.0);
	                                                                              
	std::vector<double> v(nn * 4, 0.0), cnt(nn, 0.0);
	for (int i = 0; i < n; ++i)
	{
		const int k = nodeOf[(size_t)i];
		if (k < 0) continue;
		for (int a = 0; a < 3; ++a) v[4 * (size_t)k + a] += xyz[3 * (size_t)i + a] - orig[3 * (size_t)i + a];
		v[4 * (size_t)k + 3] += amount ? (double)(*amount)[(size_t)i] : 0.0;
		cnt[(size_t)k] += 1.0;
	}
	for (size_t k = 0; k < nn; ++k)
		for (int a = 0; a < 4; ++a) v[4 * k + a] = fixed[k] && region[(size_t)rep[k]] ? 0.0 : v[4 * k + a] / std::max(1.0, cnt[k]);
	std::vector<double> w(v);
	for (int s = 0; s < steps; ++s)
	{
		for (size_t k = 0; k < nn; ++k)
		{
			if (fixed[k]) continue;
			const int b = start[k], e = start[k + 1];
			if (b == e) continue;
			double avg[4] = { 0, 0, 0, 0 };
			for (int j = b; j < e; ++j)
				for (int a = 0; a < 4; ++a) avg[a] += v[4 * (size_t)adj[(size_t)j] + a];
			for (int a = 0; a < 4; ++a) w[4 * k + a] = 0.5 * v[4 * k + a] + 0.5 * avg[a] / (double)(e - b);
		}
		v.swap(w);
	}
	for (int i = 0; i < n; ++i)
	{
		const int k = nodeOf[(size_t)i];
		if (k < 0 || fixed[(size_t)k]) continue;
		const double* o = &orig[3 * (size_t)i];
		const double np_[3] = { o[0] + v[4 * (size_t)k], o[1] + v[4 * (size_t)k + 1], o[2] + v[4 * (size_t)k + 2] };
		if (inBox(np_)) continue;                                          
		double* cur = &xyz[3 * (size_t)i];
		cur[0] = np_[0]; cur[1] = np_[1]; cur[2] = np_[2];
		const double d2 = v[4 * (size_t)k] * v[4 * (size_t)k] + v[4 * (size_t)k + 1] * v[4 * (size_t)k + 1] + v[4 * (size_t)k + 2] * v[4 * (size_t)k + 2];
		moved[(size_t)i] = d2 > 0.0 ? 1 : 0;
		if (amount) (*amount)[(size_t)i] = (float)v[4 * (size_t)k + 3];
	}
}

}               

int ApplyImpactDeform(double* xyz, int n, const ImpactDeformParams& p, std::vector<float>* amount,
                      const int32_t* quads, size_t nquads)
{
	if (!p.enable || n <= 0 || p.impacts.empty()) return 0;
	if (amount) amount->assign((size_t)n, 0.0f);
	const std::vector<double> orig(xyz, xyz + 3 * (size_t)n);
	double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
	for (int i = 0; i < n; ++i)
		for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], orig[3 * i + k]); hi[k] = std::max(hi[k], orig[3 * i + k]); }
	const int G[3] = { std::max(1, std::min(128, p.groupsX)), std::max(1, std::min(128, p.groupsY)), std::max(1, std::min(128, p.groupsZ)) };
	double cs[3];
	for (int k = 0; k < 3; ++k) cs[k] = std::max(1e-9, (hi[k] - lo[k]) / G[k]);
	std::vector<std::vector<int>> groups((size_t)G[0] * G[1] * G[2]);
	auto cellOf = [&](const double* q, int k) { return std::max(0, std::min(G[k] - 1, (int)std::floor((q[k] - lo[k]) / cs[k]))); };
	for (int i = 0; i < n; ++i)
	{
		const double* q = &orig[3 * (size_t)i];
		groups[((size_t)cellOf(q, 0) * G[1] + cellOf(q, 1)) * G[2] + cellOf(q, 2)].push_back(i);
	}
	const double Oshared[3] = { p.originX, p.originY, p.originZ };
	const bool painted = p.protectWeights.size() == (size_t)n;
	auto weightOf = [&](int i) { return painted ? Clamp((double)p.protectWeights[(size_t)i], 0.0, 1.0) : 0.0; };
	const double noiseScale = std::max(0.0, p.noiseScale);
	const double P = Clamp(p.maxDeform, 0.0, 1.0);
	auto inBox = [&](const double* q) {
		for (const DeformBox& b : p.protection)
			if (q[0] >= b.minX && q[0] <= b.maxX && q[1] >= b.minY && q[1] <= b.maxY && q[2] >= b.minZ && q[2] <= b.maxZ) return true;
		return false;
	};
	auto curveAt = [&](double d) {
		if (p.curve.size() < 2) return (1.0 - d * d) * 0.8;
		const double x = Clamp(d, 0.0, 1.0) * (p.curve.size() - 1);
		const size_t i = std::min(p.curve.size() - 2, (size_t)x);
		const double t = x - i;
		return p.curve[i] * (1 - t) + p.curve[i + 1] * t;
	};
	std::vector<char> moved((size_t)n, 0);
	for (const DeformImpact& im : p.impacts)
	{
		const double R = std::max(1e-9, im.radius);
		const double c[3] = { im.x, im.y, im.z };
		const double S = im.strength * p.strength * P;                        
		const double O[3] = { im.ownOrigin ? im.ox : Oshared[0], im.ownOrigin ? im.oy : Oshared[1], im.ownOrigin ? im.oz : Oshared[2] };
		                                                                 
		std::vector<int> cand;
		for (int gx = 0; gx < G[0]; ++gx)
			for (int gy = 0; gy < G[1]; ++gy)
				for (int gz = 0; gz < G[2]; ++gz)
				{
					const int g[3] = { gx, gy, gz };
					double d2 = 0.0;
					for (int k = 0; k < 3; ++k)
					{
						const double a = lo[k] + g[k] * cs[k], b = a + cs[k];
						const double e = c[k] < a ? a - c[k] : (c[k] > b ? c[k] - b : 0.0);
						d2 += e * e;
					}
					if (d2 <= R * R)
					{
						const auto& gr = groups[((size_t)gx * G[1] + gy) * G[2] + gz];
						cand.insert(cand.end(), gr.begin(), gr.end());
					}
				}
		const double freqR = 8.0 * Clamp(1.0 - R / 100.0, 0.2, 1.0) * noiseScale;
		for (int i : cand)
		{
			double* cur = &xyz[3 * (size_t)i];
			const double dx = cur[0] - c[0], dy = cur[1] - c[1], dz = cur[2] - c[2];
			const double dist2 = dx * dx + dy * dy + dz * dz;
			if (dist2 > R * R) continue;
			const double d = std::sqrt(dist2) / R;
			double def = curveAt(d);
			const double fr = freqR * Clamp(1.0 - d * 1.2, 0.1, 1.0);
			const double a1 = Clamp(1.0 - std::pow(d, 2.0), 0.0, 0.5) * p.totalAmplitude;
			const double a2 = Clamp(1.0 - std::pow(d, 3.0), 0.0, 0.3) * p.totalAmplitude;
			const double a3 = Clamp(1.0 - std::pow(d, 4.0), 0.0, 0.2) * p.totalAmplitude;
			def += (Perlin1D(d * fr) * a1 + Perlin1D(d * fr * 1.5) * a2 + Perlin1D(d * fr * 2.0) * a3) * p.noiseFactor;
			const double* o = &orig[3 * (size_t)i];
			if (inBox(o)) { cur[0] = o[0]; cur[1] = o[1]; cur[2] = o[2]; continue; }
			const double free_ = 1.0 - weightOf(i);
			if (free_ <= 0.0) continue;                             
			                                     
			const double q[3] = { cur[0] - O[0], cur[1] - O[1], cur[2] - O[2] };
			const double q0[3] = { o[0] - O[0], o[1] - O[1], o[2] - O[2] };
			const double k = def * S;
			double nq[3] = { q[0] - q[0] * k, q[1] - q[1] * k, q[2] - q[2] * k };
			const double newL2 = nq[0] * nq[0] + nq[1] * nq[1] + nq[2] * nq[2];
			const double maxL2 = (q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2]) * (1.0 - P);
			if (!(newL2 > maxL2))
				for (int a = 0; a < 3; ++a) nq[a] = q0[a] * (1.0 - P);
			double np_[3] = { nq[0] + O[0], nq[1] + O[1], nq[2] + O[2] };
			if (free_ < 1.0)                                                 
				for (int a = 0; a < 3; ++a) np_[a] = cur[a] + (np_[a] - cur[a]) * free_;
			if (inBox(np_)) continue;                                       
			cur[0] = np_[0]; cur[1] = np_[1]; cur[2] = np_[2];
			moved[(size_t)i] = 1;
			if (amount) (*amount)[(size_t)i] += (float)(k * free_);
		}
	}
	if (p.smoothness > 0.0 && quads != nullptr && nquads > 0)
		SmoothDisplacement(xyz, orig, n, p, quads, nquads, moved, amount, inBox, weightOf);
	int count = 0;
	for (char m : moved) count += m;
	return count;
}
