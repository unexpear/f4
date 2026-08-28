#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/ids/handle.h"

#include "graphics/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/types.h"

namespace f4::gfx {

using containers::Array;

// ---------------------------------------------------------------------------
// CommandBuffer — record-only by construction. The engine NEVER talks to the
// device directly; it records commands here, then IDevice::submit replays them
// (immediately on a GL backend, deferred on Vulkan/DX12). See plan §3.
// ---------------------------------------------------------------------------

enum class CommandKind : std::uint8_t {
    BindPipeline,
    BindVertexBuffer,
    BindIndexBuffer,
    BindResources,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    BeginRenderPass,
    EndRenderPass,
};

// One resource bound at (bindPoint, slot) within BindResources.
struct BoundResource {
    std::uint32_t slot = 0;
    // Exactly one handle is meaningful depending on BindingType; encoded as a
    // union-ish via the generic handle key plus a type tag carried by the
    // caller (PipelineLayout describes types; we just store the value).
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
    Extent viewport;
    Extent scissor;
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;

    Array<BoundResource> resources;
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
    void bindResource(std::uint32_t slot, std::uint32_t handleIndex, std::uint32_t handleGeneration);
    void setViewport(Extent viewport);
    void setScissor(Extent scissor);
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1);
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1);
    void beginRenderPass(FramebufferHandle framebuffer);
    void endRenderPass();

    // Test/observability surface — a recorded command log (NullDevice submits
    // a validated replay of this).
    [[nodiscard]] auto commands() const -> const Array<Command>& { return commands_; }

private:
    void pushCommand(const Command& cmd);

    bool recording_ = false;
    Array<Command> commands_;
};

} // namespace f4::gfx
