#pragma once

#include <cstdint>

#include "graphics/buffer/buffer_desc.h"
#include "graphics/texture/texture_desc.h"

namespace f4::gfx {

// CPU-side byte footprint of a texture record at mip 0 (per-mip cost halves
// each level). Depth/stencil formats count at their single-component size.
[[nodiscard]] constexpr auto textureFormatBytes(TextureFormat format) -> std::uint64_t {
    switch (format) {
    case TextureFormat::R8_UNORM: return 1;
    case TextureFormat::RG8_UNORM: return 2;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::BGRA8_UNORM: return 4;
    case TextureFormat::R16_FLOAT: return 2;
    case TextureFormat::RG16_FLOAT: return 4;
    case TextureFormat::RGBA16_FLOAT: return 8;
    case TextureFormat::RGBA32_FLOAT: return 16;
    case TextureFormat::Depth32F: return 4;
    case TextureFormat::Depth24Stencil8: return 4;
    }
    return 0;
}

// Backing-store bytes for a texture including all mip levels.
[[nodiscard]] constexpr auto textureBytes(const GpuTexture& t) -> std::uint64_t {
    const std::uint64_t bytesPerTexel = textureFormatBytes(t.format);
    std::uint64_t total = 0;
    std::uint64_t w = t.width;
    std::uint64_t h = t.height;
    for (std::uint32_t mip = 0; mip < t.mipLevels; ++mip) {
        total += (w > 0 ? w : 1) * (h > 0 ? h : 1) * bytesPerTexel;
        w /= 2;
        h /= 2;
        if (w == 0 && h == 0) break;
    }
    return total;
}

// Neutral, backend-independent diagnostics snapshot from IDevice::stats().
// Per-frame render counters are reset by resetFrameStats(); the resource
// accounting reflects live objects and is always current.
struct DeviceStats {
    // ── Per-frame render counters ──────────────────────────────────────
    std::uint64_t drawCalls = 0;        // distinct Draw (non-indexed) commands
    std::uint64_t drawIndexedCalls = 0; // distinct DrawIndexed commands
    std::uint64_t dispatchCalls = 0;    // distinct compute Dispatch commands
    std::uint64_t batches = 0;          // total draw commands (indexed + non)
    std::uint64_t instances = 0;        // summed instance counts across draws
    std::uint64_t primitives = 0;       // summed primitives (triangles) across draws
    std::uint64_t vertices = 0;         // summed vertex/instance expansions

    // ── Live GPU resource accounting (always current) ──────────────────
    std::uint64_t bufferCount = 0;
    std::uint64_t bufferBytes = 0;
    std::uint64_t textureCount = 0;
    std::uint64_t textureBytes = 0;
    std::uint64_t samplerCount = 0;
    std::uint64_t shaderCount = 0;
    std::uint64_t shaderCompiled = 0;   // cumulative successful compiles
    std::uint64_t shaderFailed = 0;     // cumulative failed compiles
    std::uint64_t shaderWarnings = 0;   // cumulative compile-with-warnings
    std::uint64_t pipelineCount = 0;
    std::uint64_t framebufferCount = 0;
    std::uint64_t swapchainCount = 0;
};

} // namespace f4::gfx
