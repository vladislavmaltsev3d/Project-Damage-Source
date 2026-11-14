// параметры: добавляем слои штукатурки, следы инструмента и мелкий рельеф
// ProjectDamage plaster displacement — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Plaster Displacement effect (recipe:
// plaster-displace-v1). Plain data with no ProjectDamage/Manifold includes, so
// project_damage_core.h can include it ahead of ProjectDamageParams and carry one
// `PlasterDisplaceParams plaster;` member without any include cycle. All
// lengths are scene units (cm in the Blender addon, the same units as every
// other ProjectDamage size); shares/chances are 0..1.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct PlasterDisplaceParams
{
                                                                           
    std::vector<double> zoneWeights;
	bool    enable = false;
	int32_t seed   = 0;                                                                            
	bool    avoid  = true;                                                       

	                                                                               
	                                                                        
	                                                                     
	                                                                       
	                                                            
	double  edgeLength = 0.35;                                          
	int32_t maxTris    = 1500000;                                         
	double  detail     = 1.0;                                                       

	                                                                               
	double  strength   = 0.6;                                                    
	double  sink       = 0.5;                                                      
	double  thicknessCap = 0.2;                                                        

	                                                                               
	double  undulation = 0.25;                                                   
	double  undulationScale = 45.0;                                 

	                                                                               
	                                                                     
	                                                                          
	                                                                             
	int32_t coats       = 3;                                               
	double  coverage    = 1.1;                                         
	double  lengthMin   = 10.0;                                                  
	double  lengthMax   = 28.0;
	double  widthMin    = 5.0;                                           
	double  widthMax    = 12.0;
	double  angle       = 0.0;                                                                  
	double  angleRandom = 0.3;                                          
	double  crossChance = 0.25;                                                              
	double  curve       = 0.3;                                              
	double  flatten     = 0.85;                                                       
	double  tilt        = 0.35;                                                           
	double  jagged      = 0.5;                                                         
	double  streaks     = 0.3;                                                   

	                                                                                
	double  ridgeHeight = 0.45;               
	double  ridgeWidth  = 0.6;                                          
	double  ridgeSharp  = 0.7;                                                    
	double  ridgeTrail  = 0.85;                                                         

	                                                                               
	double  grain       = 0.04;                                  
	double  grainSize   = 0.3;                                                    
	double  pits        = 0.3;                                                       

	                                                                               
	                                                                        
	                                                      
	bool    crackEnable = true;
	double  crackAmount = 0.45;                                                   
	double  crackScale  = 40.0;                            
	double  crackWidth  = 0.3;                                                               
	double  crackDepth  = 0.25;
	double  crackMeander = 0.5;                         
	double  crackBreakup = 0.4;                                                   
	double  crackStep   = 0.06;                                                   
	double  crackLip    = 0.4;                                                            

	                                                                          
	double  upX = 0.0, upY = 0.0, upZ = 1.0;
};
