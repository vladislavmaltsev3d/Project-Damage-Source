// параметры: пробиваем отверстия и оставляем в них арматуру и кабели
// ProjectDamage hanging-rebar holes — GPL-3.0-or-later; see LICENSE.
// Plain data (recipe hang-hole-v1). Lengths in scene units (cm in the addon).
#pragma once
#include <cstdint>
#include <string>

// исходные значения и ограничения отдельных частей эффекта
struct HangHoleParams
{
    double edgeWallDepth = 0.0;
    double rebarResolution = 1.0;

	bool    enable  = false;
	int32_t seed    = 0;
	int32_t mode    = 0;                                                                      
	bool    avoid   = true;
	               
	int32_t amount  = 2;
	double thickness = 100.0; // Percent of the automatic through-hole thickness limit.
	double  sizeMin = 30.0, sizeMax = 55.0;                      
	double  jagged  = 0.6;                       
	double  flare   = 0.35;                                                          
	double  roughness = 1.2;                                          
	int32_t surfMode = 0;                                                                      
	std::string texPath;
	double  texScale = 20.0, texStrength = 1.0, texOffX = 0.0, texOffY = 0.0, texOffZ = 0.0, texEdge = 0.0;
	                                                                             
	                                                     
	double  noiseScale = 1.0, noiseDetail = 2.0, noiseRoughness = 0.5, noiseDistortion = 0.0, noiseStrength = 1.0;
	       
	bool    barsEnable = true;
	double  barSpacing = 15.0, barRadius = 0.6, barDepth = 0.5;                                        
	double  barBroken  = 0.65;                                                 
	double  droop      = 0.7;                                      
	double  barBow     = 0.6;                        
	         
	bool    cablesEnable = true;
	int32_t cableAmount  = 6;              
	double  cableRadius  = 0.35, cableLength = 1.0, cableSag = 0.5, cableLoops = 0.35;
	double  gravityX = 0.0, gravityY = 0.0, gravityZ = -1.0;                  
	int32_t sides = 8;
};
