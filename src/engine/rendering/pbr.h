#pragma once

#include <cstdint>
#include <span>

#include "core/math/vec2.h"
#include "core/math/vec3.h"

namespace f4::engine {

// CPU reference for the metallic-roughness shader path. Tests use these
// routines to lock the BRDF, color-space, shadow-filter, and tone-map policy.
struct PbrDirectInput {
    math::Vec3 baseColor{1.0f};
    math::Vec3 normal{0.0f, 0.0f, 1.0f};
    math::Vec3 viewDirection{0.0f, 0.0f, 1.0f};
    math::Vec3 lightDirection{0.0f, 0.0f, 1.0f};
    math::Vec3 lightRadiance{1.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
};

struct PbrDirectResult {
    math::Vec3 diffuse{};
    math::Vec3 specular{};
    math::Vec3 total{};
};

[[nodiscard]] auto srgbToLinear(const math::Vec3& color) -> math::Vec3;
[[nodiscard]] auto linearToSrgb(const math::Vec3& color) -> math::Vec3;
[[nodiscard]] auto fresnelSchlick(float cosTheta, const math::Vec3& f0) -> math::Vec3;
[[nodiscard]] auto distributionGgx(float nDotH, float roughness) -> float;
[[nodiscard]] auto geometrySmith(float nDotV, float nDotL, float roughness) -> float;
[[nodiscard]] auto evaluatePbrDirect(const PbrDirectInput& input) -> PbrDirectResult;

// Khronos/Filament-compatible latitude-longitude mapping: +Y is the pole and
// +Z is the center of the texture. U wraps, V is clamped.
[[nodiscard]] auto directionToEquirectUv(const math::Vec3& direction) -> math::Vec2;

// Manual percentage-closer filtering used by every backend. Samples outside
// the light frustum are lit and the source is a normalized 0..1 depth image.
[[nodiscard]] auto shadowPcf(std::span<const float> depths, std::uint32_t width,
                             std::uint32_t height, const math::Vec2& uv,
                             float receiverDepth, float bias,
                             std::uint32_t radius = 1) -> float;

[[nodiscard]] auto acesToneMap(const math::Vec3& hdr, float exposure = 1.0f)
    -> math::Vec3;

} // namespace f4::engine
