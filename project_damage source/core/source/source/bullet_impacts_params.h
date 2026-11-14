// параметры: строим следы пуль с формой, зависящей от материала
// ProjectDamage bullet impacts — GPL-3.0-or-later; see LICENSE.
//
// Parameter block for the Bullet Impacts effect (recipe: bullet-impacts-v1).
// Plain data with no ProjectDamage/Manifold includes, so project_damage_core.h can include it
// ahead of ProjectDamageParams and carry one `BulletImpactParams bullet;` member
// without any include cycle. All lengths are scene units (cm in the Blender
// addon, the same units as every other ProjectDamage size); shares/chances are 0..1.
// Values marked "x size" are multiples of the impact's hole radius.
//
// One struct serves all three materials. `material` picks which recipe runs;
// the metal*, wood* and conc* blocks are only read by their own recipe, the
// rest is shared. BulletImpactPreset() below loads the reference-matched
// defaults for one material (call it from the Python preset button, or when
// the user switches the material dropdown).
#pragma once

#include <cstdint>

// исходные значения и ограничения отдельных частей эффекта
struct BulletImpactParams
{
	bool    enable   = false;
	int32_t seed     = 0;                                                                          
	int32_t mode     = 0;                                                       
	int32_t material = 0;                                                    
	bool    avoid    = true;                                   
	double  detail   = 1.0;                                                          

	                                                                                
	double  sizeMin  = 0.5;                                   
	double  sizeMax  = 0.9;
	double  sizeBias = 0.5;                                                      
	double  penetration    = 2.0;                                            
	double  penetrationVar = 0.3;                                               
	double  thicknessCap   = 0.6;                                                               

	                                                                               
	                                                           
	                                                                          
	                                                                   
	int32_t shotMode = 0;
	double  oblique  = 0.3;                                                           
	double  shotX = 0.0, shotY = 1.0, shotZ = 0.0;
	double  elongate = 1.0;                                                              

	                                                                                 
	int32_t amount        = 40;                                                      
	int32_t clusters      = 4;                                                   
	double  clusterShare  = 0.75;                                               
	double  clusterSpread = 12.0;                 
	double  minSpacing    = 0.9;                                                                

	                                                                                   
	                                                                              
	                                                                             
	                                                                              
	                                                                         
	                                                                         
	                   
	double  paintSpacing = 3.0;
	double  paintSpread  = 1.0;
	double  paintSnap    = 5.0;                                                      
	double  paintClose   = 0.15;                                                               
	double  paintSingle  = 2.0;

	                                                                                      
	                                                                        
	                                                                            
	                                                            
	bool    haloEnable = true;
	double  haloScale  = 2.6;                            
	double  haloDepth  = 0.015;
	double  haloJagged = 0.5;

	                                                                                    
	int32_t pitAmount = 3;                      
	double  pitSize   = 0.18;               
	double  pitSpread = 3.5;                

	                                                                                      
	double  metalHoleJagged = 10.0;                       
	double  metalDish       = 0.88;                                                 
	double  metalDishWidth  = 0.76;                       
	double  metalLipHeight  = 0.35;                                               
	double  metalLipWidth   = 0.55;                       
	int32_t metalPetals     = 6;                               
	double  metalPetalTear  = 0.5;                                           
	double  metalCurl       = 0.4;                                      
	double  metalExitBurr   = 0.9;                                                           
	double  metalDentChance = 0.1;                                                           

	                                                                                        
	int32_t grainMode = 0;                                                                 
	double  grainX = 0.0, grainY = 0.0, grainZ = 1.0;
	double  woodCrush       = 0.3;                                                 
	double  woodSplinter    = 1.4;                                                     
	double  woodFiberWidth  = 0.3;                                 
	double  woodExitScale   = 3.0;                                 
	double  woodExitDepth   = 0.45;                                                     
	int32_t woodSlivers     = 4;                                                 
	double  woodSliverLength = 2.0;          
	double  woodSplitChance = 0.35;                                                      
	double  woodSplitLength = 8.0;           

	                                                                                           
	double  concSpallChance = 0.55;                                                       
	double  concSpallScale  = 3.5;                         
	double  concSpallDepth  = 1.5;                                     
	double  concSkinDepth   = 0.2;                                        
	double  concBoreDepth   = 2.0;                                                  
	double  concJagged      = 0.6;                                
	double  concRoughness   = 0.6;                                    
	double  concAggregate   = 0.5;                           
	double  concExitScale   = 1.6;                                                   
	int32_t concCracks      = 1;                                        
	double  concCrackLength = 1.2;                   
	double  crackWidth      = 0.08;                                                 
	double  crackDepth      = 0.35;
};

                                                                         
                                                                            
                                                       
inline void BulletImpactPreset(BulletImpactParams& p, int32_t material)
{
	const BulletImpactParams d;
	BulletImpactParams q = d;
	q.enable = p.enable; q.seed = p.seed; q.mode = p.mode; q.avoid = p.avoid; q.detail = p.detail;
	q.shotMode = p.shotMode; q.shotX = p.shotX; q.shotY = p.shotY; q.shotZ = p.shotZ;
	q.amount = p.amount; q.clusters = p.clusters; q.clusterSpread = p.clusterSpread;
	q.grainMode = p.grainMode; q.grainX = p.grainX; q.grainY = p.grainY; q.grainZ = p.grainZ;
	q.material = material;
	switch (material) {
	case 1:                                                          
		q.sizeMin = 0.35; q.sizeMax = 0.6; q.penetration = 8.0;
		q.haloEnable = false; q.pitAmount = 0;
		q.minSpacing = 1.2; q.clusterShare = 0.6;
		break;
	case 2:                                                              
		q.sizeMin = 0.3; q.sizeMax = 0.75; q.sizeBias = 0.65; q.penetration = 1.0;
		q.haloEnable = false; q.pitAmount = 4; q.pitSize = 0.3; q.pitSpread = 6.0;
		q.minSpacing = 1.3; q.clusterShare = 0.5; q.oblique = 0.35;
		break;
	default:                                                            
		q.material = 0;
		break;
	}
	p = q;
}
