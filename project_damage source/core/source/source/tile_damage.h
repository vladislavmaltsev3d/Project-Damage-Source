// общие объявления: строим плиточное покрытие с локальным разрушением
// ProjectDamage broken tile generator - GPL-3.0-or-later; see LICENSE.
//
// A tiled wall or floor built from scratch and broken: impact zones knock
// tiles out (some leave pieces stuck on), loosen and tilt others; crack paths
// run out of the zones, follow the grout lines and split the tiles they
// cross; edges and corners are chipped (through chips and shallow glaze
// chips); the grout crumbles along cracks and around missing tiles; the bed
// under a missing tile shows the ridges of the adhesive.
// Tiles lie in XY centred on the origin, their faces at Z = tile thickness,
// the bed below Z = 0.
#pragma once
#include "damage_gen_common.h"
#include "tile_damage_params.h"

namespace pdgen {

// An impact zone placed by the host (an Empty): position in XY, radius, strength.
struct TileImpactMarker { double x = 0, y = 0, radius = 0.3, strength = 1.0; };

// Material slots: 0 glaze (tile faces), 1 tile body (edges, backs, chips),
// 2 grout, 3 bed / adhesive.
// Face part: the tile index for tile pieces, -1 for grout and bed.
// Point attributes: tile_random (0..1 per tile, for colour variation),
// tile_chip (0..1, the glaze is chipped off there), tile_crack (1 along cracks),
// tile_damage (0..1 the zone strength).
struct TileDamageResult
{
	GenMesh mesh;
	bool ok = false;
	int tiles = 0, missing = 0, cracked = 0, pieces = 0, loose = 0, chips = 0;
	double resolution = 0.0;
};

bool BuildTileDamage(const TileDamageParams& p, const std::vector<TileImpactMarker>& markers, TileDamageResult& out);

} // namespace pdgen
