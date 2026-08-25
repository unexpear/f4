#include "core/noise/noise.h"

#include <cmath>

namespace f4::noise {

// Locked integer mixer (splitmix64 finalizer). Pure u64 ops: identical bits
// on every toolchain and GPU. These constants are part of the determinism
// contract and must never change.
inline namespace determinism {
constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kMixM1 = 0xBF58476D1CE4E5B9ull;
constexpr std::uint64_t kMixM2 = 0x94D049BB133111EBull;

[[nodiscard]] inline auto mix64(std::uint64_t z) -> std::uint64_t {
    z += kGoldenGamma;
    z = (z ^ (z >> 30)) * kMixM1;
    z = (z ^ (z >> 27)) * kMixM2;
    return z ^ (z >> 31);
}

[[nodiscard]] inline auto hash64(std::uint64_t lattice, std::uint64_t seed) -> std::uint64_t {
    return mix64(lattice ^ mix64(seed));
}
} // namespace determinism

// Exact [0,1) float from the top 24 hash bits: n * 2^-24, exactly
// representable, no rounding anywhere.
[[nodiscard]] inline auto unitFromHash(std::uint64_t h) -> float {
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Explicit coordinate-to-lattice conversion (see header contract).
[[nodiscard]] inline auto floorToI64(float v) -> std::int64_t {
    return static_cast<std::int64_t>(std::floor(v));
}

[[nodiscard]] inline auto fade(float t) -> float { return (t * t) * (3.0f - 2.0f * t); }

[[nodiscard]] inline auto lerp(float a, float b, float t) -> float { return a + ((b - a) * t); }

// Unit-length gradient tables. The irrational components are frozen float
// LITERALS - never runtime sqrt - so they are identical everywhere.
inline namespace gradients {
struct Grad2 {
    float x;
    float y;
};
constexpr Grad2 kGrad2[8] = {
    {1.0f, 0.0f},           {-1.0f, 0.0f},          {0.0f, 1.0f},           {0.0f, -1.0f},
    {0.70710678118654752f, 0.70710678118654752f},
    {-0.70710678118654752f, 0.70710678118654752f},
    {0.70710678118654752f, -0.70710678118654752f},
    {-0.70710678118654752f, -0.70710678118654752f},
};

struct Grad3 {
    float x;
    float y;
    float z;
};
constexpr Grad3 kGrad3[12] = {
    {1.0f, 1.0f, 0.0f},   {-1.0f, 1.0f, 0.0f},  {1.0f, -1.0f, 0.0f},  {-1.0f, -1.0f, 0.0f},
    {1.0f, 0.0f, 1.0f},   {-1.0f, 0.0f, 1.0f},  {1.0f, 0.0f, -1.0f},  {-1.0f, 0.0f, -1.0f},
    {0.0f, 1.0f, 1.0f},   {0.0f, -1.0f, 1.0f},  {0.0f, 1.0f, -1.0f},  {0.0f, -1.0f, -1.0f},
};

// Simplex skew constants for 2D: F2 = (sqrt(3)-1)/2, G2 = (3-sqrt(3))/6.
constexpr float kSimplexF2 = 0.36602540378443865f;
constexpr float kSimplexG2 = 0.21132486540518713f;
} // namespace gradients

auto value2D(float x, float y, std::uint64_t seed) -> float {
    const std::int64_t xi = floorToI64(x);
    const std::int64_t yi = floorToI64(y);
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);

    const float v00 =
        (unitFromHash(hash64(static_cast<std::uint64_t>(xi), seed ^ (static_cast<std::uint64_t>(yi) * 0x51'7C'C1'B7'27'22'0A'95ull))) * 2.0f) - 1.0f;
    const float v10 = (unitFromHash(hash64(static_cast<std::uint64_t>(xi + 1),
                                           seed ^ (static_cast<std::uint64_t>(yi) * 0x51'7C'C1'B7'27'22'0A'95ull))) *
                       2.0f) -
                      1.0f;
    const float v01 = (unitFromHash(hash64(static_cast<std::uint64_t>(xi),
                                           seed ^ (static_cast<std::uint64_t>(yi + 1) * 0x51'7C'C1'B7'27'22'0A'95ull))) *
                       2.0f) -
                      1.0f;
    const float v11 = (unitFromHash(hash64(static_cast<std::uint64_t>(xi + 1),
                                           seed ^ (static_cast<std::uint64_t>(yi + 1) * 0x51'7C'C1'B7'27'22'0A'95ull))) *
                       2.0f) -
                      1.0f;

    const float sx = fade(fx);
    const float sy = fade(fy);
    return lerp(lerp(v00, v10, sx), lerp(v01, v11, sx), sy);
}

auto value3D(float x, float y, float z, std::uint64_t seed) -> float {
    const std::int64_t xi = floorToI64(x);
    const std::int64_t yi = floorToI64(y);
    const std::int64_t zi = floorToI64(z);
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float fz = z - static_cast<float>(zi);

    const auto cornerHash = [&](int dx, int dy, int dz) -> float {
        const std::uint64_t h =
            hash64(static_cast<std::uint64_t>(xi + dx) ^ (static_cast<std::uint64_t>(yi + dy) << 21) ^
                       (static_cast<std::uint64_t>(zi + dz) << 42),
                   seed);
        return (unitFromHash(h) * 2.0f) - 1.0f;
    };

    const float sx = fade(fx);
    const float sy = fade(fy);
    const float sz = fade(fz);
    const float x00 = lerp(cornerHash(0, 0, 0), cornerHash(1, 0, 0), sx);
    const float x10 = lerp(cornerHash(0, 1, 0), cornerHash(1, 1, 0), sx);
    const float x01 = lerp(cornerHash(0, 0, 1), cornerHash(1, 0, 1), sx);
    const float x11 = lerp(cornerHash(0, 1, 1), cornerHash(1, 1, 1), sx);
    return lerp(lerp(x00, x10, sy), lerp(x01, x11, sy), sz);
}

auto perlin2D(float x, float y, std::uint64_t seed) -> float {
    const std::int64_t xi = floorToI64(x);
    const std::int64_t yi = floorToI64(y);
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);

    const auto gradientDot = [&](int dx, int dy) -> float {
        const std::uint64_t h =
            hash64(static_cast<std::uint64_t>(xi + dx) ^ (static_cast<std::uint64_t>(yi + dy) << 32),
                   seed);
        const Grad2& g = kGrad2[h & 7];
        const float dxs = fx - static_cast<float>(dx);
        const float dys = fy - static_cast<float>(dy);
        return (g.x * dxs) + (g.y * dys);
    };

    const float sx = fade(fx);
    const float sy = fade(fy);
    const float n00 = gradientDot(0, 0);
    const float n10 = gradientDot(1, 0);
    const float n01 = gradientDot(0, 1);
    const float n11 = gradientDot(1, 1);
    return lerp(lerp(n00, n10, sx), lerp(n01, n11, sx), sy);
}

auto perlin3D(float x, float y, float z, std::uint64_t seed) -> float {
    const std::int64_t xi = floorToI64(x);
    const std::int64_t yi = floorToI64(y);
    const std::int64_t zi = floorToI64(z);
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float fz = z - static_cast<float>(zi);

    const auto gradientDot = [&](int dx, int dy, int dz) -> float {
        const std::uint64_t h =
            hash64((static_cast<std::uint64_t>(xi + dx)) ^
                       ((static_cast<std::uint64_t>(yi + dy)) << 21) ^
                       ((static_cast<std::uint64_t>(zi + dz)) << 42),
                   seed);
        const Grad3& g = kGrad3[h % 12];
        const float dxs = fx - static_cast<float>(dx);
        const float dys = fy - static_cast<float>(dy);
        const float dzs = fz - static_cast<float>(dz);
        return (g.x * dxs) + (g.y * dys) + (g.z * dzs);
    };

    const float sx = fade(fx);
    const float sy = fade(fy);
    const float sz = fade(fz);

    const float n000 = gradientDot(0, 0, 0);
    const float n100 = gradientDot(1, 0, 0);
    const float n010 = gradientDot(0, 1, 0);
    const float n110 = gradientDot(1, 1, 0);
    const float n001 = gradientDot(0, 0, 1);
    const float n101 = gradientDot(1, 0, 1);
    const float n011 = gradientDot(0, 1, 1);
    const float n111 = gradientDot(1, 1, 1);

    const float x00 = lerp(n000, n100, sx);
    const float x10 = lerp(n010, n110, sx);
    const float x01 = lerp(n001, n101, sx);
    const float x11 = lerp(n011, n111, sx);
    return lerp(lerp(x00, x10, sy), lerp(x01, x11, sy), sz);
}

auto simplex2D(float x, float y, std::uint64_t seed) -> float {
    // Skew input space to determine which simplex (triangle) we are in.
    const float s = (x + y) * kSimplexF2;
    const std::int64_t i = floorToI64(x + s);
    const std::int64_t j = floorToI64(y + s);
    const float t = (static_cast<float>(i) + static_cast<float>(j)) * kSimplexG2;
    // Unskew the cell origin back to (x,y) space.
    const float x0 = x - (static_cast<float>(i) - t);
    const float y0 = y - (static_cast<float>(j) - t);

    // Offsets for the middle corner: lower triangle vs upper triangle.
    const std::int64_t i1 = x0 > y0 ? 1 : 0;
    const std::int64_t j1 = x0 > y0 ? 0 : 1;

    const float x1 = x0 - static_cast<float>(i1) + kSimplexG2;
    const float y1 = y0 - static_cast<float>(j1) + kSimplexG2;
    const float x2 = x0 - 1.0f + (2.0f * kSimplexG2);
    const float y2 = y0 - 1.0f + (2.0f * kSimplexG2);

    const auto cornerContribution = [&](std::int64_t ci, std::int64_t cj, float dx,
                                        float dy) -> float {
        const float tt = 0.5f - (dx * dx) - (dy * dy);
        if (tt < 0.0f) {
            return 0.0f;
        }
        const std::uint64_t h = hash64(static_cast<std::uint64_t>(ci) ^
                                           (static_cast<std::uint64_t>(cj) << 32),
                                       seed);
        const Grad2& g = kGrad2[h & 7];
        const float t4 = (tt * tt) * (tt * tt);
        return t4 * ((g.x * dx) + (g.y * dy));
    };

    // 70.0 scales [-1,1] after the (0.5 - d^2)^4 kernel; classic constant.
    return 70.0f * (cornerContribution(i, j, x0, y0) +
                    cornerContribution(i + i1, j + j1, x1, y1) +
                    cornerContribution(i + 1, j + 1, x2, y2));
}

auto fbm2D(const FbmParams& params, float x, float y, std::uint64_t seed) -> float {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float normalization = 0.0f;
    for (int octave = 0; octave < params.octaves; ++octave) {
        sum += amplitude * perlin2D(x * frequency, y * frequency,
                                    seed ^ (static_cast<std::uint64_t>(octave) * kGoldenGamma));
        normalization += amplitude;
        amplitude *= params.gain;
        frequency *= params.lacunarity;
    }
    return sum / normalization; // single documented IEEE division
}

auto fbm3D(const FbmParams& params, float x, float y, float z, std::uint64_t seed) -> float {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float normalization = 0.0f;
    for (int octave = 0; octave < params.octaves; ++octave) {
        sum += amplitude *
               perlin3D(x * frequency, y * frequency, z * frequency,
                        seed ^ (static_cast<std::uint64_t>(octave) * kGoldenGamma));
        normalization += amplitude;
        amplitude *= params.gain;
        frequency *= params.lacunarity;
    }
    return sum / normalization;
}

auto warp2D(float x, float y, std::uint64_t seed, float strength) -> math::Vec2 {
    const float dx = perlin2D(x, y, seed ^ 0xA153'9E37'79B9'7F4Aull);
    const float dy = perlin2D(x, y, seed ^ 0x8F35'BF58'476D'1CE4ull);
    return math::Vec2(dx * strength, dy * strength);
}

} // namespace f4::noise