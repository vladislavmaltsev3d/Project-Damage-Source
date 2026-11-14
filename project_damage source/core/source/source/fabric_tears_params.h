// параметры: общие расчёты разрывов ткани и её нитей
// ProjectDamage fabric tears — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Fabric Tears effect (recipe: fabric-tears-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `FabricTearParams fabric;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon, the
// same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct FabricTearParams
{
	bool    enable = false;
    bool wovenThreads = true;                                                         
	int32_t seed   = 0;                                                                            
	                                                                    
	                                                                           
	                                           
	bool    flat = true;
	                                                                           
	                                                                             
	                                                                       
	                                                             
	bool    solid = false;
	                                                                         
	                                                                       
	                                                                      
	                                                                      
	double  solidBevel = 0.8;
	int32_t solidBevelSegments = 3;
	                                                                  
	                                                                    
	                                                                           
	                                                                             
	                                                                         
	                                                                
	int32_t tearStyle   = 2;
    double distributionScale=30.0, distributionStrength=0.8;
    double shapeNoiseScale=6.0, shapeNoiseStrength=0.65;
	double  tearRip     = 0.40;                                                   
	double  tearCorner  = 0.25;
	double  tearWorn    = 0.25;
	double  tearPuncture = 0.10;
	double  grainSnap   = 0.85;                                                                                
	double  yarnSize    = 0.25;                                                               
	double  flapLift    = 0.6;                                                                                        
	                                                                       
	                                                                         
	                                                                         
	                                                                          
	                                                                         
	double  layerDepth = 2.0;

	                                                                            
	                                                                         
	                                                                         
	                                                                          
	                                                                  
	double  thickness = 0.0;
	                                                                          
	                                                                          
	                                                                          
	                                                            
	double  skinDepth = 0.0;
	double  cutMargin = 0.75;                                                                       
	double  detail    = 1.0;                                                  
	bool    avoid     = true;                                  

	                                                                            
	bool    holesEnable    = true;
	int32_t holeAmount     = 5;                                                                 
	double  holeLengthMin  = 8.0;                                                 
	double  holeLengthMax  = 24.0;
	double  holeWidthMin   = 2.5;                                         
	double  holeWidthMax   = 7.0;
	double  holeJagged     = 0.55;                                             
	double  holeLobes      = 0.45;                                                      
	double  holeMeander    = 0.25;                                       
	int32_t holeSplitMax   = 3;                                                       
	double  holeBridge     = 1.5;                                                           
	int32_t holeDirMode    = 1;                                                         
	double  holeDirX = 1.0, holeDirY = 0.0, holeDirZ = 0.0;
	double  holeDirJitter  = 15.0;             

	                                                                             
	bool    edgeEnable       = true;
	int32_t edgeAmount       = 14;                                                       
	double  edgeSpacingJitter = 0.6;                                                
	double  edgeLengthMin    = 8.0;                                       
	double  edgeLengthMax    = 35.0;
	double  edgeWidthMin     = 0.3;                                                     
	double  edgeWidthMax     = 3.0;
	double  edgeTaper        = 1.0;                                                         
	double  edgeJagged       = 0.5;
	double  edgeMeander      = 0.2;
	double  edgeAngleJitter  = 12.0;                                        

	                                                                             
	bool    threadsEnable  = true;
	double  threadRadius   = 0.06;
	int32_t threadSides    = 6;                                   
	double  threadSpacing  = 0.35;                                                            

	bool    bridgeEnable   = true;                                                                       
	double  bridgeShare    = 0.35;                                           
	double  threadSag      = 0.12;                                      
	double  threadSkew     = 0.15;                                           
	double  threadWobble   = 0.25;                      

	bool    brokenEnable   = true;                                                
	double  brokenShare    = 0.35;                                         
	double  threadDroop    = 0.6;                                              
	double  threadCurl     = 0.4;                                          
	double  threadTaper    = 0.45;                                                 

	bool    frayEnable     = true;                                        
	double  fraySpacing    = 0.25;
	double  frayLengthMin  = 0.2;
	double  frayLengthMax  = 0.9;

	bool    hangEnable     = true;                                                  
	int32_t hangAmount     = 20;
	double  hangLengthMin  = 3.0;
	double  hangLengthMax  = 14.0;

	                                                                             
	                                                                        
	                                                                           
	                                                                           
	                                                                        
	                              
	bool    paintHolesEnable  = true;
	bool    paintEdgesEnable  = true;
	bool    paintOnly         = false;                                                        
	double  paintJagged       = 0.5;                                     
	double  paintRagSize      = 1.2;                                                   
	double  paintEdgeSpacing  = 1.8;                                                             
	double  paintEdgeLengthMin = 3.0;
	double  paintEdgeLengthMax = 14.0;
	double  paintEdgeWidthMin = 0.3;
	double  paintEdgeWidthMax = 1.6;
	double  paintHangSpacing  = 0.8;                                                                 

	double  gravityX = 0.0, gravityY = 0.0, gravityZ = -1.0;                  
	int32_t maxThreads = 6000;                                   
};
