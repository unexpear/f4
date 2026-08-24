#pragma once

#include <doctest/doctest.h>
#include <ostream>

#include "core/math/comparison.h"

namespace f4::math {

inline auto operator<<(std::ostream& os, const Vec2& v) -> std::ostream& {
    return os << '(' << v.x << ", " << v.y << ')';
}

inline auto operator<<(std::ostream& os, const Vec3& v) -> std::ostream& {
    return os << '(' << v.x << ", " << v.y << ", " << v.z << ')';
}

inline auto operator<<(std::ostream& os, const Vec4& v) -> std::ostream& {
    return os << '(' << v.x << ", " << v.y << ", " << v.z << ", " << v.w << ')';
}

inline auto operator<<(std::ostream& os, const Quat& q) -> std::ostream& {
    return os << "(w" << q.w << " xyz(" << q.x << ", " << q.y << ", " << q.z << "))";
}

template <int N>
inline auto printMat(std::ostream& os, const float* values) -> std::ostream& {
    os << '[';
    for (int i = 0; i < N * N; ++i) {
        if (i != 0) {
            os << (i % N == 0 ? "; " : ", ");
        }
        os << values[i];
    }
    return os << ']';
}

inline auto operator<<(std::ostream& os, const Mat3& m) -> std::ostream& {
    float rowMajor[9];
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            rowMajor[r * 3 + c] = m(r, c);
        }
    }
    return printMat<3>(os, rowMajor);
}

inline auto operator<<(std::ostream& os, const Mat4& m) -> std::ostream& {
    float rowMajor[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            rowMajor[r * 4 + c] = m(r, c);
        }
    }
    return printMat<4>(os, rowMajor);
}

} // namespace f4::math

namespace f4test {

using f4::math::kDefaultTolerance;
using f4::math::kLooseTolerance;
using f4::math::kTightTolerance;
using f4::math::Tolerance;

[[nodiscard]] inline auto feq(float a, float b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto veq(const f4::math::Vec2& a, const f4::math::Vec2& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto veq(const f4::math::Vec3& a, const f4::math::Vec3& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto veq(const f4::math::Vec4& a, const f4::math::Vec4& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto qeq(const f4::math::Quat& a, const f4::math::Quat& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto meq(const f4::math::Mat3& a, const f4::math::Mat3& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

[[nodiscard]] inline auto meq(const f4::math::Mat4& a, const f4::math::Mat4& b, const Tolerance& tol = kDefaultTolerance) -> bool {
    return within(a, b, tol);
}

#define F4_CHECK_NEAR(actual, expected, ...)                                             \
    do {                                                                                 \
        INFO("actual=" << (actual) << "\n\texpected=" << (expected));                    \
        CHECK(f4::math::within((actual), (expected), ##__VA_ARGS__));                    \
    } while (false)

} // namespace f4test
