// параметры: предыдущий вариант подготовки объёмных осколков
// ProjectDamage rigid-body fracture — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for Rigid Body Fracture (recipe: rb-fracture-v1): the
// source cut into CONVEX pieces (3D Voronoi cells) ready for a rigid-body
// simulation, with material-specific cell layouts and Grease Pencil hints.
// Plain data, no ProjectDamage/Manifold includes. Lengths in scene units (cm
// in the Blender addon); shares 0..1.
#pragma once
#include <cstdint>
#include <vector>

struct RBImpact { double x = 0, y = 0, z = 0, radius = 0, strength = 1.0; };

// исходные значения и ограничения отдельных частей эффекта
struct RBFractureParams
{
	bool    enable   = false;
	int32_t seed     = 0;
	                                                                   
	                                                                 
	                                                                          
	                                                                      
	                                                                
	int32_t material = 0;
	int32_t pieces   = 40;                                                      
	int32_t maxPieces = 1500;                       
	double  gap      = 0.0;                                               
	double  sizeVariation = -1;                                                      
	double  minPiece = 0.0002;                                                         

	                                                                          
	                                                                         
	                                                                         
	                                                                     
	double  cutSpacing   = 0.0;                                                  
	double  impactRadius = 20.0;                                                    
	double  impactDensity = 8.0;
	double  rubble   = -1;                                                             
	std::vector<RBImpact> impacts;                                             

	                                                               
	double  grainX = 0, grainY = 0, grainZ = 1;
	double  grainStretch = 5.0;                                 
	double  weaveStretch = 2.0;          
	                       
	int32_t glassSpokes = 16;
	double  glassRingGrowth = 1.9;

	                                      
	double  innerUvScale = 10.0;
	double  smoothAngle  = 60.0;
};
