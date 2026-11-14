// общие объявления: создаём повреждения дерева вдоль волокон
// ProjectDamage wood damage — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "wood_damage_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                            
                                                                           
                                                                            
                                                              
                                                                            
                                                                       
                                                                            
                                                              
                                                                              
                                                                               
                                                                                          
                                                                          
                                                                            
                                                                         
                                                    
                                                                           
                                                        
void PlaceWoodDamage(const CoreInput& in, const WoodDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& splinters);
PROJECT_DAMAGE_NS_MACRO_END
