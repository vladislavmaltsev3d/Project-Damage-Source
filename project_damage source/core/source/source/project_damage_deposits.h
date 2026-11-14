// общие объявления: строим отложения, наплывы и заполнение углов
// ProjectDamage surface deposits — GPL-3.0-or-later; see LICENSE.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#pragma once
#include "project_damage_core.h"

PROJECT_DAMAGE_NS_MACRO_BEGIN
std::vector<manifold::Manifold> PlaceSurfaceDeposits(
    const CoreInput& in, const ProjectDamageParams& params,
    const std::vector<EVec>& normals);
PROJECT_DAMAGE_NS_MACRO_END
