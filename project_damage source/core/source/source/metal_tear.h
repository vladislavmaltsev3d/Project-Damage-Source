// общие объявления: режем металлический лист вдоль рисунка и отгибаем его края
// ProjectDamage metal tear — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
#pragma once
#include "project_damage_core.h"
#include "metal_tear_params.h"
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                               
  
                                                                            
                                                                         
                                                                             
                                                                             
                                                                         
                                                                          
                                                                         
                                                                   
  
                                                                           
                                                                          
                                                                           
                                                                          
                                                                      
bool ApplyMetalTearDent(const CoreInput& in, const MetalTearParams& p, Int32 mainSeed,
	const m::Manifold& base, m::Manifold& out, std::string* log = nullptr);
void PlaceMetalTearCuts(const CoreInput& in, const MetalTearParams& p, Int32 mainSeed,
	std::vector<m::Manifold>& cutters);
PROJECT_DAMAGE_NS_MACRO_END
