#pragma once

#include <cstdint>

#include "graphics/device/device.h"

namespace f4::platform {
class Window;
}

namespace f4::gfx {

// Vulkan 1.3 device foundation. Resource and command translation are added in
// the following M9 slices; unsupported calls return BackendUnavailable rather
// than silently behaving like the NullDevice.
class VulkanDevice final : public IDevice {
public:
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    auto operator=(const VulkanDevice&) -> VulkanDevice& = delete;

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
    auto writeTexture(TextureHandle, std::uint32_t, std::span<const std::byte>)
        -> GfxError override;
    auto submit(CommandBuffer&) -> GfxError override;
    auto waitIdle() -> void override;

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

    [[nodiscard]] auto deviceName() const -> const char*;
    [[nodiscard]] auto apiVersion() const -> std::uint32_t;
    [[nodiscard]] auto graphicsQueueFamily() const -> std::uint32_t;
    [[nodiscard]] auto presentQueueFamily() const -> std::uint32_t;
    [[nodiscard]] auto validationEnabled() const -> bool;
    [[nodiscard]] auto synchronizationValidationEnabled() const -> bool;
    [[nodiscard]] auto validationWarningCount() const -> std::uint64_t;
    [[nodiscard]] auto validationErrorCount() const -> std::uint64_t;
    [[nodiscard]] auto hasSurface() const -> bool;
    [[nodiscard]] auto preferredSurfaceFormat() const -> TextureFormat;
    [[nodiscard]] auto framesInFlight() const -> std::uint32_t;

    // Readback surface used by Vulkan conformance tests and capture tooling.
    auto readTexture(TextureHandle texture, std::uint32_t mipLevel,
                     std::span<std::byte> destination) -> GfxError;

private:
    friend auto createVulkanDevice(platform::Window*) -> IDevice*;

    explicit VulkanDevice(platform::Window* window);
    [[nodiscard]] auto initialize(platform::Window* window) -> bool;
    [[nodiscard]] auto ready() const -> bool;
    void collectRetired();

    struct Impl;
    Impl* impl_ = nullptr;
};

// A null window creates a headless graphics/compute device. Passing a Vulkan
// Window also creates a VkSurfaceKHR and selects a presentation-capable queue.
[[nodiscard]] auto createVulkanDevice(platform::Window* window = nullptr) -> IDevice*;

} // namespace f4::gfx
