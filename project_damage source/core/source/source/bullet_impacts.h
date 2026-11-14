// общие объявления: строим следы пуль с формой, зависящей от материала
// ProjectDamage bullet impacts — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "bullet_impacts_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                    
                                                                         
                                                                          
                                                                     
                                                                           
                                                                        
                                                                           
                                                                      
                                                                   
                                                                          
                                                                         
                                                                        
                                                                       
                                                                
                                                                         
                                                                            
                                                                      
                                                                           
                                                                            
                                                                         
                                                                            
                                         
                                                                                          
                                                                          
                                                                            
                                                                    
                                                                          
                                                              
void PlaceBulletImpacts(const CoreInput& in, const BulletImpactParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& halos, std::vector<m::Manifold>& solids);
PROJECT_DAMAGE_NS_MACRO_END
