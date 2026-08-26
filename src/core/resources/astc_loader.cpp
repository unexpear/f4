#include "core/resources/astc_loader.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <atomic>

// basisu_astc_helpers.h is compiled inside basisu_transcoder_impl.cpp.
// We include just the header for declarations; the implementation is already linked.
#include <basisu_transcoder.h>
#include <basisu_astc_helpers.h>

namespace f4::resources {

// ── Ensure ASTC dequant tables are initialized once ─────────────────────────

static void ensureAstcTablesInit() {
    static std::atomic<bool> s_inited{false};
    bool expected = false;
    if (s_inited.compare_exchange_strong(expected, true)) {
        astc_helpers::init_tables();
    }
}

// ── ASTC constants ──────────────────────────────────────────────────────────

static constexpr std::uint8_t kAstcMagic[4] = {0x13, 0xAB, 0xA1, 0x5C};
static constexpr std::size_t kAstcHeaderSize = 16;

// The 14 standard 2D block sizes.
struct AstcBlockSize { std::uint8_t w, h; };

static constexpr AstcBlockSize kAstcBlockSizes[14] = {
    {4, 4}, {5, 4}, {5, 5}, {6, 5},
    {6, 6}, {8, 5}, {8, 6}, {10, 5},
    {10, 6}, {8, 8}, {10, 8}, {10, 10},
    {12, 10}, {12, 12},
};

// ── Little-endian readers ───────────────────────────────────────────────────

[[nodiscard]] static auto readU24LE(const std::uint8_t* p) -> std::uint32_t {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16);
}

// ── TextureFormat lookup from block size + HDR flag ─────────────────────────

[[nodiscard]] static auto astcFormatFromBlockSize(
    std::uint8_t bw, std::uint8_t bh, bool hdr)
    -> std::optional<TextureFormat>
{
    for (int i = 0; i < 14; ++i) {
        if (kAstcBlockSizes[i].w == bw && kAstcBlockSizes[i].h == bh) {
            return hdr
                ? static_cast<TextureFormat>(static_cast<std::uint8_t>(TextureFormat::ASTC_4X4_HFLOAT) + i)
                : static_cast<TextureFormat>(static_cast<std::uint8_t>(TextureFormat::ASTC_4X4_UNORM) + i);
        }
    }
    return std::nullopt;
}

// ── Decode a single block to raster position ────────────────────────────────

static void decodeBlockLdr(
    const std::uint8_t* blockData, std::uint8_t* raster,
    std::uint32_t imgW, std::uint32_t imgH,
    std::uint32_t bx, std::uint32_t by,
    std::uint32_t bw, std::uint32_t bh, std::size_t bytesPerPixel)
{
    std::uint8_t blockPixels[16 * 16 * 4]; // max block is 12×12 = 144 pixels, but 16×16 is safe
    basisu_astc::astc::decompress_ldr(blockPixels, blockData, false,
        static_cast<int>(bw), static_cast<int>(bh));

    const std::uint32_t startX = bx * bw;
    const std::uint32_t startY = by * bh;
    const std::uint32_t endX = std::min(startX + bw, imgW);
    const std::uint32_t endY = std::min(startY + bh, imgH);

    for (std::uint32_t y = startY; y < endY; ++y) {
        const std::uint32_t localY = y - startY;
        for (std::uint32_t x = startX; x < endX; ++x) {
            const std::uint32_t localX = x - startX;
            const std::size_t srcOff = (static_cast<std::size_t>(localY) * bw + localX) * 4;
            const std::size_t dstOff = (static_cast<std::size_t>(y) * imgW + x) * bytesPerPixel;
            std::memcpy(raster + dstOff, blockPixels + srcOff, bytesPerPixel);
        }
    }
}

static void decodeBlockHdr(
    const std::uint8_t* blockData, std::uint8_t* raster,
    std::uint32_t imgW, std::uint32_t imgH,
    std::uint32_t bx, std::uint32_t by,
    std::uint32_t bw, std::uint32_t bh)
{
    float blockPixels[16 * 16 * 4]; // max block 12×12, 4 floats each
    basisu_astc::astc::decompress_hdr(blockPixels, blockData,
        static_cast<int>(bw), static_cast<int>(bh));

    const std::uint32_t startX = bx * bw;
    const std::uint32_t startY = by * bh;
    const std::uint32_t endX = std::min(startX + bw, imgW);
    const std::uint32_t endY = std::min(startY + bh, imgH);

    for (std::uint32_t y = startY; y < endY; ++y) {
        const std::uint32_t localY = y - startY;
        for (std::uint32_t x = startX; x < endX; ++x) {
            const std::uint32_t localX = x - startX;
            const std::size_t srcOff = (static_cast<std::size_t>(localY) * bw + localX) * 4;
            const std::size_t dstOff = (static_cast<std::size_t>(y) * imgW + x) * 4 * sizeof(float);
            std::memcpy(raster + dstOff, blockPixels + srcOff * sizeof(float), 4 * sizeof(float));
        }
    }
}

// ── Public entry point ──────────────────────────────────────────────────────

auto loadAstc(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    ensureAstcTablesInit();

    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto dataSize = bytes.size();

    if (dataSize < kAstcHeaderSize) {
        return LoadError::Truncated;
    }

    if (std::memcmp(data, kAstcMagic, 4) != 0) {
        return LoadError::BadSignature;
    }

    const auto blockW = data[4];
    const auto blockH = data[5];
    const auto blockZ = data[6];
    const auto dimX = readU24LE(data + 7);
    const auto dimY = readU24LE(data + 10);
    const auto dimZ = readU24LE(data + 13);

    if (blockZ != 1 || dimZ != 1) {
        return LoadError::UnsupportedCompression; // 3D / array not supported
    }
    if (dimX == 0 || dimY == 0) {
        return LoadError::BadDimensions;
    }

    // Determine HDR from block size.  We try LDR first; the actual format
    // is resolved by decompression: if all blocks decode as LDR, use UNORM;
    // if any is HDR, use HFLOAT.  For simplicity in v1 we treat the file as
    // LDR unless a probe block reports HDR.
    bool hdr = false;
    {
        bool probeHdr = false;
        // Probe the first block to detect HDR.
        const std::size_t expectedBlocks =
            static_cast<std::size_t>((dimX + blockW - 1) / blockW)
            * ((dimY + blockH - 1) / blockH);
        const std::size_t headerPlusBlocks = kAstcHeaderSize + expectedBlocks * 16;
        if (headerPlusBlocks <= dataSize) {
            basisu_astc::astc::is_hdr(data + kAstcHeaderSize,
                static_cast<int>(blockW), static_cast<int>(blockH), probeHdr);
        }
        hdr = probeHdr;
    }

    auto fmtOpt = astcFormatFromBlockSize(blockW, blockH, hdr);
    if (!fmtOpt) return LoadError::UnsupportedBpp;
    auto fmt = *fmtOpt;

    const std::size_t imgW = dimX;
    const std::size_t imgH = dimY;
    const std::size_t gridW = (imgW + blockW - 1) / blockW;
    const std::size_t gridH = (imgH + blockH - 1) / blockH;
    const std::size_t totalBlocks = gridW * gridH;
    const std::size_t dataOffset = kAstcHeaderSize;
    const std::size_t dataRequired = dataOffset + totalBlocks * 16;

    if (dataRequired > dataSize) {
        return LoadError::Truncated;
    }

    const std::size_t bpp = hdr ? (4 * sizeof(float)) : 4;
    const std::size_t rasterSize = imgW * imgH * bpp;

    Texture tex;
    tex.format = fmt;

    TextureMip mip;
    mip.width = static_cast<std::uint32_t>(imgW);
    mip.height = static_cast<std::uint32_t>(imgH);
    mip.bytes.resize(rasterSize);

    for (std::size_t by = 0; by < gridH; ++by) {
        for (std::size_t bx = 0; bx < gridW; ++bx) {
            const std::size_t blockIdx = by * gridW + bx;
            const auto* blockData = data + dataOffset + blockIdx * 16;

            if (hdr) {
                decodeBlockHdr(blockData, reinterpret_cast<std::uint8_t*>(mip.bytes.data()),
                    static_cast<std::uint32_t>(imgW), static_cast<std::uint32_t>(imgH),
                    static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by),
                    blockW, blockH);
            } else {
                decodeBlockLdr(blockData, reinterpret_cast<std::uint8_t*>(mip.bytes.data()),
                    static_cast<std::uint32_t>(imgW), static_cast<std::uint32_t>(imgH),
                    static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by),
                    blockW, blockH, bpp);
            }
        }
    }

    tex.mips.push(std::move(mip));
    return tex;
}

} // namespace f4::resources
