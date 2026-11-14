// параметры: строим расщеплённые деревянные детали
// ProjectDamage split wood generator: parameters - GPL-3.0-or-later; see LICENSE.
// Plain data. Lengths in the generated object's units. The grain runs along X.
#pragma once
#include <cstdint>

namespace pdgen {

// исходные значения и ограничения отдельных частей эффекта
struct WoodSplitParams
{
	int32_t seed = 0;
	// board
	double length = 1.0, width = 0.14, thickness = 0.03;
	int32_t layers = 4;                    // fibre layers through the thickness; each breaks on its own
	double resolution = 0.0015;            // mesh edge across the grain
	double grainStretch = 4.0;             // mesh edge along the grain = resolution x this
	int32_t maxVerts = 800000;
	double bow = 0.0;                      // whole board bowed (height at the middle)
	// break across the grain
	int32_t breakMode = 1;                 // 0 none, 1 snapped in two, 2 broken end (one piece)
	double breakPos = 0.0;                 // -0.5 .. 0.5 of the length from the middle
	double breakAngle = 18.0;              // degrees the pieces bend at the break (sag)
	double breakGap = 0.04;                // the pieces pulled apart along the grain
	double breakJag = 0.012;               // ragged base line of the break
	// splinters (fibre bundles torn out at the break)
	double splinterWidthMin = 0.003, splinterWidthMax = 0.014;
	double splinterLengthMin = 0.015, splinterLengthMax = 0.28;
	double longShare = 0.3;                // share of long splinters
	double tipShape = 0.7;                 // 0 blunt, 1 needle-sharp tips
	double splinterLift = 0.35;            // splinters peel away from the board (top up, bottom down)
	double splinterSway = 0.15;            // and bend sideways
	double splinterGap = 0.0012;           // cracks between neighbouring splinters
	double crackDepth = 0.06;              // how far those cracks run back into the board
	double layerShift = 0.6;               // 0: all layers break alike, 1: each layer on its own
	// splits along the grain (the board forks)
	int32_t splits = 1;
	double splitLength = 0.35;             // share of the piece's length
	double splitOpen = 0.035;              // opening at the end
	double splitJag = 0.5;                 // fibres torn along the split
	// detail
	double grainRough = 0.4;               // fibre teeth on torn edges along the grain
	double uvScale = 1.0;
};

template <class P, class F>
void VisitWoodSplitFields(P& p, F&& f)
{
	f("seed", p.seed); f("length", p.length); f("width", p.width); f("thickness", p.thickness); f("layers", p.layers);
	f("resolution", p.resolution); f("grain_stretch", p.grainStretch); f("max_verts", p.maxVerts); f("bow", p.bow);
	f("break_mode", p.breakMode); f("break_pos", p.breakPos); f("break_angle", p.breakAngle); f("break_gap", p.breakGap);
	f("break_jag", p.breakJag); f("splinter_width_min", p.splinterWidthMin); f("splinter_width_max", p.splinterWidthMax);
	f("splinter_length_min", p.splinterLengthMin); f("splinter_length_max", p.splinterLengthMax); f("long_share", p.longShare);
	f("tip_shape", p.tipShape); f("splinter_lift", p.splinterLift); f("splinter_sway", p.splinterSway);
	f("splinter_gap", p.splinterGap); f("crack_depth", p.crackDepth); f("layer_shift", p.layerShift);
	f("splits", p.splits); f("split_length", p.splitLength); f("split_open", p.splitOpen); f("split_jag", p.splitJag);
	f("grain_rough", p.grainRough); f("uv_scale", p.uvScale);
}

} // namespace pdgen
