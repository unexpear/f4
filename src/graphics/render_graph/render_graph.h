#pragma once

#include <cstdint>
#include <functional>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::gfx {

// Single-queue graph with deterministic dependency scheduling and explicit
// access-state barriers. Persistent resources live until release()/clear();
// transient resources are released after each submission.

using containers::Array;
using containers::String;

enum class ResourceLifetime : std::uint8_t { Persistent, Transient };
enum class ResourceType : std::uint8_t { Texture, Buffer, Framebuffer };

struct ResourceDesc {
    String name;
    ResourceType type = ResourceType::Texture;
    ResourceLifetime lifetime = ResourceLifetime::Persistent;
    TextureDesc texture{};
    BufferDesc buffer{};
    FramebufferDesc framebuffer{};
    // Imported handles are borrowed and are never destroyed by the graph.
    TextureHandle importedTexture{};
    FramebufferHandle importedFramebuffer{};
};

struct PassResourceAccess {
    std::uint32_t resource = 0; // ResourceId
    ResourceState state{};
};

struct PassDesc {
    String name;
    Array<std::uint32_t> reads;  // ResourceId
    Array<std::uint32_t> writes; // ResourceId
    Array<PassResourceAccess> accesses;
    // Record callback: called during execute() inside a render pass if the pass
    // writes a framebuffer resource; otherwise called outside any pass (e.g. compute).
    // Keep it small; capture by value where possible.
    std::function<void(CommandBuffer&)> record;
    // Pass IDs that must execute first (forward references are allowed).
    // This orders callbacks, not GPU memory: declare accesses for barriers.
    Array<std::uint32_t> dependsOn;
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
    [[nodiscard]] auto importTexture(String name, TextureHandle texture) -> ResourceId;
    [[nodiscard]] auto importFramebuffer(String name, FramebufferHandle framebuffer) -> ResourceId;
    [[nodiscard]] auto declarePass(PassDesc desc) -> std::uint32_t;

    [[nodiscard]] auto resourceCount() const -> std::size_t { return resources_.size(); }
    [[nodiscard]] auto passCount() const -> std::size_t { return passes_.size(); }
    [[nodiscard]] auto resource(ResourceId id) const -> const ResourceDesc& { return resources_[id]; }
    [[nodiscard]] auto pass(std::uint32_t index) const -> const PassDesc& { return passes_[index]; }

    // Resources are unversioned: RAW/WAR/WAW hazards retain declaration order.
    // Independent ready passes use the lowest declaration ID first. Declare
    // all callback resource uses, reuse one ID per imported handle, and declare
    // framebuffer attachment texture uses separately. Untracked side effects
    // require explicit dependencies; hidden aliases are unsupported. No pass
    // culling or async compute.
    [[nodiscard]] auto compile() -> GfxError;
    // Empty until compile succeeds; invalidated by any new declaration/clear.
    [[nodiscard]] auto executionOrder() const -> const Array<std::uint32_t>& { return order_; }

    // Releases materialized resources, then removes all declarations.
    [[nodiscard]] auto clear(IDevice& dev) -> GfxError;

    // Compiles, then executes passes in dependency order. If a pass writes a
    // framebuffer resource, a BeginRenderPass/EndRenderPass is wrapped around
    // record(). Otherwise record() is invoked bare. The supplied CommandBuffer
    // is begun before recording, then ended and submitted by the graph.
    [[nodiscard]] auto execute(IDevice& dev, CommandBuffer& cb) -> GfxError;

    // Materializes all declared resources (createTexture/createBuffer/createFramebuffer).
    // Persistent handles are reused; missing transient handles are recreated.
    [[nodiscard]] auto realize(IDevice& dev) -> GfxError;
    [[nodiscard]] auto release(IDevice& dev) -> GfxError;

    [[nodiscard]] auto texture(ResourceId id) const -> TextureHandle;
    [[nodiscard]] auto buffer(ResourceId id) const -> BufferHandle;
    [[nodiscard]] auto framebuffer(ResourceId id) const -> FramebufferHandle;

private:
    [[nodiscard]] auto validateResources() const -> GfxError;
    [[nodiscard]] auto validatePasses() const -> GfxError;
    [[nodiscard]] auto releaseTransient(IDevice& dev) -> GfxError;
    [[nodiscard]] auto releaseResource(IDevice& dev, ResourceId id) -> GfxError;

    Array<ResourceDesc> resources_;
    Array<PassDesc> passes_;
    Array<std::uint32_t> order_;
    bool compiled_ = false;
    // Materialized handles and ownership flags, parallel to resources_.
    Array<TextureHandle> textures_;
    Array<BufferHandle> buffers_;
    Array<FramebufferHandle> framebuffers_;
    Array<ResourceState> states_;
    Array<std::uint8_t> owned_;
    IDevice* owner_ = nullptr;
};

} // namespace f4::gfx
