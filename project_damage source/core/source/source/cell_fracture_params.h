// параметры: делим объём на замкнутые осколки выбранного типа
// ProjectDamage cell fracture — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for Cell Fracture (recipe: cell-fracture-v1): the source
// split into solid pieces that fill its whole volume, like a Voronoi cell
// fracture, but with material-shaped, rough (non-planar, non-convex) break
// faces. Built on Ronnegren 2020, "Real Time Mesh Fracturing Using 2D Voronoi
// Diagrams" (BTH): seeds pulled toward the hit, Voronoi cells clipped by the
// mesh, 2D (planar) diagrams for thin objects, a circular pattern for glass,
// and the merged intact remainder the thesis lists as future work.
// Plain data, no ProjectDamage/Manifold includes. Lengths in scene units (cm
// in the Blender addon); shares 0..1.
#pragma once
#include <cstdint>
#include <vector>

                                                                       
                                                                         
                  
struct CellProximity { double x = 0, y = 0, z = 0, radius = 0, strength = 1.0; };

// исходные значения и ограничения отдельных частей эффекта
struct CellFractureParams
{
	bool    enable   = false;
	int32_t seed     = 0;
	                                                                
	                                                                            
	                                                         
	                                                          
	                                                                       
	int32_t material = 2;
	int32_t pieces   = 30;                                                                            
	int32_t maxPieces = 600;                                                                      
	double  gap      = 0.0;                                                                        
	double  sizeVariation = -1;                                                      

	              
	double  roughness = -1;                                                                     
	double  roughScale = 1.0;                                              
	double  detail   = 1.0;                                                                      
	                                                                           
	                                                                       
	int32_t planar   = 0;
	double  planarRatio = 0.2;                                                          

	                                                                 
	double  proxRadius   = 30.0;                                                   
	double  proxDensity  = 6.0;                                              
	double  proxFalloff  = 2.0;                                      
	double  rubble   = -1;                                                                   
	bool    keepRemainder = false;                                                      
	double  remainderReach = 1.5;                                         
	std::vector<CellProximity> proximity;

	                                                                       
	                                                     
	int32_t strokeMode = 2;
	double  strokeSpacing = 0.0;                                                  

	                                                                     
    int32_t guidanceAmount = 0;
    double guidanceRadius = 20.0;
    double guidanceDepth = 30.0;
    double guidanceFalloff = 2.0;
    double guidanceDistance = 5.0;

	       
	double  grainX = 0, grainY = 0, grainZ = 1;                                  
	double  grainStretch = 6.0;                           
	double  splinter = 1.0;                                             
	        
	int32_t glassSpokes = 14;                                 
	double  glassRingGrowth = 1.8;                                     
	double  glassBranch = 0.6;                                              
	double  glassWander = 0.7;                                      
	double  glassHole = 0.0;                                                                     

	                                      
	double  innerUvScale = 10.0;
	double  smoothAngle  = 60.0;
};
