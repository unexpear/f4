#pragma once

#include <cstdint>
#include <cstring>

#include "core/containers/array.h"
#include "core/ids/handle.h"

#include "graphics/device/gpu_timestamps.h"
#include "graphics/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/types.h"

namespace f4::gfx {

using containers::Array;

// ---------------------------------------------------------------------------
// CommandBuffer — record-only by construction. The engine NEVER talks to the
// device directly; it records commands here, then IDevice::submit replays them
// (immediately on GL and encoded on Vulkan/DX12/Metal/WebGPU). See plan §3.
// ---------------------------------------------------------------------------

enum class CommandKind : std::uint8_t {
    ResourceBarrier,
    BindPipeline,
    BindVertexBuffer,
    BindIndexBuffer,
    BindResources,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    Dispatch,
    BeginRenderPass,
    EndRenderPass,
    WriteTimestamp,
};

enum class BarrierResourceType : std::uint8_t { Buffer, Texture };

// One resource bound at (bindPoint, slot) within BindResources. `type` tells the
// backend which handle namespace to resolve (a buffer for Uniform/Storage, a
// texture/sampler otherwise) and how to bind it on the device.
struct BoundResource {
    std::uint32_t bindPoint = 0;
    std::uint32_t slot = 0;
    BindingType type = BindingType::UniformBuffer;
    std::uint32_t handleIndex = 0;
    std::uint32_t handleGeneration = 0;
};

struct Command {
    CommandKind kind = CommandKind::BindPipeline;

    PipelineHandle pipeline{};
    std::uint32_t vertexSlot = 0;
    BufferHandle buffer{};
    std::uint32_t strideBytes = 0;
    IndexFormat indexFormat = IndexFormat::U16;
    FramebufferHandle framebuffer{};
    TextureHandle texture{};
    BarrierResourceType barrierType = BarrierResourceType::Buffer;
    ResourceState before{};
    ResourceState after{};
    Extent viewport;
    Extent scissor;
    // Rectangle origins in attachment pixels, counted from the attachment's
    // first stored row (GL window coordinates; top-left everywhere else).
    std::uint32_t viewportX = 0;
    std::uint32_t viewportY = 0;
    std::uint32_t scissorX = 0;
    std::uint32_t scissorY = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t groupCountX = 0;
    std::uint32_t groupCountY = 0;
    std::uint32_t groupCountZ = 0;
    std::uint32_t timestampIndex = 0; // WriteTimestamp, < kMaxGpuTimestamps

    // Inline resource storage — zero heap allocation for ≤8 resources.
    static constexpr std::uint8_t kInlineCapacity = 8;
    std::uint8_t resourceCount = 0;
    BoundResource resources[kInlineCapacity];
};

class CommandBuffer {
public:
    // Recording lifecycle.
    void begin() { recording_ = true; commands_.clear(); }
    void end() { recording_ = false; }

    [[nodiscard]] auto isRecording() const -> bool { return recording_; }
    [[nodiscard]] auto commandCount() const -> std::size_t { return commands_.size(); }

    // Command recording (asserts while not recording).
    void bindPipeline(PipelineHandle pipeline);
    void bindVertexBuffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t strideBytes);
    void bindIndexBuffer(BufferHandle buffer, IndexFormat format);
    // Bind a typed resource at a slot. Extra resource kinds (textures/samplers)
    // must use the typed overload; the plain three-argument form treats the
    // handle as a uniform buffer (existing callers keep compiling untyped).
    void bindResource(std::uint32_t slot, BindingType type, std::uint32_t handleIndex,
                      std::uint32_t handleGeneration);
    void bindResource(std::uint32_t bindPoint, std::uint32_t slot, BindingType type,
                      std::uint32_t handleIndex, std::uint32_t handleGeneration);
    void bindResource(std::uint32_t slot, std::uint32_t handleIndex, std::uint32_t handleGeneration);
    void bufferBarrier(BufferHandle buffer, ResourceState before, ResourceState after);
    void textureBarrier(TextureHandle texture, ResourceState before, ResourceState after);
    // (x, y) offsets the rectangle inside the attachment, in pixels from the
    // attachment's first stored row/column. Every backend passes it straight
    // through, so a sub-rectangle covers the same stored texels everywhere
    // (NDC +Y still points to the higher row on OpenGL, the lower elsewhere).
    void setViewport(Extent viewport, std::uint32_t x = 0, std::uint32_t y = 0);
    void setScissor(Extent scissor, std::uint32_t x = 0, std::uint32_t y = 0);
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1);
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1,
                     std::uint32_t firstIndex = 0);
    void dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY = 1,
                  std::uint32_t groupCountZ = 1);
    void beginRenderPass(FramebufferHandle framebuffer);
    void endRenderPass();
    // GPU timestamp `index` (< kMaxGpuTimestamps), outside render passes. See
    // graphics/device/gpu_timestamps.h; devices without timer queries ignore it.
    void writeTimestamp(std::uint32_t index);
    // Highest timestamp index + 1 and a bit mask of written indices (0/0 when
    // the buffer writes none). Backends use it to size the query set.
    void timestampUsage(std::uint32_t& count, std::uint32_t& written) const;

    // Test/observability surface — a recorded command log (NullDevice submits
    // a validated replay of this).
    [[nodiscard]] auto commands() const -> const Array<Command>& { return commands_; }

private:
    void pushCommand(const Command& cmd);
    void pushCommand(Command&& cmd);

    bool recording_ = false;
    Array<Command> commands_;
};

} // namespace f4::gfx
