// общие объявления: разрываем металлическую поверхность вокруг точек пробоя
// ProjectDamage metal tear generator - GPL-3.0-or-later; see LICENSE.
//
// Bent and torn sheet metal, built from scratch (standalone, like the cable
// and paper generators: no damage source, a fresh mesh is returned).
//   - blast holes: the centre is blown out, radial tears split the sheet into
//     petals and the petals curl away from the blast; the sheet around the
//     hole bulges
//   - punctures: small shrapnel holes with 3-5 petals
//   - torn slits with bent-out lips, a ragged torn border with bites
//   - dents (bowl .. creased cone, radial buckles), crumple, warp,
//     corrugation and a whole-sheet bend
// The flat sheet lies in XY centred on the origin; blasts and dents push
// along Z (blast side +1: petals stand out of the front, +Z).
#pragma once
#include "damage_gen_common.h"
#include "gen_metal_tear_params.h"
#include "mesh_edit.h"
#include <vector>

namespace pdgen {

// A blast placed by the host (an Empty): position in the flat sheet's XY,
// petal length, side (+1 petals out of +Z, -1 out of -Z) and petal count
// (0 = random between petalsMin and petalsMax).
struct MetalBlastMarker
{
	double x = 0, y = 0, radius = 0.15;
	int32_t side = 1, petals = 0;
};

// Material slots: 0 front (+Z face), 1 back, 2 torn edge / side walls.
// Point attributes: metal_torn (1 on torn edges, fading over tornBand),
// metal_dent (0..1 dent depth), metal_scorch (0..1 around blast holes),
// metal_bend (0..1 how sharply the metal is bent there), metal_petal (1 on petals).
struct MetalTearResult
{
	GenMesh mesh;
	bool ok = false;
	int blasts = 0, punctures = 0, petals = 0, tears = 0, dents = 0, bites = 0;
	double resolution = 0.0;   // the grid edge actually used (after the maxVerts cap)
};

// Pure function of its inputs.
bool BuildMetalTear(const MetalTearParams& p, const std::vector<MetalBlastMarker>& markers, MetalTearResult& out);

// ---------------------------------------------------------------------------
// On an object: the same damage made in an existing mesh (a car door, a
// barrel, a panel) instead of a new sheet. The faces near the damage are
// refined to the resolution and cut; the rest of the mesh, its UVs and its
// materials stay as they were. Blasts, punctures, dents and tears are placed
// at random on the surface (where it is flat enough) and at the markers; the
// sheet settings (size, border, corrugation, bend, warp) are not used.
// thickness > 0 adds an inner skin that far inside and walls along every open
// border, so the holes show the metal's thickness.
// Material ids: the object's own (0 .. sourceMaterials - 1), then
// sourceMaterials: inner skin, sourceMaterials + 1: torn edges (walls).
// Point attributes as for the sheet.
struct MetalMeshMarker
{
	V3 p;                       // in the object's space (scale applied)
	double radius = 0.15;
	int32_t petals = 0;         // 0: random
	int32_t side = 0;           // 0: from where the marker is (petals curl away from it), +1 out, -1 in
};
bool BuildMetalTearOnMesh(const MetalTearParams& p, const MeshInput& in, const std::vector<MetalMeshMarker>& markers,
	int32_t sourceMaterials, MetalTearResult& out);

} // namespace pdgen
