// параметры: строим пробой стекла, острые края и расходящиеся трещины
// ProjectDamage broken glass — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Broken Glass effect (recipe: broken-glass-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `BrokenGlassParams glass;` member without
// any include cycle. All lengths are scene units (cm in the Blender addon, the
// same units as every other ProjectDamage size); shares/chances are 0..1. "x radius"
// means a multiple of the opening radius of that impact (for a drawn hole: the
// radius of the largest circle that fits inside the contour).
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct BrokenGlassParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                            

	                                                                              
	                                                                         
	                                                                   
	                                                                        
	                                                                          
	                                                                       
	                                                                         
	                                                                           
	                                         
	int32_t thicknessLogic = 1;
	                                                                           
	                                                                   
	                                                                       
	                                                                          
	                            
	double  thickness = 0.0;
	                                                                         
	                                                                       
	                                                                         
	                                                                        
	double  skinDepth = 0.0;
	                                                                        
	                                                                       
	                                                                      
	                                                          
	bool    autoHole = true;
	                                                                         
	                                                       
	double  autoHoleThickness = 0.0;
	double  cutMargin = 0.75;                                                          
	double  detail    = 1.0;                                                     
	bool    avoid     = true;                                  

	                                                                                
	bool    impactEnable   = true;
	int32_t impactAmount   = 1;                                                           
	double  holeChance     = 0.85;                                                                             
	double  holeSizeMin    = 10.0;                    
	double  holeSizeMax    = 24.0;
	double  holeLobes      = 0.4;                                                             
	double  impactSpacing  = 1.0;                                                             

	                                                                                
	int32_t radialMin      = 9;                                                                    
	int32_t radialMax      = 16;
	double  radialJitter   = 0.6;                                           
	double  spikeChance    = 0.8;                                                                
	double  spikeLength    = 0.75;                              
	double  spikeWidth     = 0.45;                                                                  
	double  spikeSplit     = 0.35;                                                                
	double  spikeLean      = 0.5;                                                    
	double  notchJitter    = 0.3;                                                                       
	double  blowout        = 0.15;                                                            
	double  blowoutReach   = 0.7;                                 
	double  edgeKink       = 0.12;                                                          
	double  shardCrack     = 0.5;                                                

	                                                                                
	bool    crackEnable      = true;
	double  crackWidth       = 0.08;
	double  crackDepth       = 0.6;                                            
	bool    crackBothSides   = true;                                                
	double  crackReach       = 2.6;                                                          
	double  crackReachJitter = 0.45;
	double  crackMeander     = 0.12;                                     
	double  crackBranch      = 0.4;                                 
	double  crackTaper       = 0.3;                             
	double  crushSize        = 0.05;                                                             
	int32_t ringCount        = 4;                         
	double  ringFirst        = 0.3;                                                     
	double  ringGrowth       = 1.55;                                        
	double  ringFill         = 0.65;                                      
	                                                                                
	                                                                     
	                                                                         
	                                                                           
	                                                                         
	                                                                       
	                                                                    
	bool    through         = false;                                                
	double  throughGap      = 0.0;                                                                              
	int32_t throughRings    = 2;                                                   
	double  throughFall     = 0.0;                                              
	double  throughMinPiece = 0.0002;                                           
	                                                                           
	                                                                                
	                                                                           
	                                                                    
	                                                                       
	                                                                           
	                                                                          
	bool    fillHole   = false;
	int32_t fillCount  = 40;                                       
	bool    fillOriginEnabled = false;
	double  fillOriginX = 0.0, fillOriginY = 0.0, fillOriginZ = 0.0;
	double  fillCenter = 0.7;                                                   
	double  ringJitter       = 0.25;
	double  ringBow          = 0.08;                                                          

	                                                                                
	bool    chipEnable  = true;
	double  chipChance  = 0.6;
	double  chipSpacing = 1.5;                                                              
	double  chipSize    = 0.8;                                 
	double  chipDepth   = 0.45;                                                           

	                                                                               
	                                                                          
	                                                                      
	                                                                 
	                                                                            
	                                                                         
	                        
	bool    paintHolesEnable   = true;
	bool    paintImpactsEnable = true;
	bool    paintOnly          = false;                                                         
	double  paintShardWidth    = 5.0;                                                   
	bool    paintHoleCracks    = true;                                             
	double  paintPointSize     = 2.0;                                                    
	double  paintImpactSize    = 8.0;                                             
	bool    paintPointHole     = true;                                                                 

	int32_t maxCracks = 4000;                                                        
};
