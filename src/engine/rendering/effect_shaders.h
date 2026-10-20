#pragma once

#include "core/result.h"
#include "graphics/device/device.h"
#include "graphics/gfx_error.h"
#include "graphics/handle.h"

namespace f4::engine {

// Renderer-owned effect shaders that sit beside the PBR material shaders:
// the point-light shadow face pass and the HDR bloom post chain. Paths come
// from per-backend compile definitions (SPIR-V/DXIL/metallib/GLSL files) or,
// on WebGPU, from the generated WGSL header.
enum class EffectShader {
    PointShadowVertex,
    PointShadowFragment,
    BloomVertex,
    BloomFragment,
};

[[nodiscard]] auto loadEffectShader(gfx::IDevice& device, EffectShader shader)
    -> Result<gfx::ShaderHandle, gfx::GfxError>;

} // namespace f4::engine
