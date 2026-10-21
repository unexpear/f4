#pragma once

#include <cstddef>
#include <span>

#include "core/result.h"

#include "graphics/buffer/buffer_desc.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device_stats.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/framebuffer/swapchain_desc.h"
#include "graphics/gfx_error.h"
#include "graphics/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/texture/texture_desc.h"
#include "graphics/types.h"

namespace f4::gfx {

// Abstract RHI device. Zero backend-specific types appear in this interface or
// its results — callers hold only opaque handles and neutral enums.
class IDevice {
public:
    virtual ~IDevice() = default;

    [[nodiscard]] virtual auto backend() const -> Backend = 0;

    // Object creation. Handles are opaque; records are owned by the device.
    virtual auto createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
        -> Result<BufferHandle, GfxError> = 0;
    virtual auto createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> = 0;
    virtual auto createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> = 0;
    virtual auto createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> = 0;
    virtual auto createPipeline(const PipelineStateDesc& desc) -> Result<PipelineHandle, GfxError> = 0;
    virtual auto createSwapchain(const SwapchainDesc& desc) -> Result<SwapchainHandle, GfxError> = 0;
    virtual auto createFramebuffer(const FramebufferDesc& desc)
        -> Result<FramebufferHandle, GfxError> = 0;

    virtual auto destroyBuffer(BufferHandle) -> GfxError = 0;
    virtual auto destroyTexture(TextureHandle) -> GfxError = 0;
    virtual auto destroySampler(SamplerHandle) -> GfxError = 0;
    virtual auto destroyShader(ShaderHandle) -> GfxError = 0;
    virtual auto destroyPipeline(PipelineHandle) -> GfxError = 0;
    virtual auto destroySwapchain(SwapchainHandle) -> GfxError = 0;
    virtual auto destroyFramebuffer(FramebufferHandle) -> GfxError = 0;

    // Upload to an existing buffer (bounds-checked by the backend).
    virtual auto writeBuffer(BufferHandle, std::uint64_t offset, std::span<const std::byte> data)
        -> GfxError = 0;

    // Synchronous buffer readback. The range is bounds-checked; callers must
    // still declare device-write ordering with a command-buffer barrier.
    virtual auto readBuffer(BufferHandle, std::uint64_t offset, std::span<std::byte> destination)
        -> GfxError = 0;

    // Upload CPU texel data to an existing texture's mip level. The data size
    // must exactly match the level's footprint (bounds-checked by the backend);
    // this is how CPU bitmaps (e.g. a glyph atlas) reach the GPU.
    virtual auto writeTexture(TextureHandle, std::uint32_t mipLevel,
                              std::span<const std::byte> data) -> GfxError = 0;

    // Submit recorded commands for execution.
    virtual auto submit(CommandBuffer&) -> GfxError = 0;

    virtual auto waitIdle() -> void = 0;

    // Diagnostics: snapshot of per-frame render counters plus live GPU resource
    // accounting. Always current; the per-frame counters accumulate across
    // submits and are cleared by resetFrameStats() (call once per frame).
    [[nodiscard]] virtual auto stats() const -> DeviceStats = 0;

    // Clears per-frame draw, dispatch, instance, primitive, vertex, and batch
    // counters. Resource accounting is unaffected.
    virtual auto resetFrameStats() -> void = 0;

    // GPU timestamps (gpu_timestamps.h). Devices without timer queries return
    // false here and ignore WriteTimestamp commands.
    [[nodiscard]] virtual auto supportsGpuTimestamps() const -> bool { return false; }
    // Serial of the most recent submit that wrote timestamps (0 if none).
    [[nodiscard]] virtual auto lastGpuTimestampSerial() const -> std::uint64_t { return 0; }
    // Newest finished timestamp set, without waiting for the GPU.
    virtual auto latestGpuTimestamps() -> GpuTimestampFrame { return {}; }

    // Async compute: a second, compute-only queue (Vulkan: a dedicated
    // compute queue family; DX12: a COMPUTE command queue) that runs work
    // alongside the graphics queue. submitAsyncCompute() submits compute-only
    // commands (pipelines, resource binds, dispatches; buffers only, no
    // timestamps) ahead of the next submit(), which waits for them at its
    // CommandBuffer::waitAsyncCompute() point. Buffers it writes are visible
    // to the graphics commands after that wait. Devices without such a queue
    // return false / BackendUnavailable; callers then record the same work
    // inline (see RenderGraph::setAsyncCompute).
    [[nodiscard]] virtual auto supportsAsyncCompute() const -> bool { return false; }
    virtual auto submitAsyncCompute(CommandBuffer&) -> GfxError {
        return GfxError::BackendUnavailable;
    }
};

// Factory. The compiled-in backend (F4_GRAPHICS_BACKEND) decides the concrete
// device. Always non-null unless the backend cannot initialize. The caller
// owns the returned pointer and releases it via deleteDevice().
[[nodiscard]] auto createDevice() -> IDevice*;
void deleteDevice(IDevice* device);

} // namespace f4::gfx
