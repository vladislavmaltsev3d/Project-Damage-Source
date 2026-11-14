// параметры: создаём ржавые отверстия и неровные края вокруг них
// ProjectDamage rusty holes — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Rusty Holes effect (recipe: rusty-holes-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h
// can include it ahead of ProjectDamageParams. All lengths are scene units (cm
// in the Blender addon); shares/chances are 0..1. "x size" means a multiple of
// the hole cluster's radius.
//
// Only corroded-through holes are cut: no crust, no dents, no displacement.
// Geometry is added only along the hole outlines (one straight wall per
// outline edge, `thickness` deep, open behind), so the face count stays low.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct RustyHoleParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                 
	int32_t mode   = 0;                                                         
	bool    avoid  = true;                                     
	double  detail = 0.8;                                                          

	                                                                                   
	int32_t amount     = 5;                      
	double  sizeMin    = 1.5;                             
	double  sizeMax    = 5.0;
	double  sizeBias   = 0.5;                                                
	double  minSpacing = 0.8;                                                                    

	                                                                                     
	int32_t lobes      = 4;                                                            
	double  lobeSpread = 0.55;                                               
	double  jagged     = 0.7;                                              
	double  roughness  = 0.5;                                     
	double  scallop    = 1.4;                                                            
	double  merge      = 0.35;                                                       
	double  lace       = 0.6;                                                                    

	                                                                                     
	int32_t pitAmount  = 28;                                       
	double  pitSize    = 0.14;                                     
	double  pitSpread  = 2.4;                                        

	                                                                                       
	int32_t crackAmount  = 2;                                  
	double  crackLength  = 0.9;           
	double  crackWidth   = 0.15;                                               
	double  crackMeander = 0.7;
	double  crackFork    = 0.25;                           
	double  crackPits    = 0.35;                                       

	                                                                                   
	                                                                          
	                                                                         
	double  flow    = 0.4;
	double  stretch = 0.25;
	double  downX = 0.0, downY = 0.0, downZ = -1.0;

	                                                                                   
	                                                                   
	                                                                      
	                                          
	double  thickness    = 2.0;
	double  minBridge    = 0.1;                                                                 

	                                                                                     
	                                                                           
	                                                                          
	                                                                           
	                                                                  
	double  paintSize    = 1.2;                      
	double  paintSpacing = 1.4;                                    
	double  paintGaps    = 0.3;                                       
	bool    paintCrack   = true;                                 
	double  paintSnap    = 5.0;                                                    
	double  paintClose   = 0.15;                                                             
	double  paintSingle  = 2.0;
};
