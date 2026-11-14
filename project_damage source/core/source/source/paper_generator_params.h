// параметры: мнем и рвём исходную геометрию как бумагу
// ProjectDamage paper generator — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Paper generator (recipe: paper-generator-v1).
// Plain data with no ProjectDamage/Manifold includes, like the other *_params.h
// files, so it can be bound to Python on its own. All lengths are WORLD units of
// the sheet's object space (the host converts its cm settings, like
// CableGenParams); shares / strengths are 0..1 unless noted.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct PaperGenParams
{
	int32_t seed = 0;

	// --- sheet ---------------------------------------------------------------
	// The flat sheet lies in the object's XY plane, centred on the origin, front
	// face along +Z. UVs map the flat sheet to 0..1, so crumpling never stretches
	// a printed texture (newspaper page, letter, ...).
	double  width      = 0.21;     // along X  (A4 portrait in metres)
	double  height     = 0.297;    // along Y
	double  resolution = 0.002;    // target edge length of the mesh
	int32_t quads      = 1;        // 1 = regular quad grid inside, triangles only at torn borders; 0 = all triangles
	double  thickness  = 0.0;      // 0 = single surface (one side); > 0 = closed shell with side walls
	double  edgeRough  = 0.1;      // slight raggedness of the original (uncut) border, 0..1

	// --- crumple -------------------------------------------------------------
	// A network of sharp creases between flat facets (random-height piecewise
	// linear field over a random triangulation, up to three octaves), blended
	// with soft wrinkles. Strength is the facet slope, so it looks the same at
	// every Size.
	double  crumpleStrength  = 0.5;   // 0 = flat .. 1 = heavily crumpled (above 1 allowed, up to 3)
	double  crumpleSize      = 0.04;  // average distance between creases
	double  crumpleDetail    = 0.5;   // weight of the finer crease octaves 0..1
	double  crumpleSharpness = 0.8;   // 0 = soft wrinkles .. 1 = sharp creases and flat facets
	double  waviness         = 0.15;  // large soft undulation of the whole sheet 0..1
	double  waveSize         = 0.15;  // its wavelength
	int32_t foldsX           = 0;     // straight fold lines across X (newspaper folded and opened)
	int32_t foldsY           = 0;     // ... across Y
	double  foldDepth        = 0.004; // height of the zig-zag between folds

	// --- torn edges (Tear strokes, corner tears, border bites) ----------------
	// The strokes / procedural lines only choose WHICH area is torn away; the torn
	// outline itself is generated: it wanders around the drawn line by up to
	// tornWidth with big lobes, long sharp tongues of paper and fine fibres, and
	// leaves small loose flakes - like a poster torn off a wall.
	double  tornWidth         = 0.02;   // how far the torn outline wanders from the drawn line / area
	double  tornScale         = 0.03;   // size of the big lobes (tongue width ~ 0.45 x this)
	double  tongues           = 0.6;    // long sharp tongues / fingers of paper 0..1
	double  tongueStretch     = 3.0;    // tongue length / width 1..8
	double  tongueAngle       = -90.0;  // degrees in the sheet plane: the direction the paper was pulled
	double  tongueAngleRandom = 0.25;   // 0..1 wander of that direction across the sheet
	double  tornDetail        = 0.7;    // fractal fibres down to the resolution 0..1
	double  flakes            = 0.5;    // share of the small loose bits near a torn edge that stay 0..1
	double  peelWidth         = 0.006;  // width of the 'peel' attribute band (printed layer gone), 0 = off

	// --- torn look of the HOLES (procedural + Hole strokes): rounded -----------
	double  ragSize   = 0.012;   // feature size of the ragged torn outline
	double  jagged    = 0.45;    // large-scale raggedness of torn outlines 0..1
	double  fiber     = 0.4;     // fine fibrous jaggedness 0..1
	double  roundness = 0.5;     // rounding of holes and torn-away areas 0..1 (x ragSize)
	double  tearGap   = 0.0;     // width of a tear line between pieces (0 = auto, the thinnest that separates)
	int32_t keepPieces = 0;      // 0 = only the largest piece survives a tear; 1 = every piece stays (paper torn apart)
	double  minPiece  = 0.02;    // keepPieces: pieces smaller than this share of the largest are dropped
	double  edgeCurl  = 0.0;     // torn rims lift / curl up (x curlWidth) 0..1
	double  curlWidth = 0.02;    // how far into the sheet the curl reaches
	double  edgeDistMax = 0.03;  // cap of the edge_dist / tear_dist attributes

	// --- procedural interior holes -------------------------------------------
	int32_t holeAmount     = 0;
	double  holeSizeMin    = 0.01;   // radius
	double  holeSizeMax    = 0.035;
	double  holeIrregular  = 0.4;    // lobed outline 0..1
	double  holeElongation = 0.3;    // stretched holes 0..1
	double  holeMargin     = 0.015;  // min distance of a hole from the sheet border

	// --- procedural torn edges ----------------------------------------------
	int32_t cornerTears    = 0;      // corners torn off (at most 4, each corner once)
	double  cornerSizeMin  = 0.03;   // leg length of the torn-off corner
	double  cornerSizeMax  = 0.08;
	int32_t edgeBites      = 0;      // pieces torn out of the border
	double  biteSizeMin    = 0.015;  // width of a bite along the border
	double  biteSizeMax    = 0.05;
	double  biteDepth      = 0.5;    // depth of a bite, x its width

	// --- Grease Pencil -------------------------------------------------------
	// Tears list:  open stroke  = a tear line (the smaller side is torn off);
	//              closed stroke = the area inside is torn away.
	// Holes list:  closed stroke = a hole with a torn, rounded rim;
	//              open stroke   = a gash along the stroke, strokeHoleWidth wide.
	// An open tear line whose end stops within tearSnap of the border is extended
	// to it, so a line drawn "almost to the edge" still tears the piece off.
	double  strokeHoleWidth = 0.012;
	double  tearSnap        = 0.03;
	double  closeShare      = 0.12;  // a stroke counts as closed when its ends are this close (x its length) or cyclic
	int32_t paintOnly       = 0;     // with any stroke present, skip the procedural holes / tears

	int32_t maxVerts = 200000;      // safety cap; the resolution is coarsened to stay below it
};
