// параметры: получаем обломки из объёма, потерянного при повреждении
// ProjectDamage damage debris — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for Damage Debris (recipe: damage-debris-v1): the material
// any damage effect REMOVED from the source, returned as separate solids
// (chunks) so the host can make objects of them for a rigid-body simulation.
// Plain data, no ProjectDamage/Manifold includes, so project_damage_core.h can
// carry one `DamageDebrisParams debris;` member. Lengths in scene units (cm in
// the Blender addon); shares 0..1.
#pragma once
#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct DamageDebrisParams
{
	bool    enable       = false;
	int32_t seed         = 0;                                              
	                                                                          
	                                                                         
	                                          
	double  minVolume    = 0.05;
	                                                                       
	                                                        
	double  minThickness = 0.3;
	                                                                      
	                                                      
	double  chunkSize    = 0.0;
	double  chunkJitter  = 0.5;                                                 
	int32_t maxPieces    = 400;                                        
	                                                                       
	                                                               
	double  innerUvScale = 10.0;
	                                                                        
	                                                                      
	                                                                    
	                                                                     
	                                                   
	double  smoothAngle  = 60.0;
};
