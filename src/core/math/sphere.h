#pragma once

#include "core/math/aabb.h"
#include "core/math/numerics.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Sphere {
    Vec3 center{0.0f, 0.0f, 0.0f};
    float radius{0.0f};

    [[nodiscard]] constexpr auto contains(const Vec3& p) const -> bool {
        return lengthSq(p - center) <= radius * radius;
    }

    [[nodiscard]] auto contains(const Sphere& o) const -> bool {
        return distance(o.center, center) + o.radius <= radius;
    }

    [[nodiscard]] constexpr auto intersects(const Sphere& o) const -> bool {
        const float r = radius + o.radius;
        return lengthSq(o.center - center) <= r * r;
    }

    [[nodiscard]] static auto circumscribingAABB(const AABB& box) -> Sphere {
        Sphere s;
        s.center = box.center();
        s.radius = length(box.extents()) * 0.5f;
        return s;
    }

    [[nodiscard]] auto transformed(const Mat4& m) const -> Sphere {
        Sphere out;
        out.center = m.transformPoint(center);
        const float sx = length(m.col[0].xyz());
        const float sy = length(m.col[1].xyz());
        const float sz = length(m.col[2].xyz());
        out.radius = radius * max(sx, max(sy, sz));
        return out;
    }
};

} // namespace f4::math
