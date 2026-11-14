// общие объявления: обмен настройками и геометрией эффекта глины с Python
// ProjectDamage clay — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
//
// Python binding of the Clay block, included once by
// core_standalone/project_damage_core_py.cpp:
//   BindClay(mod)              next to BindConcretePits(mod);
//   .def_prop_ro("clay", ...)  on Params, next to "pits";
//   CheckClay(prm.clay)        in BuildWornPy, next to the plaster checks.
#pragma once
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/vector.h>
#include <cmath>
#include <stdexcept>
#include "clay.h"

inline void CheckClay(const ClayParams& p)
{
	if (!p.enable) return;
	const double v[] = { p.refSize, p.resolution, p.softness, p.lumps, p.lumpScale, p.sag, p.bulge, p.gravX, p.gravY,
		p.gravZ, p.presses, p.pressSize, p.pressDepth, p.pressRim, p.fingerprints, p.smears, p.smearLength,
		p.grain, p.grainScale, p.cracks, p.crackWidth, p.crackDepth, p.thicknessCap };
	for (double x : v)
		if (!std::isfinite(x)) throw std::invalid_argument("clay: every parameter must be finite");
	if (!(p.resolution > 0.0)) throw std::invalid_argument("clay_resolution must be > 0");
}

inline void BindClay(nanobind::module_& mod)
{
	namespace nb = nanobind;
	using P = ClayParams;
	nb::class_<P>(mod, "ClayParams").def(nb::init<>())
		.def_rw("enable", &P::enable).def_rw("seed", &P::seed).def_rw("avoid", &P::avoid)
		.def_rw("ref_size", &P::refSize).def_rw("resolution", &P::resolution).def_rw("max_tris", &P::maxTris)
		.def_rw("softness", &P::softness).def_rw("lumps", &P::lumps).def_rw("lump_scale", &P::lumpScale)
		.def_rw("sag", &P::sag).def_rw("bulge", &P::bulge)
		.def_rw("grav_x", &P::gravX).def_rw("grav_y", &P::gravY).def_rw("grav_z", &P::gravZ)
		.def_rw("presses", &P::presses).def_rw("press_size", &P::pressSize).def_rw("press_depth", &P::pressDepth)
		.def_rw("press_rim", &P::pressRim).def_rw("fingerprints", &P::fingerprints)
		.def_rw("smears", &P::smears).def_rw("smear_length", &P::smearLength)
		.def_rw("grain", &P::grain).def_rw("grain_scale", &P::grainScale)
		.def_rw("cracks", &P::cracks).def_rw("crack_width", &P::crackWidth).def_rw("crack_depth", &P::crackDepth)
		.def_rw("paint_only", &P::paintOnly).def_rw("paint_radius", &P::paintRadius).def_rw("paint_segments", &P::paintSegments)
		.def_rw("thickness_cap", &P::thicknessCap)
		.def("set_zone_weights", [](P& p, nb::ndarray<const double, nb::c_contig, nb::device::cpu> w) {
			p.zoneWeights.assign(w.data(), w.data() + w.size());
		}, nb::arg("weights"), "one weight per core input point (1 = full clay); empty = everywhere");
	mod.attr("clay") = true;
}
