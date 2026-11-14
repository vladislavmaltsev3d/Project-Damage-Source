// параметры: строим трещины, плиты и выбоины на грунте
// ProjectDamage ground cracks — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Ground Cracks effect (recipe: ground-cracks-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `GroundCracksParams ground;` member
// without any include cycle. All lengths are scene units (cm in the Blender
// addon, the same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct GroundCracksParams
{
	bool    enable = false;
    bool generatedGeometry = true;
	int32_t seed   = 0;                                                                            
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 1.0;                                                         
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                                       
	double  maxSlope = 35.0;                                                                           

	                                                                             
	                                                                       
	                                                                               
	int32_t amount        = 4;                                                      
	double  sizeMin       = 80.0;                                                          
	double  sizeMax       = 180.0;
	double  areaChance    = 0.6;                                                           
	double  areaSize      = 0.35;                                
	double  areaStretch   = 1.5;                                                      

	                                                                             
	                                                                             
	                                                                      
	double  paintSnap   = 10.0;                                                        
	double  paintClose  = 0.15;                                                              
	double  paintJitter = 1.2;                                                                            
	double  paintBranch = 1.0;                                             

	                                                                                 
	bool    crackEnable      = true;
	int32_t crackAmount      = 6;                                                                                 
	int32_t crackGenerations = 2;                                                                      
	double  crackLengthMin   = 40.0;                                
	double  crackLengthMax   = 140.0;
	double  crackWidth       = 0.6;                                                      
	double  crackWidthVar    = 0.5;                                    
	double  crackTaper       = 0.85;                                            
	double  crackDepth       = 2.0;
	double  crackMeander     = 0.45;                                                  
	double  crackKink        = 0.5;                                                                  
	double  crackBranch      = 3.0;                                                       
	double  crackJoin        = 0.9;                                                                    
	double  crackChip        = 0.6;                                                            

	                                                                                 
	double  areaJagged       = 0.6;                                                                        
	double  rimFade          = 0.35;                                                                         
	double  plateSize        = 14.0;                               
	double  plateCenterScale = 0.45;                                                                             
	double  plateJagged      = 0.5;                                    
	double  gapWidth         = 1.2;                                                          
	double  gapDepth         = 3.0;
	double  plateSink        = 1.0;                          
	double  plateTilt        = 0.8;                                                       

	                                                                                     
	double  potholeChance = 0.5;                                                                   
	double  potholeSize   = 0.55;                                                  
	double  potholeLoose  = 0.35;                                                                           
	double  potholeDepth  = 6.0;
	double  roughness     = 0.6;                                                  
	double  aggregateSize = 1.2;                             
	double  thicknessCap  = 0.7;                                                        

	                                                                                        
	bool    rubbleEnable  = true;
	int32_t rubbleAmount  = 8;                          
	double  rubbleSizeMin = 2.0;
	double  rubbleSizeMax = 8.0;

	                                                                                       
	double  heaveChance = 0.15;                                                                
	double  heaveHeight = 4.0;                         
	double  heaveWidth  = 45.0;                                                
	double  heaveGap    = 1.5;                                       
};
