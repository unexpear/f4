#include <doctest/doctest.h>

#include <cstdint>
#include <array>
#include <span>
#include <vector>

#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/framebuffer/swapchain_desc.h"
#include "graphics/gfx_error.h"
#include "graphics/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/texture/texture_desc.h"
#include "graphics/types.h"

#ifndef F4_GRAPHICS_BACKEND_null
#error "M6 graphics tests require the null backend (F4_GRAPHICS_BACKEND=null)"
#endif

using namespace f4::gfx;

namespace {

struct TestDevice {
    IDevice* dev = nullptr;
    explicit TestDevice() {
        dev = createDevice();
        REQUIRE(dev != nullptr);
    }
    ~TestDevice() { deleteDevice(dev); }
};

// A minimal valid pipeline: two shaders + a layout + vertex input.
PipelineStateDesc makePipelineDesc(ShaderHandle vs, ShaderHandle fs) {
    PipelineStateDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = fs;
    desc.topology = PrimitiveTopology::TriangleList;

    VertexInputLayout::VertexBinding vb;
    vb.binding = 0;
    vb.strideBytes = 12;
    desc.vertexInput.bindings.push(vb);

    VertexInputLayout::VertexAttribute attr;
    attr.binding = 0;
    attr.location = 0;
    attr.format = VertexFormat::Float32x3;
    attr.offsetBytes = 0;
    desc.vertexInput.attributes.push(attr);

    return desc;
}

BufferHandle makeBuffer(IDevice* dev, std::uint64_t size, BufferUsage usage) {
    BufferDesc desc;
    desc.sizeBytes = size;
    desc.usage = usage;
    auto r = dev->createBuffer(desc, {});
    REQUIRE(r.hasValue());
    return r.value();
}

} // namespace

TEST_CASE("RHI: createDevice returns the compiled backend kind") {
    TestDevice t;
    CHECK(t.dev->backend() == Backend::Null);
}

TEST_CASE("RHI: null pipeline+buffer can be created at bounds") {
    TestDevice t;
    BufferDesc bd;
    bd.sizeBytes = 1024;
    bd.usage = BufferUsage::Vertex | BufferUsage::Index;
    auto b = t.dev->createBuffer(bd, {});
    REQUIRE(b.hasValue());
    CHECK(t.dev->destroyBuffer(b.value()) == GfxError::None);
}

TEST_CASE("RHI: zero-size buffer rejected") {
    TestDevice t;
    BufferDesc bd;
    bd.sizeBytes = 0;
    auto b = t.dev->createBuffer(bd, {});
    CHECK(b.hasError());
    CHECK(b.error() == GfxError::BadDescriptor);
}

TEST_CASE("RHI: stale buffer handle destroyed as StaleHandle") {
    TestDevice t;
    BufferDesc bd;
    bd.sizeBytes = 64;
    auto b = t.dev->createBuffer(bd, {});
    REQUIRE(b.hasValue());
    REQUIRE(t.dev->destroyBuffer(b.value()) == GfxError::None);
    // Destroying the (now stale) handle must be reported, not a crash.
    CHECK(t.dev->destroyBuffer(b.value()) == GfxError::StaleHandle);
}

TEST_CASE("RHI: writeBuffer bounds checked") {
    TestDevice t;
    BufferHandle b = makeBuffer(t.dev, 64, BufferUsage::Uniform);
    std::vector<std::byte> data(32, std::byte{'A'});
    CHECK(t.dev->writeBuffer(b, 0, data) == GfxError::None);
    // Fits exactly at the top of the buffer.
    CHECK(t.dev->writeBuffer(b, 32, data) == GfxError::None);
    // Overflows.
    CHECK(t.dev->writeBuffer(b, 33, data) == GfxError::BadDescriptor);
    // Stale handle.
    REQUIRE(t.dev->destroyBuffer(b) == GfxError::None);
    CHECK(t.dev->writeBuffer(b, 0, data) == GfxError::StaleHandle);
}

TEST_CASE("RHI: createDestroy cycles for every object kind") {
    TestDevice t;
    IDevice* d = t.dev;

    TextureDesc td;
    td.width = 16;
    td.height = 16;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = d->createTexture(td);
    REQUIRE(tex.hasValue());
    REQUIRE(d->destroyTexture(tex.value()) == GfxError::None);

    SamplerDesc sd;
    auto sam = d->createSampler(sd);
    REQUIRE(sam.hasValue());
    REQUIRE(d->destroySampler(sam.value()) == GfxError::None);

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());

    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());

    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    REQUIRE(d->destroyPipeline(pipe.value()) == GfxError::None);

    REQUIRE(d->destroyShader(vs.value()) == GfxError::None);
    REQUIRE(d->destroyShader(fs.value()) == GfxError::None);

    SwapchainDesc sc;
    sc.surfaceFormat = TextureFormat::RGBA8_UNORM;
    sc.extent = {128, 72};
    auto sw = d->createSwapchain(sc);
    REQUIRE(sw.hasValue());
    REQUIRE(d->destroySwapchain(sw.value()) == GfxError::None);

    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    REQUIRE(d->destroyFramebuffer(fb.value()) == GfxError::None);
}

TEST_CASE("RHI: pipeline with dangling shader handle rejected") {
    TestDevice t;
    IDevice* d = t.dev;

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    REQUIRE(d->destroyShader(vs.value()) == GfxError::None);

    // Reuse the now-stale shader handle in a pipeline -> invalid handle.
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), vs.value()));
    CHECK(pipe.hasError());
    CHECK(pipe.error() == GfxError::InvalidHandle);
}

TEST_CASE("CommandBuffer: records a scripted sequence in order") {
    TestDevice t;
    IDevice* d = t.dev;

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());

    BufferHandle vb = makeBuffer(d, 1024, BufferUsage::Vertex);
    BufferHandle ib = makeBuffer(d, 512, BufferUsage::Index);
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindVertexBuffer(0, vb, 12);
    cb.bindIndexBuffer(ib, IndexFormat::U32);
    cb.drawIndexed(36, 1);
    cb.endRenderPass();
    cb.end();
    CHECK(cb.commandCount() == 6);

    // Submitting the well-formed script succeeds.
    CHECK(d->submit(cb) == GfxError::None);
}

TEST_CASE("CommandBuffer: draw with no pipeline bound fails submit") {
    TestDevice t;
    IDevice* d = t.dev;

    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.draw(3, 1); // no pipeline ever bound
    cb.endRenderPass();
    cb.end();

    CHECK(d->submit(cb) == GfxError::BadDescriptor);
}

TEST_CASE("CommandBuffer: drawIndexed with no index buffer fails submit") {
    TestDevice t;
    IDevice* d = t.dev;

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());

    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    // NOTE: no bindIndexBuffer
    cb.drawIndexed(12, 1);
    cb.endRenderPass();
    cb.end();

    CHECK(d->submit(cb) == GfxError::BadDescriptor);
}

TEST_CASE("CommandBuffer: draw outside a render pass fails submit") {
    TestDevice t;
    IDevice* d = t.dev;

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), vs.value()));
    REQUIRE(pipe.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.bindPipeline(pipe.value());
    cb.draw(3, 1); // no render pass active
    cb.end();

    CHECK(d->submit(cb) == GfxError::BadDescriptor);
}

TEST_CASE("CommandBuffer: recording outside begin fails to record (empty)") {
    CommandBuffer cb;
    // Not begun; recording is a no-op (commands() stays empty).
    CHECK(cb.commandCount() == 0);
    CHECK(!cb.isRecording());
}

TEST_CASE("RHI: submit failure leaves clear state counters") {
    TestDevice t;
    IDevice* d = t.dev;

    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.draw(3, 1); // no pipeline -> fails
    cb.endRenderPass();
    cb.end();
    CHECK(d->submit(cb) == GfxError::BadDescriptor);
}

TEST_CASE("CommandBuffer: bindResource with 0 resources draws successfully") {
    TestDevice t;
    IDevice* d = t.dev;
    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    BufferHandle vb = makeBuffer(d, 12, BufferUsage::Vertex);

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindVertexBuffer(0, vb, 12);
    cb.draw(3, 1);
    cb.endRenderPass();
    cb.end();
    CHECK(cb.commandCount() == 5);
    CHECK(d->submit(cb) == GfxError::None);
}

TEST_CASE("CommandBuffer: bindResource with 1 resource records correctly") {
    TestDevice t;
    IDevice* d = t.dev;
    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    BufferHandle vb = makeBuffer(d, 12, BufferUsage::Vertex);
    BufferHandle ubo = makeBuffer(d, 64, BufferUsage::Uniform);

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindVertexBuffer(0, vb, 12);
    cb.bindResource(0, ubo.index, ubo.generation);
    cb.draw(3, 1);
    cb.endRenderPass();
    cb.end();
    // beginRB, bindPipeline, bindVB, bindResource(1), draw, endRB = 6 commands
    // (bindResource coalesces to 1 command)
    CHECK(cb.commandCount() == 6);
    CHECK(d->submit(cb) == GfxError::None);
}

TEST_CASE("CommandBuffer: 4 bindResource calls coalesce into 1 command") {
    TestDevice t;
    IDevice* d = t.dev;
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    BufferHandle ubo0 = makeBuffer(d, 64, BufferUsage::Uniform);
    BufferHandle ubo1 = makeBuffer(d, 64, BufferUsage::Uniform);
    BufferHandle ubo2 = makeBuffer(d, 64, BufferUsage::Uniform);
    BufferHandle ubo3 = makeBuffer(d, 64, BufferUsage::Uniform);

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindResource(0, ubo0.index, ubo0.generation);
    cb.bindResource(1, ubo1.index, ubo1.generation);
    cb.bindResource(2, ubo2.index, ubo2.generation);
    cb.bindResource(3, ubo3.index, ubo3.generation);
    cb.endRenderPass();
    cb.end();
    // beginRB, bindResource(4 coalesced), endRB = 3 commands
    CHECK(cb.commandCount() == 3);
    const auto& cmds = cb.commands();
    CHECK(cmds[1].kind == CommandKind::BindResources);
    CHECK(cmds[1].resourceCount == 4);
}

TEST_CASE("CommandBuffer: bindResource separated by draw creates 2 commands") {
    TestDevice t;
    IDevice* d = t.dev;
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    BufferHandle ubo0 = makeBuffer(d, 64, BufferUsage::Uniform);
    BufferHandle ubo1 = makeBuffer(d, 64, BufferUsage::Uniform);
    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindResource(0, ubo0.index, ubo0.generation);
    cb.draw(3, 1);
    cb.bindResource(0, ubo1.index, ubo1.generation);
    cb.draw(3, 1);
    cb.endRenderPass();
    cb.end();
    // beginRB, bindPipeline, bindResource(1), draw, bindResource(1), draw, endRB = 7
    CHECK(cb.commandCount() == 7);
    const auto& cmds = cb.commands();
    CHECK(cmds[2].kind == CommandKind::BindResources);
    CHECK(cmds[2].resourceCount == 1);
    CHECK(cmds[4].kind == CommandKind::BindResources);
    CHECK(cmds[4].resourceCount == 1);
}

TEST_CASE("CommandBuffer: 9 bindResource calls produce 2 commands (8+1)") {
    TestDevice t;
    IDevice* d = t.dev;
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());
    BufferHandle ubos[9];
    for (int i = 0; i < 9; ++i) {
        ubos[i] = makeBuffer(d, 64, BufferUsage::Uniform);
    }

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    for (int i = 0; i < 9; ++i) {
        cb.bindResource(static_cast<std::uint32_t>(i), ubos[i].index, ubos[i].generation);
    }
    cb.endRenderPass();
    cb.end();
    // beginRB, bindResource(8), bindResource(1), endRB = 4 commands
    CHECK(cb.commandCount() == 4);
    const auto& cmds = cb.commands();
    CHECK(cmds[1].kind == CommandKind::BindResources);
    CHECK(cmds[1].resourceCount == 8);
    CHECK(cmds[2].kind == CommandKind::BindResources);
    CHECK(cmds[2].resourceCount == 1);
}

TEST_CASE("CommandBuffer: move semantics preserve command content") {
    TestDevice t;
    IDevice* d = t.dev;
    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    BufferHandle vb = makeBuffer(d, 12, BufferUsage::Vertex);
    BufferHandle ib = makeBuffer(d, 24, BufferUsage::Index);
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindVertexBuffer(0, vb, 12);
    cb.bindIndexBuffer(ib, IndexFormat::U16);
    cb.drawIndexed(6, 1);
    cb.endRenderPass();
    cb.end();

    // Verify commands survived the move into the array.
    CHECK(cb.commandCount() == 6);
    CHECK(cb.commands()[0].kind == CommandKind::BeginRenderPass);
    CHECK(cb.commands()[1].kind == CommandKind::BindPipeline);
    CHECK(cb.commands()[2].kind == CommandKind::BindVertexBuffer);
    CHECK(cb.commands()[3].kind == CommandKind::BindIndexBuffer);
    CHECK(cb.commands()[4].kind == CommandKind::DrawIndexed);
    CHECK(cb.commands()[5].kind == CommandKind::EndRenderPass);
    CHECK(cb.commands()[4].indexCount == 6);

    CHECK(d->submit(cb) == GfxError::None);
}

TEST_CASE("CommandBuffer: pipeline+resources+draw full sequence") {
    TestDevice t;
    IDevice* d = t.dev;
    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    BufferHandle vb = makeBuffer(d, 12, BufferUsage::Vertex);
    BufferHandle ib = makeBuffer(d, 24, BufferUsage::Index);
    BufferHandle ubo0 = makeBuffer(d, 64, BufferUsage::Uniform);
    BufferHandle ubo1 = makeBuffer(d, 64, BufferUsage::Uniform);
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipe.value());
    cb.bindVertexBuffer(0, vb, 12);
    cb.bindIndexBuffer(ib, IndexFormat::U16);
    cb.bindResource(0, ubo0.index, ubo0.generation);
    cb.bindResource(1, ubo1.index, ubo1.generation);
    cb.drawIndexed(6, 2);
    cb.endRenderPass();
    cb.end();
    // beginRB, bindPipeline, bindVB, bindIB, bindResource(2 coalesced), drawIndexed, endRB = 7
    CHECK(cb.commandCount() == 7);
    CHECK(cb.commands()[5].kind == CommandKind::DrawIndexed);
    CHECK(cb.commands()[5].instanceCount == 2);
    CHECK(d->submit(cb) == GfxError::None);
}

TEST_CASE("Stats: per-frame draw/instance/primitive/batch counters") {
    TestDevice t;
    IDevice* d = t.dev;
    d->resetFrameStats();

    ShaderDesc vdesc;
    vdesc.stage = ShaderStage::Vertex;
    auto vs = d->createShader(vdesc);
    REQUIRE(vs.hasValue());
    ShaderDesc fdesc;
    fdesc.stage = ShaderStage::Fragment;
    auto fs = d->createShader(fdesc);
    REQUIRE(fs.hasValue());
    auto pipe = d->createPipeline(makePipelineDesc(vs.value(), fs.value()));
    REQUIRE(pipe.hasValue());
    BufferHandle vb = makeBuffer(d, 12, BufferUsage::Vertex);
    BufferHandle ib = makeBuffer(d, 24, BufferUsage::Index);
    FramebufferDesc fd;
    fd.extent = {128, 72};
    auto fb = d->createFramebuffer(fd);
    REQUIRE(fb.hasValue());

    auto submitFrame = [&]() {
        CommandBuffer cb;
        cb.begin();
        cb.beginRenderPass(fb.value());
        cb.bindPipeline(pipe.value());
        cb.bindVertexBuffer(0, vb, 12);
        cb.bindIndexBuffer(ib, IndexFormat::U16);
        // One indexed draw (6 idx, 2 instances) + one non-indexed draw (3 verts,
        // 1 instance).
        cb.drawIndexed(6, 2);
        cb.draw(3, 1);
        cb.endRenderPass();
        cb.end();
        CHECK(d->submit(cb) == GfxError::None);
    };

    submitFrame();
    DeviceStats s = d->stats();
    CHECK(s.drawCalls == 1);
    CHECK(s.drawIndexedCalls == 1);
    CHECK(s.batches == 2);
    CHECK(s.instances == 3);        // 2 + 1
    CHECK(s.primitives == 2u * 2u + 1u * 1u); // (6/3*2) + (3/3*1) = 4 + 1
    CHECK(s.vertices == 6u * 2u + 3u * 1u);

    // A second frame accumulates on top.
    submitFrame();
    s = d->stats();
    CHECK(s.batches == 4);
    CHECK(s.instances == 6);
    CHECK(s.drawCalls == 2);

    // resetFrameStats clears the per-frame counters but not resource counts.
    d->resetFrameStats();
    s = d->stats();
    CHECK(s.batches == 0);
    CHECK(s.instances == 0);
    CHECK(s.bufferCount == 2); // vb + ib
    CHECK(s.textureCount == 0);
}

TEST_CASE("Stats: resource byte/count accounting") {
    TestDevice t;
    IDevice* d = t.dev;
    d->resetFrameStats();

    BufferHandle big = makeBuffer(d, 4096, BufferUsage::Vertex);
    BufferHandle small = makeBuffer(d, 128, BufferUsage::Uniform);

    TextureDesc td;
    td.width = 16;
    td.height = 16;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = d->createTexture(td);
    REQUIRE(tex.hasValue());

    DeviceStats s = d->stats();
    CHECK(s.bufferCount == 2);
    CHECK(s.bufferBytes == 4096 + 128);
    CHECK(s.textureCount == 1);
    CHECK(s.textureBytes == 16u * 16u * 4u); // RGBA8

    // Destroying a buffer reduces both count and bytes.
    REQUIRE(d->destroyBuffer(big) == GfxError::None);
    s = d->stats();
    CHECK(s.bufferCount == 1);
    CHECK(s.bufferBytes == 128);

    // Destroying the remaining buffer drops accounting to zero.
    REQUIRE(d->destroyBuffer(small) == GfxError::None);
    s = d->stats();
    CHECK(s.bufferCount == 0);
    CHECK(s.bufferBytes == 0);

    REQUIRE(d->destroyTexture(tex.value()) == GfxError::None);
    s = d->stats();
    CHECK(s.textureCount == 0);
    CHECK(s.textureBytes == 0);
}

TEST_CASE("Stats: shaderCompiled tracks successful compiles") {
    TestDevice t;
    IDevice* d = t.dev;
    d->resetFrameStats();
    ShaderDesc vs;
    vs.stage = ShaderStage::Vertex;
    auto h = d->createShader(vs);
    REQUIRE(h.hasValue());
    CHECK(d->stats().shaderCompiled == 1);
    CHECK(d->stats().shaderCount == 1);
}

TEST_CASE("Texture: writeTexture validates footprint, mip level, and liveness") {
    TestDevice t;
    IDevice* d = t.dev;

    TextureDesc td;
    td.width = 16;
    td.height = 16;
    td.format = TextureFormat::RGBA8_UNORM; // mipLevels defaults to 1
    auto tex = d->createTexture(td);
    REQUIRE(tex.hasValue());

    // Correct full-mip footprint succeeds.
    std::array<std::byte, 16u * 16u * 4u> full{};
    CHECK(d->writeTexture(tex.value(), 0, std::span(full)) == GfxError::None);

    // A single mip with mismatched size is rejected.
    std::array<std::byte, 7> tiny{};
    CHECK(d->writeTexture(tex.value(), 0, std::span(tiny)) == GfxError::BadDescriptor);

    // Out-of-range mip is rejected.
    std::array<std::byte, 16u * 16u * 4u> again{};
    CHECK(d->writeTexture(tex.value(), 1, std::span(again)) == GfxError::BadDescriptor);

    // Stale handle is rejected.
    TextureHandle stale = tex.value();
    stale.index = stale.index + 1;
    CHECK(d->writeTexture(stale, 0, std::span(full)) == GfxError::StaleHandle);
}

TEST_CASE("Texture: mip count cannot exceed the complete chain") {
    TestDevice t;
    TextureDesc valid;
    valid.width = 8;
    valid.height = 4;
    valid.mipLevels = 4;
    CHECK(t.dev->createTexture(valid).hasValue());

    TextureDesc invalid = valid;
    invalid.mipLevels = 5;
    const auto result = t.dev->createTexture(invalid);
    REQUIRE(result.hasError());
    CHECK(result.error() == GfxError::BadDescriptor);
}

TEST_CASE("Resources: typed bindResource validates texture/sampler handles") {
    TestDevice t;
    IDevice* d = t.dev;

    TextureDesc td;
    td.width = 8;
    td.height = 8;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = d->createTexture(td);
    REQUIRE(tex.hasValue());
    auto samp = d->createSampler(SamplerDesc{});
    REQUIRE(samp.hasValue());

    // Texture + sampler bound at slots 0/1 with no draw succeeds.
    {
        CommandBuffer cb;
        cb.begin();
        cb.bindResource(0, BindingType::Texture, tex.value().index, tex.value().generation);
        cb.bindResource(1, BindingType::Sampler, samp.value().index, samp.value().generation);
        cb.end();
        CHECK(d->submit(cb) == GfxError::None);
    }

    // A texture slot pointing at a bogus handle fails validation.
    {
        CommandBuffer cb;
        cb.begin();
        TextureHandle bogus = tex.value();
        bogus.index = bogus.index + 1;
        cb.bindResource(0, BindingType::Texture, bogus.index, bogus.generation);
        cb.end();
        CHECK(d->submit(cb) == GfxError::InvalidHandle);
    }

    // A sampler slot pointing at a bogus handle fails validation.
    {
        CommandBuffer cb;
        cb.begin();
        SamplerHandle bogus = samp.value();
        bogus.generation = bogus.generation + 1;
        cb.bindResource(0, BindingType::Sampler, bogus.index, bogus.generation);
        cb.end();
        CHECK(d->submit(cb) == GfxError::InvalidHandle);
    }
}
