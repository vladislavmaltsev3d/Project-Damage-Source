// параметры: прокладываем арматуру и другие внутренние детали через повреждённый объём
// ProjectDamage break rebar - GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the reinforcement bars the Boolean (Breaker) damage
// exposes in the volume it removes (recipe: break-rebar-v1). Plain data with
// no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams. All lengths are scene units (cm in the
// Blender addon, like every other ProjectDamage size); shares/chances are 0..1.
#pragma once

#include <cstdint>
#include <vector>

enum BreakRebarType : int32_t
{
	BREAK_REBAR_RIBBED  = 0,                                                                  
	BREAK_REBAR_HELICAL = 1,                                                           
	BREAK_REBAR_SMOOTH  = 2,                                                 
	BREAK_REBAR_TWISTED = 3,                             
	BREAK_REBAR_STRAND  = 4,                                
};

enum BreakRebarLayout : int32_t
{
	BREAK_REBAR_GRID        = 0,                                 
	BREAK_REBAR_GRID_DOUBLE = 1,                                   
	BREAK_REBAR_CAGE        = 2,                                                    
	BREAK_REBAR_CAGE_ROUND  = 3,
	BREAK_REBAR_CENTRAL = 4,                                                            
};

enum BreakRebarMode : int32_t
{
	BREAK_REBAR_AROUND   = 0,                                                                           
	BREAK_REBAR_BUILT_IN = 1,                                                                         
};

enum BreakRebarFit : int32_t
{
	BREAK_REBAR_FIT_AUTO  = 0,                                                                                   
	BREAK_REBAR_FIT_BOX   = 1,                                                                        
	BREAK_REBAR_FIT_SHAPE = 2,                                                                               
};

enum BreakRebarMaterial : int32_t
{
	BREAK_FILL_REBAR  = 0,                                         
	BREAK_FILL_BRICKS = 1,                                                          
	BREAK_FILL_PLANKS = 2,                                                                     
	BREAK_FILL_FRAMING = 3,
	BREAK_FILL_CHIPS = 4,
    BREAK_FILL_LATHS = 5,                                                                
};

// исходные значения и ограничения отдельных частей эффекта
struct BreakRebarParams
{
    double rebarResolution = 1.0;

    bool experimentalRiven = false;
    bool stableVolume = false;
    int32_t volumeAxis = 0;
    int32_t volumeBudget = 4096;
    double volumeClearance = 0.1;
    double volumeMinLength = 2.0;
    double volumeMinSection = 0.1;
    double volumeOriginX = 0.0, volumeOriginY = 0.0, volumeOriginZ = 0.0;
	bool    enable = false;
    bool generatedGeometry = true;
	bool    existingDamage = false;                                                                         
	int32_t seed   = 0;                                                 

	int32_t material = BREAK_FILL_REBAR;
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                                                           
	int32_t mode     = BREAK_REBAR_AROUND;
	int32_t fit      = BREAK_REBAR_FIT_AUTO;

	                                                                               
	int32_t layout   = BREAK_REBAR_GRID;
	int32_t axis     = 0;                                                                    
	int32_t axisMask = 0;                                                                                        
	int32_t side     = 0;                                                                               
	double  angle    = 0.0;                                              
	double  cover    = 3.0;                                                                 
	double  spacingU = 15.0;                                                     
	double  spacingV = 15.0;                                                      
	bool    barsU    = true;                                                                             
	bool    barsV    = true;                     
	int32_t outer    = 0;                                                    
	double  amount   = 1.0;                                             
	double  jitter   = 0.08;                                              

	                                                                                     
	double  tieSpacing = 20.0;                                                               
	double  tieRadius  = 0.4;                         
	bool    spiral     = false;                                                      

	                                                                                 
	int32_t type       = BREAK_REBAR_RIBBED;
	double  radius     = 0.6;                                                    
	double  scale      = 1.0;                                                                       
	double  ribHeight  = 0.12;                                        
	double  ribPitch   = 0.65;                             
	double  twistPitch = 10.0;                                                          
	int32_t sides      = 12;                                                                           
	double  detail     = 1.0;                                           

	                                                                                   
	double  embed      = 6.0;                                                 
	double  minExposed = 1.0;                                                           

	                                                                                       
	double  wobble = 0.15;                            
	double  bow    = 0.3;                                           
	double  bend   = 0.5;                                            
	double  broken = 0.3;                                                       
	double  trim   = 0.35;                                                                 
	double  neck   = 0.5;                                               
	bool    dropLoose = true;                                                                        

	                                                                                         
	double  deform  = 0.0;                                                                                      
	double  reach   = 0.0;                                                                                                 
	bool    core    = false;                                                                      
	bool    showAll = false;                                                                       

	                                                                                              
	                                                                             
	                                                                         
	                                                                             
	                                                          
	double  brickLength = 25.0;
	double  brickHeight = 6.5;
	double  brickDepth  = 12.0;                    
	double  mortar      = 1.0;                 
	double  missing     = 0.3;                                                                                                                                  
	double  brickBroken = 0.3;                                               
	double  chamfer     = 0.12;                                         
	double  brickJitter = 0.15;                                          
	bool    mortarJoints = true;                                                                    
	double  mortarRecess = 0.08;                                                                   
	                                                                       
	                                                                   
	                                                                         
	                                                                         
	                                                                   
	                              
	bool    brick3D = false;

	                                                                                          
	double  plankWidth     = 12.0;
	double  plankThickness = 2.5;
	double  plankGap       = 0.6;
	int32_t plankDir       = 0;                                              
	double  splinter       = 0.6;                                                         

	                                                                                              
	                                                                              
	                                                                             
	double  studWidth     = 3.8;                                                                
	double  studDepth     = 8.9;                                                           
	double  studSpacing   = 40.6;                                                                      
	double  joistDepth    = 14.0;                                                          
	double  nogginSpacing = 60.0;                                                            
	bool    doublePlate   = true;                                                     
	double  skin          = 1.25;                                                                
	double  cavityReach   = 45.0;                                                     
	double  paperTear     = 4.0;                                                               
	double  paperDepth    = 0.25;                                  
	double  nails         = 0.6;                                                               
	double  nailLength    = 1.0;                                         
	double  woodBroken    = 0.2;                                                                  
	double  facetAngle    = 20.0;                                                                          

	                                                                                           
	                                                                                        
	                                                                             
	// Wooden Chips fill (BREAK_FILL_CHIPS): plywood veneer stack, see break_chips.cpp
	int32_t chipPlies        = 5;      // veneer plies
	double  chipPlyThickness = 0.0;    // 0 = the object thickness divided by the plies
	double  chipPlyAngle     = 90.0;   // grain turn from one ply to the next, degrees
	double  chipLength       = 12.0;   // typical tear length along the grain
	double  chipWidth        = 1.0;    // fibre bundle / splinter width
	double  chipSharp        = 0.8;    // 0 blunt, 1 needle tips
	double  chipLift         = 0.5;    // how far splinters bend out; 1 = up to 75 degrees
	double  chipAmount       = 1.0;    // splinter density
	double  chipCrater       = 4.0;    // extra opening of the ply at the impact face
	int32_t chipFace         = 0;      // impact face: 0 auto, 1 + side, 2 - side

	std::vector<double>  strokePts;
	std::vector<int32_t> strokeCount;
	double  strokeRadius = 1.0;             
	double  strokeEmbed  = 1.0;                                                             
};
