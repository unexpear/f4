#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/aabb.h"
#include "core/math/mat4.h"
#include "core/math/plane.h"
#include "core/math/sphere.h"

using namespace f4::math;
using namespace f4test;

TEST_CASE("aabb basic properties") {
    const AABB box{Vec3(-1.0f, -2.0f, 0.0f), Vec3(1.0f, 2.0f, 4.0f)};
    CHECK(box.isValid());
    CHECK(veq(box.extents(), Vec3(2.0f, 4.0f, 4.0f)));
    CHECK(veq(box.center(), Vec3(0.0f, 0.0f, 2.0f)));
    CHECK(feq(box.volume(), 32.0f));
    CHECK(!AABB{Vec3(1.0f), Vec3(0.0f)}.isValid());
}

TEST_CASE("aabb expand and merge") {
    AABB box;
    box.expand(Vec3(-1.0f, 2.0f, 3.0f));
    box.expand(Vec3(4.0f, -2.0f, -5.0f));
    CHECK(veq(box.min, Vec3(-1.0f, -2.0f, -5.0f)));
    CHECK(veq(box.max, Vec3(4.0f, 2.0f, 3.0f)));

    const AABB merged = AABB::merged(box, AABB{Vec3(100.0f), Vec3(101.0f)});
    CHECK(veq(merged.min, Vec3(-1.0f, -2.0f, -5.0f)));
    CHECK(veq(merged.max, Vec3(101.0f)));
}

TEST_CASE("aabb contains and closest point") {
    const AABB box{Vec3(0.0f), Vec3(2.0f, 2.0f, 2.0f)};
    CHECK(box.contains(Vec3(1.0f, 1.0f, 1.0f)));
    CHECK(box.contains(Vec3(0.0f)));
    CHECK(!box.contains(Vec3(2.1f, 1.0f, 1.0f)));
    CHECK(veq(box.closestPoint(Vec3(5.0f, -1.0f, 0.5f)), Vec3(2.0f, 0.0f, 0.5f)));
}

TEST_CASE("aabb intersects") {
    const AABB a{Vec3(0.0f), Vec3(2.0f)};
    const AABB b{Vec3(1.9f), Vec3(3.0f)};
    const AABB c{Vec3(2.1f), Vec3(3.0f)};
    CHECK(a.intersects(b));
    CHECK(b.intersects(a));
    CHECK(!a.intersects(c));
}

TEST_CASE("aabb translated and transformed") {
    const AABB box{Vec3(0.0f), Vec3(2.0f, 2.0f, 2.0f)};

    const AABB moved = box.translated(Vec3(1.0f, -1.0f, 0.0f));
    CHECK(veq(moved.min, Vec3(1.0f, -1.0f, 0.0f)));

    const Mat4 rotY90 = Mat4::rotationY(radians(90.0f));
    const AABB rotated = box.transformed(rotY90);
    CHECK(veq(rotated.min, Vec3(0.0f, 0.0f, -2.0f)));
    CHECK(veq(rotated.max, Vec3(2.0f, 2.0f, 0.0f)));
}

TEST_CASE("sphere containment and intersection") {
    const Sphere s{Vec3(0.0f), 2.0f};
    CHECK(s.contains(Vec3(1.0f, 1.0f, 1.0f)));
    CHECK(!s.contains(Vec3(2.0f, 0.0f, 0.1f)));
    CHECK(s.contains(Sphere{Vec3(1.5f, 0.0f, 0.0f), 0.25f}));
    CHECK(!s.contains(Sphere{Vec3(1.0f), 1.0f}));
    CHECK(s.intersects(Sphere{Vec3(3.5f, 0.0f, 0.0f), 1.6f}));
    CHECK(!s.intersects(Sphere{Vec3(3.5f, 0.0f, 0.0f), 1.4f}));
}

TEST_CASE("sphere from aabb") {
    const AABB box{Vec3(0.0f), Vec3(2.0f, 0.0f, 0.0f)};
    const Sphere s = Sphere::circumscribingAABB(box);
    CHECK(veq(s.center, Vec3(1.0f, 0.0f, 0.0f)));
    CHECK(feq(s.radius, 1.0f));
    CHECK(s.contains(Vec3(2.0f, 0.0f, 0.0f)));
}

TEST_CASE("sphere transformed scales radius by largest axis") {
    const Sphere s{Vec3(0.0f), 1.0f};
    const Sphere out = s.transformed(Mat4::scaling(Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(feq(out.radius, 3.0f));
}

TEST_CASE("plane from normal and point") {
    const Plane ground = Plane::fromNormalPoint(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 0.0f));
    CHECK(feq(ground.distanceTo(Vec3(0.0f, 5.0f, 0.0f)), 5.0f));
    CHECK(feq(ground.distanceTo(Vec3(1.0f, -3.0f, 1.0f)), -3.0f));
    CHECK(feq(ground.distanceTo(Vec3(7.0f, 0.0f, -2.0f)), 0.0f));
}

TEST_CASE("plane from three points") {
    const Plane p = Plane::fromPoints(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));
    CHECK(veq(p.normal, Vec3(0.0f, 1.0f, 0.0f)));
    CHECK(feq(p.d, 0.0f));

    const Plane shifted = Plane::fromPoints(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, 2.0f, -1.0f));
    CHECK(veq(shifted.normal, Vec3(0.0f, 1.0f, 0.0f), kDefaultTolerance));
    CHECK(feq(shifted.d, -2.0f));
}

TEST_CASE("plane normalization preserves the plane and rescales distances") {
    Plane scaled;
    scaled.normal = Vec3(0.0f, 2.0f, 0.0f);
    scaled.d = -8.0f;
    const Plane n = scaled.normalized();
    CHECK(feq(length(n.normal), 1.0f));

    const Vec3 onPlane(3.0f, 4.0f, -2.0f);
    CHECK(approxZero(n.distanceTo(onPlane), 1e-5f));
    CHECK(approxZero(scaled.distanceTo(onPlane), 1e-5f));

    const Vec3 above(3.0f, 6.0f, -2.0f);
    CHECK(feq(n.distanceTo(above), scaled.distanceTo(above) * 0.5f));
}

TEST_CASE("degenerate point aabb") {
    const AABB point{Vec3(1.0f, 2.0f, 3.0f), Vec3(1.0f, 2.0f, 3.0f)};
    CHECK(point.isValid());
    CHECK(feq(point.volume(), 0.0f));
    CHECK(point.contains(Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(!point.contains(Vec3(1.0f, 2.0f, 3.5f)));
    CHECK(veq(point.closestPoint(Vec3(9.0f, -1.0f, 3.0f)), Vec3(1.0f, 2.0f, 3.0f)));

    const AABB grown = AABB::merged(point, AABB{Vec3(-1.0f), Vec3(0.5f)});
    CHECK(veq(grown.min, Vec3(-1.0f, -1.0f, -1.0f)));
    CHECK(veq(grown.max, Vec3(1.0f, 2.0f, 3.0f)));
}

TEST_CASE("zero radius sphere contains only its center") {
    const Sphere dot{Vec3(2.0f, -3.0f, 4.0f), 0.0f};
    CHECK(dot.contains(dot.center));
    CHECK(!dot.contains(dot.center + Vec3(1e-4f, 0.0f, 0.0f)));
    CHECK(dot.intersects(Sphere{dot.center, 0.0f}));
    CHECK(!dot.intersects(Sphere{dot.center + Vec3(0.5f, 0.0f, 0.0f), 0.25f}));
}

TEST_CASE("mirrored transform swaps aabb min and max") {
    const AABB box{Vec3(0.0f, 1.0f, -1.0f), Vec3(2.0f, 3.0f, 1.0f)};
    const AABB mirrored = box.transformed(Mat4::scaling(Vec3(-1.0f, 1.0f, 1.0f)));

    CHECK(veq(mirrored.min, Vec3(-2.0f, 1.0f, -1.0f)));
    CHECK(veq(mirrored.max, Vec3(0.0f, 3.0f, 1.0f)));
}

TEST_CASE("sphere radius stays positive under mirroring") {
    const Sphere s{Vec3(1.0f, 0.0f, 0.0f), 1.5f};
    const Sphere out = s.transformed(Mat4::scaling(Vec3(-2.0f, 1.0f, 1.0f)));

    CHECK(feq(out.radius, 3.0f));
    CHECK(veq(out.center, Vec3(-2.0f, 0.0f, 0.0f)));
}

TEST_CASE("collinear points produce the degenerate zero plane contract") {
    const Plane p = Plane::fromPoints(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f, 0.0f, 0.0f));

    CHECK(p.normal == Vec3(0.0f));
    CHECK(feq(p.d, 0.0f));
    F4_CHECK_NEAR(p.distanceTo(Vec3(7.0f, -8.0f, 9.0f)), 0.0f, kTightTolerance);

    Plane z;
    z.normal = Vec3(0.0f);
    z.d = 0.0f;
    const Plane passthrough = z.normalized();
    CHECK(passthrough.normal == Vec3(0.0f));
    CHECK(feq(passthrough.d, 0.0f));
}
