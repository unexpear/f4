#pragma once

#include <cmath>

namespace f4::math {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = kPi * 2.0f;
inline constexpr float kHalfPi = kPi * 0.5f;
inline constexpr float kEpsilon = 1e-6f;

constexpr auto radians(float deg) -> float {
    return deg * (kPi / 180.0f);
}

constexpr auto degrees(float rad) -> float {
    return rad * (180.0f / kPi);
}

template <typename T>
constexpr auto min(T a, T b) -> T {
    return a < b ? a : b;
}

template <typename T>
constexpr auto max(T a, T b) -> T {
    return a > b ? a : b;
}

template <typename T>
constexpr auto clamp(T v, T lo, T hi) -> T {
    return v < lo ? lo : (v > hi ? hi : v);
}

template <typename T>
constexpr auto lerp(T a, T b, float t) -> T {
    return a + (b - a) * t;
}

constexpr auto saturate(float v) -> float {
    return clamp(v, 0.0f, 1.0f);
}

inline auto approx(float a, float b, float eps = kEpsilon) -> bool {
    return std::fabs(a - b) <= eps;
}

inline auto approxZero(float v, float eps = kEpsilon) -> bool {
    return std::fabs(v) <= eps;
}

inline auto sign(float v) -> float {
    if (v > 0.0f) return 1.0f;
    if (v < 0.0f) return -1.0f;
    return 0.0f;
}

} // namespace f4::math
