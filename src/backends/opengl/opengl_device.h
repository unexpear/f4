#pragma once

#include <epoxy/gl.h>

#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/device/object_store.h"

namespace f4::gfx {

// Real OpenGL backend on a callers-provided (SDL2) GL 4.3 core context. IDevice
// factory resolves to this when F4_GRAPHICS_BACKEND=opengl. `submit` replays
// recorded commands immediately (plan §3: GL executes eagerly). The device does
// not own the context — the host (windowed demo or the headless conformance
// test) keeps it current and calls present() after submit.
class OpenGLDevice final : public IDevice {
public:
    ~OpenGLDevice() override;

    OpenGLDevice(const OpenGLDevice&) = delete;
    auto operator=(const OpenGLDevice&) -> OpenGLDevice& = delete;

    [[nodiscard]] auto backend() const -> Backend override;

    auto createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
        -> Result<BufferHandle, GfxError> override;
    auto createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> override;
    auto createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> override;
    auto createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> override;
    auto createPipeline(const PipelineStateDesc& desc) -> Result<PipelineHandle, GfxError> override;
    auto createSwapchain(const SwapchainDesc& desc) -> Result<SwapchainHandle, GfxError> override;
    auto createFramebuffer(const FramebufferDesc& desc)
        -> Result<FramebufferHandle, GfxError> override;

    auto destroyBuffer(BufferHandle) -> GfxError override;
    auto destroyTexture(TextureHandle) -> GfxError override;
    auto destroySampler(SamplerHandle) -> GfxError override;
    auto destroyShader(ShaderHandle) -> GfxError override;
    auto destroyPipeline(PipelineHandle) -> GfxError override;
    auto destroySwapchain(SwapchainHandle) -> GfxError override;
    auto destroyFramebuffer(FramebufferHandle) -> GfxError override;

    auto writeBuffer(BufferHandle, std::uint64_t offset, std::span<const std::byte> data)
        -> GfxError override;
    auto readBuffer(BufferHandle, std::uint64_t offset, std::span<std::byte> destination)
        -> GfxError override;
    auto writeTexture(TextureHandle, std::uint32_t mipLevel, std::span<const std::byte> data)
        -> GfxError override;
    auto submit(CommandBuffer&) -> GfxError override;
    auto waitIdle() -> void override;

    // --- Host-side convenience ---------------------------------------------
    // Fast-path helper the demo/tests use instead of recording a buffer upload:
    // does createBuffer + writeBuffer with one GL allocation, returning a
    // handle. (Optional; tests may also use the abstract path only.)
    auto createBufferUpload(const BufferDesc& desc, std::span<const std::byte> data)
        -> Result<BufferHandle, GfxError>;

    // Expose the GL enum name of the active context extension set for logging.
    [[nodiscard]] auto renderer() const -> const char* { return renderer_; }

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

private:
    friend auto createOpenGLDevice() -> IDevice*;

    OpenGLDevice();

    struct Impl;
    Impl* impl_;
    const char* renderer_ = nullptr;
};

// Factory declared in device.cpp under F4_GRAPHICS_BACKEND_opengl.
auto createOpenGLDevice() -> IDevice*;

} // namespace f4::gfx
