#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/math/comparison.h"

namespace f4::engine {

enum class ShaderParityStatus : std::uint8_t {
    Match,
    SizeMismatch,
    InvalidTolerance,
    NonFinite,
    OutsideTolerance,
};

struct ShaderFloatParityReport {
    ShaderParityStatus status = ShaderParityStatus::Match;
    std::size_t sampleCount = 0;
    std::size_t mismatchCount = 0;
    std::size_t worstSample = 0;
    float maxAbsoluteError = 0.0f;
    float maxRelativeError = 0.0f;

    [[nodiscard]] auto matches() const -> bool { return status == ShaderParityStatus::Match; }
};

// Uses the core math tolerance contract (absolute OR relative threshold).
// Non-finite samples fail explicitly instead of depending on NaN behavior.
[[nodiscard]] auto compareShaderFloats(std::span<const float> reference,
                                       std::span<const float> actual,
                                       math::Tolerance tolerance = math::kParityTolerance)
    -> ShaderFloatParityReport;

struct ShaderImageParityReport {
    ShaderParityStatus status = ShaderParityStatus::Match;
    std::size_t pixelCount = 0;
    std::size_t mismatchPixelCount = 0;
    std::size_t firstMismatchPixel = 0;
    std::uint8_t maxChannelDelta = 0;

    [[nodiscard]] auto matches() const -> bool { return status == ShaderParityStatus::Match; }
};

// Exact pixel parity gate for tightly packed RGBA8 reference captures.
[[nodiscard]] auto compareShaderImagesRgba8(std::span<const std::uint8_t> reference,
                                            std::span<const std::uint8_t> actual)
    -> ShaderImageParityReport;

} // namespace f4::engine
