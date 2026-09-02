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

    // Returns the common point of three planes. False means the planes do not
    // define a stable unique intersection (parallel or nearly parallel).
    [[nodiscard]] static auto intersection(const Plane& a, const Plane& b, const Plane& c,
                                           Vec3& point) -> bool {
        const double bcx = static_cast<double>(b.normal.y) * c.normal.z -
                           static_cast<double>(b.normal.z) * c.normal.y;
        const double bcy = static_cast<double>(b.normal.z) * c.normal.x -
                           static_cast<double>(b.normal.x) * c.normal.z;
        const double bcz = static_cast<double>(b.normal.x) * c.normal.y -
                           static_cast<double>(b.normal.y) * c.normal.x;
        const double cax = static_cast<double>(c.normal.y) * a.normal.z -
                           static_cast<double>(c.normal.z) * a.normal.y;
        const double cay = static_cast<double>(c.normal.z) * a.normal.x -
                           static_cast<double>(c.normal.x) * a.normal.z;
        const double caz = static_cast<double>(c.normal.x) * a.normal.y -
                           static_cast<double>(c.normal.y) * a.normal.x;
        const double abx = static_cast<double>(a.normal.y) * b.normal.z -
                           static_cast<double>(a.normal.z) * b.normal.y;
        const double aby = static_cast<double>(a.normal.z) * b.normal.x -
                           static_cast<double>(a.normal.x) * b.normal.z;
        const double abz = static_cast<double>(a.normal.x) * b.normal.y -
                           static_cast<double>(a.normal.y) * b.normal.x;
        const double denominator = static_cast<double>(a.normal.x) * bcx +
                                   static_cast<double>(a.normal.y) * bcy +
                                   static_cast<double>(a.normal.z) * bcz;
        if (std::fabs(denominator) <= static_cast<double>(kEpsilon)) {
            return false;
        }
        point = Vec3(
            static_cast<float>((bcx * -a.d + cax * -b.d + abx * -c.d) / denominator),
            static_cast<float>((bcy * -a.d + cay * -b.d + aby * -c.d) / denominator),
            static_cast<float>((bcz * -a.d + caz * -b.d + abz * -c.d) / denominator));
        return true;
    }
};

} // namespace f4::math
