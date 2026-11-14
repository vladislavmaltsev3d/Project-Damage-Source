// параметры: разрываем металлическую поверхность вокруг точек пробоя
// ProjectDamage metal tear generator: parameters - GPL-3.0-or-later; see LICENSE.
// Plain data, no includes besides <cstdint>. Lengths are in the units of the
// generated object's space (the host converts its cm settings x unit scale).
#pragma once
#include <cstdint>

namespace pdgen {

// исходные значения и ограничения отдельных частей эффекта
struct MetalTearParams
{
	int32_t seed = 0;
	// sheet
	double width = 1.2, height = 0.8;      // flat sheet size (X, Y)
	double thickness = 0.002;              // 0 = single surface
	double resolution = 0.006;             // grid edge length
	int32_t maxVerts = 600000;             // grid cap: resolution grows to stay under it
	double corrugation = 0.0;              // corrugated sheet: wave height
	double corrugationPeriod = 0.076;      // wave length
	int32_t corrugationShape = 0;          // 0 sine, 1 trapezoid (box profile)
	double bend = 0.0;                     // whole-sheet bend: curvature (1 / radius), signed
	double bendAngle = 0.0;                // bend direction in the sheet, degrees (0 = around Y)
	double warp = 0.003;                   // low frequency waviness (height)
	double warpScale = 0.6;                // its size
	// dents
	int32_t dents = 5;
	double dentSizeMin = 0.05, dentSizeMax = 0.18;   // radius
	double dentDepth = 0.18;               // depth as share of the radius
	double dentSharpness = 0.5;            // 0 soft bowl .. 1 creased cone
	double dentCreases = 0.5;              // radial buckles in the dent walls
	double dentBack = 0.25;                // share of dents pushed from the back
	// blast holes (petals)
	int32_t blasts = 1;                    // random blast holes (plus the marker blasts)
	double blastSizeMin = 0.12, blastSizeMax = 0.2;  // petal length (radius of the torn star)
	int32_t petalsMin = 5, petalsMax = 8;
	double holeSize = 0.3;                 // blown-out centre, share of the radius
	double petalGap = 0.35;                // gap between petals at the hole, share of the petal width
	double petalCurl = 0.6;                // 0 flat .. 1 tip turned through 180 deg (curled back)
	double curlRandom = 0.35;              // per petal variation
	double petalTwist = 0.25;              // petals twist about their length
	double petalCrumple = 0.4;             // folds and wrinkles in the petals
	double slitRandom = 0.35;              // uneven petal lengths and angles
	double jagged = 0.5;                   // ragged torn edges
	double bulge = 0.18;                   // the sheet around the hole bulges (height / radius)
	double bulgeRadius = 2.2;              // bulge reach, times the radius
	int32_t blastSide = 1;                 // random blasts: +1 petals out of the front (+Z), -1 out of the back
	// punctures: small holes with 3-5 petals (shrapnel)
	int32_t punctures = 0;
	double punctureSizeMin = 0.008, punctureSizeMax = 0.025;
	// tears: torn slits with bent lips
	int32_t tears = 2;
	double tearLengthMin = 0.12, tearLengthMax = 0.35;
	double tearWidth = 0.02;               // widest opening
	double tearOpen = 0.6;                 // how far the lips are bent out
	// border
	double edgeTear = 0.02;                // torn sheet border: depth of the ragged edge
	double edgeTearScale = 0.06;           // size of its lobes
	int32_t edgeBites = 2;                 // big torn-out notches in the border
	double biteSize = 0.12;
	double edgeCurl = 0.03;                // torn border bent out of plane (height)
	double edgeCurlWidth = 0.08;
	// detail
	double crumple = 0.5;                  // fine buckling around the damage
	double crumpleSize = 0.03;
	double minPiece = 0.0004;              // loose bits below this area are dropped
	double tornBand = 0.02;                // reach of the metal_torn mask
	double uvScale = 1.0;                  // UV per unit length
};

// One visit per field: lets the binding map Python dict keys onto the struct.
template <class P, class F>
void VisitMetalTearFields(P& p, F&& f)
{
	f("seed", p.seed); f("width", p.width); f("height", p.height); f("thickness", p.thickness);
	f("resolution", p.resolution); f("max_verts", p.maxVerts); f("corrugation", p.corrugation);
	f("corrugation_period", p.corrugationPeriod); f("corrugation_shape", p.corrugationShape); f("bend", p.bend);
	f("bend_angle", p.bendAngle); f("warp", p.warp); f("warp_scale", p.warpScale);
	f("dents", p.dents); f("dent_size_min", p.dentSizeMin); f("dent_size_max", p.dentSizeMax); f("dent_depth", p.dentDepth);
	f("dent_sharpness", p.dentSharpness); f("dent_creases", p.dentCreases); f("dent_back", p.dentBack);
	f("blasts", p.blasts); f("blast_size_min", p.blastSizeMin); f("blast_size_max", p.blastSizeMax);
	f("petals_min", p.petalsMin); f("petals_max", p.petalsMax); f("hole_size", p.holeSize); f("petal_gap", p.petalGap);
	f("petal_curl", p.petalCurl); f("curl_random", p.curlRandom); f("petal_twist", p.petalTwist);
	f("petal_crumple", p.petalCrumple); f("slit_random", p.slitRandom); f("jagged", p.jagged); f("bulge", p.bulge);
	f("bulge_radius", p.bulgeRadius); f("blast_side", p.blastSide);
	f("punctures", p.punctures); f("puncture_size_min", p.punctureSizeMin); f("puncture_size_max", p.punctureSizeMax);
	f("tears", p.tears); f("tear_length_min", p.tearLengthMin); f("tear_length_max", p.tearLengthMax);
	f("tear_width", p.tearWidth); f("tear_open", p.tearOpen);
	f("edge_tear", p.edgeTear); f("edge_tear_scale", p.edgeTearScale); f("edge_bites", p.edgeBites); f("bite_size", p.biteSize);
	f("edge_curl", p.edgeCurl); f("edge_curl_width", p.edgeCurlWidth);
	f("crumple", p.crumple); f("crumple_size", p.crumpleSize); f("min_piece", p.minPiece); f("torn_band", p.tornBand);
	f("uv_scale", p.uvScale);
}

} // namespace pdgen
