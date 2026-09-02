#include "engine/rendering/shader_parity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f4::engine {

auto compareShaderFloats(std::span<const float> reference, std::span<const float> actual,
                         math::Tolerance tolerance) -> ShaderFloatParityReport {
    ShaderFloatParityReport report;
    if (reference.size() != actual.size()) {
        report.status = ShaderParityStatus::SizeMismatch;
        return report;
    }
    if (!std::isfinite(tolerance.abs) || !std::isfinite(tolerance.rel) ||
        tolerance.abs < 0.0f || tolerance.rel < 0.0f) {
        report.status = ShaderParityStatus::InvalidTolerance;
        return report;
    }

    report.sampleCount = reference.size();
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const float expected = reference[i];
        const float observed = actual[i];
        if (!std::isfinite(expected) || !std::isfinite(observed)) {
            if (report.mismatchCount == 0) report.worstSample = i;
            ++report.mismatchCount;
            report.status = ShaderParityStatus::NonFinite;
            continue;
        }

        const float absoluteError = std::fabs(expected - observed);
        const float scale = std::max(std::fabs(expected), std::fabs(observed));
        const float relativeError =
            scale > std::numeric_limits<float>::min() ? absoluteError / scale : 0.0f;
        if (absoluteError > report.maxAbsoluteError) {
            report.maxAbsoluteError = absoluteError;
            report.worstSample = i;
        }
        report.maxRelativeError = std::max(report.maxRelativeError, relativeError);
        if (!math::within(expected, observed, tolerance)) {
            ++report.mismatchCount;
            if (report.status == ShaderParityStatus::Match) {
                report.status = ShaderParityStatus::OutsideTolerance;
            }
        }
    }
    return report;
}

auto compareShaderImagesRgba8(std::span<const std::uint8_t> reference,
                              std::span<const std::uint8_t> actual,
                              std::uint8_t allowedChannelDelta)
    -> ShaderImageParityReport {
    ShaderImageParityReport report;
    if (reference.size() != actual.size() || (reference.size() % 4) != 0) {
        report.status = ShaderParityStatus::SizeMismatch;
        return report;
    }

    report.pixelCount = reference.size() / 4;
    for (std::size_t pixel = 0; pixel < report.pixelCount; ++pixel) {
        bool pixelMismatch = false;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const std::size_t index = pixel * 4 + channel;
            const int delta = std::abs(static_cast<int>(reference[index]) -
                                       static_cast<int>(actual[index]));
            report.maxChannelDelta =
                std::max(report.maxChannelDelta, static_cast<std::uint8_t>(delta));
            pixelMismatch = pixelMismatch || delta > allowedChannelDelta;
        }
        if (pixelMismatch) {
            if (report.mismatchPixelCount == 0) report.firstMismatchPixel = pixel;
            ++report.mismatchPixelCount;
        }
    }
    if (report.mismatchPixelCount != 0) {
        report.status = ShaderParityStatus::OutsideTolerance;
    }
    return report;
}

} // namespace f4::engine
