#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/transform.h"

using namespace f4::math;
using namespace f4test;

TEST_CASE("identity transform composes to identity matrix") {
    const Mat4 m = composeTRS(Transform::identity());
    CHECK(meq(m, Mat4::identity()));
}

TEST_CASE("decompose inverts composeTRS") {
    const Transform t{
        Vec3(1.5f, -2.0f, 3.0f),
        Quat::fromEuler(radians(30.0f), radians(-20.0f), radians(10.0f)),
        Vec3(1.0f, 2.0f, 0.5f)};
    const Mat4 m = composeTRS(t);
    const Transform back = decompose(m);

    CHECK(veq(back.position, t.position));
    Vec3 s = back.scale;
    Quat r = back.rotation;
    if (s.z < 0.0f) {
        s = -s;
        r = -r;
    }
    CHECK(feq(s.x, t.scale.x));
    CHECK(feq(s.y, t.scale.y));
    CHECK(feq(s.z, t.scale.z));

    const Vec3 v(0.25f, -1.5f, 2.75f);
    CHECK(veq(r.rotate(v), t.rotation.rotate(v), kLooseTolerance));
}

TEST_CASE("mirror scale decomposes to negative z-scale with equivalent rotation") {
    const Mat4 m = Mat4::scaling(Vec3(-1.0f, 1.0f, 1.0f));
    const Transform t = decompose(m);

    CHECK(feq(t.scale.x, 1.0f));
    CHECK(feq(t.scale.y, 1.0f));
    CHECK(feq(t.scale.z, -1.0f));
    CHECK(veq(t.rotation.rotate(Vec3(1.0f, 0.0f, 0.0f)), Vec3(-1.0f, 0.0f, 0.0f)));
    CHECK(veq(composeTRS(t).transformPoint(Vec3(1.0f, 2.0f, 3.0f)), m.transformPoint(Vec3(1.0f, 2.0f, 3.0f))));
}

TEST_CASE("hierarchy composition matches manual nesting") {
    const Transform parent{Vec3(10.0f, 0.0f, 0.0f), Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), radians(90.0f)), Vec3(2.0f)};
    const Transform child{Vec3(1.0f, 0.0f, 0.0f), Quat::identity(), Vec3(1.0f)};

    const Mat4 world = composeHierarchy(composeTRS(parent), composeTRS(child));
    const Vec3 p = world.transformPoint(Vec3(0.0f));
    CHECK(veq(p, Vec3(10.0f, 0.0f, -2.0f)));
}

TEST_CASE("negative scale with translation and rotation decomposes consistently") {
    const Transform original{
        Vec3(5.0f, -1.0f, 2.0f),
        Quat::fromEuler(radians(30.0f), radians(-20.0f), radians(10.0f)),
        Vec3(-2.0f, 1.0f, 3.0f)};
    const Mat4 m = composeTRS(original);
    const Transform back = decompose(m);

    CHECK(veq(back.position, Vec3(5.0f, -1.0f, 2.0f)));
    CHECK(back.scale.z < 0.0f);
    CHECK(feq(std::fabs(back.scale.x), 2.0f));
    CHECK(feq(std::fabs(back.scale.y), 1.0f));
    CHECK(feq(std::fabs(back.scale.z), 3.0f));

    const Vec3 probe(0.75f, -0.5f, 1.25f);
    F4_CHECK_NEAR(composeTRS(back).transformPoint(probe), m.transformPoint(probe), kLooseTolerance);
}

TEST_CASE("decompose of composed transform is a fixed point") {
    const Transform t{
        Vec3(-4.5f, 2.25f, 8.0f),
        Quat::fromAxisAngle(normalized(Vec3(1.0f, -2.0f, 0.5f)), radians(73.0f)),
        Vec3(1.5f, 0.75f, 2.25f)};

    const Transform once = decompose(composeTRS(t));
    const Transform twice = decompose(composeTRS(once));

    CHECK(veq(twice.position, t.position, kTightTolerance));
    CHECK(feq(std::fabs(twice.scale.x), std::fabs(t.scale.x), kTightTolerance));
    CHECK(feq(std::fabs(twice.scale.y), std::fabs(t.scale.y), kTightTolerance));
    CHECK(feq(std::fabs(twice.scale.z), std::fabs(t.scale.z), kTightTolerance));

    const Vec3 v(1.0f, -0.75f, 0.25f);
    F4_CHECK_NEAR(once.rotation.rotate(v), t.rotation.rotate(v), kDefaultTolerance);
    F4_CHECK_NEAR(twice.rotation.rotate(v), t.rotation.rotate(v), kDefaultTolerance);
}

TEST_CASE("three level hierarchy chain yields exact coordinates") {
    const Transform grandparent{Vec3(10.0f, 0.0f, 0.0f), Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), radians(90.0f)), Vec3(2.0f)};
    const Transform parent{Vec3(1.0f, 0.0f, 0.0f), Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), radians(90.0f)), Vec3(1.0f)};
    const Transform child{Vec3(1.0f, 0.0f, 0.0f), Quat::identity(), Vec3(1.0f)};

    const Mat4 gpM = composeTRS(grandparent);
    const Mat4 pM = composeHierarchy(gpM, composeTRS(parent));
    const Mat4 cM = composeHierarchy(pM, composeTRS(child));

    const Vec3 inChild = cM.transformPoint(Vec3(0.5f, 0.0f, 0.0f));

    F4_CHECK_NEAR(inChild, Vec3(10.0f, 3.0f, -2.0f));
}
