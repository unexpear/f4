#pragma once

#include "core/math/numerics.h"

namespace f4::math {

struct Vec3 {
    float x{};
    float y{};
    float z{};

    constexpr Vec3() = default;
    constexpr Vec3(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}

    constexpr auto operator[](int i) -> float& { return (&x)[i]; }
    constexpr auto operator[](int i) const -> float { return (&x)[i]; }

    friend constexpr auto operator+(const Vec3& a, const Vec3& b) -> Vec3 { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr auto operator-(const Vec3& a, const Vec3& b) -> Vec3 { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr auto operator*(const Vec3& a, const Vec3& b) -> Vec3 { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    friend constexpr auto operator/(const Vec3& a, const Vec3& b) -> Vec3 { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
    friend constexpr auto operator*(const Vec3& a, float s) -> Vec3 { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr auto operator*(float s, const Vec3& a) -> Vec3 { return a * s; }
    friend constexpr auto operator/(const Vec3& a, float s) -> Vec3 { return {a.x / s, a.y / s, a.z / s}; }
    friend constexpr auto operator-(const Vec3& a) -> Vec3 { return {-a.x, -a.y, -a.z}; }

    friend constexpr auto operator+=(Vec3& a, const Vec3& b) -> Vec3& { a = a + b; return a; }
    friend constexpr auto operator-=(Vec3& a, const Vec3& b) -> Vec3& { a = a - b; return a; }
    friend constexpr auto operator*=(Vec3& a, float s) -> Vec3& { a = a * s; return a; }
    friend constexpr auto operator/=(Vec3& a, float s) -> Vec3& { a = a / s; return a; }

    friend constexpr auto operator==(const Vec3& a, const Vec3& b) -> bool { return a.x == b.x && a.y == b.y && a.z == b.z; }
    friend constexpr auto operator!=(const Vec3& a, const Vec3& b) -> bool { return !(a == b); }
};

[[nodiscard]] constexpr auto dot(const Vec3& a, const Vec3& b) -> float {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr auto cross(const Vec3& a, const Vec3& b) -> Vec3 {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr auto lengthSq(const Vec3& v) -> float {
    return dot(v, v);
}

[[nodiscard]] inline auto length(const Vec3& v) -> float {
    return std::sqrt(lengthSq(v));
}

[[nodiscard]] inline auto distance(const Vec3& a, const Vec3& b) -> float {
    return length(b - a);
}

[[nodiscard]] inline auto normalized(const Vec3& v) -> Vec3 {
    const float len = length(v);
    return len > kEpsilon ? v / len : Vec3(0.0f);
}

[[nodiscard]] inline auto reflect(const Vec3& incident, const Vec3& n) -> Vec3 {
    return incident - 2.0f * dot(n, incident) * n;
}

[[nodiscard]] constexpr auto lerp(const Vec3& a, const Vec3& b, float t) -> Vec3 {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

[[nodiscard]] constexpr auto minComponentwise(const Vec3& a, const Vec3& b) -> Vec3 {
    return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)};
}

[[nodiscard]] constexpr auto maxComponentwise(const Vec3& a, const Vec3& b) -> Vec3 {
    return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
}

} // namespace f4::math
