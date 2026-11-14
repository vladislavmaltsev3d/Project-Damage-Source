// параметры: строим проём и кирпичное заполнение старого варианта стены
// ProjectDamage wall hole with bricks — GPL-3.0-or-later; see LICENSE.
// Plain data (recipe wall-hole-v1). Lengths in scene units (cm in the addon).
#pragma once
#include <cstdint>
#include <vector>

// исходные значения и ограничения отдельных частей эффекта
struct WallHoleParams
{
	bool    enable = false;
	int32_t seed = 0;
	std::vector<double> sites;                                                                 
	double  holeWidth = 60.0, holeHeight = 130.0, holeLift = 0.0;                                         
	double  jagged = 0.6;
	double  edgeRagged = 0.6;                                                                               
	double  depthVariation = 0.35;                                                               
	double  brickLength = 21.5, brickHeight = 6.5, brickWidth = 10.25, mortar = 1.0;
	int32_t bond = 0;                                                                   
	double  jointDepth = 0.8;                                        
	double  plasterThickness = 1.5, plasterBand = 8.0;
	double  brokenChance = 0.35;
	bool    customBricks = false;                                                                    
	bool    rubbleEnable = true;
	double  rubbleShare = 0.6, rubbleSpread = 1.0;
	int32_t rubbleMax = 300;
	double  upX = 0.0, upY = 0.0, upZ = 1.0;
};
