// общие объявления: строим пену из объединённых неровных объёмов
// ProjectDamage expanding foam — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                    
struct FoamParams
{
	int32_t seed = 0;
	double  maxGap = 0.02, minGap = 0.0;                             
	double  density = 40000.0;                                                          
	double  spacing = 0.9;                                               
	double  minOpenness = 0.08;                                                 
	double  blobSize = 1.6;
	double  bulge = 1.2;                                              
	double  lumpiness = 1.0;
	double  sag = 0.6;                                        
	double  upX = 0.0, upY = 0.0, upZ = 1.0;
	int32_t maxBlobs = 20000;
	                                                                  
	int32_t style = 1;                                                                    
	double  spread = 1.0;                                                           
	double  cauliflower = 1.0;                                                  
	double  drips = 0.35;                                                         
	double  dripLength = 2.5;                                 
	double  coverage = 1.0;                                                     
	double  trim = 0.0;                                                              
	double  resolution = 0.14;                                           
	int32_t maxCells = 8000000;                                                  
};

struct FoamResult
{
	std::vector<EVec> points;
	std::vector<double> uvs;
	std::vector<float> variation;
	std::vector<float> cut;                                                       
	std::vector<float> cavity;                                                               
	std::vector<std::array<Int32, 4>> polys;
	int blobs = 0;
};

bool SprayFoam(const MeshView& mv, const FoamParams& f, FoamResult& r);
                                                                            
                                                                         
                                                                             
                           
bool FoamAlongPath(const std::vector<EVec>& centres, const std::vector<EVec>& normals,
	const std::vector<EVec>& tangents, const FoamParams& f, FoamResult& r);
PROJECT_DAMAGE_NS_MACRO_END
