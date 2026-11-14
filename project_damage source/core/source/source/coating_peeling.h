// общие объявления: старый способ построения отслоившегося покрытия
// ProjectDamage coating peeling — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                                                                           
                                                                            
                                                                          
                                                                             
                                                                         
                                                                         
                                                                          
                                                                          
                                                                      
                                                                          
                                                        
void PlaceCoatingPeeling(const CoreInput& in, const ProjectDamageParams& params,
                         const std::vector<EVec>& normals,
                         std::vector<manifold::Manifold>& cutters,
                         std::vector<manifold::Manifold>& buildup);
PROJECT_DAMAGE_NS_MACRO_END
