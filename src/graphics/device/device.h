#pragma once

#include <cstddef>
#include <span>

#include "core/result.h"

#include "graphics/buffer/buffer_desc.h"
#include "graphics/command/command_buffer.h"
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

    // Submit recorded commands for execution.
    virtual auto submit(CommandBuffer&) -> GfxError = 0;

    virtual auto waitIdle() -> void = 0;
};

// Factory. The compiled-in backend (F4_GRAPHICS_BACKEND) decides the concrete
// device. Always non-null unless the backend cannot initialize. The caller
// owns the returned pointer and releases it via deleteDevice().
[[nodiscard]] auto createDevice() -> IDevice*;
void deleteDevice(IDevice* device);

} // namespace f4::gfx
