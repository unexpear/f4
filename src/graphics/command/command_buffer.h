#pragma once

#include <cstdint>
#include <cstring>

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
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t groupCountX = 0;
    std::uint32_t groupCountY = 0;
    std::uint32_t groupCountZ = 0;

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
    void setViewport(Extent viewport);
    void setScissor(Extent scissor);
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1);
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1,
                     std::uint32_t firstIndex = 0);
    void dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY = 1,
                  std::uint32_t groupCountZ = 1);
    void beginRenderPass(FramebufferHandle framebuffer);
    void endRenderPass();

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
