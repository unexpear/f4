struct U64 {
    lo: u32,
    hi: u32,
};

struct Outputs {
    values: array<f32>,
};

struct Parameters {
    firstSample: u32,
};

@group(0) @binding(0) var<uniform> parameters: Parameters;
@group(0) @binding(3) var<storage, read_write> outputBuffer: Outputs;

const kGoldenGamma = U64(0x7f4a7c15u, 0x9e3779b9u);
const kMixM1 = U64(0x1ce4e5b9u, 0xbf58476du);
const kMixM2 = U64(0x133111ebu, 0x94d049bbu);
const kYMultiplier = U64(0x27220a95u, 0x517cc1b7u);

fn add64(a: U64, b: U64) -> U64 {
    let low = a.lo + b.lo;
    return U64(low, a.hi + b.hi + select(0u, 1u, low < a.lo));
}

fn xor64(a: U64, b: U64) -> U64 {
    return U64(a.lo ^ b.lo, a.hi ^ b.hi);
}

fn shiftRight64(value: U64, bits: u32) -> U64 {
    if (bits == 0u) { return value; }
    if (bits < 32u) {
        return U64((value.lo >> bits) | (value.hi << (32u - bits)), value.hi >> bits);
    }
    if (bits < 64u) { return U64(value.hi >> (bits - 32u), 0u); }
    return U64(0u, 0u);
}

fn shiftLeft64(value: U64, bits: u32) -> U64 {
    if (bits == 0u) { return value; }
    if (bits < 32u) {
        return U64(value.lo << bits,
                   (value.hi << bits) | (value.lo >> (32u - bits)));
    }
    if (bits < 64u) { return U64(0u, value.lo << (bits - 32u)); }
    return U64(0u, 0u);
}

fn multiply64(a: U64, b: U64) -> U64 {
    let a0 = a.lo & 0xffffu;
    let a1 = a.lo >> 16u;
    let b0 = b.lo & 0xffffu;
    let b1 = b.lo >> 16u;
    let p0 = a0 * b0;
    let p1 = a0 * b1;
    let p2 = a1 * b0;
    let p3 = a1 * b1;
    let middle = (p0 >> 16u) + (p1 & 0xffffu) + (p2 & 0xffffu);
    let low = (p0 & 0xffffu) | (middle << 16u);
    let high = p3 + (p1 >> 16u) + (p2 >> 16u) + (middle >> 16u) +
               (a.lo * b.hi) + (a.hi * b.lo);
    return U64(low, high);
}

fn fromInt(value: i32) -> U64 {
    return U64(bitcast<u32>(value), select(0u, 0xffffffffu, value < 0));
}

fn mix64(value: U64) -> U64 {
    var mixed = add64(value, kGoldenGamma);
    mixed = multiply64(xor64(mixed, shiftRight64(mixed, 30u)), kMixM1);
    mixed = multiply64(xor64(mixed, shiftRight64(mixed, 27u)), kMixM2);
    return xor64(mixed, shiftRight64(mixed, 31u));
}

fn hash64(lattice: U64, seed: U64) -> U64 {
    return mix64(xor64(lattice, mix64(seed)));
}

fn unitFromHash(hash: U64) -> f32 {
    return f32(hash.hi >> 8u) * (1.0 / 16777216.0);
}

fn fadeNoise(t: f32) -> f32 { return (t * t) * (3.0 - 2.0 * t); }
fn lerpNoise(a: f32, b: f32, t: f32) -> f32 { return a + ((b - a) * t); }

fn grad2(index: u32) -> vec2f {
    switch (index & 7u) {
    case 0u: { return vec2f(1.0, 0.0); }
    case 1u: { return vec2f(-1.0, 0.0); }
    case 2u: { return vec2f(0.0, 1.0); }
    case 3u: { return vec2f(0.0, -1.0); }
    case 4u: { return vec2f(0.70710678118654752, 0.70710678118654752); }
    case 5u: { return vec2f(-0.70710678118654752, 0.70710678118654752); }
    case 6u: { return vec2f(0.70710678118654752, -0.70710678118654752); }
    default: { return vec2f(-0.70710678118654752, -0.70710678118654752); }
    }
}

fn grad3(index: u32) -> vec3f {
    switch (index) {
    case 0u: { return vec3f(1.0, 1.0, 0.0); }
    case 1u: { return vec3f(-1.0, 1.0, 0.0); }
    case 2u: { return vec3f(1.0, -1.0, 0.0); }
    case 3u: { return vec3f(-1.0, -1.0, 0.0); }
    case 4u: { return vec3f(1.0, 0.0, 1.0); }
    case 5u: { return vec3f(-1.0, 0.0, 1.0); }
    case 6u: { return vec3f(1.0, 0.0, -1.0); }
    case 7u: { return vec3f(-1.0, 0.0, -1.0); }
    case 8u: { return vec3f(0.0, 1.0, 1.0); }
    case 9u: { return vec3f(0.0, -1.0, 1.0); }
    case 10u: { return vec3f(0.0, 1.0, -1.0); }
    default: { return vec3f(0.0, -1.0, -1.0); }
    }
}

fn mod12(value: U64) -> u32 {
    return (((value.hi % 12u) * 4u) + (value.lo % 12u)) % 12u;
}

fn value2D(x: f32, y: f32, seed: U64) -> f32 {
    let xi = i32(floor(x));
    let yi = i32(floor(y));
    let fx = x - f32(xi);
    let fy = y - f32(yi);
    let row0 = xor64(seed, multiply64(fromInt(yi), kYMultiplier));
    let row1 = xor64(seed, multiply64(fromInt(yi + 1), kYMultiplier));
    let v00 = unitFromHash(hash64(fromInt(xi), row0)) * 2.0 - 1.0;
    let v10 = unitFromHash(hash64(fromInt(xi + 1), row0)) * 2.0 - 1.0;
    let v01 = unitFromHash(hash64(fromInt(xi), row1)) * 2.0 - 1.0;
    let v11 = unitFromHash(hash64(fromInt(xi + 1), row1)) * 2.0 - 1.0;
    let sx = fadeNoise(fx);
    let sy = fadeNoise(fy);
    return lerpNoise(lerpNoise(v00, v10, sx), lerpNoise(v01, v11, sx), sy);
}

fn lattice3(x: i32, y: i32, z: i32) -> U64 {
    return xor64(xor64(fromInt(x), shiftLeft64(fromInt(y), 21u)),
                 shiftLeft64(fromInt(z), 42u));
}

fn valueCorner3(x: i32, y: i32, z: i32, seed: U64) -> f32 {
    return unitFromHash(hash64(lattice3(x, y, z), seed)) * 2.0 - 1.0;
}

fn value3D(x: f32, y: f32, z: f32, seed: U64) -> f32 {
    let xi = i32(floor(x));
    let yi = i32(floor(y));
    let zi = i32(floor(z));
    let sx = fadeNoise(x - f32(xi));
    let sy = fadeNoise(y - f32(yi));
    let sz = fadeNoise(z - f32(zi));
    let x00 = lerpNoise(valueCorner3(xi, yi, zi, seed),
                        valueCorner3(xi + 1, yi, zi, seed), sx);
    let x10 = lerpNoise(valueCorner3(xi, yi + 1, zi, seed),
                        valueCorner3(xi + 1, yi + 1, zi, seed), sx);
    let x01 = lerpNoise(valueCorner3(xi, yi, zi + 1, seed),
                        valueCorner3(xi + 1, yi, zi + 1, seed), sx);
    let x11 = lerpNoise(valueCorner3(xi, yi + 1, zi + 1, seed),
                        valueCorner3(xi + 1, yi + 1, zi + 1, seed), sx);
    return lerpNoise(lerpNoise(x00, x10, sy), lerpNoise(x01, x11, sy), sz);
}

fn perlinCorner2(x: i32, y: i32, dx: f32, dy: f32, seed: U64) -> f32 {
    let lattice = xor64(fromInt(x), shiftLeft64(fromInt(y), 32u));
    let gradient = grad2(hash64(lattice, seed).lo);
    return gradient.x * dx + gradient.y * dy;
}

fn perlin2D(x: f32, y: f32, seed: U64) -> f32 {
    let xi = i32(floor(x));
    let yi = i32(floor(y));
    let fx = x - f32(xi);
    let fy = y - f32(yi);
    let sx = fadeNoise(fx);
    let sy = fadeNoise(fy);
    let n00 = perlinCorner2(xi, yi, fx, fy, seed);
    let n10 = perlinCorner2(xi + 1, yi, fx - 1.0, fy, seed);
    let n01 = perlinCorner2(xi, yi + 1, fx, fy - 1.0, seed);
    let n11 = perlinCorner2(xi + 1, yi + 1, fx - 1.0, fy - 1.0, seed);
    return lerpNoise(lerpNoise(n00, n10, sx), lerpNoise(n01, n11, sx), sy);
}

fn perlinCorner3(x: i32, y: i32, z: i32, dx: f32, dy: f32, dz: f32,
                 seed: U64) -> f32 {
    let gradient = grad3(mod12(hash64(lattice3(x, y, z), seed)));
    return gradient.x * dx + gradient.y * dy + gradient.z * dz;
}

fn perlin3D(x: f32, y: f32, z: f32, seed: U64) -> f32 {
    let xi = i32(floor(x));
    let yi = i32(floor(y));
    let zi = i32(floor(z));
    let fx = x - f32(xi);
    let fy = y - f32(yi);
    let fz = z - f32(zi);
    let sx = fadeNoise(fx);
    let sy = fadeNoise(fy);
    let sz = fadeNoise(fz);
    let n000 = perlinCorner3(xi, yi, zi, fx, fy, fz, seed);
    let n100 = perlinCorner3(xi + 1, yi, zi, fx - 1.0, fy, fz, seed);
    let n010 = perlinCorner3(xi, yi + 1, zi, fx, fy - 1.0, fz, seed);
    let n110 = perlinCorner3(xi + 1, yi + 1, zi, fx - 1.0, fy - 1.0, fz, seed);
    let n001 = perlinCorner3(xi, yi, zi + 1, fx, fy, fz - 1.0, seed);
    let n101 = perlinCorner3(xi + 1, yi, zi + 1, fx - 1.0, fy, fz - 1.0, seed);
    let n011 = perlinCorner3(xi, yi + 1, zi + 1, fx, fy - 1.0, fz - 1.0, seed);
    let n111 = perlinCorner3(xi + 1, yi + 1, zi + 1, fx - 1.0, fy - 1.0,
                             fz - 1.0, seed);
    let x00 = lerpNoise(n000, n100, sx);
    let x10 = lerpNoise(n010, n110, sx);
    let x01 = lerpNoise(n001, n101, sx);
    let x11 = lerpNoise(n011, n111, sx);
    return lerpNoise(lerpNoise(x00, x10, sy), lerpNoise(x01, x11, sy), sz);
}

fn simplexCorner(i: i32, j: i32, dx: f32, dy: f32, seed: U64) -> f32 {
    let t = 0.5 - dx * dx - dy * dy;
    if (t < 0.0) { return 0.0; }
    let lattice = xor64(fromInt(i), shiftLeft64(fromInt(j), 32u));
    let gradient = grad2(hash64(lattice, seed).lo);
    let t4 = (t * t) * (t * t);
    return t4 * (gradient.x * dx + gradient.y * dy);
}

fn simplex2D(x: f32, y: f32, seed: U64) -> f32 {
    let f2 = 0.36602540378443865;
    let g2 = 0.21132486540518713;
    let s = (x + y) * f2;
    let i = i32(floor(x + s));
    let j = i32(floor(y + s));
    let t = (f32(i) + f32(j)) * g2;
    let x0 = x - (f32(i) - t);
    let y0 = y - (f32(j) - t);
    let i1 = select(0, 1, x0 > y0);
    let j1 = select(1, 0, x0 > y0);
    let x1 = x0 - f32(i1) + g2;
    let y1 = y0 - f32(j1) + g2;
    let x2 = x0 - 1.0 + 2.0 * g2;
    let y2 = y0 - 1.0 + 2.0 * g2;
    return 70.0 * (simplexCorner(i, j, x0, y0, seed) +
                   simplexCorner(i + i1, j + j1, x1, y1, seed) +
                   simplexCorner(i + 1, j + 1, x2, y2, seed));
}

fn octaveSeed(seed: U64, octave: i32) -> U64 {
    return xor64(seed, multiply64(U64(bitcast<u32>(octave), 0u), kGoldenGamma));
}

fn fbm2D(x: f32, y: f32, seed: U64) -> f32 {
    var sum = 0.0;
    var amplitude = 1.0;
    var frequency = 1.0;
    var normalization = 0.0;
    for (var octave = 0; octave < 4; octave += 1) {
        sum += amplitude * perlin2D(x * frequency, y * frequency,
                                    octaveSeed(seed, octave));
        normalization += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return sum / normalization;
}

fn fbm3D(x: f32, y: f32, z: f32, seed: U64) -> f32 {
    var sum = 0.0;
    var amplitude = 1.0;
    var frequency = 1.0;
    var normalization = 0.0;
    for (var octave = 0; octave < 4; octave += 1) {
        sum += amplitude * perlin3D(x * frequency, y * frequency, z * frequency,
                                    octaveSeed(seed, octave));
        normalization += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return sum / normalization;
}

fn sampleSeed(index: u32) -> U64 {
    if (index == 0u) { return U64(0u, 0u); }
    if (index == 1u) { return U64(1u, 0u); }
    if (index == 2u) { return U64(0x89abcdefu, 0x01234567u); }
    return U64(0x76543210u, 0xfedcba98u);
}

fn samplePoint(index: u32) -> vec3f {
    if (index == 0u) { return vec3f(0.125, -0.75, 1.5); }
    if (index == 1u) { return vec3f(-13.25, 7.875, -2.5); }
    if (index == 2u) { return vec3f(1024.125, -2048.75, 0.03125); }
    return vec3f(-0.001, 0.999, 42.25);
}

@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalId: vec3u) {
    let sampleIndex = globalId.x + parameters.firstSample;
    if (sampleIndex >= 4u) { return; }
    let point = samplePoint(sampleIndex);
    let seed = sampleSeed(sampleIndex);
    let base = sampleIndex * 9u;
    outputBuffer.values[base + 0u] = value2D(point.x, point.y, seed);
    outputBuffer.values[base + 1u] = value3D(point.x, point.y, point.z, seed);
    outputBuffer.values[base + 2u] = perlin2D(point.x, point.y, seed);
    outputBuffer.values[base + 3u] = perlin3D(point.x, point.y, point.z, seed);
    outputBuffer.values[base + 4u] = simplex2D(point.x, point.y, seed);
    outputBuffer.values[base + 5u] = fbm2D(point.x, point.y, seed);
    outputBuffer.values[base + 6u] = fbm3D(point.x, point.y, point.z, seed);
    outputBuffer.values[base + 7u] = perlin2D(
        point.x, point.y, xor64(seed, U64(0x79b97f4au, 0xa1539e37u))) * 2.75;
    outputBuffer.values[base + 8u] = perlin2D(
        point.x, point.y, xor64(seed, U64(0x476d1ce4u, 0x8f35bf58u))) * 2.75;
}
