#include "engine/rendering/pbr.h"

#include <algorithm>
#include <cmath>

namespace f4::engine {

namespace {

constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] auto saturate(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] auto srgbChannelToLinear(float value) -> float {
    const float c = std::max(value, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] auto linearChannelToSrgb(float value) -> float {
    const float c = std::max(value, 0.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] auto mul(const math::Vec3& a, const math::Vec3& b) -> math::Vec3 {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

} // namespace

auto srgbToLinear(const math::Vec3& color) -> math::Vec3 {
    return {srgbChannelToLinear(color.x), srgbChannelToLinear(color.y),
            srgbChannelToLinear(color.z)};
}

auto linearToSrgb(const math::Vec3& color) -> math::Vec3 {
    return {linearChannelToSrgb(color.x), linearChannelToSrgb(color.y),
            linearChannelToSrgb(color.z)};
}

auto fresnelSchlick(float cosTheta, const math::Vec3& f0) -> math::Vec3 {
    const float oneMinusCos = 1.0f - saturate(cosTheta);
    const float factor = oneMinusCos * oneMinusCos * oneMinusCos * oneMinusCos * oneMinusCos;
    return f0 + (math::Vec3(1.0f) - f0) * factor;
}

auto distributionGgx(float nDotH, float roughness) -> float {
    const float r = std::clamp(roughness, 0.045f, 1.0f);
    const float alpha = r * r;
    const float alpha2 = alpha * alpha;
    const float nh = saturate(nDotH);
    const float denominator = nh * nh * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / std::max(kPi * denominator * denominator, 1.0e-7f);
}

auto geometrySmith(float nDotV, float nDotL, float roughness) -> float {
    const float r = std::clamp(roughness, 0.045f, 1.0f);
    const float k = ((r + 1.0f) * (r + 1.0f)) * 0.125f;
    const auto schlick = [k](float cosine) {
        const float c = saturate(cosine);
        return c / std::max(c * (1.0f - k) + k, 1.0e-7f);
    };
    return schlick(nDotV) * schlick(nDotL);
}

auto evaluatePbrDirect(const PbrDirectInput& input) -> PbrDirectResult {
    const math::Vec3 n = math::normalized(input.normal);
    const math::Vec3 v = math::normalized(input.viewDirection);
    const math::Vec3 l = math::normalized(input.lightDirection);
    const math::Vec3 h = math::normalized(v + l);
    const float nDotV = saturate(math::dot(n, v));
    const float nDotL = saturate(math::dot(n, l));
    const float nDotH = saturate(math::dot(n, h));
    const float hDotV = saturate(math::dot(h, v));
    const float metallic = saturate(input.metallic);
    const float roughness = std::clamp(input.roughness, 0.045f, 1.0f);
    const math::Vec3 base = math::maxComponentwise(input.baseColor, math::Vec3(0.0f));
    const math::Vec3 f0 = math::lerp(math::Vec3(0.04f), base, metallic);
    const math::Vec3 fresnel = fresnelSchlick(hDotV, f0);
    const float denominator = std::max(4.0f * nDotV * nDotL, 1.0e-5f);
    const math::Vec3 specular = fresnel *
        (distributionGgx(nDotH, roughness) * geometrySmith(nDotV, nDotL, roughness) /
         denominator);
    const math::Vec3 diffuse = mul(math::Vec3(1.0f) - fresnel, base) *
                               ((1.0f - metallic) / kPi);

    PbrDirectResult result;
    result.diffuse = mul(diffuse, input.lightRadiance) * nDotL;
    result.specular = mul(specular, input.lightRadiance) * nDotL;
    result.total = result.diffuse + result.specular;
    return result;
}

auto directionToEquirectUv(const math::Vec3& direction) -> math::Vec2 {
    const math::Vec3 d = math::normalized(direction);
    if (math::lengthSq(d) == 0.0f) return {0.5f, 0.5f};
    const float u = std::atan2(d.x, d.z) / (2.0f * kPi) + 0.5f;
    const float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
    return {u - std::floor(u), v};
}

auto shadowPcf(std::span<const float> depths, std::uint32_t width,
               std::uint32_t height, const math::Vec2& uv, float receiverDepth,
               float bias, std::uint32_t radius) -> float {
    if (width == 0 || height == 0 || depths.size() !=
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return 1.0f;
    }
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ||
        receiverDepth < 0.0f || receiverDepth > 1.0f) {
        return 1.0f;
    }

    const int centerX = static_cast<int>(uv.x * static_cast<float>(width - 1));
    const int centerY = static_cast<int>(uv.y * static_cast<float>(height - 1));
    float lit = 0.0f;
    std::uint32_t sampleCount = 0;
    const int extent = static_cast<int>(radius);
    for (int y = -extent; y <= extent; ++y) {
        for (int x = -extent; x <= extent; ++x) {
            const int sx = std::clamp(centerX + x, 0, static_cast<int>(width) - 1);
            const int sy = std::clamp(centerY + y, 0, static_cast<int>(height) - 1);
            const float mapDepth = depths[static_cast<std::size_t>(sy) * width +
                                          static_cast<std::size_t>(sx)];
            lit += receiverDepth - std::max(bias, 0.0f) <= mapDepth ? 1.0f : 0.0f;
            ++sampleCount;
        }
    }
    return lit / static_cast<float>(sampleCount);
}

auto acesToneMap(const math::Vec3& hdr, float exposure) -> math::Vec3 {
    const math::Vec3 x = math::maxComponentwise(hdr * std::max(exposure, 0.0f),
                                                 math::Vec3(0.0f));
    const auto channel = [](float value) {
        constexpr float a = 2.51f;
        constexpr float b = 0.03f;
        constexpr float c = 2.43f;
        constexpr float d = 0.59f;
        constexpr float e = 0.14f;
        return std::clamp((value * (a * value + b)) /
                              (value * (c * value + d) + e),
                          0.0f, 1.0f);
    };
    return {channel(x.x), channel(x.y), channel(x.z)};
}

} // namespace f4::engine
