#pragma once

#include <cstdint>

#include "graphics/types.h"

namespace f4::gfx {

struct SwapchainDesc {
    TextureFormat surfaceFormat = TextureFormat::RGBA8_UNORM;
    Extent extent;
    PresentMode presentMode = PresentMode::Fifo;
};

struct GpuSwapchain {
    TextureFormat surfaceFormat = TextureFormat::RGBA8_UNORM;
    Extent extent;
    PresentMode presentMode = PresentMode::Fifo;
};

} // namespace f4::gfx
