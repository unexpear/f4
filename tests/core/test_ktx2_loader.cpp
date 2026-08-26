#include <doctest/doctest.h>

#include "core/resources/ktx2_loader.h"
#include "core/resources/load_error.h"

#include <cstring>
#include <vector>

using namespace f4::resources;

// ── KTX2 builder helper ─────────────────────────────────────────────────────

static constexpr std::uint8_t kKtx2Ident[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

static void putU32LE(std::vector<std::uint8_t>& v, std::uint32_t val) {
    v.push_back(static_cast<std::uint8_t>(val));
    v.push_back(static_cast<std::uint8_t>(val >> 8));
    v.push_back(static_cast<std::uint8_t>(val >> 16));
    v.push_back(static_cast<std::uint8_t>(val >> 24));
}

static void putU64LE(std::vector<std::uint8_t>& v, std::uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        v.push_back(static_cast<std::uint8_t>(val >> (i * 8)));
    }
}

// Build a minimal KTX2 with raw uncompressed texels. No DFD, KVD, or SGD.
static auto buildKtx2Raw(
    std::uint32_t vkFormat, std::uint32_t width, std::uint32_t height,
    const std::vector<std::uint8_t>& texelData)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> buf;
    buf.reserve(80 + 24 + texelData.size());

    // Header (80 bytes)
    buf.insert(buf.end(), kKtx2Ident, kKtx2Ident + 12);
    putU32LE(buf, vkFormat);   // vkFormat
    putU32LE(buf, 1);          // typeSize
    putU32LE(buf, width);
    putU32LE(buf, height);
    putU32LE(buf, 0);          // pixelDepth
    putU32LE(buf, 0);          // layerCount
    putU32LE(buf, 1);          // faceCount
    putU32LE(buf, 1);          // levelCount
    putU32LE(buf, 0);          // supercompressionScheme = None

    const std::uint32_t dfdByteOffset = 80 + 24; // after header + 1 level index
    const std::uint32_t dfdByteLength = 0;
    const std::uint32_t kvdByteOffset = dfdByteOffset;
    const std::uint32_t kvdByteLength = 0;
    putU32LE(buf, dfdByteOffset);
    putU32LE(buf, dfdByteLength);
    putU32LE(buf, kvdByteOffset);
    putU32LE(buf, kvdByteLength);
    putU64LE(buf, 0); // sgdByteOffset
    putU64LE(buf, 0); // sgdByteLength

    // Level index entry (24 bytes): byteOffset, byteLength, uncompressedByteLength
    const std::uint64_t dataOffset = 80 + 24; // right after level index
    putU64LE(buf, dataOffset);
    putU64LE(buf, static_cast<std::uint64_t>(texelData.size()));
    putU64LE(buf, static_cast<std::uint64_t>(texelData.size()));

    // Texel data
    buf.insert(buf.end(), texelData.begin(), texelData.end());
    return buf;
}

// ── Test cases ──────────────────────────────────────────────────────────────

TEST_CASE("KTX2 1x1 R8") {
    const std::vector<std::uint8_t> texel = {0xAA};
    auto ktx = buildKtx2Raw(9, 1, 1, texel);

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::R8_UNORM);
    REQUIRE(tex.mips.size() == 1);
    CHECK(tex.mips[0].width == 1);
    CHECK(tex.mips[0].height == 1);
    CHECK(tex.mips[0].bytes.size() == 1);
    CHECK(static_cast<std::uint8_t>(tex.mips[0].bytes[0]) == 0xAA);
}

TEST_CASE("KTX2 2x2 RGBA8 multi-color") {
    // 4 pixels: red, green, blue, white
    const std::vector<std::uint8_t> texel = {
        255, 0, 0, 255,   // red
        0, 255, 0, 255,   // green
        0, 0, 255, 255,   // blue
        255, 255, 255, 255 // white
    };
    auto ktx = buildKtx2Raw(37, 2, 2, texel); // VK_FORMAT_R8G8B8A8_UNORM = 37

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    REQUIRE(tex.mips.size() == 1);
    CHECK(tex.mips[0].width == 2);
    CHECK(tex.mips[0].height == 2);
    CHECK(tex.mips[0].bytes.size() == 16);

    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 255); CHECK(p[1] == 0);   CHECK(p[2] == 0);   CHECK(p[3] == 255);
    CHECK(p[4] == 0);   CHECK(p[5] == 255); CHECK(p[6] == 0);   CHECK(p[7] == 255);
    CHECK(p[8] == 0);   CHECK(p[9] == 0);   CHECK(p[10] == 255); CHECK(p[11] == 255);
    CHECK(p[12] == 255); CHECK(p[13] == 255); CHECK(p[14] == 255); CHECK(p[15] == 255);
}

TEST_CASE("KTX2 1x1 RGBA32_FLOAT white") {
    // float32(1.0) x 4 channels = 16 bytes
    const std::uint32_t one = 0x3F800000; // 1.0f in IEEE754
    std::vector<std::uint8_t> texel(16);
    for (int i = 0; i < 4; ++i) {
        texel[i * 4 + 0] = static_cast<std::uint8_t>(one);
        texel[i * 4 + 1] = static_cast<std::uint8_t>(one >> 8);
        texel[i * 4 + 2] = static_cast<std::uint8_t>(one >> 16);
        texel[i * 4 + 3] = static_cast<std::uint8_t>(one >> 24);
    }
    auto ktx = buildKtx2Raw(109, 1, 1, texel); // VK_FORMAT_R32G32B32A32_SFLOAT = 109

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA32_FLOAT);
    REQUIRE(tex.mips.size() == 1);
    CHECK(tex.mips[0].bytes.size() == 16);
}

TEST_CASE("KTX2 1x1 BGRA8") {
    const std::vector<std::uint8_t> texel = {0x44, 0x33, 0x22, 0xFF}; // B=0x44 G=0x33 R=0x22 A=0xFF
    auto ktx = buildKtx2Raw(44, 1, 1, texel); // VK_FORMAT_B8G8R8A8_UNORM = 44

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::BGRA8_UNORM);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.mips[0].bytes.data());
    CHECK(p[0] == 0x44); CHECK(p[1] == 0x33); CHECK(p[2] == 0x22); CHECK(p[3] == 0xFF);
}

TEST_CASE("KTX2 1x1 R16_FLOAT") {
    // half(1.0) = 0x3C00
    const std::vector<std::uint8_t> texel = {0x00, 0x3C};
    auto ktx = buildKtx2Raw(76, 1, 1, texel); // VK_FORMAT_R16_SFLOAT = 76

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::R16_FLOAT);
}

TEST_CASE("KTX2 1x1 RGBA16_FLOAT") {
    // 4 × half(1.0) = 8 bytes
    const std::vector<std::uint8_t> texel = {0x00,0x3C, 0x00,0x3C, 0x00,0x3C, 0x00,0x3C};
    auto ktx = buildKtx2Raw(97, 1, 1, texel); // VK_FORMAT_R16G16B16A16_SFLOAT = 97

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    REQUIRE(result.hasValue());
    CHECK(result.value().format == TextureFormat::RGBA16_FLOAT);
}

TEST_CASE("KTX2 bad magic") {
    std::vector<std::uint8_t> bad(80, 0);
    bad[0] = 0xDE; bad[1] = 0xAD;
    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bad.data()), bad.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::BadSignature);
}

TEST_CASE("KTX2 truncated header") {
    std::vector<std::uint8_t> short_data(10, 0);
    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(short_data.data()), short_data.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("KTX2 unsupported VK format") {
    // VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM = 1000156000 — way out of range
    auto ktx = buildKtx2Raw(1000156000, 1, 1, {0});
    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::UnsupportedBpp);
}

TEST_CASE("KTX2 zero dimensions") {
    auto ktx = buildKtx2Raw(37, 0, 1, {});
    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(ktx.data()), ktx.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::BadDimensions);
}

TEST_CASE("KTX2 Basis path rejects garbage as BadSignature") {
    // Build a KTX2 with vkFormat=0 but fill level data with garbage.
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), kKtx2Ident, kKtx2Ident + 12);
    putU32LE(buf, 0);  // vkFormat = 0 → Basis path
    putU32LE(buf, 1);  // typeSize
    putU32LE(buf, 1);  // width
    putU32LE(buf, 1);  // height
    putU32LE(buf, 0);  // pixelDepth
    putU32LE(buf, 0);  // layerCount
    putU32LE(buf, 1);  // faceCount
    putU32LE(buf, 1);  // levelCount
    putU32LE(buf, 0);  // supercompressionScheme = None (raw Basis → will be rejected)
    putU32LE(buf, 104); // dfdByteOffset
    putU32LE(buf, 0);   // dfdByteLength
    putU32LE(buf, 104); // kvdByteOffset
    putU32LE(buf, 0);   // kvdByteLength
    putU64LE(buf, 0);   // sgdByteOffset
    putU64LE(buf, 0);   // sgdByteLength
    // Level index
    putU64LE(buf, 104); // byteOffset
    putU64LE(buf, 8);   // byteLength
    putU64LE(buf, 8);   // uncompressedByteLength
    // Garbage level data
    buf.insert(buf.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00});

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    CHECK(!result.hasValue());
    CHECK(result.error() == LoadError::BadSignature);
}

TEST_CASE("KTX2 mip chain 4x4 → 2x2 → 1x1") {
    // VK_FORMAT_R8G8B8A8_UNORM, 3 levels
    const std::uint32_t w = 4, h = 4, levels = 3;
    std::vector<std::uint8_t> texel;
    // Level 0: 4×4 = 64 bytes, filled with 0x11
    texel.insert(texel.end(), 64, 0x11);
    // Level 1: 2×2 = 16 bytes, filled with 0x22
    texel.insert(texel.end(), 16, 0x22);
    // Level 2: 1×1 = 4 bytes, filled with 0x33
    texel.insert(texel.end(), 4, 0x33);

    std::vector<std::uint8_t> buf;
    buf.reserve(80 + 72 + texel.size());
    buf.insert(buf.end(), kKtx2Ident, kKtx2Ident + 12);
    putU32LE(buf, 37); // VK_FORMAT_R8G8B8A8_UNORM
    putU32LE(buf, 1);  // typeSize
    putU32LE(buf, w);
    putU32LE(buf, h);
    putU32LE(buf, 0); putU32LE(buf, 0); putU32LE(buf, 1);
    putU32LE(buf, levels);
    putU32LE(buf, 0); // supercompression = None
    const std::uint32_t dfdOff = static_cast<std::uint32_t>(80 + 3 * 24);
    putU32LE(buf, dfdOff); putU32LE(buf, 0);
    putU32LE(buf, dfdOff); putU32LE(buf, 0);
    putU64LE(buf, 0); putU64LE(buf, 0);

    // Level index: sizes = 64, 16, 4; offsets packed sequentially
    std::uint64_t offset = 80 + 3 * 24; // after level index
    const std::uint64_t sizes[] = {64, 16, 4};
    for (int i = 0; i < 3; ++i) {
        putU64LE(buf, offset);
        putU64LE(buf, sizes[i]);
        putU64LE(buf, sizes[i]);
        offset += sizes[i];
    }
    buf.insert(buf.end(), texel.begin(), texel.end());

    auto result = loadKtx2(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    REQUIRE(result.hasValue());
    auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    REQUIRE(tex.mips.size() == 3);
    CHECK(tex.mips[0].width == 4);  CHECK(tex.mips[0].height == 4);
    CHECK(tex.mips[1].width == 2);  CHECK(tex.mips[1].height == 2);
    CHECK(tex.mips[2].width == 1);  CHECK(tex.mips[2].height == 1);
    CHECK(tex.mips[0].bytes.size() == 64);
    CHECK(tex.mips[1].bytes.size() == 16);
    CHECK(tex.mips[2].bytes.size() == 4);
    // Spot-check mip data
    CHECK(static_cast<std::uint8_t>(tex.mips[0].bytes[0]) == 0x11);
    CHECK(static_cast<std::uint8_t>(tex.mips[1].bytes[0]) == 0x22);
    CHECK(static_cast<std::uint8_t>(tex.mips[2].bytes[0]) == 0x33);
}
