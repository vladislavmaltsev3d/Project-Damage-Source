// общие объявления: строим пробой стекла, острые края и расходящиеся трещины
// ProjectDamage broken glass — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "broken_glass_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                               
           
                                                                     
                                                                              
                                                                      
struct BrokenGlassPaint
{
	const std::vector<SurfacePaintRegion>* holes = nullptr;
	const std::vector<SurfacePaintRegion>* impacts = nullptr;
	double reach = 5.0;                                                             
};

                                                                             
                                                                         
                                                                            
                                                                         
                                                       
                                                                         
                                                                       
                       
                                                                             
                                                                         
                                                                     
                                                                         
                                                                                          
                                                                        
                                                                              
                                                      
                                                                          
                                                                            
                         
void PlaceBrokenGlass(const CoreInput& in, const BrokenGlassParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& chips, const BrokenGlassPaint* paint = nullptr);

                                                                           
                                                                             
                                                                        
                                                                            
                                                                             
                                                                              
                                                                          
                                                                        
                                                                        
                                                                          
                                                                   
void PlaceBrokenGlassThrough(const CoreInput& in, const BrokenGlassParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& chips, std::vector<m::Manifold>& gaps, const BrokenGlassPaint* paint = nullptr);

                                                                              
                                                                 
void PlaceBrokenGlassLegacy(const CoreInput& in, const BrokenGlassParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& holes, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& chips, const BrokenGlassPaint* paint = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
