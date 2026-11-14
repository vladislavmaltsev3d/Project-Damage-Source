// параметры: добавляем мелкие поры, вмятины и отслоения бетона
// ProjectDamage concrete pitting — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Concrete Pitting effect (recipe: concrete-pits-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h
// can include it ahead of ProjectDamageParams and carry one
// `ConcretePitsParams pits;` member without an include cycle. All lengths are
// scene units (cm in the Blender addon, like every other ProjectDamage size);
// shares are 0..1.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct ConcretePitsParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                 
	bool    avoid  = true;                                                       

	                                                                               
	                                                                            
	                                                                       
	                                                                   
	                       
	double  edgeLength = 0.3;                                                         
	double  fineEdge   = 0.04;                                                     
	                                                                        
	                                                                                 
	int32_t maxTris    = 3000000;
	double  detail     = 1.0;                                                       

	                                                                               
	double  strength     = 1.0;                                 
	double  thicknessCap = 0.25;                                                      

	                                                                              
	double  poreDensity   = 5.0;                                                       
	double  poreSizeMin   = 0.2;                 
	double  poreSizeMax   = 1.2;
	double  poreSmallBias = 0.6;                                                  
	double  poreDepth     = 0.45;                   
	double  poreSharp     = 0.65;                                                   
	double  poreJagged    = 0.7;                             
	double  poreStretch   = 0.45;                                  
	double  poreRim       = 0.35;                                             
	double  poreMerge     = 0.35;                                                            
	double  clustering    = 0.6;                                                           
	double  clusterScale  = 30.0;                           

	                                                                             
	bool    flakeEnable   = true;
	double  flakeCoverage = 0.12;                                                      
	double  flakeScale    = 22.0;             
	double  flakeDepth    = 0.22;                               
	int32_t flakeLayers   = 2;                   
	double  flakeJagged   = 0.6;                        
	double  flakeRough    = 0.6;                                                     
	double  aggregateSize = 0.5;                            
	double  flakePores    = 5.0;                                                 

	                                                                               
	double  dentDensity = 0.35;                                     
	double  dentSizeMin = 0.8;                    
	double  dentSizeMax = 3.5;
	double  dentDepth   = 0.08;                     
	double  dentChip    = 0.6;                                                          

	                                                                                 
	double  grain     = 0.012;                                
	double  grainSize = 0.25;                                                         
	double  undulation      = 0.04;                         
	double  undulationScale = 45.0;

	                                                                               
	                                                                        
	                                                                           
	                                                                       
	std::vector<double> weights;
	double  zoneFeather = 0.25;                                                  
};
