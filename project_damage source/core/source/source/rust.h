// общие объявления: строим старый вариант ржавых повреждений
// ProjectDamage rust - GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "paint_regions.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                              
                                                                             
                                                                        
                                                                             
                                                                          
                                                                 
struct RustField {
    std::vector<double> R;
    PaintRegions paint;                                                    
    bool painted = false;                                         
    double intensity = 0.0;
    bool valid = false;
};

RustField ComputeRustField(const CoreInput& in, const ProjectDamageParams& params);

                                                                            
                                                                               
                                                                             
                                                                
  
                                                                              
                                                                             
                  
void PlaceRust(const CoreInput& in, const ProjectDamageParams& params, const RustField& field,
    std::vector<manifold::Manifold>& cutters, std::vector<manifold::Manifold>& buildup);

                                                                             
                                                                        
                                                                             
                                               
void BuildRustPaint(const CoreInput& in, const ProjectDamageParams& params, const RustField& field,
    double paintThickness, std::vector<manifold::Manifold>& cuts,
    std::vector<manifold::Manifold>& adds);

                                                                              
                                                                 
void EvaluateRustAttributes(const CoreInput& in, const ProjectDamageParams& params,
    const RustField& field, const std::vector<EVec>& points,
    std::vector<float>& rust, std::vector<float>& streak);
PROJECT_DAMAGE_NS_MACRO_END
