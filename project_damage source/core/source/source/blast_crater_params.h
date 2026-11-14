// параметры: строим воронку и выброшенные вокруг неё части
// ProjectDamage blast crater generator: parameters - GPL-3.0-or-later; see LICENSE.
// Plain data. Lengths in the generated object's units. The ground lies in XY, Z up.
#pragma once
#include <cstdint>

namespace pdgen {

// исходные значения и ограничения отдельных частей эффекта
struct BlastCraterParams
{
	int32_t seed = 0;
	// ground
	double width = 10.0, height = 10.0;    // ground patch, centred on the origin
	double groundThickness = 0.6;          // closed slab below Z = 0 (0: the top surface only)
	double resolution = 0.05;
	int32_t maxVerts = 1500000;
	double terrain = 0.03;                 // gentle unevenness of the ground
	double terrainScale = 3.0;
	// the crater (at the origin; Crater Points add more)
	int32_t mainCrater = 1;
	double radius = 1.6;
	double depth = 0.7;
	double floorFlat = 0.25;               // flat bottom, share of the radius
	double rimHeight = 0.3;
	double ejecta = 0.6;                   // reach of the thrown-out blanket beyond the rim, share of the radius
	double rays = 0.5;                     // ejecta streaks
	int32_t rayCount = 9;
	double asymmetry = 0.25;               // oblique blast: rim and blanket stronger downrange
	double direction = 30.0;               // downrange, degrees from +X
	double irregularity = 0.5;             // ragged outline and floor
	double clods = 0.5;                    // lumpy, clodded soil on the blanket
	double clodSize = 0.12;
	// pavement over the ground (0 none, 1 asphalt, 2 concrete slabs)
	int32_t pavement = 1;
	double pavementThickness = 0.08;
	double holeSize = 1.12;                // pavement torn away out to this share of the radius
	double plateSize = 0.6;                // broken plates near the rim (larger further out)
	double crackWidth = 0.025;
	double crackReach = 2.6;               // broken zone, share of the radius
	int32_t radialCracks = 7;              // long cracks running out of the broken zone
	double crackLength = 2.5;              // their length, share of the radius
	double plateTilt = 12.0;               // degrees: plates at the rim are pushed up and tilted
	double slabSize = 2.0;                 // concrete: slab size (joints)
	// debris
	int32_t debris = 250;
	double debrisSize = 0.18;
	double debrisSpread = 0.8;             // how far it is thrown, share of the radius
	double debrisInside = 0.2;             // share that fell back into the crater
	double scorch = 0.8;                   // burnt ground around the crater (attribute)
	double uvScale = 1.0;
};

template <class P, class F>
void VisitBlastCraterFields(P& p, F&& f)
{
	f("seed", p.seed); f("width", p.width); f("height", p.height); f("ground_thickness", p.groundThickness);
	f("resolution", p.resolution); f("max_verts", p.maxVerts); f("terrain", p.terrain); f("terrain_scale", p.terrainScale);
	f("main_crater", p.mainCrater); f("radius", p.radius); f("depth", p.depth); f("floor_flat", p.floorFlat);
	f("rim_height", p.rimHeight); f("ejecta", p.ejecta); f("rays", p.rays); f("ray_count", p.rayCount);
	f("asymmetry", p.asymmetry); f("direction", p.direction); f("irregularity", p.irregularity);
	f("clods", p.clods); f("clod_size", p.clodSize);
	f("pavement", p.pavement); f("pavement_thickness", p.pavementThickness); f("hole_size", p.holeSize);
	f("plate_size", p.plateSize); f("crack_width", p.crackWidth); f("crack_reach", p.crackReach);
	f("radial_cracks", p.radialCracks); f("crack_length", p.crackLength); f("plate_tilt", p.plateTilt); f("slab_size", p.slabSize);
	f("debris", p.debris); f("debris_size", p.debrisSize); f("debris_spread", p.debrisSpread); f("debris_inside", p.debrisInside);
	f("scorch", p.scorch); f("uv_scale", p.uvScale);
}

} // namespace pdgen
