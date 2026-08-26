#pragma once

#include <cstddef>
#include <span>

#include "core/resources/load_error.h"
#include "core/resources/texture.h"
#include "core/result.h"

namespace f4::resources {

// Call once before the first loadKtx2() to initialise the BasisU transcoder.
void initBasisTranscoder();

// Parse a KTX2 container and decode every mip level into a CPU-sampled Texture.
// For raw Vulkan formats (vkFormat != 0), texels are copied/converted directly.
// For Basis payloads (ETC1S / UASTC with vkFormat == 0), the transcoder is
// invoked to produce RGBA8_UNORM output.
[[nodiscard]] auto loadKtx2(std::span<const std::byte> bytes)
    -> Result<Texture, LoadError>;

} // namespace f4::resources
