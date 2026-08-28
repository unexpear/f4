#pragma once

#include "core/ids/handle.h"
#include "core/ids/generational_arena.h"

namespace f4::gfx {

// One opaque handle family per device-object kind. Mutually incompatible at
// compile time; stale-handle detection inherited from the M2 arena design.
struct GpuBufferTag;
struct GpuTextureTag;
struct GpuSamplerTag;
struct GpuShaderTag;
struct GpuPipelineTag;
struct GpuSwapchainTag;
struct GpuFramebufferTag;

using BufferHandle = ids::Handle<GpuBufferTag>;
using TextureHandle = ids::Handle<GpuTextureTag>;
using SamplerHandle = ids::Handle<GpuSamplerTag>;
using ShaderHandle = ids::Handle<GpuShaderTag>;
using PipelineHandle = ids::Handle<GpuPipelineTag>;
using SwapchainHandle = ids::Handle<GpuSwapchainTag>;
using FramebufferHandle = ids::Handle<GpuFramebufferTag>;

} // namespace f4::gfx
