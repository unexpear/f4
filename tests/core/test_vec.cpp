#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/vec2.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"

using namespace f4::math;
using namespace f4test;

TEST_CASE("vec arithmetic operators") {
    Vec3 a(1.0f, 2.0f, 3.0f);
    Vec3 b(4.0f, 5.0f, 6.0f);

    CHECK(veq(a + b, Vec3(5.0f, 7.0f, 9.0f)));
    CHECK(veq(b - a, Vec3(3.0f, 3.0f, 3.0f)));
    CHECK(veq(a * 2.0f, Vec3(2.0f, 4.0f, 6.0f)));
    CHECK(veq(2.0f * a, Vec3(2.0f, 4.0f, 6.0f)));
    CHECK(veq(b / 2.0f, Vec3(2.0f, 2.5f, 3.0f)));
    CHECK(veq(-a, Vec3(-1.0f, -2.0f, -3.0f)));

    Vec3 c = a;
    c += b;
    CHECK(veq(c, Vec3(5.0f, 7.0f, 9.0f)));
    c -= a;
    CHECK(veq(c, b));
    c *= 0.5f;
    CHECK(veq(c, Vec3(2.0f, 2.5f, 3.0f)));
    c /= 2.0f;
    CHECK(veq(c, Vec3(1.0f, 1.25f, 1.5f)));
}

TEST_CASE("vec subscript and splat constructor") {
    Vec4 v(1.0f);
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 1.0f);
    CHECK(v[2] == 1.0f);
    CHECK(v[3] == 1.0f);
    v[2] = 9.0f;
    CHECK(v.z == 9.0f);

    Vec3 p(7.0f);
    CHECK(p.x == 7.0f);
    CHECK(p.y == 7.0f);
    CHECK(p.z == 7.0f);
}

TEST_CASE("dot product") {
    CHECK(dot(Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f)) == 0.0f);
    CHECK(dot(Vec3(1.0f, 2.0f, 3.0f), Vec3(4.0f, 5.0f, 6.0f)) == 32.0f);
    CHECK(dot(Vec4(1.0f, 2.0f, 3.0f, 4.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f)) == 10.0f);
}

TEST_CASE("cross product is orthogonal and right-handed") {
    Vec3 x(1.0f, 0.0f, 0.0f);
    Vec3 y(0.0f, 1.0f, 0.0f);
    Vec3 z = cross(x, y);

    CHECK(veq(z, Vec3(0.0f, 0.0f, 1.0f)));
    CHECK(approx(dot(x, z), 0.0f));
    CHECK(approx(dot(y, z), 0.0f));
    CHECK(veq(cross(y, x), -z));

    CHECK(cross(x, x) == Vec3(0.0f, 0.0f, 0.0f));
}

TEST_CASE("length, distance, normalize") {
    CHECK(feq(length(Vec2(3.0f, 4.0f)), 5.0f));
    CHECK(lengthSq(Vec3(2.0f, 0.0f, 0.0f)) == 4.0f);
    CHECK(feq(distance(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 2.0f, 2.0f)), 3.0f));

    Vec3 n = normalized(Vec3(0.0f, 3.0f, 4.0f));
    CHECK(feq(length(n), 1.0f));
    CHECK(veq(n, Vec3(0.0f, 0.6f, 0.8f)));

    Vec3 zero = normalized(Vec3(0.0f, 0.0f, 0.0f));
    CHECK(zero == Vec3(0.0f, 0.0f, 0.0f));
}

TEST_CASE("reflect mirrors across the normal") {
    Vec3 incident(1.0f, -1.0f, 0.0f);
    Vec3 n(0.0f, 1.0f, 0.0f);
    CHECK(veq(reflect(incident, n), Vec3(1.0f, 1.0f, 0.0f)));
}

TEST_CASE("perp of vec2") {
    CHECK(veq(perp(Vec2(1.0f, 0.0f)), Vec2(0.0f, 1.0f)));
    CHECK(approx(dot(Vec2(1.0f, 2.0f), perp(Vec2(1.0f, 2.0f))), 0.0f));
}

TEST_CASE("componentwise min/max and lerp") {
    CHECK(veq(minComponentwise(Vec3(1.0f, 5.0f, 3.0f), Vec3(2.0f, 4.0f, 3.0f)), Vec3(1.0f, 4.0f, 3.0f)));
    CHECK(veq(maxComponentwise(Vec3(1.0f, 5.0f, 3.0f), Vec3(2.0f, 4.0f, 3.0f)), Vec3(2.0f, 5.0f, 3.0f)));
    CHECK(veq(lerp(Vec3(0.0f), Vec3(10.0f, 20.0f, 30.0f), 0.25f), Vec3(2.5f, 5.0f, 7.5f)));
    CHECK(veq(lerp(Vec4(0.0f), Vec4(4.0f), 0.5f), Vec4(2.0f)));
}

TEST_CASE("vec4 xyz accessor") {
    Vec4 v(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK(veq(v.xyz(), Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(Vec4(Vec3(1.0f, 2.0f, 3.0f), 4.0f) == v);
}

TEST_CASE("zero vector battery") {
    const Vec2 z2(0.0f);
    const Vec3 z3(0.0f, 0.0f, 0.0f);
    const Vec4 z4(0.0f);

    CHECK(lengthSq(z2) == 0.0f);
    CHECK(length(z3) == 0.0f);
    CHECK(dot(z3, Vec3(1.0f, -5.0f, 9.0f)) == 0.0f);
    CHECK(cross(z3, Vec3(1.0f, -5.0f, 9.0f)) == Vec3(0.0f));
    CHECK(cross(Vec3(1.0f, -5.0f, 9.0f), z3) == Vec3(0.0f));
    CHECK(minComponentwise(z3, Vec3(1.0f, -5.0f, 9.0f)) == Vec3(0.0f, -5.0f, 0.0f));
    CHECK(maxComponentwise(z4, Vec4(1.0f)) == Vec4(1.0f));
    CHECK(lerp(z3, Vec3(10.0f), 0.5f) == Vec3(5.0f));
    CHECK(reflect(Vec3(1.0f, -1.0f, 0.0f), z3) == Vec3(1.0f, -1.0f, 0.0f));

    CHECK(normalized(z2) == Vec2(0.0f));
    CHECK(normalized(z3) == Vec3(0.0f));

    const Vec3 p(1.0f, 2.0f, 3.0f);
    F4_CHECK_NEAR(distance(p, p), 0.0f, kTightTolerance);
}

TEST_CASE("ieee division by zero pins") {
    const float zero = 0.0f;

    const Vec3 posDiv = Vec3(1.0f, -2.0f, 3.0f) / zero;
    CHECK(std::isinf(posDiv.x));
    CHECK(!std::signbit(posDiv.x));
    CHECK(std::isinf(posDiv.y));
    CHECK(std::signbit(posDiv.y));
    CHECK(std::isinf(posDiv.z));
    CHECK(!std::signbit(posDiv.z));

    const Vec3 zeroDiv = Vec3(0.0f) / zero;
    CHECK(std::isnan(zeroDiv.x));
    CHECK(std::isnan(zeroDiv.y));
    CHECK(std::isnan(zeroDiv.z));
}
