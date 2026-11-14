// общие объявления: растим ветви плюща, удерживая их у поверхности и направляющих
// ProjectDamage ivy generator — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                       
struct IvyParams
{
	int32_t seed = 0;
	double  size = 1.0;                                                     
	double  primaryWeight = 0.5;                                               
	double  randomWeight = 0.5;                                                 
	double  gravityWeight = 1.0;                                               
	double  adhesionWeight = 0.2;                                               
	double  branchingProbability = 0.05;                                        
	double  maxFloatLength = 0.1;                                           
	double  maxAdhesionDistance = 0.2;                                
	double  upX = 0.0, upY = 0.0, upZ = 1.0;                     
	int32_t maxNodes = 4000;
	int32_t maxSteps = 600;
	double  branchSize = 0.15;                                           
	int32_t branchSides = 6;
	double  uvScale = 1.0;                                
	double  leafSize = 1.0;                                               
	double  leafDensity = 0.7;                    
	int32_t leafDensityMultiplier = 2;                    
	double  leafOffset = 0.0;                                                  
	double  stemLength = 0.5;                                                                          
	double  stemThickness = 1.0;                                                                
	double  stemAttach = 0.25;                                                                                              
	int32_t leafUvDirection = 3;                                                                                   
	double  leafTilt = 30.0;                                                                                         
    int32_t adhesionSmoothing = 5;                                                      
    double leafMaturity = 1.0;                                                        
	                                                           
	double  guideWeight = 1.0;                                                        
	double  guideRadius = 0.15;                                  
	                                                                          
	                                            
	double  hangDensity = 0.15;                                                   
	double  hangLength = 0.3;                                                              
};

struct IvyResult
{
	std::vector<EVec> branchPoints;
	std::vector<std::array<double, 2>> branchUvVert;                      
	std::vector<std::array<Int32, 4>> branchPolys;
	std::vector<EVec> leafPoints;
	std::vector<std::array<Int32, 4>> leafPolys;
	std::vector<double> leafUvs;                                                       
	int roots = 0, nodes = 0;
};

                                                                                         
                                                                                        
bool GrowIvy(const MeshView& target, const std::vector<EVec>& seeds, const IvyParams& p, IvyResult& out,
	const std::vector<std::vector<EVec>>* guides = nullptr, const std::vector<std::vector<EVec>>* curves = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
