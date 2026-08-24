#pragma once

#include "core/math/aabb.h"
#include "core/math/mat4.h"
#include "core/math/plane.h"
#include "core/math/sphere.h"

namespace f4::math {

inline constexpr int kFrustumPlaneLeft = 0;
inline constexpr int kFrustumPlaneRight = 1;
inline constexpr int kFrustumPlaneBottom = 2;
inline constexpr int kFrustumPlaneTop = 3;
inline constexpr int kFrustumPlaneNear = 4;
inline constexpr int kFrustumPlaneFar = 5;
inline constexpr int kFrustumPlaneCount = 6;

struct Frustum {
    Plane planes[kFrustumPlaneCount]{};

    [[nodiscard]] static auto fromViewProjection(const Mat4& vp) -> Frustum {
        Frustum f;
        const auto row = [&vp](int r, int c) { return vp(r, c); };

        f.planes[kFrustumPlaneLeft] =
            Plane{Vec3(row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2)), row(3, 3) + row(0, 3)}.normalized();
        f.planes[kFrustumPlaneRight] =
            Plane{Vec3(row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2)), row(3, 3) - row(0, 3)}.normalized();
        f.planes[kFrustumPlaneBottom] =
            Plane{Vec3(row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2)), row(3, 3) + row(1, 3)}.normalized();
        f.planes[kFrustumPlaneTop] =
            Plane{Vec3(row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2)), row(3, 3) - row(1, 3)}.normalized();
        f.planes[kFrustumPlaneNear] =
            Plane{Vec3(row(3, 0) + row(2, 0), row(3, 1) + row(2, 1), row(3, 2) + row(2, 2)), row(3, 3) + row(2, 3)}.normalized();
        f.planes[kFrustumPlaneFar] =
            Plane{Vec3(row(3, 0) - row(2, 0), row(3, 1) - row(2, 1), row(3, 2) - row(2, 2)), row(3, 3) - row(2, 3)}.normalized();
        return f;
    }

    [[nodiscard]] bool contains(const Vec3& p) const {
        for (int i = 0; i < kFrustumPlaneCount; ++i) {
            if (planes[i].distanceTo(p) < -kEpsilon) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool overlaps(const Sphere& s) const {
        for (int i = 0; i < kFrustumPlaneCount; ++i) {
            if (planes[i].distanceTo(s.center) < -s.radius) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool intersects(const AABB& box) const {
        for (int i = 0; i < kFrustumPlaneCount; ++i) {
            const Vec3& n = planes[i].normal;
            const Vec3 positive{
                n.x >= 0.0f ? box.max.x : box.min.x,
                n.y >= 0.0f ? box.max.y : box.min.y,
                n.z >= 0.0f ? box.max.z : box.min.z};
            if (planes[i].distanceTo(positive) < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace f4::math
