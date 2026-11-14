// параметры: деформируем поверхность как глину, сохраняя допустимую толщину
// ProjectDamage clay — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Parameter block for the Clay effect (recipe: clay-v1). Plain data, no
// ProjectDamage/Manifold includes, so project_damage_core.h can carry one
// `ClayParams clay;` member without an include cycle.
//
// Every size is RELATIVE to the reference size R (refSize, or the object's
// largest bounding-box dimension when refSize is 0), so the same settings give
// the same hand-made look on a 5 cm figurine and a 2 m sculpture. Percent
// fields are 0..100 like the UI.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct ClayParams
{
	bool    enable = false;
	int32_t seed   = 0;
	bool    avoid  = true;                                                                          

	                                                                                
	double  refSize    = 0.0;                                                     
	double  resolution = 140.0;                                                     
	int32_t maxTris    = 600000;                                             

	                                                                                
	double  softness   = 60.0;                                                         
	double  lumps      = 35.0;                                           
	double  lumpScale  = 1.0;                                        
	double  sag        = 0.0;                                             
	double  bulge      = 50.0;                                                       
	double  gravX = 0.0, gravY = 0.0, gravZ = -1.0;                        

	                                                                                
	double  presses      = 30.0;                                                
	double  pressSize    = 10.0;                     
	double  pressDepth   = 35.0;                             
	double  pressRim     = 50.0;                                            
	double  fingerprints = 50.0;                                        
	double  smears       = 20.0;                                                       
	double  smearLength  = 3.0;                                    

	                                                                                
	double  grain      = 20.0;                                                      
	double  grainScale = 1.0;
	double  cracks     = 0.0;                                          
	double  crackWidth = 50.0;               
	double  crackDepth = 50.0;               

	double  thicknessCap = 0.45;                                                          
	bool paintOnly=false;
	double paintRadius=15.0;
	std::vector<double> paintSegments;
	std::vector<double> zoneWeights;                                                     
};
