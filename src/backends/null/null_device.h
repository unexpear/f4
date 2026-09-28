#pragma once

#include <optional>

#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"

namespace f4::gfx {

// Software RHI backend with no window/GPU. Validates handles, records command
// state, and replays recorded commands against a state machine (pipeline bound
// before draw, index buffer before drawIndexed, active render pass), reporting
// the first error. Exposes a draw/memory log for headless conformance tests.
class NullDevice final : public IDevice {
public:
    NullDevice();
    ~NullDevice() override;

    NullDevice(const NullDevice&) = delete;
    auto operator=(const NullDevice&) -> NullDevice& = delete;

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

    [[nodiscard]] auto stats() const -> DeviceStats override;
    auto resetFrameStats() -> void override;

    // --- Test surface -------------------------------------------------------
    [[nodiscard]] auto lastSubmissionError() const -> GfxError { return lastError_; }
    [[nodiscard]] auto drawCount() const -> std::uint64_t { return stats_.drawCalls; }
    [[nodiscard]] auto drawIndexedCount() const -> std::uint64_t { return stats_.drawIndexedCalls; }
    [[nodiscard]] auto dispatchCount() const -> std::uint64_t { return stats_.dispatchCalls; }
    [[nodiscard]] auto bufferBytes() const -> std::uint64_t { return bufferBytes_; }
    [[nodiscard]] auto liveBuffers() const -> std::size_t;
    void failNextPipelineCreation(GfxError error) { nextPipelineError_ = error; }
    // One-shot upload fault after N successful buffer creations (headless tests).
    void failBufferCreationAfter(std::size_t successes) { bufferFailureCountdown_ = successes; }

private:
    struct Impl;
    Impl* impl_;
    GfxError lastError_ = GfxError::None;
    GfxError nextPipelineError_ = GfxError::None;
    std::optional<std::size_t> bufferFailureCountdown_;
    DeviceStats stats_;
    std::uint64_t bufferBytes_ = 0;
};

} // namespace f4::gfx
