#pragma once

#include "core/math/numerics.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Quat {
    float w{};
    float x{};
    float y{};
    float z{};

    constexpr Quat() = default;
    constexpr Quat(float ww, float xx, float yy, float zz) : w(ww), x(xx), y(yy), z(zz) {}

    [[nodiscard]] static constexpr auto identity() -> Quat { return {1.0f, 0.0f, 0.0f, 0.0f}; }

    [[nodiscard]] static auto fromAxisAngle(const Vec3& axis, float radiansAngle) -> Quat {
        const Vec3 n = f4::math::normalized(axis);
        const float half = radiansAngle * 0.5f;
        const float s = std::sin(half);
        return {std::cos(half), n.x * s, n.y * s, n.z * s};
    }

    [[nodiscard]] static auto fromEuler(float yawRadians, float pitchRadians, float rollRadians) -> Quat {
        const Quat yaw = fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), yawRadians);
        const Quat pitch = fromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), pitchRadians);
        const Quat roll = fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), rollRadians);
        return yaw * pitch * roll;
    }

    constexpr auto operator-() const -> Quat { return {-w, -x, -y, -z}; }

    friend constexpr auto operator*(const Quat& a, const Quat& b) -> Quat {
        return {
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    friend constexpr auto operator+(const Quat& a, const Quat& b) -> Quat { return {a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr auto operator-(const Quat& a, const Quat& b) -> Quat { return {a.w - b.w, a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr auto operator*(const Quat& q, float s) -> Quat { return {q.w * s, q.x * s, q.y * s, q.z * s}; }
    friend constexpr auto operator*(float s, const Quat& q) -> Quat { return q * s; }

    friend constexpr auto operator==(const Quat& a, const Quat& b) -> bool { return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z; }
    friend constexpr auto operator!=(const Quat& a, const Quat& b) -> bool { return !(a == b); }

    [[nodiscard]] friend constexpr auto dot(const Quat& a, const Quat& b) -> float {
        return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    }

    [[nodiscard]] constexpr auto conj() const -> Quat { return {w, -x, -y, -z}; }

    [[nodiscard]] constexpr auto lengthSq() const -> float { return dot(*this, *this); }

    [[nodiscard]] auto length() const -> float { return std::sqrt(lengthSq()); }

    [[nodiscard]] auto normalized() const -> Quat {
        const float len = length();
        if (len <= kEpsilon) {
            return identity();
        }
        const float inv = 1.0f / len;
        return {w * inv, x * inv, y * inv, z * inv};
    }

    [[nodiscard]] auto inverse() const -> Quat {
        const float ls = lengthSq();
        const float inv = 1.0f / ls;
        return {w * inv, -x * inv, -y * inv, -z * inv};
    }

    [[nodiscard]] constexpr auto rotate(const Vec3& v) const -> Vec3 {
        const Vec3 u{x, y, z};
        const Vec3 t = 2.0f * cross(u, v);
        return v + w * t + cross(u, t);
    }

    [[nodiscard]] static auto slerp(const Quat& a, const Quat& b, float t) -> Quat {
        Quat end = b;
        float d = dot(a, b);
        if (d < 0.0f) {
            end = -b;
            d = -d;
        }
        if (d > 0.9995f) {
            return (a + (end - a) * t).normalized();
        }
        const float theta0 = std::acos(clamp(d, -1.0f, 1.0f));
        const float theta = theta0 * t;
        const float sinTheta0 = std::sin(theta0);
        const float sinTheta = std::sin(theta);
        const float s0 = std::cos(theta) - d * sinTheta / sinTheta0;
        const float s1 = sinTheta / sinTheta0;
        return a * s0 + end * s1;
    }
};

[[nodiscard]] inline auto quatFromAxisAngle(const Vec3& axis, float radiansAngle) -> Quat {
    return Quat::fromAxisAngle(axis, radiansAngle);
}

} // namespace f4::math
