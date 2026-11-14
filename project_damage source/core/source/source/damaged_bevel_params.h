// параметры: строим фаску с локальными сколами вдоль рёбер
// ProjectDamage damaged edge bevel — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Damaged Edge Bevel asset (recipe: damaged-bevel-v1):
// a worn concrete edge. Every sharp convex edge (or the edges picked with
// grease pencil strokes) gets a slightly rounded chamfer, and spaced stretches
// of each edge are broken: the corner is chewed off irregularly, with spall
// chips around it.
//
//   Smoothness     how soft the chamfer and the breakage are
//   Damage         how deep and wide the breakage is
//   Damage Amount  how chewed a damaged stretch is (bites and chips per length)
//   Damage Factor  share of every edge's length that is damaged, as spaced
//                  patches: 1 = the whole edge, 0.5 = half of it, 0 = none
//
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h
// can include it ahead of ProjectDamageParams. Lengths are scene units (cm in
// the add-on), shares and factors 0..1.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct DamagedBevelParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                 
	bool    avoid  = true;                                                     
	double  detail = 0.3;                                                                

	                                                                                
	                                                                             
	                                                                    
	double  angle       = 40.0;                                                    
	bool    drawnOnly   = false;                                                  
	double  paintRadius = 4.0;
	double  paintAlign  = 40.0;
	double  paintCover  = 0.3;
	double  maxTurn     = 35.0;                                                            

	                                                                                
	double  width      = 1.0;                                                                 
	double  smoothness = 0.35;                                                                   
	double  uneven     = 0.15;                                                            
	double  maxShare   = 0.45;                                                                       
	double  profileShape = 0.0;                                                                    
	                                                                                           

	                                                                                  
	double  damage       = 0.5;                                                         
	double  damageAmount = 0.5;                                                             
	double  damageFactor = 0.5;                                                      
	double  damageDepth  = 4.0;                                                        
	double  patchLength  = 15.0;                                         
	double  chipScatter  = 0.5;                                                        
	double  jagged       = 0.6;                                                              
	double  placementRandom = 0.75;                                                           
	                                                                                            
	bool    smoothDamage = false;                                                       
	double  smoothDamageAmount = 0.6;                                                      

	                                                                                    
	                                                                           
	                                                                           
	                                                                       
	bool    exportSegments = false;

	                                                                                
	double  maskWidth = 1.5;                                                       
	double  maskNoise = 0.6;
	double  maskScale = 3.0;

	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                                    
};

                                                                              
                                                                              
                                                                        
                                                                         
                           
enum DamagedBevelPreset : int32_t
{
	DB_PRESET_CUSTOM = 0,
	DB_PRESET_DAMAGED,                                                                         
	DB_PRESET_CLEAN,                                          
	DB_PRESET_NARROW,                                   
	DB_PRESET_LIGHT,                                                       
	DB_PRESET_HEAVY,                                                     
	DB_PRESET_CHIPPED,                                                              
	DB_PRESET_WEATHERED,                                                                   
	DB_PRESET_COUNT
};

                                                                        
inline void ApplyDamagedBevelPreset(DamagedBevelParams& p, int32_t preset)
{
	switch (preset)
	{
	case DB_PRESET_DAMAGED:
		p.width = 1.0;  p.smoothness = 0.35; p.uneven = 0.15;
		p.damage = 0.5; p.damageAmount = 0.5; p.damageFactor = 0.5;
		p.damageDepth = 4.0; p.patchLength = 15.0; p.chipScatter = 0.5; p.jagged = 0.6;
		p.placementRandom = 0.75; p.smoothDamage = false; p.smoothDamageAmount = 0.6;
		break;
	case DB_PRESET_LIGHT:
		p.width = 0.8;  p.smoothness = 0.4;  p.uneven = 0.12;
		p.damage = 0.3; p.damageAmount = 0.35; p.damageFactor = 0.25;
		p.damageDepth = 4.0; p.patchLength = 8.0; p.chipScatter = 0.35; p.jagged = 0.5;
		p.placementRandom = 0.85; p.smoothDamage = false; p.smoothDamageAmount = 0.6;
		break;
	case DB_PRESET_HEAVY:
		p.width = 1.2;  p.smoothness = 0.3;  p.uneven = 0.2;
		p.damage = 0.85; p.damageAmount = 0.75; p.damageFactor = 0.75;
		p.damageDepth = 5.0; p.patchLength = 25.0; p.chipScatter = 0.8; p.jagged = 0.75;
		p.placementRandom = 0.7; p.smoothDamage = false; p.smoothDamageAmount = 0.6;
		break;
	case DB_PRESET_CHIPPED:
		p.width = 0.6;  p.smoothness = 0.2;  p.uneven = 0.1;
		p.damage = 0.3; p.damageAmount = 0.9; p.damageFactor = 0.55;
		p.damageDepth = 4.0; p.patchLength = 5.0; p.chipScatter = 1.0; p.jagged = 0.8;
		p.placementRandom = 0.9; p.smoothDamage = false; p.smoothDamageAmount = 0.6;
		break;
	case DB_PRESET_WEATHERED:
		p.width = 1.5;  p.smoothness = 0.75; p.uneven = 0.25;
		p.damage = 0.6; p.damageAmount = 0.4; p.damageFactor = 0.55;
		p.damageDepth = 4.0; p.patchLength = 18.0; p.chipScatter = 0.3; p.jagged = 0.3;
		p.placementRandom = 0.8; p.smoothDamage = true; p.smoothDamageAmount = 0.7;
		break;
	case DB_PRESET_CLEAN:
		p.width = 3.0;  p.smoothness = 0.08; p.uneven = 0.0; p.damageFactor = 0.0;
		break;
	case DB_PRESET_NARROW:
		p.width = 0.5;  p.smoothness = 0.12; p.uneven = 0.0; p.damageFactor = 0.0;
		break;
	default:
		break;
	}
}
