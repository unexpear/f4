#pragma once

#include <cmath>

#include "core/assert.h"
#include "core/math/mat4.h"
#include "core/math/numerics.h"
#include "core/math/quat.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] static constexpr auto identity() -> Transform { return {}; }
};

[[nodiscard]] inline auto composeTRS(const Transform& t) -> Mat4 {
    return Mat4::translation(t.position).mul(Mat4::fromQuat(t.rotation)).mul(Mat4::scaling(t.scale));
}

[[nodiscard]] constexpr auto composeHierarchy(const Mat4& parentWorld, const Mat4& childLocal) -> Mat4 {
    return parentWorld.mul(childLocal);
}

[[nodiscard]] inline auto decompose(const Mat4& m) -> Transform {
    Transform out;
    out.position = m.col[3].xyz();

    Vec3 c0 = m.col[0].xyz();
    Vec3 c1 = m.col[1].xyz();
    Vec3 c2 = m.col[2].xyz();

    const float sx = length(c0);
    const float sy = length(c1);
    float sz = length(c2);

    if (dot(cross(c0, c1), c2) < 0.0f) {
        sz = -sz;
    }

    F4_ASSERT(sx > kEpsilon && sy > kEpsilon && std::fabs(sz) > kEpsilon,
              "cannot decompose matrix with zero scale");

    out.scale = Vec3(sx, sy, sz);

    Mat3 rotation;
    rotation.col[0] = c0 / sx;
    rotation.col[1] = c1 / sy;
    rotation.col[2] = c2 / sz;
    out.rotation = toQuat(rotation);
    return out;
}

} // namespace f4::math
