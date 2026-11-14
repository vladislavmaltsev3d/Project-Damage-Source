// общие объявления: строим трещины, плиты и выбоины на грунте
// ProjectDamage ground cracks — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "ground_cracks_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                                                                       
                                                                           
                                                        
                                                                            
                                                                   
                                                                             
                                                                   
                                                                       
                                                        
                                                                                          
                                                                          
                                                                            
                                                                             
                                                   
                                                                        
                                                                          
                        
void PlaceGroundCracks(const CoreInput& in, const GroundCracksParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& areas, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& buildup, int32_t* rubbleCount = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
