// общие объявления: строим кабели вдоль линий и укладываем их с учётом препятствий
// ProjectDamage cable generator — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include <array>
#include <cstdint>
#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                         
                                                                        
                                                                             
                                                    
struct CableGenParams
{
	int32_t seed = 0;
	int32_t type = 0;                                                                                  
	double  diameter = 0.011;                                               
	int32_t count = 1;                                                                     
	int32_t clumps = 1;                                                                    
	double  radius = 0.0;                                                                  
	int32_t arrangement = 0;                                                             
	double  gap = 0.002;                                                          
	double  twist = 2.0;                                           
	               
	double  lengthMin = 1.0, lengthMax = 2.0;                                   
	        
	double  noise = 0.0;                                                                  
	double  noiseLength = 0.3;                          
	double  sag = 0.0;                                                                              
	double  sagRandom = 0.3;
	double  spread = 0.3;                                                          
	                                              
	double  diameterRandom = 0.0;                                                         
	double  fanOut = 0.0;                                                  
	double  coils = 0.0;                                                                           
	double  coilRadius = 0.12;
	double  coilTurns = 2.0;
	double  dangle = 0.0;                                                                              
	double  dangleLength = 1.0;                                     
	                                
	double  gravity = 0.0;                                      
	double  metre = 1.0;                                   
	double  slack = 0.05;                                                             
	double  stiffness = 0.45;                                                             
	double  friction = 0.6;                                                               
	int32_t pin = 0;                                                                        
	int32_t steps = 300;                                              
	int32_t useFloor = 0;
	double  floorHeight = 0.0;                   
	       
	int32_t endStart = 0;                                                                         
	int32_t endEnd = 0;
	double  strip = 0.04;
	double  splay = 0.4;
	double  fray = 0.5;
	             
	double  damage = 0.0;                                                               
	double  damageSize = 1.0;                                       
	double  strands = 0.6;                                                                
	double  strandLength = 1.0;                         
	int32_t damageEverywhere = 0;                                                   
	                    
	double  tieSpacing = 0.0;
	double  tieWidth = 0.004;
	int32_t clips = 0;
	int32_t sides = 10;                      
	double  segment = 0.01;
	int32_t useRadius = 1;
	int32_t useTilt = 1;
	double  uvScale = 1.0;
	double  downX = 0.0, downY = 0.0, downZ = -1.0;
	int32_t maxVerts = 3000000;
};

                                                                         
                                                                  
struct CableGenCurve
{
	std::vector<EVec> pts;
	std::vector<double> tilt, radius;
	std::vector<uint8_t> anchor;
	bool cyclic = false;
};

struct CableGenScene
{
	std::vector<EVec> spawns;                                           
	std::vector<EVec> colliderPts;                                               
	std::vector<std::array<int32_t, 3>> colliderTris;
	std::vector<std::array<double, 4>> zones;                                     
};

                                                                      
                                                                          
                                                      
struct CableGenResult
{
	std::vector<EVec> points;
	std::vector<int32_t> faceVerts, faceSizes, faceMat, faceCable;
	std::vector<std::array<double, 2>> loopUv;                         
	int cables = 0, ties = 0, clips = 0, holes = 0, connectors = 0, particles = 0, simSteps = 0;
};

bool BuildCableCurves(const std::vector<CableGenCurve>& curves, const CableGenScene& scene, const CableGenParams& p,
	CableGenResult& out);
PROJECT_DAMAGE_NS_MACRO_END
