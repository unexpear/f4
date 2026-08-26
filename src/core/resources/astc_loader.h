#pragma once

#include <cstddef>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Parse an ASTC file (16-byte header + raw block data) and decompress every
// block into a CPU-sampled Texture.  Supports all 14 standard 2D block sizes
// in both LDR (→ RGBA8_UNORM) and HDR (→ RGBA32_FLOAT) profiles.  3D and
// array textures are rejected (depth != 1).
[[nodiscard]] auto loadAstc(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
