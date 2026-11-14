// общие объявления: строим каркас внутри выреза, сохраняя размеры досок и креплений
// ProjectDamage break framing - GPL-3.0-or-later; see LICENSE.
//
// Wood stud framing behind a drywall skin, the fourth fill of the Boolean
// (Breaker) damage (BreakRebarParams::material = BREAK_FILL_FRAMING, recipe
// break-framing-v1). The object is treated as a hollow shell of drywall or
// plaster (`skin` thick) around a timber frame: studs, plates, noggins and
// headers on wall faces, joists, rims and blocking on decks, floors and
// ceilings. The frame is laid out per flat face of the object (curved faces
// are split into facets), so walls with openings, L and T walls, stages,
// stairs and multi-storey blocks all get one.
#pragma once
#include "project_damage_core.h"
#include "break_rebar.h"

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

                                                                           
                                                                           
                                                                          
                    
                                                                          
                                                                           
                                                                           
                                              
                                                                          
                                                          
void BuildBreakFraming(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, m::Manifold* carve);

PROJECT_DAMAGE_NS_MACRO_END
