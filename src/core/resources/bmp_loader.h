#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Decode a BMP image from raw file bytes into a Texture record.
// Supports uncompressed 24-bit (BGR→RGBA) and 32-bit (BGRA→RGBA).
// Bottom-up (default BMP) and top-down (negative height) orientations.
[[nodiscard]] auto loadBmp(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
