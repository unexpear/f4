#pragma once

#include "core/math/numerics.h"
#include "core/math/vec3.h"

namespace f4::math {

struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float d{0.0f};

    [[nodiscard]] auto distanceTo(const Vec3& p) const -> float {
        return dot(normal, p) + d;
    }

    [[nodiscard]] static auto fromNormalPoint(const Vec3& unitNormal, const Vec3& point) -> Plane {
        Plane p;
        p.normal = f4::math::normalized(unitNormal);
        p.d = -dot(p.normal, point);
        return p;
    }

    [[nodiscard]] static auto fromPoints(const Vec3& a, const Vec3& b, const Vec3& c) -> Plane {
        return fromNormalPoint(cross(b - a, c - a), a);
    }

    [[nodiscard]] auto normalized() const -> Plane {
        const float len = length(normal);
        if (len <= kEpsilon) {
            return *this;
        }
        const float inv = 1.0f / len;
        return {normal * inv, d * inv};
    }
};

} // namespace f4::math
