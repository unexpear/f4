#pragma once

#include "core/math/numerics.h"
#include "core/math/vec3.h"
#include "core/math/mat4.h"

namespace f4::math {

struct AABB {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    [[nodiscard]] constexpr auto isValid() const -> bool {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    [[nodiscard]] constexpr auto extents() const -> Vec3 { return max - min; }
    [[nodiscard]] constexpr auto center() const -> Vec3 { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr auto halfExtents() const -> Vec3 { return extents() * 0.5f; }

    [[nodiscard]] constexpr auto volume() const -> float {
        const Vec3 e = extents();
        return e.x * e.y * e.z;
    }

    constexpr void expand(const Vec3& p) {
        min = minComponentwise(min, p);
        max = maxComponentwise(max, p);
    }

    constexpr void expand(const AABB& other) {
        expand(other.min);
        expand(other.max);
    }

    [[nodiscard]] static constexpr auto merged(const AABB& a, const AABB& b) -> AABB {
        return {minComponentwise(a.min, b.min), maxComponentwise(a.max, b.max)};
    }

    [[nodiscard]] constexpr auto contains(const Vec3& p) const -> bool {
        return p.x >= min.x && p.x <= max.x
            && p.y >= min.y && p.y <= max.y
            && p.z >= min.z && p.z <= max.z;
    }

    [[nodiscard]] constexpr auto intersects(const AABB& o) const -> bool {
        return min.x <= o.max.x && max.x >= o.min.x
            && min.y <= o.max.y && max.y >= o.min.y
            && min.z <= o.max.z && max.z >= o.min.z;
    }

    [[nodiscard]] constexpr auto closestPoint(const Vec3& p) const -> Vec3 {
        return {clamp(p.x, min.x, max.x), clamp(p.y, min.y, max.y), clamp(p.z, min.z, max.z)};
    }

    [[nodiscard]] constexpr auto translated(const Vec3& offset) const -> AABB {
        return {min + offset, max + offset};
    }

    [[nodiscard]] auto transformed(const Mat4& m) const -> AABB {
        const Vec3 first = m.transformPoint(corner(0));
        AABB out{first, first};
        for (int i = 1; i < 8; ++i) {
            out.expand(m.transformPoint(corner(i)));
        }
        return out;
    }

    [[nodiscard]] constexpr auto corner(int index) const -> Vec3 {
        return {
            (index & 1) != 0 ? max.x : min.x,
            (index & 2) != 0 ? max.y : min.y,
            (index & 4) != 0 ? max.z : min.z};
    }
};

} // namespace f4::math
