// общие объявления: обмен настройками и геометрией эрозии с Python
// ProjectDamage voxel erosion — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
//
// Python binding of the Voxel Erosion block, included once by
// core_standalone/project_damage_core_py.cpp (after the nanobind includes):
//   BindVoxelErosion(mod)      next to BindConcretePits(mod);
//   .def_prop_ro("erosion", ...) on Params, next to "pits".
#pragma once
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/vector.h>
#include <cmath>
#include <stdexcept>
#include "voxel_erosion.h"

                                                                         
inline void CheckVoxelErosion(const VoxelErosionParams& p)
{
	if (!p.enable) return;
	const double v[] = { p.damage, p.noiseScale, p.noiseContrast, p.postSmooth, p.edgeWallDepth, p.voxelSize, p.refSize,
		p.thicknessCap, p.posX, p.posY, p.posZ, p.rotX, p.rotY, p.rotZ, p.sclX, p.sclY, p.sclZ, p.maskAdjust };
	for (double x : v)
		if (!std::isfinite(x)) throw std::invalid_argument("erosion: every parameter must be finite");
	if (!(p.voxelSize > 0.0)) throw std::invalid_argument("erosion_voxel_size must be > 0");
	for (double x : p.world)
		if (!std::isfinite(x)) throw std::invalid_argument("erosion world matrix must be finite");
}

inline void BindVoxelErosion(nanobind::module_& mod)
{
	namespace nb = nanobind;
	using P = VoxelErosionParams;
	using F64 = nb::ndarray<const double, nb::c_contig, nb::device::cpu>;
	nb::class_<P>(mod, "VoxelErosionParams").def(nb::init<>())
		.def_rw("enable", &P::enable)
		.def_rw("seed", &P::seed)
		.def_rw("avoid", &P::avoid)
		.def_rw("damage", &P::damage)
		.def_rw("noise_type", &P::noiseType)
		.def_rw("noise_scale", &P::noiseScale)
		.def_rw("noise_contrast", &P::noiseContrast)
		.def_rw("round_first", &P::roundFirst)
		.def_rw("post_smooth", &P::postSmooth)
		.def_rw("edge_wall_depth", &P::edgeWallDepth)
		.def_rw("voxel_size", &P::voxelSize)
		.def_rw("ref_size", &P::refSize)
		.def_rw("thickness_cap", &P::thicknessCap)
		.def_rw("max_voxels", &P::maxVoxels)
		.def_rw("global_coords", &P::globalCoords)
		.def_rw("pos_x", &P::posX).def_rw("pos_y", &P::posY).def_rw("pos_z", &P::posZ)
		.def_rw("rot_x", &P::rotX).def_rw("rot_y", &P::rotY).def_rw("rot_z", &P::rotZ)
		.def_rw("scl_x", &P::sclX).def_rw("scl_y", &P::sclY).def_rw("scl_z", &P::sclZ)
		.def_rw("mask_box", &P::maskBox)
		.def_rw("mask_invert", &P::maskInvert)
		.def_rw("mask_adjust", &P::maskAdjust)
		.def_ro("mask_width", &P::maskWidth)
		.def_ro("mask_height", &P::maskHeight)
		.def_ro("world", &P::world)
		.def_ro("zone_weights", &P::zoneWeights)
		.def_ro("mask_uvs", &P::maskUvs)
		.def_ro("mask_pixels", &P::maskPixels)
		.def("set_world", [](P& p, F64 m) {
			if (m.size() != 12) throw std::invalid_argument("set_world: 12 values (3x4 row-major) required");
			p.world.assign(m.data(), m.data() + 12);
		}, nb::arg("matrix"), "object -> world, row-major 3x4, scene units; empty array clears")
		.def("set_zone_weights", [](P& p, F64 w) { p.zoneWeights.assign(w.data(), w.data() + w.size()); },
			nb::arg("weights"), "one weight per core input point (1 = full erosion); empty = everywhere")
		.def("set_mask_uvs", [](P& p, F64 uv) {
			if (uv.size() % 8 != 0) throw std::invalid_argument("set_mask_uvs: (polys, 4, 2) or (polys, 8) required");
			p.maskUvs.assign(uv.data(), uv.data() + uv.size());
		}, nb::arg("uvs"), "per input polygon 4 corners x (u, v), the read_uvs layout; empty = box projection")
		.def("set_mask_image", [](P& p, nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu> img) {
			p.maskHeight = (int32_t)img.shape(0);
			p.maskWidth = (int32_t)img.shape(1);
			p.maskPixels.assign(img.data(), img.data() + img.size());
		}, nb::arg("pixels"), "(height, width) float32 grey, row 0 = bottom (Blender order); white = protected")
		.def("clear_masks", [](P& p) {
			p.zoneWeights.clear(); p.maskUvs.clear(); p.maskPixels.clear(); p.maskWidth = p.maskHeight = 0;
		});

	                                                               
	mod.def("erosion_transfer_uvs", [](nb::ndarray<const double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> srcPts,
			nb::ndarray<const int32_t, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> srcTris,
			nb::ndarray<const double, nb::shape<-1, 6>, nb::c_contig, nb::device::cpu> srcUvs,
			nb::ndarray<const double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> qPts,
			nb::ndarray<const int32_t, nb::ndim<1>, nb::c_contig, nb::device::cpu> qFace) {
		if (srcUvs.shape(0) != srcTris.shape(0)) throw std::invalid_argument("erosion_transfer_uvs: one UV row per source triangle");
		if (qFace.shape(0) != qPts.shape(0)) throw std::invalid_argument("erosion_transfer_uvs: one face index per query point");
		const size_t np = srcPts.shape(0);
		for (size_t i = 0; i < srcTris.size(); ++i)
			if (srcTris.data()[i] < 0 || (size_t)srcTris.data()[i] >= np) throw std::invalid_argument("erosion_transfer_uvs: triangle index out of range");
		std::vector<EVec> sp(np), qp(qPts.shape(0));
		for (size_t i = 0; i < np; ++i) sp[i] = EVec(srcPts(i, 0), srcPts(i, 1), srcPts(i, 2));
		for (size_t i = 0; i < qp.size(); ++i) qp[i] = EVec(qPts(i, 0), qPts(i, 1), qPts(i, 2));
		std::vector<int32_t> tris(srcTris.data(), srcTris.data() + srcTris.size());
		std::vector<double> uvs(srcUvs.data(), srcUvs.data() + srcUvs.size());
		std::vector<int32_t> qf(qFace.data(), qFace.data() + qFace.size());
		std::vector<double> out;
		{
			nb::gil_scoped_release release;
			TransferSurfaceUvs(sp, tris, uvs, qp, qf, out);
		}
		double* data = new double[out.size()];
		std::copy(out.begin(), out.end(), data);
		nb::capsule owner(data, [](void* q) noexcept { delete[] (double*)q; });
		return nb::ndarray<nb::numpy, double, nb::ndim<2>>(data, { qp.size(), (size_t)2 }, owner);
	}, nb::arg("src_points"), nb::arg("src_tris"), nb::arg("src_uvs"), nb::arg("query_points"), nb::arg("query_face"));
	mod.attr("voxel_erosion") = true;
}
