// общие объявления: разрушаем поверхность камня по выбранному рисунку
// ProjectDamage erosion / weathering generator - GPL-3.0-or-later; see LICENSE.
//
// Stone or concrete worn by weather: a source mesh (or a generated block,
// column, boulder or ashlar wall) is turned into a signed distance volume and
// worn there: convex edges rounded (unevenly, concave corners stay sharp),
// chunks broken off the edges with fresh faceted faces, a granular surface,
// bedding-layer grooves, small pits, larger hollows (honeycomb / tafoni) and,
// for concrete, the cement worn back around exposed aggregate stones.
// The volume is meshed with manifold surface nets (quads, closed).
#pragma once
#include "damage_gen_common.h"
#include "erosion_params.h"

namespace pdgen {

// A zone placed by the host (an Empty) where the weathering is stronger.
struct ErosionMarker { double x = 0, y = 0, z = 0, radius = 0.2, strength = 1.0; };

// Material slots: 0 weathered surface, 1 fresh break (lost edge chunks),
// 2 aggregate stones, 3 mortar (ashlar wall).
// Point attributes: erosion_wear (0..1 the local weathering strength),
// erosion_depth (0..1 how far below the original surface), erosion_cavity
// (0..1 in pits and hollows), erosion_edge (0..1 on worn convex edges).
struct ErosionResult
{
	GenMesh mesh;
	bool ok = false;
	std::string error;
	int voxels = 0, pits = 0, cavities = 0, chunks = 0, stones = 0, pieces = 0;
	double resolution = 0.0;
};

// srcPoints / srcTris: the source mesh for shape 0, ignored otherwise. It
// should be closed; inside is where the winding number along rays in X, Y and
// Z is not zero (majority of the three), so overlapping closed parts join and
// small holes are bridged.
bool BuildErosion(const ErosionParams& p, const std::vector<V3>& srcPoints, const std::vector<std::array<int32_t, 3>>& srcTris,
	const std::vector<ErosionMarker>& markers, ErosionResult& out);

} // namespace pdgen
