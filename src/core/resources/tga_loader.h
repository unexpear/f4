#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Decode a TGA image from raw file bytes into a Texture record.
// Supports true-color type 2 (uncompressed) and type 10 (RLE),
// 16/24/32 bpp, no color maps. Both row orders (top-down and
// bottom-up) are handled via the image descriptor field.
[[nodiscard]] auto loadTga(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
