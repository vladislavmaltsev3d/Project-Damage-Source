// общие объявления: делим объём на замкнутые осколки выбранного типа
// ProjectDamage cell fracture — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "cell_fracture_params.h"
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                        
                                                                         
                                                                           
                                                                         
                        
                                                                      
                                                                           
                                                                           
                                                                               
                                                                          
                                                        
                                                                       
                                                                            
                                 
                                                                           
                                   
struct CellPieceInfo { double volume = 0.0; EVec center; };
void BuildCellFracture(const CoreInput& in, const m::Manifold& source, uint32_t sourceId,
	const CellFractureParams& p, Int32 mainSeed,
	const std::vector<std::vector<EVec>>* strokes,
	std::vector<m::Manifold>& pieces, std::vector<CellPieceInfo>& info,
	std::string* log = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
