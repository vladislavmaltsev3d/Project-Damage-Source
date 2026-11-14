// общие объявления: вскрываем стену и размещаем кирпичи за её наружным слоем
// ProjectDamage brick damage — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "brick_damage_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                                                                           
                                                                              
                                                                          
                                                                  
                                                                         
                                                                      
                                                                             
                                                                   
                                                                          
                                                                              
                                                                           
                                                                            
                                                                               
                                                                             
                                                                                
                                                                                 
                                                                       
                                                                                          
                                                                          
                                                                            
                                                                   
                                                                       
                                                                           
                                                                      
void PlaceBrickDamage(const CoreInput& in, const BrickDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	std::vector<m::Manifold>& cutters, std::vector<m::Manifold>& cracks,
	std::vector<m::Manifold>& bricks, const std::vector<MeshView>* brickModels = nullptr);

                                                                              
                                                                             
                                                                         
                                                       
                                                                           
                                                                                
                                                                                
                                                                          
                                                                        
                                                              
                                                                                 
                                                                            
                                                                              
                                                                            
                                                                             
                                                                         
                                                              
                              
                                                                              
                                                                              
                                                                             
                                                                                 
                                                                            
                                                                              
                                                                            
                                                                               
                                                      
struct BrickDamageOut {
	std::vector<m::Manifold> cutters, breaks, seams, behind, render, cracks, bricks, mortar;
	std::vector<m::Box> zones;
};
void PlaceBrickDamage(const CoreInput& in, const BrickDamageParams& p, Int32 mainSeed,
	const IFieldMask* mask, const std::vector<std::vector<EVec>>* paintStrokes,
	BrickDamageOut& out, const std::vector<MeshView>* brickModels = nullptr);

                                                                             
                                                                           
                                                                              
                                                                                 
                                                                             
                                                                        
                                                                          
struct BrickDamageMesh {
	std::vector<EVec> points;
	std::vector<ProjectDamagePoly> polys;
	std::vector<Int32> polyBrick;
	Int32 bricks = 0;
};
void BrickDamageBricksMesh(const std::vector<m::Manifold>& bricks, BrickDamageMesh& out,
	const std::vector<m::Manifold>* mortar = nullptr);

                                                                              
                                                                               
                                                                               
                                                                        
                                                                        
                                                                         
                                                                           
                                                                      
m::Manifold BrickDamageRemesh(const m::Manifold& result, const std::vector<m::Box>& zones, double edge);
struct BrickSurfaceMesh {
    std::vector<EVec> points,flat;
    std::vector<Int32> triangles,materials,pieces;
};
BrickSurfaceMesh BuildBrickSurface(const BrickDamageParams&,const std::vector<double>&,const std::vector<double>&,const std::vector<double>&,
 double,double,double,double,int,int,const std::vector<double>&,const std::vector<double>&,double,double,const std::vector<double>* = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
