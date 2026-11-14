// общие объявления: строим повреждённую штукатурку, дранку и деревянный каркас
// ProjectDamage lath damage — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "lath_damage_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                         
                                                                           
                                                                           
                                                                              
                                                                         
                                                                      
                                                                             
                                                                   
                                                                           
                                                                           
                                                                                          
                                                                          
                                                                            
                                                                   
                                                                       
                                                                             
void PlaceLathDamage(const CoreInput& in, const LathDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& laths);

                                                                         
                                                                            
                                                                             
                                                                           
                                                                          
                                                                         
                                                                    
struct LathDamageOut {
	std::vector<m::Manifold> cutters, cracks, planks;
};
void PlaceLathDamage(const CoreInput& in, const LathDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	LathDamageOut& out);
std::vector<manifold::Manifold> BuildLathSurface(const LathDamageParams&, double,double,double,double,int,int,
 const std::vector<double>&,const std::vector<uint8_t>&,const std::vector<uint8_t>&,std::vector<int32_t>&,std::vector<double>&,double nailSize=1.);
PROJECT_DAMAGE_NS_MACRO_END
