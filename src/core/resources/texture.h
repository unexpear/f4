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
    ASTC_4X4_UNORM,
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
    case TextureFormat::ASTC_4X4_UNORM: return true;
    default: return false;
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