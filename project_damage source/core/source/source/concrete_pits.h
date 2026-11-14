// общие объявления: добавляем мелкие поры, вмятины и отслоения бетона
// ProjectDamage concrete pitting — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "concrete_pits_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                       
  
                                                                       
                                                                          
                                                                             
                                                                         
                                                                         
                            
bool ApplyConcretePits(const CoreInput& in, const ConcretePitsParams& p, Int32 mainSeed,
	const IFieldMask* mask, const m::Manifold& base, m::Manifold& out, std::string* log = nullptr);

                                                                           
                                                                       
                                                                             
                                                                            
                                                                               
bool ConcretePitsHeightMap(const ConcretePitsParams& p, Int32 mainSeed, double tileSize,
	int32_t res, std::vector<float>& out);
PROJECT_DAMAGE_NS_MACRO_END
