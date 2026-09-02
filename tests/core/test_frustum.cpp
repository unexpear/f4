#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/frustum.h"
#include "core/math/mat4.h"

using namespace f4::math;
using namespace f4test;

namespace {

auto standardCamera() -> Mat4 {
    const Mat4 view = Mat4::lookAtRH(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = Mat4::perspectiveRH_ZO(radians(90.0f), 1.0f, 0.1f, 100.0f);
    return proj.mul(view);
}

} // namespace

TEST_CASE("ndc cube frustum from identity") {
    const Frustum f = Frustum::fromViewProjection(Mat4::identity());
    CHECK(f.contains(Vec3(0.0f)));
    CHECK(f.contains(Vec3(0.9f, -0.9f, 0.9f)));
    CHECK(!f.contains(Vec3(1.1f, 0.0f, 0.0f)));
    CHECK(!f.contains(Vec3(0.0f, 0.0f, 2.0f)));
    CHECK(f.contains(Vec3(0.0f, 0.0f, 0.0f)));
    CHECK(!f.contains(Vec3(0.0f, 0.0f, -0.001f)));

    CHECK(f.intersects(AABB{Vec3(-0.5f), Vec3(0.5f)}));
    CHECK(!f.intersects(AABB{Vec3(2.0f, 2.0f, 2.0f), Vec3(3.0f, 3.0f, 3.0f)}));
}

TEST_CASE("frustum boundary probes exercise every plane") {
    const Frustum f = Frustum::fromViewProjection(standardCamera());
    const Vec3 interior(0.0f, 0.0f, 0.0f);
    REQUIRE(f.contains(interior));

    for (int i = 0; i < kFrustumPlaneCount; ++i) {
        const Plane& plane = f.planes[i];
        const Vec3 boundary = interior - plane.normal * plane.distanceTo(interior);
        CAPTURE(i);
        CHECK(f.contains(boundary));
        CHECK(f.contains(boundary + plane.normal * 0.001f));
        CHECK_FALSE(f.contains(boundary - plane.normal * 0.001f));

        constexpr float radius = 0.01f;
        CHECK(f.overlaps(Sphere{boundary - plane.normal * radius, radius}));
        CHECK_FALSE(f.overlaps(Sphere{boundary - plane.normal * (radius + 0.001f), radius}));
    }
}

TEST_CASE("frustum planes and corners roundtrip") {
    const Mat4 view = Mat4::lookAtRH(Vec3(3.0f, 4.0f, 8.0f), Vec3(-1.0f, 0.5f, -2.0f),
                                    Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 projection = Mat4::perspectiveRH_ZO(radians(67.0f), 16.0f / 9.0f, 0.2f, 80.0f);
    const Frustum original = Frustum::fromViewProjection(projection.mul(view));
    Frustum::Corners corners{};
    REQUIRE(original.corners(corners));

    const Frustum rebuilt = Frustum::fromCorners(corners);
    constexpr Tolerance reconstructionTolerance{2.0e-4f, 2.0e-4f};
    for (int i = 0; i < kFrustumPlaneCount; ++i) {
        CAPTURE(i);
        CHECK(veq(rebuilt.planes[i].normal, original.planes[i].normal,
                  reconstructionTolerance));
        CHECK(feq(rebuilt.planes[i].d, original.planes[i].d, reconstructionTolerance));
    }
    for (const Vec3& corner : corners) {
        INFO("corner=" << corner << " distances="
                       << original.planes[0].distanceTo(corner) << ','
                       << original.planes[1].distanceTo(corner) << ','
                       << original.planes[2].distanceTo(corner) << ','
                       << original.planes[3].distanceTo(corner) << ','
                       << original.planes[4].distanceTo(corner) << ','
                       << original.planes[5].distanceTo(corner) << " rebuilt="
                       << rebuilt.planes[0].distanceTo(corner) << ','
                       << rebuilt.planes[1].distanceTo(corner) << ','
                       << rebuilt.planes[2].distanceTo(corner) << ','
                       << rebuilt.planes[3].distanceTo(corner) << ','
                       << rebuilt.planes[4].distanceTo(corner) << ','
                       << rebuilt.planes[5].distanceTo(corner));
        CHECK(original.contains(corner));
        CHECK(rebuilt.contains(corner));
    }

    Frustum degenerate{};
    for (Plane& plane : degenerate.planes) {
        plane.normal = Vec3(1.0f, 0.0f, 0.0f);
        plane.d = 0.0f;
    }
    CHECK_FALSE(degenerate.corners(corners));
}

TEST_CASE("camera frustum contains scene points in front of the camera") {
    const Frustum f = Frustum::fromViewProjection(standardCamera());

    CHECK(f.contains(Vec3(0.0f, 0.0f, 0.0f)));
    CHECK(f.contains(Vec3(1.0f, 0.0f, -5.0f)));
    CHECK(f.contains(Vec3(-4.0f, 4.0f, -4.0f)));
}

TEST_CASE("camera frustum rejects behind and outside points") {
    const Frustum f = Frustum::fromViewProjection(standardCamera());

    CHECK(!f.contains(Vec3(0.0f, 0.0f, 10.0f)));
    CHECK(!f.contains(Vec3(50.0f, 0.0f, -5.0f)));
    CHECK(!f.contains(Vec3(0.0f, 200.0f, -5.0f)));
    CHECK(!f.contains(Vec3(0.0f, 0.0f, -150.0f)));
}

TEST_CASE("sphere overlap against camera frustum") {
    const Frustum f = Frustum::fromViewProjection(standardCamera());

    CHECK(f.overlaps(Sphere{Vec3(0.0f), 0.5f}));
    CHECK(f.overlaps(Sphere{Vec3(6.0f, 6.0f, 0.0f), 20.0f}));
    CHECK(!f.overlaps(Sphere{Vec3(100.0f, 100.0f, 100.0f), 1.0f}));
    CHECK(!f.overlaps(Sphere{Vec3(0.0f, 0.0f, 8.0f), 1.0f}));
}

TEST_CASE("aabb intersection is conservative") {
    const Frustum f = Frustum::fromViewProjection(standardCamera());

    CHECK(f.intersects(AABB{Vec3(-1.0f), Vec3(1.0f)}));
    CHECK(f.intersects(AABB{Vec3(-1000.0f), Vec3(1000.0f)}));
    CHECK(!f.intersects(AABB{Vec3(60.0f, 60.0f, 60.0f), Vec3(61.0f, 61.0f, 61.0f)}));
}
