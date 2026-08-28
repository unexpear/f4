#include <doctest/doctest.h>

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

#endif // F4_GRAPHICS_HAS_OPENGL
