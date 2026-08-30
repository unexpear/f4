#pragma once

#include <cstdint>
#include <functional>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::gfx {

// M8 RenderGraph v1: declarations-only, executed in declaration order on GL.
// No auto-reordering, barrier synthesis, or lifetime allocation.
// Engine declares WHAT and WHEN; RHI executes HOW.

using containers::Array;
using containers::String;

enum class ResourceLifetime : std::uint8_t { Persistent, Transient };
enum class ResourceType : std::uint8_t { Texture, Buffer };

struct ResourceDesc {
    String name;
    ResourceType type = ResourceType::Texture;
    ResourceLifetime lifetime = ResourceLifetime::Persistent;
    // Texture path
    TextureDesc texture{};
    // Buffer path
    BufferDesc buffer{};
    // Framebuffer handle used when writing; null-texture attachments mean default FBO 0.
    FramebufferDesc framebuffer{};
};

struct PassDesc {
    String name;
    Array<std::uint32_t> reads;  // ResourceId
    Array<std::uint32_t> writes; // ResourceId
    // Record callback: called during execute() inside a render pass if the pass
    // writes a framebuffer resource; otherwise called outside any pass (e.g. compute).
    // Keep it small; capture by value where possible.
    std::function<void(CommandBuffer&)> record;
};

struct FrameContext {
    IDevice* device = nullptr;
    FramebufferHandle framebuffer{};
    Extent viewport{};
};

class RenderGraph {
public:
    using ResourceId = std::uint32_t;

    [[nodiscard]] auto declareResource(ResourceDesc desc) -> ResourceId;
    [[nodiscard]] auto declarePass(PassDesc desc) -> std::uint32_t;

    [[nodiscard]] auto resourceCount() const -> std::size_t { return resources_.size(); }
    [[nodiscard]] auto passCount() const -> std::size_t { return passes_.size(); }
    [[nodiscard]] auto resource(ResourceId id) const -> const ResourceDesc& { return resources_[id]; }
    [[nodiscard]] auto pass(std::uint32_t index) const -> const PassDesc& { return passes_[index]; }

    void clear();

    // Executes passes in declaration order. For each pass, if it writes a
    // framebuffer resource, a BeginRenderPass/EndRenderPass is wrapped around
    // record(). Otherwise record() is invoked bare. After all passes, the
    // supplied CommandBuffer is submitted.
    [[nodiscard]] auto execute(IDevice& dev, CommandBuffer& cb) -> GfxError;

    // Materializes all declared resources (createTexture/createBuffer/createFramebuffer).
    // Call before execute() on first frame or after clear().
    [[nodiscard]] auto realize(IDevice& dev) -> GfxError;
    void release(IDevice& dev);

private:
    Array<ResourceDesc> resources_;
    Array<PassDesc> passes_;
    // Realized handles — parallel to resources_
    Array<TextureHandle> textures_;
    Array<BufferHandle> buffers_;
    Array<FramebufferHandle> framebuffers_;
    bool realized_ = false;
};

} // namespace f4::gfx
