#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <ios>
#include <iterator>

#include "core/math/vec2.h"
#include "core/noise/noise.h"

using namespace f4;
using f4::noise::fbm2D;
using f4::noise::fbm3D;
using f4::noise::perlin2D;
using f4::noise::perlin3D;
using f4::noise::simplex2D;
using f4::noise::value2D;
using f4::noise::value3D;
using f4::noise::warp2D;

namespace {
struct Golden {
    float x;
    float y;
    float z;
    std::uint64_t seed;
    std::uint64_t bits;
};

// Bit-exact expected values.
//
// GENERATION RECIPE (frozen - do not regenerate casually):
//   Compiled against src/core/noise/noise.cpp AS COMMITTED, GCC ucrt64,
//   flags: -std=c++20 -ffp-contract=off -O2, after the whole six-preset
//   matrix was verified green under the locked FP contract
//   (-ffp-contract=off / /fp:precise). Values are IEEE-754 bit patterns of
//   single-precision results. MSVC suites passing these entries IS the
//   cross-toolchain parity proof required by plan section 11.6.
static constexpr Golden kValue2D[] = {
    {0.25f, 0.75f, 0.f, 0x0000000000000001ull, 0x3EA29B7Full}, // value2D
    {-3.4000001f, 7.9000001f, 0.f, 0x0000000000000001ull, 0xBF0A0A36ull},
    {11.f, -2.125f, 0.f, 0x0000000000000001ull, 0x3EDC31FCull},
    {0.f, 0.f, 0.f, 0x0000000000000001ull, 0xBE86F954ull},
    {0.25f, 0.75f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBD63E054ull},
    {-3.4000001f, 7.9000001f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBE8736E6ull},
    {11.f, -2.125f, 0.f, 0xDEADBEEFCAFEBABEull, 0x3F0599B4ull},
    {0.f, 0.f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBE4E04F0ull},
};
static constexpr Golden kValue3D[] = {
    {0.25f, 0.75f, 0.5f, 0x0000000000000001ull, 0x3F1B61D6ull},
    {-3.4000001f, 7.9000001f, -1.20000005f, 0x0000000000000001ull, 0x3EEEDE25ull},
    {11.f, -2.125f, 3.75f, 0x0000000000000001ull, 0x3EEDFFEEull},
    {0.f, 0.f, 0.f, 0x0000000000000001ull, 0xBE86F954ull},
    {0.25f, 0.75f, 0.5f, 0xDEADBEEFCAFEBABEull, 0x3EB47D74ull},
    {-3.4000001f, 7.9000001f, -1.20000005f, 0xDEADBEEFCAFEBABEull, 0xBD81D570ull},
    {11.f, -2.125f, 3.75f, 0xDEADBEEFCAFEBABEull, 0xBF11698Aull},
    {0.f, 0.f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBE4E04F0ull},
};
static constexpr Golden kPerlin2D[] = {
    {0.370000005f, 1.62f, 0.f, 0x0000000000000001ull, 0x3C46EC18ull},
    {-8.13000011f, 4.05000019f, 0.f, 0x0000000000000001ull, 0xBD977938ull},
    {100.5f, -60.25f, 0.f, 0x0000000000000001ull, 0xBDF7A068ull},
    {0.f, 0.f, 0.f, 0x0000000000000001ull, 0x00000000ull},
    {0.370000005f, 1.62f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBE0748ABull},
    {-8.13000011f, 4.05000019f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBE114E02ull},
    {100.5f, -60.25f, 0.f, 0xDEADBEEFCAFEBABEull, 0x3EE3E819ull},
    {0.f, 0.f, 0.f, 0xDEADBEEFCAFEBABEull, 0x00000000ull},
};
static constexpr Golden kPerlin3D[] = {
    {0.310000002f, 1.76999998f, 0.439999998f, 0x0000000000000001ull, 0xBE3F5844ull},
    {-8.13000011f, 4.05000019f, -2.5999999f, 0x0000000000000001ull, 0xBEACDE46ull},
    {55.125f, -12.75f, 90.5f, 0x0000000000000001ull, 0xBEB77D00ull},
    {0.f, 0.f, 0.f, 0x0000000000000001ull, 0x00000000ull},
    {0.310000002f, 1.76999998f, 0.439999998f, 0xDEADBEEFCAFEBABEull, 0x3EBFBD71ull},
    {-8.13000011f, 4.05000019f, -2.5999999f, 0xDEADBEEFCAFEBABEull, 0x3E44436Aull},
    {55.125f, -12.75f, 90.5f, 0xDEADBEEFCAFEBABEull, 0xBE27DA00ull},
    {0.f, 0.f, 0.f, 0xDEADBEEFCAFEBABEull, 0x00000000ull},
};
static constexpr Golden kSimplex2D[] = {
    {0.620000005f, 0.479999989f, 0.f, 0x0000000000000001ull, 0x3E1E2559ull},
    {-14.1999998f, 9.85000038f, 0.f, 0x0000000000000001ull, 0x3DF3A5BDull},
    {77.6999969f, -33.2999992f, 0.f, 0x0000000000000001ull, 0xBE2F198Aull},
    {0.f, 0.f, 0.f, 0x0000000000000001ull, 0x00000000ull},
    {0.620000005f, 0.479999989f, 0.f, 0xDEADBEEFCAFEBABEull, 0x3E1E2559ull},
    {-14.1999998f, 9.85000038f, 0.f, 0xDEADBEEFCAFEBABEull, 0xBF1AF105ull},
    {77.6999969f, -33.2999992f, 0.f, 0xDEADBEEFCAFEBABEull, 0xB7437FE3ull},
    {0.f, 0.f, 0.f, 0xDEADBEEFCAFEBABEull, 0x00000000ull},
};
static constexpr Golden kFbm2D[] = {
    {1.23399997f, -2.34500003f, 0.f, 0x000000000000002Aull, 0xBE0DA7D9ull},
    {40.75f, 18.25f, 0.f, 0x000000000000002Aull, 0xBE1AF909ull},
    {-100.f, 0.00100000005f, 0.f, 0x000000000000002Aull, 0x3AA2D9A4ull},
    {1.23399997f, -2.34500003f, 0.f, 0x0000000000000007ull, 0x3E0FBB95ull},
    {40.75f, 18.25f, 0.f, 0x0000000000000007ull, 0xBE02B5ADull},
    {-100.f, 0.00100000005f, 0.f, 0x0000000000000007ull, 0x3B092EF7ull},
};
static constexpr Golden kFbm3D[] = {
    {1.5f, 2.5f, 3.5f, 0x000000000000002Aull, 0x3E888889ull},
    {-20.125f, 10.375f, -30.0625f, 0x000000000000002Aull, 0xBD6D1300ull},
    {1.5f, 2.5f, 3.5f, 0x0000000000000007ull, 0xBD888889ull},
    {-20.125f, 10.375f, -30.0625f, 0x0000000000000007ull, 0x3DC0C1AFull},
};
static constexpr Golden kWarp2D[] = {
    {0.5f, 1.25f, 0.f, 0x0000000000000063ull, 0xBFA35777ull}, // warp2D.x
    {0.5f, 1.25f, 0.f, 0x0000000000000063ull, 0xBD768368ull}, // warp2D.y
    {-9.75f, 22.5f, 0.f, 0x0000000000000063ull, 0x3D8C0000ull},
    {-9.75f, 22.5f, 0.f, 0x0000000000000063ull, 0xBF50B22Bull},
};

[[nodiscard]] auto bitsOf(float v) -> std::uint32_t {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

template <typename Fn>
void checkGoldenBits(const Golden (&table)[], std::size_t count, const char* label, Fn&& fn) {
    for (std::size_t i = 0; i < count; ++i) {
        const Golden& g = table[i];
        const float v = fn(g.x, g.y, g.z, g.seed);
        INFO(label << " case " << i << ": got 0x" << std::hex << bitsOf(v) << " want 0x"
                   << static_cast<std::uint32_t>(g.bits));
        CHECK(bitsOf(v) == static_cast<std::uint32_t>(g.bits));
    }
}
} // namespace

TEST_CASE("golden vectors are bit-exact") {
    checkGoldenBits(kValue2D, std::size(kValue2D), "value2D",
                    [](float x, float y, float z, std::uint64_t s) {
                        (void)z;
                        return value2D(x, y, s);
                    });
    checkGoldenBits(kValue3D, std::size(kValue3D), "value3D",
                    [](float x, float y, float z, std::uint64_t s) { return value3D(x, y, z, s); });
    checkGoldenBits(kPerlin2D, std::size(kPerlin2D), "perlin2D",
                    [](float x, float y, float z, std::uint64_t s) {
                        (void)z;
                        return perlin2D(x, y, s);
                    });
    checkGoldenBits(kPerlin3D, std::size(kPerlin3D), "perlin3D",
                    [](float x, float y, float z, std::uint64_t s) { return perlin3D(x, y, z, s); });
    checkGoldenBits(kSimplex2D, std::size(kSimplex2D), "simplex2D",
                    [](float x, float y, float z, std::uint64_t s) {
                        (void)z;
                        return simplex2D(x, y, s);
                    });
    checkGoldenBits(
        kFbm2D, std::size(kFbm2D), "fbm2D", [](float x, float y, float z, std::uint64_t s) {
            (void)z;
            noise::FbmParams p{};
            p.octaves = 5;
            return fbm2D(p, x, y, s);
        });
    checkGoldenBits(
        kFbm3D, std::size(kFbm3D), "fbm3D", [](float x, float y, float z, std::uint64_t s) {
            noise::FbmParams p{};
            p.octaves = 4;
            return fbm3D(p, x, y, z, s);
        });
    SUBCASE("warp2D components") {
        const math::Vec2 w1 = warp2D(0.5f, 1.25f, 99ull, 3.5f);
        CHECK(bitsOf(w1.x) == 0xBFA35777u);
        CHECK(bitsOf(w1.y) == 0xBD768368u);
        const math::Vec2 w2 = warp2D(-9.75f, 22.5f, 99ull, 3.5f);
        CHECK(bitsOf(w2.x) == 0x3D8C0000u);
        CHECK(bitsOf(w2.y) == 0xBF50B22Bu);
    }
}

TEST_CASE("outputs stay within documented ranges") {
    const std::uint64_t seed = 0x1234'5678'9ABC'DEF0ull;
    for (int iy = -30; iy <= 30; ++iy) {
        for (int ix = -30; ix <= 30; ++ix) {
            const float x = static_cast<float>(ix) + 0.37f;
            const float y = static_cast<float>(iy) - 0.62f;
            const float z = static_cast<float>((ix * 31 + iy * 17) % 13) - 6.5f;
            CHECK(value2D(x, y, seed) >= -1.000001f);
            CHECK(value2D(x, y, seed) <= 1.000001f);
            CHECK(perlin2D(x, y, seed) >= -1.000001f);
            CHECK(perlin2D(x, y, seed) <= 1.000001f);
            CHECK(simplex2D(x, y, seed) >= -1.000001f);
            CHECK(simplex2D(x, y, seed) <= 1.000001f);
            if ((ix + iy) % 3 == 0) {
                CHECK(value3D(x, y, z, seed) >= -1.000001f);
                CHECK(value3D(x, y, z, seed) <= 1.000001f);
                CHECK(perlin3D(x, y, z, seed) >= -1.000001f);
                CHECK(perlin3D(x, y, z, seed) <= 1.000001f);
            }
        }
    }

    const noise::FbmParams params{};
    CHECK(fbm2D(params, 12.34f, -5.67f, seed) >= -1.0f);
    CHECK(fbm2D(params, 12.34f, -5.67f, seed) <= 1.0f);
    CHECK(fbm3D(params, 1.0f, 2.0f, 3.0f, seed) >= -1.0f);
    CHECK(fbm3D(params, 1.0f, 2.0f, 3.0f, seed) <= 1.0f);
}

TEST_CASE("noise is continuous under small steps") {
    const std::uint64_t seed = 0xFEED'FACE'0DD0'BA11ull;
    const float h = 0.001f;
    for (int iy = 0; iy < 60; ++iy) {
        for (int ix = 0; ix < 60; ++ix) {
            const float x = static_cast<float>(ix) * 0.73f - 21.9f;
            const float y = static_cast<float>(iy) * 1.11f - 33.3f;
            const float z = y * 0.5f;
            CHECK(std::abs(value2D(x + h, y, seed) - value2D(x, y, seed)) <= 0.05f);
            CHECK(std::abs(perlin2D(x + h, y, seed) - perlin2D(x, y, seed)) <= 0.05f);
            CHECK(std::abs(simplex2D(x + h, y, seed) - simplex2D(x, y, seed)) <= 0.05f);
            CHECK(std::abs(perlin2D(x, y + h, seed) - perlin2D(x, y, seed)) <= 0.05f);
            if (ix % 7 == 0) {
                CHECK(std::abs(value3D(x, y, z + h, seed) - value3D(x, y, z, seed)) <= 0.05f);
                CHECK(std::abs(perlin3D(x, y, z + h, seed) - perlin3D(x, y, z, seed)) <= 0.05f);
            }
        }
    }
}

TEST_CASE("perlin is exactly zero at every integer lattice point") {
    const std::uint64_t seed = 0x0BAD'C0DE'DEAD'F00Dull;
    for (std::int64_t iy = -5; iy <= 5; ++iy) {
        for (std::int64_t ix = -5; ix <= 5; ++ix) {
            const float fx = static_cast<float>(ix);
            const float fy = static_cast<float>(iy);
            const std::uint32_t b2 = bitsOf(perlin2D(fx, fy, seed));
            CHECK((b2 == 0x00000000u || b2 == 0x80000000u));
            if ((ix + iy) % 2 == 0) {
                const std::uint32_t b3 = bitsOf(perlin3D(fx, fy, fy * 2.0f, seed));
                CHECK((b3 == 0x00000000u || b3 == 0x80000000u));
            }
        }
    }
}

TEST_CASE("identical seeds reproduce bit-identical fields; seeds diverge") {
    const std::uint64_t seed = 0xABCD'EF01'2345'6789ull;
    bool differsSomewhere = false;
    for (int i = 0; i < 10000; ++i) {
        const float x = static_cast<float>(i % 101) * 1.7f - 85.0f;
        const float y = static_cast<float>(i % 53) * 2.3f - 60.0f;
        const float a = perlin2D(x, y, seed);
        CHECK(bitsOf(a) == bitsOf(perlin2D(x, y, seed))); // replay identical
        const float b = perlin2D(x, y, seed ^ 0xFFFF'FFFF'FFFF'FFFFull);
        if (bitsOf(a) != bitsOf(b)) {
            differsSomewhere = true;
        }
    }
    CHECK(differsSomewhere);
}

TEST_CASE("fbm octave schedule is public contract and replicable") {
    // Documented schedule: octave i uses seed ^ (i * 0x9E3779B97F4A7C15).
    const std::uint64_t seed = 0x00C0'FFEE'0000'0042ull;
    const noise::FbmParams params{};

    auto replicateFbm2D = [&](float x, float y) {
        float sum = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float normalization = 0.0f;
        for (int octave = 0; octave < params.octaves; ++octave) {
            sum += amplitude *
                   perlin2D(x * frequency, y * frequency,
                            seed ^ (static_cast<std::uint64_t>(octave) * 0x9E37'79B9'7F4A'7C15ull));
            normalization += amplitude;
            amplitude *= params.gain;
            frequency *= params.lacunarity;
        }
        return sum / normalization;
    };

    for (int i = 0; i < 200; ++i) {
        const float x = static_cast<float>(i) * 0.913f - 90.0f;
        const float y = static_cast<float>(i % 17) * 3.11f - 25.0f;
        CHECK(bitsOf(fbm2D(params, x, y, seed)) == bitsOf(replicateFbm2D(x, y)));
    }

    // Default params: 4 octaves, gain 0.5, lacunarity 2 -> normalization 1.875.
    CHECK(params.octaves == 4);
    CHECK(params.gain == 0.5f);
    CHECK(params.lacunarity == 2.0f);
}