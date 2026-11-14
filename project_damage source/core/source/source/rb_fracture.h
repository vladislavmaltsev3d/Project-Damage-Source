// общие объявления: предыдущий вариант подготовки объёмных осколков
// ProjectDamage rigid-body fracture — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "rb_fracture_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                                
                         
                                                                                
                                                                           
                                                                              
                                                                 
                                                                     
                                                                       
                                                                            
                                                     
struct RBPieceInfo { double volume = 0.0; EVec center; };
void BuildRigidBodyFracture(const CoreInput& in, const m::Manifold& source, uint32_t sourceId,
	const RBFractureParams& p, Int32 mainSeed,
	const std::vector<std::vector<EVec>>* cutStrokes,
	const std::vector<std::vector<EVec>>* impactStrokes,
	std::vector<m::Manifold>& pieces, std::vector<RBPieceInfo>& info);
PROJECT_DAMAGE_NS_MACRO_END
