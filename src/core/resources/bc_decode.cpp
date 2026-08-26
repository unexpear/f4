#include "core/resources/bc_decode.h"

#include <cstring>

namespace f4::resources {

namespace {

auto readU16(const std::byte* p) -> std::uint16_t {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// RGB565 -> RGB888 with bit replication.
void unpack565(std::uint16_t c, std::uint8_t* rgb) {
    const std::uint8_t r5 = static_cast<std::uint8_t>((c >> 11) & 0x1F);
    const std::uint8_t g6 = static_cast<std::uint8_t>((c >> 5) & 0x3F);
    const std::uint8_t b5 = static_cast<std::uint8_t>(c & 0x1F);
    rgb[0] = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
    rgb[1] = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
    rgb[2] = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
}

// Round-to-nearest weighted mix: (wa*a + wb*b + (wa+wb)/2) / (wa+wb).
auto mix(std::uint8_t a, std::uint8_t b, int wa, int wb) -> std::uint8_t {
    const int total = wa + wb;
    return static_cast<std::uint8_t>((wa * a + wb * b + total / 2) / total);
}

// Shared BC1 color endpoint decode. fourColorMode selects between the
// c0<=c1 "punch-through" contract (BC1) and forced 4-color (BC2/BC3).
void decodeBC1Colors(const std::byte* block, bool fourColorMode,
                     std::uint8_t colors[4][4]) {
    const std::uint16_t c0 = readU16(block);
    const std::uint16_t c1 = readU16(block + 2);

    std::uint8_t e0[3], e1[3];
    unpack565(c0, e0);
    unpack565(c1, e1);

    colors[0][0] = e0[0]; colors[0][1] = e0[1]; colors[0][2] = e0[2]; colors[0][3] = 255;
    colors[1][0] = e1[0]; colors[1][1] = e1[1]; colors[1][2] = e1[2]; colors[1][3] = 255;

    if (fourColorMode || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            colors[2][ch] = mix(e0[ch], e1[ch], 2, 1);
            colors[3][ch] = mix(e0[ch], e1[ch], 1, 2);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            colors[2][ch] = mix(e0[ch], e1[ch], 1, 1);
        }
        colors[2][3] = 255;
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }
}

// Read sixteen 2-bit indices from 4 bytes; pixel i uses bits (2i, 2i+1).
void read2BitIndices(const std::byte* p, std::uint8_t* idx) {
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>(p[i / 4]);
        idx[i] = static_cast<std::uint8_t>((byte >> (2 * (i % 4))) & 0x3);
    }
}

// Read sixteen 3-bit indices from a 48-bit little-endian field.
void read3BitIndices(const std::byte* p, std::uint8_t* idx) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        idx[i] = static_cast<std::uint8_t>((bits >> (3 * i)) & 0x7);
    }
}

// BC4-style single-channel endpoint decode.
void decodeBC4Channel(const std::byte* block, std::uint8_t* out) {
    const std::uint8_t r0 = static_cast<std::uint8_t>(block[0]);
    const std::uint8_t r1 = static_cast<std::uint8_t>(block[1]);

    std::uint8_t palette[8];
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (int k = 2; k < 8; ++k) {
            palette[k] = static_cast<std::uint8_t>(
                ((8 - k) * r0 + (k - 1) * r1 + 3) / 7);
        }
    } else {
        for (int k = 2; k < 6; ++k) {
            palette[k] = static_cast<std::uint8_t>(
                ((6 - k) * r0 + (k - 1) * r1 + 2) / 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    std::uint8_t idx[16];
    read3BitIndices(block + 2, idx);
    for (int i = 0; i < 16; ++i) {
        out[i] = palette[idx[i]];
    }
}

} // anonymous namespace

void decodeBC1Block(const std::byte* block, std::uint8_t* rgba) {
    std::uint8_t colors[4][4];
    decodeBC1Colors(block, false, colors);
    std::uint8_t idx[16];
    read2BitIndices(block + 4, idx);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(rgba + i * 4, colors[idx[i]], 4);
    }
}

void decodeBC2Block(const std::byte* block, std::uint8_t* rgba) {
    std::uint8_t colors[4][4];
    decodeBC1Colors(block + 8, true, colors);
    std::uint8_t idx[16];
    read2BitIndices(block + 12, idx);
    for (int i = 0; i < 16; ++i) {
        rgba[i * 4 + 0] = colors[idx[i]][0];
        rgba[i * 4 + 1] = colors[idx[i]][1];
        rgba[i * 4 + 2] = colors[idx[i]][2];
        const std::uint8_t a = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(block[i / 2]) >> (4 * (i % 2)) & 0xF);
        rgba[i * 4 + 3] = static_cast<std::uint8_t>((a << 4) | a);
    }
}

void decodeBC3Block(const std::byte* block, std::uint8_t* rgba) {
    std::uint8_t colors[4][4];
    decodeBC1Colors(block + 8, true, colors);
    std::uint8_t idx[16];
    read2BitIndices(block + 12, idx);

    std::uint8_t alpha[16];
    decodeBC4Channel(block, alpha);

    for (int i = 0; i < 16; ++i) {
        rgba[i * 4 + 0] = colors[idx[i]][0];
        rgba[i * 4 + 1] = colors[idx[i]][1];
        rgba[i * 4 + 2] = colors[idx[i]][2];
        rgba[i * 4 + 3] = alpha[i];
    }
}

void decodeBC4Block(const std::byte* block, std::uint8_t* r) {
    decodeBC4Channel(block, r);
}

void decodeBC5Block(const std::byte* block, std::uint8_t* rg) {
    std::uint8_t r[16], g[16];
    decodeBC4Channel(block, r);
    decodeBC4Channel(block + 8, g);
    for (int i = 0; i < 16; ++i) {
        rg[i * 2 + 0] = r[i];
        rg[i * 2 + 1] = g[i];
    }
}

} // namespace f4::resources0},
