// параметры: режем металлический лист вдоль рисунка и отгибаем его края
// ProjectDamage metal tear — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core (Cornelius
// Dammrich's GPL-3 ProjectDamage); not part of the upstream release.
//
// Parameter block for Metal Tear Draw (recipe: metal-tear-v1): a slit torn
// through sheet metal along every Grease Pencil stroke, the metal around it
// pushed in with the tear. Plain data (no includes beyond std), so
// project_damage_core.h can carry one `MetalTearParams metalTear;` member.
// Lengths are scene units (cm in the Blender addon); percents are 0..100.
#pragma once

#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct MetalTearParams
{
	bool    enable = false;
	int32_t seed   = 0;

	                                                                      
	std::vector<double>  strokePts;
	std::vector<int32_t> strokeCount;

	                                                                                  
	double width     = 0.6;                                 
	double taper     = 70.0;                                                               
	double jagged    = 50.0;                                 
	double meander   = 30.0;                                                  
	double cutDepth  = 1.0;                                                                   

	                                                                                  
	double dentDepth = 0.8;                                                    
	double dentWidth = 2.5;                                            
	double lipCurl   = 50.0;                                                             
	double blisters  = 40.0;                                                                   
	double blisterSize = 0.25;

	                                                                                  
	double  detail  = 1.5;                                   
	int32_t maxTris = 1500000;
};
