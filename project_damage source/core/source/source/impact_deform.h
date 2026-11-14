// общие объявления: деформируем меш вокруг точки удара с учётом защищённых мест
// ProjectDamage impact deformation — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "impact_deform_params.h"
#include <cstddef>
#include <cstdint>
#include <vector>

                                                                             
                                                                             
                                                                         
                                                                         
                                                                              
                                                                       
                                        
int ApplyImpactDeform(double* xyz, int n, const ImpactDeformParams& p, std::vector<float>* amount,
                      const int32_t* quads = nullptr, size_t nquads = 0);
