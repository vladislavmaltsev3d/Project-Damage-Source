// общие объявления: мнем и рвём исходную геометрию как бумагу
// ProjectDamage paper generator — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "paper_generator_params.h"
#include <array>
#include <cstdint>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
// A sheet of paper built from scratch: crumpled, with rounded torn holes, torn
// corners / borders and Grease Pencil driven tears and holes. Standalone like
// the cable generator: it needs no damage source, it returns a fresh mesh.
//
// One Grease Pencil stroke in the sheet's OBJECT space (the host transforms the
// stroke points by sheet.matrix_world.inverted() @ gp.matrix_world). Only X and
// Y are used: the flat sheet lies in XY and crumpling only moves points along
// Z, so a stroke drawn onto the crumpled sheet lands where it was drawn.
struct PaperGenStroke
{
	std::vector<EVec> pts;
	bool cyclic = false;       // the GP stroke's cyclic flag (ends that nearly meet also count, see closeShare)
};

struct PaperGenPaint
{
	std::vector<PaperGenStroke> tears;   // layer "Tear": tear lines / torn-away areas
	std::vector<PaperGenStroke> holes;   // layer "Hole": holes / gashes inside the sheet
};

// Material slots: 0 front (+Z), 1 back, 2 side walls of the shell (thickness > 0).
// Per point attributes (one per entry of points):
//   edgeDist : distance (flat sheet) to the nearest border of any kind, capped at edgeDistMax
//   tearDist : distance to the nearest TORN border (tears, holes, bites), capped at edgeDistMax
//              - use it for the white fibrous rim of torn paper in the shader
//   crease   : -1..1, valley .. ridge of the crumple (dirt in valleys, wear on ridges)
//   peel     : 0..1, 1 in the ragged band along torn borders where the printed top
//              layer tore further back than the white base (mix print -> bare paper)
struct PaperGenSurface
{
	std::vector<EVec> points;
	std::vector<std::array<int32_t, 3>> triangles;
};

struct PaperGenResult
{
	std::vector<EVec> points;
	std::vector<int32_t> faceVerts, faceSizes, faceMat;
	std::vector<std::array<double, 2>> loopUv;   // one per face corner
	std::vector<float> edgeDist, tearDist, crease, peel;
	std::vector<int32_t> sourceFace;
	std::vector<std::array<double, 3>> sourceBary;
	int pieces = 0, holes = 0, tears = 0, loops = 0;
	double resolution = 0.0;   // the edge length actually used (after the maxVerts cap)
};

// Pure function of its inputs (same params + strokes = same mesh).
// Returns false when nothing is left of the sheet (e.g. everything torn away).
bool BuildPaper(const PaperGenParams& p, const PaperGenPaint& paint, PaperGenResult& out);
bool ApplyPaper(const PaperGenSurface& surface, const PaperGenParams& p, const PaperGenPaint& paint, PaperGenResult& out);
PROJECT_DAMAGE_NS_MACRO_END
