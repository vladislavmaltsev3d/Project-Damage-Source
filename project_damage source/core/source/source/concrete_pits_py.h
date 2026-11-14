// общие объявления: передаём настройки пористого бетона между Python и расчётом
// ProjectDamage concrete pitting — GPL-3.0-or-later; see LICENSE.
//
// Python binding of the Concrete Pitting block, included once by
// core_standalone/project_damage_core_py.cpp (after the nanobind includes):
//   BindConcretePits(mod)  next to the PlasterDisplaceParams class;
//   .def_prop_ro("pits", ...) on Params, next to "plaster".
#pragma once
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/vector.h>
#include "concrete_pits.h"

inline void BindConcretePits(nanobind::module_& mod)
{
	namespace nb = nanobind;
	nb::class_<ConcretePitsParams>(mod, "ConcretePitsParams").def(nb::init<>())
		.def_rw("enable", &ConcretePitsParams::enable)
		.def_rw("seed", &ConcretePitsParams::seed)
		.def_rw("avoid", &ConcretePitsParams::avoid)
		.def_rw("edge_length", &ConcretePitsParams::edgeLength)
		.def_rw("fine_edge", &ConcretePitsParams::fineEdge)
		.def_rw("max_tris", &ConcretePitsParams::maxTris)
		.def_rw("detail", &ConcretePitsParams::detail)
		.def_rw("strength", &ConcretePitsParams::strength)
		.def_rw("thickness_cap", &ConcretePitsParams::thicknessCap)
		.def_rw("pore_density", &ConcretePitsParams::poreDensity)
		.def_rw("pore_size_min", &ConcretePitsParams::poreSizeMin)
		.def_rw("pore_size_max", &ConcretePitsParams::poreSizeMax)
		.def_rw("pore_small_bias", &ConcretePitsParams::poreSmallBias)
		.def_rw("pore_depth", &ConcretePitsParams::poreDepth)
		.def_rw("pore_sharp", &ConcretePitsParams::poreSharp)
		.def_rw("pore_jagged", &ConcretePitsParams::poreJagged)
		.def_rw("pore_stretch", &ConcretePitsParams::poreStretch)
		.def_rw("pore_rim", &ConcretePitsParams::poreRim)
		.def_rw("pore_merge", &ConcretePitsParams::poreMerge)
		.def_rw("clustering", &ConcretePitsParams::clustering)
		.def_rw("cluster_scale", &ConcretePitsParams::clusterScale)
		.def_rw("flake_enable", &ConcretePitsParams::flakeEnable)
		.def_rw("flake_coverage", &ConcretePitsParams::flakeCoverage)
		.def_rw("flake_scale", &ConcretePitsParams::flakeScale)
		.def_rw("flake_depth", &ConcretePitsParams::flakeDepth)
		.def_rw("flake_layers", &ConcretePitsParams::flakeLayers)
		.def_rw("flake_jagged", &ConcretePitsParams::flakeJagged)
		.def_rw("flake_rough", &ConcretePitsParams::flakeRough)
		.def_rw("aggregate_size", &ConcretePitsParams::aggregateSize)
		.def_rw("flake_pores", &ConcretePitsParams::flakePores)
		.def_rw("dent_density", &ConcretePitsParams::dentDensity)
		.def_rw("dent_size_min", &ConcretePitsParams::dentSizeMin)
		.def_rw("dent_size_max", &ConcretePitsParams::dentSizeMax)
		.def_rw("dent_depth", &ConcretePitsParams::dentDepth)
		.def_rw("dent_chip", &ConcretePitsParams::dentChip)
		.def_rw("grain", &ConcretePitsParams::grain)
		.def_rw("grain_size", &ConcretePitsParams::grainSize)
		.def_rw("undulation", &ConcretePitsParams::undulation)
		.def_rw("undulation_scale", &ConcretePitsParams::undulationScale)
		.def_rw("zone_feather", &ConcretePitsParams::zoneFeather)
		.def_rw("weights", &ConcretePitsParams::weights)
		.def("set_weights", [](ConcretePitsParams& p, nb::ndarray<const double, nb::ndim<1>, nb::c_contig, nb::device::cpu> w) {
			p.weights.assign(w.data(), w.data() + w.shape(0));
		}, nb::arg("weights"), "one weight per input vertex (float64); an empty array = everywhere");
	                                                                           
	                                                                         
	mod.def("concrete_pits_height_map", [](const ConcretePitsParams& p, int32_t seed, double tileSize, int32_t res) {
		std::vector<float> h;
		{
			nb::gil_scoped_release release;
			if (!ConcretePitsHeightMap(p, seed, tileSize, res, h))
				throw std::invalid_argument("concrete_pits_height_map: tile_size > 0 and 8 <= res <= 16384 required");
		}
		float* data = new float[h.size()];
		std::copy(h.begin(), h.end(), data);
		nb::capsule owner(data, [](void* q) noexcept { delete[] (float*)q; });
		return nb::ndarray<nb::numpy, float, nb::ndim<2>>(data, { (size_t)res, (size_t)res }, owner);
	}, nb::arg("params"), nb::arg("seed"), nb::arg("tile_size"), nb::arg("res"));
	mod.attr("concrete_pits") = true;
}
