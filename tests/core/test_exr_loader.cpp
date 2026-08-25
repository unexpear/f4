#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "core/resources/exr_loader.h"
#include "core/resources/load_error.h"
#include "core/resources/texture.h"

using namespace f4;
using resources::LoadError;
using resources::TextureFormat;
using resources::loadExr;

namespace {
void writeU32(std::vector<std::byte>& buf, std::size_t off, std::uint32_t v) {
    std::memcpy(buf.data() + off, &v, 4);
}

// IEEE 754 float → half-float.
auto floatToHalf(float f) -> std::uint16_t {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23u) & 0xFFu) - 127;
    std::uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent > 15) return static_cast<std::uint16_t>(sign | 0x7C00u); // inf
    if (exponent < -14) {
        // Denorm or zero.
        mantissa = (mantissa | 0x800000u) >> (1 - exponent - 14);
        return static_cast<std::uint16_t>(sign | (mantissa >> 13u));
    }
    return static_cast<std::uint16_t>(sign |
        (static_cast<std::uint32_t>(exponent + 15) << 10u) |
        (mantissa >> 13u));
}

// Build a minimal uncompressed EXR with the given RGBA half-float pixels.
// width and height must be > 0. Pixel data is (width*height) × {R,G,B,A} floats.
auto makeExrUncompressed(int width, int height,
                         const float* rgba) -> std::vector<std::byte> {
    const std::int32_t xMin = 0, yMin = 0;
    const std::int32_t xMax = width - 1, yMax = height - 1;

    // --- Channel list attribute ---
    // chlist: "R"H0 G0 B0 + "G"... + "B"... + "A"... + trailing \0
    // Each channel entry: name\0 + type(1) + pLinear(1) + reserved(3) = nameLen+1+5
    std::vector<std::byte> chList;
    const char* names[] = {"R", "G", "B", "A"};
    for (const char* n : names) {
        const std::size_t nLen = std::strlen(n);
        for (std::size_t i = 0; i <= nLen; ++i)
            chList.push_back(static_cast<std::byte>(n[i]));
        // pixelType: int32 LE = 1 (HALF)
        const std::int32_t pt = 1;
        const auto ptOff = chList.size();
        chList.resize(chList.size() + 4);
        std::memcpy(chList.data() + ptOff, &pt, 4);
        chList.push_back(std::byte{0}); // pLinear
        chList.insert(chList.end(), 3, std::byte{0}); // reserved
    }
    chList.push_back(std::byte{0}); // trailing empty name

    // --- Build header attributes ---
    // Build the whole header as a byte array.
    std::vector<std::byte> header;

    // Magic.
    header.insert(header.end(), {std::byte{0x76}, std::byte{0x2f}, std::byte{0x31}, std::byte{0x01}});
    // Version 2.
    const auto verOff = header.size();
    header.resize(header.size() + 4);
    writeU32(header, verOff, 2);

    // Helper to push an attribute.
    auto pushAttr = [&](const char* name, const char* type,
                        const std::byte* data, std::uint32_t dataLen) {
        std::size_t nameLen = std::strlen(name);
        std::size_t typeLen = std::strlen(type);
        header.insert(header.end(), reinterpret_cast<const std::byte*>(name),
                      reinterpret_cast<const std::byte*>(name) + nameLen + 1);
        header.insert(header.end(), reinterpret_cast<const std::byte*>(type),
                      reinterpret_cast<const std::byte*>(type) + typeLen + 1);
        const auto dl = dataLen;
        const auto dlo = static_cast<std::size_t>(header.size());
        header.resize(header.size() + 4);
        std::memcpy(header.data() + dlo, &dl, 4);
        header.insert(header.end(), data, data + dataLen);
    };

    // compression = 0 (NONE)
    {
        std::byte v = std::byte{0};
        pushAttr("compression", "compression", &v, 1);
    }

    // dataWindow = box2i: xMin, yMin, xMax, yMax
    {
        std::byte buf[16];
        std::memcpy(buf, &xMin, 4);
        std::memcpy(buf + 4, &yMin, 4);
        std::memcpy(buf + 8, &xMax, 4);
        std::memcpy(buf + 12, &yMax, 4);
        pushAttr("dataWindow", "box2i", buf, 16);
    }

    // displayWindow = box2i (same as dataWindow)
    {
        std::byte buf[16];
        std::memcpy(buf, &xMin, 4);
        std::memcpy(buf + 4, &yMin, 4);
        std::memcpy(buf + 8, &xMax, 4);
        std::memcpy(buf + 12, &yMax, 4);
        pushAttr("displayWindow", "box2i", buf, 16);
    }

    // lineOrder = 0 (increasing)
    {
        std::byte v = std::byte{0};
        pushAttr("lineOrder", "lineOrder", &v, 1);
    }

    // pixelAspectRatio = 1.0
    {
        float f = 1.0f;
        std::byte buf[4];
        std::memcpy(buf, &f, 4);
        pushAttr("pixelAspectRatio", "float", buf, 4);
    }

    // screenWindowCenter = float2 (0,0)
    {
        float xy[2] = {0.0f, 0.0f};
        std::byte buf[8];
        std::memcpy(buf, xy, 8);
        pushAttr("screenWindowCenter", "float[2]", buf, 8);
    }

    // screenWindowWidth = 1.0
    {
        float f = 1.0f;
        std::byte buf[4];
        std::memcpy(buf, &f, 4);
        pushAttr("screenWindowWidth", "float", buf, 4);
    }

    // channels = chlist
    pushAttr("channels", "chlist", chList.data(), static_cast<std::uint32_t>(chList.size()));

    // End of header: empty string
    header.push_back(std::byte{0});

    // --- Scanline data ---
    // For each row y: scanline y (int32) + dataSize (int32) + pixel data
    // Pixel data: RGBA half-float interleaved per pixel, left to right.
    const std::size_t pixelBytes = static_cast<std::size_t>(width) * 4 * 2; // 4 channels × half(2 bytes)
    const std::size_t scanlinePayload = pixelBytes;

    std::vector<std::byte> scanlines;
    for (int y = 0; y < height; ++y) {
        // scanline number
        const std::int32_t lineNum = y;
        scanlines.resize(scanlines.size() + 4);
        std::memcpy(scanlines.data() + scanlines.size() - 4, &lineNum, 4);

        // data size
        const auto ds = static_cast<std::int32_t>(scanlinePayload);
        scanlines.resize(scanlines.size() + 4);
        std::memcpy(scanlines.data() + scanlines.size() - 4, &ds, 4);

        // pixel data
        const std::size_t rowStart = scanlines.size();
        scanlines.resize(scanlines.size() + scanlinePayload);
        for (int x = 0; x < width; ++x) {
            const std::size_t px = static_cast<std::size_t>(y * width + x);
            const float* src = rgba + px * 4;
            std::uint16_t halves[4] = {
                floatToHalf(src[0]),
                floatToHalf(src[1]),
                floatToHalf(src[2]),
                floatToHalf(src[3]),
            };
            std::memcpy(scanlines.data() + rowStart + x * 8, halves, 8);
        }
    }

    // Assemble.
    std::vector<std::byte> file;
    file.insert(file.end(), header.begin(), header.end());
    file.insert(file.end(), scanlines.begin(), scanlines.end());
    return file;
}
} // anonymous namespace

TEST_CASE("EXR: 1x1 uncompressed half RGBA") {
    const float pixel[4] = {0.5f, 0.25f, 1.0f, 0.75f};
    auto exr = makeExrUncompressed(1, 1, pixel);
    REQUIRE(exr.size() > 0);

    auto result = loadExr(exr);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA32_FLOAT);
    CHECK(tex.mips[0].width == 1);
    CHECK(tex.mips[0].height == 1);

    const auto* out = reinterpret_cast<const float*>(tex.mips[0].bytes.data());
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(0.01f));
    CHECK(out[1] == doctest::Approx(0.25f).epsilon(0.01f));
    CHECK(out[2] == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(out[3] == doctest::Approx(0.75f).epsilon(0.01f));
}

TEST_CASE("EXR: 2x2 uncompressed half RGBA") {
    float pixels[16] = {
        1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
    };
    auto exr = makeExrUncompressed(2, 2, pixels);
    auto result = loadExr(exr);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.mips[0].width == 2);
    CHECK(tex.mips[0].height == 2);
    CHECK(tex.mips[0].bytes.size() == 2 * 2 * 16);

    const auto* out = reinterpret_cast<const float*>(tex.mips[0].bytes.data());
    // Top-left: red
    CHECK(out[0] == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(out[1] == doctest::Approx(0.0f).epsilon(0.01f));
    // Bottom-right: white
    CHECK(out[12] == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(out[13] == doctest::Approx(1.0f).epsilon(0.01f));
}

TEST_CASE("EXR: HDR values preserved (half-float range)") {
    // Test values > 1.0 which are the main use case for EXR.
    const float pixel[4] = {5.0f, 10.0f, 0.0f, 1.0f};
    auto exr = makeExrUncompressed(1, 1, pixel);
    auto result = loadExr(exr);
    REQUIRE(result.hasValue());
    const auto* out = reinterpret_cast<const float*>(result.value().mips[0].bytes.data());
    CHECK(out[0] == doctest::Approx(5.0f).epsilon(0.1f));
    CHECK(out[1] == doctest::Approx(10.0f).epsilon(0.2f));
}

TEST_CASE("EXR: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadExr(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("EXR: bad magic rejected") {
    std::vector<std::byte> bad = {std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{0}};
    auto result = loadExr(bad);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::BadSignature);
}
