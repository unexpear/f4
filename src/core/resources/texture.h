#pragma once

#include <cstdint>

#include "core/containers/array.h"

namespace f4::resources {

using containers::Array;

// CPU-side texture formats. Block-compressed entries hold GPU-native payloads
// (uploaded as-is from M7+); "support" does not imply software decode (§12).
enum class TextureFormat : std::uint8_t {
    R8_UNORM,
    RG8_UNORM,
    RGBA8_UNORM,
    BGRA8_UNORM,
    R16_FLOAT,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    BC1_RGBA_UNORM,
    BC2_UNORM,
    BC3_UNORM,
    BC4_UNORM,
    BC5_UNORM,
    BC6H_UFLOAT,
    BC7_UNORM,
    // ASTC LDR — all 14 standard 2D block sizes, decode to RGBA8_UNORM.
    ASTC_4X4_UNORM,
    ASTC_5X4_UNORM,
    ASTC_5X5_UNORM,
    ASTC_6X5_UNORM,
    ASTC_6X6_UNORM,
    ASTC_8X5_UNORM,
    ASTC_8X6_UNORM,
    ASTC_10X5_UNORM,
    ASTC_10X6_UNORM,
    ASTC_8X8_UNORM,
    ASTC_10X8_UNORM,
    ASTC_10X10_UNORM,
    ASTC_12X10_UNORM,
    ASTC_12X12_UNORM,
    // ASTC HDR — all 14 standard 2D block sizes, decode to RGBA32_FLOAT.
    ASTC_4X4_HFLOAT,
    ASTC_5X4_HFLOAT,
    ASTC_5X5_HFLOAT,
    ASTC_6X5_HFLOAT,
    ASTC_6X6_HFLOAT,
    ASTC_8X5_HFLOAT,
    ASTC_8X6_HFLOAT,
    ASTC_10X5_HFLOAT,
    ASTC_10X6_HFLOAT,
    ASTC_8X8_HFLOAT,
    ASTC_10X8_HFLOAT,
    ASTC_10X10_HFLOAT,
    ASTC_12X10_HFLOAT,
    ASTC_12X12_HFLOAT,
};

[[nodiscard]] constexpr auto isBlockCompressed(TextureFormat format) -> bool {
    switch (format) {
    case TextureFormat::BC1_RGBA_UNORM:
    case TextureFormat::BC2_UNORM:
    case TextureFormat::BC3_UNORM:
    case TextureFormat::BC4_UNORM:
    case TextureFormat::BC5_UNORM:
    case TextureFormat::BC6H_UFLOAT:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::ASTC_4X4_UNORM:
    case TextureFormat::ASTC_5X4_UNORM:
    case TextureFormat::ASTC_5X5_UNORM:
    case TextureFormat::ASTC_6X5_UNORM:
    case TextureFormat::ASTC_6X6_UNORM:
    case TextureFormat::ASTC_8X5_UNORM:
    case TextureFormat::ASTC_8X6_UNORM:
    case TextureFormat::ASTC_10X5_UNORM:
    case TextureFormat::ASTC_10X6_UNORM:
    case TextureFormat::ASTC_8X8_UNORM:
    case TextureFormat::ASTC_10X8_UNORM:
    case TextureFormat::ASTC_10X10_UNORM:
    case TextureFormat::ASTC_12X10_UNORM:
    case TextureFormat::ASTC_12X12_UNORM:
    case TextureFormat::ASTC_4X4_HFLOAT:
    case TextureFormat::ASTC_5X4_HFLOAT:
    case TextureFormat::ASTC_5X5_HFLOAT:
    case TextureFormat::ASTC_6X5_HFLOAT:
    case TextureFormat::ASTC_6X6_HFLOAT:
    case TextureFormat::ASTC_8X5_HFLOAT:
    case TextureFormat::ASTC_8X6_HFLOAT:
    case TextureFormat::ASTC_10X5_HFLOAT:
    case TextureFormat::ASTC_10X6_HFLOAT:
    case TextureFormat::ASTC_8X8_HFLOAT:
    case TextureFormat::ASTC_10X8_HFLOAT:
    case TextureFormat::ASTC_10X10_HFLOAT:
    case TextureFormat::ASTC_12X10_HFLOAT:
    case TextureFormat::ASTC_12X12_HFLOAT: return true;
    default: return false;
    }
}

[[nodiscard]] constexpr auto isAstcHdr(TextureFormat fmt) -> bool {
    switch (fmt) {
    case TextureFormat::ASTC_4X4_HFLOAT:
    case TextureFormat::ASTC_5X4_HFLOAT:
    case TextureFormat::ASTC_5X5_HFLOAT:
    case TextureFormat::ASTC_6X5_HFLOAT:
    case TextureFormat::ASTC_6X6_HFLOAT:
    case TextureFormat::ASTC_8X5_HFLOAT:
    case TextureFormat::ASTC_8X6_HFLOAT:
    case TextureFormat::ASTC_10X5_HFLOAT:
    case TextureFormat::ASTC_10X6_HFLOAT:
    case TextureFormat::ASTC_8X8_HFLOAT:
    case TextureFormat::ASTC_10X8_HFLOAT:
    case TextureFormat::ASTC_10X10_HFLOAT:
    case TextureFormat::ASTC_12X10_HFLOAT:
    case TextureFormat::ASTC_12X12_HFLOAT: return true;
    default: return false;
    }
}

// Block-compressed output format for a given ASTC variant.
[[nodiscard]] constexpr auto astcOutputFormat(TextureFormat fmt) -> TextureFormat {
    return isAstcHdr(fmt) ? TextureFormat::RGBA32_FLOAT : TextureFormat::RGBA8_UNORM;
}

// ASTC block width (texels) for a given TextureFormat. Returns 0 for non-ASTC.
[[nodiscard]] constexpr auto astcBlockWidth(TextureFormat fmt) -> std::uint32_t {
    switch (fmt) {
    case TextureFormat::ASTC_4X4_UNORM:  case TextureFormat::ASTC_4X4_HFLOAT:  return 4;
    case TextureFormat::ASTC_5X4_UNORM:  case TextureFormat::ASTC_5X4_HFLOAT:  return 5;
    case TextureFormat::ASTC_5X5_UNORM:  case TextureFormat::ASTC_5X5_HFLOAT:  return 5;
    case TextureFormat::ASTC_6X5_UNORM:  case TextureFormat::ASTC_6X5_HFLOAT:  return 6;
    case TextureFormat::ASTC_6X6_UNORM:  case TextureFormat::ASTC_6X6_HFLOAT:  return 6;
    case TextureFormat::ASTC_8X5_UNORM:  case TextureFormat::ASTC_8X5_HFLOAT:  return 8;
    case TextureFormat::ASTC_8X6_UNORM:  case TextureFormat::ASTC_8X6_HFLOAT:  return 8;
    case TextureFormat::ASTC_10X5_UNORM: case TextureFormat::ASTC_10X5_HFLOAT: return 10;
    case TextureFormat::ASTC_10X6_UNORM: case TextureFormat::ASTC_10X6_HFLOAT: return 10;
    case TextureFormat::ASTC_8X8_UNORM:  case TextureFormat::ASTC_8X8_HFLOAT:  return 8;
    case TextureFormat::ASTC_10X8_UNORM: case TextureFormat::ASTC_10X8_HFLOAT: return 10;
    case TextureFormat::ASTC_10X10_UNORM:case TextureFormat::ASTC_10X10_HFLOAT:return 10;
    case TextureFormat::ASTC_12X10_UNORM:case TextureFormat::ASTC_12X10_HFLOAT:return 12;
    case TextureFormat::ASTC_12X12_UNORM:case TextureFormat::ASTC_12X12_HFLOAT:return 12;
    default: return 0;
    }
}

// ASTC block height (texels) for a given TextureFormat. Returns 0 for non-ASTC.
[[nodiscard]] constexpr auto astcBlockHeight(TextureFormat fmt) -> std::uint32_t {
    switch (fmt) {
    case TextureFormat::ASTC_4X4_UNORM:  case TextureFormat::ASTC_4X4_HFLOAT:  return 4;
    case TextureFormat::ASTC_5X4_UNORM:  case TextureFormat::ASTC_5X4_HFLOAT:  return 4;
    case TextureFormat::ASTC_5X5_UNORM:  case TextureFormat::ASTC_5X5_HFLOAT:  return 5;
    case TextureFormat::ASTC_6X5_UNORM:  case TextureFormat::ASTC_6X5_HFLOAT:  return 5;
    case TextureFormat::ASTC_6X6_UNORM:  case TextureFormat::ASTC_6X6_HFLOAT:  return 6;
    case TextureFormat::ASTC_8X5_UNORM:  case TextureFormat::ASTC_8X5_HFLOAT:  return 5;
    case TextureFormat::ASTC_8X6_UNORM:  case TextureFormat::ASTC_8X6_HFLOAT:  return 6;
    case TextureFormat::ASTC_10X5_UNORM: case TextureFormat::ASTC_10X5_HFLOAT: return 5;
    case TextureFormat::ASTC_10X6_UNORM: case TextureFormat::ASTC_10X6_HFLOAT: return 6;
    case TextureFormat::ASTC_8X8_UNORM:  case TextureFormat::ASTC_8X8_HFLOAT:  return 8;
    case TextureFormat::ASTC_10X8_UNORM: case TextureFormat::ASTC_10X8_HFLOAT: return 8;
    case TextureFormat::ASTC_10X10_UNORM:case TextureFormat::ASTC_10X10_HFLOAT:return 10;
    case TextureFormat::ASTC_12X10_UNORM:case TextureFormat::ASTC_12X10_HFLOAT:return 10;
    case TextureFormat::ASTC_12X12_UNORM:case TextureFormat::ASTC_12X12_HFLOAT:return 12;
    default: return 0;
    }
}

struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Array<std::byte> bytes;
};

// 2D-only in v1; array/cube/3D kinds arrive when a consumer needs them.
struct Texture {
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    Array<TextureMip> mips; // mip 0 first, strictly halving dimensions
};

} // namespace f4::resources
