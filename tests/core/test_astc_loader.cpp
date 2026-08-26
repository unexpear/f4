#include <doctest/doctest.h>

#include "core/resources/astc_loader.h"
#include "core/resources/load_error.h"
#include "core/resources/texture.h"

#include <cstring>
#include <vector>

using namespace f4::resources;

// ── ASTC file builder ───────────────────────────────────────────────────────

static constexpr std::uint8_t kAstcMagic[4] = {0x13, 0xAB, 0xA1, 0x5C};

static void putU24LE(std::vector<std::uint8_t>& v, std::uint32_t val) {
    v.push_back(static_cast<std::uint8_t>(val));
    v.push_back(static_cast<std::uint8_t>(val >> 8));
    v.push_back(static_cast<std::uint8_t>(val >> 16));
}

// Build a minimal .astc file with void-extent constant-color blocks.
// For LDR: rh/gh/bh/ah are uint16 packed as (val | val<<8).
// For HDR: rh/gh/bh/ah are raw half-float bit patterns.
static auto buildAstcFile(
    std::uint8_t blockW, std::uint8_t blockH,
    std::uint32_t dimX, std::uint32_t dimY,
    const std::vector<std::uint8_t>& blockData)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> buf;
    buf.reserve(16 + blockData.size());

    // Header (16 bytes)
    buf.insert(buf.end(), kAstcMagic, kAstcMagic + 4);
    buf.push_back(blockW);
    buf.push_back(blockH);
    buf.push_back(1); // blockZ = 1 (2D)
    putU24LE(buf, dimX);
    putU24LE(buf, dimY);
    putU24LE(buf, 1); // dimZ = 1

    // Block data
    buf.insert(buf.end(), blockData.begin(), blockData.end());
    return buf;
}

// Create a void-extent LDR block for a solid RGBA color.
// r/g/b/a are 8-bit values; packed into 16-bit unorm as (val | val<<8).
static auto makeVoidExtentLdr(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    -> std::vector<std::uint8_t>
{
    std::uint16_t rh = static_cast<std::uint16_t>(r) | (static_cast<std::uint16_t>(r) << 8);
    std::uint16_t gh = static_cast<std::uint16_t>(g) | (static_cast<std::uint16_t>(g) << 8);
    std::uint16_t bh = static_cast<std::uint16_t>(b) | (static_cast<std::uint16_t>(b) << 8);
    std::uint16_t ah = static_cast<std::uint16_t>(a) | (static_cast<std::uint16_t>(a) << 8);

    std::vector<std::uint8_t> blk(16, 0xFF);
    blk[0] = 0xFC;
    blk[1] = 0xFD;
    blk[8]  = static_cast<std::uint8_t>(rh);
    blk[9]  = static_cast<std::uint8_t>(rh >> 8);
    blk[10] = static_cast<std::uint8_t>(gh);
    blk[11] = static_cast<std::uint8_t>(gh >> 8);
    blk[12] = static_cast<std::uint8_t>(bh);
    blk[13] = static_cast<std::uint8_t>(bh >> 8);
    blk[14] = static_cast<std::uint8_t>(ah);
    blk[15] = static_cast<std::uint8_t>(ah >> 8);
    return blk;
}

// Create a void-extent HDR block with half-float RGBA values.
static auto makeVoidExtentHdr(std::uint16_t rh, std::uint16_t gh, std::uint16_t bh, std::uint16_t ah)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> blk(16, 0xFF);
    blk[0] = 0xFC;
    // Byte 1 stays 0xFF (HDR void extent bit is set via byte 1 bit 0 = 1 → already 0xFF)
    blk[8]  = static_cast<std::uint8_t>(rh);
    blk[9]  = static_cast<std::uint8_t>(rh >> 8);
    blk[10] = static_cast<std::uint8_t>(gh);
    blk[11] = static_cast<std::uint8_t>(gh >> 8);
    blk[12] = static_cast<std::uint8_t>(bh);
    blk[13] = static_cast<std::uint8_t>(bh >> 8);
    blk[14] = static_cast<std::uint8_t>(ah);
    blk[15] = static_cast<std::uint8_t>(ah >> 8);
    return blk;
}

// ── Test cases ──────────────────────────────────────────────────────────────

TEST_CASE("ASTC 4x4 1x1 red LDR") {
    auto blk = makeVoidExtentLdr(255, 0, 0, 255);
    auto file = buildAstcFile(4, 4, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_4X4_UNORM);
    REQUIRE(tex.mips.size() == 1);
    CHECK(tex.mips[0].width == 1);
    CHECK(tex.mips[0].height == 1);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 255); CHECK(p[1] == 0); CHECK(p[2] == 0); CHECK(p[3] == 255);
}

TEST_CASE("ASTC 4x4 1x1 green LDR") {
    auto blk = makeVoidExtentLdr(0, 255, 0, 255);
    auto file = buildAstcFile(4, 4, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_4X4_UNORM);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 0); CHECK(p[1] == 255); CHECK(p[2] == 0); CHECK(p[3] == 255);
}

TEST_CASE("ASTC 4x4 1x1 half-white LDR") {
    auto blk = makeVoidExtentLdr(128, 128, 128, 255);
    auto file = buildAstcFile(4, 4, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_4X4_UNORM);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 128); CHECK(p[1] == 128); CHECK(p[2] == 128); CHECK(p[3] == 255);
}

TEST_CASE("ASTC 6x6 4x4 white LDR") {
    // 4x4 image in 6x6 blocks = 1 block
    auto blk = makeVoidExtentLdr(255, 255, 255, 255);
    auto file = buildAstcFile(6, 6, 4, 4, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_6X6_UNORM);
    CHECK(tex.mips[0].width == 4);
    CHECK(tex.mips[0].height == 4);
    // All 16 pixels should be white
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    for (int i = 0; i < 16; ++i) {
        CHECK(p[i * 4 + 0] == 255);
        CHECK(p[i * 4 + 1] == 255);
        CHECK(p[i * 4 + 2] == 255);
        CHECK(p[i * 4 + 3] == 255);
    }
}

TEST_CASE("ASTC 8x8 2x2 blue LDR") {
    // 2x2 image in 8x8 blocks = 1 block
    auto blk = makeVoidExtentLdr(0, 0, 255, 255);
    auto file = buildAstcFile(8, 8, 2, 2, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_8X8_UNORM);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    for (int i = 0; i < 4; ++i) {
        CHECK(p[i * 4 + 0] == 0);
        CHECK(p[i * 4 + 1] == 0);
        CHECK(p[i * 4 + 2] == 255);
        CHECK(p[i * 4 + 3] == 255);
    }
}

TEST_CASE("ASTC 12x12 1x1 black LDR") {
    auto blk = makeVoidExtentLdr(0, 0, 0, 255);
    auto file = buildAstcFile(12, 12, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_12X12_UNORM);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 0); CHECK(p[1] == 0); CHECK(p[2] == 0); CHECK(p[3] == 255);
}

TEST_CASE("ASTC 5x4 1x1 red LDR") {
    auto blk = makeVoidExtentLdr(255, 0, 0, 255);
    auto file = buildAstcFile(5, 4, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::ASTC_5X4_UNORM);
}

TEST_CASE("ASTC 10x5 1x1 green LDR") {
    auto blk = makeVoidExtentLdr(0, 255, 0, 255);
    auto file = buildAstcFile(10, 5, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::ASTC_10X5_UNORM);
}

TEST_CASE("ASTC 12x10 1x1 white LDR") {
    auto blk = makeVoidExtentLdr(255, 255, 255, 255);
    auto file = buildAstcFile(12, 10, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::ASTC_12X10_UNORM);
}

TEST_CASE("ASTC 4x4 1x1 red HDR") {
    // Half-float 1.0 = 0x3C00, 0.0 = 0x0000
    auto blk = makeVoidExtentHdr(0x3C00, 0x0000, 0x0000, 0x3C00);
    auto file = buildAstcFile(4, 4, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_4X4_HFLOAT);
    CHECK(tex.mips[0].width == 1);
    CHECK(tex.mips[0].height == 1);
    CHECK(tex.mips[0].bytes.size() == 4 * sizeof(float));
    const auto* p = reinterpret_cast<const float*>(tex.mips[0].bytes.data());
    CHECK(p[0] == doctest::Approx(1.0f)); // R
    CHECK(p[1] == doctest::Approx(0.0f)); // G
    CHECK(p[2] == doctest::Approx(0.0f)); // B
    CHECK(p[3] == doctest::Approx(1.0f)); // A
}

TEST_CASE("ASTC 8x8 1x1 green HDR") {
    // Half-float: 0.0, 1.0, 0.0, 1.0
    auto blk = makeVoidExtentHdr(0x0000, 0x3C00, 0x0000, 0x3C00);
    auto file = buildAstcFile(8, 8, 1, 1, blk);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::ASTC_8X8_HFLOAT);
    const auto* p = reinterpret_cast<const float*>(result.value().mips[0].bytes.data());
    CHECK(p[0] == doctest::Approx(0.0f));
    CHECK(p[1] == doctest::Approx(1.0f));
    CHECK(p[2] == doctest::Approx(0.0f));
    CHECK(p[3] == doctest::Approx(1.0f));
}

TEST_CASE("ASTC bad magic") {
    std::vector<std::uint8_t> bad(16, 0);
    bad[0] = 0xDE; bad[1] = 0xAD;
    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bad.data()), bad.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::BadSignature);
}

TEST_CASE("ASTC truncated header") {
    std::vector<std::uint8_t> short_data(8, 0);
    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(short_data.data()), short_data.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("ASTC zero dimensions") {
    std::vector<std::uint8_t> buf(16, 0);
    buf[0] = 0x13; buf[1] = 0xAB; buf[2] = 0xA1; buf[3] = 0x5C;
    buf[4] = 4; buf[5] = 4; buf[6] = 1; // block 4x4x1
    // dimX = 0 (bytes 7-9), dimY = 1 (bytes 10-12), dimZ = 1 (bytes 13-15)
    buf[10] = 1; buf[13] = 1;
    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::BadDimensions);
}

TEST_CASE("ASTC 3D block rejected") {
    std::vector<std::uint8_t> buf(16, 0);
    buf[0] = 0x13; buf[1] = 0xAB; buf[2] = 0xA1; buf[3] = 0x5C;
    buf[4] = 4; buf[5] = 4; buf[6] = 2; // blockZ = 2 (3D!)
    buf[7] = 4; buf[10] = 4; buf[13] = 1;
    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::UnsupportedCompression);
}

TEST_CASE("ASTC truncated block data") {
    auto blk = makeVoidExtentLdr(255, 0, 0, 255);
    auto file = buildAstcFile(4, 4, 1, 1, blk);
    file.resize(file.size() - 4); // chop off last 4 bytes

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("ASTC TextureFormat block size helpers") {
    CHECK(astcBlockWidth(TextureFormat::ASTC_4X4_UNORM) == 4);
    CHECK(astcBlockHeight(TextureFormat::ASTC_4X4_UNORM) == 4);
    CHECK(astcBlockWidth(TextureFormat::ASTC_12X12_HFLOAT) == 12);
    CHECK(astcBlockHeight(TextureFormat::ASTC_12X12_HFLOAT) == 12);
    CHECK(astcBlockWidth(TextureFormat::ASTC_8X5_UNORM) == 8);
    CHECK(astcBlockHeight(TextureFormat::ASTC_8X5_UNORM) == 5);
    CHECK(astcBlockWidth(TextureFormat::R8_UNORM) == 0); // non-ASTC format
    CHECK(isAstcHdr(TextureFormat::ASTC_6X6_HFLOAT) == true);
    CHECK(isAstcHdr(TextureFormat::ASTC_6X6_UNORM) == false);
    CHECK(astcOutputFormat(TextureFormat::ASTC_4X4_HFLOAT) == TextureFormat::RGBA32_FLOAT);
    CHECK(astcOutputFormat(TextureFormat::ASTC_4X4_UNORM) == TextureFormat::RGBA8_UNORM);
    CHECK(isBlockCompressed(TextureFormat::ASTC_10X10_UNORM) == true);
    CHECK(isBlockCompressed(TextureFormat::ASTC_12X12_HFLOAT) == true);
}

TEST_CASE("ASTC multi-block image 16x16 in 4x4") {
    // 16x16 image = 4×4 = 16 blocks, all solid red
    auto blk = makeVoidExtentLdr(255, 0, 0, 255);
    std::vector<std::uint8_t> allBlocks;
    for (int i = 0; i < 16; ++i) {
        allBlocks.insert(allBlocks.end(), blk.begin(), blk.end());
    }
    auto file = buildAstcFile(4, 4, 16, 16, allBlocks);

    auto result = loadAstc(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(file.data()), file.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::ASTC_4X4_UNORM);
    CHECK(tex.mips[0].width == 16);
    CHECK(tex.mips[0].height == 16);
    CHECK(tex.mips[0].bytes.size() == 16 * 16 * 4);
    // Spot-check corners
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 255); CHECK(p[1] == 0); CHECK(p[2] == 0); CHECK(p[3] == 255);       // top-left
    CHECK(p[(15 * 16 + 15) * 4 + 0] == 255);                                           // bottom-right
}
