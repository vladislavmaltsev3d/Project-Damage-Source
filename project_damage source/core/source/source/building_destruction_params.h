// параметры: рассчитываем разрушение здания вокруг заданных точек
// ProjectDamage building destruction — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Building Destruction effect (recipe:
// building-destruction-v1). Plain data with no ProjectDamage/Manifold includes, so
// project_damage_core.h can include it ahead of ProjectDamageParams and carry one
// `BuildingDestructionParams blast;` member without any include cycle.
// All lengths are scene units (cm in the Blender addon, the same units as
// every other ProjectDamage size); shares/chances are 0..1. Values marked "x radius"
// are fractions of the blast radius the empty sets.
//
// The building's up axis is the source's local +Z. Floors are horizontal
// bands of floorHeight counted from the bottom of the mesh + floorOffset.
#pragma once

#include <cstdint>
#include <vector>

                                                                          
                                                                     
                                                                             
                                                   
                                                                            
                                                                           
                                                                           
                                                          
                                                                          
                                                                       
struct BuildingBlastEmpty
{
	double rel[12] = { 0.0, 0.0, 0.0,  1.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 0.0, 1.0 };
	double size     = 300.0;
	double strength = 1.0;                                                  
};

// исходные значения и ограничения отдельных частей эффекта
struct BuildingDestructionParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                            
	bool    avoid  = true;                                     
	double  detail = 1.0;                                                                       
	std::vector<BuildingBlastEmpty> empties;

	                                                                                
	double  radiusScale = 1.0;                                     
	double  proximity   = 1.0;                                                         
	                                                                               
	double  proxShrink  = 0.5;                                                   
	double  jagged      = 0.6;                                                      
	double  noiseScale  = 0.35;                                        

	                                                                                 
	double  floorHeight   = 300.0;                          
	double  floorOffset   = 0.0;                                           
	double  slabThickness = 25.0;                         
	double  wallDepth     = 38.0;                                  

	                                                                                
	                                                                           
	                                                          
	double  coreSize    = 0.45;                                       
	double  roomSize    = 0.95;                                                            
	double  roomFacets  = 11.0;                                                                    
	double  partitionSpacing   = 420.0;                                                                       
	double  partitionThickness = 12.0;
	double  slabBreak   = 0.6;                                                                  
	double  slabDroop   = 0.5;                                                        
	double  wallSize    = 0.9;                                                          
	double  fringe      = 0.2;                                                       
	double  fringeDensity = 0.3;                                          
	double  spallReach  = 1.35;                                           
	double  spallCover  = 0.55;                                                   

	                                                                                   
	double  brickLength = 25.0;                                             
	double  brickHeight = 6.5;
	double  jointWidth  = 1.0;                  
	double  jointDepth  = 0.8;                                                
	double  brickJitter = 0.04;                                                                     
	double  plasterDepth = 2.0;                                           

	                                                                                 
	bool    crackEnable  = true;
	int32_t crackCount   = 8;                                                        
	double  crackLength  = 0.6;              
	double  crackWidth   = 0.8;                            
	double  crackDepth   = 4.0;
};
