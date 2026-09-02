#pragma once

#include <cstdint>

#include "graphics/types.h"

namespace f4::gfx {

struct TextureDesc {
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
    // Optional backend allocation hint. It does not define render-pass clear
    // behavior; attachment clearOnLoad remains authoritative.
    bool hasOptimizedClear = false;
    ClearValue optimizedClear{};
};

[[nodiscard]] constexpr auto maxTextureMipLevels(std::uint32_t width,
                                                  std::uint32_t height) -> std::uint32_t {
    std::uint32_t size = width > height ? width : height;
    std::uint32_t levels = 0;
    while (size != 0) {
        ++levels;
        size >>= 1u;
    }
    return levels;
}

struct GpuTexture {
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
};

} // namespace f4::gfx
