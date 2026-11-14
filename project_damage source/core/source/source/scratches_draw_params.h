// параметры: прокладываем царапины вдоль нарисованных штрихов
// ProjectDamage scratches draw — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Scratches Draw effect (recipe: scratches-draw-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `ScratchDrawParams scratch;` member
// without any include cycle. All lengths are scene units (cm in the Blender
// addon, the same units as every other ProjectDamage size); shares/chances are 0..1.
//
// The user draws grease pencil strokes on or near the surface. Scratches are
// scattered within `spread` of each stroke, mostly running along the stroke
// with some angle jitter, and are carved into the mesh as shallow grooves.
// `style` picks the look. Styles 0-2 are built-in looks that the core
// applies itself (ScratchDrawLook below), so the shape fields are ignored
// for them; style 3 (custom) reads every shape field as set. Density,
// spread, sizeScale and the other placement controls work in every style.
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct ScratchDrawParams
{
	bool    enable = false;
	int32_t seed   = 0;                                                                            
	int32_t style  = 0;                                                                                        
	int32_t mode   = 0;                                                                                                 
	bool    avoid  = true;                                     
	double  detail = 1.0;                                               

	                                                                                   
	double  density     = 0.8;                                         
	int32_t maxScratches = 600;                                                        
	double  spread      = 2.5;                                                     
	double  sizeScale   = 1.0;                                                                
	double  falloff     = 1.8;                                                        
	double  angleJitter = 18.0;                                                         
	double  crossShare  = 0.08;                                              
	double  along       = 0.0;                                                                         
	double  paintSnap   = 5.0;                                                     

	                                                                                       
	double  lengthMin = 1.0;                            
	double  lengthMax = 5.0;
	double  lengthBias = 0.6;                                                        
	double  width     = 0.05;                                 
	double  widthVar  = 0.5;                                  
	double  depth     = 0.025;                                 
	double  depthVar  = 0.5;
	int32_t profile   = 0;                                          
	double  taper     = 0.3;                                                             
	double  gouge     = 0.4;                                                                   
	double  curve     = 0.15;                                                  
	double  wobble    = 0.05;                                     
	double  gaps      = 0.2;                                                                 
	double  wrapAngle = 35.0;                                                                      

	                                                                                         
	double  bundleChance = 0.3;
	int32_t bundleMin    = 2;
	int32_t bundleMax    = 5;
	double  bundleGap    = 0.12;                                         
	double  bundleThin   = 0.6;                                                     

	                                                                                          
	                                                                        
	                                                                    
	bool    scuffEnable = false;
	double  scuffWidth  = 6.0;                                              
	double  scuffDepth  = 0.004;

	                                                                                           
	                                                                           
	                                                         
	double  maskFalloff = 0.15;
};

enum : int32_t { kScratchFine = 0, kScratchKeyed = 1, kScratchScuffed = 2, kScratchCustom = 3 };
enum : int32_t { kScratchScatter = 0, kScratchSingle = 1 };

                                                                          
                                                                     
                                                                            
                                                                            
inline void ScratchDrawLook(ScratchDrawParams& p, int32_t style)
{
	switch (style) {
	case kScratchKeyed:
		p.falloff = 2.5; p.angleJitter = 8.0; p.crossShare = 0.04; p.along = 0.3;
		p.lengthMin = 3.0; p.lengthMax = 12.0; p.lengthBias = 0.4;
		p.width = 0.09; p.widthVar = 0.4; p.depth = 0.06; p.depthVar = 0.4; p.profile = 0;
		p.taper = 0.25; p.gouge = 0.7; p.curve = 0.1; p.wobble = 0.03; p.gaps = 0.1;
		p.bundleChance = 0.1; p.bundleMin = 2; p.bundleMax = 3; p.bundleGap = 0.15; p.bundleThin = 0.5;
		p.scuffEnable = false; p.scuffWidth = 6.0; p.scuffDepth = 0.004; p.maskFalloff = 0.2;
		break;
	case kScratchScuffed:
		p.falloff = 1.3; p.angleJitter = 25.0; p.crossShare = 0.12; p.along = 0.0;
		p.lengthMin = 0.5; p.lengthMax = 2.5; p.lengthBias = 0.65;
		p.width = 0.035; p.widthVar = 0.5; p.depth = 0.015; p.depthVar = 0.5; p.profile = 1;
		p.taper = 0.35; p.gouge = 0.2; p.curve = 0.2; p.wobble = 0.06; p.gaps = 0.35;
		p.bundleChance = 0.7; p.bundleMin = 3; p.bundleMax = 7; p.bundleGap = 0.07; p.bundleThin = 0.7;
		p.scuffEnable = true; p.scuffWidth = 6.0; p.scuffDepth = 0.004; p.maskFalloff = 0.25;
		break;
	case kScratchFine:
		p.falloff = 1.8; p.angleJitter = 18.0; p.crossShare = 0.08; p.along = 0.0;
		p.lengthMin = 1.0; p.lengthMax = 5.0; p.lengthBias = 0.6;
		p.width = 0.05; p.widthVar = 0.5; p.depth = 0.025; p.depthVar = 0.5; p.profile = 0;
		p.taper = 0.3; p.gouge = 0.4; p.curve = 0.15; p.wobble = 0.05; p.gaps = 0.2;
		p.bundleChance = 0.3; p.bundleMin = 2; p.bundleMax = 5; p.bundleGap = 0.12; p.bundleThin = 0.6;
		p.scuffEnable = false; p.scuffWidth = 6.0; p.scuffDepth = 0.004; p.maskFalloff = 0.15;
		break;
	default:
		break;
	}
}

                                                                           
                                                                   
inline void ScratchDrawPreset(ScratchDrawParams& p, int32_t style)
{
	p.style = style;
	ScratchDrawLook(p, style);
	if (style == kScratchKeyed) p.spread = 1.5;
	else if (style == kScratchScuffed) p.spread = 3.0;
	else if (style == kScratchFine) p.spread = 2.5;
}
