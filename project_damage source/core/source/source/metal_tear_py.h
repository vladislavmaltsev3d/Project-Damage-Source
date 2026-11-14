// общие объявления: передаём рисунок разрыва металла в расчёт
// ProjectDamage metal tear — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
//
// Python binding of the Metal Tear block, included once by
// core_standalone/project_damage_core_py.cpp:
//   BindMetalTear(mod)                next to BindConcretePits(mod);
//   .def_prop_ro("metal_tear", ...)   on Params, next to "pits";
//   CheckMetalTear(prm.metalTear)     in BuildWornPy, next to the plaster checks.
#pragma once
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/vector.h>
#include <cmath>
#include <stdexcept>
#include "metal_tear.h"

inline void CheckMetalTear(const MetalTearParams& p)
{
	if (!p.enable) return;
	const double v[] = { p.width, p.taper, p.jagged, p.meander, p.cutDepth, p.dentDepth, p.dentWidth, p.lipCurl,
		p.blisters, p.blisterSize, p.detail };
	for (double x : v)
		if (!std::isfinite(x)) throw std::invalid_argument("metal_tear: every parameter must be finite");
	for (double x : p.strokePts)
		if (!std::isfinite(x)) throw std::invalid_argument("metal_tear: stroke points must be finite");
	if (!(p.width > 0.0 && p.dentWidth > 0.0 && p.detail > 0.0))
		throw std::invalid_argument("metal_tear: width, dent_width and detail must be > 0");
}

inline void BindMetalTear(nanobind::module_& mod)
{
	namespace nb = nanobind;
	using P = MetalTearParams;
	nb::class_<P>(mod, "MetalTearParams").def(nb::init<>())
		.def_rw("enable", &P::enable).def_rw("seed", &P::seed)
		.def_rw("width", &P::width).def_rw("taper", &P::taper).def_rw("jagged", &P::jagged)
		.def_rw("meander", &P::meander).def_rw("cut_depth", &P::cutDepth)
		.def_rw("dent_depth", &P::dentDepth).def_rw("dent_width", &P::dentWidth).def_rw("lip_curl", &P::lipCurl)
		.def_rw("blisters", &P::blisters).def_rw("blister_size", &P::blisterSize)
		.def_rw("detail", &P::detail).def_rw("max_tris", &P::maxTris)
		.def_ro("stroke_pts", &P::strokePts).def_ro("stroke_count", &P::strokeCount)
		.def("set_strokes", [](P& p, nb::ndarray<const double, nb::c_contig, nb::device::cpu> xyz,
				nb::ndarray<const int32_t, nb::c_contig, nb::device::cpu> counts) {
			size_t total = 0;
			for (size_t i = 0; i < counts.size(); ++i) total += (size_t)std::max(0, counts.data()[i]);
			if (xyz.size() != total * 3) throw std::invalid_argument("metal_tear.set_strokes: xyz must hold 3 values per counted point");
			p.strokePts.assign(xyz.data(), xyz.data() + xyz.size());
			p.strokeCount.assign(counts.data(), counts.data() + counts.size());
		}, nb::arg("xyz"), nb::arg("counts"), "strokes in source-local core units: flat xyz, point count per stroke");
	mod.attr("metal_tear") = true;
}
