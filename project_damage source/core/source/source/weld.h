// общие объявления: ведём сварные швы вдоль выбранных рёбер и линий
// ProjectDamage welded edges — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                               
struct WeldParams
{
	int32_t seed = 0;
	bool    edgesEnable = true;
	int32_t edgeType = 0;                                                                 
	double  angle = 30.0;                                                          
	double  maskThreshold = 0.5;                                                                      
	bool    strokesEnable = true;
	double  strokeReach = 1e30;                                                         
	double  width = 1.0, height = 0.35;
	double  pitch = 0.3;                            
	double  rippleHeight = 0.25;                              
	double  curvature = 0.6;                                              
	double  wobble = 0.5;                                      
	int32_t sides = 16;                               
	double  detail = 1.0;                                             
	double  uvScale = 1.0;
};

struct WeldResult
{
	std::vector<EVec> points;
	std::vector<double> uvs;                          
	std::vector<float> profile;                                   
	std::vector<std::array<Int32, 4>> polys;
	int beads = 0;
};

                                                                     
                                                        
using WeldEdgePairs = std::vector<std::array<Int32, 2>>;
bool BuildWelds(const MeshView& mv, const std::vector<float>& weights, const std::vector<std::vector<EVec>>& strokes,
	const WeldParams& w, WeldResult& r, const WeldEdgePairs* selectedEdges = nullptr,
	const WeldEdgePairs* excludedEdges = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
