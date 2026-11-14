// сохраняем подготовленные данные между расчётами
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

#include "manifold/manifold.h"

#include "project_damage_core.h"
#include "project_damage_internal.h"
#include "wallpaper.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <limits>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

using namespace project_damage;

static Bool SameShapeFields(const BreakerSpec& a, const BreakerSpec& b)
{
	return a.mode == b.mode && a.seed == b.seed
		&& a.resolution == b.resolution && a.strength == b.strength
		&& a.macroH == b.macroH && a.turbH == b.turbH && a.fineH == b.fineH
		&& a.noiseScale == b.noiseScale
		&& a.surfSmooth == b.surfSmooth && a.surfDetail == b.surfDetail && a.surfRough == b.surfRough
		&& a.surfCrease == b.surfCrease && a.surfWarp == b.surfWarp
		&& a.macroScale == b.macroScale && a.turbScale == b.turbScale && a.fineScale == b.fineScale
		&& a.surfAniso == b.surfAniso && a.surfAnisoAxis == b.surfAnisoAxis && a.surfTerrace == b.surfTerrace
		&& a.customHash == b.customHash && a.customSheet == b.customSheet;
}

Bool SameCustomLocal(const BreakerSpec& a, const BreakerSpec& b)
{
	return SameShapeFields(a, b);
}

Bool SameBreaker(const BreakerSpec& a, const BreakerSpec& b)
{
	return SameShapeFields(a, b)
		&& a.flip == b.flip
		&& a.length == b.length && a.width == b.width && a.radius == b.radius
		&& a.thickness == b.thickness
		&& a.rimAmp == b.rimAmp && a.rimScale == b.rimScale
		&& a.surfMode == b.surfMode && a.texPath == b.texPath
		&& a.texOffX == b.texOffX && a.texOffY == b.texOffY && a.texOffZ == b.texOffZ
		&& a.texScale == b.texScale && a.texStrength == b.texStrength && a.texEdge == b.texEdge
		&& a.texExit == b.texExit && a.texMacro == b.texMacro
		&& a.smoothSlope == b.smoothSlope && a.smoothWidth == b.smoothWidth
		&& SameEMat(a.rel, b.rel);
}

ProjectDamageDamageCache* DamageCacheAlloc()
{
	return new ProjectDamageDamageCache;
}

void DamageCacheFree(ProjectDamageDamageCache*& cache)
{
	delete cache;
	cache = nullptr;
}

Bool DamageCacheTake(ProjectDamageDamageCache* c, UInt64 fp,
	std::vector<m::Manifold>& cutters, std::vector<Int32>& cutterClass,
	std::vector<Int32>& cutterId, std::vector<m::Manifold>& buildup,
	std::vector<Int32>& buildId)
{
	if (!c->valid || !c->effectsReady || c->fp != fp)
		return false;
	cutters = c->cutters;
	cutterClass = c->cutterClass;
	cutterId = c->cutterId;
	buildup = c->buildup;
	buildId = c->buildId;
	return true;
}

void DamageCacheStore(ProjectDamageDamageCache* c, UInt64 fp,
	const std::vector<m::Manifold>& cutters, const std::vector<Int32>& cutterClass,
	const std::vector<Int32>& cutterId, const std::vector<m::Manifold>& buildup,
	const std::vector<Int32>& buildId)
{
	c->fp = fp;
	c->cutters = cutters;
	c->cutterClass = cutterClass;
	c->cutterId = cutterId;
	c->buildup = buildup;
	c->buildId = buildId;
	c->valid = true;
	c->damaged = m::Manifold();
	c->hasDamaged = false;
}

void DamageCacheClear(ProjectDamageDamageCache* c)
{
	c->valid = false;
    c->effectsReady = false; c->effectOwners.clear();
	c->cutters.clear();
	c->cutterClass.clear();
	c->cutterId.clear();
	c->buildup.clear();
	c->buildId.clear();
	c->breakSpec.clear();
	c->breakCutter.clear();
	c->breakLocal.clear();
	c->breakBbMin = EVec(0.0);
	c->breakBbMax = EVec(0.0);
	c->damaged = m::Manifold();
	c->hasDamaged = false;
}

namespace
{
constexpr Int32 kMaxCacheArray = 64 * 1024 * 1024;
constexpr uint32_t kCacheMagic = 0x43474445u;
constexpr uint32_t kCacheFormat = 3;                            

template <typename T>
void PutPod(std::vector<uint8_t>& buf, const T& v)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	buf.insert(buf.end(), p, p + sizeof(T));
}

template <typename T>
void PutArr(std::vector<uint8_t>& buf, const std::vector<T>& v)
{
	PutPod(buf, (int32_t)v.size());
	if (v.empty())
		return;
	const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
	buf.insert(buf.end(), p, p + v.size() * sizeof(T));
}

Bool PutIdx(std::vector<uint8_t>& buf, const std::vector<uint64_t>& v)
{
	std::vector<int32_t> narrow(v.size());
	for (size_t i = 0; i < v.size(); i++)
	{
		if (v[i] > (uint64_t)LIMIT<Int32>::MAX)
			return false;
		narrow[i] = (int32_t)v[i];
	}
	PutArr(buf, narrow);
	return true;
}

struct ByteReader
{
	const uint8_t* p;
	const uint8_t* end;
	Bool ok = true;

	template <typename T>
	T Pod()
	{
		T v = T();
		if (!ok || (size_t)(end - p) < sizeof(T))
		{
			ok = false;
			return v;
		}
		std::memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		return v;
	}

	template <typename T>
	void Arr(std::vector<T>& out)
	{
		out.clear();
		const int32_t n = Pod<int32_t>();
		if (!ok || n < 0 || n > kMaxCacheArray
			|| (size_t)(end - p) < (size_t)n * sizeof(T))
		{
			ok = false;
			return;
		}
		out.resize((size_t)n);
		if (n > 0)
		{
			std::memcpy(out.data(), p, (size_t)n * sizeof(T));
			p += (size_t)n * sizeof(T);
		}
	}

	void Idx(std::vector<uint64_t>& out)
	{
		std::vector<int32_t> narrow;
		Arr(narrow);
		if (!ok)
			return;
		out.resize(narrow.size());
		for (size_t i = 0; i < narrow.size(); i++)
		{
			if (narrow[i] < 0)
			{
				ok = false;
				return;
			}
			out[i] = (uint64_t)narrow[i];
		}
	}
};
}

std::vector<uint8_t> DamageCacheSerialize(const ProjectDamageDamageCache* cache)
{
	std::vector<uint8_t> buf;
	if (cache == nullptr || !cache->valid || !cache->hasDamaged)
		return buf;
	const m::MeshGL64 mg = cache->damaged.GetMeshGL64();
	PutPod(buf, kCacheMagic);
	PutPod(buf, kCacheFormat);
	PutPod(buf, (uint64_t)cache->fp);
	PutPod(buf, (int32_t)cache->damagedBaseId);
    const WallpaperFaceIds wp = WallpaperIds();
    PutPod(buf, wp.front); PutPod(buf, wp.back); PutPod(buf, wp.wall);
    PutPod(buf, wp.flapFront); PutPod(buf, wp.flapBack);
	PutArr(buf, cache->cutterId);
	PutArr(buf, cache->cutterClass);
	PutArr(buf, cache->buildId);
	PutPod(buf, (int32_t)mg.numProp);
	PutPod(buf, (double)mg.tolerance);
	PutArr(buf, mg.vertProperties);
	const Bool idxOk = PutIdx(buf, mg.triVerts)
		&& PutIdx(buf, mg.mergeFromVert)
		&& PutIdx(buf, mg.mergeToVert)
		&& PutIdx(buf, mg.runIndex);
	PutArr(buf, mg.runOriginalID);
	PutArr(buf, mg.runTransform);
	PutArr(buf, mg.runFlags);
	if (!idxOk || !PutIdx(buf, mg.faceID))
		buf.clear();
	return buf;
}

Bool DamageCacheDeserialize(ProjectDamageDamageCache* cache, const uint8_t* data, size_t len)
{
	if (cache != nullptr)
		DamageCacheClear(cache);
	if (cache == nullptr || data == nullptr || len == 0)
		return false;

	ByteReader r = { data, data + len };
	if (r.Pod<uint32_t>() != kCacheMagic || r.Pod<uint32_t>() != kCacheFormat)
		return false;
	const uint64_t fp = r.Pod<uint64_t>();
	int32_t baseId = r.Pod<int32_t>();
    const int32_t wpFront = r.Pod<int32_t>(), wpBack = r.Pod<int32_t>(), wpWall = r.Pod<int32_t>();
    const int32_t wpFlapFront = r.Pod<int32_t>(), wpFlapBack = r.Pod<int32_t>();
    const WallpaperFaceIds wpNow = WallpaperIds();
	std::vector<Int32> cutterId, cutterClass, buildId;
	r.Arr(cutterId);
	r.Arr(cutterClass);
	r.Arr(buildId);
	const int32_t numProp = r.Pod<int32_t>();
	const double tolerance = r.Pod<double>();
	m::MeshGL64 mg;
	r.Arr(mg.vertProperties);
	r.Idx(mg.triVerts);
	r.Idx(mg.mergeFromVert);
	r.Idx(mg.mergeToVert);
	r.Idx(mg.runIndex);
	r.Arr(mg.runOriginalID);
	r.Arr(mg.runTransform);
	r.Arr(mg.runFlags);
	r.Idx(mg.faceID);
	if (!r.ok)
		return false;

	mg.numProp = (uint64_t)numProp;
	mg.tolerance = tolerance;
	const size_t nVert = numProp > 0 ? mg.vertProperties.size() / (size_t)numProp : 0;
	if ((numProp != 3 && numProp != 5) || nVert == 0
		|| mg.vertProperties.size() % (size_t)numProp != 0
		|| mg.triVerts.empty() || mg.triVerts.size() % 3 != 0
		|| cutterId.size() != cutterClass.size()
		|| mg.mergeFromVert.size() != mg.mergeToVert.size()
		|| mg.runOriginalID.empty()
		|| (mg.runIndex.size() != mg.runOriginalID.size()
			&& mg.runIndex.size() != mg.runOriginalID.size() + 1)
		|| (!mg.faceID.empty() && mg.faceID.size() != mg.triVerts.size() / 3)
		|| (!mg.runTransform.empty() && mg.runTransform.size() != mg.runOriginalID.size() * 12))
		return false;
	for (uint64_t v : mg.triVerts)
		if ((size_t)v >= nVert)
			return false;
	uint64_t prevRun = 0;
	for (uint64_t run : mg.runIndex)
	{
		if (run % 3 != 0 || run < prevRun || run > mg.triVerts.size())
			return false;
		prevRun = run;
	}
	if (mg.runIndex.front() != 0
		|| (mg.runIndex.size() == mg.runOriginalID.size() + 1
			&& mg.runIndex.back() != mg.triVerts.size()))
		return false;

	{
		std::vector<Int32> distinct;
		distinct.reserve(cutterId.size() + buildId.size() + mg.runOriginalID.size() + 1);
		auto collect = [&](Int32 id) { if (id >= 0) distinct.push_back(id); };
		for (Int32 id : cutterId)
			collect(id);
		for (Int32 id : buildId)
			collect(id);
		for (uint32_t id : mg.runOriginalID)
			collect((Int32)id);
		collect(baseId);
		std::sort(distinct.begin(), distinct.end());
		distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
		if (!distinct.empty())
		{
			const uint32_t first = m::Manifold::ReserveIDs((uint32_t)distinct.size());
			auto remap = [&](Int32 id) -> Int32
			{
                if (id == wpFront) return wpNow.front;
                if (id == wpBack) return wpNow.back;
                if (id == wpWall) return wpNow.wall;
                if (id == wpFlapFront) return wpNow.flapFront;
                if (id == wpFlapBack) return wpNow.flapBack;
				if (id < 0)
					return id;
				const auto it = std::lower_bound(distinct.begin(), distinct.end(), id);
				return (Int32)(first + (uint32_t)(it - distinct.begin()));
			};
			for (Int32& id : cutterId)
				id = remap(id);
			for (Int32& id : buildId)
				id = remap(id);
			for (uint32_t& id : mg.runOriginalID)
				id = (uint32_t)remap((Int32)id);
			baseId = remap(baseId);
		}
	}

	const m::Manifold man(mg);
	if (man.Status() != m::Manifold::Error::NoError || man.NumTri() == 0)
		return false;

	cache->fp = fp;
	cache->cutterId = std::move(cutterId);
	cache->effectsReady = false; cache->effectOwners.clear();
	cache->cutterClass = std::move(cutterClass);
	cache->buildId = std::move(buildId);
	cache->damaged = man;
	cache->damagedBaseId = baseId;
	cache->hasDamaged = true;
	cache->valid = true;
	return true;
}

PROJECT_DAMAGE_NS_MACRO_END
