// общие объявления: прокладываем царапины вдоль нарисованных штрихов
// ProjectDamage scratches draw — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "scratches_draw_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                      
                                                                          
                                                                           
                                                                          
                                                                      
                                                                
                                                                        
                                                                   
                                                                           
                                                                          
                                                                     
                                                                         
                                                                  
                                                                                          
                                                                          
                                                                             
                      
                                                                           
                                                                     
struct ScratchLine
{
	std::vector<EVec>   pts;
	std::vector<double> halfWidth;                                     
};

void PlaceScratches(const CoreInput& in, const ScratchDrawParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* strokes,
	std::vector<m::Manifold>& grooves, std::vector<m::Manifold>& scuffs,
	std::vector<ScratchLine>* lines);

                                                                        
                                                                           
                                                                                     
                                                       
void ScratchMaskWeights(const std::vector<ScratchLine>& lines, const EVec* pts, int32_t npts,
	double falloff, std::vector<double>& out);
PROJECT_DAMAGE_NS_MACRO_END
