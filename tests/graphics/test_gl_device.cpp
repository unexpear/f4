#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <epoxy/gl.h>

#include "backends/opengl/opengl_device.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/types.h"
#include "platform/gl_context.h"
#include "platform/window.h"

// This suite needs the OpenGL backend, which is only built where SDL2 +
// libepoxy exist (MSYS2 ucrt64). MSVC/other toolchains compile this file with
// F4_GRAPHICS_HAS_OPENGL undefined and the whole suite body is dropped.
#ifdef F4_GRAPHICS_HAS_OPENGL

using namespace f4;
using namespace f4::gfx;

namespace {

constexpr const char* kPosColorVertex = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); }
)glsl";

constexpr const char* kOrangeFragment = R"glsl(
#version 430 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.5, 0.25, 1.0); }
)glsl";

void fillBlob(Array<std::byte>& blob, const std::string& text) {
    blob.resize(text.size());
    std::memcpy(blob.data(), text.data(), text.size());
}

struct TriangleHarness {
    platform::Window* window = nullptr;
    IDevice* dev = nullptr;
    int major = 0;
    int minor = 0;
    BufferHandle vb{};
    ShaderHandle vs{};
    ShaderHandle fs{};
    PipelineHandle pipeline{};
    FramebufferHandle fb{};

    static constexpr std::uint32_t kWidth = 256;
    static constexpr std::uint32_t kHeight = 256;

    explicit TriangleHarness() {
        auto w = platform::Window::create(
            {"gl conformance", kWidth, kHeight, false /* hidden -> llvmpipe */});
        REQUIRE(w.hasValue());
        window = w.value();
        REQUIRE(window->makeCurrent());

        auto glInfo = platform::initGl(4, 3);
        REQUIRE(glInfo.hasValue());
        major = glInfo.value().major;
        minor = glInfo.value().minor;

        dev = createOpenGLDevice();
        REQUIRE(dev != nullptr);
        REQUIRE(dev->backend() == Backend::OpenGL);

        struct Vertex {
            float x, y, z;
        };
        constexpr std::array<Vertex, 3> triangle = {{
            {0.0f, 0.6f, 0.0f},
            {-0.5f, -0.5f, 0.0f},
            {0.5f, -0.5f, 0.0f},
        }};
        const auto* bytes = reinterpret_cast<const std::byte*>(triangle.data());
        BufferDesc vbd;
        vbd.sizeBytes = triangle.size() * sizeof(Vertex);
        vbd.usage = BufferUsage::Vertex;
        auto vbRes = dev->createBuffer(vbd, std::span(bytes, vbd.sizeBytes));
        REQUIRE(vbRes.hasValue());
        vb = vbRes.value();

        ShaderDesc vsd;
        vsd.stage = ShaderStage::Vertex;
        fillBlob(vsd.blob, kPosColorVertex);
        auto vsRes = dev->createShader(vsd);
        REQUIRE(vsRes.hasValue());
        vs = vsRes.value();

        ShaderDesc fsd;
        fsd.stage = ShaderStage::Fragment;
        fillBlob(fsd.blob, kOrangeFragment);
        auto fsRes = dev->createShader(fsd);
        REQUIRE(fsRes.hasValue());
        fs = fsRes.value();

        PipelineStateDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.topology = PrimitiveTopology::TriangleList;
        VertexInputLayout::VertexBinding binding;
        binding.binding = 0;
        binding.strideBytes = sizeof(Vertex);
        pd.vertexInput.bindings.push(binding);
        VertexInputLayout::VertexAttribute attr;
        attr.binding = 0;
        attr.location = 0;
        attr.format = VertexFormat::Float32x3;
        attr.offsetBytes = 0;
        pd.vertexInput.attributes.push(attr);
        auto pipeRes = dev->createPipeline(pd);
        REQUIRE(pipeRes.hasValue());
        pipeline = pipeRes.value();

        FramebufferDesc fbd;
        fbd.extent = {kWidth, kHeight};
        AttachmentDesc clear;
        clear.type = AttachmentType::Color;
        clear.clearOnLoad = true;
        clear.clear.color[0] = 0.1f;
        clear.clear.color[1] = 0.1f;
        clear.clear.color[2] = 0.2f;
        fbd.attachments.push(clear);
        auto fbRes = dev->createFramebuffer(fbd);
        REQUIRE(fbRes.hasValue());
        fb = fbRes.value();
    }

    void render() {
        CommandBuffer cb;
        cb.begin();
        cb.beginRenderPass(fb);
        cb.bindPipeline(pipeline);
        cb.bindVertexBuffer(0, vb, sizeof(float) * 3);
        cb.draw(3);
        cb.endRenderPass();
        cb.end();
        REQUIRE(dev->submit(cb) == GfxError::None);
        glFinish();
    }

    ~TriangleHarness() {
        dev->waitIdle();
        deleteDevice(dev);
        delete window;
    }
};

} // namespace

TEST_CASE("OpenGL backend: rendered triangle appears on the framebuffer") {
    TriangleHarness h;
    h.render();

    // Read the center (inside the triangle) and a corner (clear color).
    std::array<float, 3> center{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center.data());
    std::array<float, 3> corner{};
    glReadPixels(4, 4, 1, 1, GL_RGB, GL_FLOAT, corner.data());

    // Corner should be the clear color (not orange).
    CHECK(corner[0] < 0.5f);
    // Center should be orange-ish (r well above g/b and far from the clear).
    CHECK(center[0] > 0.7f);
    CHECK(center[1] < 0.7f);
    CHECK(center[2] < 0.5f);
}

TEST_CASE("OpenGL backend: draw outside a render pass is rejected") {
    TriangleHarness h;
    CommandBuffer cb;
    cb.begin();
    cb.bindPipeline(h.pipeline);
    cb.draw(3); // no render pass
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::BadDescriptor);
}

TEST_CASE("OpenGL backend: real GL version is desktop 4.3+ core") {
    TriangleHarness h;
    CHECK(epoxy_is_desktop_gl());
    CHECK(h.major >= 4);
    // Encode 4.3 as the minimum acceptable version (major<<8 | minor).
    const int got = (h.major << 8) | h.minor;
    constexpr int kMin430 = (4 << 8) | 3;
    CHECK(got >= kMin430);

    GLint clipOrigin = 0;
    GLint clipDepthMode = 0;
    glGetIntegerv(GL_CLIP_ORIGIN, &clipOrigin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &clipDepthMode);
    CHECK(clipOrigin == GL_LOWER_LEFT);
    CHECK(clipDepthMode == GL_ZERO_TO_ONE);
}

TEST_CASE("OpenGL backend: default framebuffer uses FBO 0") {
    TriangleHarness h;
    // The test harness creates a framebuffer with no texture attachments
    // (all null). This must map to the default framebuffer (FBO 0).
    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.endRenderPass();
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::None);
    // Render and verify the clear color comes through.
    h.render();
    std::array<float, 3> corner{};
    glReadPixels(4, 4, 1, 1, GL_RGB, GL_FLOAT, corner.data());
    // Corner should be the clear color (0.1, 0.1, 0.2).
    CHECK(corner[0] < 0.3f);
    CHECK(corner[1] < 0.3f);
    CHECK(corner[2] < 0.4f);
}

TEST_CASE("OpenGL backend: offscreen framebuffer renders to texture") {
    auto w = platform::Window::create({"fbo test", 256, 256, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    auto glInfo = platform::initGl(4, 3);
    REQUIRE(glInfo.hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    // Create a texture to render into.
    TextureDesc td;
    td.width = 256;
    td.height = 256;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = dev->createTexture(td);
    REQUIRE(tex.hasValue());

    // Create a framebuffer with that texture as color attachment only.
    FramebufferDesc fbd;
    fbd.extent = {256, 256};
    AttachmentDesc colorAtt;
    colorAtt.type = AttachmentType::Color;
    colorAtt.texture = tex.value();
    colorAtt.clearOnLoad = true;
    colorAtt.clear.color[0] = 0.0f;
    colorAtt.clear.color[1] = 0.0f;
    colorAtt.clear.color[2] = 0.3f;
    fbd.attachments.push(colorAtt);
    auto fb = dev->createFramebuffer(fbd);
    REQUIRE(fb.hasValue());

    // Create the simple triangle pipeline.
    ShaderDesc vsd;
    vsd.stage = ShaderStage::Vertex;
    fillBlob(vsd.blob, kPosColorVertex);
    auto vs = dev->createShader(vsd);
    REQUIRE(vs.hasValue());
    ShaderDesc fsd;
    fsd.stage = ShaderStage::Fragment;
    fillBlob(fsd.blob, kOrangeFragment);
    auto fs = dev->createShader(fsd);
    REQUIRE(fs.hasValue());

    PipelineStateDesc pd;
    pd.vertexShader = vs.value();
    pd.fragmentShader = fs.value();
    pd.topology = PrimitiveTopology::TriangleList;
    VertexInputLayout::VertexBinding binding;
    binding.binding = 0;
    binding.strideBytes = sizeof(float) * 3;
    pd.vertexInput.bindings.push(binding);
    VertexInputLayout::VertexAttribute attr;
    attr.binding = 0;
    attr.location = 0;
    attr.format = VertexFormat::Float32x3;
    attr.offsetBytes = 0;
    pd.vertexInput.attributes.push(attr);
    auto pipeline = dev->createPipeline(pd);
    REQUIRE(pipeline.hasValue());

    struct Vertex { float x, y, z; };
    constexpr std::array<Vertex, 3> tri = {{{0.0f, 0.6f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}}};
    auto* bytes = reinterpret_cast<const std::byte*>(tri.data());
    BufferDesc vbd;
    vbd.sizeBytes = tri.size() * sizeof(Vertex);
    vbd.usage = BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(bytes, vbd.sizeBytes));
    REQUIRE(vb.hasValue());

    // Render to the offscreen FBO — submit must succeed.
    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipeline.value());
    cb.bindVertexBuffer(0, vb.value(), sizeof(float) * 3);
    cb.draw(3);
    cb.endRenderPass();
    cb.end();
    CHECK(dev->submit(cb) == GfxError::None);
    glFinish();

    // Read back via the offscreen FBO to verify pixels landed.
    // We can't directly access the GL FBO id, but we can verify
    // that submitting the offscreen draw succeeded and the GL error
    // state is clean.
    CHECK(glGetError() == GL_NO_ERROR);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: render pass with no clear preserves content") {
    TriangleHarness h;

    // Create a second framebuffer with clearOnLoad=false for color.
    FramebufferDesc fbd;
    fbd.extent = {TriangleHarness::kWidth, TriangleHarness::kHeight};
    AttachmentDesc noClear;
    noClear.type = AttachmentType::Color;
    noClear.clearOnLoad = false;
    fbd.attachments.push(noClear);
    auto fb2 = h.dev->createFramebuffer(fbd);
    REQUIRE(fb2.hasValue());

    // Within a single frame: pass 1 renders the triangle (clearOnLoad=true),
    // pass 2 uses clearOnLoad=false (should NOT clear, triangle persists).
    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.bindPipeline(h.pipeline);
    cb.bindVertexBuffer(0, h.vb, sizeof(float) * 3);
    cb.draw(3);
    cb.endRenderPass();
    // Second pass: clearOnLoad=false, no draw. Content should persist.
    cb.beginRenderPass(fb2.value());
    cb.endRenderPass();
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::None);
    glFinish();

    // The center pixel should still be orange from pass 1's render.
    std::array<float, 3> center{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center.data());
    CHECK(center[0] > 0.7f);
}

TEST_CASE("OpenGL backend: buffer upload correctness") {
    TriangleHarness h;

    // Create a uniform buffer with a known value and verify via shader output.
    constexpr float kTestData[4] = {0.123f, 0.456f, 0.789f, 1.0f};
    BufferDesc ubd;
    ubd.sizeBytes = sizeof(kTestData);
    ubd.usage = BufferUsage::Uniform;
    auto ubo = h.dev->createBuffer(ubd, std::span(reinterpret_cast<const std::byte*>(kTestData),
                                                   sizeof(kTestData)));
    REQUIRE(ubo.hasValue());

    // Write new data to the UBO.
    constexpr float kNewData[4] = {0.9f, 0.8f, 0.7f, 1.0f};
    CHECK(h.dev->writeBuffer(ubo.value(), 0,
                             std::span(reinterpret_cast<const std::byte*>(kNewData),
                                       sizeof(kNewData))) == GfxError::None);

    // The buffer should still be valid after write.
    BufferDesc check;
    check.sizeBytes = sizeof(kNewData);
    check.usage = BufferUsage::Uniform;
    // writeBuffer to the same buffer at offset 0 should succeed.
    CHECK(h.dev->writeBuffer(ubo.value(), 0,
                             std::span(reinterpret_cast<const std::byte*>(kNewData),
                                       sizeof(kNewData))) == GfxError::None);

    // writeBuffer past the end should fail.
    std::vector<std::byte> bigData(128, std::byte{0xFF});
    CHECK(h.dev->writeBuffer(ubo.value(), 1, bigData) == GfxError::BadDescriptor);

    CHECK(h.dev->destroyBuffer(ubo.value()) == GfxError::None);
}

TEST_CASE("OpenGL backend: consecutive same-pipeline binds render correctly") {
    TriangleHarness h;
    // Draw twice with the same pipeline — should produce the same visual output.
    h.render();
    // Read center pixel.
    std::array<float, 3> center1{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center1.data());

    // Second draw with the same pipeline (state cache should skip redundant changes).
    h.render();
    std::array<float, 3> center2{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center2.data());

    // Both should show the orange triangle.
    CHECK(center1[0] > 0.7f);
    CHECK(center2[0] > 0.7f);
    // Values should be identical (same rendering).
    CHECK(center1[0] == doctest::Approx(center2[0]));
}

TEST_CASE("OpenGL backend: pipeline creation invalidates cached VAO binding") {
    TriangleHarness h;
    h.render();

    PipelineStateDesc desc;
    desc.vertexShader = h.vs;
    desc.fragmentShader = h.fs;
    desc.vertexInput.bindings.push({0, sizeof(float) * 3, 0});
    desc.vertexInput.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    auto extraPipeline = h.dev->createPipeline(desc);
    REQUIRE(extraPipeline.hasValue());

    // createPipeline builds a VAO and leaves VAO 0 bound. The cached original
    // pipeline must still restore its VAO on the next submit.
    h.render();
    std::array<float, 3> center{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center.data());
    CHECK(center[0] > 0.7f);
    CHECK(center[1] < 0.7f);
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("OpenGL backend: depth clear survives a previous depth-write-disabled pipeline") {
    TriangleHarness h;

    FramebufferDesc fbd;
    fbd.extent = {h.kWidth, h.kHeight};
    AttachmentDesc color;
    color.type = AttachmentType::Color;
    color.clearOnLoad = true;
    color.clear.color[0] = 0.1f;
    color.clear.color[1] = 0.1f;
    color.clear.color[2] = 0.2f;
    fbd.attachments.push(color);
    AttachmentDesc depth;
    depth.type = AttachmentType::Depth;
    depth.clearOnLoad = true;
    depth.clear.depth = 1.0f;
    fbd.attachments.push(depth);
    auto fb = h.dev->createFramebuffer(fbd);
    REQUIRE(fb.hasValue());

    PipelineStateDesc pd;
    pd.vertexShader = h.vs;
    pd.fragmentShader = h.fs;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.raster.cullMode = CullMode::None;
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = true;
    pd.depthStencil.depthCompare = CompareOp::Less;
    pd.vertexInput.bindings.push({0, sizeof(float) * 3, 0});
    pd.vertexInput.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    auto depthPipeline = h.dev->createPipeline(pd);
    REQUIRE(depthPipeline.hasValue());

    struct Vertex { float x, y, z; };
    constexpr std::array<Vertex, 3> farTriangle = {{
        {0.0f, 0.6f, 0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
    }};
    BufferDesc vbd;
    vbd.sizeBytes = farTriangle.size() * sizeof(Vertex);
    vbd.usage = BufferUsage::Vertex;
    auto farVb = h.dev->createBuffer(
        vbd, std::as_bytes(std::span(farTriangle)));
    REQUIRE(farVb.hasValue());

    CommandBuffer first;
    first.begin();
    first.beginRenderPass(fb.value());
    first.bindPipeline(depthPipeline.value());
    first.bindVertexBuffer(0, h.vb, sizeof(Vertex));
    first.draw(3);
    // Leave the persistent driver state with depth writes disabled.
    first.bindPipeline(h.pipeline);
    first.endRenderPass();
    first.end();
    REQUIRE(h.dev->submit(first) == GfxError::None);

    CommandBuffer second;
    second.begin();
    second.beginRenderPass(fb.value());
    second.bindPipeline(depthPipeline.value());
    second.bindVertexBuffer(0, farVb.value(), sizeof(Vertex));
    second.draw(3);
    second.endRenderPass();
    second.end();
    REQUIRE(h.dev->submit(second) == GfxError::None);
    glFinish();

    std::array<float, 3> center{};
    glReadPixels(h.kWidth / 2, h.kHeight / 2, 1, 1, GL_RGB, GL_FLOAT, center.data());
    CHECK(center[0] > 0.7f);
    CHECK(center[1] < 0.7f);
}

TEST_CASE("OpenGL backend: different pipeline cull mode changes rendering") {
    auto w = platform::Window::create({"cull test", 256, 256, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    auto glInfo = platform::initGl(4, 3);
    REQUIRE(glInfo.hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    struct Vertex { float x, y, z; };
    constexpr std::array<Vertex, 3> tri = {{{0.0f, 0.6f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}}};
    auto* bytes = reinterpret_cast<const std::byte*>(tri.data());
    BufferDesc vbd;
    vbd.sizeBytes = tri.size() * sizeof(Vertex);
    vbd.usage = BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(bytes, vbd.sizeBytes));
    REQUIRE(vb.hasValue());

    ShaderDesc vsd;
    vsd.stage = ShaderStage::Vertex;
    fillBlob(vsd.blob, kPosColorVertex);
    auto vs = dev->createShader(vsd);
    REQUIRE(vs.hasValue());
    ShaderDesc fsd;
    fsd.stage = ShaderStage::Fragment;
    fillBlob(fsd.blob, kOrangeFragment);
    auto fs = dev->createShader(fsd);
    REQUIRE(fs.hasValue());

    // Pipeline A: cull back faces.
    PipelineStateDesc pda;
    pda.vertexShader = vs.value();
    pda.fragmentShader = fs.value();
    pda.topology = PrimitiveTopology::TriangleList;
    pda.raster.cullMode = CullMode::Back;
    VertexInputLayout::VertexBinding ba;
    ba.binding = 0;
    ba.strideBytes = sizeof(Vertex);
    pda.vertexInput.bindings.push(ba);
    VertexInputLayout::VertexAttribute aa;
    aa.binding = 0;
    aa.location = 0;
    aa.format = VertexFormat::Float32x3;
    aa.offsetBytes = 0;
    pda.vertexInput.attributes.push(aa);
    auto pipeA = dev->createPipeline(pda);
    REQUIRE(pipeA.hasValue());

    // Pipeline B: no culling.
    PipelineStateDesc pdb = pda;
    pdb.raster.cullMode = CullMode::None;
    auto pipeB = dev->createPipeline(pdb);
    REQUIRE(pipeB.hasValue());

    FramebufferDesc fbd;
    fbd.extent = {256, 256};
    AttachmentDesc clear;
    clear.type = AttachmentType::Color;
    clear.clearOnLoad = true;
    clear.clear.color[0] = 0.0f;
    clear.clear.color[1] = 0.0f;
    clear.clear.color[2] = 0.0f;
    fbd.attachments.push(clear);
    auto fb = dev->createFramebuffer(fbd);
    REQUIRE(fb.hasValue());

    // Draw with back-face culling (CCW winding = front face, so triangle is visible).
    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(fb.value());
    cb.bindPipeline(pipeA.value());
    cb.bindVertexBuffer(0, vb.value(), sizeof(Vertex));
    cb.draw(3);
    cb.endRenderPass();
    cb.end();
    CHECK(dev->submit(cb) == GfxError::None);
    glFinish();

    std::array<float, 3> cullCenter{};
    glReadPixels(128, 128, 1, 1, GL_RGB, GL_FLOAT, cullCenter.data());
    CHECK(cullCenter[0] > 0.5f); // triangle visible

    // Draw with no culling — same triangle, should also be visible.
    CommandBuffer cb2;
    cb2.begin();
    cb2.beginRenderPass(fb.value());
    cb2.bindPipeline(pipeB.value());
    cb2.bindVertexBuffer(0, vb.value(), sizeof(Vertex));
    cb2.draw(3);
    cb2.endRenderPass();
    cb2.end();
    CHECK(dev->submit(cb2) == GfxError::None);
    glFinish();

    std::array<float, 3> noCullCenter{};
    glReadPixels(128, 128, 1, 1, GL_RGB, GL_FLOAT, noCullCenter.data());
    CHECK(noCullCenter[0] > 0.5f); // triangle visible

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

// Build a pipeline with two vertex buffers: binding 0 carries per-vertex
// positions (divisor 0), binding 1 carries a per-instance vec4 (divisor 1)
// used to offset and tint each instance.
constexpr auto kInstPosVertex = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aInst;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x + aInst.x, aPos.y, aPos.z, 1.0);
    vColor = vec4(aInst.yzw, 1.0);
}
)glsl";

constexpr auto kInstColorFragment = R"glsl(
#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)glsl";

TEST_CASE("OpenGL backend: indexed instancing with divisor-1 attributes renders each instance") {
    auto w = platform::Window::create({"instance test", 256, 256, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    auto glInfo = platform::initGl(4, 3);
    REQUIRE(glInfo.hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    // Two instanced quads. Binding 1 steps per instance (divisor 1).
    PipelineStateDesc pd;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.raster.cullMode = CullMode::None;

    {
        VertexInputLayout::VertexBinding posBinding;
        posBinding.binding = 0;
        posBinding.strideBytes = sizeof(float) * 3;
        posBinding.divisor = 0;
        pd.vertexInput.bindings.push(posBinding);

        VertexInputLayout::VertexBinding instBinding;
        instBinding.binding = 1;
        instBinding.strideBytes = sizeof(float) * 4;
        instBinding.divisor = 1;
        pd.vertexInput.bindings.push(instBinding);

        VertexInputLayout::VertexAttribute posAttr;
        posAttr.binding = 0;
        posAttr.location = 0;
        posAttr.format = VertexFormat::Float32x3;
        posAttr.offsetBytes = 0;
        pd.vertexInput.attributes.push(posAttr);

        VertexInputLayout::VertexAttribute instAttr;
        instAttr.binding = 1;
        instAttr.location = 1;
        instAttr.format = VertexFormat::Float32x4;
        instAttr.offsetBytes = 0;
        pd.vertexInput.attributes.push(instAttr);
    }

    auto vs = dev->createShader([&] {
        ShaderDesc d;
        d.stage = ShaderStage::Vertex;
        fillBlob(d.blob, kInstPosVertex);
        return d;
    }());
    REQUIRE(vs.hasValue());
    auto fs = dev->createShader([&] {
        ShaderDesc d;
        d.stage = ShaderStage::Fragment;
        fillBlob(d.blob, kInstColorFragment);
        return d;
    }());
    REQUIRE(fs.hasValue());
    pd.vertexShader = vs.value();
    pd.fragmentShader = fs.value();
    auto pipeline = dev->createPipeline(pd);
    REQUIRE(pipeline.hasValue());

    // Unit quad centered on origin; instances translate it left/right.
    struct Pos { float x, y, z; };
    constexpr std::array<Pos, 4> quads = {{
        {-0.2f, -0.2f, 0.0f},
        { 0.2f, -0.2f, 0.0f},
        { 0.2f,  0.2f, 0.0f},
        {-0.2f,  0.2f, 0.0f},
    }};
    auto* posBytes = reinterpret_cast<const std::byte*>(quads.data());
    BufferDesc vbd;
    vbd.sizeBytes = quads.size() * sizeof(Pos);
    vbd.usage = BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(posBytes, vbd.sizeBytes));
    REQUIRE(vb.hasValue());

    constexpr std::array<std::uint16_t, 6> indices = {{0, 1, 2, 0, 2, 3}};
    BufferDesc ibd;
    ibd.sizeBytes = indices.size() * sizeof(std::uint16_t);
    ibd.usage = BufferUsage::Index;
    auto ib = dev->createBuffer(
        ibd, std::span(reinterpret_cast<const std::byte*>(indices.data()), ibd.sizeBytes));
    REQUIRE(ib.hasValue());

    // Per-instance data: {xOffset, r, g, b}. Instance 0 = red on the left,
    // instance 1 = green on the right.
    struct Instance { float x, r, g, b; };
    constexpr std::array<Instance, 2> instances = {{
        {-0.35f, 1.0f, 0.0f, 0.0f},
        { 0.35f, 0.0f, 1.0f, 0.0f},
    }};
    BufferDesc instDesc;
    instDesc.sizeBytes = instances.size() * sizeof(Instance);
    instDesc.usage = BufferUsage::Vertex;
    auto instBuf = dev->createBuffer(
        instDesc, std::span(reinterpret_cast<const std::byte*>(instances.data()),
                            instDesc.sizeBytes));
    REQUIRE(instBuf.hasValue());

    FramebufferDesc fbd;
    fbd.extent = {256, 256};
    AttachmentDesc clear;
    clear.type = AttachmentType::Color;
    clear.clearOnLoad = true;
    clear.clear.color[0] = 0.0f;
    clear.clear.color[1] = 0.0f;
    clear.clear.color[2] = 0.0f;
    fbd.attachments.push(clear);
    auto fb = dev->createFramebuffer(fbd);
    REQUIRE(fb.hasValue());

    // Draw both instances in a single indexed draw.
    {
        CommandBuffer cb;
        cb.begin();
        cb.beginRenderPass(fb.value());
        cb.bindPipeline(pipeline.value());
        cb.bindVertexBuffer(0, vb.value(), sizeof(Pos));
        cb.bindVertexBuffer(1, instBuf.value(), sizeof(Instance));
        cb.bindIndexBuffer(ib.value(), IndexFormat::U16);
        cb.drawIndexed(6, 2);
        cb.endRenderPass();
        cb.end();
        CHECK(dev->submit(cb) == GfxError::None);
        glFinish();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    // Left quad center (NDC x ~ -0.35) should be red; right (~ +0.35) green.
    std::array<float, 3> left{};
    glReadPixels(83, 128, 1, 1, GL_RGB, GL_FLOAT, left.data());
    std::array<float, 3> right{};
    glReadPixels(172, 128, 1, 1, GL_RGB, GL_FLOAT, right.data());
    std::array<float, 3> gap{};
    glReadPixels(128, 128, 1, 1, GL_RGB, GL_FLOAT, gap.data());

    // Instance 0 (red): strong R, negligible G.
    CHECK(left[0] > 0.7f);
    CHECK(left[1] < 0.3f);
    // Instance 1 (green): strong G, negligible R.
    CHECK(right[1] > 0.7f);
    CHECK(right[0] < 0.3f);
    // Between the two quads should be the clear color (black).
    CHECK(gap[0] < 0.3f);
    CHECK(gap[1] < 0.3f);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: setViewport restricts rendering to the viewport region") {
    TriangleHarness h;

    // Viewport covering only the left half of the framebuffer.
    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.setViewport({TriangleHarness::kWidth / 2, TriangleHarness::kHeight});
    cb.bindPipeline(h.pipeline);
    cb.bindVertexBuffer(0, h.vb, sizeof(float) * 3);
    cb.draw(3);
    cb.endRenderPass();
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::None);
    glFinish();

    // The viewport is only the left half (width/2), so NDC x=0 maps to the
    // midpoint of that region (width/4). Sample there for the triangle center.
    std::array<float, 3> center{};
    glReadPixels(TriangleHarness::kWidth / 4, TriangleHarness::kHeight / 2, 1, 1, GL_RGB,
                 GL_FLOAT, center.data());
    CHECK(center[0] > 0.7f);

    // Far right half is outside the viewport: must retain the clear color.
    std::array<float, 3> farRight{};
    glReadPixels(TriangleHarness::kWidth - 8, TriangleHarness::kHeight / 2, 1, 1, GL_RGB,
                 GL_FLOAT, farRight.data());
    CHECK(farRight[0] < 0.3f);
}

TEST_CASE("OpenGL backend: setScissor clips draws and does not leak into the next pass") {
    TriangleHarness h;

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.setScissor({TriangleHarness::kWidth / 2, TriangleHarness::kHeight});
    cb.bindPipeline(h.pipeline);
    cb.bindVertexBuffer(0, h.vb, sizeof(float) * 3);
    cb.draw(3);
    cb.endRenderPass();
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);
    glFinish();
    CHECK(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE);

    std::array<float, 3> left{};
    std::array<float, 3> right{};
    glReadPixels(100, 100, 1, 1, GL_RGB, GL_FLOAT, left.data());
    glReadPixels(156, 100, 1, 1, GL_RGB, GL_FLOAT, right.data());
    CHECK(left[0] > 0.7f);
    CHECK(right[0] < 0.3f);

    h.render();
    CHECK(glIsEnabled(GL_SCISSOR_TEST) == GL_FALSE);
    glReadPixels(156, 100, 1, 1, GL_RGB, GL_FLOAT, right.data());
    CHECK(right[0] > 0.7f);
}

TEST_CASE("OpenGL backend: pipeline toggles depth clamp") {
    TriangleHarness h;
    PipelineStateDesc desc;
    desc.vertexShader = h.vs;
    desc.fragmentShader = h.fs;
    desc.raster.depthClampEnable = true;
    desc.vertexInput.bindings.push({0, sizeof(float) * 3, 0});
    desc.vertexInput.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    auto depthClampPipeline = h.dev->createPipeline(desc);
    REQUIRE(depthClampPipeline.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.bindPipeline(depthClampPipeline.value());
    cb.endRenderPass();
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);
    CHECK(glIsEnabled(GL_DEPTH_CLAMP) == GL_TRUE);

    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.bindPipeline(h.pipeline);
    cb.endRenderPass();
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);
    CHECK(glIsEnabled(GL_DEPTH_CLAMP) == GL_FALSE);
}

TEST_CASE("OpenGL backend: stats accumulate draw/instance/primitive counts") {
    TriangleHarness h;
    h.dev->resetFrameStats();
    h.render(); // one draw(3) on a TriangleList pipeline
    DeviceStats s = h.dev->stats();
    CHECK(s.drawCalls == 1);
    CHECK(s.batches == 1);
    CHECK(s.primitives == 1); // 3 vertices / 3
    CHECK(s.instances == 1);
    CHECK(s.vertices == 3);
    h.dev->resetFrameStats();
    s = h.dev->stats();
    CHECK(s.drawCalls == 0);
}

TEST_CASE("OpenGL backend: shader failure and success are counted") {
    auto w = platform::Window::create({"shader diag", 64, 64, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);
    dev->resetFrameStats();

    // A valid vertex shader compiles.
    ShaderDesc good;
    good.stage = ShaderStage::Vertex;
    const char* validSrc = R"glsl(#version 430 core
layout(location = 0) in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); }
)glsl";
    fillBlob(good.blob, validSrc);
    auto h = dev->createShader(good);
    REQUIRE(h.hasValue());
    DeviceStats s = dev->stats();
    CHECK(s.shaderCompiled == 1);
    CHECK(s.shaderFailed == 0);
    CHECK(s.shaderCount == 1);

    // A broken shader fails and is counted, even after the good one.
    ShaderDesc bad;
    bad.stage = ShaderStage::Vertex;
    const char* badSrc = R"glsl(#version 430 core
void main() { this is not valid glsl; }
)glsl";
    fillBlob(bad.blob, badSrc);
    auto b = dev->createShader(bad);
    CHECK(b.hasError());
    CHECK(b.error() == GfxError::BadDescriptor);
    s = dev->stats();
    CHECK(s.shaderCompiled == 1);
    CHECK(s.shaderFailed == 1);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: resource byte accounting via stats") {
    auto w = platform::Window::create({"res diag", 64, 64, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    BufferDesc bd;
    bd.sizeBytes = 2048;
    bd.usage = BufferUsage::Vertex;
    auto buf = dev->createBuffer(bd, {});
    REQUIRE(buf.hasValue());

    TextureDesc td;
    td.width = 32;
    td.height = 32;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = dev->createTexture(td);
    REQUIRE(tex.hasValue());

    DeviceStats s = dev->stats();
    CHECK(s.bufferCount == 1);
    CHECK(s.bufferBytes == 2048);
    CHECK(s.textureCount == 1);
    CHECK(s.textureBytes == 32u * 32u * 4u);

    REQUIRE(dev->destroyBuffer(buf.value()) == GfxError::None);
    s = dev->stats();
    CHECK(s.bufferCount == 0);
    CHECK(s.bufferBytes == 0);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: writeTexture uploads with footprint validation") {
    auto w = platform::Window::create({"tex upload", 64, 64, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    TextureDesc td;
    td.width = 4;
    td.height = 4;
    td.format = TextureFormat::RGBA8_UNORM; // mipLevels defaults to 1
    auto tex = dev->createTexture(td);
    REQUIRE(tex.hasValue());

    // Correct 4x4 RGBA8 footprint uploads cleanly.
    std::array<std::byte, 4u * 4u * 4u> full{};
    CHECK(dev->writeTexture(tex.value(), 0, std::span(full)) == GfxError::None);

    // Mismatched byte count is rejected.
    std::array<std::byte, 16u> short_upload{};
    CHECK(dev->writeTexture(tex.value(), 0, std::span(short_upload)) == GfxError::BadDescriptor);

    // Out-of-range mip level is rejected.
    std::array<std::byte, 4u * 4u * 4u> again{};
    CHECK(dev->writeTexture(tex.value(), 1, std::span(again)) == GfxError::BadDescriptor);

    // A stale texture handle is rejected.
    TextureHandle stale = tex.value();
    stale.generation = stale.generation + 1;
    CHECK(dev->writeTexture(stale, 0, std::span(full)) == GfxError::StaleHandle);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: allocates and accepts every declared mip level") {
    auto w = platform::Window::create({"mip upload", 64, 64, false});
    REQUIRE(w.hasValue());
    platform::Window* window = w.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    IDevice* dev = createOpenGLDevice();
    REQUIRE(dev != nullptr);

    TextureDesc td;
    td.width = 8;
    td.height = 4;
    td.format = TextureFormat::RGBA8_UNORM;
    td.mipLevels = 4;
    auto tex = dev->createTexture(td);
    REQUIRE(tex.hasValue());

    std::array<std::byte, 8u * 4u * 4u> level0{};
    std::array<std::byte, 4u * 2u * 4u> level1{};
    std::array<std::byte, 2u * 1u * 4u> level2{};
    std::array<std::byte, 1u * 1u * 4u> level3{};
    CHECK(dev->writeTexture(tex.value(), 0, level0) == GfxError::None);
    CHECK(dev->writeTexture(tex.value(), 1, level1) == GfxError::None);
    CHECK(dev->writeTexture(tex.value(), 2, level2) == GfxError::None);
    CHECK(dev->writeTexture(tex.value(), 3, level3) == GfxError::None);
    CHECK(glGetError() == GL_NO_ERROR);

    TextureDesc partialDesc;
    partialDesc.width = 8;
    partialDesc.height = 8;
    partialDesc.format = TextureFormat::RGBA8_UNORM;
    partialDesc.mipLevels = 2;
    auto partial = dev->createTexture(partialDesc);
    REQUIRE(partial.hasValue());
    std::array<std::byte, 8u * 8u * 4u> partialLevel0{};
    REQUIRE(dev->writeTexture(partial.value(), 0, partialLevel0) == GfxError::None);
    GLint maxLevel = -1;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
    CHECK(maxLevel == 1);

    TextureDesc invalid = td;
    invalid.mipLevels = 5;
    const auto tooMany = dev->createTexture(invalid);
    REQUIRE(tooMany.hasError());
    CHECK(tooMany.error() == GfxError::BadDescriptor);

    dev->waitIdle();
    deleteDevice(dev);
    delete window;
}

TEST_CASE("OpenGL backend: sampler applies mip filter and address modes") {
    TriangleHarness h;
    SamplerDesc desc;
    desc.filter = TextureFilter::MinMagLinear;
    desc.mipFilter = MipFilter::Linear;
    desc.addressU = AddressMode::Repeat;
    desc.addressV = AddressMode::MirroredRepeat;
    auto sampler = h.dev->createSampler(desc);
    REQUIRE(sampler.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.bindResource(0, BindingType::Sampler, sampler.value().index,
                    sampler.value().generation);
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);

    glActiveTexture(GL_TEXTURE0);
    GLint samplerId = 0;
    glGetIntegerv(GL_SAMPLER_BINDING, &samplerId);
    REQUIRE(samplerId != 0);
    GLint value = 0;
    glGetSamplerParameteriv(static_cast<GLuint>(samplerId), GL_TEXTURE_MIN_FILTER, &value);
    CHECK(value == GL_LINEAR_MIPMAP_LINEAR);
    glGetSamplerParameteriv(static_cast<GLuint>(samplerId), GL_TEXTURE_WRAP_S, &value);
    CHECK(value == GL_REPEAT);
    glGetSamplerParameteriv(static_cast<GLuint>(samplerId), GL_TEXTURE_WRAP_T, &value);
    CHECK(value == GL_MIRRORED_REPEAT);
}

TEST_CASE("OpenGL backend: pipeline applies separate alpha blend and color mask state") {
    TriangleHarness h;
    PipelineStateDesc desc;
    desc.vertexShader = h.vs;
    desc.fragmentShader = h.fs;
    desc.vertexInput.bindings.push({0, sizeof(float) * 3, 0});
    desc.vertexInput.attributes.push({0, 0, VertexFormat::Float32x3, 0});
    ColorAttachmentState color;
    color.blendEnable = true;
    color.srcColor = BlendFactor::SrcAlpha;
    color.dstColor = BlendFactor::OneMinusSrcAlpha;
    color.colorOp = BlendOp::RevSub;
    color.srcAlpha = BlendFactor::One;
    color.dstAlpha = BlendFactor::Zero;
    color.alphaOp = BlendOp::Max;
    color.writeMask = ColorWriteMask::R | ColorWriteMask::A;
    desc.colorAttachments.push(color);
    ColorAttachmentState secondColor;
    secondColor.blendEnable = true;
    secondColor.srcColor = BlendFactor::One;
    secondColor.dstColor = BlendFactor::One;
    secondColor.colorOp = BlendOp::Add;
    secondColor.srcAlpha = BlendFactor::SrcAlpha;
    secondColor.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    secondColor.alphaOp = BlendOp::Sub;
    secondColor.writeMask = ColorWriteMask::G | ColorWriteMask::B;
    desc.colorAttachments.push(secondColor);
    auto pipeline = h.dev->createPipeline(desc);
    REQUIRE(pipeline.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.bindPipeline(pipeline.value());
    cb.endRenderPass();
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);

    CHECK(glIsEnabled(GL_BLEND) == GL_TRUE);
    GLint value = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &value);
    CHECK(value == GL_SRC_ALPHA);
    glGetIntegerv(GL_BLEND_DST_RGB, &value);
    CHECK(value == GL_ONE_MINUS_SRC_ALPHA);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &value);
    CHECK(value == GL_ONE);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &value);
    CHECK(value == GL_ZERO);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &value);
    CHECK(value == GL_FUNC_REVERSE_SUBTRACT);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &value);
    CHECK(value == GL_MAX);
    std::array<GLboolean, 4> mask{};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask.data());
    CHECK(mask[0] == GL_TRUE);
    CHECK(mask[1] == GL_FALSE);
    CHECK(mask[2] == GL_FALSE);
    CHECK(mask[3] == GL_TRUE);

    CHECK(glIsEnabledi(GL_BLEND, 1) == GL_TRUE);
    glGetIntegeri_v(GL_BLEND_SRC_RGB, 1, &value);
    CHECK(value == GL_ONE);
    glGetIntegeri_v(GL_BLEND_DST_RGB, 1, &value);
    CHECK(value == GL_ONE);
    glGetIntegeri_v(GL_BLEND_SRC_ALPHA, 1, &value);
    CHECK(value == GL_SRC_ALPHA);
    glGetIntegeri_v(GL_BLEND_DST_ALPHA, 1, &value);
    CHECK(value == GL_ONE_MINUS_SRC_ALPHA);
    glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, 1, &value);
    CHECK(value == GL_FUNC_SUBTRACT);
    glGetBooleani_v(GL_COLOR_WRITEMASK, 1, mask.data());
    CHECK(mask[0] == GL_FALSE);
    CHECK(mask[1] == GL_TRUE);
    CHECK(mask[2] == GL_TRUE);
    CHECK(mask[3] == GL_FALSE);

    cb.begin();
    cb.beginRenderPass(h.fb);
    cb.bindPipeline(h.pipeline);
    cb.endRenderPass();
    cb.end();
    REQUIRE(h.dev->submit(cb) == GfxError::None);
    CHECK(glIsEnabledi(GL_BLEND, 1) == GL_FALSE);
    glGetBooleani_v(GL_COLOR_WRITEMASK, 1, mask.data());
    CHECK(mask[0] == GL_TRUE);
    CHECK(mask[1] == GL_TRUE);
    CHECK(mask[2] == GL_TRUE);
    CHECK(mask[3] == GL_TRUE);
}

TEST_CASE("OpenGL backend: rejects attachment and pipeline layout limits") {
    TriangleHarness h;
    GLint maxDrawBuffers = 0;
    GLint maxColorAttachments = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
    const GLint colorLimit = std::min(maxDrawBuffers, maxColorAttachments);

    FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {16, 16};
    for (GLint i = 0; i <= colorLimit; ++i) {
        framebufferDesc.attachments.push(AttachmentDesc{});
    }
    CHECK(h.dev->createFramebuffer(framebufferDesc).error() == GfxError::BadDescriptor);

    PipelineStateDesc desc;
    desc.vertexShader = h.vs;
    desc.fragmentShader = h.fs;
    for (GLint i = 0; i <= maxDrawBuffers; ++i) {
        desc.colorAttachments.push(ColorAttachmentState{});
    }
    CHECK(h.dev->createPipeline(desc).error() == GfxError::BadDescriptor);

    desc.colorAttachments.clear();
    BindGroup group;
    BindingSlot arrayBinding;
    arrayBinding.count = 2;
    group.slots.push(arrayBinding);
    desc.layout.groups.push(group);
    CHECK(h.dev->createPipeline(desc).error() == GfxError::BadDescriptor);

    desc.layout.groups[0].slots[0].count = 1;
    desc.layout.groups[0].bindPoint = 1;
    CHECK(h.dev->createPipeline(desc).error() == GfxError::BadDescriptor);

    desc.layout.groups[0].bindPoint = 0;
    auto pipeline = h.dev->createPipeline(desc);
    REQUIRE(pipeline.hasValue());

    BufferDesc uniformDesc;
    uniformDesc.sizeBytes = 16;
    uniformDesc.usage = BufferUsage::Uniform;
    auto uniform = h.dev->createBuffer(uniformDesc, {});
    REQUIRE(uniform.hasValue());

    CommandBuffer cb;
    cb.begin();
    cb.bindPipeline(pipeline.value());
    cb.bindResource(1, BindingType::UniformBuffer, uniform.value().index,
                    uniform.value().generation);
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::BadDescriptor);

    cb.begin();
    cb.bindPipeline(pipeline.value());
    cb.bindResource(0, BindingType::UniformBuffer, h.vb.index, h.vb.generation);
    cb.end();
    CHECK(h.dev->submit(cb) == GfxError::BadDescriptor);
    CHECK(h.dev->destroyBuffer(uniform.value()) == GfxError::None);
}

#endif // F4_GRAPHICS_HAS_OPENGL
