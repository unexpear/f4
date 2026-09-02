#include "demos/demo_triangle.h"

#include <cstring>

#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
out vec3 vColor;
void main() { gl_Position = vec4(aPos, 1.0); vColor = aColor; }
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)glsl";

constexpr float kPositions[] = {
    0.0f, 0.5f, 0.0f,
    -0.5f, -0.5f, 0.0f,
    0.5f, -0.5f, 0.0f,
};
constexpr float kColors[] = {
    1.0f, 0.3f, 0.3f,
    0.3f, 1.0f, 0.3f,
    0.3f, 0.3f, 1.0f,
};

} // namespace

bool TriangleScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;

    // Position buffer
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kPositions);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kPositions), sizeof(kPositions)));
    if (!vb.hasValue()) return false;
    vbPos_ = vb.value();

    // Color buffer
    gfx::BufferDesc cbd;
    cbd.sizeBytes = sizeof(kColors);
    cbd.usage = gfx::BufferUsage::Vertex;
    auto cb = dev->createBuffer(cbd, std::span(reinterpret_cast<const std::byte*>(kColors), sizeof(kColors)));
    if (!cb.hasValue()) return false;
    vbColor_ = cb.value();

    // Shaders
    auto makeShader = [&](gfx::ShaderStage stage, const char* src) {
        gfx::ShaderDesc d;
        d.stage = stage;
        d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size());
        return dev->createShader(d);
    };
    auto vs = makeShader(gfx::ShaderStage::Vertex, kVs);
    auto fs = makeShader(gfx::ShaderStage::Fragment, kFs);
    if (!vs.hasValue() || !fs.hasValue()) return false;
    vs_ = vs.value();
    fs_ = fs.value();

    // Pipeline
    gfx::PipelineStateDesc pd;
    pd.vertexShader = vs_;
    pd.fragmentShader = fs_;
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.raster.cullMode = gfx::CullMode::None;
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 12;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = gfx::VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
    }
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 1;
        b.strideBytes = 12;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 1;
        ca.location = 1;
        ca.format = gfx::VertexFormat::Float32x3;
        ca.offsetBytes = 0;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    inited_ = true;
    return true;
}

void TriangleScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbColor_.isValid()) dev->destroyBuffer(vbColor_);
    if (vbPos_.isValid()) dev->destroyBuffer(vbPos_);
    inited_ = false;
}

void TriangleScene::onResize(std::uint32_t, std::uint32_t) {
    // Nothing to do for this simple demo
}

void TriangleScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {
    // Static triangle, no update needed
}

void TriangleScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbPos_, 12);
    cb.bindVertexBuffer(1, vbColor_, 12);
    cb.draw(3);
}

} // namespace f4::demo