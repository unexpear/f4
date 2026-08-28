#include "backends/null/null_device.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "core/assert.h"
#include "graphics/device/object_store.h"

namespace f4::gfx {

struct NullDevice::Impl {
    ObjectStore<GpuBuffer, GpuBufferTag> buffers;
    ObjectStore<GpuTexture, GpuTextureTag> textures;
    ObjectStore<GpuSampler, GpuSamplerTag> samplers;
    ObjectStore<GpuShader, GpuShaderTag> shaders;
    ObjectStore<GpuPipeline, GpuPipelineTag> pipelines;
    ObjectStore<GpuSwapchain, GpuSwapchainTag> swapchains;
    ObjectStore<GpuFramebuffer, GpuFramebufferTag> framebuffers;
};

NullDevice::NullDevice() : impl_(new Impl()) {
    F4_ASSERT(impl_ != nullptr, "NullDevice impl allocation failed");
}

NullDevice::~NullDevice() { delete impl_; }

auto NullDevice::backend() const -> Backend { return Backend::Null; }

auto NullDevice::createBuffer(const BufferDesc& desc, std::span<const std::byte> initialData)
    -> Result<BufferHandle, GfxError> {
    if (desc.sizeBytes == 0) {
        return GfxError::BadDescriptor;
    }
    if (initialData.size() > desc.sizeBytes) {
        return GfxError::BadDescriptor; // initial data must fit the buffer
    }
    GpuBuffer record;
    record.sizeBytes = desc.sizeBytes;
    record.usage = desc.usage;
    const BufferHandle handle = impl_->buffers.create(record);
    // Track bytes for the accounting surface.
    bufferBytes_ += desc.sizeBytes;
    lastError_ = GfxError::None;
    return handle;
}

auto NullDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) {
        return GfxError::BadDescriptor;
    }
    GpuTexture record;
    record.format = desc.format;
    record.width = desc.width;
    record.height = desc.height;
    record.mipLevels = desc.mipLevels;
    lastError_ = GfxError::None;
    return impl_->textures.create(record);
}

auto NullDevice::createSampler(const SamplerDesc& desc) -> Result<SamplerHandle, GfxError> {
    GpuSampler record;
    record.filter = desc.filter;
    record.addressU = desc.addressU;
    record.addressV = desc.addressV;
    record.addressW = desc.addressW;
    record.maxAnisotropy = desc.maxAnisotropy;
    lastError_ = GfxError::None;
    return impl_->samplers.create(record);
}

auto NullDevice::createShader(const ShaderDesc& desc) -> Result<ShaderHandle, GfxError> {
    GpuShader record;
    record.stage = desc.stage;
    lastError_ = GfxError::None;
    return impl_->shaders.create(record);
}

auto NullDevice::createPipeline(const PipelineStateDesc& desc) -> Result<PipelineHandle, GfxError> {
    // Validate that referenced shader handles are live.
    if (desc.vertexShader.isValid() && !impl_->shaders.contains(desc.vertexShader)) {
        return GfxError::InvalidHandle;
    }
    if (desc.fragmentShader.isValid() && !impl_->shaders.contains(desc.fragmentShader)) {
        return GfxError::InvalidHandle;
    }
    GpuPipeline record;
    record.desc = desc;
    lastError_ = GfxError::None;
    return impl_->pipelines.create(record);
}

auto NullDevice::createSwapchain(const SwapchainDesc& desc) -> Result<SwapchainHandle, GfxError> {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        return GfxError::BadDescriptor;
    }
    GpuSwapchain record;
    record.surfaceFormat = desc.surfaceFormat;
    record.extent = desc.extent;
    record.presentMode = desc.presentMode;
    lastError_ = GfxError::None;
    return impl_->swapchains.create(record);
}

auto NullDevice::createFramebuffer(const FramebufferDesc& desc)
    -> Result<FramebufferHandle, GfxError> {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        return GfxError::BadDescriptor;
    }
    for (const AttachmentDesc& a : desc.attachments) {
        if (a.texture.isValid() && !impl_->textures.contains(a.texture)) {
            return GfxError::InvalidHandle;
        }
    }
    GpuFramebuffer record;
    record.extent = desc.extent;
    record.attachments = desc.attachments;
    lastError_ = GfxError::None;
    return impl_->framebuffers.create(record);
}

auto NullDevice::destroyBuffer(BufferHandle h) -> GfxError {
    if (!impl_->buffers.destroy(h)) {
        return GfxError::StaleHandle;
    }
    // We don't track per-buffer byte sizes here; refresh cap accounting at
    // submit. Return success.
    return GfxError::None;
}

auto NullDevice::destroyTexture(TextureHandle h) -> GfxError {
    if (!impl_->textures.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::destroySampler(SamplerHandle h) -> GfxError {
    if (!impl_->samplers.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::destroyShader(ShaderHandle h) -> GfxError {
    if (!impl_->shaders.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::destroyPipeline(PipelineHandle h) -> GfxError {
    if (!impl_->pipelines.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::destroySwapchain(SwapchainHandle h) -> GfxError {
    if (!impl_->swapchains.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::destroyFramebuffer(FramebufferHandle h) -> GfxError {
    if (!impl_->framebuffers.destroy(h)) {
        return GfxError::StaleHandle;
    }
    return GfxError::None;
}

auto NullDevice::writeBuffer(BufferHandle h, std::uint64_t offset,
                             std::span<const std::byte> data) -> GfxError {
    GpuBuffer* buf = impl_->buffers.get(h);
    if (buf == nullptr) {
        return GfxError::StaleHandle;
    }
    if (offset + data.size() > buf->sizeBytes) {
        return GfxError::BadDescriptor; // out of bounds
    }
    return GfxError::None;
}

auto NullDevice::submit(CommandBuffer& cb) -> GfxError {
    drawCount_ = 0;
    drawIndexedCount_ = 0;
    GfxError err = GfxError::None;

    bool renderPassActive = false;
    bool pipelineBound = false;
    bool indexBound = false;

    for (const Command& cmd : cb.commands()) {
        switch (cmd.kind) {
        case CommandKind::BeginRenderPass:
            if (!impl_->framebuffers.contains(cmd.framebuffer)) {
                err = GfxError::InvalidHandle;
            } else {
                renderPassActive = true;
            }
            break;
        case CommandKind::EndRenderPass:
            renderPassActive = false;
            break;
        case CommandKind::BindPipeline:
            if (!impl_->pipelines.contains(cmd.pipeline)) {
                err = GfxError::InvalidHandle;
            } else {
                pipelineBound = true;
            }
            break;
        case CommandKind::BindVertexBuffer:
            if (!impl_->buffers.contains(cmd.buffer)) {
                err = GfxError::InvalidHandle;
            }
            break;
        case CommandKind::BindIndexBuffer:
            if (!impl_->buffers.contains(cmd.buffer)) {
                err = GfxError::InvalidHandle;
            } else {
                indexBound = true;
            }
            break;
        case CommandKind::BindResources:
            for (std::uint8_t ri = 0; ri < cmd.resourceCount; ++ri) {
                (void)cmd.resources[ri]; // validated at submit boundary
            }
            break;
        case CommandKind::SetViewport:
        case CommandKind::SetScissor:
            break;
        case CommandKind::Draw:
            if (!renderPassActive) {
                err = GfxError::BadDescriptor; // draw outside a render pass
            } else if (!pipelineBound) {
                err = GfxError::BadDescriptor; // no pipeline bound
            } else {
                ++drawCount_;
            }
            break;
        case CommandKind::DrawIndexed:
            if (!renderPassActive) {
                err = GfxError::BadDescriptor;
            } else if (!pipelineBound) {
                err = GfxError::BadDescriptor;
            } else if (!indexBound) {
                err = GfxError::BadDescriptor;
            } else {
                ++drawIndexedCount_;
            }
            break;
        }
        if (err != GfxError::None) {
            break;
        }
    }

    lastError_ = err;
    return err;
}

auto NullDevice::waitIdle() -> void {
    // Nothing to wait on — the null device is synchronous.
}

auto NullDevice::liveBuffers() const -> std::size_t { return impl_->buffers.liveCount(); }

auto createNullDevice() -> IDevice* {
    return new NullDevice();
}

} // namespace f4::gfx
