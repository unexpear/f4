#pragma once

#include <cstddef>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Block-compression family selector shared by DDS (M5.5) and KTX2 (M5.6).
enum class BcFormat : std::uint8_t {
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC6H_UF16,
    BC6H_SF16,
    BC7,
};

// Compressed block payload size in bytes (BC1/BC4 are 8, the rest 16).
[[nodiscard]] constexpr auto bcBlockSize(BcFormat format) -> std::size_t {
    switch (format) {
    case BcFormat::BC1:
    case BcFormat::BC4: return 8;
    default: return 16;
    }
}

// Decoded output format produced by the CPU software decoder for a BC format.
// LDR formats expand to 8-bit unorm; HDR BC6H expands to float32 (§12).
[[nodiscard]] auto decodedFormat(BcFormat format) -> TextureFormat;

// Parse a DDS container (legacy fourcc or DX10 header) and software-decode
// every mip level into a CPU-sampled Texture. Supports only 2D textures and
// the BC1–BC7 block-compressed payloads; other DDS encodings are rejected.
[[nodiscard]] auto loadDds(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
