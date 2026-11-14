// параметры: создаём выбоины бетона и открываем внутреннюю арматуру
// ProjectDamage concrete damage — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Concrete Damage effect (recipe: concrete-damage-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `ConcreteDamageParams concrete;` member
// without any include cycle. All lengths are scene units (cm in the Blender
// addon, the same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once
#include "damage_point.h"

#include <cstdint>
#include <string>

// исходные значения и ограничения отдельных частей эффекта
struct ConcreteDamageParams
{
    double edgeWallDepth = 0.0;
    double rebarResolution = 1.0;

	bool organic = false;
    std::vector<SurfaceDamagePoint> proximity;
	bool    enable = false;
    bool generatedGeometry = true;
	int32_t seed   = 0;                                                                            
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 1.0;                                           

	                                                                             
	int32_t amount    = 5;                                                               
	double  sizeMin   = 12.0;                         
	double  sizeMax   = 26.0;
	int32_t lobes     = 4;                                                                 
	double  stretch   = 1.6;                                             
	double  edgeBias  = 0.25;                                                             

	                                                                             
	                                                                          
	                                                                         
	double  paintRadius = 6.0;                                                   
	double  paintSnap   = 5.0;                                                     
	double  paintClose  = 0.15;                                                              

	                                                                             
	double  depthMin    = 1.5;                                                 
	double  depthMax    = 4.0;
	double  jagged      = 0.6;                                             
	double  outlineScale = 1.0;                                                     
	int32_t terraces    = 2;                                                           
	double  terraceSkin = 0.3;                                                     
	double  terraceInset = 0.18;                                                           
	double  wallSoft    = 0.15;                                                  
	double  roughness   = 0.6;                                         
	double  aggregateSize = 0.8;                                      
    double surfaceDistortion = 0.0;                                        
    double surfaceRoughness = 0.45;                                       
    double surfaceStrength = 1.0;                                            
	double  surfaceDetail = 3.0;                                                                              
	double  thicknessCap = 0.45;                                                    
	                                                                         
	                                                                       
	                                                               
	                                                                     
	                                                                         
	                                                                        
	                                                               
	int32_t profile     = 0;
    double woodThickness=2.0, woodWidth=12.0, woodSplinter=6.0, woodAngle=0.0;
	double  smoothWidth = 0.75;                                                  
	int32_t surfMode    = 0;
	std::string texPath;                                                               
	double  texScale    = 20.0;                                 
	double  texStrength = 1.0;                                        
	double  texOffX = 0.0, texOffY = 0.0, texOffZ = 0.0;
	double  texEdge     = 0.0;                                                       

	                                                                               
	bool    holeEnable  = false;
	double  holeChance  = 0.6;                                                              
	double  holeSize    = 0.45;                                                  
	double  holeJagged  = 0.5;                     

	                                                                               
	bool    crackEnable    = true;
	int32_t crackAmount    = 4;                                                       
	double  crackLengthMin = 6.0;
	double  crackLengthMax = 22.0;
	double  crackWidth     = 0.35;                            
	double  crackDepth     = 1.0;
	double  crackMeander   = 0.45;                  
	double  crackBranch    = 0.35;                                  
	double  crackInterior  = 0.3;                                                        

	                                                                                
	                                                                         
	                                                                       
	                                                                   
	bool    rebarEnable   = true;
	double  rebarAmount   = 0.85;                                              
	double  rebarDensity  = 1.0;                                                         
	double  rebarRadius   = 0.6;
	double  rebarScale    = 1.0;                                                         
	double  rebarSpacingX = 10.0;                                                    
	double  rebarSpacingY = 12.0;                                                      
	bool    rebarBarsX    = true;                    
	bool    rebarBarsY    = true;                  
	int32_t rebarOuter    = 0;                                                                
	double  rebarAngle    = 0.0;                                                            
	double  rebarCover    = 2.0;                                                            
	double  rebarExpose   = 0.7;                                                      
	double  rebarEmbed    = 4.0;                                                
	double  rebarRibs     = 0.15;                                          
	int32_t rebarSides    = 8;                                  
	double  rebarJitter   = 0.3;                                             
	double  rebarWobble   = 0.2;                                 
	double  rebarBend     = 0.3;                                                       
	double  rebarBroken   = 0.35;                                                 
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                        
};
