// параметры: вскрываем стену и размещаем кирпичи за её наружным слоем
// ProjectDamage brick damage — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Brick Damage effect (recipe: brick-damage-v1):
// render (stucco / plaster) broken off a brick wall to show the brick courses
// behind it, with bricks knocked out brick by brick, broken bricks along the
// stepped rim, loose bricks and, optionally, holes through the wall.
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `BrickDamageParams brick;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon,
// the same units as every other ProjectDamage size); shares/chances are 0..1.
#pragma once
#include "damage_point.h"

#include <cstdint>
#include <string>

// исходные значения и ограничения отдельных частей эффекта
struct BrickDamageParams
{
    double edgeWallDepth = 0.0;
	bool organic = false;
    std::vector<SurfaceDamagePoint> proximity;
    bool splitMaterials = true;
	bool    enable = false;
    bool generatedGeometry = true;
	int32_t seed   = 0;                                                                            
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 1.0;                                                                         

	                                                                             
	int32_t amount    = 3;                                                         
	double  sizeMin   = 35.0;                         
	double  sizeMax   = 70.0;
	int32_t lobes     = 4;                                                                 
	double  stretch   = 1.6;                                             
	double  edgeBias  = 0.2;                                                              

	                                                                             
	                                                                          
	                                                                         
	double  paintRadius = 15.0;                                                  
	double  paintSnap   = 5.0;                                                     
	double  paintClose  = 0.15;                                                              

	                                                                                
	                                                                      
	                                                                        
	                                                  
	double  renderMin    = 1.5;
	double  renderMax    = 3.0;
	double  jagged       = 0.6;                                            
	double  outlineScale = 1.0;                                                     
	double  crumble      = 0.5;                                                        
	double  crumbleSize  = 1.5;                                                       
	                                                                           
	                                                                           
	                                                                         
	int32_t profile     = 0;
	double  smoothWidth = 0.75;
	int32_t terraces     = 2;                                
	double  terraceSkin  = 0.45;                                                     
	double  terraceInset = 0.06;                                                               
	double  wallSoft     = 0.05;                                                        
	double  roughness    = 0.3;                                                   
	double  renderStick  = 0.15;                                                                     
	double  thicknessCap = 0.85;                                                                 
	                                                                          
	                                                                           
	                                                                        
	                                                                          
	                                                                         
	                                                                          
	                                                        
	bool    sharpEdge    = true;

	                                                                                     
	                                                                      
	                                      
	                                                                       
	                                                                        
	                                                                    
	                                                                        
	                                                                            
	                                                                           
	                                                        
	int32_t style = 0;

	                                                                                    
	                                                                         
	                                                         
	int32_t bond         = 0;                                                                       
	double  brickLength  = 25.0;                           
	double  brickHeight  = 6.5;                                          
	double  brickWidth   = 12.0;                                                                                          
	double  mortar       = 1.0;                                 
	double  mortarDepth  = 0.6;                                                   
	double  brickScale   = 1.0;                                                               
	double  bondOffset   = 0.5;                                                                       

	                                                                                    
	double  faceJitter   = 0.25;                                         
	double  faceTilt     = 0.6;                                   
	double  brickRough   = 0.35;                           
	double  chip         = 0.6;                                            
	double  chipSize     = 1.2;                                            
	double  jointRough   = 0.5;                                       

	                                                                                      
	                                                                          
	                                                                      
	                                                                     
	                                                                       
	int32_t layers       = 2;                                                              
	                           
	                                                                         
	                                                                        
	                                                                           
	                                                                             
	                                                                            
	                                                                             
	                                                                              
	                                                                      
	                        
	                                                                         
	                               
	int32_t brickMode    = 1;
	                                         
	                                                                                
	                                                                           
	                                                                            
	                                                                         
	                                                                           
	                                                                           
	int32_t behind       = 1;
	double  damageDepth  = 10.0;                                                        
	double  damageNoise  = 1.5;                                                                               
	double  damageScale  = 8.0;                                 
	                    
	                                                                         
	                                                                            
	                                                                        
	                                                                  
	bool    corners      = true;
	                                                                                    
	                                                                           
	bool    jointFill    = true;
	                                                                          
	                                                                             
	                                                                    
	bool    remesh       = true;
	double  remeshSize   = 2.0;                                                       
	double  breakSize    = 0.55;                                                                
	double  layerInset   = 0.18;                                                                 
	double  breakJagged  = 0.5;                                                                
	double  missing      = 0.03;                                                             
	double  broken       = 0.4;                                                                  
	double  brokenRough  = 0.5;                                                
	double  collarFill   = 0.35;                                                                               

	                                                                                       
	double  loose        = 0.08;                                            
	double  looseDepth   = 2.0;                                              
	double  looseTilt    = 5.0;                                         
	double  looseOut     = 0.3;                                                               
	double  looseOutDist = 3.0;                                                        

	                                                                                         
	                                                           
	                                                                         
	                                                                       
	                                                                         
	                                                                          
	                                                                                                                                
	                                                                        
	                                                                        
	                                                                            
	                                                
	bool    useModel  = false;                  
	bool    modelFlip = false;
	double  modelTurn = 0.5;                                                                                                  

	                                                                               
	bool    holeEnable  = false;
	double  holeChance  = 0.6;                                                              
	double  holeSize    = 0.35;                                                                  

	                                                                                
	bool    crackEnable    = true;
	int32_t crackAmount    = 3;                                                       
	double  crackLengthMin = 10.0;
	double  crackLengthMax = 35.0;
	double  crackWidth     = 0.35;                            
	double  crackDepth     = 1.0;
	double  crackMeander   = 0.45;                  
	double  crackBranch    = 0.35;                                  
	double  crackInterior  = 0.0;                                                  

	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                              
	                                                                          
	                                                                    
	                                                                      
	                                                                     
	                                       
	int32_t surfMode    = 0;
	std::string texPath;                                                               
	double  texScale    = 20.0;
	double  texStrength = 1.0;                                        
	double  texOffX = 0.0, texOffY = 0.0, texOffZ = 0.0;
	double  texEdge     = 0.0;                       
	                                                                       
	                                                                   
	                                                                    
	                                                                
	double  noiseScale = 10.0, noiseDetail = 4.0, noiseRoughness = 0.5, noiseDistortion = 0.0, noiseStrength = 0.0;
};
