#pragma once

#include "core/math/numerics.h"

namespace f4::math {

struct Vec2 {
    float x{};
    float y{};

    constexpr Vec2() = default;
    constexpr Vec2(float xx, float yy) : x(xx), y(yy) {}
    constexpr explicit Vec2(float s) : x(s), y(s) {}

    constexpr auto operator[](int i) -> float& { return (&x)[i]; }
    constexpr auto operator[](int i) const -> float { return (&x)[i]; }

    friend constexpr auto operator+(const Vec2& a, const Vec2& b) -> Vec2 { return {a.x + b.x, a.y + b.y}; }
    friend constexpr auto operator-(const Vec2& a, const Vec2& b) -> Vec2 { return {a.x - b.x, a.y - b.y}; }
    friend constexpr auto operator*(const Vec2& a, const Vec2& b) -> Vec2 { return {a.x * b.x, a.y * b.y}; }
    friend constexpr auto operator/(const Vec2& a, const Vec2& b) -> Vec2 { return {a.x / b.x, a.y / b.y}; }
    friend constexpr auto operator*(const Vec2& a, float s) -> Vec2 { return {a.x * s, a.y * s}; }
    friend constexpr auto operator*(float s, const Vec2& a) -> Vec2 { return a * s; }
    friend constexpr auto operator/(const Vec2& a, float s) -> Vec2 { return {a.x / s, a.y / s}; }
    friend constexpr auto operator-(const Vec2& a) -> Vec2 { return {-a.x, -a.y}; }

    friend constexpr auto operator+=(Vec2& a, const Vec2& b) -> Vec2& { a = a + b; return a; }
    friend constexpr auto operator-=(Vec2& a, const Vec2& b) -> Vec2& { a = a - b; return a; }
    friend constexpr auto operator*=(Vec2& a, float s) -> Vec2& { a = a * s; return a; }
    friend constexpr auto operator/=(Vec2& a, float s) -> Vec2& { a = a / s; return a; }

    friend constexpr auto operator==(const Vec2& a, const Vec2& b) -> bool { return a.x == b.x && a.y == b.y; }
    friend constexpr auto operator!=(const Vec2& a, const Vec2& b) -> bool { return !(a == b); }
};

[[nodiscard]] constexpr auto dot(const Vec2& a, const Vec2& b) -> float {
    return a.x * b.x + a.y * b.y;
}

[[nodiscard]] inline auto lengthSq(const Vec2& v) -> float {
    return dot(v, v);
}

[[nodiscard]] inline auto length(const Vec2& v) -> float {
    return std::sqrt(lengthSq(v));
}

[[nodiscard]] inline auto normalized(const Vec2& v) -> Vec2 {
    const float len = length(v);
    return len > kEpsilon ? v / len : Vec2(0.0f);
}

[[nodiscard]] constexpr auto perp(const Vec2& v) -> Vec2 {
    return {-v.y, v.x};
}

[[nodiscard]] constexpr auto lerp(const Vec2& a, const Vec2& b, float t) -> Vec2 {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}

[[nodiscard]] constexpr auto minComponentwise(const Vec2& a, const Vec2& b) -> Vec2 {
    return {min(a.x, b.x), min(a.y, b.y)};
}

[[nodiscard]] constexpr auto maxComponentwise(const Vec2& a, const Vec2& b) -> Vec2 {
    return {max(a.x, b.x), max(a.y, b.y)};
}

} // namespace f4::math
