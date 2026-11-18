// общие объявления: прокладываем арматуру и другие внутренние детали через повреждённый объём
// ProjectDamage break rebar - GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "break_rebar_params.h"

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                             
                                                                      


                                                                             
                                                                            
                                                                         
                                                                        
                                                                          
                                                 
                                                                           
                                                                        
void BuildBreakRebar(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, const m::Manifold* left = nullptr, const SurfaceDirectionField* guide = nullptr);

void BuildRebarSurfacePath(const BreakRebarParams& p, const std::vector<EVec>& points,
    const std::vector<EVec>& normals, bool freeStart, bool freeEnd, BreakRebarMesh& out, bool customPath = false);

PROJECT_DAMAGE_NS_MACRO_END
