// общие объявления: добавляем сетку только рядом с повреждением
// ProjectDamage adaptive refinement — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <functional>

PROJECT_DAMAGE_NS_MACRO_BEGIN
using AdaptiveTriangleSize = std::function<double(const m::vec3&,const m::vec3&,const m::vec3&)>;
                                                                          
                                                                             
                                                                              
                                                                        
m::Manifold RefineNearDamage(const m::Manifold& base, const std::vector<m::Box>& zones,
	double nearSize, double grade, int maxLevels, size_t maxTris,
	const std::vector<Int32>* onlyRuns = nullptr, const AdaptiveTriangleSize* triangleSize = nullptr);
                                                                           
                                                                         
                                                                         
                                                                          
PROJECT_DAMAGE_NS_MACRO_END
