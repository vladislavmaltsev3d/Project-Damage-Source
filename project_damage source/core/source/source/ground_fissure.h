// общие объявления: вырезаем грунтовый разлом и строим его края и наклонённые плиты
// ProjectDamage ground fissures — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "ground_fissure_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                        
                                                                            
                                                                             
                                                                           
                                      
                                                                            
                                                                            
                                         
                                                                 
                                                                          
                                                                            
                                                                              
                                                                               
                                                                              
                                    
                                                                                             
                                                                              
                                                                            
                                                                      
                                                                    
                                                                 
void PlaceGroundFissures(const CoreInput& in, const GroundFissureParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& slabs);
PROJECT_DAMAGE_NS_MACRO_END
