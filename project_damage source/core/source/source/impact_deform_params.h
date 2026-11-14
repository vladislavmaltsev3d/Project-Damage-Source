// параметры: деформируем меш вокруг точки удара с учётом защищённых мест
// ProjectDamage impact deformation — GPL-3.0-or-later; see LICENSE.
// Port of the Deformable Mesh Component (UE) hit deformation. Plain data.
#pragma once
#include <cstdint>
#include <vector>

struct DeformImpact
{
	double x = 0, y = 0, z = 0, radius = 10.0, strength = 1.0;
	                                                                             
	                                       
	bool   ownOrigin = false;
	double ox = 0, oy = 0, oz = 0;
};
struct DeformBox { double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0; };

// исходные значения и ограничения отдельных частей эффекта
struct ImpactDeformParams
{
	bool    enable = false;
	double  strength = 1.0;                                           
	double  maxDeform = 0.75;                                                                                         
	double  totalAmplitude = 1.0;                       
	double  noiseFactor = 0.3;                      
	double  noiseScale = 1.0;                                                                                 
	double  smoothness = 0.0;                                                                      
	double  originX = 0, originY = 0, originZ = 0;                                                               
	int32_t groupsX = 8, groupsY = 8, groupsZ = 8;                                                        
	                                                                                              
	                                      
	std::vector<double> curve;
	std::vector<DeformImpact> impacts;
	std::vector<DeformBox> protection;                                                                           
	                                                                                       
	                                                    
	std::vector<float> protectWeights;
	int32_t signature = 0;                                                                              
};
