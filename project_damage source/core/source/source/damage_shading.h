// общие объявления: помечаем повреждённые грани для отдельного сглаживания
// ProjectDamage damage shading — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                            
                                                                           
                                                        
                                                                              
                                                                              
                                                                        
                                                                        
                                                                         
                                                                         
                                                                            
                             
  
                                                                             
                                                                            
                                                                             
                                                                             
                                                                             
                                                                             
                                                                            
                                                                          
                                                       
void ComputeDamageShading(const std::vector<EVec>& points, const std::vector<ProjectDamagePoly>& polys,
	const std::vector<unsigned char>& group, double damageAngle, double baseAngle,
	std::vector<double>& cornerNormals, const MeshView* source = nullptr,
	const std::vector<double>* srcCornerNormals = nullptr, const std::vector<Int32>* basePolyOf = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
