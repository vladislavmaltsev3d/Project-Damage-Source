// общие объявления: создаём выбоины бетона и открываем внутреннюю арматуру
// ProjectDamage concrete damage — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "concrete_damage_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                             
                                      
                                                                            
                                                                         
                                                                             
                                                                   
                                                                         
                                                                           
                                                                                          
                                                                          
                                                                            
                                                                   
                                                                       
                                                                             
void PlaceConcreteDamage(const CoreInput& in, const ConcreteDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& rebar);
PROJECT_DAMAGE_NS_MACRO_END
