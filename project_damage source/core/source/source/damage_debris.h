// общие объявления: получаем обломки из объёма, потерянного при повреждении
// ProjectDamage damage debris — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "damage_debris_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                               
                                                       
                                                                               
                                                                           
                                                                     
                                                                          
                                                                             
                                                                          
                                      
                                                                            
                                                                    
void ExtractDamageDebris(const m::Manifold& source, const m::Manifold& result,
	const DamageDebrisParams& p, Int32 mainSeed, std::vector<m::Manifold>& pieces);

                                                                         
                                                              
                                                                         
                                                                         
                                                                             
                                                     
                                                                            
                                                                            
                                                                              
                                                                           
                                                                        
                                                                         
                                                                                
                                                                           
                                                                            
                                                           
struct DebrisMesh
{
	std::vector<EVec> points;
	std::vector<ProjectDamagePoly> polys;
	std::vector<Int32> piece, basePoly;
	std::vector<double> uvs;                            
	std::vector<double> normals;                                           
	bool sourceUvs = false;                                              
};
void BuildDebrisMesh(const std::vector<m::Manifold>& pieces, uint32_t baseId, const MeshView& source,
	const DamageDebrisParams& p, DebrisMesh& out);
PROJECT_DAMAGE_NS_MACRO_END
