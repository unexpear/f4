#include "graphics/render_graph/render_graph.h"

#include "core/assert.h"

namespace f4::gfx {

auto RenderGraph::declareResource(ResourceDesc desc) -> ResourceId {
    const ResourceId id = static_cast<ResourceId>(resources_.size());
    resources_.push(static_cast<ResourceDesc&&>(desc));
    return id;
}

auto RenderGraph::declarePass(PassDesc desc) -> std::uint32_t {
    const std::uint32_t idx = static_cast<std::uint32_t>(passes_.size());
    passes_.push(static_cast<PassDesc&&>(desc));
    return idx;
}

void RenderGraph::clear() {
    resources_.clear();
    passes_.clear();
    textures_.clear();
    buffers_.clear();
    framebuffers_.clear();
    realized_ = false;
}

auto RenderGraph::realize(IDevice& dev) -> GfxError {
    if (realized_) {
        return GfxError::None;
    }
    textures_.clear();
    buffers_.clear();
    framebuffers_.clear();
    textures_.resize(resources_.size());
    buffers_.resize(resources_.size());
    framebuffers_.resize(resources_.size());

    for (std::size_t i = 0; i < resources_.size(); ++i) {
        const ResourceDesc& r = resources_[i];
        if (r.type == ResourceType::Texture) {
            // Framebuffer resources are described via r.framebuffer (may be empty for plain textures)
            if (!r.framebuffer.attachments.isEmpty() || r.framebuffer.extent.width != 0) {
                auto fbr = dev.createFramebuffer(r.framebuffer);
                if (!fbr.hasValue()) return fbr.error();
                framebuffers_[i] = fbr.value();
            } else if (r.texture.width != 0 && r.texture.height != 0) {
                auto tr = dev.createTexture(r.texture);
                if (!tr.hasValue()) return tr.error();
                textures_[i] = tr.value();
            }
        } else if (r.type == ResourceType::Buffer) {
            if (r.buffer.sizeBytes == 0) {
                return GfxError::BadDescriptor;
            }
            auto br = dev.createBuffer(r.buffer, {});
            if (!br.hasValue()) return br.error();
            buffers_[i] = br.value();
        }
    }
    realized_ = true;
    return GfxError::None;
}

void RenderGraph::release(IDevice& dev) {
    if (!realized_) {
        return;
    }
    for (auto h : textures_) if (h.isValid()) dev.destroyTexture(h);
    for (auto h : buffers_) if (h.isValid()) dev.destroyBuffer(h);
    for (auto h : framebuffers_) if (h.isValid()) dev.destroyFramebuffer(h);
    textures_.clear();
    buffers_.clear();
    framebuffers_.clear();
    realized_ = false;
}

auto RenderGraph::execute(IDevice& dev, CommandBuffer& cb) -> GfxError {
    if (!realized_) {
        if (auto err = realize(dev); err != GfxError::None) return err;
    }
    for (const PassDesc& p : passes_) {
        // Resolve framebuffer handle: first write that has a framebuffer, else no wrap.
        FramebufferHandle fb{};
        for (std::uint32_t rid : p.writes) {
            if (rid < framebuffers_.size() && framebuffers_[rid].isValid()) {
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
    return dev.submit(cb);
}

} // namespace f4::gfx
