#pragma once

#include "core/math/mat3.h"
#include "core/math/numerics.h"
#include "core/math/quat.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"

namespace f4::math {

// M8 decision: the shared 0..1 projection convention remains forward-Z.
// Near maps to 0, far maps to 1; reversed-Z is deferred until every backend
// can switch clear values and depth comparisons as one atomic policy change.
inline constexpr bool kUsesReversedZ = false;

struct Mat4 {
    Vec4 col[4];

    constexpr Mat4()
        : col{Vec4(1.0f, 0.0f, 0.0f, 0.0f), Vec4(0.0f, 1.0f, 0.0f, 0.0f),
              Vec4(0.0f, 0.0f, 1.0f, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)} {}

    constexpr explicit Mat4(const Vec4& c0, const Vec4& c1, const Vec4& c2, const Vec4& c3)
        : col{c0, c1, c2, c3} {}

    constexpr auto operator()(int r, int c) -> float& { return col[c][r]; }
    constexpr auto operator()(int r, int c) const -> float { return col[c][r]; }

    [[nodiscard]] static constexpr auto identity() -> Mat4 { return {}; }
    [[nodiscard]] static constexpr auto zero() -> Mat4 {
        return Mat4{Vec4(0.0f), Vec4(0.0f), Vec4(0.0f), Vec4(0.0f)};
    }

    [[nodiscard]] constexpr auto mul(const Mat4& m) const -> Mat4 {
        return Mat4{
            col[0] * m.col[0].x + col[1] * m.col[0].y + col[2] * m.col[0].z + col[3] * m.col[0].w,
            col[0] * m.col[1].x + col[1] * m.col[1].y + col[2] * m.col[1].z + col[3] * m.col[1].w,
            col[0] * m.col[2].x + col[1] * m.col[2].y + col[2] * m.col[2].z + col[3] * m.col[2].w,
            col[0] * m.col[3].x + col[1] * m.col[3].y + col[2] * m.col[3].z + col[3] * m.col[3].w};
    }

    [[nodiscard]] constexpr auto mul(const Vec4& v) const -> Vec4 {
        return col[0] * v.x + col[1] * v.y + col[2] * v.z + col[3] * v.w;
    }

    [[nodiscard]] auto transformPoint(const Vec3& p) const -> Vec3 {
        const Vec4 r = mul(Vec4(p, 1.0f));
        const float invW = 1.0f / r.w;
        return r.xyz() * invW;
    }

    [[nodiscard]] constexpr auto transformVector(const Vec3& v) const -> Vec3 {
        return mul(Vec4(v, 0.0f)).xyz();
    }

    [[nodiscard]] constexpr auto transpose() const -> Mat4 {
        return Mat4{
            Vec4(col[0].x, col[1].x, col[2].x, col[3].x),
            Vec4(col[0].y, col[1].y, col[2].y, col[3].y),
            Vec4(col[0].z, col[1].z, col[2].z, col[3].z),
            Vec4(col[0].w, col[1].w, col[2].w, col[3].w)};
    }

    [[nodiscard]] auto determinant() const -> float;

    [[nodiscard]] auto inverse() const -> Mat4;

    [[nodiscard]] static constexpr auto translation(const Vec3& t) -> Mat4 {
        return Mat4{Vec4(1.0f, 0.0f, 0.0f, 0.0f), Vec4(0.0f, 1.0f, 0.0f, 0.0f),
                    Vec4(0.0f, 0.0f, 1.0f, 0.0f), Vec4(t.x, t.y, t.z, 1.0f)};
    }

    [[nodiscard]] static constexpr auto scaling(const Vec3& s) -> Mat4 {
        return Mat4{Vec4(s.x, 0.0f, 0.0f, 0.0f), Vec4(0.0f, s.y, 0.0f, 0.0f),
                    Vec4(0.0f, 0.0f, s.z, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    [[nodiscard]] static auto rotationX(float radiansAngle) -> Mat4 {
        const float c = std::cos(radiansAngle);
        const float s = std::sin(radiansAngle);
        return Mat4{Vec4(1.0f, 0.0f, 0.0f, 0.0f), Vec4(0.0f, c, s, 0.0f),
                    Vec4(0.0f, -s, c, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    [[nodiscard]] static auto rotationY(float radiansAngle) -> Mat4 {
        const float c = std::cos(radiansAngle);
        const float s = std::sin(radiansAngle);
        return Mat4{Vec4(c, 0.0f, -s, 0.0f), Vec4(0.0f, 1.0f, 0.0f, 0.0f),
                    Vec4(s, 0.0f, c, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    [[nodiscard]] static auto rotationZ(float radiansAngle) -> Mat4 {
        const float c = std::cos(radiansAngle);
        const float s = std::sin(radiansAngle);
        return Mat4{Vec4(c, s, 0.0f, 0.0f), Vec4(-s, c, 0.0f, 0.0f),
                    Vec4(0.0f, 0.0f, 1.0f, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    [[nodiscard]] static auto fromQuat(const Quat& q) -> Mat4 {
        const Mat3 r = Mat3::fromQuat(q);
        return Mat4{
            Vec4(r.col[0], 0.0f), Vec4(r.col[1], 0.0f), Vec4(r.col[2], 0.0f),
            Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
    }

    [[nodiscard]] static auto lookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up) -> Mat4 {
        const Vec3 forward = normalized(target - eye);
        const Vec3 side = normalized(cross(forward, up));
        const Vec3 up2 = cross(side, forward);
        return Mat4{
            Vec4(side.x, up2.x, -forward.x, 0.0f),
            Vec4(side.y, up2.y, -forward.y, 0.0f),
            Vec4(side.z, up2.z, -forward.z, 0.0f),
            Vec4(-dot(side, eye), -dot(up2, eye), dot(forward, eye), 1.0f)};
    }

    [[nodiscard]] static auto perspectiveRH_ZO(float fovYRadians, float aspect, float zNear, float zFar) -> Mat4 {
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        return Mat4{
            Vec4(f / aspect, 0.0f, 0.0f, 0.0f),
            Vec4(0.0f, f, 0.0f, 0.0f),
            Vec4(0.0f, 0.0f, zFar / (zNear - zFar), -1.0f),
            Vec4(0.0f, 0.0f, (zNear * zFar) / (zNear - zFar), 0.0f)};
    }

    [[nodiscard]] static auto orthoRH_ZO(float left, float right, float bottom, float top, float zNear, float zFar) -> Mat4 {
        const float sx = 2.0f / (right - left);
        const float sy = 2.0f / (top - bottom);
        const float sz = 1.0f / (zNear - zFar);
        return Mat4{
            Vec4(sx, 0.0f, 0.0f, 0.0f),
            Vec4(0.0f, sy, 0.0f, 0.0f),
            Vec4(0.0f, 0.0f, sz, 0.0f),
            Vec4(-(right + left) / (right - left), -(top + bottom) / (top - bottom), zNear / (zNear - zFar), 1.0f)};
    }
};

inline auto Mat4::determinant() const -> float {
    const auto a = [&](int r, int c) { return (*this)(r, c); };
    const float m2233 = a(2, 2) * a(3, 3) - a(2, 3) * a(3, 2);
    const float m2133 = a(2, 1) * a(3, 3) - a(2, 3) * a(3, 1);
    const float m2032 = a(2, 0) * a(3, 2) - a(2, 2) * a(3, 0);
    const float m2033 = a(2, 0) * a(3, 3) - a(2, 3) * a(3, 0);
    const float m2132 = a(2, 1) * a(3, 2) - a(2, 2) * a(3, 1);
    const float m2031 = a(2, 0) * a(3, 1) - a(2, 1) * a(3, 0);
    return a(0, 0) * (a(1, 1) * m2233 - a(1, 2) * m2133 + a(1, 3) * m2132)
         - a(0, 1) * (a(1, 0) * m2233 - a(1, 2) * m2033 + a(1, 3) * m2032)
         + a(0, 2) * (a(1, 0) * m2133 - a(1, 1) * m2033 + a(1, 3) * m2031)
         - a(0, 3) * (a(1, 0) * m2132 - a(1, 1) * m2032 + a(1, 2) * m2031);
}

inline auto Mat4::inverse() const -> Mat4 {
    float m[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r * 4 + c] = (*this)(r, c);
        }
    }

    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15]
           + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
           - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
           + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
            - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
           - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
           + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
           - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
            + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
           + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
           - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
            + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
            - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
           - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
           + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
            - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
            + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    det = 1.0f / det;
    Mat4 result;
    for (int i = 0; i < 16; ++i) {
        result(i / 4, i % 4) = inv[i] * det;
    }
    return result;
}

// Extracts the rotation of an orthonormal upper-left 3x3 block (e.g. the
// output of composeTRS with unit scale). Translation and the fourth row/column
// are ignored; see toQuat(const Mat3&) for drift and sign semantics.
[[nodiscard]] inline auto toQuat(const Mat4& m) -> Quat {
    const Mat3 rotation{m.col[0].xyz(), m.col[1].xyz(), m.col[2].xyz()};
    return toQuat(rotation);
}

} // namespace f4::math
