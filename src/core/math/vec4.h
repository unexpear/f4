#pragma once

#include "core/math/numerics.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Vec4 {
    float x{};
    float y{};
    float z{};
    float w{};

    constexpr Vec4() = default;
    constexpr Vec4(float xx, float yy, float zz, float ww) : x(xx), y(yy), z(zz), w(ww) {}
    constexpr explicit Vec4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr Vec4(const Vec3& v, float ww) : x(v.x), y(v.y), z(v.z), w(ww) {}

    constexpr auto operator[](int i) -> float& { return (&x)[i]; }
    constexpr auto operator[](int i) const -> float { return (&x)[i]; }

    [[nodiscard]] constexpr auto xyz() const -> Vec3 { return {x, y, z}; }

    friend constexpr auto operator+(const Vec4& a, const Vec4& b) -> Vec4 { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
    friend constexpr auto operator-(const Vec4& a, const Vec4& b) -> Vec4 { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
    friend constexpr auto operator*(const Vec4& a, const Vec4& b) -> Vec4 { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
    friend constexpr auto operator*(const Vec4& a, float s) -> Vec4 { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
    friend constexpr auto operator*(float s, const Vec4& a) -> Vec4 { return a * s; }
    friend constexpr auto operator/(const Vec4& a, float s) -> Vec4 { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
    friend constexpr auto operator-(const Vec4& a) -> Vec4 { return {-a.x, -a.y, -a.z, -a.w}; }

    friend constexpr auto operator+=(Vec4& a, const Vec4& b) -> Vec4& { a = a + b; return a; }
    friend constexpr auto operator-=(Vec4& a, const Vec4& b) -> Vec4& { a = a - b; return a; }
    friend constexpr auto operator*=(Vec4& a, float s) -> Vec4& { a = a * s; return a; }

    friend constexpr auto operator==(const Vec4& a, const Vec4& b) -> bool { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
    friend constexpr auto operator!=(const Vec4& a, const Vec4& b) -> bool { return !(a == b); }
};

[[nodiscard]] constexpr auto dot(const Vec4& a, const Vec4& b) -> float {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] inline auto lengthSq(const Vec4& v) -> float {
    return dot(v, v);
}

[[nodiscard]] inline auto length(const Vec4& v) -> float {
    return std::sqrt(lengthSq(v));
}

[[nodiscard]] constexpr auto lerp(const Vec4& a, const Vec4& b, float t) -> Vec4 {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)};
}

[[nodiscard]] constexpr auto minComponentwise(const Vec4& a, const Vec4& b) -> Vec4 {
    return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w)};
}

[[nodiscard]] constexpr auto maxComponentwise(const Vec4& a, const Vec4& b) -> Vec4 {
    return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w)};
}

} // namespace f4::math
