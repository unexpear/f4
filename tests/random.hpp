#pragma once

#include <cmath>
#include <cstdint>
#include <random>

#include "core/math/numerics.h"
#include "core/math/quat.h"
#include "core/math/vec3.h"

namespace f4test {

// Deterministic random source for property tests and, later, M8 GPU-parity
// fixtures. Portability contract: the same seed produces the exact same
// sequence on every compiler and standard library.
//   - mt19937 and its seeding are fully specified by the standard.
//   - Floats never touch std::uniform_real_distribution (implementation-defined
//     output); they are built from raw 32-bit words via a canonical mapping.
// The stream position depends only on the seed and the call sequence, so a
// fixture recorded today replays identically under GCC, MSVC, and shaders-fed
// tables in M8.
class Random {
public:
    explicit Random(std::uint32_t seed) : engine_(seed) {}

    [[nodiscard]] auto nextU32() -> std::uint32_t { return engine_(); }

    // Canonical [0, 1): top 24 bits scaled by 2^-24. Every returned float is
    // an exact multiple of 2^-24, so the mapping is reproducible everywhere
    // IEEE-754 single precision exists (including GPUs).
    [[nodiscard]] auto nextUnit() -> float {
        return static_cast<float>(engine_() >> 8) * (1.0f / 16777216.0f);
    }

    // Uniform-ish value in [minInclusive, maxExclusive).
    [[nodiscard]] auto nextRange(float minInclusive, float maxExclusive) -> float {
        return minInclusive + (maxExclusive - minInclusive) * nextUnit();
    }

    // Uniform-ish in [-1, 1).
    [[nodiscard]] auto nextSigned() -> float { return nextRange(-1.0f, 1.0f); }

    // Unit-length direction via rejection sampling inside the unit sphere,
    // then normalization. Bounded attempts; the fallback has probability far
    // below one in 2^24 per call. Deterministic for a given seed.
    [[nodiscard]] auto nextDirection() -> f4::math::Vec3 {
        using f4::math::Vec3;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const Vec3 p(nextSigned(), nextSigned(), nextSigned());
            const float lenSq = lengthSq(p);
            if (lenSq > 1e-6f && lenSq <= 1.0f) {
                return p / std::sqrt(lenSq);
            }
        }
        return Vec3(1.0f, 0.0f, 0.0f);
    }

    // Rotation uniform over angle, near-uniform over axis.
    [[nodiscard]] auto nextQuat() -> f4::math::Quat {
        return f4::math::Quat::fromAxisAngle(nextDirection(), nextSigned() * f4::math::kPi);
    }

private:
    std::mt19937 engine_;
};

} // namespace f4test