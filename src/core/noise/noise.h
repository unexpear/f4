#pragma once

#include <cstdint>

#include "core/math/vec2.h"

namespace f4::noise {

// Deterministic, float-parity procedural noise (plan §11.6 / M13).
//
// Contract:
//  - EVERY function takes an explicit u64 seed. No hidden state, no platform
//    RNG anywhere.
//  - All permutation work is pure u64 integer mixing with LOCKED constants
//    (see noise.cpp): bit-identical results on every compiler, OS, and GPU.
//  - Hot paths use only +, -, *, and one final IEEE division where noted.
//    No sin/cos/exp/log; gradient tables come from integer hashes. The only
//    square roots in the library are frozen into float LITERALS (compile-time
//    constants, never runtime sqrt).
//  - Coordinate-to-lattice conversion is explicit: std::floor then cast to
//    i64; fractional part computed as v - floor(v). Inputs with
//    |coordinate| <= 2^21 keep exact fractional bits (documented domain).
//  - FBM octave schedule is PUBLIC API CONTRACT: octave i samples with
//    seed ^ (i * 0x9E3779B97F4A7C15ull) - callers may replicate any single
//    octave outside this library and get identical numbers.
//
// Ranges: value/perlin/simplex outputs lie in [-1, 1]; fbm normalizes to
// [-1, 1] by construction (amplitude sum).

struct FbmParams {
    int octaves = 4;
    float lacunarity = 2.0f;
    float gain = 0.5f;
};

[[nodiscard]] auto value2D(float x, float y, std::uint64_t seed) -> float;
[[nodiscard]] auto value3D(float x, float y, float z, std::uint64_t seed) -> float;

[[nodiscard]] auto perlin2D(float x, float y, std::uint64_t seed) -> float;
[[nodiscard]] auto perlin3D(float x, float y, float z, std::uint64_t seed) -> float;

[[nodiscard]] auto simplex2D(float x, float y, std::uint64_t seed) -> float;

[[nodiscard]] auto fbm2D(const FbmParams& params, float x, float y, std::uint64_t seed) -> float;
[[nodiscard]] auto fbm3D(const FbmParams& params, float x, float y, float z,
                         std::uint64_t seed) -> float;

// Returns a warped offset (dx, dy) sampled from paired perlin fields.
[[nodiscard]] auto warp2D(float x, float y, std::uint64_t seed, float strength) -> math::Vec2;

} // namespace f4::noise