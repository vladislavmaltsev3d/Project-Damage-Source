// общие объявления: раскалываем деревянный слой у края пробоя
// ProjectDamage break chips - GPL-3.0-or-later; see LICENSE.
//
// Wooden Chips, the fifth fill of the Boolean (Breaker) damage
// (BreakRebarParams::material = BREAK_FILL_CHIPS, recipe break-chips-v1): an
// impact on plywood. The panel is treated as a stack of veneer plies whose
// grain alternates; each ply tears along its own grain, the plies nearest the
// impact face open widest, and torn fibres stay attached at the end of their
// tear and bend out of the panel.
#pragma once
#include "project_damage_core.h"
#include "break_rebar.h"

#include <vector>

PROJECT_DAMAGE_NS_MACRO_BEGIN

// `out` receives the splinters (closed shards, one bar each; grain carries each
// point in its shard's own frame, like the framing fill). `carve`, when given,
// receives the stepped torn crater to subtract from the damaged solid.
void BuildBreakChips(const m::Manifold& source, const std::vector<m::Manifold>& cutters,
	const BreakRebarParams& p, Int32 mainSeed, BreakRebarMesh& out, m::Manifold* carve);

PROJECT_DAMAGE_NS_MACRO_END
