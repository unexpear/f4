#include "core/resources/tga_loader.h"

#include <cstring>

namespace f4::resources {

namespace {

// TGA header: 18 bytes, packed.
#pragma pack(push, 1)
struct TgaHeader {
    std::uint8_t idLength;
    std::uint8_t colorMapType;
    std::uint8_t imageType;
    std::uint8_t colorMapSpec[5];
    std::uint16_t xOrigin;
    std::uint16_t yOrigin;
    std::uint16_t width;
    std::uint16_t height;
    std::uint8_t bpp;
    std::uint8_t imageDescriptor;
};
#pragma pack(pop)

inline constexpr std::size_t kTgaHeaderSize = 18;
inline constexpr std::uint8_t kTgaTypeTrueColor = 2;
inline constexpr std::uint8_t kTgaTypeRleTrueColor = 10;

// Convert one BGR[A] pixel to RGBA8 and write to dst.
void convertPixel(const std::byte* src, std::byte* dst, std::uint32_t bpp) {
    if (bpp == 24) {
        dst[0] = src[2]; // R
        dst[1] = src[1]; // G
        dst[2] = src[0]; // B
        dst[3] = std::byte{0xFF};
    } else if (bpp == 32) {
        dst[0] = src[2]; // R
        dst[1] = src[1]; // G
        dst[2] = src[0]; // B
        dst[3] = src[3]; // A
    } else {
        // 16bpp BGR555: bit15=alpha, 14-10=R, 9-5=G, 4-0=B
        const std::uint16_t px =
            static_cast<std::uint16_t>(src[0]) |
            (static_cast<std::uint16_t>(src[1]) << 8);
        const auto r = static_cast<std::uint8_t>((px >> 10) & 0x1Fu);
        const auto g = static_cast<std::uint8_t>((px >> 5) & 0x1Fu);
        const auto b = static_cast<std::uint8_t>(px & 0x1Fu);
        const std::uint8_t a = (px & 0x8000u) ? 0xFFu : 0x00u;
        dst[0] = static_cast<std::byte>((r << 3) | (r >> 2));
        dst[1] = static_cast<std::byte>((g << 3) | (g >> 2));
        dst[2] = static_cast<std::byte>((b << 3) | (b >> 2));
        dst[3] = static_cast<std::byte>(a);
    }
}

} // anonymous namespace

auto loadTga(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    if (bytes.size() < kTgaHeaderSize) {
        return LoadError::Truncated;
    }

    TgaHeader header{};
    std::memcpy(&header, bytes.data(), kTgaHeaderSize);

    if (header.colorMapType != 0) {
        return LoadError::UnsupportedCompression;
    }

    if (header.imageType != kTgaTypeTrueColor &&
        header.imageType != kTgaTypeRleTrueColor) {
        return LoadError::UnsupportedCompression;
    }

    if (header.bpp != 16 && header.bpp != 24 && header.bpp != 32) {
        return LoadError::UnsupportedBpp;
    }

    if (header.width == 0 || header.height == 0) {
        return LoadError::BadDimensions;
    }

    const std::uint32_t w = header.width;
    const std::uint32_t h = header.height;
    const std::uint32_t bpp = header.bpp;
    const std::uint32_t bytesPerPixel = bpp / 8;
    const bool rle = (header.imageType == kTgaTypeRleTrueColor);

    // Bit 5 of image descriptor: 0 = bottom-up (default), 1 = top-down.
    const bool topDown = (header.imageDescriptor & 0x20u) != 0;

    // Stream starts after header + optional image ID.
    const std::size_t streamStart = kTgaHeaderSize + header.idLength;
    if (streamStart > bytes.size()) {
        return LoadError::Truncated;
    }

    const std::byte* src = bytes.data() + streamStart;
    const std::byte* srcEnd = bytes.data() + bytes.size();

    Texture tex;
    tex.format = TextureFormat::RGBA8_UNORM;
    tex.mips.resize(1);
    tex.mips[0].width = w;
    tex.mips[0].height = h;
    tex.mips[0].bytes.resize(static_cast<std::size_t>(w) * h * 4);

    std::byte* dstBase = tex.mips[0].bytes.data();
    const std::size_t rowBytes = static_cast<std::size_t>(w) * 4;

    if (!rle) {
        // Type 2: uncompressed true-color. Raw BGR[A] pixels, no packet headers.
        const std::size_t totalPixelBytes = static_cast<std::size_t>(w) * h * bytesPerPixel;
        if (src + totalPixelBytes > srcEnd) {
            return LoadError::Truncated;
        }

        for (std::uint32_t row = 0; row < h; ++row) {
            const std::uint32_t dstRow = topDown ? row : (h - 1 - row);
            std::byte* rowDst = dstBase + static_cast<std::size_t>(dstRow) * rowBytes;

            for (std::uint32_t col = 0; col < w; ++col) {
                convertPixel(src, rowDst + col * 4, bpp);
                src += bytesPerPixel;
            }
        }
    } else {
        // Type 10: RLE true-color. Packet-encoded.
        Array<std::byte> rowBuf;
        rowBuf.resize(static_cast<std::size_t>(w) * 4);

        for (std::uint32_t row = 0; row < h; ++row) {
            const std::uint32_t dstRow = topDown ? row : (h - 1 - row);
            std::byte* rowDst = rowBuf.data();

            std::uint32_t pixelsDecoded = 0;
            while (pixelsDecoded < w) {
                if (src >= srcEnd) {
                    return LoadError::Truncated;
                }

                const std::uint8_t pktHeader = static_cast<std::uint8_t>(*src);
                const bool isRun = (pktHeader & 0x80u) != 0;
                const std::uint32_t count =
                    static_cast<std::uint32_t>((pktHeader & 0x7Fu) + 1u);

                if (pixelsDecoded + count > w) {
                    return LoadError::Truncated;
                }

                if (isRun) {
                    // RLE run: one pixel repeated count times.
                    if (src + 1 + bytesPerPixel > srcEnd) {
                        return LoadError::Truncated;
                    }
                    ++src;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        convertPixel(src, rowDst, bpp);
                        rowDst += 4;
                    }
                    src += bytesPerPixel;
                } else {
                    // Raw packet: count pixels following.
                    const std::size_t rawBytes =
                        static_cast<std::size_t>(count) * bytesPerPixel;
                    if (src + 1 + rawBytes > srcEnd) {
                        return LoadError::Truncated;
                    }
                    ++src;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        convertPixel(src, rowDst, bpp);
                        rowDst += 4;
                        src += bytesPerPixel;
                    }
                }
                pixelsDecoded += count;
            }

            // Copy decoded row to final position.
            std::memcpy(dstBase + static_cast<std::size_t>(dstRow) * rowBytes,
                         rowBuf.data(), rowBytes);
        }
    }

    return static_cast<Texture&&>(tex);
}

} // namespace f4::resources
