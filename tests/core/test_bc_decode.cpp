#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "core/resources/bc_decode.h"
#include "core/resources/dds_loader.h"
#include "core/resources/load_error.h"
#include "core/resources/texture.h"

using namespace f4;
using resources::BcFormat;
using resources::LoadError;
using resources::TextureFormat;
using resources::decodeBC1Block;
using resources::decodeBC2Block;
using resources::decodeBC3Block;
using resources::decodeBC4Block;
using resources::decodeBC5Block;
using resources::decodeBC6HBlock;
using resources::decodeBC7Block;
using resources::loadDds;

namespace {

struct BitWriter {
    std::uint8_t data[16] = {};
    std::size_t pos = 0;

    void write(std::uint32_t value, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if ((value >> i) & 1u) {
                data[pos >> 3u] |= static_cast<std::uint8_t>(1u << (pos & 7u));
            }
            ++pos;
        }
    }
};

constexpr std::uint16_t rgb565(int r5, int g6, int b5) {
    return static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

auto bc1ColorBlock(std::uint16_t c0, std::uint16_t c1,
                   const std::uint8_t idx[16]) -> std::vector<std::byte> {
    std::vector<std::byte> b(8, std::byte{0});
    std::memcpy(b.data(), &c0, 2);
    std::memcpy(b.data() + 2, &c1, 2);
    for (int i = 0; i < 16; ++i) {
        const int byteIdx = i / 4;
        const int shift = 2 * (i % 4);
        b[4 + byteIdx] |= static_cast<std::byte>(idx[i] << shift);
    }
    return b;
}

void pack3BitIndices(std::byte* out, const std::uint8_t idx[16]) {
    for (int i = 0; i < 6; ++i) out[i] = std::byte{0};
    for (int i = 0; i < 16; ++i) {
        const std::size_t bitPos = 3 * static_cast<std::size_t>(i);
        for (int bit = 0; bit < 3; ++bit) {
            if ((idx[i] >> bit) & 1u) {
                const std::size_t p = bitPos + static_cast<std::size_t>(bit);
                out[p / 8] |= static_cast<std::byte>(1u << (p % 8));
            }
        }
    }
}

} // anonymous namespace

TEST_CASE("BC1: solid red block (all indices 0)") {
    std::uint8_t idx[16] = {};
    auto block = bc1ColorBlock(rgb565(31, 0, 0), rgb565(0, 0, 31), idx);
    std::uint8_t rgba[16 * 4];
    decodeBC1Block(block.data(), rgba);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgba[i * 4 + 0] == 255);
        CHECK(rgba[i * 4 + 1] == 0);
        CHECK(rgba[i * 4 + 2] == 0);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("BC1: index selects interpolated colors") {
    std::uint8_t idx[16] = {};
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3;
    auto block = bc1ColorBlock(rgb565(31, 0, 0), rgb565(0, 0, 31), idx);
    std::uint8_t rgba[16 * 4];
    decodeBC1Block(block.data(), rgba);
    CHECK(rgba[0] == 255); CHECK(rgba[2] == 0);
    CHECK(rgba[4] == 0); CHECK(rgba[6] == 255);
    // Pixel 2 = 2/3 c0 + 1/3 c1 = (170, 0, 85).
    CHECK(rgba[8] == 170); CHECK(rgba[9] == 0); CHECK(rgba[10] == 85);
    // Pixel 3 = 1/3 c0 + 2/3 c1 = (85, 0, 170).
    CHECK(rgba[12] == 85); CHECK(rgba[13] == 0); CHECK(rgba[14] == 170);
}

TEST_CASE("BC1: punch-through alpha when c0 <= c1") {
    std::uint8_t idx[16] = {};
    for (int i = 0; i < 16; ++i) idx[i] = 3;
    auto block = bc1ColorBlock(rgb565(0, 0, 31), rgb565(31, 0, 0), idx);
    std::uint8_t rgba[16 * 4];
    decodeBC1Block(block.data(), rgba);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgba[i * 4 + 3] == 0);
    }
}

TEST_CASE("BC2: explicit alpha + 4-color block") {
    std::vector<std::byte> block(16, std::byte{0});
    for (int i = 0; i < 8; ++i) block[i] = std::byte{0xFF};
    std::uint8_t idx[16];
    for (int i = 0; i < 16; ++i) idx[i] = 2;
    auto color = bc1ColorBlock(rgb565(31, 0, 0), rgb565(0, 0, 31), idx);
    std::memcpy(block.data() + 8, color.data(), 8);

    std::uint8_t rgba[16 * 4];
    decodeBC2Block(block.data(), rgba);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgba[i * 4 + 0] == 170);
        CHECK(rgba[i * 4 + 1] == 0);
        CHECK(rgba[i * 4 + 2] == 85);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("BC2: 4-bit alpha expansion") {
    std::vector<std::byte> block(16, std::byte{0});
    block[0] = std::byte{0x08};
    std::uint8_t idx[16] = {};
    auto color = bc1ColorBlock(rgb565(31, 0, 0), rgb565(31, 0, 0), idx);
    std::memcpy(block.data() + 8, color.data(), 8);

    std::uint8_t rgba[16 * 4];
    decodeBC2Block(block.data(), rgba);
    CHECK(rgba[3] == 136);
}

TEST_CASE("BC3: interpolated alpha + green color") {
    std::vector<std::byte> block(16, std::byte{0});
    block[0] = std::byte{200};
    block[1] = std::byte{100};
    std::uint8_t idx[16] = {};
    auto color = bc1ColorBlock(rgb565(0, 63, 0), rgb565(0, 0, 0), idx);
    std::memcpy(block.data() + 8, color.data(), 8);

    std::uint8_t rgba[16 * 4];
    decodeBC3Block(block.data(), rgba);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgba[i * 4 + 0] == 0);
        CHECK(rgba[i * 4 + 1] == 255);
        CHECK(rgba[i * 4 + 2] == 0);
        CHECK(rgba[i * 4 + 3] == 200);
    }
}

TEST_CASE("BC4: full palette interpolation") {
    std::uint8_t idx[16];
    for (int i = 0; i < 16; ++i) idx[i] = static_cast<std::uint8_t>(i % 8);
    std::vector<std::byte> block(8, std::byte{0});
    block[0] = std::byte{210};
    block[1] = std::byte{70};
    pack3BitIndices(block.data() + 2, idx);

    std::uint8_t out[16];
    decodeBC4Block(block.data(), out);
    const std::uint8_t expected[8] = {210, 70, 190, 170, 150, 130, 110, 90};
    for (int i = 0; i < 16; ++i) {
        CHECK(out[i] == expected[i % 8]);
    }
}

TEST_CASE("BC4: 6-value mode with 0 and 255 endpoints") {
    std::uint8_t idx[16];
    for (int i = 0; i < 16; ++i) idx[i] = (i % 2) ? 7 : 6;
    std::vector<std::byte> block(8, std::byte{0});
    block[0] = std::byte{100};
    block[1] = std::byte{200};
    pack3BitIndices(block.data() + 2, idx);

    std::uint8_t out[16];
    decodeBC4Block(block.data(), out);
    for (int i = 0; i < 16; ++i) {
        CHECK(out[i] == ((i % 2) ? 255 : 0));
    }
}

TEST_CASE("BC5: two independent channels") {
    std::vector<std::byte> block(16, std::byte{0});
    block[0] = std::byte{100};
    block[1] = std::byte{100};
    block[8] = std::byte{50};
    block[9] = std::byte{50};

    std::uint8_t rg[16 * 2];
    decodeBC5Block(block.data(), rg);
    for (int i = 0; i < 16; ++i) {
        CHECK(rg[i * 2 + 0] == 100);
        CHECK(rg[i * 2 + 1] == 50);
    }
}

// ============================================================================
// BC7
// ============================================================================

TEST_CASE("BC7 mode 6: solid white single subset") {
    BitWriter w;
    w.write(0, 6);      // six leading zeros
    w.write(1, 1);      // mode 6 marker
    for (int i = 0; i < 8; ++i) w.write(0x7F, 7); // R0,R1,G0,G1,B0,B1,A0,A1
    w.write(1, 1);      // P0
    w.write(1, 1);      // P1
    w.write(0, 63);     // indices: 3-bit fixup + 15x4-bit, all zero

    std::uint8_t rgba[16 * 4];
    decodeBC7Block(reinterpret_cast<const std::byte*>(w.data), rgba);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgba[i * 4 + 0] == 255);
        CHECK(rgba[i * 4 + 1] == 255);
        CHECK(rgba[i * 4 + 2] == 255);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("BC7 mode 1: two subsets white/black partition") {
    BitWriter w;
    w.write(0, 1);      // bit0 = 0
    w.write(1, 1);      // bit1 = 1 -> mode 1
    w.write(0, 6);      // shape 0
    // R endpoints: R0=63,R1=63 (subset0), R2=0,R3=0 (subset1)
    w.write(63, 6); w.write(63, 6); w.write(0, 6); w.write(0, 6);
    // G endpoints
    w.write(63, 6); w.write(63, 6); w.write(0, 6); w.write(0, 6);
    // B endpoints
    w.write(63, 6); w.write(63, 6); w.write(0, 6); w.write(0, 6);
    // P-bits: P0=1 (subset0), P1=0 (subset1)
    w.write(1, 1); w.write(0, 1);
    // Indices: 46 zero bits
    w.write(0, 46);

    std::uint8_t rgba[16 * 4];
    decodeBC7Block(reinterpret_cast<const std::byte*>(w.data), rgba);
    // Shape 0: pixels {0,1,4,5,8,9,12,13} subset0 (white), rest subset1 (black).
    const bool subset0[16] = {1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0};
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t expect = subset0[i] ? 255 : 0;
        CHECK(rgba[i * 4 + 0] == expect);
        CHECK(rgba[i * 4 + 1] == expect);
        CHECK(rgba[i * 4 + 2] == expect);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

// ============================================================================
// BC6H
// ============================================================================

TEST_CASE("BC6H mode 11: max endpoints decode to 65504") {
    BitWriter w;
    // Mode 0x03 (mode 11): low2=3, next3=0.
    w.write(3, 2);
    w.write(0, 3);
    // Six endpoint fields x 10 bits = 1023 each.
    for (int i = 0; i < 6; ++i) w.write(1023, 10);
    // Indices: 3-bit fixup + 15x4-bit = 63 zero bits.
    w.write(0, 63);

    float rgb[16 * 3];
    decodeBC6HBlock(reinterpret_cast<const std::byte*>(w.data), rgb, false);
    for (int i = 0; i < 16; ++i) {
        CHECK(rgb[i * 3 + 0] == doctest::Approx(65504.0f));
        CHECK(rgb[i * 3 + 1] == doctest::Approx(65504.0f));
        CHECK(rgb[i * 3 + 2] == doctest::Approx(65504.0f));
    }
}

TEST_CASE("BC6H invalid mode decodes to black") {
    BitWriter w;
    // Mode 0x1f (reserved): low2=3, next3=7.
    w.write(3, 2);
    w.write(7, 3);
    w.write(0, 123); // remaining bits unused

    float rgb[16 * 3];
    decodeBC6HBlock(reinterpret_cast<const std::byte*>(w.data), rgb, false);
    for (int i = 0; i < 16 * 3; ++i) {
        CHECK(rgb[i] == 0.0f);
    }
}

// ============================================================================
// DDS container
// ============================================================================

namespace {
// Write a little-endian u32 into buf at byte offset.
void putU32(std::vector<std::byte>& buf, std::size_t off, std::uint32_t v) {
    std::memcpy(buf.data() + off, &v, 4);
}

// Build a legacy-fourcc DDS header (124 bytes) and return header+payload.
auto makeDds(std::uint32_t fourCC, std::uint32_t width, std::uint32_t height,
             std::uint32_t mipCount, const std::vector<std::byte>& payload)
    -> std::vector<std::byte> {
    std::vector<std::byte> file(4 + 124 + payload.size(), std::byte{0});
    putU32(file, 0, 0x20534444u); // "DDS "
    std::size_t h = 4;
    putU32(file, h + 0, 124);                 // dwSize
    putU32(file, h + 4, 0x1 | 0x2 | 0x4 | 0x1000); // flags: CAPS|HEIGHT|WIDTH|PIXELFORMAT
    putU32(file, h + 8, height);
    putU32(file, h + 12, width);
    putU32(file, h + 16, 0);                  // pitch
    putU32(file, h + 20, 0);                  // depth
    putU32(file, h + 24, mipCount);
    // reserved1[11] stays zero
    putU32(file, h + 72, 32);                 // ddspf.dwSize
    putU32(file, h + 76, 0x4);                // ddspf.dwFlags = FOURCC
    putU32(file, h + 80, fourCC);             // ddspf.dwFourCC
    putU32(file, h + 108, 0x1000);            // dwCaps = TEXTURE
    std::memcpy(file.data() + 128, payload.data(), payload.size());
    return file;
}

// Build a DX10-header DDS file.
auto makeDdsDx10(std::uint32_t dxgiFormat, std::uint32_t width, std::uint32_t height,
                 std::uint32_t mipCount, const std::vector<std::byte>& payload)
    -> std::vector<std::byte> {
    std::vector<std::byte> file(4 + 124 + 20 + payload.size(), std::byte{0});
    putU32(file, 0, 0x20534444u);
    std::size_t h = 4;
    putU32(file, h + 0, 124);
    putU32(file, h + 4, 0x1 | 0x2 | 0x4 | 0x1000);
    putU32(file, h + 8, height);
    putU32(file, h + 12, width);
    putU32(file, h + 16, 0);
    putU32(file, h + 20, 0);
    putU32(file, h + 24, mipCount);
    putU32(file, h + 72, 32);
    putU32(file, h + 76, 0x4);
    putU32(file, h + 80, 0x30315844u); // "DX10"
    putU32(file, h + 108, 0x1000);
    // DX10 header at offset 128.
    putU32(file, 128 + 0, dxgiFormat);
    putU32(file, 128 + 4, 3);          // resourceDimension = TEXTURE2D
    putU32(file, 128 + 8, 0);          // miscFlag
    putU32(file, 128 + 12, 1);         // arraySize
    putU32(file, 128 + 16, 0);         // miscFlags2
    std::memcpy(file.data() + 148, payload.data(), payload.size());
    return file;
}
} // anonymous namespace

TEST_CASE("DDS: legacy DXT1 4x4 solid red") {
    std::uint8_t idx[16] = {};
    auto block = bc1ColorBlock(rgb565(31, 0, 0), rgb565(31, 0, 0), idx);
    auto file = makeDds(0x31545844u, 4, 4, 1, block); // "DXT1"

    auto result = loadDds(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    REQUIRE(tex.mips.size() == 1);
    CHECK(tex.mips[0].width == 4);
    CHECK(tex.mips[0].height == 4);
    CHECK(tex.mips[0].bytes.size() == 4 * 4 * 4);
    // Every pixel red opaque.
    for (int i = 0; i < 16; ++i) {
        CHECK(tex.mips[0].bytes[i * 4 + 0] == std::byte{255});
        CHECK(tex.mips[0].bytes[i * 4 + 1] == std::byte{0});
        CHECK(tex.mips[0].bytes[i * 4 + 2] == std::byte{0});
        CHECK(tex.mips[0].bytes[i * 4 + 3] == std::byte{255});
    }
}

TEST_CASE("DDS: DX10 header BC7 solid white") {
    BitWriter w;
    w.write(0, 6); w.write(1, 1);
    for (int i = 0; i < 8; ++i) w.write(0x7F, 7);
    w.write(1, 1); w.write(1, 1);
    w.write(0, 63);
    std::vector<std::byte> block(16);
    std::memcpy(block.data(), w.data, 16);

    auto file = makeDdsDx10(98, 4, 4, 1, block); // DXGI_FORMAT_BC7_UNORM
    auto result = loadDds(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    for (int i = 0; i < 16; ++i) {
        CHECK(tex.mips[0].bytes[i * 4 + 0] == std::byte{255});
        CHECK(tex.mips[0].bytes[i * 4 + 3] == std::byte{255});
    }
}

TEST_CASE("DDS: BC4 decoded to R8_UNORM") {
    // r0=r1=200, all indices 0 -> every pixel R=200.
    std::vector<std::byte> block(8, std::byte{0});
    block[0] = std::byte{200};
    block[1] = std::byte{200};

    auto file = makeDdsDx10(80, 4, 4, 1, block); // DXGI_FORMAT_BC4_UNORM
    auto result = loadDds(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::R8_UNORM);
    CHECK(tex.mips[0].bytes.size() == 4 * 4 * 1);
    for (int i = 0; i < 16; ++i) {
        CHECK(tex.mips[0].bytes[i] == std::byte{200});
    }
}

TEST_CASE("DDS: BC6H UF16 decoded to RGBA32_FLOAT") {
    BitWriter w;
    w.write(3, 2); w.write(0, 3);
    for (int i = 0; i < 6; ++i) w.write(1023, 10);
    w.write(0, 63);
    std::vector<std::byte> block(16);
    std::memcpy(block.data(), w.data, 16);

    auto file = makeDdsDx10(95, 4, 4, 1, block); // DXGI_FORMAT_BC6H_UF16
    auto result = loadDds(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA32_FLOAT);
    CHECK(tex.mips[0].bytes.size() == 4 * 4 * 16);
    const auto* f = reinterpret_cast<const float*>(tex.mips[0].bytes.data());
    CHECK(f[0] == doctest::Approx(65504.0f));
    CHECK(f[3] == doctest::Approx(1.0f)); // alpha
}

TEST_CASE("DDS: mip chain sizes halve per level") {
    // 8x8 BC1 with 3 mips: 8x8 (4 blocks), 4x4 (1), 2x2 (1).
    std::uint8_t idx[16] = {};
    auto block = bc1ColorBlock(rgb565(0, 0, 31), rgb565(0, 0, 31), idx);
    std::vector<std::byte> payload;
    for (int i = 0; i < 4; ++i) payload.insert(payload.end(), block.begin(), block.end());
    for (int i = 0; i < 1; ++i) payload.insert(payload.end(), block.begin(), block.end());
    for (int i = 0; i < 1; ++i) payload.insert(payload.end(), block.begin(), block.end());

    auto file = makeDds(0x31545844u, 8, 8, 3, payload);
    auto result = loadDds(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    REQUIRE(tex.mips.size() == 3);
    CHECK(tex.mips[0].width == 8);
    CHECK(tex.mips[0].height == 8);
    CHECK(tex.mips[0].bytes.size() == 8 * 8 * 4);
    CHECK(tex.mips[1].width == 4);
    CHECK(tex.mips[1].height == 4);
    CHECK(tex.mips[2].width == 2);
    CHECK(tex.mips[2].height == 2);
    CHECK(tex.mips[2].bytes.size() == 2 * 2 * 4);
    // Blue everywhere.
    CHECK(tex.mips[1].bytes[0] == std::byte{0});
    CHECK(tex.mips[1].bytes[2] == std::byte{255});
}

TEST_CASE("DDS: bad magic rejected") {
    std::vector<std::byte> bad(128, std::byte{0});
    auto result = loadDds(bad);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::BadSignature);
}

TEST_CASE("DDS: truncated rejected") {
    std::vector<std::byte> tiny(8, std::byte{0});
    auto result = loadDds(tiny);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("DDS: unsupported fourcc rejected") {
    std::vector<std::byte> block(8, std::byte{0});
    auto file = makeDds(0x44444444u, 4, 4, 1, block); // "DDDD"
    auto result = loadDds(file);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::UnsupportedCompression);
}
