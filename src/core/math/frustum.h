#pragma once

#include <array>

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

inline constexpr int kFrustumCornerNearLeftBottom = 0;
inline constexpr int kFrustumCornerNearRightBottom = 1;
inline constexpr int kFrustumCornerNearRightTop = 2;
inline constexpr int kFrustumCornerNearLeftTop = 3;
inline constexpr int kFrustumCornerFarLeftBottom = 4;
inline constexpr int kFrustumCornerFarRightBottom = 5;
inline constexpr int kFrustumCornerFarRightTop = 6;
inline constexpr int kFrustumCornerFarLeftTop = 7;
inline constexpr int kFrustumCornerCount = 8;
inline constexpr float kFrustumBoundaryTolerance = 1.0e-5f;

struct Frustum {
    using Corners = std::array<Vec3, kFrustumCornerCount>;

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
        // Zero-to-one clip space is 0 <= z <= w: near is row 2, not
        // row 3 + row 2 (the latter is the OpenGL negative-one-to-one rule).
        f.planes[kFrustumPlaneNear] =
            Plane{Vec3(row(2, 0), row(2, 1), row(2, 2)), row(2, 3)}.normalized();
        f.planes[kFrustumPlaneFar] =
            Plane{Vec3(row(3, 0) - row(2, 0), row(3, 1) - row(2, 1), row(3, 2) - row(2, 2)), row(3, 3) - row(2, 3)}.normalized();
        return f;
    }

    [[nodiscard]] auto corners(Corners& out) const -> bool {
        return Plane::intersection(planes[kFrustumPlaneNear], planes[kFrustumPlaneLeft],
                                   planes[kFrustumPlaneBottom],
                                   out[kFrustumCornerNearLeftBottom]) &&
               Plane::intersection(planes[kFrustumPlaneNear], planes[kFrustumPlaneRight],
                                   planes[kFrustumPlaneBottom],
                                   out[kFrustumCornerNearRightBottom]) &&
               Plane::intersection(planes[kFrustumPlaneNear], planes[kFrustumPlaneRight],
                                   planes[kFrustumPlaneTop], out[kFrustumCornerNearRightTop]) &&
               Plane::intersection(planes[kFrustumPlaneNear], planes[kFrustumPlaneLeft],
                                   planes[kFrustumPlaneTop], out[kFrustumCornerNearLeftTop]) &&
               Plane::intersection(planes[kFrustumPlaneFar], planes[kFrustumPlaneLeft],
                                   planes[kFrustumPlaneBottom],
                                   out[kFrustumCornerFarLeftBottom]) &&
               Plane::intersection(planes[kFrustumPlaneFar], planes[kFrustumPlaneRight],
                                   planes[kFrustumPlaneBottom],
                                   out[kFrustumCornerFarRightBottom]) &&
               Plane::intersection(planes[kFrustumPlaneFar], planes[kFrustumPlaneRight],
                                   planes[kFrustumPlaneTop], out[kFrustumCornerFarRightTop]) &&
               Plane::intersection(planes[kFrustumPlaneFar], planes[kFrustumPlaneLeft],
                                   planes[kFrustumPlaneTop], out[kFrustumCornerFarLeftTop]);
    }

    // Corner order is near LB/RB/RT/LT followed by far LB/RB/RT/LT.
    [[nodiscard]] static auto fromCorners(const Corners& corners) -> Frustum {
        Vec3 center{};
        for (const Vec3& corner : corners) center += corner;
        center /= static_cast<float>(kFrustumCornerCount);

        const auto inwardPlane = [&corners, &center](int a, int b, int c) {
            Plane plane = Plane::fromPoints(corners[a], corners[b], corners[c]);
            if (plane.distanceTo(center) < 0.0f) {
                plane.normal = -plane.normal;
                plane.d = -plane.d;
            }
            return plane;
        };

        Frustum result;
        result.planes[kFrustumPlaneLeft] =
            inwardPlane(kFrustumCornerNearLeftBottom, kFrustumCornerFarLeftBottom,
                        kFrustumCornerFarLeftTop);
        result.planes[kFrustumPlaneRight] =
            inwardPlane(kFrustumCornerNearRightBottom, kFrustumCornerNearRightTop,
                        kFrustumCornerFarRightTop);
        result.planes[kFrustumPlaneBottom] =
            inwardPlane(kFrustumCornerNearLeftBottom, kFrustumCornerNearRightBottom,
                        kFrustumCornerFarRightBottom);
        result.planes[kFrustumPlaneTop] =
            inwardPlane(kFrustumCornerNearLeftTop, kFrustumCornerFarLeftTop,
                        kFrustumCornerFarRightTop);
        result.planes[kFrustumPlaneNear] =
            inwardPlane(kFrustumCornerNearLeftBottom, kFrustumCornerNearLeftTop,
                        kFrustumCornerNearRightTop);
        result.planes[kFrustumPlaneFar] =
            inwardPlane(kFrustumCornerFarLeftBottom, kFrustumCornerFarRightBottom,
                        kFrustumCornerFarRightTop);
        return result;
    }

    [[nodiscard]] bool contains(const Vec3& p) const {
        for (int i = 0; i < kFrustumPlaneCount; ++i) {
            if (planes[i].distanceTo(p) < -kFrustumBoundaryTolerance) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool overlaps(const Sphere& s) const {
        for (int i = 0; i < kFrustumPlaneCount; ++i) {
            if (planes[i].distanceTo(s.center) < -s.radius - kFrustumBoundaryTolerance) {
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
            if (planes[i].distanceTo(positive) < -kFrustumBoundaryTolerance) {
                return false;
            }
        }
        return true;
    }
};

} // namespace f4::math
