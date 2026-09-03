#pragma once

#include <cstdint>

#include "graphics/device/device.h"

namespace f4::platform {
class Window;
}

namespace f4::gfx {

class Dx12Device final : public IDevice {
public:
    ~Dx12Device() override;

    Dx12Device(const Dx12Device&) = delete;
    auto operator=(const Dx12Device&) -> Dx12Device& = delete;

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
    auto submit(CommandBuffer&) -> GfxError override;
    auto waitIdle() -> void override;

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

    [[nodiscard]] auto deviceName() const -> const char*;
    [[nodiscard]] auto featureLevel() const -> std::uint32_t;
    [[nodiscard]] auto debugLayerEnabled() const -> bool;
    [[nodiscard]] auto validationWarningCount() const -> std::uint64_t;
    [[nodiscard]] auto validationErrorCount() const -> std::uint64_t;
    [[nodiscard]] auto hasWindow() const -> bool;
    [[nodiscard]] auto preferredSurfaceFormat() const -> TextureFormat;
    [[nodiscard]] auto framesInFlight() const -> std::uint32_t;

    auto readTexture(TextureHandle texture, std::uint32_t mipLevel,
                     std::span<std::byte> destination) -> GfxError;

private:
    friend auto createDx12Device(platform::Window*) -> IDevice*;

    explicit Dx12Device(platform::Window* window);
    [[nodiscard]] auto initialize(platform::Window* window) -> bool;
    [[nodiscard]] auto ready() const -> bool;
    void collectRetired();

    struct Impl;
    Impl* impl_ = nullptr;
};

// A null window creates a headless D3D12 device. Passing a native-capable SDL
// window enables DXGI swapchain creation and presentation.
[[nodiscard]] auto createDx12Device(platform::Window* window = nullptr) -> IDevice*;

} // namespace f4::gfx
