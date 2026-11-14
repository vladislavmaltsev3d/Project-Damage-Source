// параметры: создаём повреждения дерева вдоль волокон
// ProjectDamage wood damage — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Wood Damage effect (recipe: wood-damage-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `WoodDamageParams wood;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon,
// the same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct WoodDamageParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                            
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 1.0;                                           

	                                                                               
	                                                                           
	                                                                      
	                                                             
	int32_t grainMode = 0;
	double  grainX = 0.0, grainY = 0.0, grainZ = 1.0;

	                                                                             
	int32_t amount    = 4;                                                              
	double  sizeMin   = 6.0;                                                 
	double  sizeMax   = 14.0;
	int32_t lobes     = 3;                                          
	double  stretch   = 2.5;                                              
	double  endBias   = 0.5;                                                            
	double  edgeBias  = 0.2;                                                     

	                                                                             
	                                                                          
	                                                                        
	double  paintRadius = 4.0;                                                   
	double  paintSnap   = 5.0;                                                     
	double  paintClose  = 0.15;                                                              

	                                                                                 
	double  depthMin     = 0.6;                                            
	double  depthMax     = 1.6;
	int32_t layers       = 3;                                                                  
	double  layerInset   = 0.6;                                                                              
	double  fiberDepthVar = 0.5;                                                          
	double  thicknessCap = 0.6;                                                
	double  jagged       = 0.5;                                     

	                                                                                  
	double  fiberWidth    = 1.6;                                              
	double  fiberJitter   = 0.7;                                       
	double  splinterLength = 6.0;                                                  
	double  splinterSharp = 0.8;                                             
	double  splinterChunk = 0.5;                                                                
	double  splinterRamp  = 0.5;                                                    
	double  floorFibers   = 0.5;                                          

	                                                                                  
	                                                                           
	                                                                       
	bool    grooveEnable  = true;
	double  grooveSpacing = 0.45;                                        
	double  grooveDepth   = 0.12;                
	double  grooveWave    = 0.5;                                               
	double  grooveBreakup = 0.5;                                                    
	double  weatherHalo   = 0.5;                                                                

	                                                                                    
	bool    breakEnable = true;
	double  breakChance = 0.35;                                                              
	double  breakSize   = 0.5;                                                            

	                                                                                      
	bool    checkEnable    = true;
	int32_t checkAmount    = 3;                                                                  
	int32_t checkGlobal    = 6;                                                                     
	double  checkLengthMin = 8.0;
	double  checkLengthMax = 35.0;
	double  checkWidth     = 0.25;                            
	double  checkDepth     = 0.8;
	double  checkMeander   = 0.15;                                       

	                                                                                      
	int32_t knotAmount  = 2;                                           
	double  knotSize    = 1.2;                               
	double  knotStretch = 1.4;                                  
	double  knotDepth   = 1.0;
	int32_t knotChecks  = 2;                                       

	                                                                                         
	                                                                            
	                                     
	bool    sliverEnable = true;
	double  sliverAmount = 0.3;                                                    
	double  sliverLength = 4.0;
	double  sliverWidth  = 0.5;                                
	double  sliverThick  = 0.18;
	double  sliverLift   = 0.35;                                                   
	double  sliverEmbed  = 0.8;                                                  
};
