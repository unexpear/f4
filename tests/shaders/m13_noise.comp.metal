#include <metal_stdlib>
using namespace metal;

constant uint2 kGoldenGamma(0x7f4a7c15u, 0x9e3779b9u);
constant uint2 kMixM1(0x1ce4e5b9u, 0xbf58476du);
constant uint2 kMixM2(0x133111ebu, 0x94d049bbu);
constant uint2 kYMultiplier(0x27220a95u, 0x517cc1b7u);

uint2 add64(uint2 a, uint2 b) {
    const uint low = a.x + b.x;
    return uint2(low, a.y + b.y + (low < a.x ? 1u : 0u));
}

uint2 xor64(uint2 a, uint2 b) { return a ^ b; }

uint2 shiftRight64(uint2 value, uint bits) {
    return uint2((value.x >> bits) | (value.y << (32u - bits)), value.y >> bits);
}

uint2 shiftLeft64(uint2 value, uint bits) {
    if (bits == 32u) return uint2(0u, value.x);
    if (bits > 32u) return uint2(0u, value.x << (bits - 32u));
    return uint2(value.x << bits, (value.y << bits) | (value.x >> (32u - bits)));
}

uint2 multiply64(uint2 a, uint2 b) {
    const uint a0 = a.x & 0xffffu;
    const uint a1 = a.x >> 16u;
    const uint b0 = b.x & 0xffffu;
    const uint b1 = b.x >> 16u;
    const uint p0 = a0 * b0;
    const uint p1 = a0 * b1;
    const uint p2 = a1 * b0;
    const uint p3 = a1 * b1;
    const uint middle = (p0 >> 16u) + (p1 & 0xffffu) + (p2 & 0xffffu);
    const uint low = (p0 & 0xffffu) | (middle << 16u);
    uint high = p3 + (p1 >> 16u) + (p2 >> 16u) + (middle >> 16u);
    high += (a.x * b.y) + (a.y * b.x);
    return uint2(low, high);
}

uint2 fromInt(int value) {
    return uint2(uint(value), value < 0 ? 0xffffffffu : 0u);
}

uint2 mix64(uint2 value) {
    value = add64(value, kGoldenGamma);
    value = multiply64(xor64(value, shiftRight64(value, 30u)), kMixM1);
    value = multiply64(xor64(value, shiftRight64(value, 27u)), kMixM2);
    return xor64(value, shiftRight64(value, 31u));
}

uint2 hash64(uint2 lattice, uint2 seed) {
    return mix64(xor64(lattice, mix64(seed)));
}

float unitFromHash(uint2 hash) {
    return float(hash.y >> 8u) * (1.0f / 16777216.0f);
}

float fadeNoise(float value) { return (value * value) * (3.0f - 2.0f * value); }
float lerpNoise(float a, float b, float value) { return a + ((b - a) * value); }

float2 grad2(uint index) {
    switch (index & 7u) {
    case 0u: return float2(1.0f, 0.0f);
    case 1u: return float2(-1.0f, 0.0f);
    case 2u: return float2(0.0f, 1.0f);
    case 3u: return float2(0.0f, -1.0f);
    case 4u: return float2(0.70710678118654752f, 0.70710678118654752f);
    case 5u: return float2(-0.70710678118654752f, 0.70710678118654752f);
    case 6u: return float2(0.70710678118654752f, -0.70710678118654752f);
    default: return float2(-0.70710678118654752f, -0.70710678118654752f);
    }
}

float3 grad3(uint index) {
    switch (index) {
    case 0u: return float3(1.0f, 1.0f, 0.0f);
    case 1u: return float3(-1.0f, 1.0f, 0.0f);
    case 2u: return float3(1.0f, -1.0f, 0.0f);
    case 3u: return float3(-1.0f, -1.0f, 0.0f);
    case 4u: return float3(1.0f, 0.0f, 1.0f);
    case 5u: return float3(-1.0f, 0.0f, 1.0f);
    case 6u: return float3(1.0f, 0.0f, -1.0f);
    case 7u: return float3(-1.0f, 0.0f, -1.0f);
    case 8u: return float3(0.0f, 1.0f, 1.0f);
    case 9u: return float3(0.0f, -1.0f, 1.0f);
    case 10u: return float3(0.0f, 1.0f, -1.0f);
    default: return float3(0.0f, -1.0f, -1.0f);
    }
}

uint mod12(uint2 value) {
    return (((value.y % 12u) * 4u) + (value.x % 12u)) % 12u;
}

float value2D(float x, float y, uint2 seed) {
    const int xi = int(floor(x));
    const int yi = int(floor(y));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    const uint2 row0 = xor64(seed, multiply64(fromInt(yi), kYMultiplier));
    const uint2 row1 = xor64(seed, multiply64(fromInt(yi + 1), kYMultiplier));
    const float v00 = unitFromHash(hash64(fromInt(xi), row0)) * 2.0f - 1.0f;
    const float v10 = unitFromHash(hash64(fromInt(xi + 1), row0)) * 2.0f - 1.0f;
    const float v01 = unitFromHash(hash64(fromInt(xi), row1)) * 2.0f - 1.0f;
    const float v11 = unitFromHash(hash64(fromInt(xi + 1), row1)) * 2.0f - 1.0f;
    const float sx = fadeNoise(fx);
    const float sy = fadeNoise(fy);
    return lerpNoise(lerpNoise(v00, v10, sx), lerpNoise(v01, v11, sx), sy);
}

uint2 lattice3(int x, int y, int z) {
    return xor64(xor64(fromInt(x), shiftLeft64(fromInt(y), 21u)),
                 shiftLeft64(fromInt(z), 42u));
}

float valueCorner3(int x, int y, int z, uint2 seed) {
    return unitFromHash(hash64(lattice3(x, y, z), seed)) * 2.0f - 1.0f;
}

float value3D(float x, float y, float z, uint2 seed) {
    const int xi = int(floor(x));
    const int yi = int(floor(y));
    const int zi = int(floor(z));
    const float sx = fadeNoise(x - float(xi));
    const float sy = fadeNoise(y - float(yi));
    const float sz = fadeNoise(z - float(zi));
    const float x00 = lerpNoise(valueCorner3(xi, yi, zi, seed),
                                valueCorner3(xi + 1, yi, zi, seed), sx);
    const float x10 = lerpNoise(valueCorner3(xi, yi + 1, zi, seed),
                                valueCorner3(xi + 1, yi + 1, zi, seed), sx);
    const float x01 = lerpNoise(valueCorner3(xi, yi, zi + 1, seed),
                                valueCorner3(xi + 1, yi, zi + 1, seed), sx);
    const float x11 = lerpNoise(valueCorner3(xi, yi + 1, zi + 1, seed),
                                valueCorner3(xi + 1, yi + 1, zi + 1, seed), sx);
    return lerpNoise(lerpNoise(x00, x10, sy), lerpNoise(x01, x11, sy), sz);
}

float perlinCorner2(int x, int y, float dx, float dy, uint2 seed) {
    const uint2 lattice = xor64(fromInt(x), shiftLeft64(fromInt(y), 32u));
    const float2 gradient = grad2(hash64(lattice, seed).x);
    return gradient.x * dx + gradient.y * dy;
}

float perlin2D(float x, float y, uint2 seed) {
    const int xi = int(floor(x));
    const int yi = int(floor(y));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    const float sx = fadeNoise(fx);
    const float sy = fadeNoise(fy);
    const float n00 = perlinCorner2(xi, yi, fx, fy, seed);
    const float n10 = perlinCorner2(xi + 1, yi, fx - 1.0f, fy, seed);
    const float n01 = perlinCorner2(xi, yi + 1, fx, fy - 1.0f, seed);
    const float n11 = perlinCorner2(xi + 1, yi + 1, fx - 1.0f, fy - 1.0f, seed);
    return lerpNoise(lerpNoise(n00, n10, sx), lerpNoise(n01, n11, sx), sy);
}

float perlinCorner3(int x, int y, int z, float dx, float dy, float dz,
                    uint2 seed) {
    const float3 gradient = grad3(mod12(hash64(lattice3(x, y, z), seed)));
    return gradient.x * dx + gradient.y * dy + gradient.z * dz;
}

float perlin3D(float x, float y, float z, uint2 seed) {
    const int xi = int(floor(x));
    const int yi = int(floor(y));
    const int zi = int(floor(z));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    const float fz = z - float(zi);
    const float sx = fadeNoise(fx);
    const float sy = fadeNoise(fy);
    const float sz = fadeNoise(fz);
    const float n000 = perlinCorner3(xi, yi, zi, fx, fy, fz, seed);
    const float n100 = perlinCorner3(xi + 1, yi, zi, fx - 1.0f, fy, fz, seed);
    const float n010 = perlinCorner3(xi, yi + 1, zi, fx, fy - 1.0f, fz, seed);
    const float n110 = perlinCorner3(xi + 1, yi + 1, zi, fx - 1.0f, fy - 1.0f, fz, seed);
    const float n001 = perlinCorner3(xi, yi, zi + 1, fx, fy, fz - 1.0f, seed);
    const float n101 = perlinCorner3(xi + 1, yi, zi + 1, fx - 1.0f, fy, fz - 1.0f, seed);
    const float n011 = perlinCorner3(xi, yi + 1, zi + 1, fx, fy - 1.0f, fz - 1.0f, seed);
    const float n111 = perlinCorner3(xi + 1, yi + 1, zi + 1,
                                     fx - 1.0f, fy - 1.0f, fz - 1.0f, seed);
    const float x00 = lerpNoise(n000, n100, sx);
    const float x10 = lerpNoise(n010, n110, sx);
    const float x01 = lerpNoise(n001, n101, sx);
    const float x11 = lerpNoise(n011, n111, sx);
    return lerpNoise(lerpNoise(x00, x10, sy), lerpNoise(x01, x11, sy), sz);
}

float simplexCorner(int i, int j, float dx, float dy, uint2 seed) {
    const float t = 0.5f - dx * dx - dy * dy;
    if (t < 0.0f) return 0.0f;
    const uint2 lattice = xor64(fromInt(i), shiftLeft64(fromInt(j), 32u));
    const float2 gradient = grad2(hash64(lattice, seed).x);
    const float t4 = (t * t) * (t * t);
    return t4 * (gradient.x * dx + gradient.y * dy);
}

float simplex2D(float x, float y, uint2 seed) {
    const float f2 = 0.36602540378443865f;
    const float g2 = 0.21132486540518713f;
    const float s = (x + y) * f2;
    const int i = int(floor(x + s));
    const int j = int(floor(y + s));
    const float t = (float(i) + float(j)) * g2;
    const float x0 = x - (float(i) - t);
    const float y0 = y - (float(j) - t);
    const int i1 = x0 > y0 ? 1 : 0;
    const int j1 = x0 > y0 ? 0 : 1;
    const float x1 = x0 - float(i1) + g2;
    const float y1 = y0 - float(j1) + g2;
    const float x2 = x0 - 1.0f + 2.0f * g2;
    const float y2 = y0 - 1.0f + 2.0f * g2;
    return 70.0f * (simplexCorner(i, j, x0, y0, seed) +
                    simplexCorner(i + i1, j + j1, x1, y1, seed) +
                    simplexCorner(i + 1, j + 1, x2, y2, seed));
}

uint2 octaveSeed(uint2 seed, int octave) {
    return xor64(seed, multiply64(uint2(uint(octave), 0u), kGoldenGamma));
}

float fbm2D(float x, float y, uint2 seed) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float normalization = 0.0f;
    for (int octave = 0; octave < 4; ++octave) {
        sum += amplitude * perlin2D(x * frequency, y * frequency,
                                    octaveSeed(seed, octave));
        normalization += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return sum / normalization;
}

float fbm3D(float x, float y, float z, uint2 seed) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float normalization = 0.0f;
    for (int octave = 0; octave < 4; ++octave) {
        sum += amplitude * perlin3D(x * frequency, y * frequency, z * frequency,
                                    octaveSeed(seed, octave));
        normalization += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return sum / normalization;
}

uint2 sampleSeed(uint index) {
    if (index == 0u) return uint2(0u, 0u);
    if (index == 1u) return uint2(1u, 0u);
    if (index == 2u) return uint2(0x89abcdefu, 0x01234567u);
    return uint2(0x76543210u, 0xfedcba98u);
}

float3 samplePoint(uint index) {
    if (index == 0u) return float3(0.125f, -0.75f, 1.5f);
    if (index == 1u) return float3(-13.25f, 7.875f, -2.5f);
    if (index == 2u) return float3(1024.125f, -2048.75f, 0.03125f);
    return float3(-0.001f, 0.999f, 42.25f);
}

void storeValue(device float* outputBuffer, uint index, float value) {
    outputBuffer[index] = value;
}

kernel void m13_noise(device float* outputBuffer [[buffer(16)]],
                      uint3 dispatchThreadId [[thread_position_in_grid]]) {
    const uint sampleIndex = dispatchThreadId.x;
    if (sampleIndex >= 4u) return;
    const float3 position = samplePoint(sampleIndex);
    const uint2 seed = sampleSeed(sampleIndex);
    const uint base = sampleIndex * 9u;
    storeValue(outputBuffer, base + 0u, value2D(position.x, position.y, seed));
    storeValue(outputBuffer, base + 1u, value3D(position.x, position.y, position.z, seed));
    storeValue(outputBuffer, base + 2u, perlin2D(position.x, position.y, seed));
    storeValue(outputBuffer, base + 3u, perlin3D(position.x, position.y, position.z, seed));
    storeValue(outputBuffer, base + 4u, simplex2D(position.x, position.y, seed));
    storeValue(outputBuffer, base + 5u, fbm2D(position.x, position.y, seed));
    storeValue(outputBuffer, base + 6u, fbm3D(position.x, position.y, position.z, seed));
    storeValue(outputBuffer, base + 7u,
               perlin2D(position.x, position.y,
                        xor64(seed, uint2(0x79b97f4au, 0xa1539e37u))) * 2.75f);
    storeValue(outputBuffer, base + 8u,
               perlin2D(position.x, position.y,
                        xor64(seed, uint2(0x476d1ce4u, 0x8f35bf58u))) * 2.75f);
}
