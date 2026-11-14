// параметры: разрушаем объём и собираем новую поверхность по оставшейся форме
// ProjectDamage voxel erosion — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Parameter block for the Voxel Erosion effect (recipe: voxel-erosion-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h
// can include it ahead of ProjectDamageParams and carry one
// `VoxelErosionParams erosion;` member without an include cycle.
//
// Lengths are scene units (cm in the Blender addon). Percent fields are
// 0..100 like the UI. Everything that controls the LOOK is expressed in the
// "reference space" of a piece: the piece (or refSize) scaled so its largest
// dimension is 2, exactly like the Geometry Nodes original with Relative
// Scale on - so the same settings give the same look on any object size.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct VoxelErosionParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                   
	bool    avoid  = true;                                                      

	                                                                               
	double  damage        = 25.0;                                                       
	int32_t noiseType     = 2;                                                                               
	double  noiseScale    = 1.0;                                            
	double  noiseContrast = 0.0;                                                   
	bool    roundFirst    = true;                                                               
	double  postSmooth    = 0.0;
	double edgeWallDepth = 3.0;                                         

	                                                                               
	double  voxelSize   = 0.35;                                             
	                                                                     
	                                                                      
	                                                                  
	                                           
	double  refSize     = 0.0;
	double  thicknessCap = 0.0;                                                          
	int32_t maxVoxels   = 16000000;                                             

	                                                                               
	bool    globalCoords = false;                                                  
	std::vector<double> world;                                                      
	double  posX = 0.0, posY = 0.0, posZ = 0.0;                                      
	double  rotX = 0.0, rotY = 0.0, rotZ = 0.0;                                           
	double  sclX = 1.0, sclY = 1.0, sclZ = 1.0;                     

	                                                                               
	                                                                         
	                                     
	std::vector<double> zoneWeights;
	                                                                          
	                                                                 
	std::vector<float> maskPixels;
	int32_t maskWidth  = 0;
	int32_t maskHeight = 0;
	std::vector<double> maskUvs;                                                                  
	bool    maskBox    = false;                                           
	bool    maskInvert = false;
	double  maskAdjust = 0.0;                                          
};
