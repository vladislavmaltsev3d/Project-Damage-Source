// общие объявления: пробиваем отверстия и оставляем в них арматуру и кабели
// ProjectDamage hanging-rebar holes — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                                          
void PlaceHangHoleCutters(const CoreInput& in, const HangHoleParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, const ProjectDamageHost& host);
                                                                        
                                                            
void BuildHangHoleTubes(const CoreInput& in, const HangHoleParams& p, Int32 mainSeed, const m::Manifold& base,
	const std::vector<m::Manifold>& breakSolids, std::vector<m::Manifold>& tubes, std::vector<Int32>* tubeKinds = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
