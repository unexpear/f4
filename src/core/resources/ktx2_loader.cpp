#include "core/resources/ktx2_loader.h"

#include <algorithm>
#include <cstring>
#include <optional>

// BasisU transcoder — included without IMPLEMENTATION (that lives in basisu_transcoder_impl.cpp).
#include <basisu_transcoder.h>

namespace f4::resources {

// ── KTX2 constants ──────────────────────────────────────────────────────────

static constexpr std::uint8_t kKtx2Ident[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

static constexpr std::size_t kKtx2HeaderSize = 80;
static constexpr std::size_t kKtx2LevelIndexEntrySize = 24; // 3 × uint64

// Supercompression schemes (KTX 2.0 spec §8).
enum Ktx2Supercompression : std::uint32_t {
    Ktx2SS_None      = 0,
    Ktx2SS_BasisLZ   = 1,
    Ktx2SS_Zstandard = 2,
    Ktx2SS_Zlib      = 3,
};

// Vulkan format IDs we handle (from vulkan_core.h).
enum VkFormat : std::uint32_t {
    VK_FORMAT_R8_UNORM                = 9,
    VK_FORMAT_R8_SNORM                = 10,
    VK_FORMAT_R8G8_UNORM              = 16,
    VK_FORMAT_R8G8B8_UNORM            = 23,
    VK_FORMAT_B8G8R8_UNORM            = 30,
    VK_FORMAT_R8G8B8A8_UNORM          = 37,
    VK_FORMAT_B8G8R8A8_UNORM          = 44,
    VK_FORMAT_R8G8B8A8_SRGB           = 43,
    VK_FORMAT_B8G8R8A8_SRGB           = 45,
    VK_FORMAT_R16_SFLOAT              = 76,
    VK_FORMAT_R16G16_SFLOAT           = 83,
    VK_FORMAT_R16G16B16A16_SFLOAT     = 97,
    VK_FORMAT_R32_SFLOAT              = 100,
    VK_FORMAT_R32G32_SFLOAT           = 103,
    VK_FORMAT_R32G32B32_SFLOAT        = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT     = 109,
};

// ── Little-endian readers ───────────────────────────────────────────────────

[[nodiscard]] static auto readU32LE(const std::uint8_t* p) -> std::uint32_t {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] static auto readU64LE(const std::uint8_t* p) -> std::uint64_t {
    return static_cast<std::uint64_t>(p[0])
         | (static_cast<std::uint64_t>(p[1]) << 8)
         | (static_cast<std::uint64_t>(p[2]) << 16)
         | (static_cast<std::uint64_t>(p[3]) << 24)
         | (static_cast<std::uint64_t>(p[4]) << 32)
         | (static_cast<std::uint64_t>(p[5]) << 40)
         | (static_cast<std::uint64_t>(p[6]) << 48)
         | (static_cast<std::uint64_t>(p[7]) << 56);
}

// ── VK_FORMAT → TextureFormat mapping ───────────────────────────────────────

struct VkFormatMapping {
    VkFormat vk;
    TextureFormat tex;
};

static constexpr VkFormatMapping kVkMappings[] = {
    {VK_FORMAT_R8_UNORM,            TextureFormat::R8_UNORM},
    {VK_FORMAT_R8G8_UNORM,          TextureFormat::RG8_UNORM},
    {VK_FORMAT_R8G8B8A8_UNORM,      TextureFormat::RGBA8_UNORM},
    {VK_FORMAT_B8G8R8A8_UNORM,      TextureFormat::BGRA8_UNORM},
    {VK_FORMAT_R8G8B8A8_SRGB,       TextureFormat::RGBA8_UNORM},
    {VK_FORMAT_B8G8R8A8_SRGB,       TextureFormat::BGRA8_UNORM},
    {VK_FORMAT_R16_SFLOAT,          TextureFormat::R16_FLOAT},
    {VK_FORMAT_R16G16B16A16_SFLOAT, TextureFormat::RGBA16_FLOAT},
    {VK_FORMAT_R32G32B32A32_SFLOAT, TextureFormat::RGBA32_FLOAT},
};

[[nodiscard]] static auto mapVkFormat(std::uint32_t vk) -> std::optional<TextureFormat> {
    for (const auto& m : kVkMappings) {
        if (static_cast<std::uint32_t>(m.vk) == vk) return m.tex;
    }
    return std::nullopt;
}

// Bytes per pixel for a given uncompressed TextureFormat.
[[nodiscard]] static auto bytesPerPixel(TextureFormat fmt) -> std::size_t {
    switch (fmt) {
    case TextureFormat::R8_UNORM:            return 1;
    case TextureFormat::RG8_UNORM:           return 2;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::BGRA8_UNORM:         return 4;
    case TextureFormat::R16_FLOAT:           return 2;
    case TextureFormat::RGBA16_FLOAT:        return 8;
    case TextureFormat::RGBA32_FLOAT:        return 16;
    default:                                 return 0;
    }
}

// ── Raw-format loader (vkFormat != 0) ──────────────────────────────────────

[[nodiscard]] static auto loadKtx2Raw(
    const std::uint8_t* data, std::size_t dataSize,
    std::uint32_t width, std::uint32_t height, std::uint32_t depth,
    std::uint32_t levelCount, std::uint32_t vkFormat)
    -> Result<Texture, LoadError>
{
    if (depth != 0 && depth != 1) {
        return LoadError::UnsupportedCompression; // 3D textures not supported yet
    }

    auto fmtOpt = mapVkFormat(vkFormat);
    if (!fmtOpt) return LoadError::UnsupportedBpp;
    auto fmt = *fmtOpt;

    // Compute the byte size of the level index array.
    const std::size_t levelIndexStart = kKtx2HeaderSize;

    // Compute how many mip levels we actually read (capped to what we produce).
    const std::uint32_t mipCount = (levelCount == 0) ? 1 : levelCount;

    Texture tex;
    tex.format = fmt;
    tex.mips.reserve(mipCount);

    for (std::uint32_t mip = 0; mip < mipCount; ++mip) {
        // Read level index entry.
        const std::size_t entryOff = levelIndexStart + static_cast<std::size_t>(mip) * kKtx2LevelIndexEntrySize;
        if (entryOff + kKtx2LevelIndexEntrySize > dataSize) {
            return LoadError::Truncated;
        }
        const std::uint64_t byteOffset   = readU64LE(data + entryOff);
        const std::uint64_t byteLength   = readU64LE(data + entryOff + 8);

        const std::uint32_t mipW = std::max(width >> mip, 1u);
        const std::uint32_t mipH = std::max(height >> mip, 1u);
        const std::size_t bpp = bytesPerPixel(fmt);
        const std::size_t expectedSize = static_cast<std::size_t>(mipW) * mipH * bpp;

        if (byteOffset + byteLength > dataSize) {
            return LoadError::Truncated;
        }
        if (static_cast<std::size_t>(byteLength) < expectedSize) {
            return LoadError::Truncated;
        }

        TextureMip mipTex;
        mipTex.width = mipW;
        mipTex.height = mipH;
        mipTex.bytes.resize(expectedSize);
        std::memcpy(mipTex.bytes.data(), data + byteOffset, expectedSize);
        tex.mips.push(std::move(mipTex));
    }

    return tex;
}

// ── Basis path (vkFormat == 0) via basisu transcoder ───────────────────────

[[nodiscard]] static auto loadKtx2Basis(
    const std::uint8_t* data, std::size_t dataSize)
    -> Result<Texture, LoadError>
{
    basist::ktx2_transcoder tc;
    if (!tc.init(data, static_cast<std::uint32_t>(dataSize))) {
        return LoadError::BadSignature;
    }

    const auto format = tc.get_basis_tex_format();
    const bool isBasisLdr = (format == basist::basis_tex_format::cETC1S)
                         || (format == basist::basis_tex_format::cUASTC_LDR_4x4)
                         || (format == basist::basis_tex_format::cXUASTC_LDR_4x4)
                         || (format == basist::basis_tex_format::cXUBC7);
    const bool isBasisHdr = (format == basist::basis_tex_format::cUASTC_HDR_4x4)
                         || (format == basist::basis_tex_format::cASTC_HDR_6x6)
                         || (format == basist::basis_tex_format::cUASTC_HDR_6x6_INTERMEDIATE);

    if (!isBasisLdr && !isBasisHdr) {
        return LoadError::UnsupportedCompression;
    }

    if (!tc.start_transcoding()) {
        return LoadError::BadSignature;
    }

    const std::uint32_t width  = tc.get_width();
    const std::uint32_t height = tc.get_height();
    const std::uint32_t levels = tc.get_levels();
    const std::uint32_t mipCount = (levels == 0) ? 1 : levels;

    // Choose output format: RGBA32 for LDR → float representation of 8-bit, or
    // RGBA16_FLOAT for HDR.
    const auto targetFmt = isBasisHdr
        ? basist::transcoder_texture_format::cTFRGBA_HALF
        : basist::transcoder_texture_format::cTFRGBA32;

    const TextureFormat texFmt = isBasisHdr
        ? TextureFormat::RGBA16_FLOAT
        : TextureFormat::RGBA8_UNORM;

    const std::size_t bppOut = isBasisHdr ? 8 : 4;

    Texture tex;
    tex.format = texFmt;
    tex.mips.reserve(mipCount);

    for (std::uint32_t mip = 0; mip < mipCount; ++mip) {
        const std::uint32_t mipW = std::max(width >> mip, 1u);
        const std::uint32_t mipH = std::max(height >> mip, 1u);
        const std::size_t outSize = static_cast<std::size_t>(mipW) * mipH * bppOut;

        TextureMip mipTex;
        mipTex.width = mipW;
        mipTex.height = mipH;
        mipTex.bytes.resize(outSize);

        if (!tc.transcode_image_level(
                mip, 0, 0,
                mipTex.bytes.data(),
                static_cast<std::uint32_t>(mipW) * mipH,
                targetFmt)) {
            return LoadError::BadSignature;
        }

        tex.mips.push(std::move(mipTex));
    }

    return tex;
}

// ── Public entry point ──────────────────────────────────────────────────────

auto loadKtx2(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto dataSize = bytes.size();

    if (dataSize < kKtx2HeaderSize) {
        return LoadError::Truncated;
    }

    if (std::memcmp(data, kKtx2Ident, 12) != 0) {
        return LoadError::BadSignature;
    }

    const auto vkFormat    = readU32LE(data + 12);
    const auto typeSize    = readU32LE(data + 16);
    const auto width       = readU32LE(data + 20);
    const auto height      = readU32LE(data + 24);
    const auto depth       = readU32LE(data + 28);
    const auto layerCount  = readU32LE(data + 32);
    const auto faceCount   = readU32LE(data + 36);
    const auto levelCount  = readU32LE(data + 40);
    const auto supercomp   = readU32LE(data + 44);

    (void)typeSize;
    (void)layerCount;
    (void)faceCount;

    if (width == 0 || height == 0) {
        return LoadError::BadDimensions;
    }
    if (faceCount > 1) {
        return LoadError::UnsupportedCompression; // cubemaps not supported yet
    }

    const bool isBasis = (vkFormat == 0);

    if (isBasis) {
        // Basis path: delegate to the transcoder which handles ETC1S / UASTC.
        if (supercomp == Ktx2SS_Zstandard) {
            return LoadError::UnsupportedCompression; // zstd not enabled
        }
        return loadKtx2Basis(data, dataSize);
    }

    // Raw Vulkan format path.
    if (supercomp != Ktx2SS_None) {
        return LoadError::UnsupportedCompression; // compressed raw formats not supported
    }
    return loadKtx2Raw(data, dataSize, width, height, depth,
                       levelCount == 0 ? 1 : levelCount, vkFormat);
}

} // namespace f4::resources
