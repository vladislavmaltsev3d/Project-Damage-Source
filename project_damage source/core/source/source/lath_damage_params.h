// параметры: строим повреждённую штукатурку, дранку и деревянный каркас
// ProjectDamage lath damage — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Lath Damage effect (recipe: lath-damage-v1):
// plaster broken off a wall to show the wooden lath strips behind it.
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `LathDamageParams lath;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon,
// the same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once
#include "damage_point.h"

#include <cstdint>
#include <string>

// исходные значения и ограничения отдельных частей эффекта
struct LathDamageParams
{
    double edgeWallDepth = 0.0;
	bool organic = false;
    std::vector<SurfaceDamagePoint> proximity;
	bool    enable = false;
    bool generatedGeometry = true;
	int32_t seed   = 0;                                                                            
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 1.0;                                           

	                                                                             
	int32_t amount    = 3;                                                         
	double  sizeMin   = 20.0;                         
	double  sizeMax   = 40.0;
	int32_t lobes     = 4;                                                                 
	double  stretch   = 1.8;                                             
	double  edgeBias  = 0.1;                                                              

	                                                                             
	                                                                          
	                                                                         
	double  paintRadius = 8.0;                                                   
	double  paintSnap   = 5.0;                                                     
	double  paintClose  = 0.15;                                                              

	                                                                               
	                                                                        
	                                            
	double  plasterMin   = 1.5;                                                 
	double  plasterMax   = 2.5;
	double  jagged       = 0.6;                                            
	double  outlineScale = 1.0;                                                     
	double  crumble      = 0.6;                                                          
	double  crumbleSize  = 1.5;                                                                       
	                                                                           
	                                                                           
	                                                                         
	int32_t profile     = 0;
	double  smoothWidth = 0.75;
	int32_t terraces     = 2;                                                              
	double  terraceSkin  = 0.35;                                                      
	double  terraceInset = 0.12;                                                               
	double  wallSoft     = 0.05;                                                         
	                                                                         
	                                                                         
	                                                                     
	                                                                        
	                                                                     
	                                                                        
	double  rimSmooth    = 0.5;
	                                                                           
	                                                                          
	                                                                        
	                                                                        
	                                                            
	double  wallSlope    = 72.0;
	double  roughness    = 0.4;                                                    
	double  thicknessCap = 0.7;                                                                  

	                                                                               
	double  cavityDepth = 3.0;                                                            
	double  keys        = 0.35;                                                          
	double  keySize     = 2.5;                                      
	                                                                          
	                                                                         
	                                                                        
	                                                        
	double  cavitySmooth = 0.6;
	                                                                       
	                                                                        
	                                                                            
	double  cavitySlope  = 40.0;

	                                                                               
	bool    holeEnable  = false;
	double  holeChance  = 0.6;                                                              
	double  holeSize    = 0.45;                                                  
	double  holeJagged  = 0.6;                                                          
	                                                                 
	                                                                     
	                                                                        
	                                                                        
	                                                                      
	                                                                      
	                       
	double  holeNoiseScale = 0.3;                                                         
	double  holeDetail  = 0.35;                                                                     
	double  holeLaths   = 0.15;                                                                           

	                                                                                
	bool    crackEnable    = true;
	int32_t crackAmount    = 3;                                                       
	double  crackLengthMin = 6.0;
	double  crackLengthMax = 20.0;
	double  crackWidth     = 0.3;                             
	double  crackDepth     = 0.8;
	double  crackMeander   = 0.45;                  
	double  crackBranch    = 0.35;                                  
	double  crackInterior  = 0.0;                                                  

	                                                                                  
	                                                                           
	                                                                         
	                                                                  
	bool    lathEnable    = true;
	double  lathAmount    = 1.0;                                                 
	double  lathWidth     = 3.5;                            
	double  lathThickness = 0.8;                                     
	double  lathGap       = 0.9;                                                             
	double  lathScale     = 1.0;                                                        
	double  lathAngle     = 0.0;                                                            
	double  lathLength    = 120.0;                                                                
	double  lathEmbed     = 6.0;                                                     
	double  lathJitter    = 0.4;                                                             
	double  lathTilt      = 0.6;                                                        
	double  lathWarp      = 0.25;                                                
	double  lathEdge      = 0.4;                                                    
	double  lathMissing   = 0.06;                                                                   
	double  lathBroken    = 0.15;                                                            
	double  lathBend      = 1.2;                                                         
	double  lathSplinter  = 1.2;                                                    
	                                                                          
	                                                                          
	                                                                      
	                                                                         
	                                                                    
	                                         
	double  lathOptimize  = 0.5;
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                            
	                                                                                     
	                                                                      
	                                                                       
	                                                                        
	                                                                          
	                                                                          
	                                                                      
	                                                                          
	                                                                       
	                                                             
	int32_t style          = 0;                                                                               
	double  rivenWave      = 0.8;                                                
	double  rivenWaveLength = 35.0;                                      
	double  rivenWidthVar  = 0.35;                                                       
	double  rivenTwist     = 4.0;                                                    
	double  rivenRough     = 0.6;                                                       
	bool    studEnable     = true;                                                             
	double  studSpacing    = 40.0;                                                     
	double  studWidth      = 5.0;                                
	double  studDepth      = 9.0;                                                        
	int32_t boardStuds     = 3;                                                   
	int32_t jointBundle    = 6;                                                      
	double  lathHang       = 0.35;                                                                 
	                                                                                         
	                                                                         
	                                                                       
	                                                                         
	                                                                       
	                                                                       
	                                                                  
	                                                                    
	double  nogginSpacing  = 60.0;                                          
	bool    framePlates    = true;                                           
	bool    doublePlate    = true;                      
	double  nailAmount     = 0.6;                                                       
	double  nailLength     = 1.0;                                                            
	                                                                                                   
	                                                                          
	                                                                 
	int32_t surfMode    = 0;
	std::string texPath;                                                               
	double  texScale    = 20.0;                                 
	double  texStrength = 1.0;                                        
	double  texOffX = 0.0, texOffY = 0.0, texOffZ = 0.0;
	double  texEdge     = 0.0;                                                  
	                                                                       
	                                                                   
	                                                                    
	                                                                
	double  noiseScale = 10.0, noiseDetail = 4.0, noiseRoughness = 0.5, noiseDistortion = 0.0, noiseStrength = 0.0;
};
