// общие объявления: формируем вмятины и разрывы металла от воздействия
// ProjectDamage metal deform — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "metal_deform_params.h"

#include <memory>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                         
                             
struct MetalDeformMarks;

                                                                             
                                                                           
  
                                                                        
                                                                              
                                                                           
                                                                            
                                                                            
                                                                             
                                                                              
                                                              
  
                                                                       
                                                                               
  
                                                                             
                                                                         
                                                                    
bool ApplyMetalDeform(const CoreInput& in, const MetalDeformParams& p, Int32 mainSeed,
	const IFieldMask* mask, const m::Manifold& src, m::Manifold& out,
	std::shared_ptr<const MetalDeformMarks>& marks);

                                                           
                                                                        
                                                                  
                                                                              
                                                                                   
                                              
                                                                                      
void EvaluateMetalDeformAttributes(const MetalDeformMarks* marks, const std::vector<EVec>& points,
	std::vector<float>& dent, std::vector<float>& crease, std::vector<float>& contact);
PROJECT_DAMAGE_NS_MACRO_END
