// общие объявления: создаём ржавые отверстия и неровные края вокруг них
// ProjectDamage rusty holes — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "rusty_holes_params.h"
#include <unordered_map>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                          
                                                                          
                                                                            
                                                                                                                              
                                                                 
                                                                              
                                                                          
                                                                           
                                                                             
                                                                               
                                                                           
                                                                              
                                                                              
                                                                           
                                                                       
void PlaceRustyHoles(const CoreInput& in, const RustyHoleParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters);
                                                                         
                                                                           
                                                                         
                                                                           
                                                                             
                          
void RustyCutAxes(const std::vector<m::Manifold>& cutters, const std::vector<Int32>& ids,
	const std::unordered_map<Int32, Int32>& owners, Int32 effect, std::unordered_map<Int32, m::vec3>& axes);
void RustyOpenFloors(m::MeshGL& mesh, const std::unordered_map<Int32, m::vec3>& axes);
PROJECT_DAMAGE_NS_MACRO_END
