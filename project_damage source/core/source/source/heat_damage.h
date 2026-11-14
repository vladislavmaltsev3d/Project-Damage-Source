// общие объявления: изменяем поверхность для следов нагрева
#pragma once
#include "project_damage_core.h"
PROJECT_DAMAGE_NS_MACRO_BEGIN
struct HeatField { std::vector<double> E; bool valid = false; };
double HeatCharAmount(const ProjectDamageParams&, double);
HeatField ComputeHeatField(const CoreInput&, const ProjectDamageParams&);
void PlaceHeatDamage(const CoreInput&, const ProjectDamageParams&, const std::vector<EVec>&,
    const HeatField&, std::vector<m::Manifold>&, std::vector<m::Manifold>&);
void EvaluateHeatAttributes(const CoreInput&, const ProjectDamageParams&, const HeatField&,
    const std::vector<EVec>&, std::vector<float>&, std::vector<float>&, std::vector<float>&);
PROJECT_DAMAGE_NS_MACRO_END
