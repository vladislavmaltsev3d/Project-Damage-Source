// общие объявления: разрушаем объём и собираем новую поверхность по оставшейся форме
// ProjectDamage voxel erosion — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
#pragma once
#include "project_damage_core.h"
#include "voxel_erosion_params.h"
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                            
                                                                            
                                                 
  
                                                                          
                                                                 
                                                                   
                                                       
  
                                                                           
                                                                     
                                                                        
                               
void PlaceVoxelErosion(const CoreInput& in, const VoxelErosionParams& p, Int32 mainSeed,
	const IFieldMask* mask, std::vector<m::Manifold>& cutters, std::string* log = nullptr);

                                                                             
                                                                              
                                                                              
                                                                         
                                                                             
                                                     
                                                                            
                                 
void TransferSurfaceUvs(const std::vector<EVec>& srcPts, const std::vector<int32_t>& srcTris,
	const std::vector<double>& srcUvs, const std::vector<EVec>& qPts,
	const std::vector<int32_t>& qFace, std::vector<double>& outUv);
PROJECT_DAMAGE_NS_MACRO_END
