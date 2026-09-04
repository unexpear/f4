#pragma once

#include <cstdint>

#include "graphics/device/device.h"

namespace f4::gfx {

class WebGpuDevice final : public IDevice {
public:
    ~WebGpuDevice() override;

    WebGpuDevice(const WebGpuDevice&) = delete;
    auto operator=(const WebGpuDevice&) -> WebGpuDevice& = delete;

    [[nodiscard]] auto backend() const -> Backend override;
    [[nodiscard]] auto ready() const -> bool;
    [[nodiscard]] auto preferredSurfaceFormat() const -> TextureFormat;
    [[nodiscard]] auto supportsSurfaceFormat(TextureFormat) const -> bool;
    [[nodiscard]] auto maxTextureDimension2D() const -> std::uint32_t;
    [[nodiscard]] auto validationErrorCount() const -> std::uint64_t;

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
    auto readTexture(TextureHandle, std::uint32_t, std::span<std::byte>) -> GfxError;
    auto writeTexture(TextureHandle, std::uint32_t, std::span<const std::byte>)
        -> GfxError override;
    auto submit(CommandBuffer&) -> GfxError override;
    auto waitIdle() -> void override;

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

private:
    explicit WebGpuDevice(const char* canvasSelector);

    struct Impl;
    Impl* impl_ = nullptr;

    friend auto createWebGpuDevice(const char*) -> IDevice*;
};

[[nodiscard]] auto createWebGpuDevice(const char* canvasSelector = "#canvas") -> IDevice*;

} // namespace f4::gfx
