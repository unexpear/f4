#pragma once

#include <cstdint>
#include <functional>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::gfx {

// M8 RenderGraph v1: declared resources and passes, executed in declaration
// order. No auto-reordering or barrier synthesis yet; those remain M9/M12
// concerns. Persistent resources live until release()/clear(), while transient
// resources are released after each submission.

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
    // Compute-only pass (no framebuffer, no declared accesses) that may run
    // on the device's async compute queue; see RenderGraph::setAsyncCompute.
    // A later pass must call CommandBuffer::waitAsyncCompute() before using
    // what it writes.
    bool asyncCompute = false;
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

    // Releases materialized resources, then removes all declarations.
    [[nodiscard]] auto clear(IDevice& dev) -> GfxError;

    // Executes passes in declaration order. For each pass, if it writes a
    // framebuffer resource, a BeginRenderPass/EndRenderPass is wrapped around
    // record(). Otherwise record() is invoked bare. After all passes, the
    // supplied CommandBuffer is begun, ended, and submitted by the graph.
    [[nodiscard]] auto execute(IDevice& dev, CommandBuffer& cb) -> GfxError;

    // When on (and the graph has fewer than kMaxGpuTimestamps passes),
    // execute() writes GPU timestamp i before pass i's barriers and timestamp
    // passCount() after the last pass, so pass i takes ts[i+1] - ts[i].
    void setGpuTimestamps(bool enabled) { gpuTimestamps_ = enabled; }
    [[nodiscard]] auto gpuTimestampsWritten() const -> bool { return timestampsWritten_; }

    // When on and the device supportsAsyncCompute(), execute() records the
    // passes marked asyncCompute into a second CommandBuffer and submits it
    // with IDevice::submitAsyncCompute() ahead of the graphics submit. They
    // keep their timestamp slots on the graphics timeline (reading ~0 ms).
    // Off, or without an async queue, they are recorded inline as usual.
    void setAsyncCompute(bool enabled) { asyncCompute_ = enabled; }
    // Whether the last execute() used the async compute queue.
    [[nodiscard]] auto asyncComputeUsed() const -> bool { return asyncComputeUsed_; }

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
    // Materialized handles and ownership flags, parallel to resources_.
    Array<TextureHandle> textures_;
    Array<BufferHandle> buffers_;
    Array<FramebufferHandle> framebuffers_;
    Array<ResourceState> states_;
    Array<std::uint8_t> owned_;
    IDevice* owner_ = nullptr;
    bool gpuTimestamps_ = false;
    bool timestampsWritten_ = false;
    bool asyncCompute_ = false;
    bool asyncComputeUsed_ = false;
};

} // namespace f4::gfx
