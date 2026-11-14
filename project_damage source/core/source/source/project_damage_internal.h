// общие объявления: общие внутренние данные и помощники расчёта повреждений
// ProjectDamage - worn edge damage cut into mesh objects.
// Copyright (C) 2026 Cornelius Dammrich
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// This file is also conveyed by its author, the sole copyright holder,
// under a separate proprietary licence as part of the ProjectDamage plugin for
// Cinema 4D. That does not affect your rights under the GPL above.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#pragma once
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
#include <cstdlib>
#include <cstdio>
#include <chrono>

#include "project_damage_types.h"
#include "project_damage_host.h"
#include "manifold/manifold.h"

#include "project_damage_core.h"
#include "py_random.h"

#include <vector>
#include <map>
#include <string>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

namespace project_damage {
constexpr double FLAT_EPS    = 0.001;
constexpr double MARGIN      = 5.0;
constexpr double TILT_DEG    = 13.0;
constexpr int    DOME_SEG    = 9;
constexpr int    OUTLINE_N   = 22;
constexpr int    SURF_SEG    = 22;
constexpr double SPALL_ASPECT = 0.65;
constexpr double SPALL_MAX_RADIUS = 1.39;

inline double MaxD(double a, double b) { return a > b ? a : b; }
inline int    MaxI(int a, int b)       { return a > b ? a : b; }

inline Int32 PyRound(double x) { return (Int32)std::nearbyint(x); }

                                                                               
                                                                               
inline thread_local int gProfileOverride = -1;
struct CoreProfileScope {
    int previous;
    explicit CoreProfileScope(int mode) : previous(gProfileOverride) { gProfileOverride = mode; }
    ~CoreProfileScope() { gProfileOverride = previous; }
};

inline bool ProfileEnabled()
{
    static const bool on = [] {
        const char* value = std::getenv("PD_PROFILE");
        return value && value[0] && value[0] != '0';
    }();
    return gProfileOverride >= 0 ? gProfileOverride != 0 : on;
}

inline void ProfileStatus(const std::string& phase, int pct)
{
    if (!ProfileEnabled()) return;
    using clk = std::chrono::steady_clock;
    static thread_local clk::time_point start = clk::now(), last = start;
    static thread_local std::string lastPhase = "start";
    const auto now = clk::now();
    if (pct == 5 && phase.rfind("reading mesh", 0) == 0) {
        start = last = now; lastPhase = "start";
    }
    double mb = 0.0;
#if defined(__APPLE__) || defined(__linux__)
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    mb = ru.ru_maxrss / (1024.0 * 1024.0);
#else
    mb = ru.ru_maxrss / 1024.0;
#endif
#endif
    std::fprintf(stderr, "[pd-profile] %.2fs +%.2fs %.0f MB peak (done: %s) -> %s\n",
        std::chrono::duration<double>(now - start).count(),
        std::chrono::duration<double>(now - last).count(), mb,
        lastPhase.c_str(), phase.c_str());
    std::fflush(stderr);
    last = now; lastPhase = phase;
}

inline void HostStatus(const ProjectDamageHost& host, const std::string& phase, int pct)
{
    ProfileStatus(phase, pct);
	if (host.statusText) host.statusText(phase.c_str());
	if (host.statusBar) host.statusBar(pct);
}
inline void HostLog(const ProjectDamageHost& host, const std::string& msg)
{
	if (host.log) host.log(msg.c_str());
}

inline Int32 BuildWeldSlots(const m::MeshGL& mesh, std::vector<Int32>& slotOf)
{
	slotOf.clear();
	if (mesh.numProp < 5)
		return 0;
	const Int32 nv = (Int32)mesh.NumVert();
	std::vector<Int32> rep((size_t)nv);
	for (Int32 i = 0; i < nv; i++)
		rep[(size_t)i] = i;
	const size_t n = mesh.mergeFromVert.size() < mesh.mergeToVert.size()
		? mesh.mergeFromVert.size() : mesh.mergeToVert.size();
	for (size_t k = 0; k < n; k++)
	{
		const Int32 from = (Int32)mesh.mergeFromVert[k];
		const Int32 to = (Int32)mesh.mergeToVert[k];
		if (from >= 0 && from < nv && to >= 0 && to < nv)
			rep[(size_t)from] = to;
	}
	for (Int32 i = 0; i < nv; i++)
	{
		Int32 r = i;
		Int32 hops = 0;
		while (rep[(size_t)r] != r && hops++ < nv)
			r = rep[(size_t)r];
		if (rep[(size_t)r] != r)
			return 0;
		rep[(size_t)i] = r;
	}
	slotOf.assign((size_t)nv, NOTOK);
	Int32 next = 0;
	for (Int32 i = 0; i < nv; i++)
		if (rep[(size_t)i] == i)
			slotOf[(size_t)i] = next++;
	for (Int32 i = 0; i < nv; i++)
		if (rep[(size_t)i] != i)
			slotOf[(size_t)i] = slotOf[(size_t)rep[(size_t)i]];
	return next;
}

inline std::pair<Int32, Int32> EdgeKey(Int32 a, Int32 b)
{
	return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

struct ExposedEdge
{
	Int32 p0 = 0, p1 = 0;
	Bool  convex = false;
	std::vector<std::pair<Int32, Int32>> reprs;
};

inline bool EdgeAvoided(const ExposedEdge& e, const std::vector<bool>* avoid)
{
	if (avoid == nullptr || e.reprs.empty())
		return false;
	for (const std::pair<Int32, Int32>& r : e.reprs)
	{
		const size_t pi = (size_t)r.first;
		if (pi >= avoid->size() || !(*avoid)[pi])
			return false;
	}
	return true;
}

enum class DamageClass : Int32 { Edge = 0, Corner = 1, Surface = 2,
	Fracture1 = 3, Fracture2 = 4, Fracture3 = 5, FractureChip = 6,
	Buildup = 7, Break = 8, Coating = 9, Rust = 10,
    WallpaperFront = 11, WallpaperBack = 12, WallpaperWall = 13,
    BrickFront = 14, BrickBreak = 15, BrickSeam = 16, BrickBehind = 17, Bevel = 18, EdgeWear = 19, ScratchGroove = 20, ScratchScuff = 21, BrickRender = 22 };

struct Chain
{
	std::vector<Int32> points;
	std::vector<ExposedEdge*> edges;
	Float length = 0.0;
	bool blockedStart = false, blockedEnd = false;
};

inline m::vec3 RotAxis(const m::vec3& v, const m::vec3& axis, double ang)
{
	const double L = m::la::length(axis);
	const m::vec3 a = axis / (L != 0.0 ? L : 1.0);
	const double c = std::cos(ang), s = std::sin(ang);
	return v * c + m::la::cross(a, v) * s + a * (m::la::dot(a, v) * (1.0 - c));
}

inline void CutterSegs(Float detail, Float ratio, int& domeSeg, int& outlineN)
{
	const Float r = ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
	const Float d = detail > 1e6 ? 1e6 : (detail > 0.0 ? detail : 0.0);
	domeSeg = MaxI(4, PyRound(DOME_SEG * d * r));
	outlineN = MaxI(10, PyRound(OUTLINE_N * d * r));
}

inline std::map<std::pair<Int32, Int32>, ExposedEdge*> BuildEdgeMap(const std::vector<Chain>& chains)
{
	std::map<std::pair<Int32, Int32>, ExposedEdge*> emap;
	for (const Chain& ch : chains)
		for (ExposedEdge* e : ch.edges)
			emap[EdgeKey(e->p0, e->p1)] = e;
	return emap;
}

m::SimplePolygon MakeSpallOutline(double su, double sv, PyRandom& rng, int n);
struct SpallSpec
{
	m::SimplePolygon outline;
	double su = 0.0, depth = 0.0, rimSoft = 0.0;
	int rings = 0;
	m::mat3x4 frame;
	uint64_t grain = 0;
	double offX = 0.0, offY = 0.0;
	double steep = 2.0, ripple = 0.0, ripplePh = 0.0, facet = 0.0;
	                                                          
	double profile = 0.0;
};
SpallSpec SpallPlan(const m::vec3& p0, const m::vec3& tangent, const m::vec3& nout,
	double su, double sv, double depth, PyRandom& rng, int domeSeg, int outlineN,
	double rimSoft, double tiltDeg = -1.0);
void SpallBuildAll(const std::vector<SpallSpec>& specs, const std::vector<bool>& keep,
	std::vector<m::Manifold>& out);

inline uint64_t GrainHash(uint64_t seed, uint64_t a, uint64_t b = 0)
{
	uint64_t z = seed + 0x9E3779B97F4A7C15ull * (a + 1) + 0xD1B54A32D192ED03ull * (b + 1);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}
inline double Grain01(uint64_t seed, uint64_t a, uint64_t b = 0)
{
	return (double)(GrainHash(seed, a, b) >> 11) * (1.0 / 9007199254740992.0);
}

struct BreakSurface
{
	std::vector<EVec>   pts;
	std::vector<Int32>  tris;
};
BreakSurface BuildBreakSurface(const BreakerSpec& s, Float ampScale);
                                                                             
Float BrValueNoise(Float x, Float z, Float scale, Int64 salt);
Float BrFbm(Float x, Float z, Float scale, Int64 salt, Int32 oct);
Float BrTurbulence(Float x, Float z, Float scale, Int64 salt, Int32 oct);
Float BrValueNoise3(const EVec& p, Float scale, Int64 salt);
Float BrFbm3(const EVec& p, Float scale, Int64 salt, Int32 oct);
Float BrTurbulence3(const EVec& p, Float scale, Int64 salt, Int32 oct);
                                                                            
                                                                               
                                                                              
                                                                       
Bool  BrClassicSurface(const BreakerSpec& s);
Float BrSurface3(const EVec& p, const BreakerSpec& s, Float L, Int64 sd);
Float BrSurface2(Float x, Float z, const BreakerSpec& s, Float L, Int64 sd);
void BuildStripSheets(const BreakerSpec& s, Float ampScale, BreakSurface& top, BreakSurface& bot);
m::Manifold BuildBreakCutter(const BreakerSpec& s, Float ampScale,
	const EVec& bbMin, const EVec& bbMax);

m::Manifold BuildCustomCutter(const BreakerSpec& s, Float ampScale);
BreakSurface BuildCustomSurface(const BreakerSpec& s, Float ampScale);
m::Manifold BuildCustomLocal(const BreakerSpec& s, Float ampScale);
m::Manifold PlaceCustom(const m::Manifold& local, const EMat& rel);
Bool CustomIsSheet(const BreakerSpec& s);
m::Manifold MergeOverlappingShells(const m::Manifold& solid);

int DropFallenFragments(m::Manifold& result, const m::Manifold& source, double keepShare);
void PlaceFractureVolume(MeshView meshView, const EVec* pts,
	const std::vector<EVec>& normals, const std::vector<Chain>& chains,
	const ProjectDamageParams& params, const ProjectDamageHost& host,
	const std::vector<bool>* avoidPoly,
	const std::vector<std::vector<EVec>>* guideSplines,
	std::vector<m::Manifold>& crackCutters,
	std::vector<Int32>& crackLevels, std::vector<m::Manifold>& spallCutters,
	const std::vector<Int32>* guideLevels);
void PlaceFracture(MeshView meshView, const EVec* pts,
	const std::vector<EVec>& normals, const std::vector<Chain>& chains,
	const ProjectDamageParams& params, const ProjectDamageHost& host,
	const std::vector<bool>* avoidPoly,
	const std::vector<std::vector<EVec>>* guideSplines,
	std::vector<m::Manifold>& crackCutters,
	std::vector<Int32>& crackLevels, std::vector<m::Manifold>& spallCutters,
	const std::vector<Int32>* guideLevels = nullptr);
}

struct IncrementalDamageCache;
class ProjectDamageDamageCache
{
public:
    std::shared_ptr<IncrementalDamageCache> incremental;
    std::vector<Int32> incrementalBuilt, incrementalReused;
	UInt64 fp = 0;
	Bool valid = false;
    Bool effectsReady = false;
    std::unordered_map<Int32, Int32> effectOwners;
	std::vector<m::Manifold> cutters;
	std::vector<Int32> cutterClass;
	std::vector<Int32> cutterId;
	std::vector<m::Manifold> buildup;
	std::vector<Int32> buildId;
	std::vector<BreakerSpec> breakSpec;
	std::vector<m::Manifold> breakCutter;
	EVec breakBbMin = EVec(0.0);
	EVec breakBbMax = EVec(0.0);
	std::vector<m::Manifold> breakLocal;
	m::Manifold damaged;
	Bool hasDamaged = false;
	Int32 damagedBaseId = 0;
};

Bool DamageCacheTake(ProjectDamageDamageCache* c, UInt64 fp,
	std::vector<m::Manifold>& cutters, std::vector<Int32>& cutterClass,
	std::vector<Int32>& cutterId, std::vector<m::Manifold>& buildup,
	std::vector<Int32>& buildId);
void DamageCacheStore(ProjectDamageDamageCache* c, UInt64 fp,
	const std::vector<m::Manifold>& cutters, const std::vector<Int32>& cutterClass,
	const std::vector<Int32>& cutterId, const std::vector<m::Manifold>& buildup,
	const std::vector<Int32>& buildId);
void DamageCacheClear(ProjectDamageDamageCache* c);
Bool SameBreaker(const BreakerSpec& a, const BreakerSpec& b);
Bool SameCustomLocal(const BreakerSpec& a, const BreakerSpec& b);

PROJECT_DAMAGE_NS_MACRO_END
