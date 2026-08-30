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

// Point light stub for M8 — stored, not consumed by the forward pass.
// radius is the influence radius (linear falloff to 0 at edge in future shading).
struct PointLight {
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Vec3 color{1.f, 1.f, 1.f};
    float intensity = 1.f;
    float range = 10.f;
};

} // namespace f4::engine
