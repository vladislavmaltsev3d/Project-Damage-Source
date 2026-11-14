// общие объявления: растим корни по поверхности и заданным направлениям
// ProjectDamage roots generator — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                       
struct RootsParams
{
	int32_t seed = 0;
	double  size = 3.0;                                                     
	int32_t rootsPerSeed = 6;                                                          
	double  rootLength = 0.5;                                    
	double  lengthRandom = 0.3;                                                   
	double  primaryWeight = 0.6;                                                 
	double  randomWeight = 0.35;                                                  
	double  gravityWeight = 0.4;                                                  
	double  spreadWeight = 0.4;                                                   
	double  branchingProbability = 0.08;                                          
	double  branchAngle = 45.0;                                                    
	int32_t maxLevels = 3;                                                        
	double  childLength = 0.5;                                                       
	double  rootSize = 0.6;                                              
	double  taper = 0.8;                                                            
	double  tipRadius = 0.06;                                                     
	double  childRadius = 0.7;                                                                     
	double  embed = 0.35;                                                          
	double  flare = 0.15;                                                           
	double  flatten = 0.25;                                                                     
	int32_t sides = 8;                             
	double  uvScale = 1.0;                                
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                     
	int32_t maxNodes = 20000;
	int32_t maxSteps = 2000;
	double  guideWeight = 1.0;                                                        
	double  guideRadius = 0.15;                                  
};

struct RootsResult
{
	std::vector<EVec> points;
	std::vector<std::array<double, 2>> uvVert;                 
	std::vector<std::array<Int32, 4>> polys;
	int roots = 0, nodes = 0;
};

                                                                                         
bool GrowRoots(const MeshView& target, const std::vector<EVec>& seeds, const RootsParams& p, RootsResult& out,
	const std::vector<std::vector<EVec>>* guides = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
