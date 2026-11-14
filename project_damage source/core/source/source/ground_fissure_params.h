// параметры: вырезаем грунтовый разлом и строим его края и наклонённые плиты
// ProjectDamage ground fissures — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Ground Fissures effect (recipe: ground-fissure-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `GroundFissureParams fissure;` member
// without any include cycle. All lengths are scene units (cm in the Blender
// addon, the same units as every other ProjectDamage size); shares/chances are 0..1,
// angles are degrees.
//
// Paint only: every fissure follows one drawn Grease Pencil (or curve)
// stroke. There is no automatic placement.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct GroundFissureParams
{
	bool    enable = false;
    bool generatedGeometry = true;
	int32_t seed   = 0;          // mixed with ProjectDamageParams::seed, like heatSeed/coatingSeed
	bool    avoid  = true;       // honour CoreInput::avoidPoly
	double  detail = 1.0;        // grid resolution of every solid (0.25..4)
	double  upX = 0.0, upY = 0.0, upZ = 1.0;   // object-space "up": strokes are dropped onto the ground along -up

	// --- the main fissure -------------------------------------------------------
	double  width        = 140.0;  // opening at the surface (full width, mid-stroke)
	double  widthRandom  = 0.35;  // slow variation of the width along the stroke (0..1)
	double  depth        = 450.0; // depth at the centre line
	double  depthRandom  = 0.25;  // variation of the depth along the stroke (0..1)
	double  taper        = 0.15;   // share of the stroke length at each end that narrows to a crack
	double  jagged       = 0.6;   // ragged, angular rim (0 = smooth)
	double  edgeScale    = 1.0;   // feature size of the rim noise (x width)
	double  wallLean     = 0.3;   // walls lean in/out by up to this x half-width (undercuts)
	double  wallRough    = 0.5;   // rough walls and floor (0 = smooth)
	double  floorShape   = 0.5;   // 0 = flat floor, 1 = sharp V
	double  thicknessCap = 0.85;  // max depth / local ground thickness (0 = off); never breaks through

	// --- tilted slabs along the rims ------------------------------------------
	bool    slabEnable     = true;
	int32_t slabRows       = 2;      // 1..3 rows of broken slabs on each side
	double  slabLengthMin  = 90.0;   // slab length along the fissure
	double  slabLengthMax  = 240.0;
	double  slabWidthMin   = 70.0;   // slab width outward from the rim (first row)
	double  slabWidthMax   = 170.0;
	double  slabRowFalloff = 0.55;    // each further row: width and tilt x this
	double  slabThickMin   = 40.0;   // slab thickness
	double  slabThickMax   = 90.0;
	double  tiltMin        = 8.0;    // tilt of the rim edge about the slab's back (hinge) edge
	double  tiltMax        = 32.0;
	double  sinkChance     = 0.25;   // share of slabs that drop into the fissure instead of lifting
	double  roll           = 7.0;    // random roll about the axis across the fissure
	double  flatChance     = 0.15;   // share of slabs left untilted (only the cracks show)
	double  sideBias       = 0.3;    // -1..1: positive lifts the right-hand side more (stroke direction)
	double  slabGap        = 2.5;    // width of the cracks between and behind slabs
	double  slabJagged     = 0.5;    // ragged slab outlines
	double  slabSkew       = 0.35;   // slant of the cross cracks between slabs (0 = square)

	// --- side branches ----------------------------------------------------------
	bool    branchEnable     = true;
	double  branchDensity    = 0.6;    // branches per 100 units of fissure length (both sides)
	double  branchLengthMin  = 120.0;
	double  branchLengthMax  = 450.0;
	double  branchWidth      = 14.0;    // opening at the start; tapers to a hairline
	double  branchDepth      = 50.0;
	double  branchMeander    = 0.5;    // 0 = straight
	double  branchAngle      = 45.0;   // max deviation from square to the fissure
	double  branchFork       = 0.35;   // chance of one fork per branch

	// --- rubble ------------------------------------------------------------------
	bool    rubbleEnable  = true;
	double  rubbleDensity = 1.0;    // chunks per 100 units of fissure length
	double  rubbleSizeMin = 0.6;    // chunk size, x local fissure width
	double  rubbleSizeMax = 1.2;
	double  rubbleWedged  = 0.6;    // share of chunks jammed between the walls (rest lie on the floor)

	// --- realism (recipe ground-fissure-v2) ------------------------------------
	// realistic false = Classic: the v1 result, unchanged. true = Realistic:
	// broken concrete instead of clean cell-fracture blocks: rough fracture
	// faces on slabs, sockets and chasm walls (ledges, undercuts), crumbled
	// and chipped slab edges and rim, irregular slab outlines, hairline
	// cracks across slab tops, angular rubble and small debris.
	bool    realistic     = true;
	double  edgeDamage    = 0.7;   // chipped, crumbled edges on slabs and the rim (0 = clean, 2 = heavy)
	double  faceRough     = 0.7;   // relief of the broken faces: slab sides, sockets, chasm walls (0..2)
	double  surfaceCracks = 0.6;   // hairline cracks across slab tops (about this many per slab, 0..3)
	double  debris        = 1.0;   // small broken fragments on the floor and along the rims (0..3)
	double  debrisSize    = 1.0;   // fragment size multiplier (0.25..3)
	double  splitChance   = 0.55;  // share of slabs split by a corner break or a diagonal crack (0..1)
};
