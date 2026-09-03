#include "backends/null/null_device.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "core/assert.h"
#include "graphics/device/object_store.h"

namespace f4::gfx {

namespace {

struct NullBuffer {
    GpuBuffer rec;
    std::vector<std::byte> data;
};

} // namespace

struct NullDevice::Impl {
    ObjectStore<NullBuffer, GpuBufferTag> buffers;
    ObjectStore<GpuTexture, GpuTextureTag> textures;
    ObjectStore<GpuSampler, GpuSamplerTag> samplers;
    ObjectStore<GpuShader, GpuShaderTag> shaders;
    ObjectStore<GpuPipeline, GpuPipelineTag> pipelines;
    ObjectStore<GpuSwapchain, GpuSwapchainTag> swapchains;
    ObjectStore<GpuFramebuffer, GpuFramebufferTag> framebuffers;
};

// Primitives produced by `count` indices/vertices for the given topology.
[[nodiscard]] auto countPrimitives(PrimitiveTopology topology, std::uint64_t count)
    -> std::uint64_t {
    switch (topology) {
    case PrimitiveTopology::PointList: return count;
    case PrimitiveTopology::LineList: return count / 2;
    case PrimitiveTopology::TriangleList: return count / 3;
    case PrimitiveTopology::TriangleStrip: return count > 2 ? count - 2 : 0;
    }
    return 0;
}

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
    NullBuffer record;
    record.rec.sizeBytes = desc.sizeBytes;
    record.rec.usage = desc.usage;
    record.data.resize(static_cast<std::size_t>(desc.sizeBytes));
    if (!initialData.empty()) {
        std::copy(initialData.begin(), initialData.end(), record.data.begin());
    }
    const BufferHandle handle = impl_->buffers.create(record);
    // Track bytes for the accounting surface.
    bufferBytes_ += desc.sizeBytes;
    lastError_ = GfxError::None;
    return handle;
}

auto NullDevice::createTexture(const TextureDesc& desc) -> Result<TextureHandle, GfxError> {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > maxTextureMipLevels(desc.width, desc.height)) {
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
    record.mipFilter = desc.mipFilter;
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
    ++stats_.shaderCompiled;
    return impl_->shaders.create(record);
}

auto NullDevice::createPipeline(const PipelineStateDesc& desc) -> Result<PipelineHandle, GfxError> {
    if (desc.layout.pushConstantSizeBytes != 0) return GfxError::BadDescriptor;
    const bool compute = desc.computeShader.isValid();
    const bool hasVertex = desc.vertexShader.isValid();
    const bool hasFragment = desc.fragmentShader.isValid();
    if ((compute && (hasVertex || hasFragment)) ||
        (!compute && (!hasVertex || !hasFragment))) {
        return GfxError::BadDescriptor;
    }
    if (compute && (!desc.vertexInput.bindings.isEmpty() ||
                    !desc.vertexInput.attributes.isEmpty() ||
                    !desc.colorAttachments.isEmpty() ||
                    desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable)) {
        return GfxError::BadDescriptor;
    }
    const std::uint32_t allowedStages = compute
        ? shaderStageBit(ShaderStage::Compute)
        : shaderStageBit(ShaderStage::Vertex) | shaderStageBit(ShaderStage::Fragment);
    for (const BindGroup& group : desc.layout.groups) {
        for (const BindingSlot& slot : group.slots) {
            if (slot.count != 1 || (slot.stageMask & ~allowedStages) != 0) {
                return GfxError::BadDescriptor;
            }
        }
    }
    // Validate that referenced shader handles are live.
    if (desc.vertexShader.isValid() && !impl_->shaders.contains(desc.vertexShader)) {
        return GfxError::InvalidHandle;
    }
    if (desc.fragmentShader.isValid() && !impl_->shaders.contains(desc.fragmentShader)) {
        return GfxError::InvalidHandle;
    }
    if (desc.computeShader.isValid() && !impl_->shaders.contains(desc.computeShader)) {
        return GfxError::InvalidHandle;
    }
    const GpuShader* vertex = impl_->shaders.get(desc.vertexShader);
    const GpuShader* fragment = impl_->shaders.get(desc.fragmentShader);
    const GpuShader* computeShader = impl_->shaders.get(desc.computeShader);
    if ((vertex != nullptr && vertex->stage != ShaderStage::Vertex) ||
        (fragment != nullptr && fragment->stage != ShaderStage::Fragment) ||
        (computeShader != nullptr && computeShader->stage != ShaderStage::Compute)) {
        return GfxError::BadDescriptor;
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
    NullBuffer* buf = impl_->buffers.get(h);
    if (buf == nullptr) {
        return GfxError::StaleHandle;
    }
    if (offset > buf->rec.sizeBytes || data.size() > buf->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor; // out of bounds
    }
    std::copy(data.begin(), data.end(), buf->data.begin() + static_cast<std::size_t>(offset));
    return GfxError::None;
}

auto NullDevice::readBuffer(BufferHandle h, std::uint64_t offset,
                            std::span<std::byte> destination) -> GfxError {
    const NullBuffer* buf = impl_->buffers.get(h);
    if (buf == nullptr) return GfxError::StaleHandle;
    if (offset > buf->rec.sizeBytes || destination.size() > buf->rec.sizeBytes - offset) {
        return GfxError::BadDescriptor;
    }
    std::copy_n(buf->data.begin() + static_cast<std::size_t>(offset), destination.size(),
                destination.begin());
    return GfxError::None;
}

auto NullDevice::writeTexture(TextureHandle h, std::uint32_t mipLevel,
                              std::span<const std::byte> data) -> GfxError {
    GpuTexture* tex = impl_->textures.get(h);
    if (tex == nullptr) {
        return GfxError::StaleHandle;
    }
    if (mipLevel >= tex->mipLevels) {
        return GfxError::BadDescriptor;
    }
    std::uint32_t w = tex->width >> mipLevel;
    std::uint32_t hh = tex->height >> mipLevel;
    if (w == 0) {
        w = 1;
    }
    if (hh == 0) {
        hh = 1;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(hh) *
        textureFormatBytes(tex->format);
    if (data.size() != expected) {
        return GfxError::BadDescriptor; // footprint mismatch
    }
    return GfxError::None;
}

auto NullDevice::submit(CommandBuffer& cb) -> GfxError {
    GfxError err = GfxError::None;

    bool renderPassActive = false;
    bool pipelineBound = false;
    bool computePipeline = false;
    bool indexBound = false;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;

    for (const Command& cmd : cb.commands()) {
        switch (cmd.kind) {
        case CommandKind::ResourceBarrier:
            if (cmd.barrierType == BarrierResourceType::Texture) {
                if (!impl_->textures.contains(cmd.texture)) err = GfxError::InvalidHandle;
            } else if (!impl_->buffers.contains(cmd.buffer)) {
                err = GfxError::InvalidHandle;
            }
            break;
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
                GpuPipeline* p = impl_->pipelines.get(cmd.pipeline);
                if (p != nullptr) {
                    topology = p->desc.topology;
                    computePipeline = p->desc.computeShader.isValid();
                    if (renderPassActive && computePipeline) err = GfxError::BadDescriptor;
                }
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
                const BoundResource& res = cmd.resources[ri];
                if (res.type == BindingType::Texture) {
                    TextureHandle th;
                    th.index = res.handleIndex;
                    th.generation = res.handleGeneration;
                    if (!impl_->textures.contains(th)) {
                        err = GfxError::InvalidHandle;
                    }
                } else if (res.type == BindingType::Sampler) {
                    SamplerHandle sh;
                    sh.index = res.handleIndex;
                    sh.generation = res.handleGeneration;
                    if (!impl_->samplers.contains(sh)) {
                        err = GfxError::InvalidHandle;
                    }
                } else {
                    BufferHandle bh;
                    bh.index = res.handleIndex;
                    bh.generation = res.handleGeneration;
                    if (!impl_->buffers.contains(bh)) {
                        err = GfxError::InvalidHandle;
                    }
                }
                if (err != GfxError::None) {
                    break;
                }
            }
            break;
        case CommandKind::SetViewport:
        case CommandKind::SetScissor:
            break;
        case CommandKind::Draw: {
            if (!renderPassActive) {
                err = GfxError::BadDescriptor; // draw outside a render pass
            } else if (!pipelineBound || computePipeline) {
                err = GfxError::BadDescriptor; // no pipeline bound
            } else {
                ++stats_.drawCalls;
                ++stats_.batches;
                stats_.instances += cmd.instanceCount;
                stats_.vertices += static_cast<std::uint64_t>(cmd.vertexCount) * cmd.instanceCount;
                stats_.primitives += countPrimitives(topology, cmd.vertexCount) * cmd.instanceCount;
            }
            break;
        }
        case CommandKind::DrawIndexed: {
            if (!renderPassActive) {
                err = GfxError::BadDescriptor;
            } else if (!pipelineBound || computePipeline) {
                err = GfxError::BadDescriptor;
            } else if (!indexBound) {
                err = GfxError::BadDescriptor;
            } else {
                ++stats_.drawIndexedCalls;
                ++stats_.batches;
                stats_.instances += cmd.instanceCount;
                stats_.vertices += static_cast<std::uint64_t>(cmd.indexCount) * cmd.instanceCount;
                stats_.primitives += countPrimitives(topology, cmd.indexCount) * cmd.instanceCount;
            }
            break;
        }
        case CommandKind::Dispatch:
            if (renderPassActive || !pipelineBound || !computePipeline ||
                cmd.groupCountX == 0 || cmd.groupCountY == 0 || cmd.groupCountZ == 0) {
                err = GfxError::BadDescriptor;
            } else {
                ++stats_.dispatchCalls;
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

auto NullDevice::stats() const -> DeviceStats {
    DeviceStats s = stats_;
    s.bufferCount = impl_->buffers.liveCount();
    s.bufferBytes = 0;
    impl_->buffers.forEach([&](const NullBuffer* b) { s.bufferBytes += b->rec.sizeBytes; });
    s.textureCount = impl_->textures.liveCount();
    s.textureBytes = 0;
    impl_->textures.forEach([&](const GpuTexture* t) { s.textureBytes += textureBytes(*t); });
    s.samplerCount = impl_->samplers.liveCount();
    s.shaderCount = impl_->shaders.liveCount();
    s.pipelineCount = impl_->pipelines.liveCount();
    s.framebufferCount = impl_->framebuffers.liveCount();
    s.swapchainCount = impl_->swapchains.liveCount();
    return s;
}

auto NullDevice::resetFrameStats() -> void { stats_ = DeviceStats{}; }

auto NullDevice::waitIdle() -> void {
    // Nothing to wait on — the null device is synchronous.
}

auto NullDevice::liveBuffers() const -> std::size_t { return impl_->buffers.liveCount(); }

auto createNullDevice() -> IDevice* {
    return new NullDevice();
}

} // namespace f4::gfx
