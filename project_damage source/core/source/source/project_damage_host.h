// общие объявления: общие типы и операции, нужные расчёту вне Blender
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

#include "project_damage_types.h"

#include <cstdint>
#include <functional>
#include <tbb/parallel_for.h>

struct ProjectDamageHost
{
	std::function<void(const char*)> statusText;
	std::function<void(int)>         statusBar;
	std::function<void(const char*)> log;
};

struct IFieldMask
{
	virtual ~IFieldMask() = default;
	IFieldMask() = default;
	IFieldMask(const IFieldMask&) = default;
	IFieldMask& operator=(const IFieldMask&) = default;
	IFieldMask(IFieldMask&&) = default;
	IFieldMask& operator=(IFieldMask&&) = default;

	virtual bool   Active() const = 0;
	virtual double Weight(const EVec& p) const = 0;
	virtual double Amount() const = 0;
	virtual double Size() const = 0;

	bool KeepW(double w, int32_t index, int32_t stream) const
	{
		if (!Active() || Amount() <= 0.0)
			return true;
		const double thr = 1.0 - Amount() * (1.0 - w);
		return MaskHashUnit(index, stream) <= thr;
	}
	double SizeScaleW(double w) const
	{
		if (!Active() || Size() <= 0.0)
			return 1.0;
		return 1.0 - Size() * (1.0 - w);
	}
	double WeightIfUsed(const EVec& p) const
	{
		return (Active() && (Amount() > 0.0 || Size() > 0.0)) ? Weight(p) : 1.0;
	}
	bool Keep(const EVec& p, int32_t index, int32_t stream) const
	{
		if (!Active() || Amount() <= 0.0)
			return true;
		return KeepW(Weight(p), index, stream);
	}
	double SizeScale(const EVec& p) const
	{
		if (!Active() || Size() <= 0.0)
			return 1.0;
		return SizeScaleW(Weight(p));
	}
	double FadeScale(const EVec& p) const
	{
		if (!Active())
			return 1.0;
		const double s = Amount() > Size() ? Amount() : Size();
		if (s <= 0.0)
			return 1.0;
		return 1.0 - s * (1.0 - Weight(p));
	}

	static double MaskHashUnit(int32_t index, int32_t stream)
	{
		uint64_t h = 1469598103934665603ULL;
		const uint32_t words[2] = { (uint32_t)index, (uint32_t)stream };
		for (int w = 0; w < 2; ++w)
		{
			for (int i = 0; i < 4; ++i)
			{
				h ^= (uint64_t)((words[w] >> (i * 8)) & 0xFF);
				h *= 1099511628211ULL;
			}
		}
		return (double)(h >> 11) / (double)(1ULL << 53);
	}
};

template <typename F>
inline void ProjectDamageParallelFor(int64_t from, int64_t to, F&& fn)
{
	tbb::parallel_for(from, to, [&](int64_t i) { fn(i); });
}
