#pragma once

#include <cmath>

#include "core/ecs/registry.h"
#include "core/ecs/view.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"

namespace f4::engine {

// Orbit camera: deterministic lookAtRH + perspectiveRH_ZO (D3D 0..1 depth).
// Entity without a Camera is not a camera; multiple cameras are allowed
// (renderer picks one). Does NOT depend on WorldMatrix — eye/target are
// explicit so the orbit path is fully deterministic for parity tests.
struct Camera {
    float fovYRadians = 1.04719758f; // 60 deg
    float nearPlane = 0.1f;
    float farPlane = 100.f;
    math::Vec3 eye{0.f, 2.f, 5.f};
    math::Vec3 center{0.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};
    math::Mat4 viewProj = math::Mat4::identity();
};

[[nodiscard]] inline auto cameraView(const Camera& c) -> math::Mat4 {
    return math::Mat4::lookAtRH(c.eye, c.center, c.up);
}

[[nodiscard]] inline auto cameraProj(const Camera& c, float aspect) -> math::Mat4 {
    return math::Mat4::perspectiveRH_ZO(c.fovYRadians, aspect, c.nearPlane, c.farPlane);
}

[[nodiscard]] inline auto cameraViewProj(const Camera& c, float aspect) -> math::Mat4 {
    return cameraProj(c, aspect).mul(cameraView(c));
}

// Recomputes viewProj for every Camera in the registry. `aspect` is
// framebufferWidth / framebufferHeight (1.0 if height is 0).
inline void updateCameras(ecs::Registry& registry, float aspect) {
    if (aspect <= 0.f) {
        aspect = 1.f;
    }
    for (auto [entity, camera] : ecs::View<Camera>(registry)) {
        (void)entity;
        camera.viewProj = cameraViewProj(camera, aspect);
    }
}

// Convenience orbit helper: eye on a horizontal circle around center.
[[nodiscard]] inline auto orbitEye(float radius, float azimuthRadians, float height,
                                   const math::Vec3& center) -> math::Vec3 {
    const float ca = std::cos(azimuthRadians);
    const float sa = std::sin(azimuthRadians);
    return {center.x + ca * radius, center.y + height, center.z + sa * radius};
}

} // namespace f4::engine
