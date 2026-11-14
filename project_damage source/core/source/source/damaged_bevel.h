// общие объявления: строим фаску с локальными сколами вдоль рёбер
// ProjectDamage damaged edge bevel — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "damaged_bevel_params.h"
#include "edge_bevel.h"                                                           

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                            
                                                                          
                                                      
                                                                       
                                                                                 
                                                                     
                                                                     
                                 
                                                                            
                                                                         
                                                                             
                                            
                                                                              
                                                                              
                                           
                                                                           
                                                                         
                                                  
void PlaceDamagedBevel(const CoreInput& in, const DamagedBevelParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>* bevels, std::vector<m::Manifold>* damage,
	EdgeWearField* wearField = nullptr, EdgeWearField* damageField = nullptr);

                                                                              
                                                                     
                                                                   
                                                                  
std::vector<std::pair<double, double>> DamagedBevelPatches(double length, bool closed,
	double factor, double patchLength, uint64_t seed, uint64_t run, double randomness = 0.75);

                                                                           
                                                                             
                                                                        
                                                                           
                                                                       
void DamagedBevelSegments(const CoreInput& in, const DamagedBevelParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<double>& points, std::vector<int32_t>& tris);

PROJECT_DAMAGE_NS_MACRO_END
