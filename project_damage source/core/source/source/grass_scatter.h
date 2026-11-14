// общие объявления: распределяем травинки по подходящим местам поверхности
// ProjectDamage grass scatter — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                  
struct GrassParams
{
	int32_t seed = 0;
	bool    fresh = false;                                  
	double  upX = 0.0, upY = 0.0, upZ = 1.0;
	double  slopeLimit = 35.0;                                                
	double  baseDensity = 300.0;
	double  edgeDensity = 2500.0, edgeRadius = 0.06, edgeAngle = 25.0;
	double  cavityDensity = 3000.0, cavityRadius = 0.08;
	int32_t strokeMode = 1;                                               
	double  strokeDensity = 2500.0, strokeRadius = 0.08;
	int32_t maxShards = 60000;
	double  lengthMin = 0.015, lengthMax = 0.07, width = 0.0018;
	double  pileHeight = 0.006;
	int32_t segments = 4;
	                                                                 
	int32_t shape = 0;
	double  stoneMin = 0.004, stoneMax = 0.025;
	double  stoneSkew = 2.5;                                               
	double  flatness = 0.55;                         
	double  angularity = 0.35;                              
	double  embed = 0.3;                                                       
	double  sorting = 0.4;                                                                             
};

struct GrassResult
{
	std::vector<EVec> points;
	std::vector<double> uvs;                                        
	std::vector<float> variation, tip;
	std::vector<std::array<Int32, 4>> polys;
	int shards = 0;
};

bool ScatterGrass(const MeshView& mv, const std::vector<std::vector<EVec>>& strokes, const GrassParams& g, GrassResult& r);
PROJECT_DAMAGE_NS_MACRO_END
