// общие объявления: укладываем обои на поверхность и отгибаем надорванные полосы
// ProjectDamage wallpaper — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "wallpaper_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                          
                                    
                                                                        
                                                                          
                                                            
                                                                         
                                                                          
                                                                           
                                                                          
                                                                             
                                           
struct WallpaperFaceIds { int32_t front = -1, back = -1, wall = -1, flapFront = -1, flapBack = -1; };
WallpaperFaceIds WallpaperIds();

                                                                          
                                                                          
                                                                             
                                                                            
                 
std::vector<uint8_t> WallpaperPaperedPolys(const CoreInput& in, const WallpaperParams& p);

                                                               
                                                                              
                                                                            
                                                  
                                                                         
                                                                          
                                                                        
                                                                             
                                                                             
                                                  
                                                                      
                                                                         
                                                                         
                                                                              
                                                                             
                                                                       
                                                                          
void PlaceWallpaper(const CoreInput& in, const WallpaperParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<EVec>& normals,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& buildup,
	const std::vector<SurfacePaintRegion>* paint = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
