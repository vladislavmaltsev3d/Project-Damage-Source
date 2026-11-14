// общие объявления: рассчитываем разрушение здания вокруг заданных точек
// ProjectDamage building destruction — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "building_destruction_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                     
                                                                             
                                        
                                                                           
                                                                            
                                                               
                                                                            
                             
                                                                               
                                                                             
                           
                                                                             
                                    
                                                                           
                                                                             
                                                                            
                                
                                                                       
                                                                              
                                                                             
                                                                              
                                                                           
                                                                            
                                      
                                                                                          
                                                                        
                                                                          
                                  
void PlaceBuildingDestruction(const CoreInput& in, const BuildingDestructionParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& exposed,
	std::vector<m::Manifold>& cracks);
PROJECT_DAMAGE_NS_MACRO_END
