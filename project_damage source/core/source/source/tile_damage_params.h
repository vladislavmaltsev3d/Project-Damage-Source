// параметры: строим плиточное покрытие с локальным разрушением
// ProjectDamage broken tile generator: parameters - GPL-3.0-or-later; see LICENSE.
// Plain data. Lengths in the generated object's units. Tiles lie in XY, Z out of the surface.
#pragma once
#include <cstdint>

namespace pdgen {

// исходные значения и ограничения отдельных частей эффекта
struct TileDamageParams
{
	int32_t seed = 0;
	// surface and tiles
	double width = 1.2, height = 1.0;      // tiled area
	double tileWidth = 0.15, tileHeight = 0.15;
	double tileThickness = 0.008;
	double groutWidth = 0.003;
	double groutRecess = 0.0012;           // grout sits this far below the tile face
	double bevel = 0.0015;                 // rounded glazed edge of the tiles
	double cornerRadius = 0.001;
	int32_t pattern = 0;                   // 0 grid, 1 running bond (rows offset), 2 stacked columns offset
	double rowOffset = 0.5;                // running bond offset, share of the tile
	double bedThickness = 0.02;            // substrate under the tiles (0: none)
	double resolution = 0.004;
	int32_t maxVerts = 1500000;
	double tileWarp = 0.0003;              // tiles not quite flat / not quite level
	// damage zones (random impacts + marker Empties)
	int32_t impacts = 1;
	double impactRadius = 0.4; 
	double damage = 0.8;                   // overall strength inside the zones
	double missing = 0.65;                 // share of tiles knocked out in the zone centre
	double leftovers = 0.5;                // share of knocked-out tiles that leave pieces stuck on
	double loose = 0.35;                   // share of tiles in the zone that are loose and tilted
	double tiltAngle = 7.0;                // degrees
	// cracks
	int32_t cracks = 6;                    // crack paths per impact
	double crackLength = 0.6;
	double followGrout = 0.6;              // how strongly cracks follow the grout lines
	double crackWidth = 0.0015;
	double crackJag = 0.5;
	double pieceShift = 0.0015;            // tile pieces along a crack move apart / sink / tilt
	double pieceTilt = 3.0;                // degrees
	// chips
	double chips = 0.35;                   // chipped edges and corners (rate)
	double chipSize = 0.012;
	double chipDepth = 0.5;                // shallow (glaze) chips: depth as share of the thickness
	double scatterChips = 0.08;            // chips on tiles outside the zones
	// grout and bed
	double groutCrumble = 0.6;             // grout broken out along cracks and around missing tiles
	double adhesive = 0.6;                 // adhesive ridges left on the bed where tiles are gone
	double adhesiveHeight = 0.003;
	double adhesiveSpacing = 0.01;
	double uvScale = 1.0;
};

template <class P, class F>
void VisitTileDamageFields(P& p, F&& f)
{
	f("seed", p.seed); f("width", p.width); f("height", p.height); f("tile_width", p.tileWidth); f("tile_height", p.tileHeight);
	f("tile_thickness", p.tileThickness); f("grout_width", p.groutWidth); f("grout_recess", p.groutRecess); f("bevel", p.bevel);
	f("corner_radius", p.cornerRadius); f("pattern", p.pattern); f("row_offset", p.rowOffset); f("bed_thickness", p.bedThickness);
	f("resolution", p.resolution); f("max_verts", p.maxVerts); f("tile_warp", p.tileWarp);
	f("impacts", p.impacts); f("impact_radius", p.impactRadius); f("damage", p.damage); f("missing", p.missing);
	f("leftovers", p.leftovers); f("loose", p.loose); f("tilt_angle", p.tiltAngle);
	f("cracks", p.cracks); f("crack_length", p.crackLength); f("follow_grout", p.followGrout); f("crack_width", p.crackWidth);
	f("crack_jag", p.crackJag); f("piece_shift", p.pieceShift); f("piece_tilt", p.pieceTilt);
	f("chips", p.chips); f("chip_size", p.chipSize); f("chip_depth", p.chipDepth); f("scatter_chips", p.scatterChips);
	f("grout_crumble", p.groutCrumble); f("adhesive", p.adhesive); f("adhesive_height", p.adhesiveHeight);
	f("adhesive_spacing", p.adhesiveSpacing); f("uv_scale", p.uvScale);
}

} // namespace pdgen
