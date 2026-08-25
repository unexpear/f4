#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Minimal OpenEXR scanline loader (uncompressed + ZIP deflate).
// Reads HALF and FLOAT channels; converts to RGBA32_FLOAT texture.
// Tiled and deep EXR files are not supported (LoadError::UnsupportedCompression).
[[nodiscard]] auto loadExr(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
