// параметры: строим бетонный пробой и трещины вокруг точки удара
// ProjectDamage concrete hit — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Concrete Hit effect (recipe: concrete-hit-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `ConcreteHitParams hit;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon, the
// same units as every other ProjectDamage size); shares/chances are 0..1. Values marked
// "x radius" are fractions of the hit radius the empty sets.
#pragma once

#include <cstdint>
#include <vector>

                                                                           
                                                                     
                                                        
                                                   
                                                                                
                                                                       
                                                                       
                                                          
                                                                            
                                                                           
                                                                 
struct ConcreteHitEmpty
{
	double rel[12] = { 0.0, 0.0, 0.0,  1.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 0.0, 1.0 };
	double size     = 20.0;
	double strength = 1.0;                                                
};

// исходные значения и ограничения отдельных частей эффекта
struct ConcreteHitParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                            
	bool    avoid  = true;                                     
	double  detail = 1.0;                                                                       
	std::vector<ConcreteHitEmpty> empties;

	                                                                               
	int32_t aim         = 0;                                               
	                                                                                                
	double  radiusScale = 1.0;                                     
	double  proximity   = 1.5;                                                       
	                                                                               
	double  proxShrink  = 0.6;                                                                    
	double  oblique     = 0.5;                                                           
	                                                                     

	                                                                              
	double  layerThickness = 0.5;                                 

	                                                                               
	double  coreSize      = 0.32;                                  
	double  coreDepth     = 0.12;                                           
	double  coreJagged    = 0.7;                                             
	double  roughness     = 0.7;                                          
	double  aggregateSize = 0.6;                             
	double  thicknessCap  = 0.45;                                                       
	bool    holeEnable    = false;                           
	double  holeSize      = 0.45;                                      

	                                                                                
	double  plateSize   = 0.5;                                                  
	double  plateGrowth = 0.5;                                                
	double  zoneJagged  = 0.35;                                                 
	double  shatter     = 0.55;                                                             
	double  tilt        = 2.0;                                                              
	double  edgeJagged  = 0.5;                                            
	double  chip        = 0.4;                                             

	                                                                               
	bool    crackEnable    = true;
	double  crackWidth     = 0.22;                            
	double  crackDepth     = 0.6;                                    
	int32_t radialCount    = 6;                                                      
	double  radialLengthMin = 0.35;             
	double  radialLengthMax = 1.1;              
	double  radialMeander  = 0.45;                  
	double  radialBranch   = 0.35;                                         
	int32_t ringCount      = 1;                                                
	double  ringFill       = 0.45;                                             
	                                                                                
	                                                                         
	                                                                       
	                                                                      
	                                                                         
	                                                                  
	bool    through         = false;                                                
	double  throughGap      = 0.15;                                      
	double  throughMaxWall  = 40.0;                                          
	double  chunkDepth      = 0.35;                                          
	double  throughRemove   = 0.0;                                              
	double  throughMinPiece = 0.00001;                                           
	                                                                             
	                                                                              
	                                                                               
};
