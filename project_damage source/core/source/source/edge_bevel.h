// общие объявления: обрабатываем форму фаски и изношенные участки рёбер
// ProjectDamage edge bevel — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "edge_bevel_params.h"

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                             
                                                                              
struct EdgeWearField
{
	struct Seg  { EVec a, b; double ra, rb; };                                                                     
	struct Chip { EVec c; double r; };
	std::vector<Seg>  segs;
	std::vector<Chip> chips;
	double width = 2.0, noise = 0.6, noiseScale = 3.0;
	EVec   noiseOffset = EVec(0.0);
};

                                                                             
                                                                            
                                                                              
                                                                              
                                                 
                                                                          
                                                             
                                                         
                                                                          
                                                                         
                                                                                   
                                                   
                                                                         
                                                                           
void PlaceEdgeBevel(const CoreInput& in, const EdgeBevelParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& bevels, std::vector<m::Manifold>& wear,
	EdgeWearField* field = nullptr);

                                                                              
                                                                               
                                                                     
std::vector<float> EdgeWearMask(const EdgeWearField& field, const EVec* pts, size_t n);

PROJECT_DAMAGE_NS_MACRO_END
