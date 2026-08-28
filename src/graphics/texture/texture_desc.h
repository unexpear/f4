#pragma once

#include <cstdint>

#include "graphics/types.h"

namespace f4::gfx {

struct TextureDesc {
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
};

struct GpuTexture {
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
};

} // namespace f4::gfx
