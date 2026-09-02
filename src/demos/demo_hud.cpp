#include "demos/demo_hud.h"

#include <cstring>

#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr float kTri[] = {
    0.0f, 0.5f, 0.0f, 1.0f, 0.3f, 0.3f,
    -0.5f, -0.5f, 0.0f, 0.3f, 1.0f, 0.3f,
    0.5f, -0.5f, 0.0f, 0.3f, 0.3f, 1.0f,
};

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

} // namespace

bool HudScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;

    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kTri);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kTri), sizeof(kTri)));
    if (!vb.hasValue()) return false;
    vbTri_ = vb.value();

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

    gfx::PipelineStateDesc pd;
    pd.vertexShader = vs_;
    pd.fragmentShader = fs_;
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.raster.cullMode = gfx::CullMode::None;
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 24;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = gfx::VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 0;
        ca.location = 1;
        ca.format = gfx::VertexFormat::Float32x3;
        ca.offsetBytes = 12;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    inited_ = true;
    return true;
}

void HudScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbTri_.isValid()) dev->destroyBuffer(vbTri_);
    inited_ = false;
}

void HudScene::onResize(std::uint32_t, std::uint32_t) {}

void HudScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void HudScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbTri_, 24);
    cb.draw(3);
}

} // namespace f4::demo