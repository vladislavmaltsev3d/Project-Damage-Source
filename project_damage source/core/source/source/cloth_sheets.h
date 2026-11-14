// общие объявления: раскладываем ткань и рассчитываем её провисание и столкновения
// ProjectDamage cloth sheets — GPL-3.0-or-later; see LICENSE.
//
// Torn cloth / tarp sheets that hang from a curve, drape over it, or lie in
// crumpled heaps around points, settled by a small position-based cloth
// solver against collider meshes. Self-contained (no Manifold types), so it
// builds into the core as one more source file.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace pd_cloth {

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
};

enum Mode : int32_t { AroundPoints = 0, AlongCurve = 1, HangingFromCurve = 2 };
enum Type : int32_t { Mixed = 0, Curtain = 1, Swag = 2, Corner = 3, Draped = 4, Crumpled = 5 };

                                                           
struct ClothParams
{
	uint32_t seed = 0;
	int32_t  mode = AroundPoints;
	int32_t  type = Mixed;
	int32_t  amount = 6;                    
	double   scale = 1.2;                                                   
	double   radius = 1.0;                                                         
	double   thickness = 0.004;                                                         
	double   gravity = 1.0;                      
	double   stiffness = 0.25;                                         
	double   noisiness = 0.5;                                                   
	int32_t  clumps = 2;                                                                  
	double   clumpTightness = 0.6;                              
	double   cellSize = 0.04;                                                          
	int32_t  steps = 240;                                              
	double   friction = 0.6;
	double   curveRadius = 0.03;                                         
	double   pinSpacing = 0.35;                                       
	bool     groundFallback = true;                                                     
	         
	bool     damaged = true;
	double   tear = 0.5;                          
	double   holes = 0.4;                                   
	double   upX = 0.0, upY = 0.0, upZ = 1.0;
	                                                                         
	                                                                           
	double   wind = 0.3;                           
	double   windX = 1.0, windY = 0.3, windZ = 0.0;
	double   turbulence = 0.6;
	                                                                         
	                                                                          
	                                                            
	bool     selfCollision = true;
	double   selfDistance = 0.01;                                      
};

struct ClothInput
{
	std::vector<V3> points;                                             
	std::vector<std::vector<V3>> curves;                                                   
	std::vector<V3> colliderTris;                                      
};

struct ClothResult
{
	std::vector<V3> points;
	std::vector<double> uvs;                                                       
	std::vector<float> sheet;                                                       
	std::vector<float> edge;                                                                   
	std::vector<std::array<int32_t, 4>> polys;                                            
	int sheets = 0;
};

bool BuildClothSheets(const ClothInput& in, const ClothParams& p, ClothResult& out);

}                       
