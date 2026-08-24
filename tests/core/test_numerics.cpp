#include <doctest/doctest.h>

#include "approx.hpp"
#include "core/math/numerics.h"

using namespace f4::math;
using namespace f4test;

TEST_CASE("radians and degrees round trip") {
    CHECK(feq(radians(180.0f), kPi));
    CHECK(feq(radians(90.0f), kHalfPi));
    CHECK(feq(degrees(kPi), 180.0f));
    for (float deg = -360.0f; deg <= 360.0f; deg += 45.0f) {
        CHECK(feq(degrees(radians(deg)), deg));
    }
}

TEST_CASE("min max clamp saturate") {
    CHECK(min(1.0f, 2.0f) == 1.0f);
    CHECK(max(1.0f, 2.0f) == 2.0f);
    CHECK(clamp(5.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(clamp(-5.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(clamp(0.75f, 0.0f, 1.0f) == 0.75f);
    CHECK(saturate(1.5f) == 1.0f);
    CHECK(saturate(-0.5f) == 0.0f);
    CHECK(saturate(0.25f) == 0.25f);
}

TEST_CASE("lerp endpoints and midpoint") {
    CHECK(lerp(0.0f, 10.0f, 0.0f) == 0.0f);
    CHECK(lerp(0.0f, 10.0f, 1.0f) == 10.0f);
    CHECK(feq(lerp(0.0f, 10.0f, 0.35f), 3.5f));
}

TEST_CASE("sign and approx") {
    CHECK(sign(42.0f) == 1.0f);
    CHECK(sign(-42.0f) == -1.0f);
    CHECK(sign(0.0f) == 0.0f);

    CHECK(approx(1.0f, 1.000001f));
    CHECK(!approx(1.0f, 1.01f));
    CHECK(approxZero(1e-7f));
    CHECK(!approxZero(0.5f));

    constexpr float big = 123456.0f;
    CHECK(approx(big, big + 0.005f, 0.01f));
    CHECK(!approx(big, big + 0.005f));
}

TEST_CASE("constants are sane") {
    CHECK(feq(kTwoPi, 2.0f * kPi, kTightTolerance));
    CHECK(feq(kHalfPi, 0.5f * kPi, kTightTolerance));
}
