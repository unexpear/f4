#pragma once

#include <cstdint>

#include "graphics/device/device.h"

namespace f4::platform {
class Window;
}

namespace f4::gfx {

class MetalDevice final : public IDevice {
public:
    ~MetalDevice() override;

    MetalDevice(const MetalDevice&) = delete;
    auto operator=(const MetalDevice&) -> MetalDevice& = delete;

    [[nodiscard]] auto backend() const -> Backend override;

    auto createBuffer(const BufferDesc&, std::span<const std::byte>)
        -> Result<BufferHandle, GfxError> override;
    auto createTexture(const TextureDesc&) -> Result<TextureHandle, GfxError> override;
    auto createSampler(const SamplerDesc&) -> Result<SamplerHandle, GfxError> override;
    auto createShader(const ShaderDesc&) -> Result<ShaderHandle, GfxError> override;
    auto createPipeline(const PipelineStateDesc&) -> Result<PipelineHandle, GfxError> override;
    auto createSwapchain(const SwapchainDesc&) -> Result<SwapchainHandle, GfxError> override;
    auto createFramebuffer(const FramebufferDesc&)
        -> Result<FramebufferHandle, GfxError> override;

    auto destroyBuffer(BufferHandle) -> GfxError override;
    auto destroyTexture(TextureHandle) -> GfxError override;
    auto destroySampler(SamplerHandle) -> GfxError override;
    auto destroyShader(ShaderHandle) -> GfxError override;
    auto destroyPipeline(PipelineHandle) -> GfxError override;
    auto destroySwapchain(SwapchainHandle) -> GfxError override;
    auto destroyFramebuffer(FramebufferHandle) -> GfxError override;

    auto writeBuffer(BufferHandle, std::uint64_t, std::span<const std::byte>)
        -> GfxError override;
    auto readBuffer(BufferHandle, std::uint64_t, std::span<std::byte>)
        -> GfxError override;
    auto writeTexture(TextureHandle, std::uint32_t, std::span<const std::byte>)
        -> GfxError override;
    auto readTexture(TextureHandle, std::uint32_t, std::span<std::byte>) -> GfxError;
    auto submit(CommandBuffer&) -> GfxError override;
    auto waitIdle() -> void override;

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

    [[nodiscard]] auto ready() const -> bool;
    [[nodiscard]] auto deviceName() const -> const char*;
    [[nodiscard]] auto hasWindow() const -> bool;
    [[nodiscard]] auto preferredSurfaceFormat() const -> TextureFormat;
    [[nodiscard]] auto framesInFlight() const -> std::uint32_t;
    [[nodiscard]] auto validationWarningCount() const -> std::uint64_t;
    [[nodiscard]] auto validationErrorCount() const -> std::uint64_t;

private:
    friend auto createMetalDevice(platform::Window*) -> IDevice*;

    explicit MetalDevice(platform::Window* window);
    [[nodiscard]] auto initialize(platform::Window* window) -> bool;
    void collectRetired();

    struct Impl;
    Impl* impl_ = nullptr;
};

// A null window creates a headless Metal device for offscreen conformance.
// Passing a Window created with Window::createMetal enables CAMetalLayer
// presentation without exposing Objective-C or Metal types above this backend.
[[nodiscard]] auto createMetalDevice(platform::Window* window = nullptr) -> IDevice*;

} // namespace f4::gfx
