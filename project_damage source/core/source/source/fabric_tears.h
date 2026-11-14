// общие объявления: общие расчёты разрывов ткани и её нитей
// ProjectDamage fabric tears — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "fabric_tears_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                               
                                     
struct FabricTearPaint
{
	const std::vector<SurfacePaintRegion>* holes = nullptr;                 
	const std::vector<SurfacePaintRegion>* edges = nullptr;                      
	double reach = 5.0;                                                             
};

                                                                        
                                                           
                                                                               
                                                                      
                                                                           
                                                                               
                                                                                          
                                                                             
                                                                            
                                                                          
                                                                          
                                                
                                                                       
                                                                       
void PlaceFabricTears(const CoreInput& in, const FabricTearParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& threads,
	const std::vector<double>* zoneWeights = nullptr, const FabricTearPaint* paint = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
