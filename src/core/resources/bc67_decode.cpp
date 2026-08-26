// BC6H and BC7 block decoders.
//
// Algorithms follow the public Microsoft BC6H/BC7 format specification
// (bit-accurate decode contract). Partition/fixup/weight tables and the
// BC6H per-mode bit descriptors are transcribed from the MIT-licensed
// DirectXTex reference decoder (microsoft/DirectXTex, BC6HBC7.cpp); see
// bc67_tables.inc (generated) for the verbatim table data.

#include "core/resources/bc_decode.h"

#include <cstring>

namespace f4::resources {

namespace {

// ---------------------------------------------------------------------------
// Supporting types required by the generated tables.
// ---------------------------------------------------------------------------

struct Prec {
    std::uint8_t r, g, b;
};

enum class Bc6hField : std::uint8_t {
    NA, M, D, RW, RX, RY, RZ, GW, GX, GY, GZ, BW, BX, BY, BZ,
};

struct Bc6hDescEntry {
    Bc6hField field;
    std::uint8_t bit;
};

struct Bc6hModeInfo {
    std::uint8_t mode;
    std::uint8_t partitions; // 0 = one region, 1 = two regions
    bool transformed;
    std::uint8_t indexPrec;
    Prec precA; // base endpoint (endpt[0].A) precision per channel
    Prec precB; // delta endpoint precision per channel
};

#include "bc67_tables.inc"

// ---------------------------------------------------------------------------
// Half-float helpers (IEEE 754 binary16).
// ---------------------------------------------------------------------------

constexpr std::uint16_t kF16SignMask = 0x8000;
constexpr std::uint16_t kF16EMMask = 0x7FFF;
constexpr std::uint16_t kF16Max = 0x7BFF; // max finite half magnitude bits

auto halfBitsToFloat(std::uint16_t h) -> float {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) >> 15u) & 1u;
    const std::uint32_t exponent = (static_cast<std::uint32_t>(h) >> 10u) & 0x1Fu;
    const std::uint32_t mantissa = static_cast<std::uint32_t>(h) & 0x3FFu;

    std::uint32_t f;
    if (exponent == 0) {
        if (mantissa == 0) {
            f = sign << 31u;
        } else {
            std::uint32_t m = mantissa;
            std::uint32_t e = 1;
            while ((m & 0x400u) == 0) { m <<= 1u; ++e; }
            f = (sign << 31u) | ((127u - 15u + 1u - e) << 23u) | ((m & 0x3FFu) << 13u);
        }
    } else if (exponent == 31) {
        f = (sign << 31u) | 0x7F800000u | (mantissa << 13u);
    } else {
        f = (sign << 31u) | ((exponent + 127u - 15u) << 23u) | (mantissa << 13u);
    }
    float result;
    std::memcpy(&result, &f, sizeof(result));
    return result;
}

#define F4_SIGN_EXTEND(x, nb) \
    ((((x) & (1 << ((nb) - 1))) ? ((~0) ^ ((1 << (nb)) - 1)) : 0) | (x))

// ---------------------------------------------------------------------------
// Bit reader over a 16-byte block (LSB-first within each byte).
// ---------------------------------------------------------------------------

struct BitReader {
    const std::uint8_t* data;
    std::size_t pos = 0;

    auto bit() -> std::uint8_t {
        const std::size_t index = pos >> 3u;
        const auto ret = static_cast<std::uint8_t>(
            (data[index] >> (pos & 7u)) & 0x01);
        ++pos;
        return ret;
    }

    auto bits(std::size_t count) -> std::uint8_t {
        if (count == 0) return 0;
        const std::size_t index = pos >> 3u;
        const std::size_t base = pos & 7u;
        std::uint8_t ret;
        if (base + count > 8) {
            const std::size_t firstBits = 8 - base;
            const std::size_t nextBits = count - firstBits;
            ret = static_cast<std::uint8_t>(
                (static_cast<unsigned>(data[index]) >> base) |
                ((static_cast<unsigned>(data[index + 1]) & ((1u << nextBits) - 1u)) << firstBits));
        } else {
            ret = static_cast<std::uint8_t>(
                (data[index] >> base) & ((1u << count) - 1u));
        }
        pos += count;
        return ret;
    }
};

auto isFixUpOffset(std::size_t partitions, std::size_t shape, std::size_t offset) -> bool {
    for (std::size_t p = 0; p <= partitions; ++p) {
        if (offset == kFixUp[partitions][shape][p]) return true;
    }
    return false;
}

auto weightTable(std::size_t prec) -> const int* {
    switch (prec) {
    case 2: return kWeight2;
    case 3: return kWeight3;
    case 4: return kWeight4;
    default: return kWeight2;
    }
}

// BC7 unorm endpoint expansion to 8 bits with MSB replication.
auto bc7Unquantize(std::uint8_t comp, std::size_t prec) -> std::uint8_t {
    const std::uint8_t shifted = static_cast<std::uint8_t>(
        static_cast<unsigned>(comp) << (8u - prec));
    return static_cast<std::uint8_t>(shifted | (shifted >> prec));
}

auto bc7Interpolate(std::uint8_t e0, std::uint8_t e1, std::size_t index,
                    std::size_t prec) -> std::uint8_t {
    const int* weights = weightTable(prec);
    const int w = weights[index];
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(e0) * static_cast<std::uint32_t>(64 - w) +
         static_cast<std::uint32_t>(e1) * static_cast<std::uint32_t>(w) + 32) >> 6);
}

// ---------------------------------------------------------------------------
// BC6H unquantization (per Microsoft spec).
// ---------------------------------------------------------------------------

auto bc6hUnquantize(int comp, std::uint8_t bitsPerComp, bool isSigned) -> int {
    int unq = 0;
    if (isSigned) {
        if (bitsPerComp >= 16) {
            unq = comp;
        } else {
            int s = 0;
            if (comp < 0) { s = 1; comp = -comp; }
            if (comp == 0) unq = 0;
            else if (comp >= ((1 << (bitsPerComp - 1)) - 1)) unq = 0x7FFF;
            else unq = ((comp << 15) + 0x4000) >> (bitsPerComp - 1);
            if (s) unq = -unq;
        }
    } else {
        if (bitsPerComp >= 15) unq = comp;
        else if (comp == 0) unq = 0;
        else if (comp == ((1 << bitsPerComp) - 1)) unq = 0xFFFF;
        else unq = ((comp << 16) + 0x8000) >> bitsPerComp;
    }
    return unq;
}

auto bc6hFinishUnquantize(int comp, bool isSigned) -> int {
    if (isSigned) {
        return (comp < 0) ? -(((-comp) * 31) >> 5) : (comp * 31) >> 5;
    }
    return (comp * 31) >> 6;
}

// Convert a signed/unsigned magnitude-encoded int to half bits, then float.
auto bc6hIntToFloat(int input, bool isSigned) -> float {
    std::uint16_t out;
    if (isSigned) {
        int s = 0;
        if (input < 0) { s = kF16SignMask; input = -input; }
        out = static_cast<std::uint16_t>(s | input);
    } else {
        out = static_cast<std::uint16_t>(input);
    }
    return halfBitsToFloat(out);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// BC7
// ---------------------------------------------------------------------------

void decodeBC7Block(const std::byte* block, std::uint8_t* rgba) {
    BitReader br{reinterpret_cast<const std::uint8_t*>(block)};

    // Mode: number of leading zero bits before the first 1.
    std::size_t first = 0;
    while (first < 128) {
        if ((static_cast<unsigned>(block[first >> 3]) >> (first & 7)) & 1u) break;
        ++first;
    }
    const std::uint8_t mode = static_cast<std::uint8_t>(first);

    if (mode >= 8) {
        // Reserved mode 8+: transparent black per spec.
        std::memset(rgba, 0, 16 * 4);
        return;
    }

    struct Bc7Mode {
        std::uint8_t partitions, partitionBits, pbits, rotationBits, indexModeBits;
        std::uint8_t indexPrec, indexPrec2;
        std::uint8_t prec[4], precWithP[4];
    };
    static constexpr Bc7Mode kModes[8] = {
        {2, 4, 6, 0, 0, 3, 0, {4,4,4,0}, {5,5,5,0}},
        {1, 6, 2, 0, 0, 3, 0, {6,6,6,0}, {7,7,7,0}},
        {2, 6, 0, 0, 0, 2, 0, {5,5,5,0}, {5,5,5,0}},
        {1, 6, 4, 0, 0, 2, 0, {7,7,7,0}, {8,8,8,0}},
        {0, 0, 0, 2, 1, 2, 3, {5,5,5,6}, {5,5,5,6}},
        {0, 0, 0, 2, 0, 2, 2, {7,7,7,8}, {7,7,7,8}},
        {0, 0, 2, 0, 0, 4, 0, {7,7,7,7}, {8,8,8,8}},
        {1, 6, 4, 0, 0, 2, 0, {5,5,5,5}, {6,6,6,6}},
    };
    const Bc7Mode& info = kModes[mode];

    br.pos = static_cast<std::size_t>(mode) + 1;
    const std::uint8_t shape = br.bits(info.partitionBits);
    const std::uint8_t rotation = br.bits(info.rotationBits);
    const std::uint8_t indexMode = br.bits(info.indexModeBits);

    const std::uint8_t numEndpts = static_cast<std::uint8_t>((info.partitions + 1u) << 1);

    std::uint8_t c[8][4]; // endpoints [endpoint][rgba]
    // Red, green, blue, alpha channels in order.
    for (int ch = 0; ch < 3; ++ch) {
        for (std::uint8_t i = 0; i < numEndpts; ++i) {
            c[i][ch] = br.bits(info.prec[ch]);
        }
    }
    for (std::uint8_t i = 0; i < numEndpts; ++i) {
        c[i][3] = info.prec[3] ? br.bits(info.prec[3]) : 255u;
    }

    // P-bits.
    std::uint8_t P[6] = {};
    for (std::uint8_t i = 0; i < info.pbits; ++i) {
        P[i] = br.bit();
    }
    if (info.pbits) {
        for (std::uint8_t i = 0; i < numEndpts; ++i) {
            const std::size_t pi = static_cast<std::size_t>(i) * info.pbits / numEndpts;
            for (std::uint8_t ch = 0; ch < 4; ++ch) {
                if (info.prec[ch] != info.precWithP[ch]) {
                    c[i][ch] = static_cast<std::uint8_t>((c[i][ch] << 1) | P[pi]);
                }
            }
        }
    }

    // Unquantize endpoints to 8 bits. Channels with zero precision are
    // implicit 255 (alpha in RGB-only modes).
    for (std::uint8_t i = 0; i < numEndpts; ++i) {
        for (std::uint8_t ch = 0; ch < 4; ++ch) {
            c[i][ch] = info.precWithP[ch] > 0
                ? bc7Unquantize(c[i][ch], info.precWithP[ch])
                : static_cast<std::uint8_t>(255u);
        }
    }

    // Color indices.
    std::uint8_t w1[16], w2[16];
    for (int i = 0; i < 16; ++i) {
        const std::size_t numBits = isFixUpOffset(info.partitions, shape, i)
            ? info.indexPrec - 1 : info.indexPrec;
        w1[i] = br.bits(numBits);
    }
    // Alpha indices (modes 4/5).
    if (info.indexPrec2) {
        for (int i = 0; i < 16; ++i) {
            const std::size_t numBits = i ? info.indexPrec2 : info.indexPrec2 - 1;
            w2[i] = br.bits(numBits);
        }
    }

    for (int i = 0; i < 16; ++i) {
        const std::uint8_t region = kPartitionTable[info.partitions][shape][i];
        const std::uint8_t* e0 = c[region << 1];
        const std::uint8_t* e1 = c[(region << 1) + 1];

        std::uint8_t out[4];
        if (info.indexPrec2 == 0) {
            for (int ch = 0; ch < 4; ++ch) {
                out[ch] = bc7Interpolate(e0[ch], e1[ch], w1[i], info.indexPrec);
            }
        } else if (indexMode == 0) {
            for (int ch = 0; ch < 3; ++ch)
                out[ch] = bc7Interpolate(e0[ch], e1[ch], w1[i], info.indexPrec);
            out[3] = bc7Interpolate(e0[3], e1[3], w2[i], info.indexPrec2);
        } else {
            for (int ch = 0; ch < 3; ++ch)
                out[ch] = bc7Interpolate(e0[ch], e1[ch], w2[i], info.indexPrec2);
            out[3] = bc7Interpolate(e0[3], e1[3], w1[i], info.indexPrec);
        }

        // Channel rotation.
        switch (rotation) {
        case 1: { auto t = out[0]; out[0] = out[3]; out[3] = t; break; }
        case 2: { auto t = out[1]; out[1] = out[3]; out[3] = t; break; }
        case 3: { auto t = out[2]; out[2] = out[3]; out[3] = t; break; }
        default: break;
        }

        std::memcpy(rgba + i * 4, out, 4);
    }
}

// ---------------------------------------------------------------------------
// BC6H
// ---------------------------------------------------------------------------

void decodeBC6HBlock(const std::byte* block, float* rgb, bool isSigned) {
    BitReader br{reinterpret_cast<const std::uint8_t*>(block)};

    // Mode: low 2 bits; if not 00/01, extend with 3 more bits.
    std::size_t pos = 0;
    std::uint8_t uMode = br.bits(2);
    if (uMode != 0x00 && uMode != 0x01) {
        uMode = static_cast<std::uint8_t>((br.bits(3) << 2) | uMode);
    }
    (void)pos;

    const int infoIdx = (uMode < 32) ? kBc6hModeToInfo[uMode] : -1;
    if (infoIdx < 0) {
        // Invalid/reserved mode: opaque black per spec.
        for (int i = 0; i < 16; ++i) {
            rgb[i * 3 + 0] = 0.0f;
            rgb[i * 3 + 1] = 0.0f;
            rgb[i * 3 + 2] = 0.0f;
        }
        return;
    }

    const Bc6hModeInfo& info = kBc6hInfo[infoIdx];
    const Bc6hDescEntry* desc = kBc6hDesc[infoIdx];

    // Endpoints: [region][2 endpoints][rgb]. Region 0 = A(0)/B(0), region 1 = A(1)/B(1).
    int endpts[2][2][3] = {};
    std::uint32_t shape = 0;

    // Read header via descriptor table.
    const std::size_t headerBits = info.partitions > 0 ? 82u : 65u;
    bool valid = true;
    while (br.pos < headerBits) {
        const std::size_t curBit = br.pos;
        const std::uint8_t b = br.bit();
        if (b) {
            const Bc6hDescEntry& e = desc[curBit];
            const int bitVal = 1 << e.bit;
            switch (e.field) {
            case Bc6hField::D:  shape |= static_cast<std::uint32_t>(bitVal); break;
            case Bc6hField::RW: endpts[0][0][0] |= bitVal; break;
            case Bc6hField::RX: endpts[0][1][0] |= bitVal; break;
            case Bc6hField::RY: endpts[1][0][0] |= bitVal; break;
            case Bc6hField::RZ: endpts[1][1][0] |= bitVal; break;
            case Bc6hField::GW: endpts[0][0][1] |= bitVal; break;
            case Bc6hField::GX: endpts[0][1][1] |= bitVal; break;
            case Bc6hField::GY: endpts[1][0][1] |= bitVal; break;
            case Bc6hField::GZ: endpts[1][1][1] |= bitVal; break;
            case Bc6hField::BW: endpts[0][0][2] |= bitVal; break;
            case Bc6hField::BX: endpts[0][1][2] |= bitVal; break;
            case Bc6hField::BY: endpts[1][0][2] |= bitVal; break;
            case Bc6hField::BZ: endpts[1][1][2] |= bitVal; break;
            default:
                valid = false;
                break;
            }
        }
        if (!valid) break;
    }

    if (!valid) {
        for (int i = 0; i < 16; ++i) {
            rgb[i * 3 + 0] = 0.0f;
            rgb[i * 3 + 1] = 0.0f;
            rgb[i * 3 + 2] = 0.0f;
        }
        return;
    }

    // Sign extension.
    const Prec& precA = info.precA;
    const Prec& precB = info.precB;
    const std::uint8_t precAChan[3] = {precA.r, precA.g, precA.b};
    const std::uint8_t precBChan[3] = {precB.r, precB.g, precB.b};

    auto signExt = [&](int region, int ep, int ch, int nb) {
        endpts[region][ep][ch] = F4_SIGN_EXTEND(endpts[region][ep][ch], nb);
    };
    if (isSigned) {
        for (int ch = 0; ch < 3; ++ch)
            signExt(0, 0, ch, precAChan[ch]);
    }
    if (isSigned || info.transformed) {
        for (std::uint8_t p = 0; p <= info.partitions; ++p) {
            if (p != 0) {
                for (int ch = 0; ch < 3; ++ch)
                    signExt(p, 0, ch, precBChan[ch]);
            }
            for (int ch = 0; ch < 3; ++ch)
                signExt(p, 1, ch, precBChan[ch]);
        }
    }

    // Inverse transform.
    if (info.transformed) {
        for (int ch = 0; ch < 3; ++ch) {
            const int wrap = (1 << precAChan[ch]) - 1;
            endpts[0][1][ch] = (endpts[0][1][ch] + endpts[0][0][ch]) & wrap;
            endpts[1][0][ch] = (endpts[1][0][ch] + endpts[0][0][ch]) & wrap;
            endpts[1][1][ch] = (endpts[1][1][ch] + endpts[0][0][ch]) & wrap;
        }
        if (isSigned) {
            for (int ch = 0; ch < 3; ++ch) {
                signExt(0, 1, ch, precAChan[ch]);
                signExt(1, 0, ch, precAChan[ch]);
                signExt(1, 1, ch, precAChan[ch]);
            }
        }
    }

    // Read indices and interpolate.
    const int* weights = info.partitions > 0 ? kWeight3 : kWeight4;
    const std::size_t maxIndex = info.partitions > 0 ? 8 : 16;

    for (int i = 0; i < 16; ++i) {
        const std::size_t numBits = isFixUpOffset(info.partitions, shape, i)
            ? info.indexPrec - 1 : info.indexPrec;
        if (br.pos + numBits > 128) { valid = false; break; }
        const std::uint8_t index = br.bits(numBits);
        if (index >= maxIndex) { valid = false; break; }

        const std::uint8_t region = kPartitionTable[info.partitions][shape][i];

        const int w = weights[index];
        for (int ch = 0; ch < 3; ++ch) {
            const int e0 = bc6hUnquantize(endpts[region][0][ch], precAChan[ch], isSigned);
            const int e1 = bc6hUnquantize(endpts[region][1][ch], precAChan[ch], isSigned);
            const int interp = (e0 * (64 - w) + e1 * w + 32) >> 6;
            const int fin = bc6hFinishUnquantize(interp, isSigned);
            rgb[i * 3 + ch] = bc6hIntToFloat(fin, isSigned);
        }
    }

    if (!valid) {
        for (int i = 0; i < 16; ++i) {
            rgb[i * 3 + 0] = 0.0f;
            rgb[i * 3 + 1] = 0.0f;
            rgb[i * 3 + 2] = 0.0f;
        }
    }
}

} // namespace f4::resources
