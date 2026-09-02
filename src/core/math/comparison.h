#pragma once

#include <cmath>

#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/numerics.h"
#include "core/math/quat.h"
#include "core/math/vec2.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"

namespace f4::math {

// Adaptive floating-point tolerance: an absolute floor handles cancellation
// near zero; the relative term scales with operand magnitude (Dawson 2012,
// "Comparing Floating Point Numbers, 2012 Edition").
//
// Extension seam: ULP-based comparison (ulpDistance via std::bit_cast) is a
// deliberate future addition to this header for GPU-parity determinism tests;
// all approximate comparisons must funnel through here so that extension
// requires no call-site changes.
struct Tolerance {
    float abs{0.0f};
    float rel{0.0f};
};

inline constexpr Tolerance kTightTolerance{1e-7f, 1e-6f};
inline constexpr Tolerance kDefaultTolerance{1e-6f, 1e-5f};
inline constexpr Tolerance kLooseTolerance{1e-4f, 1e-3f};
inline constexpr Tolerance kParityTolerance{1e-5f, 1e-4f};

[[nodiscard]] inline auto within(float a, float b, const Tolerance& t = kDefaultTolerance) -> bool {
    if (a == b) {
        return true;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return false;
    }
    const float diff = std::fabs(a - b);
    return diff <= t.abs || diff <= t.rel * max(std::fabs(a), std::fabs(b));
}

[[nodiscard]] inline auto within(float a, float b, float referenceScale, const Tolerance& t = kDefaultTolerance) -> bool {
    if (a == b) {
        return true;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return false;
    }
    const float diff = std::fabs(a - b);
    return diff <= t.abs || diff <= t.rel * referenceScale;
}

[[nodiscard]] inline auto within(const Vec2& a, const Vec2& b, const Tolerance& t = kDefaultTolerance) -> bool {
    return within(a.x, b.x, t) && within(a.y, b.y, t);
}

[[nodiscard]] inline auto within(const Vec3& a, const Vec3& b, const Tolerance& t = kDefaultTolerance) -> bool {
    return within(a.x, b.x, t) && within(a.y, b.y, t) && within(a.z, b.z, t);
}

[[nodiscard]] inline auto within(const Vec4& a, const Vec4& b, const Tolerance& t = kDefaultTolerance) -> bool {
    return within(a.xyz(), b.xyz(), t) && within(a.w, b.w, t);
}

[[nodiscard]] inline auto within(const Quat& a, const Quat& b, const Tolerance& t = kDefaultTolerance) -> bool {
    return within(a.w, b.w, t) && within(a.x, b.x, t) && within(a.y, b.y, t) && within(a.z, b.z, t);
}

[[nodiscard]] inline auto within(const Mat3& a, const Mat3& b, const Tolerance& t = kDefaultTolerance) -> bool {
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            if (!within(a(r, c), b(r, c), t)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] inline auto within(const Mat4& a, const Mat4& b, const Tolerance& t = kDefaultTolerance) -> bool {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!within(a(r, c), b(r, c), t)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace f4::math
