// общие объявления: строим расщеплённые деревянные детали
// ProjectDamage split wood generator - GPL-3.0-or-later; see LICENSE.
//
// A board (plank or beam) broken and split along the grain, built from
// scratch: the break across the grain is a line of fibre bundles torn at
// different lengths, so long pointed splinters stick out of both pieces and
// interlock; the pieces bend at the break and are pulled apart; splinters peel
// away from the board; splits along the grain fork the board. The wood is
// built from fibre layers stacked through the thickness, each breaking on its
// own, so a broken end steps in ragged layers.
// The grain runs along X, the board is centred on the origin, Z up.
#pragma once
#include "damage_gen_common.h"
#include "wood_split_params.h"
#include "mesh_edit.h"

namespace pdgen {

// Material slots: 0 wood surface (faces, edges and ends of the board),
// 1 fresh wood (broken walls, splits, the faces between layers).
// Face part: one id per piece and layer (piece * layers + layer).
// Point attributes: wood_splinter (0 at the root .. 1 at the tip of a peeled
// splinter), wood_break (1 at broken / split edges, fading over a few mm),
// wood_layer (0 bottom .. 1 top layer).
struct WoodSplitResult
{
	GenMesh mesh;
	bool ok = false;
	int pieces = 0, splinters = 0, splits = 0;
	double resolution = 0.0;
};

bool BuildWoodSplit(const WoodSplitParams& p, WoodSplitResult& out);

// ---------------------------------------------------------------------------
// On an object: an existing mesh (a beam, a plank, a post, a log) is broken
// across its grain instead of a new board being built. The grain runs along
// axis (0 X, 1 Y, 2 Z of the object; -1: its longest side); the fibre layers
// stack along the thinner of the other two. The cut is the same splintered
// break as the board's (bundles of fibres torn at different lengths, pointed
// tips, ragged layers); both pieces are closed again with the broken wood,
// bent at the break and pulled apart (break mode 1), or the far piece is
// dropped (break mode 2: a broken end). The rest of the mesh, its UVs and its
// materials stay as they were. Splits along the grain and the board settings
// (length, width, thickness, bow) are not used.
// Material ids: the object's own, then sourceMaterials: the broken wood.
// Face part: the piece (0, 1). Point attributes as for the board.
bool BuildWoodSplitOnMesh(const WoodSplitParams& p, const MeshInput& in, int32_t axis, int32_t sourceMaterials, WoodSplitResult& out);

} // namespace pdgen
