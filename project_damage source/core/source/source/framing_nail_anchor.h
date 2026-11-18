#pragma once

#include <optional>

PROJECT_DAMAGE_NS_MACRO_BEGIN

struct FramingNailSupport
{
    manifold::vec3 front;
    double depth;
};

// Find the front and back of the surviving timber at the fixing point.
inline std::optional<FramingNailSupport> FindFramingNailSupport(
    const manifold::Manifold& wood, const manifold::vec3& point,
    const manifold::vec3& outward, double reach)
{
    const auto hits = wood.RayCast(point + outward * reach, point - outward * reach);
    const manifold::RayHit* front = nullptr;
    for (const auto& hit : hits)
    {
        const double facing = manifold::la::dot(hit.normal, outward);
        if (front == nullptr && facing > 1e-6)
            front = &hit;
        else if (front != nullptr && facing < -1e-6)
        {
            const double depth = manifold::la::dot(front->position - hit.position, outward);
            if (depth > 1e-5)
                return FramingNailSupport{front->position, depth};
        }
    }
    return std::nullopt;
}

PROJECT_DAMAGE_NS_MACRO_END
