// параметры: обрабатываем форму фаски и изношенные участки рёбер
// ProjectDamage edge bevel — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Edge Bevel effect (recipe: edge-bevel-v1): sharp
// convex edges of the source mesh are cut to a kerb-style profile (chamfer,
// round, batter, cove, step), on every sharp edge or only on the edges picked
// with grease pencil strokes, and an Edge Wear layer goes on top: an uneven,
// worn bevel, chips knocked out along it, and a 0..1 wear mask for materials.
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `EdgeBevelParams bevel;` member without
// any include cycle. Lengths are scene units (cm in the addon); shares 0..1.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct EdgeBevelParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                 
	bool    avoid  = true;                                                                                   
	double  detail = 1.0;                                                                

	                                                                                
	                                                                         
	                                                                          
	                                                        
	double  angle       = 40.0;                                                    
	bool    drawnOnly   = false;
	double  paintRadius = 4.0;                                           
	double  paintAlign  = 40.0;                                               
	double  paintCover  = 0.3;                                                            
	                                                                          
	                                                       
	bool    paintPartial = false;
	double  paintTaper   = 5.0;
	double  maxTurn      = 35.0;                                                                    

	                                                                                
	                                                                                
	                                                                            
	                                                          
	                                 
	int32_t profile   = 1;
	double  roundness = 2.0;                                                               
	double  filletTop  = 0.0;                                                         
	double  filletSide = 0.0;                                                          
	                                                                         
	                                                                           
	                                                                          
	                                                                        
	int32_t sizeMode = 0;
	double  sizeTop  = 1.5;
	double  sizeSide = 1.5;
	double  maxShare = 0.5;                                                                             
	double  taper    = 0.0;                                                                    
	bool    taperFlip = false;
	bool    roundCorners = true;                                                                  

	                                                                                   
	bool    wearEnable  = true;
	double  wearUneven  = 0.25;                                                        
	double  wearUnevenScale = 25.0;                                 
	double  wearRough   = 0.3;                                         
	double  wearChips   = 8.0;                                          
	double  wearChipMin = 0.5;                                                                         
	double  wearChipMax = 1.6;
	double  wearChipDepth = 0.35;                          
	double  wearChipStretch = 1.8;                         
	double  wearBig     = 0.12;                                        
	                                                                     
	                                                                         
	                                                              
	double  wearMaskWidth = 2.0;
	double  wearMaskNoise = 0.6;
	double  wearMaskScale = 3.0;

	double  upX = 0.0, upY = 0.0, upZ = 1.0;                                           
};

                                                                         
                                                                             
                                                                           
             
enum EdgeBevelPreset : int32_t
{
	EB_PRESET_CUSTOM = 0,
	EB_PRESET_DWARF_WALL,                                                         
	EB_PRESET_ROUND_TOP,                                                                   
	EB_PRESET_FLAT_TOP,                                                       
	EB_PRESET_DROPPER,                                                                    
	EB_PRESET_HALF_BATTER,                              
	EB_PRESET_FULL_BATTER,                              
	EB_PRESET_BULL_NOSE,                             
	EB_PRESET_CHANNEL,                                                
	EB_PRESET_COUNT
};

inline void ApplyEdgeBevelPreset(EdgeBevelParams& p, int32_t preset)
{
	if (preset <= EB_PRESET_CUSTOM || preset >= EB_PRESET_COUNT)
		return;
	p.sizeMode = 1;
	p.taper = 0.0;
	p.filletTop = p.filletSide = 0.0;
	p.roundness = 2.0;
	p.maxShare = 0.95;
	switch (preset)
	{
	case EB_PRESET_DWARF_WALL:
		p.profile = 0; p.sizeTop = 0.9;  p.sizeSide = 0.55; p.filletTop = 0.05; break;
	case EB_PRESET_ROUND_TOP:
		p.profile = 1; p.sizeTop = 0.5;  p.sizeSide = 0.5;  p.maxShare = 0.499; break;
	case EB_PRESET_FLAT_TOP:
		p.profile = 1; p.sizeTop = 0.07; p.sizeSide = 0.07; p.maxShare = 0.5; break;
	case EB_PRESET_DROPPER:
		p.profile = 0; p.sizeTop = 0.35; p.sizeSide = 0.8;  p.filletTop = 0.6; p.filletSide = 0.15;
		p.taper = 0.85; break;
	case EB_PRESET_HALF_BATTER:
		p.profile = 0; p.sizeTop = 0.35; p.sizeSide = 0.8;  p.filletTop = 0.6; p.filletSide = 0.15; break;
	case EB_PRESET_FULL_BATTER:
		p.profile = 0; p.sizeTop = 0.5;  p.sizeSide = 0.9;  p.filletTop = 0.15; p.filletSide = 0.1; break;
	case EB_PRESET_BULL_NOSE:
		p.profile = 1; p.sizeTop = 0.4;  p.sizeSide = 0.4;  break;
	case EB_PRESET_CHANNEL:
		p.profile = 2; p.sizeTop = 0.6;  p.sizeSide = 0.3;  p.roundness = 2.0; break;
	default: break;
	}
}
