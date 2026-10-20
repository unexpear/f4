#pragma once

#include "core/math/vec3.h"

namespace f4::engine {

// Directional light — unshadowed in M8. `direction` points FROM light TOWARD
// scene (e.g. sun direction). Normalized by convention; renderer normalizes defensively.
struct DirectionalLight {
    math::Vec3 direction{0.f, -1.f, 0.f};
    math::Vec3 color{1.f, 1.f, 1.f};
    float intensity = 1.f;
};

// Point light consumed by the clustered forward PBR pass (see
// engine/lighting/clustered_lights.h). `position` is world space, or local to
// the entity's WorldMatrix when it has one. Radiance is color * intensity with
// windowed inverse-square falloff that reaches exactly zero at `range`:
//   window(d) = clamp(1 - (d / range)^4, 0, 1)^2,  attenuation = window / (d^2 + 1)
// `castShadows` makes the light a candidate for the renderer's small
// point-shadow budget (PbrRenderSettings::pointShadowBudget); the brightest,
// closest flagged lights get cube shadows, the rest stay unshadowed.
struct PointLight {
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Vec3 color{1.f, 1.f, 1.f};
    float intensity = 1.f;
    float range = 10.f;
    bool castShadows = false;
};

} // namespace f4::engine
