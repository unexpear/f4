#include "graphics/render_graph/render_graph.h"

#include <algorithm>
#include <limits>

#include "core/assert.h"

namespace f4::gfx {

namespace {
constexpr std::uint32_t noPass = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t writeAccess = ResourceAccessMask::ShaderWrite |
    ResourceAccessMask::ColorWrite | ResourceAccessMask::DepthWrite |
    ResourceAccessMask::TransferWrite | ResourceAccessMask::HostWrite;
} // namespace

auto RenderGraph::declareResource(ResourceDesc desc) -> ResourceId {
    const ResourceId id = static_cast<ResourceId>(resources_.size());
    resources_.push(static_cast<ResourceDesc&&>(desc));
    compiled_ = false;
    order_.clear();
    return id;
}

auto RenderGraph::importTexture(String name, TextureHandle texture) -> ResourceId {
    ResourceDesc desc;
    desc.name = static_cast<String&&>(name);
    desc.type = ResourceType::Texture;
    desc.lifetime = ResourceLifetime::Persistent;
    desc.importedTexture = texture;
    return declareResource(static_cast<ResourceDesc&&>(desc));
}

auto RenderGraph::importFramebuffer(String name, FramebufferHandle framebuffer) -> ResourceId {
    ResourceDesc desc;
    desc.name = static_cast<String&&>(name);
    desc.type = ResourceType::Framebuffer;
    desc.lifetime = ResourceLifetime::Persistent;
    desc.importedFramebuffer = framebuffer;
    return declareResource(static_cast<ResourceDesc&&>(desc));
}

auto RenderGraph::declarePass(PassDesc desc) -> std::uint32_t {
    const std::uint32_t idx = static_cast<std::uint32_t>(passes_.size());
    passes_.push(static_cast<PassDesc&&>(desc));
    compiled_ = false;
    order_.clear();
    return idx;
}

auto RenderGraph::clear(IDevice& dev) -> GfxError {
    const GfxError result = release(dev);
    if (result != GfxError::None) return result;
    resources_.clear();
    passes_.clear();
    order_.clear();
    compiled_ = false;
    return GfxError::None;
}

auto RenderGraph::validateResources() const -> GfxError {
    for (const ResourceDesc& resource : resources_) {
        switch (resource.type) {
        case ResourceType::Texture:
            if (!resource.importedTexture.isValid() &&
                (resource.texture.width == 0 || resource.texture.height == 0 ||
                 resource.texture.mipLevels == 0 ||
                 resource.texture.mipLevels >
                     maxTextureMipLevels(resource.texture.width, resource.texture.height))) {
                return GfxError::BadDescriptor;
            }
            if (resource.importedTexture.isValid() &&
                (resource.texture.width != 0 || resource.texture.height != 0 ||
                 resource.texture.mipLevels != 1 ||
                 resource.lifetime == ResourceLifetime::Transient)) {
                return GfxError::BadDescriptor;
            }
            break;
        case ResourceType::Buffer:
            if (resource.buffer.sizeBytes == 0) {
                return GfxError::BadDescriptor;
            }
            break;
        case ResourceType::Framebuffer:
            if (!resource.importedFramebuffer.isValid() &&
                (resource.framebuffer.extent.width == 0 || resource.framebuffer.extent.height == 0)) {
                return GfxError::BadDescriptor;
            }
            if (resource.importedFramebuffer.isValid() &&
                (resource.framebuffer.extent.width != 0 ||
                 resource.framebuffer.extent.height != 0 ||
                 !resource.framebuffer.attachments.isEmpty())) {
                return GfxError::BadDescriptor;
            }
            if (resource.importedFramebuffer.isValid() &&
                resource.lifetime == ResourceLifetime::Transient) {
                return GfxError::BadDescriptor;
            }
            break;
        }
    }
    return GfxError::None;
}

auto RenderGraph::validatePasses() const -> GfxError {
    constexpr std::uint32_t allStages = PipelineStageMask::VertexInput |
                                        PipelineStageMask::VertexShader |
                                        PipelineStageMask::FragmentShader |
                                        PipelineStageMask::ColorAttachment |
                                        PipelineStageMask::DepthStencil |
                                        PipelineStageMask::Transfer |
                                        PipelineStageMask::Host |
                                        PipelineStageMask::AllGraphics |
                                        PipelineStageMask::AllCommands |
                                        PipelineStageMask::ComputeShader;
    constexpr std::uint32_t allAccess = ResourceAccessMask::VertexRead |
                                        ResourceAccessMask::IndexRead |
                                        ResourceAccessMask::UniformRead |
                                        ResourceAccessMask::ShaderRead |
                                        ResourceAccessMask::ShaderWrite |
                                        ResourceAccessMask::ColorRead |
                                        ResourceAccessMask::ColorWrite |
                                        ResourceAccessMask::DepthRead |
                                        ResourceAccessMask::DepthWrite |
                                        ResourceAccessMask::TransferRead |
                                        ResourceAccessMask::TransferWrite |
                                        ResourceAccessMask::HostRead |
                                        ResourceAccessMask::HostWrite;
    for (const PassDesc& pass : passes_) {
        for (const std::uint32_t dependency : pass.dependsOn) {
            if (dependency >= passes_.size()) return GfxError::BadDescriptor;
        }
        std::uint32_t framebufferWrites = 0;
        for (const ResourceId id : pass.reads) {
            if (id >= resources_.size()) {
                return GfxError::BadDescriptor;
            }
        }
        for (const ResourceId id : pass.writes) {
            if (id >= resources_.size()) {
                return GfxError::BadDescriptor;
            }
            if (resources_[id].type == ResourceType::Framebuffer) {
                ++framebufferWrites;
            }
        }
        if (framebufferWrites > 1) {
            return GfxError::BadDescriptor;
        }
        for (const PassResourceAccess& access : pass.accesses) {
            if (access.resource >= resources_.size() || access.state.stages == 0 ||
                (access.state.stages & ~allStages) != 0 ||
                (access.state.access & ~allAccess) != 0) {
                return GfxError::BadDescriptor;
            }
            const ResourceType type = resources_[access.resource].type;
            if ((type == ResourceType::Buffer &&
                 access.state.layout != TextureLayout::Undefined) ||
                (type == ResourceType::Texture &&
                 access.state.layout == TextureLayout::Undefined)) {
                return GfxError::BadDescriptor;
            }
        }
    }
    return GfxError::None;
}

auto RenderGraph::compile() -> GfxError {
    if (compiled_) return GfxError::None;
    order_.clear();
    if (const GfxError error = validateResources(); error != GfxError::None) return error;
    if (const GfxError error = validatePasses(); error != GfxError::None) return error;

    struct Hazard {
        std::uint32_t writer = noPass;
        Array<std::uint32_t> readers;
        TextureLayout layout = TextureLayout::Undefined;
    };
    Array<Hazard> hazards;
    hazards.resize(resources_.size());
    Array<Array<std::uint32_t>> successors;
    successors.resize(passes_.size());
    Array<std::size_t> incoming;
    incoming.resize(passes_.size());
    const auto addEdge = [&](std::uint32_t from, std::uint32_t to) {
        successors[from].push(to);
        ++incoming[to];
    };

    Array<std::uint8_t> uses;
    uses.resize(resources_.size());
    Array<ResourceId> touched;
    const auto mark = [&](ResourceId resource, std::uint8_t use) {
        if (uses[resource] == 0) touched.push(resource);
        uses[resource] |= use;
    };
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        const auto id = static_cast<std::uint32_t>(i);
        const PassDesc& pass = passes_[i];
        for (const auto dependency : pass.dependsOn) addEdge(dependency, id);
        for (const auto resource : pass.reads) mark(resource, 1);
        for (const auto resource : pass.writes) mark(resource, 2);
        for (const auto& access : pass.accesses) {
            auto& hazard = hazards[access.resource];
            const bool layoutChange = resources_[access.resource].type == ResourceType::Texture &&
                hazard.layout != access.state.layout;
            // Layout transitions are exclusive even when both passes only read.
            mark(access.resource, (access.state.access & writeAccess) != 0 || layoutChange ? 2 : 1);
            hazard.layout = access.state.layout;
        }
        for (const auto resource : touched) {
            auto& hazard = hazards[resource];
            if (hazard.writer != noPass) addEdge(hazard.writer, id);
            if ((uses[resource] & 2) != 0) {
                for (const auto reader : hazard.readers) addEdge(reader, id);
                hazard.readers.clear();
                hazard.writer = id;
            } else {
                hazard.readers.push(id);
            }
            uses[resource] = 0;
        }
        touched.clear();
    }

    Array<std::uint32_t> ready;
    const auto enqueue = [&](std::uint32_t id) {
        ready.push(id);
        std::push_heap(ready.begin(), ready.end(), std::greater<std::uint32_t>{});
    };
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        if (incoming[i] == 0) enqueue(static_cast<std::uint32_t>(i));
    }
    Array<std::uint32_t> order;
    order.reserve(passes_.size());
    while (!ready.isEmpty()) {
        std::pop_heap(ready.begin(), ready.end(), std::greater<std::uint32_t>{});
        const auto id = ready[ready.size() - 1];
        ready.pop();
        order.push(id);
        for (const auto successor : successors[id]) {
            if (--incoming[successor] == 0) enqueue(successor);
        }
    }
    if (order.size() != passes_.size()) return GfxError::BadDescriptor;
    order_ = static_cast<Array<std::uint32_t>&&>(order);
    compiled_ = true;
    return GfxError::None;
}

auto RenderGraph::realize(IDevice& dev) -> GfxError {
    if (const GfxError error = validateResources(); error != GfxError::None) {
        return error;
    }
    if (owner_ != nullptr && owner_ != &dev) {
        return GfxError::InvalidHandle;
    }

    if (textures_.size() < resources_.size()) textures_.resize(resources_.size());
    if (buffers_.size() < resources_.size()) buffers_.resize(resources_.size());
    if (framebuffers_.size() < resources_.size()) framebuffers_.resize(resources_.size());
    if (states_.size() < resources_.size()) states_.resize(resources_.size());
    if (owned_.size() < resources_.size()) owned_.resize(resources_.size());

    Array<ResourceId> created;

    for (std::size_t i = 0; i < resources_.size(); ++i) {
        const ResourceDesc& r = resources_[i];
        const ResourceId id = static_cast<ResourceId>(i);
        GfxError error = GfxError::None;
        switch (r.type) {
        case ResourceType::Texture:
            if (!textures_[i].isValid()) {
                if (r.importedTexture.isValid()) {
                    textures_[i] = r.importedTexture;
                    owned_[i] = 0;
                    created.push(id);
                } else {
                    auto result = dev.createTexture(r.texture);
                    if (result.hasValue()) {
                        textures_[i] = result.value();
                        owned_[i] = 1;
                        created.push(id);
                    } else {
                        error = result.error();
                    }
                }
            }
            break;
        case ResourceType::Buffer:
            if (!buffers_[i].isValid()) {
                auto result = dev.createBuffer(r.buffer, {});
                if (result.hasValue()) {
                    buffers_[i] = result.value();
                    owned_[i] = 1;
                    created.push(id);
                } else {
                    error = result.error();
                }
            }
            break;
        case ResourceType::Framebuffer:
            if (!framebuffers_[i].isValid()) {
                if (r.importedFramebuffer.isValid()) {
                    framebuffers_[i] = r.importedFramebuffer;
                    owned_[i] = 0;
                    created.push(id);
                } else {
                    auto result = dev.createFramebuffer(r.framebuffer);
                    if (result.hasValue()) {
                        framebuffers_[i] = result.value();
                        owned_[i] = 1;
                        created.push(id);
                    } else {
                        error = result.error();
                    }
                }
            }
            break;
        }

        if (error != GfxError::None) {
            for (const ResourceId createdId : created) {
                (void)releaseResource(dev, createdId);
            }
            return error;
        }
    }
    owner_ = &dev;
    return GfxError::None;
}

auto RenderGraph::releaseResource(IDevice& dev, ResourceId id) -> GfxError {
    if (id >= resources_.size() || id >= owned_.size()) {
        return GfxError::BadDescriptor;
    }
    GfxError result = GfxError::None;
    if (owned_[id] != 0) {
        switch (resources_[id].type) {
        case ResourceType::Texture:
            if (textures_[id].isValid()) result = dev.destroyTexture(textures_[id]);
            break;
        case ResourceType::Buffer:
            if (buffers_[id].isValid()) result = dev.destroyBuffer(buffers_[id]);
            break;
        case ResourceType::Framebuffer:
            if (framebuffers_[id].isValid()) result = dev.destroyFramebuffer(framebuffers_[id]);
            break;
        }
    }
    textures_[id] = {};
    buffers_[id] = {};
    framebuffers_[id] = {};
    owned_[id] = 0;
    if (id < states_.size()) states_[id] = {};
    return result;
}

auto RenderGraph::releaseTransient(IDevice& dev) -> GfxError {
    GfxError firstError = GfxError::None;
    for (std::size_t i = 0; i < resources_.size(); ++i) {
        if (resources_[i].lifetime == ResourceLifetime::Transient) {
            const GfxError error = releaseResource(dev, static_cast<ResourceId>(i));
            if (firstError == GfxError::None && error != GfxError::None) firstError = error;
        }
    }
    return firstError;
}

auto RenderGraph::release(IDevice& dev) -> GfxError {
    if (owner_ != nullptr && owner_ != &dev) {
        return GfxError::InvalidHandle;
    }
    bool hasOwnedResources = false;
    for (std::size_t i = 0; i < owned_.size(); ++i) {
        hasOwnedResources = hasOwnedResources || owned_[i] != 0;
    }
    if (hasOwnedResources) dev.waitIdle();

    GfxError firstError = GfxError::None;
    for (std::size_t i = 0; i < resources_.size() && i < owned_.size(); ++i) {
        const GfxError error = releaseResource(dev, static_cast<ResourceId>(i));
        if (firstError == GfxError::None && error != GfxError::None) firstError = error;
    }
    textures_.clear();
    buffers_.clear();
    framebuffers_.clear();
    states_.clear();
    owned_.clear();
    owner_ = nullptr;
    return firstError;
}

auto RenderGraph::execute(IDevice& dev, CommandBuffer& cb) -> GfxError {
    if (cb.isRecording()) {
        return GfxError::BadDescriptor;
    }
    if (const GfxError error = compile(); error != GfxError::None) {
        return error;
    }
    if (const GfxError error = realize(dev); error != GfxError::None) {
        return error;
    }
    cb.begin();
    Array<ResourceState> nextStates = states_;
    for (const auto passId : order_) {
        const PassDesc& p = passes_[passId];
        for (const PassResourceAccess& access : p.accesses) {
            const ResourceId id = access.resource;
            // Equal layout/access masks do not eliminate a write dependency.
            if (nextStates[id] == access.state && (access.state.access & writeAccess) == 0) continue;
            if (resources_[id].type == ResourceType::Texture) {
                cb.textureBarrier(textures_[id], nextStates[id], access.state);
            } else if (resources_[id].type == ResourceType::Buffer) {
                cb.bufferBarrier(buffers_[id], nextStates[id], access.state);
            }
            nextStates[id] = access.state;
        }
        FramebufferHandle fb{};
        for (std::uint32_t rid : p.writes) {
            if (resources_[rid].type == ResourceType::Framebuffer) {
                fb = framebuffers_[rid];
                break;
            }
        }
        if (fb.isValid()) {
            cb.beginRenderPass(fb);
        }
        if (p.record) {
            p.record(cb);
        }
        if (fb.isValid()) {
            cb.endRenderPass();
        }
    }
    cb.end();
    const GfxError submitError = dev.submit(cb);
    if (submitError == GfxError::None) states_ = static_cast<Array<ResourceState>&&>(nextStates);
    // Synchronous backends can release immediately. Explicit backends retire
    // native objects against their submission fence while invalidating the RHI
    // handle here, so transient cleanup does not force a device-wide idle.
    const GfxError releaseError = releaseTransient(dev);
    return submitError != GfxError::None ? submitError : releaseError;
}

auto RenderGraph::texture(ResourceId id) const -> TextureHandle {
    return id < textures_.size() ? textures_[id] : TextureHandle{};
}

auto RenderGraph::buffer(ResourceId id) const -> BufferHandle {
    return id < buffers_.size() ? buffers_[id] : BufferHandle{};
}

auto RenderGraph::framebuffer(ResourceId id) const -> FramebufferHandle {
    return id < framebuffers_.size() ? framebuffers_[id] : FramebufferHandle{};
}

} // namespace f4::gfx
