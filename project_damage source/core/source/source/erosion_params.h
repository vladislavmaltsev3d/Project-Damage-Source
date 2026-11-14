// параметры: разрушаем поверхность камня по выбранному рисунку
// ProjectDamage erosion / weathering generator: parameters - GPL-3.0-or-later; see LICENSE.
// Plain data. Lengths in the generated object's units, Z up.
#pragma once
#include <cstdint>

namespace pdgen {

// исходные значения и ограничения отдельных частей эффекта
struct ErosionParams
{
	int32_t seed = 0;
	// shape: 0 the source mesh (given to BuildErosion), 1 block, 2 column, 3 boulder, 4 ashlar wall
	int32_t shape = 1;
	double sizeX = 0.6, sizeY = 0.4, sizeZ = 0.4;  // generated shapes stand on Z = 0, centred in XY
	double shapeBevel = 0.003;                     // the generated shape's own small edge round
	double blockWidth = 0.4, blockHeight = 0.2;    // ashlar wall: stone size (the wall lies in XZ, thickness along Y)
	double jointWidth = 0.012;
	double mortarRecess = 0.008;                   // mortar sits this far behind the stone faces
	// voxel volume
	double resolution = 0.005;
	int32_t maxVoxels = 12000000;                  // the resolution is coarsened to stay below it
	// how much and where
	double amount = 1.0;                           // overall weathering strength
	double variation = 0.5;                        // patches weathered more and less
	double variationScale = 0.25;
	double exposure = 0.3;                         // faces turned to the exposure direction wear more
	double exposureX = 0.0, exposureY = 0.0, exposureZ = 1.0;
	// edges
	double edgeRound = 0.025;                      // radius of the worn, rounded edges (concave corners stay)
	double edgeLoss = 0.5;                         // chunks broken off along the edges
	double edgeLossSize = 0.05;
	// surface
	double roughness = 0.0015;                     // granular surface: amplitude
	double roughScale = 0.015;
	double strata = 0.0;                           // bedding layers: depth of the grooves (0: none)
	double strataSpacing = 0.05;
	double strataAngle = 0.0;                      // tilt of the layers, degrees
	// pits and cavities
	double pits = 0.5;                             // small pits (share of the surface cells)
	double pitSize = 0.006;
	double pitDepth = 0.5;
	double cavities = 0.0;                         // larger hollows, honeycomb / tafoni
	double cavitySize = 0.06;
	double cavityDepth = 0.5;
	// concrete
	double aggregate = 0.0;                        // exposed aggregate stones (packing share)
	double aggregateSize = 0.012;
	double cementLoss = 0.003;                     // the cement between the stones is worn back this far
	double minPiece = 0.002;                       // loose bits smaller than this share of the faces are dropped
	double uvScale = 1.0;
};

template <class P, class F>
void VisitErosionFields(P& p, F&& f)
{
	f("seed", p.seed); f("shape", p.shape); f("size_x", p.sizeX); f("size_y", p.sizeY); f("size_z", p.sizeZ);
	f("shape_bevel", p.shapeBevel); f("block_width", p.blockWidth); f("block_height", p.blockHeight);
	f("joint_width", p.jointWidth); f("mortar_recess", p.mortarRecess);
	f("resolution", p.resolution); f("max_voxels", p.maxVoxels);
	f("amount", p.amount); f("variation", p.variation); f("variation_scale", p.variationScale); f("exposure", p.exposure);
	f("exposure_x", p.exposureX); f("exposure_y", p.exposureY); f("exposure_z", p.exposureZ);
	f("edge_round", p.edgeRound); f("edge_loss", p.edgeLoss); f("edge_loss_size", p.edgeLossSize);
	f("roughness", p.roughness); f("rough_scale", p.roughScale);
	f("strata", p.strata); f("strata_spacing", p.strataSpacing); f("strata_angle", p.strataAngle);
	f("pits", p.pits); f("pit_size", p.pitSize); f("pit_depth", p.pitDepth);
	f("cavities", p.cavities); f("cavity_size", p.cavitySize); f("cavity_depth", p.cavityDepth);
	f("aggregate", p.aggregate); f("aggregate_size", p.aggregateSize); f("cement_loss", p.cementLoss);
	f("min_piece", p.minPiece); f("uv_scale", p.uvScale);
}

} // namespace pdgen
