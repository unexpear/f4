#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/quat.h"

using namespace f4::math;
using namespace f4test;

namespace {

const Vec3 xAxis(1.0f, 0.0f, 0.0f);
const Vec3 yAxis(0.0f, 1.0f, 0.0f);
const Vec3 zAxis(0.0f, 0.0f, 1.0f);

} // namespace

TEST_CASE("identity quat leaves vectors unchanged") {
    const Vec3 v(1.0f, 2.0f, 3.0f);
    CHECK(veq(Quat::identity().rotate(v), v));
}

TEST_CASE("axis angle: quarter rotation about Z maps X to Y") {
    const Quat q = Quat::fromAxisAngle(zAxis, radians(90.0f));
    CHECK(veq(q.rotate(xAxis), yAxis));
    CHECK(veq(q.rotate(yAxis), -xAxis));

    const Mat4 m = Mat4::fromQuat(q);
    CHECK(veq(m.transformVector(xAxis), yAxis));
}

TEST_CASE("composition of rotations equals combined rotation") {
    const Quat qz = Quat::fromAxisAngle(zAxis, radians(90.0f));
    const Quat qy = Quat::fromAxisAngle(yAxis, radians(90.0f));
    const Vec3 v = zAxis;

    const Vec3 viaQuats = (qy * qz).rotate(v);
    const Vec3 stepwise = qy.rotate(qz.rotate(v));
    CHECK(veq(viaQuats, stepwise));
}

TEST_CASE("double cover: negated quat is the same rotation") {
    const Quat q = Quat::fromAxisAngle(xAxis, radians(37.0f));
    const Vec3 v(0.3f, -1.2f, 2.5f);
    CHECK(veq((-q).rotate(v), q.rotate(v)));
}

TEST_CASE("fromEuler applies yaw then pitch then roll") {
    const float yaw = radians(30.0f);
    const float pitch = radians(20.0f);
    const float roll = radians(10.0f);

    const Quat composed = Quat::fromAxisAngle(yAxis, yaw)
                       * Quat::fromAxisAngle(xAxis, pitch)
                       * Quat::fromAxisAngle(zAxis, roll);
    CHECK(qeq(Quat::fromEuler(yaw, pitch, roll), composed, kDefaultTolerance));
}

TEST_CASE("inverse rotates back") {
    const Quat q = Quat::fromAxisAngle(normalized(Vec3(1.0f, 1.0f, 1.0f)), radians(63.0f));
    const Vec3 v(0.5f, -0.25f, 1.75f);
    CHECK(veq(q.inverse().rotate(q.rotate(v)), v, kLooseTolerance));
}

TEST_CASE("conjugate and inverse agree and compose to identity") {
    const Quat q = Quat::fromAxisAngle(normalized(Vec3(1.0f, 1.0f, 1.0f)), radians(63.0f));
    const Quat inv = q.inverse();

    CHECK(qeq(q * inv, Quat::identity()));
    CHECK(qeq(inv * q, Quat::identity()));
    CHECK(qeq(inv, q.conj(), kLooseTolerance));
}

TEST_CASE("normalize keeps unit length and the same rotation") {
    const Quat q = Quat::fromAxisAngle(normalized(Vec3(1.0f, 1.0f, 1.0f)), radians(120.0f));
    const Quat scaled = q * 7.0f;
    const Quat n = scaled.normalized();

    CHECK(feq(n.length(), 1.0f, kDefaultTolerance));
    CHECK(qeq(n, q));
    CHECK(veq(n.rotate(xAxis), yAxis));
    CHECK(veq(n.rotate(yAxis), zAxis));
}

TEST_CASE("slerp endpoints and midpoint") {
    const Quat a = Quat::identity();
    const Quat b = Quat::fromAxisAngle(yAxis, radians(90.0f));

    CHECK(qeq(Quat::slerp(a, b, 0.0f), a, kLooseTolerance));
    CHECK(qeq(Quat::slerp(a, b, 1.0f), b, kLooseTolerance));

    const Quat mid = Quat::slerp(a, b, 0.5f);
    CHECK(veq(mid.rotate(xAxis), Vec3(std::cos(radians(45.0f)), 0.0f, -std::sin(radians(45.0f))), kLooseTolerance));
}

TEST_CASE("slerp takes the shortest path across double cover") {
    const Quat a = Quat::identity();
    const Quat bNegated = -Quat::fromAxisAngle(yAxis, radians(20.0f));

    const Quat mid = Quat::slerp(a, bNegated, 0.5f);
    const float angle = degrees(2.0f * std::acos(clamp(mid.w, -1.0f, 1.0f)));
    CHECK(angle < 21.0f);
}

TEST_CASE("zero axis and zero quaternion degenerate guards") {
    const Vec3 zero(0.0f);
    const Quat q = Quat::fromAxisAngle(zero, radians(123.0f));
    CHECK((q.x == 0.0f && q.y == 0.0f && q.z == 0.0f));
    F4_CHECK_NEAR(q.rotate(xAxis), xAxis);
    F4_CHECK_NEAR(q.rotate(yAxis), yAxis);

    const Quat zq(0.0f, 0.0f, 0.0f, 0.0f);
    const Quat nzq = zq.normalized();
    CHECK(qeq(nzq, Quat::identity()));
}

TEST_CASE("slerp is stable for identical and exactly opposite quaternions") {
    const Quat q = Quat::fromAxisAngle(yAxis, radians(37.0f));
    const Quat opposite = -q;
    const Vec3 probe(0.5f, -1.0f, 0.25f);

    const Quat sameDir = Quat::slerp(q, q, 0.35f);
    const bool sameIsQ = qeq(sameDir, q);
    const bool sameIsNegQ = qeq(sameDir, -q);
    const bool sameRotation = sameIsQ || sameIsNegQ;
    CHECK(sameRotation);

    const Quat fromOpposite = Quat::slerp(opposite, q, 0.5f);
    const bool oppIsQ = qeq(fromOpposite, q);
    const bool oppIsNegQ = qeq(fromOpposite, -q);
    const bool oppRotation = oppIsQ || oppIsNegQ;
    CHECK(oppRotation);
    F4_CHECK_NEAR(fromOpposite.rotate(probe), q.rotate(probe), kLooseTolerance);

    const Quat toOpposite = Quat::slerp(q, opposite, 0.5f);
    F4_CHECK_NEAR(toOpposite.rotate(probe), q.rotate(probe), kLooseTolerance);
}

TEST_CASE("unit quaternion rotation preserves vector length") {
    const Quat q = Quat::fromEuler(radians(31.0f), radians(-47.0f), radians(12.0f));
    const Vec3 v(0.5f, -1.25f, 2.0f);

    CHECK(feq(length(q.rotate(v)), length(v), kTightTolerance));

    const Quat zq(0.0f, 0.0f, 0.0f, 0.0f);
    const Quat invZ = zq.inverse();
    CHECK(std::isnan(invZ.w));
    CHECK(std::isnan(invZ.x));
    CHECK(std::isnan(invZ.y));
    CHECK(std::isnan(invZ.z));
}
