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

    CHECK(f.intersects(AABB{Vec3(-0.5f), Vec3(0.5f)}));
    CHECK(!f.intersects(AABB{Vec3(2.0f, 2.0f, 2.0f), Vec3(3.0f, 3.0f, 3.0f)}));
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
