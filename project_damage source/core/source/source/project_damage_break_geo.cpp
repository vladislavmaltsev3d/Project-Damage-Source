// подготавливаем форму каттеров и геометрию срезов
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
// Modified 2026-10-16: custom breakers accept any mesh (open sheets, unwelded or inside-out solids, hull fallback).

#include "manifold/manifold.h"

#include "project_damage_core.h"
#include "project_damage_internal.h"
#include "obreaker.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <map>
#include <array>
#include <unordered_map>
#include <utility>

namespace m = manifold;

PROJECT_DAMAGE_NS_MACRO_BEGIN

namespace project_damage {
static Float BrHash01(Int64 ix, Int64 iz, Int64 salt)
{
	Int64 h = (ix * 374761393LL + iz * 668265263LL + salt * 2246822519LL) & 0xFFFFFFFFLL;
	h = ((h ^ (h >> 13)) * 1274126177LL) & 0xFFFFFFFFLL;
	h = h ^ (h >> 16);
	return (Float)h / 4294967296.0;
}
Float BrValueNoise(Float x, Float z, Float scale, Int64 salt)
{
	const Float xs = x / scale, zs = z / scale;
	const Int64 ix = (Int64)std::floor(xs), iz = (Int64)std::floor(zs);
	const Float fx = xs - (Float)ix, fz = zs - (Float)iz;
	const Float u = fx * fx * (3.0 - 2.0 * fx), w = fz * fz * (3.0 - 2.0 * fz);
	const Float n00 = BrHash01(ix, iz, salt),     n10 = BrHash01(ix + 1, iz, salt);
	const Float n01 = BrHash01(ix, iz + 1, salt), n11 = BrHash01(ix + 1, iz + 1, salt);
	return (n00 * (1 - u) + n10 * u) * (1 - w) + (n01 * (1 - u) + n11 * u) * w;
}
template <typename NoiseAt>
static Float BrOctaves(NoiseAt&& noise, Float scale, Int64 salt, Int32 oct, Bool turbulence)
{
	Float total = 0.0, amp = 1.0, norm = 0.0;
	for (Int32 o = 0; o < oct; o++)
	{
		const Float n = noise(scale / (Float)(1 << o), salt + o) - 0.5;
		total += (turbulence ? Abs(n) : n) * 2.0 * amp;
		norm += amp; amp *= 0.5;
	}
	return turbulence ? 1.0 - 2.0 * (total / norm) : total / norm;
}
Float BrFbm(Float x, Float z, Float scale, Int64 salt, Int32 oct)
{
	return BrOctaves([&](Float sc, Int64 sa) { return BrValueNoise(x, z, sc, sa); }, scale, salt, oct, false);
}
Float BrTurbulence(Float x, Float z, Float scale, Int64 salt, Int32 oct)
{
	return BrOctaves([&](Float sc, Int64 sa) { return BrValueNoise(x, z, sc, sa); }, scale, salt, oct, true);
}

static Float BrHash01v(Int64 ix, Int64 iy, Int64 iz, Int64 salt)
{
	Int64 h = (ix * 374761393LL + iy * 3266489917LL + iz * 668265263LL
			   + salt * 2246822519LL) & 0xFFFFFFFFLL;
	h = ((h ^ (h >> 13)) * 1274126177LL) & 0xFFFFFFFFLL;
	h = h ^ (h >> 16);
	return (Float)h / 4294967296.0;
}
Float BrValueNoise3(const EVec& p, Float scale, Int64 salt)
{
	const EVec s = p * (1.0 / scale);
	const Int64 ix = (Int64)std::floor(s.x), iy = (Int64)std::floor(s.y), iz = (Int64)std::floor(s.z);
	const Float fx = s.x - (Float)ix, fy = s.y - (Float)iy, fz = s.z - (Float)iz;
	const Float u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy), w = fz * fz * (3 - 2 * fz);
	Float c[2][2][2];
	for (Int32 a = 0; a < 2; a++) for (Int32 b = 0; b < 2; b++) for (Int32 d = 0; d < 2; d++)
		c[a][b][d] = BrHash01v(ix + a, iy + b, iz + d, salt);
	const Float x0 = c[0][0][0] * (1 - u) + c[1][0][0] * u, x1 = c[0][1][0] * (1 - u) + c[1][1][0] * u;
	const Float x2 = c[0][0][1] * (1 - u) + c[1][0][1] * u, x3 = c[0][1][1] * (1 - u) + c[1][1][1] * u;
	return (x0 * (1 - v) + x1 * v) * (1 - w) + (x2 * (1 - v) + x3 * v) * w;
}
Float BrFbm3(const EVec& p, Float scale, Int64 salt, Int32 oct)
{
	return BrOctaves([&](Float sc, Int64 sa) { return BrValueNoise3(p, sc, sa); }, scale, salt, oct, false);
}
Float BrTurbulence3(const EVec& p, Float scale, Int64 salt, Int32 oct)
{
	return BrOctaves([&](Float sc, Int64 sa) { return BrValueNoise3(p, sc, sa); }, scale, salt, oct, true);
}

Bool BrClassicSurface(const BreakerSpec& s)
{
	return s.surfSmooth == 0.0 && s.surfDetail == 4.0 && s.surfRough == 0.5 && s.surfCrease == 1.0
		&& s.surfWarp == 0.0 && s.macroScale == 1.0 && s.turbScale == 1.0 && s.fineScale == 1.0
		&& s.surfAniso == 0.0 && s.surfTerrace == 0.0;
}

                                                                           
                                                                            
                           
template <typename NoiseAt>
static void BrOctavesEx(NoiseAt&& noise, Float scale, Int64 salt, Float oct, Float gain,
	Float& ridged, Float& rounded)
{
	Float totalAbs = 0.0, totalSig = 0.0, amp = 1.0, norm = 0.0, sc = scale;
	const Int32 n = (Int32)std::ceil(oct - 1e-9);
	for (Int32 o = 0; o < n; o++)
	{
		const Float w = FMin(1.0, oct - (Float)o);
		const Float v = noise(sc, salt + o) - 0.5;
		totalAbs += Abs(v) * 2.0 * amp * w;
		totalSig += v * 2.0 * amp * w;
		norm += amp * w;
		amp *= gain;
		sc *= 0.5;
	}
	norm = FMax(norm, 1e-12);
	ridged = 1.0 - 2.0 * (totalAbs / norm);
	rounded = totalSig / norm;
}

template <typename NoiseAt>
static Float BrLayers(NoiseAt&& vn, const BreakerSpec& s, Float L, Int64 sd)
{
	const Float ns = s.noiseScale;
	const Float sm = Clamp01(s.surfSmooth);
	const Float gain = FMin(0.95, FMax(0.05, s.surfRough));
	const Float macro = ((vn(0.50 * L * ns * FMax(0.05, s.macroScale), sd + 50) - 0.5) * 2.0) * s.macroH;
	Float ridged, rounded;
	BrOctavesEx(vn, 0.18 * L * ns * FMax(0.05, s.turbScale), sd, FMax(1.0, 3.0 * (1.0 - 0.67 * sm)), gain, ridged, rounded);
	const Float crease = Clamp01(s.surfCrease) * (1.0 - sm);
	const Float turb = (crease * ridged + (1.0 - crease) * rounded) * s.turbH * (1.0 - 0.5 * sm);
	const Float detail = FMin(8.0, FMax(1.0, s.surfDetail));
	BrOctavesEx(vn, 0.08 * L * ns * FMax(0.05, s.fineScale), sd + 100, FMax(1.0, detail * (1.0 - 0.75 * sm)), gain, ridged, rounded);
	const Float rough = rounded * s.fineH * (1.0 - sm) * (1.0 - sm);
	const Float sum = macro + turb + rough;
	if (s.surfTerrace <= 0.0)
		return sum;
	// Plywood steps: flat shelves joined by short risers, keeping a little of
	// the torn surface on them.
	const Float span = s.macroH + s.turbH + s.fineH;
	if (span <= 0.0)
		return sum;
	const Float step = 0.8 * span / FMax(1.0, s.surfTerrace);
	const Float cell = std::floor(sum / step);
	const Float f = sum / step - cell;
	const Float t = FMin(1.0, FMax(0.0, (f - 0.88) / 0.12));
	return (cell + t * t * (3.0 - 2.0 * t)) * step * 0.85 + sum * 0.15;
}

// The noise coordinate for a stretched look: a low frequency along the axis
// draws the features out into fibres. Off, the point is returned untouched.
static Float BrAnisoShrink(const BreakerSpec& s)
{
	return 1.0 - 0.96 * Clamp01(s.surfAniso);
}

Float BrSurface3(const EVec& p, const BreakerSpec& s, Float L, Int64 sd)
{
	EVec q = p;
	if (s.surfWarp > 0.0)
	{
		const Float big = 0.50 * L * s.noiseScale * FMax(0.05, s.macroScale);
		const Float amt = s.surfWarp * 0.25 * big;
		q = p + EVec((BrValueNoise3(p, big, sd + 700) - 0.5) * 2.0, (BrValueNoise3(p, big, sd + 701) - 0.5) * 2.0,
			(BrValueNoise3(p, big, sd + 702) - 0.5) * 2.0) * amt;
	}
	if (s.surfAniso > 0.0)
	{
		const Float k = BrAnisoShrink(s);
		if (s.surfAnisoAxis == 1) q.y *= k;
		else if (s.surfAnisoAxis == 2) q.z *= k;
		else q.x *= k;
	}
	return BrLayers([&](Float sc, Int64 sa) { return BrValueNoise3(q, sc, sa); }, s, L, sd);
}

Float BrSurface2(Float x, Float z, const BreakerSpec& s, Float L, Int64 sd)
{
	Float qx = x, qz = z;
	if (s.surfWarp > 0.0)
	{
		const Float big = 0.50 * L * s.noiseScale * FMax(0.05, s.macroScale);
		const Float amt = s.surfWarp * 0.25 * big;
		qx = x + (BrValueNoise(x, z, big, sd + 700) - 0.5) * 2.0 * amt;
		qz = z + (BrValueNoise(x, z, big, sd + 701) - 0.5) * 2.0 * amt;
	}
	if (s.surfAniso > 0.0)
	{
		const Float k = BrAnisoShrink(s);
		if (s.surfAnisoAxis == 0) qx *= k;
		else qz *= k;
	}
	return BrLayers([&](Float sc, Int64 sa) { return BrValueNoise(qx, qz, sc, sa); }, s, L, sd);
}

static void PlaneGridDims(const BreakerSpec& s, Int32& nx, Int32& nz)
{
	Int32 n = (Int32)(128.0 * s.resolution);
	if (n < 32) n = 32;
	if (n > 512) n = 512;
	if (s.length >= s.width)
	{
		nx = n;
		nz = MaxI(1, PyRound(n * (double)(s.width / s.length)));
	}
	else
	{
		nz = n;
		nx = MaxI(1, PyRound(n * (double)(s.length / s.width)));
	}
}

BreakSurface BuildBreakSurface(const BreakerSpec& s, Float ampScale)
{
	BreakSurface bs;
	const Float ns = s.noiseScale;
	const Int64 sd = (Int64)s.seed;
	const Bool classic = BrClassicSurface(s);

	if (s.mode == BREAKER_MODE_SPHERE)
	{
		const Float L = 2.0 * s.radius;
		Int32 nSeg = (Int32)(128.0 * s.resolution);
		if (nSeg < 32) nSeg = 32;
		if (nSeg > 512) nSeg = 512;
		const Int32 rings = MaxI(2, nSeg / 2);
		const Int32 nMid = rings - 1;

		auto vecAt = [](Float theta, Float phi) -> EVec
		{
			return EVec(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
		};
		auto radiusAt = [&](const EVec& dir) -> Float
		{
			const EVec p = dir * s.radius;
			if (!classic)
				return s.radius * (1.0 + BrSurface3(p, s, L, sd) * ampScale);
			const Float turb  = BrTurbulence3(p, 0.18 * L * ns, sd, 3)    * s.turbH;
			const Float macro = ((BrValueNoise3(p, 0.50 * L * ns, sd + 50) - 0.5) * 2.0) * s.macroH;
			const Float rough = BrFbm3(p, 0.08 * L * ns, sd + 100, 4)     * s.fineH;
			return s.radius * (1.0 + (macro + turb + rough) * ampScale);
		};

		bs.pts.resize((size_t)(2 + nMid * nSeg));
		const EVec north = vecAt(0.0, 0.0);
		bs.pts[0] = north * radiusAt(north);
		for (Int32 r = 1; r <= nMid; r++)
		{
			const Float theta = m::kPi * (Float)r / (Float)rings;
			for (Int32 si = 0; si < nSeg; si++)
			{
				const Float phi = 2.0 * m::kPi * (Float)si / (Float)nSeg;
				const EVec dir = vecAt(theta, phi);
				bs.pts[(size_t)(1 + (r - 1) * nSeg + si)] = dir * radiusAt(dir);
			}
		}
		const Int32 southIdx = 1 + nMid * nSeg;
		const EVec south = vecAt(m::kPi, 0.0);
		bs.pts[(size_t)southIdx] = south * radiusAt(south);

		auto ring = [&](Int32 r, Int32 si) { return 1 + (r - 1) * nSeg + (si % nSeg); };

		const Int32 bandTris = nMid >= 2 ? (nMid - 1) * nSeg * 2 : 0;
		bs.tris.reserve((size_t)(2 * nSeg + bandTris) * 3);
		for (Int32 si = 0; si < nSeg; si++)
		{
			bs.tris.push_back(0);
			bs.tris.push_back(ring(1, si + 1));
			bs.tris.push_back(ring(1, si));
		}
		for (Int32 r = 1; r < nMid; r++)
		{
			for (Int32 si = 0; si < nSeg; si++)
			{
				const Int32 a = ring(r, si), b = ring(r, si + 1);
				const Int32 c = ring(r + 1, si), d = ring(r + 1, si + 1);
				bs.tris.push_back(a); bs.tris.push_back(b); bs.tris.push_back(d);
				bs.tris.push_back(a); bs.tris.push_back(d); bs.tris.push_back(c);
			}
		}
		for (Int32 si = 0; si < nSeg; si++)
		{
			bs.tris.push_back(southIdx);
			bs.tris.push_back(ring(nMid, si));
			bs.tris.push_back(ring(nMid, si + 1));
		}
		return bs;
	}

	const Float L = FMax(s.length, s.width);
	auto height = [&](Float x, Float z) -> Float
	{
		Float turb = 0.0, macro = 0.0, rough = 0.0;
		if (classic)
		{
			turb  = BrTurbulence(x, z, 0.18 * L * ns, sd, 3)      * s.turbH  * L;
			macro = ((BrValueNoise(x, z, 0.50 * L * ns, sd + 50) - 0.5) * 2.0) * s.macroH * L;
			rough = BrFbm(x, z, 0.08 * L * ns, sd + 100, 4)       * s.fineH * L;
		}
		else
			macro = BrSurface2(x, z, s, L, sd) * L;
		const Float distIn = FMin(s.length * 0.5 - Abs(x), s.width * 0.5 - Abs(z));
		const Float t = Clamp01(distIn / (0.15 * L));
		const Float wgt = 1.0 - t * t * (3.0 - 2.0 * t);
		const Float rim = ((BrValueNoise(x, z, 0.28 * L * s.rimScale, sd + 200) - 0.5) * 2.0)
						  * s.rimAmp * L * wgt;
		return (macro + turb + rough + rim) * ampScale;
	};

	Int32 nx, nz;
	PlaneGridDims(s, nx, nz);
	const Int32 n1x = nx + 1, n1z = nz + 1;
	auto vid = [n1z](Int32 i, Int32 j) { return i * n1z + j; };

	bs.pts.resize((size_t)n1x * (size_t)n1z);
	for (Int32 i = 0; i < n1x; i++)
	{
		const Float x = -s.length * 0.5 + s.length * (Float)i / (Float)nx;
		for (Int32 j = 0; j < n1z; j++)
		{
			const Float z = -s.width * 0.5 + s.width * (Float)j / (Float)nz;
			bs.pts[(size_t)vid(i, j)] = EVec(x, height(x, z), z);
		}
	}
	bs.tris.reserve((size_t)nx * (size_t)nz * 6);
	for (Int32 i = 0; i < nx; i++)
	{
		for (Int32 j = 0; j < nz; j++)
		{
			const Int32 a = vid(i, j), b = vid(i + 1, j), c = vid(i, j + 1), d = vid(i + 1, j + 1);
			bs.tris.push_back(a); bs.tris.push_back(b); bs.tris.push_back(c);
			bs.tris.push_back(b); bs.tris.push_back(d); bs.tris.push_back(c);
		}
	}
	return bs;
}

static m::Manifold LocalToManifold(const EMat& place,
	const std::vector<EVec>& verts, const std::vector<Int32>& tris)
{
	m::MeshGL mesh;
	mesh.numProp = 3;
	mesh.vertProperties.resize(verts.size() * 3);
	for (size_t i = 0; i < verts.size(); i++)
	{
		const EVec gp = MulPoint(place, verts[i]);
		mesh.vertProperties[i * 3 + 0] = (float)gp.x;
		mesh.vertProperties[i * 3 + 1] = (float)gp.y;
		mesh.vertProperties[i * 3 + 2] = (float)gp.z;
	}
	mesh.triVerts.resize(tris.size());
	for (size_t i = 0; i < tris.size(); i++)
		mesh.triVerts[i] = (uint32_t)tris[i];

	const m::Manifold result(mesh);
	return result.NumTri() == 0 ? m::Manifold() : result;
}

static m::Manifold CloseSheet(const BreakSurface& sheet, const BreakerSpec& s,
	Bool capAbove, const EVec& bbMin, const EVec& bbMax)
{
	Int32 nx, nz;
	PlaneGridDims(s, nx, nz);
	const Int32 n1z = nz + 1;
	auto vid = [n1z](Int32 i, Int32 j) { return i * n1z + j; };

	const Float L = FMax(s.length, s.width);
	const EMat inv = InverseAffine(s.rel);
	Float minLocalY = std::numeric_limits<Float>::max();
	Float maxLocalY = std::numeric_limits<Float>::lowest();
	for (int c = 0; c < 8; c++)
	{
		const EVec corner((c & 1) ? bbMax.x : bbMin.x,
							 (c & 2) ? bbMax.y : bbMin.y,
							 (c & 4) ? bbMax.z : bbMin.z);
		const Float ly = MulPoint(inv, corner).y;
		minLocalY = FMin(minLocalY, ly);
		maxLocalY = FMax(maxLocalY, ly);
	}
	Float sheetMinY = std::numeric_limits<Float>::max();
	Float sheetMaxY = std::numeric_limits<Float>::lowest();
	for (const EVec& p : sheet.pts)
	{
		sheetMinY = FMin(sheetMinY, p.y);
		sheetMaxY = FMax(sheetMaxY, p.y);
	}
	Float capY;
	if (!capAbove)
	{
		capY = minLocalY - 0.1 * L;
		if (capY > sheetMinY)
			capY = sheetMinY - 0.1 * L;
	}
	else
	{
		capY = maxLocalY + 0.1 * L;
		if (capY < sheetMaxY)
			capY = sheetMaxY + 0.1 * L;
	}

	const Int32 V = (Int32)sheet.pts.size();
	const Int32 idx00 = vid(0, 0), idxN0 = vid(nx, 0), idx0N = vid(0, nz), idxNN = vid(nx, nz);
	const Int32 cap00 = V + 0, capN0 = V + 1, cap0N = V + 2, capNN = V + 3;
	std::vector<EVec> verts = sheet.pts;
	verts.reserve((size_t)V + 4);
	verts.push_back(EVec(sheet.pts[(size_t)idx00].x, capY, sheet.pts[(size_t)idx00].z));
	verts.push_back(EVec(sheet.pts[(size_t)idxN0].x, capY, sheet.pts[(size_t)idxN0].z));
	verts.push_back(EVec(sheet.pts[(size_t)idx0N].x, capY, sheet.pts[(size_t)idx0N].z));
	verts.push_back(EVec(sheet.pts[(size_t)idxNN].x, capY, sheet.pts[(size_t)idxNN].z));

	std::vector<Int32> tris = sheet.tris;
	const Int32 skirtTris = 2 * (nx + nz) + 4;
	tris.reserve(tris.size() + (size_t)(2 + skirtTris) * 3);
	tris.push_back(cap00); tris.push_back(cap0N); tris.push_back(capN0);
	tris.push_back(capN0); tris.push_back(cap0N); tris.push_back(capNN);

	for (Int32 i = 0; i < nx; i++)
	{
		const Int32 a = vid(i, 0), b = vid(i + 1, 0);
		tris.push_back(cap00); tris.push_back(b); tris.push_back(a);
		const Int32 c = vid(i, nz), d = vid(i + 1, nz);
		tris.push_back(cap0N); tris.push_back(c); tris.push_back(d);
	}
	tris.push_back(cap00); tris.push_back(capN0); tris.push_back(vid(nx, 0));
	tris.push_back(cap0N); tris.push_back(vid(nx, nz)); tris.push_back(capNN);

	for (Int32 j = 0; j < nz; j++)
	{
		const Int32 a = vid(0, j), b = vid(0, j + 1);
		tris.push_back(cap00); tris.push_back(a); tris.push_back(b);
		const Int32 c = vid(nx, j), d = vid(nx, j + 1);
		tris.push_back(capN0); tris.push_back(d); tris.push_back(c);
	}
	tris.push_back(cap00); tris.push_back(vid(0, nz)); tris.push_back(cap0N);
	tris.push_back(capN0); tris.push_back(capNN); tris.push_back(vid(nx, nz));

	if (!capAbove)
	{
		for (size_t i = 0; i < tris.size(); i += 3)
			std::swap(tris[i + 1], tris[i + 2]);
	}
	return LocalToManifold(s.rel, verts, tris);
}

static Float CustomNoise(const EVec& p, const BreakerSpec& s, Float L,
	Float ampScale, Int64 salt)
{
	const Float ns = s.noiseScale;
	const Int64 sd = (Int64)s.seed + salt;
	if (!BrClassicSurface(s))
		return BrSurface3(p, s, L, sd) * L * ampScale;
	const Float turb  = BrTurbulence3(p, 0.18 * L * ns, sd, 3)                   * s.turbH  * L;
	const Float macro = ((BrValueNoise3(p, 0.50 * L * ns, sd + 50) - 0.5) * 2.0) * s.macroH * L;
	const Float rough = BrFbm3(p, 0.08 * L * ns, sd + 100, 4)                    * s.fineH  * L;
	return (macro + turb + rough) * ampScale;
}

static Float CustomRefineLength(const BreakerSpec& s, Float L)
{
	Int32 cells = (Int32)(50.0 * s.resolution);
	if (cells < 16)  cells = 16;
	if (cells > 256) cells = 256;
	return L / (Float)cells;
}

struct CustomMesh
{
	std::vector<EVec>  pts;
	std::vector<Int32> tris;
};

enum class CustomTopo { Closed, Sheet, Broken, Empty };

static CustomMesh WeldCustom(const CustomShape& c)
{
	CustomMesh out;
	const Float q = FMax(c.size, 1e-9) * 1e-6;
	std::map<std::array<Int64, 3>, Int32> slot;
	std::vector<Int32> remap(c.pts.size(), -1);
	for (size_t i = 0; i < c.pts.size(); i++)
	{
		const EVec& p = c.pts[i];
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
			continue;
		const std::array<Int64, 3> key = { (Int64)std::llround(p.x / q),
			(Int64)std::llround(p.y / q), (Int64)std::llround(p.z / q) };
		auto it = slot.find(key);
		if (it == slot.end())
		{
			it = slot.emplace(key, (Int32)out.pts.size()).first;
			out.pts.push_back(p);
		}
		remap[i] = it->second;
	}
	out.tris.reserve(c.tris.size());
	for (size_t t = 0; t + 2 < c.tris.size(); t += 3)
	{
		const Int32 i0 = c.tris[t], i1 = c.tris[t + 1], i2 = c.tris[t + 2];
		if (i0 < 0 || i1 < 0 || i2 < 0 || (size_t)i0 >= remap.size()
			|| (size_t)i1 >= remap.size() || (size_t)i2 >= remap.size())
			continue;
		const Int32 a = remap[(size_t)i0], b = remap[(size_t)i1], d = remap[(size_t)i2];
		if (a < 0 || b < 0 || d < 0 || a == b || b == d || a == d)
			continue;
		out.tris.push_back(a); out.tris.push_back(b); out.tris.push_back(d);
	}
	return out;
}

static UInt64 CustomEdgeKey(Int32 a, Int32 b)
{
	return a < b ? ((UInt64)(UInt32)a << 32) | (UInt32)b : ((UInt64)(UInt32)b << 32) | (UInt32)a;
}

static CustomTopo ClassifyCustom(const CustomMesh& cm, std::vector<std::pair<Int32, Int32>>* boundary)
{
	if (cm.tris.empty())
		return CustomTopo::Empty;
	std::unordered_map<UInt64, Int32> uses;
	uses.reserve(cm.tris.size());
	for (size_t t = 0; t < cm.tris.size(); t += 3)
		for (Int32 k = 0; k < 3; k++)
			uses[CustomEdgeKey(cm.tris[t + (size_t)k], cm.tris[t + (size_t)((k + 1) % 3)])]++;
	Bool open = false, broken = false;
	for (const auto& e : uses)
	{
		if (e.second == 1) open = true;
		else if (e.second != 2) broken = true;
	}
	if (broken)
		return CustomTopo::Broken;
	if (!open)
		return CustomTopo::Closed;
	if (boundary != nullptr)
	{
		boundary->clear();
		for (size_t t = 0; t < cm.tris.size(); t += 3)
			for (Int32 k = 0; k < 3; k++)
			{
				const Int32 a = cm.tris[t + (size_t)k], b = cm.tris[t + (size_t)((k + 1) % 3)];
				if (uses[CustomEdgeKey(a, b)] == 1)
					boundary->emplace_back(a, b);
			}
	}
	return CustomTopo::Sheet;
}

Bool CustomIsSheet(const BreakerSpec& s)
{
	if (!s.custom || s.custom->tris.empty() || s.custom->size <= 0.0)
		return false;
	return ClassifyCustom(WeldCustom(*s.custom), nullptr) == CustomTopo::Sheet;
}

static m::Manifold OrientedManifold(const EMat& place, const std::vector<EVec>& verts,
	std::vector<Int32> tris)
{
	m::Manifold solid = LocalToManifold(place, verts, tris);
	if (solid.NumTri() != 0 && solid.Volume() < 0.0)
	{
		for (size_t i = 0; i < tris.size(); i += 3)
			std::swap(tris[i + 1], tris[i + 2]);
		solid = LocalToManifold(place, verts, tris);
	}
	return solid;
}

static m::Manifold CustomSolid(const CustomShape& c)
{
	const EMat id(m::la::identity);
	m::Manifold solid = LocalToManifold(id, c.pts, c.tris);
	if (solid.NumTri() != 0 && solid.Volume() > 0.0)
		return solid;
	const CustomMesh cm = WeldCustom(c);
	if (ClassifyCustom(cm, nullptr) == CustomTopo::Closed)
	{
		solid = OrientedManifold(id, cm.pts, cm.tris);
		if (solid.NumTri() != 0)
			return solid;
	}
	if (cm.pts.size() < 4)
		return m::Manifold();
	return m::Manifold::Hull(cm.pts);
}

static m::Manifold WarpCustomSolid(m::Manifold solid, const BreakerSpec& s, Float ampScale)
{
	const Float L = s.custom->size;
	if (ampScale <= 0.0 || (s.macroH <= 0.0 && s.turbH <= 0.0 && s.fineH <= 0.0))
		return solid;
	solid = solid.RefineToLength((double)CustomRefineLength(s, L));
	return solid.Warp([&s, L, ampScale](m::vec3& v)
	{
		const EVec p = v;
		v.x += (double)CustomNoise(p, s, L, ampScale, 0);
		v.y += (double)CustomNoise(p, s, L, ampScale, 300);
		v.z += (double)CustomNoise(p, s, L, ampScale, 400);
	});
}

m::Manifold BuildCustomLocal(const BreakerSpec& s, Float ampScale)
{
	if (!s.custom || s.custom->tris.empty() || s.custom->size <= 0.0)
		return m::Manifold();
	const m::Manifold solid = CustomSolid(*s.custom);
	if (solid.NumTri() == 0)
		return m::Manifold();
	return WarpCustomSolid(solid, s, ampScale);
}

m::Manifold PlaceCustom(const m::Manifold& local, const EMat& rel)
{
	if (local.NumTri() == 0)
		return m::Manifold();
	return local.Transform(rel);
}

m::Manifold BuildCustomCutter(const BreakerSpec& s, Float ampScale)
{
	return PlaceCustom(BuildCustomLocal(s, ampScale), s.rel);
}

static void SubdivideToLength(CustomMesh& cm, Float h)
{
	constexpr size_t kMaxTris = 400000;
	for (Int32 pass = 0; pass < 8; pass++)
	{
		Float maxEdge = 0.0;
		for (size_t t = 0; t < cm.tris.size(); t += 3)
			for (Int32 k = 0; k < 3; k++)
				maxEdge = FMax(maxEdge, m::la::length(cm.pts[(size_t)cm.tris[t + (size_t)k]]
					- cm.pts[(size_t)cm.tris[t + (size_t)((k + 1) % 3)]]));
		if (maxEdge <= 1.5 * h || cm.tris.size() * 4 > kMaxTris * 3)
			return;
		std::unordered_map<UInt64, Int32> mid;
		mid.reserve(cm.tris.size() * 2);
		auto midOf = [&](Int32 a, Int32 b) -> Int32
		{
			const UInt64 key = CustomEdgeKey(a, b);
			auto it = mid.find(key);
			if (it != mid.end())
				return it->second;
			const Int32 idx = (Int32)cm.pts.size();
			cm.pts.push_back((cm.pts[(size_t)a] + cm.pts[(size_t)b]) * 0.5);
			mid.emplace(key, idx);
			return idx;
		};
		std::vector<Int32> next;
		next.reserve(cm.tris.size() * 4);
		for (size_t t = 0; t < cm.tris.size(); t += 3)
		{
			const Int32 a = cm.tris[t], b = cm.tris[t + 1], c = cm.tris[t + 2];
			const Int32 ab = midOf(a, b), bc = midOf(b, c), ca = midOf(c, a);
			const Int32 quad[12] = { a, ab, ca,  ab, b, bc,  ca, bc, c,  ab, bc, ca };
			next.insert(next.end(), quad, quad + 12);
		}
		cm.tris.swap(next);
	}
}

static CustomMesh WarpedSheet(const BreakerSpec& s, Float ampScale)
{
	CustomMesh cm = WeldCustom(*s.custom);
	const Float L = s.custom->size;
	const Bool noisy = ampScale > 0.0 && (s.macroH > 0.0 || s.turbH > 0.0 || s.fineH > 0.0);
	if (!noisy)
		return cm;
	EVec areaN(0.0);
	for (size_t t = 0; t < cm.tris.size(); t += 3)
		areaN += m::la::cross(cm.pts[(size_t)cm.tris[t + 1]] - cm.pts[(size_t)cm.tris[t]],
			cm.pts[(size_t)cm.tris[t + 2]] - cm.pts[(size_t)cm.tris[t]]);
	const Float nLen = m::la::length(areaN);
	if (nLen <= 0.0)
		return cm;
	const EVec N = areaN / nLen;
	SubdivideToLength(cm, CustomRefineLength(s, L));
	for (EVec& v : cm.pts)
		v += N * CustomNoise(v, s, L, ampScale, 0);
	return cm;
}

static m::Manifold BuildCustomSheetCutter(const BreakerSpec& s, Float ampScale,
	const EVec& bbMin, const EVec& bbMax)
{
	const Bool slab = s.customSheet == 1;
	CustomMesh cm = slab ? WeldCustom(*s.custom) : WarpedSheet(s, ampScale);
	std::vector<std::pair<Int32, Int32>> boundary;
	if (ClassifyCustom(cm, &boundary) != CustomTopo::Sheet)
		return m::Manifold();
	const Int32 V = (Int32)cm.pts.size();

	EVec areaN(0.0);
	std::vector<EVec> vn((size_t)V, EVec(0.0));
	for (size_t t = 0; t < cm.tris.size(); t += 3)
	{
		const EVec& a = cm.pts[(size_t)cm.tris[t]];
		const EVec& b = cm.pts[(size_t)cm.tris[t + 1]];
		const EVec& c = cm.pts[(size_t)cm.tris[t + 2]];
		const EVec n = m::la::cross(b - a, c - a);
		areaN += n;
		for (Int32 k = 0; k < 3; k++)
			vn[(size_t)cm.tris[t + (size_t)k]] += n;
	}
	const Float nLen = m::la::length(areaN);
	if (nLen <= 0.0)
		return m::Manifold();
	const EVec N = areaN / nLen;

	std::vector<EVec> verts = cm.pts;
	std::vector<Int32> tris;
	tris.reserve(cm.tris.size() * 2 + boundary.size() * 6);
	verts.resize((size_t)V * 2);

	if (slab)
	{
		const Float half = 0.5 * FMax(s.thickness, 1e-4);
		for (Int32 i = 0; i < V; i++)
		{
			const Float l = m::la::length(vn[(size_t)i]);
			const EVec n = l > 0.0 ? vn[(size_t)i] / l : N;
			verts[(size_t)i] = cm.pts[(size_t)i] + n * half;
			verts[(size_t)(V + i)] = cm.pts[(size_t)i] - n * half;
		}
		for (size_t t = 0; t < cm.tris.size(); t += 3)
		{
			tris.push_back(cm.tris[t]); tris.push_back(cm.tris[t + 1]); tris.push_back(cm.tris[t + 2]);
			tris.push_back(V + cm.tris[t]); tris.push_back(V + cm.tris[t + 2]); tris.push_back(V + cm.tris[t + 1]);
		}
		for (const auto& e : boundary)
		{
			const Int32 a = e.first, b = e.second;
			tris.push_back(b); tris.push_back(a); tris.push_back(V + a);
			tris.push_back(b); tris.push_back(V + a); tris.push_back(V + b);
		}
	}
	else
	{
		const EVec dir = s.flip ? -N : N;
		const EMat inv = InverseAffine(s.rel);
		Float far = std::numeric_limits<Float>::lowest();
		for (int c = 0; c < 8; c++)
		{
			const EVec corner((c & 1) ? bbMax.x : bbMin.x, (c & 2) ? bbMax.y : bbMin.y,
				(c & 4) ? bbMax.z : bbMin.z);
			far = FMax(far, m::la::dot(MulPoint(inv, corner), dir));
		}
		for (const EVec& p : cm.pts)
			far = FMax(far, m::la::dot(p, dir));
		far += 0.1 * FMax(s.custom->size, 1e-3);
		for (Int32 i = 0; i < V; i++)
			verts[(size_t)(V + i)] = cm.pts[(size_t)i] + dir * (far - m::la::dot(cm.pts[(size_t)i], dir));
		for (size_t t = 0; t < cm.tris.size(); t += 3)
		{
			tris.push_back(cm.tris[t]); tris.push_back(cm.tris[t + 2]); tris.push_back(cm.tris[t + 1]);
			tris.push_back(V + cm.tris[t]); tris.push_back(V + cm.tris[t + 1]); tris.push_back(V + cm.tris[t + 2]);
		}
		for (const auto& e : boundary)
		{
			const Int32 a = e.first, b = e.second;
			tris.push_back(a); tris.push_back(b); tris.push_back(V + b);
			tris.push_back(a); tris.push_back(V + b); tris.push_back(V + a);
		}
	}
	if (slab)
	{
		m::Manifold solid = OrientedManifold(EMat(m::la::identity), verts, tris);
		if (solid.NumTri() != 0)
			return PlaceCustom(WarpCustomSolid(solid, s, ampScale), s.rel);
	}
	else
	{
		m::Manifold solid = OrientedManifold(s.rel, verts, tris);
		if (solid.NumTri() != 0)
			return solid;
	}
	std::vector<EVec> world(verts.size());
	for (size_t i = 0; i < verts.size(); i++)
		world[i] = MulPoint(s.rel, verts[i]);
	return m::Manifold::Hull(world);
}

BreakSurface BuildCustomSurface(const BreakerSpec& s, Float ampScale)
{
	BreakSurface bs;
	if (CustomIsSheet(s))
	{
		const CustomMesh cm = WarpedSheet(s, ampScale);
		bs.pts = cm.pts;
		bs.tris = cm.tris;
		return bs;
	}
	BreakerSpec local = s;
	local.rel = EMat(m::la::identity);
	const m::Manifold solid = BuildCustomCutter(local, ampScale);
	if (solid.NumTri() == 0)
		return bs;
	const m::MeshGL mesh = solid.GetMeshGL();
	bs.pts.resize(mesh.NumVert());
	for (size_t i = 0; i < bs.pts.size(); i++)
		bs.pts[i] = EVec(mesh.vertProperties[i * mesh.numProp + 0],
			mesh.vertProperties[i * mesh.numProp + 1],
			mesh.vertProperties[i * mesh.numProp + 2]);
	bs.tris.assign(mesh.triVerts.begin(), mesh.triVerts.end());
	return bs;
}

// основной вход: собираем геометрию по переданным настройкам
void BuildStripSheets(const BreakerSpec& s, Float ampScale, BreakSurface& top, BreakSurface& bot)
{
	const Float half = 0.5 * s.thickness;
	top = BuildBreakSurface(s, ampScale);
	for (EVec& p : top.pts)
		p.y += half;
	BreakerSpec s2 = s;
	s2.seed = s.seed + 1000;
	bot = BuildBreakSurface(s2, ampScale);
	for (EVec& p : bot.pts)
		p.y -= half;
}

                                                                           
                                                                        
                                                                          
                                                                               
                                                                           
                                            
m::Manifold MergeOverlappingShells(const m::Manifold& solid)
{
	if (solid.Status() != m::Manifold::Error::NoError || solid.NumTri() == 0)
		return m::Manifold();
	std::vector<m::Manifold> parts = solid.Decompose();
	if (parts.size() < 2)
		return m::Manifold();
	std::vector<double> vol(parts.size());
	double total = 0.0;
	for (size_t i = 0; i < parts.size(); i++)
	{
		vol[i] = parts[i].Volume();
		total += std::fabs(vol[i]);
	}
	if (total <= 0.0)
		return m::Manifold();
	const double flat = 1e-7 * total;
	std::vector<m::Manifold> keep;
	double sum = 0.0;
	for (size_t i = 0; i < parts.size(); i++)
	{
		if (std::fabs(vol[i]) <= flat)
			continue;
		if (vol[i] < 0.0)
			return m::Manifold();
		keep.push_back(std::move(parts[i]));
		sum += vol[i];
	}
	const bool dropped = keep.size() != parts.size();
	if (keep.empty())
		return m::Manifold();
	if (keep.size() == 1)
		return dropped ? keep[0] : m::Manifold();
	m::Manifold merged = m::Manifold::BatchBoolean(keep, m::OpType::Add);
	if (merged.Status() != m::Manifold::Error::NoError || merged.NumTri() == 0)
		return m::Manifold();
	if (!dropped && sum - merged.Volume() <= 1e-6 * sum)
		return m::Manifold();
	return merged;
}

m::Manifold BuildBreakCutter(const BreakerSpec& s, Float ampScale,
	const EVec& bbMin, const EVec& bbMax)
{
	if (s.mode == BREAKER_MODE_CUSTOM)
		return CustomIsSheet(s) ? BuildCustomSheetCutter(s, ampScale, bbMin, bbMax)
			: BuildCustomCutter(s, ampScale);
	if (s.mode == BREAKER_MODE_SPHERE)
	{
		const BreakSurface sheet = BuildBreakSurface(s, ampScale);
		return LocalToManifold(s.rel, sheet.pts, sheet.tris);
	}
	if (s.mode == BREAKER_MODE_STRIP)
	{
		BreakSurface top, bot;
		BuildStripSheets(s, ampScale, top, bot);
		const m::Manifold a = CloseSheet(top, s, !s.flip, bbMin, bbMax);
		const m::Manifold b = CloseSheet(bot, s, s.flip, bbMin, bbMax);
		return s.flip ? (a ^ b) : (a + b);
	}
	const BreakSurface sheet = BuildBreakSurface(s, ampScale);
	return CloseSheet(sheet, s, !s.flip, bbMin, bbMax);
}
}

PROJECT_DAMAGE_NS_MACRO_END
