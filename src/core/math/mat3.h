#pragma once

#include "core/math/numerics.h"
#include "core/math/quat.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Mat3 {
    Vec3 col[3];

    constexpr Mat3() : col{Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)} {}
    constexpr explicit Mat3(const Vec3& c0, const Vec3& c1, const Vec3& c2) : col{c0, c1, c2} {}

    constexpr auto operator()(int r, int c) const -> float { return col[c][r]; }

    [[nodiscard]] static constexpr auto identity() -> Mat3 { return {}; }

    [[nodiscard]] static auto fromQuat(const Quat& q) -> Mat3 {
        const float xx = q.x * q.x;
        const float yy = q.y * q.y;
        const float zz = q.z * q.z;
        const float xy = q.x * q.y;
        const float xz = q.x * q.z;
        const float yz = q.y * q.z;
        const float wx = q.w * q.x;
        const float wy = q.w * q.y;
        const float wz = q.w * q.z;
        return Mat3{
            Vec3(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)),
            Vec3(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)),
            Vec3(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy))};
    }

    [[nodiscard]] constexpr auto mul(const Mat3& m) const -> Mat3 {
        return Mat3{
            col[0] * m.col[0].x + col[1] * m.col[0].y + col[2] * m.col[0].z,
            col[0] * m.col[1].x + col[1] * m.col[1].y + col[2] * m.col[1].z,
            col[0] * m.col[2].x + col[1] * m.col[2].y + col[2] * m.col[2].z};
    }

    [[nodiscard]] constexpr auto mulDir(const Vec3& v) const -> Vec3 {
        return col[0] * v.x + col[1] * v.y + col[2] * v.z;
    }

    [[nodiscard]] constexpr auto transpose() const -> Mat3 {
        return Mat3{
            Vec3(col[0].x, col[1].x, col[2].x),
            Vec3(col[0].y, col[1].y, col[2].y),
            Vec3(col[0].z, col[1].z, col[2].z)};
    }

    [[nodiscard]] auto determinant() const -> float {
        return dot(cross(col[0], col[1]), col[2]);
    }

    [[nodiscard]] auto inverse() const -> Mat3 {
        const float det = determinant();
        const float invDet = 1.0f / det;
        return Mat3{
            cross(col[1], col[2]) * invDet,
            cross(col[2], col[0]) * invDet,
            cross(col[0], col[1]) * invDet};
    }
};

// Shepperd's method, branch selected by the largest rotation diagonal so
// every branch keeps its sqrt argument away from zero for exact rotations.
// Precondition: approximately orthonormal (a rotation matrix). Mildly drifted
// inputs are tolerated: sqrt arguments are clamped at zero and the result is
// unit-length up to the input's drift. The returned sign is unspecified
// (q and -q encode the same rotation; compare rotations, never components).
[[nodiscard]] inline auto toQuat(const Mat3& m) -> Quat {
    const float trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (trace > 0.0f) {
        const float s = std::sqrt(max(0.0f, trace + 1.0f)) * 2.0f;
        return Quat{0.25f * s, (m(2, 1) - m(1, 2)) / s, (m(0, 2) - m(2, 0)) / s, (m(1, 0) - m(0, 1)) / s};
    }
    if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        const float s = std::sqrt(max(0.0f, 1.0f + m(0, 0) - m(1, 1) - m(2, 2))) * 2.0f;
        return Quat{(m(2, 1) - m(1, 2)) / s, 0.25f * s, (m(0, 1) + m(1, 0)) / s, (m(0, 2) + m(2, 0)) / s};
    }
    if (m(1, 1) > m(2, 2)) {
        const float s = std::sqrt(max(0.0f, 1.0f + m(1, 1) - m(0, 0) - m(2, 2))) * 2.0f;
        return Quat{(m(0, 2) - m(2, 0)) / s, (m(0, 1) + m(1, 0)) / s, 0.25f * s, (m(1, 2) + m(2, 1)) / s};
    }
    const float s = std::sqrt(max(0.0f, 1.0f + m(2, 2) - m(0, 0) - m(1, 1))) * 2.0f;
    return Quat{(m(1, 0) - m(0, 1)) / s, (m(0, 2) + m(2, 0)) / s, (m(1, 2) + m(2, 1)) / s, 0.25f * s};
}

} // namespace f4::math
