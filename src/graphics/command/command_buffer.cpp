#include "graphics/command/command_buffer.h"

#include <utility>

#include "core/assert.h"

namespace f4::gfx {

void CommandBuffer::pushCommand(const Command& cmd) {
    F4_ASSERT(recording_, "recording command while not recording");
    commands_.push(cmd);
}

void CommandBuffer::pushCommand(Command&& cmd) {
    F4_ASSERT(recording_, "recording command while not recording");
    commands_.push(static_cast<Command&&>(cmd));
}

void CommandBuffer::bindPipeline(PipelineHandle pipeline) {
    F4_ASSERT(pipeline.isValid(), "bindPipeline with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BindPipeline;
    cmd.pipeline = pipeline;
    pushCommand(std::move(cmd));
}

void CommandBuffer::bindVertexBuffer(std::uint32_t slot, BufferHandle buffer,
                                     std::uint32_t strideBytes) {
    F4_ASSERT(buffer.isValid(), "bindVertexBuffer with invalid handle");
    F4_ASSERT(strideBytes > 0, "bindVertexBuffer with zero stride");
    Command cmd;
    cmd.kind = CommandKind::BindVertexBuffer;
    cmd.vertexSlot = slot;
    cmd.buffer = buffer;
    cmd.strideBytes = strideBytes;
    pushCommand(std::move(cmd));
}

void CommandBuffer::bindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    F4_ASSERT(buffer.isValid(), "bindIndexBuffer with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BindIndexBuffer;
    cmd.buffer = buffer;
    cmd.indexFormat = format;
    pushCommand(std::move(cmd));
}

void CommandBuffer::bindResource(std::uint32_t slot, BindingType type, std::uint32_t handleIndex,
                                 std::uint32_t handleGeneration) {
    bindResource(0, slot, type, handleIndex, handleGeneration);
}

void CommandBuffer::bindResource(std::uint32_t bindPoint, std::uint32_t slot, BindingType type,
                                 std::uint32_t handleIndex,
                                 std::uint32_t handleGeneration) {
    F4_ASSERT(recording_, "bindResource while not recording");
    BoundResource res;
    res.bindPoint = bindPoint;
    res.slot = slot;
    res.type = type;
    res.handleIndex = handleIndex;
    res.handleGeneration = handleGeneration;

    // Coalesce: if the last command is BindResources with room, append.
    if (!commands_.isEmpty() && commands_.back().kind == CommandKind::BindResources
        && commands_.back().resourceCount < Command::kInlineCapacity) {
        Command& last = commands_.back();
        last.resources[last.resourceCount] = res;
        ++last.resourceCount;
        return;
    }

    // New BindResources command.
    Command cmd;
    cmd.kind = CommandKind::BindResources;
    cmd.resourceCount = 1;
    cmd.resources[0] = res;
    pushCommand(std::move(cmd));
}

void CommandBuffer::bindResource(std::uint32_t slot, std::uint32_t handleIndex,
                                 std::uint32_t handleGeneration) {
    bindResource(slot, BindingType::UniformBuffer, handleIndex, handleGeneration);
}

void CommandBuffer::bufferBarrier(BufferHandle buffer, ResourceState before,
                                  ResourceState after) {
    F4_ASSERT(buffer.isValid(), "bufferBarrier with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::ResourceBarrier;
    cmd.barrierType = BarrierResourceType::Buffer;
    cmd.buffer = buffer;
    cmd.before = before;
    cmd.after = after;
    pushCommand(std::move(cmd));
}

void CommandBuffer::textureBarrier(TextureHandle texture, ResourceState before,
                                   ResourceState after) {
    F4_ASSERT(texture.isValid(), "textureBarrier with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::ResourceBarrier;
    cmd.barrierType = BarrierResourceType::Texture;
    cmd.texture = texture;
    cmd.before = before;
    cmd.after = after;
    pushCommand(std::move(cmd));
}

void CommandBuffer::setViewport(Extent viewport) {
    Command cmd;
    cmd.kind = CommandKind::SetViewport;
    cmd.viewport = viewport;
    pushCommand(std::move(cmd));
}

void CommandBuffer::setScissor(Extent scissor) {
    Command cmd;
    cmd.kind = CommandKind::SetScissor;
    cmd.scissor = scissor;
    pushCommand(std::move(cmd));
}

void CommandBuffer::draw(std::uint32_t vertexCount, std::uint32_t instanceCount) {
    Command cmd;
    cmd.kind = CommandKind::Draw;
    cmd.vertexCount = vertexCount;
    cmd.instanceCount = instanceCount;
    pushCommand(std::move(cmd));
}

void CommandBuffer::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount) {
    Command cmd;
    cmd.kind = CommandKind::DrawIndexed;
    cmd.indexCount = indexCount;
    cmd.instanceCount = instanceCount;
    pushCommand(std::move(cmd));
}

void CommandBuffer::beginRenderPass(FramebufferHandle framebuffer) {
    F4_ASSERT(framebuffer.isValid(), "beginRenderPass with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BeginRenderPass;
    cmd.framebuffer = framebuffer;
    pushCommand(std::move(cmd));
}

void CommandBuffer::endRenderPass() {
    Command cmd;
    cmd.kind = CommandKind::EndRenderPass;
    pushCommand(std::move(cmd));
}

} // namespace f4::gfx
