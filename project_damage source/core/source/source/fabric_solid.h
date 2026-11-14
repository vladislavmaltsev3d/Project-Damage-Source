// общие объявления: режем ткань с толщиной и формируем открытые края
// ProjectDamage fabric tears for solidified cloth — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                            
                                                                              
                                                                          
void PlaceFabricSolidTears(const CoreInput& in, const FabricTearParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& threads, const ProjectDamageHost& host);
PROJECT_DAMAGE_NS_MACRO_END
