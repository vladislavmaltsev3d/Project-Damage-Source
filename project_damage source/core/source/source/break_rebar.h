// общие объявления: прокладываем арматуру и другие внутренние детали через повреждённый объём
// ProjectDamage break rebar - GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "break_rebar_params.h"

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                             
                                                                      
struct BreakRebarMesh
{
	std::vector<EVec>              points;
	std::vector<ProjectDamagePoly> polys;                                       
	std::vector<Int32>             polyBar;                                           
	std::vector<float>             exposure;                                                      
	std::vector<Int32>             polyPart;                                                                                    
	std::vector<float>             grain;                                                                                
	Int32                          bars = 0;
	                                                                                
	                                                                              
	                                                                            
	                                                                             
	                                                                      
	                                                                              
	                                                                            
	                                                                          
	std::vector<double>            brickFrame;
	std::vector<Int32>             brickKey;
	std::vector<double>            brickCut;
};

                                                                             
                                                                            
                                                                         
                                                                        
                                                                          
                                                 
                                                                           
                                                                        
void BuildBreakRebar(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, const m::Manifold* left = nullptr, const SurfaceDirectionField* guide = nullptr);

void BuildRebarSurfacePath(const BreakRebarParams& p, const std::vector<EVec>& points,
    const std::vector<EVec>& normals, bool freeStart, bool freeEnd, BreakRebarMesh& out);

PROJECT_DAMAGE_NS_MACRO_END
