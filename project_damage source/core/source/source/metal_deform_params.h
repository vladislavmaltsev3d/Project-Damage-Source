// параметры: формируем вмятины и разрывы металла от воздействия
// ProjectDamage metal deform — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Metal Deform effect (recipes: metal-deform-v1 for
// the Impact style, metal-crush-v1 for the Crush style).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h
// can include it ahead of ProjectDamageParams and carry one
// `MetalDeformParams metal;` member without any include cycle. All lengths
// are scene units (cm in the Blender addon, the same units as every other
// size); shares/chances are 0..1.
#pragma once

#include <cstdint>
#include <vector>

                                                                            
                                                                            
                                                                            
                                                                          
                             
struct MetalDeformImpactor
{
	std::vector<double>  pts;                      
	std::vector<int32_t> tris;                                  
	double strength = 1.0;                                                              
};

                                                                         
                                                             
                                      
                                                   
                                                                                
                                                                 
                            
                                                         
                                                                           
                                                                             
                                                                            
                 
struct MetalCrushEmpty
{
	double rel[12] = { 0.0, 0.0, 0.0,  1.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 0.0, 1.0 };
	double size     = 50.0;
	double strength = 1.0;                                                        
};

// исходные значения и ограничения отдельных частей эффекта
struct MetalDeformParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                 
	bool    avoid  = true;                                                      

	                                                                         
	                               
	                                                                        
	                                                                       
	                                                                       
	int32_t style = 0;

	std::vector<MetalDeformImpactor> impactors;                                   
	std::vector<MetalCrushEmpty>     empties;                                    

	                                                                               
	                                                                          
	                                                                       
	double  edgeLength = 0.35;
	double  detail     = 1.0;                                                       
	int32_t maxTris    = 1500000;                                        

	                                                                               
	                                                                        
	                                                                    
	                                                                       
	                                                            
	double  depthScale     = 1.0;                                                               
	double  clearance      = 0.02;                                        
	double  proximity      = 1.5;                                                   
	double  proximityDepth = 0.4;                                              

	                                                                                 
	double  falloff    = 3.0;                                                 
	double  falloffMin = 2.0;
	double  radiusMax  = 60.0;                                                             
	double  profile    = 0.45;                                                            
	double  bulge      = 0.08;                                                 

	                                                                                 
	                                                                      
	                                                                         
	                                                                          
	                                                                
	double  sheetThickness = 2.0;
	double  thicknessCap   = 0.6;

	                                                                                
	double  crease        = 0.6;                                        
	double  creaseSpacing = 5.0;                                                  
	double  creaseLength  = 1.0;                                                        
	double  creaseSharp   = 0.4;                                          
	double  creaseBranch  = 0.5;                                                        

	                                                                                
	double  crumple      = 0.2;                                       
	double  crumpleScale = 2.5;                 

	                                                                                 
	double  roughness = 0.005;                                                                

	                                                                                
	                                                                         
	                                                              
	                                                                                  
	int32_t crushAim       = 0;                                                      
	                                                                                            
	double  crushProximity = 1.5;                                                           
	                                                                                                 
	                                                                                     
	double  crushDepth  = 0.4;                                                              
	double  crushLength = 1.0;                                                               
	double  crushHinge  = 0.45;                                                                       
	double  crushSkew   = 0.5;                                                                     
	                                                                                     
	double  foldSize    = 18.0;                             
	double  fold        = 1.0;                                                               
	double  foldSharp   = 0.85;                                           
	double  foldWander  = 0.6;                                                              
	double  foldLift    = 0.5;                                                                   
	                                                                                         
	double  crushCrumple = 0.6;                                                         
	double  crushFacet   = 20.0;                
};
