#pragma once

#include <cstdint>

#include "core/containers/array.h"

namespace f4::gfx {

using containers::Array;

// Maps 1:1 to resources::ShaderStage; kept local so the RHI layer owns no
// dependency on the asset layer's enum identity beyond the convention that
// graphics::ShaderStage::Vertex == resources::ShaderStage::Vertex.
enum class ShaderStage : std::uint8_t {
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessControl,
    TessEvaluation,
};

[[nodiscard]] constexpr auto shaderStageCount() -> std::uint32_t {
    return 6;
}

[[nodiscard]] constexpr auto shaderStageBit(ShaderStage stage) -> std::uint32_t {
    return 1u << static_cast<std::uint32_t>(stage);
}

// Opaque stage source/blob: the RHI never interprets shader bytes (plan §7).
struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    Array<std::byte> blob;
};

struct GpuShader {
    ShaderStage stage = ShaderStage::Vertex;
};

} // namespace f4::gfx
