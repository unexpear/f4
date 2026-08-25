#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Decode any 8-bit image format (PNG, JPG, BMP, TGA, PSD, GIF, PIC, PNM)
// from raw file bytes into a Texture record via stb_image.
// Output is always RGBA8_UNORM regardless of source channel count.
[[nodiscard]] auto loadStbImage(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

// Decode HDR (Radiance .hdr) images into a Texture record via stb_image.
// Output is RGBA32_FLOAT (linear HDR values, 4 channels).
[[nodiscard]] auto loadStbImageHdr(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
