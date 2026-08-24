#include <doctest/doctest.h>

#include <cmath>

#include "approx.hpp"
#include "core/math/mat3.h"
#include "core/math/mat4.h"
#include "core/math/transform.h"
#include "random.hpp"

using namespace f4::math;
using namespace f4test;

namespace {

constexpr int kSamples = 128;

// Well-conditioned rotation pair: angle between a and b kept in
// [10, 170] degrees so acos/dot-based comparisons stay numerically sane.
struct QuatPair {
    Quat a;
    Quat b;
};

auto makePair(Random& rng) -> QuatPair {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const Quat a = rng.nextQuat();
        const Quat b = rng.nextQuat();
        const float d = std::fabs(dot(a.normalized(), b.normalized()));
        if (d > 0.087f && d < 0.985f) { // keeps angles in ~[10, 85] degrees
            return {a.normalized(), b.normalized()};
        }
    }
    return {Quat::identity(), Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), radians(45.0f))};
}

auto angleBetween(const Quat& a, const Quat& b) -> float {
    return std::acos(clamp(std::fabs(dot(a.normalized(), b.normalized())), 0.0f, 1.0f));
}

} // namespace

TEST_CASE("property: quaternion composition homomorphism") {
    Random rng(0xC0FFEEu);
    const Vec3 probe(0.3f, -1.1f, 0.65f);
    for (int i = 0; i < kSamples; ++i) {
        const auto [a, b] = makePair(rng);
        F4_CHECK_NEAR((a * b).rotate(probe), a.rotate(b.rotate(probe)), kDefaultTolerance);
    }
}

TEST_CASE("property: rotation preserves vector length") {
    Random rng(0xBEEF01u);
    for (int i = 0; i < kSamples; ++i) {
        const Vec3 v(rng.nextSigned() * 5.0f, rng.nextSigned() * 5.0f, rng.nextSigned() * 5.0f);
        const Quat q = rng.nextQuat();
        F4_CHECK_NEAR(length(q.rotate(v)), length(v), kDefaultTolerance);
    }
}

TEST_CASE("property: normalization idempotence") {
    Random rng(0x1D3A71Fu); // arbitrary fixed seed constant
    for (int i = 0; i < kSamples; ++i) {
        // Deliberately non-unit input scaled by a random factor.
        const Quat q = rng.nextQuat() * rng.nextRange(0.25f, 4.0f);
        const Quat once = q.normalized();
        const Quat twice = once.normalized();
        CHECK(qeq(twice, once, kTightTolerance));
    }
}

TEST_CASE("property: inverse equals conjugate for unit quaternions") {
    Random rng(0x12AB45Fu);
    for (int i = 0; i < kSamples; ++i) {
        const Quat q = rng.nextQuat();
        F4_CHECK_NEAR(q.inverse(), q.conj(), kDefaultTolerance);
        F4_CHECK_NEAR(q.inverse().length(), 1.0f, kDefaultTolerance);
    }
}

TEST_CASE("property: slerp endpoints are exact up to normalization") {
    Random rng(0x3AD901Au);
    const Vec3 probe(0.45f, -1.35f, 0.8f);
    for (int i = 0; i < kSamples; ++i) {
        const auto [a, b] = makePair(rng);

        // Start always lands on a itself.
        F4_CHECK_NEAR(Quat::slerp(a, b, 0.0f), a, kTightTolerance);

        // End must match b's ROTATION, not its components: when
        // dot(a, b) < 0 the shortest-path sign fix interpolates toward -b,
        // which encodes the identical rotation.
        const Quat end = Quat::slerp(a, b, 1.0f);
        F4_CHECK_NEAR(end.rotate(probe), b.rotate(probe), kDefaultTolerance);
    }
}

TEST_CASE("property: slerp has constant angular velocity") {
    Random rng(0x4A6B1179u);
    constexpr float kQuarter = 0.25f;
    for (int i = 0; i < kSamples / 2; ++i) {
        const auto [a, b] = makePair(rng);
        const float theta = angleBetween(a, b);

        const float tValues[3] = {kQuarter, 0.5f, 0.75f};
        for (const float t : tValues) {
            const Quat s = Quat::slerp(a, b, t);
            // acos conditioning near small angles amplifies dot error by
            // 1/sin(theta); kLoose absorbs that without hiding real drift.
            F4_CHECK_NEAR(angleBetween(s, a), t * theta, kLooseTolerance);
            F4_CHECK_NEAR(angleBetween(s, b), (1.0f - t) * theta, kLooseTolerance);
        }

        // Monotonic advance along the arc.
        const float t1 = 0.3f;
        const float t2 = 0.6f;
        CHECK(angleBetween(Quat::slerp(a, b, t2), a) > angleBetween(Quat::slerp(a, b, t1), a));
    }
}

TEST_CASE("property: shortest-path and sign-fix invariance of slerp") {
    Random rng(0x510B7FAu);
    const Vec3 probe(-0.45f, 0.8f, 1.25f);
    for (int i = 0; i < kSamples; ++i) {
        const auto [a, b] = makePair(rng);

        // Negating the endpoint must not change the swept ROTATION:
        // the sign fix inside slerp normalizes both to the same arc.
        const Quat direct = Quat::slerp(a, b, 0.5f);
        const Quat flipped = Quat::slerp(a, -b, 0.5f);
        F4_CHECK_NEAR(direct.rotate(probe), flipped.rotate(probe), kLooseTolerance);

        // Geodesic partition: the full angle splits at the midpoint.
        const float theta = angleBetween(a, b);
        F4_CHECK_NEAR(angleBetween(direct, a), 0.5f * theta, kLooseTolerance);
    }
}

TEST_CASE("property: double cover, q and -q behave identically") {
    Random rng(0xD0AB1ECu);
    const Vec3 probe(2.0f, -0.5f, 0.125f);
    for (int i = 0; i < kSamples; ++i) {
        const Quat q = rng.nextQuat();
        const Quat nq = -q;
        // rotate() mirrors exactly through negation: bitwise identity holds.
        CHECK(q.rotate(probe) == nq.rotate(probe));

        const Vec3 v(rng.nextSigned(), rng.nextSigned(), rng.nextSigned());
        CHECK(q.rotate(v) == nq.rotate(v));
    }
}

TEST_CASE("property: matrix to quat to matrix roundtrip is authoritative") {
    Random rng(0x70084A7Du);
    for (int i = 0; i < kSamples; ++i) {
        const Quat q = rng.nextQuat();
        const Mat3 m = Mat3::fromQuat(q);
        const Mat3 back = Mat3::fromQuat(toQuat(m));
        CHECK(meq(back, m, kDefaultTolerance));

        // Basis-vector semantics: recovered rotation moves axes identically.
        const Quat extracted = toQuat(m);
        F4_CHECK_NEAR(extracted.rotate(Vec3(1.0f, 0.0f, 0.0f)), m.mulDir(Vec3(1.0f, 0.0f, 0.0f)), kDefaultTolerance);
        F4_CHECK_NEAR(extracted.rotate(Vec3(0.0f, 1.0f, 0.0f)), m.mulDir(Vec3(0.0f, 1.0f, 0.0f)), kDefaultTolerance);
        F4_CHECK_NEAR(extracted.rotate(Vec3(0.0f, 0.0f, 1.0f)), m.mulDir(Vec3(0.0f, 0.0f, 1.0f)), kDefaultTolerance);
    }
}

TEST_CASE("property: euler composition order yaw*pitch*roll is pinned") {
    Random rng(0xE117B17u);
    const Vec3 probe(0.9f, -0.2f, 0.4f);
    for (int i = 0; i < kSamples; ++i) {
        const float yaw = rng.nextSigned() * kPi;
        const float pitch = rng.nextRange(-1.4f, 1.4f); // away from gimbal here
        const float roll = rng.nextSigned() * kPi;

        const Quat combined = Quat::fromEuler(yaw, pitch, roll);
        const Vec3 stepwise = Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), yaw)
                                  .rotate(Quat::fromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), pitch)
                                              .rotate(Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), roll)
                                                          .rotate(probe)));
        F4_CHECK_NEAR(combined.rotate(probe), stepwise, kLooseTolerance);
    }
}

TEST_CASE("property: zero axis produces identity behavior for any angle") {
    Random rng(0x20A115Au);
    const Vec3 zero(0.0f);
    const Vec3 probe(-1.5f, 2.5f, 0.75f);
    for (int i = 0; i < kSamples; ++i) {
        const Quat q = Quat::fromAxisAngle(zero, rng.nextSigned() * 2.0f * kPi);
        const bool vectorPartIsZero = q.x == 0.0f && q.y == 0.0f && q.z == 0.0f;
        CHECK(vectorPartIsZero);
        CHECK(q.rotate(probe) == probe);
    }
}

TEST_CASE("property: gimbal pitch plus-minus 90 degrees stays stable") {
    Random rng(0x91BA1Cu);
    const Vec3 probe(0.6f, 0.35f, -0.85f);
    for (int i = 0; i < kSamples; ++i) {
        const float yaw = rng.nextSigned() * kPi;
        const float roll = rng.nextSigned() * kPi;

        for (const float pitch : {radians(89.999f), radians(90.0f), radians(-89.999f), radians(-90.0f)}) {
            const Quat q = Quat::fromEuler(yaw, pitch, roll);
            F4_CHECK_NEAR(q.lengthSq(), 1.0f, kDefaultTolerance);

            // Composition identity must hold right up to the pole.
            const Vec3 stepwise = Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), yaw)
                                      .rotate(Quat::fromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), pitch)
                                                  .rotate(Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), roll)
                                                              .rotate(probe)));
            F4_CHECK_NEAR(q.rotate(probe), stepwise, kLooseTolerance);

            // Roundtrip through the matrix form stays put at the pole.
            const Mat3 m = Mat3::fromQuat(q);
            const Quat back = toQuat(m);
            F4_CHECK_NEAR(Mat3::fromQuat(back), m, kDefaultTolerance);
        }
    }
}

TEST_CASE("toQuat pins identity and half-turn rotations") {
    // Identity is exact: trace branch yields components 0 bit-for-bit.
    const Quat qi = toQuat(Mat3::identity());
    CHECK(qi == Quat::identity());

    for (const Vec3 axis : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)}) {
        const Mat3 halfTurn = Mat3::fromQuat(Quat::fromAxisAngle(axis, kPi));
        const Quat back = toQuat(halfTurn);
        CHECK(meq(Mat3::fromQuat(back), halfTurn));
        F4_CHECK_NEAR(back.lengthSq(), 1.0f, kDefaultTolerance);

        // Near-half-turn must not blow up either branch.
        const Mat3 almostHalf = Mat3::fromQuat(Quat::fromAxisAngle(axis, kPi - radians(0.05f)));
        const Quat backAlmost = toQuat(almostHalf);
        CHECK(meq(Mat3::fromQuat(backAlmost), almostHalf, kDefaultTolerance));
    }
}

TEST_CASE("toQuat tolerates drifted matrix inputs") {
    Random rng(0xDF71B07u);
    const Vec3 probe(0.55f, -1.2f, 0.4f);
    for (int i = 0; i < kSamples; ++i) {
        const Quat q = rng.nextQuat();
        Mat3 drifted = Mat3::fromQuat(q);
        // Mild non-orthogonal perturbation: scale + skew on columns.
        const float d1 = 1.0f + rng.nextSigned() * 1e-5f;
        const float d2 = 1.0f + rng.nextSigned() * 1e-5f;
        const float d3 = 1.0f + rng.nextSigned() * 1e-5f;
        drifted.col[0] = drifted.col[0] * d1 + drifted.col[1] * 1e-5f;
        drifted.col[1] = drifted.col[1] * d2 + drifted.col[2] * 1e-5f;
        drifted.col[2] = drifted.col[2] * d3;

        const Quat back = toQuat(drifted);
        const bool allFinite = std::isfinite(back.w) && std::isfinite(back.x) && std::isfinite(back.y) && std::isfinite(back.z);
        CHECK(allFinite);
        F4_CHECK_NEAR(back.rotate(probe), q.rotate(probe), kLooseTolerance);
    }
}

TEST_CASE("toQuat extracts rotations from mat4 ignoring translation") {
    Random rng(0xA47DA44u);
    const Vec3 probe(1.25f, 2.0f, -0.5f);
    for (int i = 0; i < kSamples / 2; ++i) {
        const Quat q = rng.nextQuat();
        const Mat4 trs = composeTRS(Transform{
            Vec3(rng.nextSigned() * 10.0f, rng.nextSigned() * 10.0f, rng.nextSigned() * 10.0f),
            q,
            Vec3(1.0f, 1.0f, 1.0f)});
        const Quat back = toQuat(trs);
        F4_CHECK_NEAR(back.rotate(probe), q.rotate(probe), kDefaultTolerance);
    }
}
