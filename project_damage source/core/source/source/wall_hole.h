// общие объявления: строим проём и кирпичное заполнение старого варианта стены
// ProjectDamage wall hole with bricks — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
struct WallHoleOut
{
	                                                                              
	std::vector<double> rimFrame, rimCut;                                                                 
	std::vector<Int32> rimKey;
	std::vector<double> rubbleFrame;
	std::vector<Int32> rubbleKey;
	Int32 rimCount = 0, rubbleCount = 0;
};
void PlaceWallHoles(const CoreInput& in, const WallHoleParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, std::vector<Int32>& classes, WallHoleOut& out, const ProjectDamageHost& host);
PROJECT_DAMAGE_NS_MACRO_END
