// общие объявления: строим воронку и выброшенные вокруг неё части
// ProjectDamage blast crater generator - GPL-3.0-or-later; see LICENSE.
//
// A patch of ground with a blast crater: a bowl with a ragged outline and a
// rubbly floor, a raised rim, a blanket of thrown-out soil that thins outward
// in streaks and clods; optional asphalt or concrete pavement over the ground,
// torn away over the crater, broken into plates around it (pushed up and
// tilted at the rim) and cracked further out; debris (stones, clods, pavement
// chunks) thrown around and fallen back into the crater.
// The ground lies in XY centred on the origin, its surface near Z = 0.
#pragma once
#include "damage_gen_common.h"
#include "blast_crater_params.h"

namespace pdgen {

// A crater placed by the host (an Empty): position in XY, radius, depth factor.
struct CraterMarker { double x = 0, y = 0, radius = 1.0, depth = 1.0; };

// Material slots: 0 ground surface, 1 ground body (sides, bottom) and soil
// clods, 2 pavement top, 3 pavement edges and underside, 4 stones.
// Face part: -1 ground, the plate index for pavement plates, 100000 + n for
// debris piece n.
// Point attributes: crater_scorch (0..1 burnt), crater_ejecta (0..1 thrown-out
// soil), crater_depth (0..1 inside the crater bowls).
struct BlastCraterResult
{
	GenMesh mesh;
	bool ok = false;
	int craters = 0, plates = 0, debris = 0;
	double resolution = 0.0;
};

bool BuildBlastCrater(const BlastCraterParams& p, const std::vector<CraterMarker>& markers, BlastCraterResult& out);

} // namespace pdgen
