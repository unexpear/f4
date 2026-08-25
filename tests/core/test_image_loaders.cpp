#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include <stb_image_write.h>

#include "core/resources/bmp_loader.h"
#include "core/resources/stb_image_loader.h"
#include "core/resources/tga_loader.h"

using namespace f4::resources;

namespace {

// Helpers for building synthetic BMP/TGA files in a std::vector<std::byte>.
struct BmpBuilder {
    std::uint32_t width = 2;
    std::uint32_t height = 2;
    std::uint16_t bpp = 24;
    bool topDown = false;
    std::vector<std::byte> pixels; // BGR or BGRA, row by row

    [[nodiscard]] auto build() const -> std::vector<std::byte> {
        const std::uint32_t bytesPerPixel = bpp / 8;
        const std::uint32_t rowBytesRaw = width * bytesPerPixel;
        const std::uint32_t rowStride = (rowBytesRaw + 3u) & ~3u;
        const std::uint32_t pixelDataSize = rowStride * height;
        const std::uint32_t fileSize = 14 + 40 + pixelDataSize;

        std::vector<std::byte> buf(fileSize, std::byte{0});

        // File header.
        buf[0] = std::byte{'B'};
        buf[1] = std::byte{'M'};
        std::memcpy(buf.data() + 2, &fileSize, 4);
        const std::uint32_t pixelOffset = 14 + 40;
        std::memcpy(buf.data() + 10, &pixelOffset, 4);

        // Info header.
        const std::uint32_t infoSize = 40;
        std::memcpy(buf.data() + 14, &infoSize, 4);
        const std::int32_t h = topDown ? -static_cast<std::int32_t>(height)
                                       : static_cast<std::int32_t>(height);
        std::memcpy(buf.data() + 18, &width, 4);
        std::memcpy(buf.data() + 22, &h, 4);
        const std::uint16_t planes = 1;
        std::memcpy(buf.data() + 26, &planes, 2);
        std::memcpy(buf.data() + 28, &bpp, 2);
        // compression = BI_RGB (0), already zero.

        // Pixel data — write each row with proper row-padding.
        std::byte* dst = buf.data() + pixelOffset;
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(pixels.data());
        const std::size_t rowBytesRaw2 = static_cast<std::size_t>(width) * bytesPerPixel;
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(dst, src, rowBytesRaw2);
            src += rowBytesRaw2;
            dst += rowStride;
        }
        return buf;
    }
};

struct TgaBuilder {
    std::uint16_t width = 2;
    std::uint16_t height = 2;
    std::uint8_t bpp = 24;
    bool topDown = true;
    bool rle = false;
    std::vector<std::byte> pixels; // BGR or BGRA, row by row

    [[nodiscard]] auto build() const -> std::vector<std::byte> {
        const std::uint8_t imageType = rle ? 10 : 2;
        const std::uint8_t desc = topDown ? 0x20 : 0x00;

        std::vector<std::byte> buf;
        buf.resize(18);

        buf[0] = std::byte{0}; // ID length
        buf[1] = std::byte{0}; // no color map
        buf[2] = std::byte{static_cast<std::uint8_t>(imageType)};
        // color map spec: 5 zero bytes
        buf[3] = buf[4] = buf[5] = buf[6] = buf[7] = std::byte{0};
        const std::uint16_t xOrigin = 0;
        const std::uint16_t yOrigin = 0;
        std::memcpy(buf.data() + 8, &xOrigin, 2);
        std::memcpy(buf.data() + 10, &yOrigin, 2);
        std::memcpy(buf.data() + 12, &width, 2);
        std::memcpy(buf.data() + 14, &height, 2);
        buf[16] = std::byte{bpp};
        buf[17] = std::byte{desc};

        if (rle) {
            // Encode as RLE: one run packet per row.
            const std::uint32_t bytesPerPixel = bpp / 8;
            for (std::uint16_t row = 0; row < height; ++row) {
                // RLE run: count-1 with bit7 set, then one pixel.
                buf.push_back(static_cast<std::byte>(0x80u | static_cast<std::uint8_t>(width - 1)));
                const std::byte* rowStart =
                    reinterpret_cast<const std::byte*>(pixels.data()) +
                    static_cast<std::size_t>(row) * bytesPerPixel;
                for (std::uint32_t b = 0; b < bytesPerPixel; ++b) {
                    buf.push_back(rowStart[b]);
                }
            }
        } else {
            buf.insert(buf.end(), pixels.begin(), pixels.end());
        }
        return buf;
    }
};

} // anonymous namespace

// ============================================================================
// BMP Tests
// ============================================================================

TEST_CASE("BMP: 24bpp bottom-up 2x2") {
    // 2x2 image, rows stored bottom-to-top (standard BMP).
    // Pixel layout in file (bottom row first):
    //   Row 1 (bottom): red(255,0,0), green(0,255,0)
    //   Row 0 (top):    blue(0,0,255), white(255,255,255)
    //
    // After BMP decode (flipped to top-down):
    //   Row 0: blue, white
    //   Row 1: red, green

    BmpBuilder b;
    b.width = 2;
    b.height = 2;
    b.bpp = 24;
    b.topDown = false; // bottom-up
    // Pixels in file order: row1(bottom), row0(top)
    b.pixels = {
        // Row 1 (bottom): red, green
        std::byte{0}, std::byte{0}, std::byte{255},       // red BGR
        std::byte{0}, std::byte{255}, std::byte{0},       // green BGR
        // Row 0 (top): blue, white
        std::byte{255}, std::byte{0}, std::byte{0},       // blue BGR
        std::byte{255}, std::byte{255}, std::byte{255},   // white BGR
    };
    auto file = b.build();
    auto result = loadBmp(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    CHECK(tex.mips[0].width == 2);
    CHECK(tex.mips[0].height == 2);

    const auto& px = tex.mips[0].bytes;
    // Row 0, pixel 0: blue → RGBA(0,0,255,255)
    CHECK(px[0] == std::byte{0});
    CHECK(px[1] == std::byte{0});
    CHECK(px[2] == std::byte{255});
    CHECK(px[3] == std::byte{255});
    // Row 0, pixel 1: white → RGBA(255,255,255,255)
    CHECK(px[4] == std::byte{255});
    CHECK(px[5] == std::byte{255});
    CHECK(px[6] == std::byte{255});
    CHECK(px[7] == std::byte{255});
    // Row 1, pixel 0: red → RGBA(255,0,0,255)
    CHECK(px[8] == std::byte{255});
    CHECK(px[9] == std::byte{0});
    CHECK(px[10] == std::byte{0});
    CHECK(px[11] == std::byte{255});
    // Row 1, pixel 1: green → RGBA(0,255,0,255)
    CHECK(px[12] == std::byte{0});
    CHECK(px[13] == std::byte{255});
    CHECK(px[14] == std::byte{0});
    CHECK(px[15] == std::byte{255});
}

TEST_CASE("BMP: 24bpp top-down") {
    BmpBuilder b;
    b.width = 1;
    b.height = 2;
    b.bpp = 24;
    b.topDown = true;
    // Row 0: red, Row 1: blue
    b.pixels = {
        std::byte{0}, std::byte{0}, std::byte{255},     // red
        std::byte{255}, std::byte{0}, std::byte{0},     // blue
    };
    auto file = b.build();
    auto result = loadBmp(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    // Row 0: red
    CHECK(px[0] == std::byte{255});
    CHECK(px[1] == std::byte{0});
    CHECK(px[2] == std::byte{0});
    CHECK(px[3] == std::byte{255});
    // Row 1: blue
    CHECK(px[4] == std::byte{0});
    CHECK(px[5] == std::byte{0});
    CHECK(px[6] == std::byte{255});
    CHECK(px[7] == std::byte{255});
}

TEST_CASE("BMP: 32bpp bottom-up") {
    BmpBuilder b;
    b.width = 1;
    b.height = 1;
    b.bpp = 32;
    b.topDown = false;
    b.pixels = {
        std::byte{0x80}, std::byte{0x40}, std::byte{0x20}, std::byte{0xAA},
    };
    auto file = b.build();
    auto result = loadBmp(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    // BGRA→RGBA: R=0x20, G=0x40, B=0x80, A=0xAA
    CHECK(px[0] == std::byte{0x20});
    CHECK(px[1] == std::byte{0x40});
    CHECK(px[2] == std::byte{0x80});
    CHECK(px[3] == std::byte{0xAA});
}

TEST_CASE("BMP: bad signature") {
    std::vector<std::byte> buf(60, std::byte{0});
    buf[0] = std::byte{'X'};
    buf[1] = std::byte{'M'};
    auto result = loadBmp(buf);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::BadSignature);
}

TEST_CASE("BMP: truncated header") {
    std::vector<std::byte> buf(10, std::byte{0});
    auto result = loadBmp(buf);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("BMP: RLE compressed rejected") {
    BmpBuilder b;
    b.width = 2;
    b.height = 1;
    b.bpp = 24;
    b.pixels = {std::byte{0}, std::byte{0}, std::byte{255},
                std::byte{0}, std::byte{255}, std::byte{0}};
    auto file = b.build();
    // Patch compression field to BI_RLE8 = 1.
    file[30] = std::byte{1};
    auto result = loadBmp(file);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::UnsupportedCompression);
}

TEST_CASE("BMP: 16bpp rejected") {
    BmpBuilder b;
    b.width = 1;
    b.height = 1;
    b.bpp = 16;
    b.pixels = {std::byte{0}, std::byte{0}};
    auto file = b.build();
    auto result = loadBmp(file);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::UnsupportedBpp);
}

TEST_CASE("BMP: zero dimensions rejected") {
    BmpBuilder b;
    b.width = 0;
    b.height = 1;
    b.pixels = {};
    auto file = b.build();
    auto result = loadBmp(file);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::BadDimensions);
}

// ============================================================================
// TGA Tests
// ============================================================================

TEST_CASE("TGA: 24bpp uncompressed top-down") {
    TgaBuilder t;
    t.width = 2;
    t.height = 2;
    t.bpp = 24;
    t.topDown = true;
    t.rle = false;
    // Row 0: red, green
    // Row 1: blue, white
    t.pixels = {
        std::byte{0}, std::byte{0}, std::byte{255},       // red BGR
        std::byte{0}, std::byte{255}, std::byte{0},       // green BGR
        std::byte{255}, std::byte{0}, std::byte{0},       // blue BGR
        std::byte{255}, std::byte{255}, std::byte{255},   // white BGR
    };
    auto file = t.build();
    auto result = loadTga(file);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    CHECK(tex.mips[0].width == 2);
    CHECK(tex.mips[0].height == 2);

    const auto& px = tex.mips[0].bytes;
    // Row 0, pixel 0: red → RGBA(255,0,0,255)
    CHECK(px[0] == std::byte{255});
    CHECK(px[1] == std::byte{0});
    CHECK(px[2] == std::byte{0});
    CHECK(px[3] == std::byte{255});
    // Row 0, pixel 1: green
    CHECK(px[4] == std::byte{0});
    CHECK(px[5] == std::byte{255});
    CHECK(px[6] == std::byte{0});
    CHECK(px[7] == std::byte{255});
    // Row 1, pixel 0: blue
    CHECK(px[8] == std::byte{0});
    CHECK(px[9] == std::byte{0});
    CHECK(px[10] == std::byte{255});
    CHECK(px[11] == std::byte{255});
    // Row 1, pixel 1: white
    CHECK(px[12] == std::byte{255});
    CHECK(px[13] == std::byte{255});
    CHECK(px[14] == std::byte{255});
    CHECK(px[15] == std::byte{255});
}

TEST_CASE("TGA: 24bpp uncompressed bottom-up") {
    TgaBuilder t;
    t.width = 2;
    t.height = 2;
    t.bpp = 24;
    t.topDown = false;
    t.rle = false;
    // File order: row0(bottom), row1(top) — after flip: row0=top, row1=bottom
    t.pixels = {
        std::byte{0}, std::byte{0}, std::byte{255},       // red BGR (bottom)
        std::byte{0}, std::byte{255}, std::byte{0},       // green BGR (bottom)
        std::byte{255}, std::byte{0}, std::byte{0},       // blue BGR (top)
        std::byte{255}, std::byte{255}, std::byte{255},   // white BGR (top)
    };
    auto file = t.build();
    auto result = loadTga(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    // After flip: row 0 = top = blue, white
    CHECK(px[0] == std::byte{0});
    CHECK(px[1] == std::byte{0});
    CHECK(px[2] == std::byte{255});
    CHECK(px[3] == std::byte{255});
    CHECK(px[4] == std::byte{255});
    CHECK(px[5] == std::byte{255});
    CHECK(px[6] == std::byte{255});
    CHECK(px[7] == std::byte{255});
    // row 1 = bottom = red, green
    CHECK(px[8] == std::byte{255});
    CHECK(px[9] == std::byte{0});
    CHECK(px[10] == std::byte{0});
    CHECK(px[11] == std::byte{255});
    CHECK(px[12] == std::byte{0});
    CHECK(px[13] == std::byte{255});
    CHECK(px[14] == std::byte{0});
    CHECK(px[15] == std::byte{255});
}

TEST_CASE("TGA: 32bpp uncompressed top-down") {
    TgaBuilder t;
    t.width = 1;
    t.height = 1;
    t.bpp = 32;
    t.topDown = true;
    t.pixels = {
        std::byte{0x80}, std::byte{0x40}, std::byte{0x20}, std::byte{0xCC},
    };
    auto file = t.build();
    auto result = loadTga(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    // BGRA→RGBA: R=0x20, G=0x40, B=0x80, A=0xCC
    CHECK(px[0] == std::byte{0x20});
    CHECK(px[1] == std::byte{0x40});
    CHECK(px[2] == std::byte{0x80});
    CHECK(px[3] == std::byte{0xCC});
}

TEST_CASE("TGA: 16bpp BGR555 top-down") {
    TgaBuilder t;
    t.width = 1;
    t.height = 1;
    t.bpp = 16;
    t.topDown = true;
    // BGR555: bit15=alpha, 14-10=R, 9-5=G, 4-0=B
    // Pure red: R=31, G=0, B=0, alpha=1 → 1_11111_00000_00000 = 0xFC00 → LE {0x00, 0xFC}
    t.pixels = {std::byte{0x00}, std::byte{0xFC}};
    auto file = t.build();
    auto result = loadTga(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    CHECK(px[0] == std::byte{255}); // R
    CHECK(px[1] == std::byte{0});   // G
    CHECK(px[2] == std::byte{0});   // B
    CHECK(px[3] == std::byte{255}); // A (bit15 set)
}

TEST_CASE("TGA: 24bpp RLE top-down") {
    // Single row: 3 red pixels encoded as one RLE run.
    TgaBuilder t;
    t.width = 3;
    t.height = 1;
    t.bpp = 24;
    t.topDown = true;
    t.rle = true;
    t.pixels = {
        std::byte{0}, std::byte{0}, std::byte{255},  // red
    };
    auto file = t.build();
    auto result = loadTga(file);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    for (int i = 0; i < 3; ++i) {
        CHECK(px[i * 4 + 0] == std::byte{255}); // R
        CHECK(px[i * 4 + 1] == std::byte{0});   // G
        CHECK(px[i * 4 + 2] == std::byte{0});   // B
        CHECK(px[i * 4 + 3] == std::byte{255}); // A
    }
}

TEST_CASE("TGA: 24bpp RLE mixed packets") {
    // Row: 2 raw pixels + 2 RLE pixels = 4 wide.
    // Encode: [raw header=0x01, BGR(red), BGR(green)], [rle header=0x81, BGR(blue)]
    std::vector<std::byte> buf;
    buf.resize(18);
    buf[0] = std::byte{0};  // ID length
    buf[1] = std::byte{0};  // no color map
    buf[2] = std::byte{10}; // RLE true-color
    buf[3] = buf[4] = buf[5] = buf[6] = buf[7] = std::byte{0};
    const std::uint16_t w = 4;
    const std::uint16_t h = 1;
    const std::uint16_t xO = 0;
    const std::uint16_t yO = 0;
    std::memcpy(buf.data() + 8, &xO, 2);
    std::memcpy(buf.data() + 10, &yO, 2);
    std::memcpy(buf.data() + 12, &w, 2);
    std::memcpy(buf.data() + 14, &h, 2);
    buf[16] = std::byte{24};
    buf[17] = std::byte{0x20}; // top-down

    // Raw packet: header 0x01 (count=2), then 2 BGR pixels
    buf.push_back(std::byte{0x01});
    buf.push_back(std::byte{0}); buf.push_back(std::byte{0}); buf.push_back(std::byte{255}); // red
    buf.push_back(std::byte{0}); buf.push_back(std::byte{255}); buf.push_back(std::byte{0}); // green

    // RLE packet: header 0x81 (count=2), then 1 BGR pixel
    buf.push_back(std::byte{0x81});
    buf.push_back(std::byte{255}); buf.push_back(std::byte{0}); buf.push_back(std::byte{0}); // blue

    auto result = loadTga(buf);
    REQUIRE(result.hasValue());
    const auto& px = result.value().mips[0].bytes;
    // Pixel 0: red
    CHECK(px[0] == std::byte{255});
    CHECK(px[4] == std::byte{0});
    // Pixel 1: green
    CHECK(px[5] == std::byte{255});
    CHECK(px[6] == std::byte{0});
    // Pixel 2: blue (RLE repeated)
    CHECK(px[8] == std::byte{0});
    CHECK(px[10] == std::byte{255});
    // Pixel 3: blue (RLE repeated)
    CHECK(px[12] == std::byte{0});
    CHECK(px[14] == std::byte{255});
}

TEST_CASE("TGA: color map rejected") {
    std::vector<std::byte> buf(18, std::byte{0});
    buf[2] = std::byte{1}; // indexed image type
    buf[12] = std::byte{24};
    auto result = loadTga(buf);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::UnsupportedCompression);
}

TEST_CASE("TGA: truncated header") {
    std::vector<std::byte> buf(10, std::byte{0});
    auto result = loadTga(buf);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("TGA: 8bpp rejected") {
    TgaBuilder t;
    t.width = 1;
    t.height = 1;
    t.bpp = 8;
    t.pixels = {std::byte{0}};
    auto file = t.build();
    auto result = loadTga(file);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::UnsupportedBpp);
}

// ============================================================================
// stb_image loader tests (PNG, JPG via stb_image)
// ============================================================================

namespace {
void stbPngWrite(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::byte>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (int i = 0; i < size; ++i) {
        out->push_back(static_cast<std::byte>(bytes[i]));
    }
}

auto makePng(int w, int h, const unsigned char* rgba) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    stbi_write_png_to_func(stbPngWrite, &result, w, h, 4, rgba, w * 4);
    return result;
}
} // anonymous namespace

TEST_CASE("stb_image: 1x1 red PNG") {
    unsigned char pixel[4] = {255, 0, 0, 255};
    auto png = makePng(1, 1, pixel);
    REQUIRE(png.size() > 0);

    auto result = loadStbImage(png);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.format == TextureFormat::RGBA8_UNORM);
    CHECK(tex.mips[0].width == 1);
    CHECK(tex.mips[0].height == 1);
    CHECK(tex.mips[0].bytes[0] == std::byte{255}); // R
    CHECK(tex.mips[0].bytes[1] == std::byte{0});   // G
    CHECK(tex.mips[0].bytes[2] == std::byte{0});   // B
    CHECK(tex.mips[0].bytes[3] == std::byte{255}); // A
}

TEST_CASE("stb_image: 2x2 blue+green+red+white PNG") {
    unsigned char pixels[16] = {
        0, 0, 255, 255,  0, 255, 0, 255,
        255, 0, 0, 255,  255, 255, 255, 255,
    };
    auto png = makePng(2, 2, pixels);
    REQUIRE(png.size() > 0);

    auto result = loadStbImage(png);
    REQUIRE(result.hasValue());
    const auto& tex = result.value();
    CHECK(tex.mips[0].width == 2);
    CHECK(tex.mips[0].height == 2);
    const auto& px = tex.mips[0].bytes;
    CHECK(px[0] == std::byte{0});
    CHECK(px[1] == std::byte{0});
    CHECK(px[2] == std::byte{255});
    CHECK(px[3] == std::byte{255});
    CHECK(px[12] == std::byte{255});
    CHECK(px[13] == std::byte{255});
    CHECK(px[14] == std::byte{255});
    CHECK(px[15] == std::byte{255});
}

TEST_CASE("stb_image: truncated input rejected") {
    std::vector<std::byte> garbage = {std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}};
    auto result = loadStbImage(garbage);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("stb_image: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadStbImage(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("stb_image_hdr: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadStbImageHdr(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}
