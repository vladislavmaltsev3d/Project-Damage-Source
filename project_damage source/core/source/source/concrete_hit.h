// общие объявления: строим бетонный пробой и трещины вокруг точки удара
// ProjectDamage concrete hit — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "concrete_hit_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                                                                         
                                                                            
                                                                             
                                                                        
                                                               
                                                                           
                                                                              
                             
                                                                             
                                                                            
                                                                          
                                                                      
                                                                             
                                              
                                                                                          
                                                                        
                                                                        
                            
void PlaceConcreteHit(const CoreInput& in, const ConcreteHitParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& pushed,
	std::vector<m::Manifold>& cracks);

                                                                         
                                                                          
                                                                                   
                                                                          
                                                                                
                                                                         
                                                                         
                                                                           
                                                                
                                                                       
                                                            
                                                                              
                                                                            
                                                                         
                                                                
void PlaceConcreteHitThrough(const CoreInput& in, const ConcreteHitParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& gaps,
	std::vector<m::Manifold>& cracks);
PROJECT_DAMAGE_NS_MACRO_END
