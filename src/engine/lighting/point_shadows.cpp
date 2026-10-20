#include "engine/lighting/point_shadows.h"

#include <algorithm>
#include <cmath>

namespace f4::engine {

auto pointShadowNearPlane(float range) -> float {
    return std::max(range * 0.02f, 0.01f);
}

auto pointShadowFaceViewProj(const math::Vec3& position, float range, std::uint32_t face)
    -> math::Mat4 {
    static constexpr float kDirections[kPointShadowFaces][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static constexpr float kUps[kPointShadowFaces][3] = {
        {0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
    const std::uint32_t index = std::min(face, kPointShadowFaces - 1u);
    const math::Vec3 direction(kDirections[index][0], kDirections[index][1],
                               kDirections[index][2]);
    const math::Vec3 up(kUps[index][0], kUps[index][1], kUps[index][2]);
    const float farPlane = std::max(range, 0.02f);
    const float nearPlane = std::min(pointShadowNearPlane(range), farPlane * 0.5f);
    const math::Mat4 view = math::Mat4::lookAtRH(position, position + direction, up);
    const math::Mat4 projection =
        math::Mat4::perspectiveRH_ZO(1.57079632679f, 1.0f, nearPlane, farPlane);
    return projection.mul(view);
}

auto pointShadowTileTransform(std::uint32_t slot, std::uint32_t face, std::uint32_t rows)
    -> math::Vec4 {
    const float columns = static_cast<float>(kPointShadowFaces);
    const float rowCount = static_cast<float>(std::max(rows, 1u));
    return {1.0f / columns, 1.0f / rowCount,
            -1.0f + (2.0f * static_cast<float>(face) + 1.0f) / columns,
            -1.0f + (2.0f * static_cast<float>(slot) + 1.0f) / rowCount};
}

auto pointShadowFaceFor(const math::Vec3& d) -> std::uint32_t {
    const float ax = std::abs(d.x);
    const float ay = std::abs(d.y);
    const float az = std::abs(d.z);
    if (ax >= ay && ax >= az) return d.x >= 0.0f ? 0u : 1u;
    if (ay >= az) return d.y >= 0.0f ? 2u : 3u;
    return d.z >= 0.0f ? 4u : 5u;
}

void selectShadowedPointLights(std::span<const PointShadowCandidate> candidates,
                               const math::Vec3& eye, std::uint32_t budget,
                               containers::Array<std::uint32_t>& selected) {
    selected.clear();
    if (budget == 0 || candidates.empty()) return;
    struct Scored {
        float score;
        std::uint32_t index;
    };
    containers::Array<Scored> scored;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const PointShadowCandidate& light = candidates[i];
        const float peak =
            std::max(light.radiance.x, std::max(light.radiance.y, light.radiance.z));
        if (!(light.range > 0.0f) || !(peak > 0.0f)) continue;
        const float distance =
            std::max(math::length(light.position - eye) - light.range, 0.0f);
        scored.push({peak / (1.0f + distance * distance), static_cast<std::uint32_t>(i)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
    });
    const std::size_t count = std::min<std::size_t>(scored.size(), budget);
    for (std::size_t i = 0; i < count; ++i) selected.push(scored[i].index);
}

} // namespace f4::engine
