// общие объявления: добавляем слои штукатурки, следы инструмента и мелкий рельеф
// ProjectDamage plaster displacement — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "plaster_displace_params.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                             
                                                                              
                                            
  
                                                                            
                                                                               
                                                                          
                                                                        
                                                                 
                                                                             
                                             
  
                                                                               
                                                                              
                                   
                                                                                              
                                                                             
                          
                                                                             
                                                                            
                                               
bool ApplyPlasterDisplacement(const CoreInput& in, const PlasterDisplaceParams& p,
	Int32 mainSeed, const IFieldMask* mask, m::Manifold& out,
    std::vector<uint8_t>* affectedFaces = nullptr, const IFieldMask* zone = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
