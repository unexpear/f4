#include "graphics/command/command_buffer.h"

#include "core/assert.h"

namespace f4::gfx {

void CommandBuffer::pushCommand(const Command& cmd) {
    F4_ASSERT(recording_, "recording command while not recording");
    commands_.push(cmd);
}

void CommandBuffer::bindPipeline(PipelineHandle pipeline) {
    F4_ASSERT(pipeline.isValid(), "bindPipeline with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BindPipeline;
    cmd.pipeline = pipeline;
    pushCommand(cmd);
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
    pushCommand(cmd);
}

void CommandBuffer::bindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    F4_ASSERT(buffer.isValid(), "bindIndexBuffer with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BindIndexBuffer;
    cmd.buffer = buffer;
    cmd.indexFormat = format;
    pushCommand(cmd);
}

void CommandBuffer::bindResource(std::uint32_t slot, std::uint32_t handleIndex,
                                 std::uint32_t handleGeneration) {
    Command cmd;
    cmd.kind = CommandKind::BindResources;
    BoundResource res;
    res.slot = slot;
    res.handleIndex = handleIndex;
    res.handleGeneration = handleGeneration;
    cmd.resources.push(res);
    pushCommand(cmd);
}

void CommandBuffer::setViewport(Extent viewport) {
    Command cmd;
    cmd.kind = CommandKind::SetViewport;
    cmd.viewport = viewport;
    pushCommand(cmd);
}

void CommandBuffer::setScissor(Extent scissor) {
    Command cmd;
    cmd.kind = CommandKind::SetScissor;
    cmd.scissor = scissor;
    pushCommand(cmd);
}

void CommandBuffer::draw(std::uint32_t vertexCount, std::uint32_t instanceCount) {
    Command cmd;
    cmd.kind = CommandKind::Draw;
    cmd.vertexCount = vertexCount;
    cmd.instanceCount = instanceCount;
    pushCommand(cmd);
}

void CommandBuffer::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount) {
    Command cmd;
    cmd.kind = CommandKind::DrawIndexed;
    cmd.indexCount = indexCount;
    cmd.instanceCount = instanceCount;
    pushCommand(cmd);
}

void CommandBuffer::beginRenderPass(FramebufferHandle framebuffer) {
    F4_ASSERT(framebuffer.isValid(), "beginRenderPass with invalid handle");
    Command cmd;
    cmd.kind = CommandKind::BeginRenderPass;
    cmd.framebuffer = framebuffer;
    pushCommand(cmd);
}

void CommandBuffer::endRenderPass() {
    Command cmd;
    cmd.kind = CommandKind::EndRenderPass;
    pushCommand(cmd);
}

} // namespace f4::gfx
