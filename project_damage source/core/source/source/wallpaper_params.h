// параметры: укладываем обои на поверхность и отгибаем надорванные полосы
// ProjectDamage wallpaper — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Wallpaper effect (recipe: wallpaper-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `WallpaperParams wallpaper;` member without
// an include cycle. All lengths are scene units (cm in the Blender addon, the
// same units as every other size); shares are 0..1, angles in degrees.
//
// The wallpaper is the outer `thickness` of the SOURCE mesh itself, exactly
// like inline Coating Peeling: nothing is offset or added as a separate layer.
// Torn areas and seams are carved into the source; the peeled flaps are solids
// fused onto it.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct WallpaperParams
{
    bool manualSeams=false;
    std::vector<double> seamEdges;
	bool    enable = false;
	int32_t seed   = 0;                                                                

	                                                                             
	double  thickness   = 0.12;                                                           
	double  stripWidth  = 53.0;                                                            
	double  seamOffset  = 0.0;                                                
	double  seamDirX = 0.0, seamDirY = 0.0, seamDirZ = 1.0;                                        
	std::vector<double> finderPoints;
	bool    finderEnable = false;
	double  finderX = 0.0, finderY = 0.0, finderZ = 0.0;
	double  wallAngle   = 25.0;                                                                        
	double  detail      = 1.0;                                                  
	bool    avoid       = true;                                

	                                                                           
	bool    seamsEnable = true;                                   
	double  seamWidth   = 0.12;
	double  seamDepth   = 0.6;                                        
	double  seamOpen    = 0.35;                                               

	                                                                            
	std::vector<double> flapMarkers;
	bool markerOnly = false;
	bool    tearsEnable   = true;
	int32_t tearAmount    = 2;                                                                                        
	double  tearLengthMin = 60.0;                                              
	double  tearLengthMax = 140.0;
	double  tearWidthMin  = 0.7;                                               
	double  tearWidthMax  = 1.0;                                              
	double  tearDiagonal  = 0.85;                                                              
	double  tearFromTop   = 0.8;                                                       
	int32_t tearSide      = 0;                                                       
	double  tearDiagonalRandomness = 0.0;
	double  tearFromTopRandomness = 0.0;
	double  tearJagged    = 0.5;                                   
	double  seamCrack     = 0.6;                                                                
	double  seamCrackLength = 0.45;                 

	                                                                            
	bool    flapsEnable  = true;
	double  flapAmount   = 0.9;                                                               
	double  flapFold     = 170.0;                                                                            
	double  flapFoldRadius = 0.035;                                          
	double  flapCurl     = 230.0;                                                                      
	double  flapCurlFocus = 2.0;                                                                            
	double  flapCone     = 0.6;                                                                                            
	double  flapWave     = 0.3;                                 
	double  flapWaveRandomness = 0.0;
	double  flapCurlFoldRandomness = 0.0;
	double  flapRagged   = 0.5;                                         

	                                                                            
	bool    liftsEnable  = true;
	int32_t liftAmount   = 4;                                                                               
	double  liftLengthMin = 10.0;
	double  liftLengthMax = 35.0;
	double  liftWidthMin  = 1.5;
	double  liftWidthMax  = 5.0;
	double  liftCurl      = 140.0;            

	                                                                            
	                                                                            
	                                                                         
	                                                                             
	                                                                         
	bool    paintEnable  = true;
	double  paintReach   = 5.0;                                                             
	double  paintJagged  = 0.35;                                            
	double  seamSnap     = 4.0;
};
