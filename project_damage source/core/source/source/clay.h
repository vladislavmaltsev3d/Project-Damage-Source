// общие объявления: деформируем поверхность как глину, сохраняя допустимую толщину
// ProjectDamage clay — GPL-3.0-or-later; see LICENSE.
// Modified work: new effect added to the ProjectDamage core.
#pragma once
#include "project_damage_core.h"
#include "clay_params.h"
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                              
                                                                
                                                                          
                                                                             
                                                             
                                                                 
                                                                            
                                                                            
        
  
                                                                             
                                                                     
                                                                             
bool ApplyClay(const CoreInput& in, const ClayParams& p, Int32 mainSeed, const IFieldMask* mask,
	const m::Manifold& base, m::Manifold& out, std::string* log = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
