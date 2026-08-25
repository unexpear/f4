#include "core/resources/bmp_loader.h"

#include <cstring>

namespace f4::resources {

namespace {

// BMP file header (14 bytes, packed).
#pragma pack(push, 1)
struct BmpFileHeader {
    std::uint8_t magic[2];
    std::uint32_t fileSize;
    std::uint32_t reserved;
    std::uint32_t pixelDataOffset;
};

// BMP info header (40 bytes BITMAPINFOHEADER, packed).
struct BmpInfoHeader {
    std::uint32_t headerSize;
    std::int32_t width;
    std::int32_t height;
    std::uint16_t planes;
    std::uint16_t bpp;
    std::uint32_t compression;
    std::uint32_t imageSize;
    std::uint32_t xPixelsPerMeter;
    std::uint32_t yPixelsPerMeter;
    std::uint32_t colorsUsed;
    std::uint32_t colorsImportant;
};
#pragma pack(pop)

inline constexpr std::uint32_t kBmpBiRgb = 0;
inline constexpr std::size_t kBmpFileHeaderSize = 14;
inline constexpr std::size_t kBmpInfoHeaderSize = 40;

} // anonymous namespace

auto loadBmp(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    if (bytes.size() < kBmpFileHeaderSize + kBmpInfoHeaderSize) {
        return LoadError::Truncated;
    }

    BmpFileHeader fileHeader{};
    std::memcpy(&fileHeader, bytes.data(), kBmpFileHeaderSize);

    if (fileHeader.magic[0] != 'B' || fileHeader.magic[1] != 'M') {
        return LoadError::BadSignature;
    }

    BmpInfoHeader infoHeader{};
    std::memcpy(&infoHeader, bytes.data() + kBmpFileHeaderSize, kBmpInfoHeaderSize);

    if (infoHeader.compression != kBmpBiRgb) {
        return LoadError::UnsupportedCompression;
    }
    if (infoHeader.bpp != 24 && infoHeader.bpp != 32) {
        return LoadError::UnsupportedBpp;
    }
    if (infoHeader.planes != 1) {
        return LoadError::UnsupportedBpp;
    }

    const bool topDown = infoHeader.height < 0;
    const std::int32_t absHeight = topDown ? -infoHeader.height : infoHeader.height;
    const std::int32_t width = infoHeader.width;

    if (width <= 0 || absHeight <= 0) {
        return LoadError::BadDimensions;
    }

    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(absHeight);

    // Row stride padded to 4-byte boundary.
    const std::uint32_t bitsPerPixel = infoHeader.bpp;
    const std::uint32_t rowBytesRaw = (w * bitsPerPixel + 7) / 8;
    const std::uint32_t rowStride = (rowBytesRaw + 3u) & ~3u;

    // Verify pixel data region is within bounds.
    const std::size_t pixelRegionStart = fileHeader.pixelDataOffset;
    const std::size_t pixelRegionSize = static_cast<std::size_t>(rowStride) * h;
    if (pixelRegionStart + pixelRegionSize > bytes.size()) {
        return LoadError::Truncated;
    }

    Texture tex;
    tex.format = TextureFormat::RGBA8_UNORM;
    tex.mips.resize(1);
    tex.mips[0].width = w;
    tex.mips[0].height = h;
    tex.mips[0].bytes.resize(static_cast<std::size_t>(w) * h * 4);

    const std::byte* pixels = bytes.data() + pixelRegionStart;
    std::byte* dst = tex.mips[0].bytes.data();

    for (std::uint32_t row = 0; row < h; ++row) {
        // BMP stores rows bottom-up by default; flip when topDown is false.
        const std::uint32_t srcRow = topDown ? row : (h - 1 - row);
        const std::byte* srcRowPtr = pixels + static_cast<std::size_t>(srcRow) * rowStride;

        for (std::uint32_t col = 0; col < w; ++col) {
            const std::byte* px = srcRowPtr + static_cast<std::size_t>(col) * (bitsPerPixel / 8);

            if (bitsPerPixel == 24) {
                // BGR → RGBA
                const std::size_t dstIdx = (static_cast<std::size_t>(row) * w + col) * 4;
                dst[dstIdx + 0] = px[2]; // R
                dst[dstIdx + 1] = px[1]; // G
                dst[dstIdx + 2] = px[0]; // B
                dst[dstIdx + 3] = std::byte{0xFF};
            } else {
                // BGRA → RGBA
                const std::size_t dstIdx = (static_cast<std::size_t>(row) * w + col) * 4;
                dst[dstIdx + 0] = px[2]; // R
                dst[dstIdx + 1] = px[1]; // G
                dst[dstIdx + 2] = px[0]; // B
                dst[dstIdx + 3] = px[3]; // A
            }
        }
    }

    return static_cast<Texture&&>(tex);
}

} // namespace f4::resources
