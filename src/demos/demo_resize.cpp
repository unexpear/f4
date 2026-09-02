#include "demos/demo_resize.h"

#include <cstring>

#include "core/math/mat4.h"
#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr float kTri[] = {
    0.0f, 0.0f, 0.0f, 1.0f, 0.3f, 0.3f,
    1.0f, 0.0f, 0.0f, 0.3f, 1.0f, 0.3f,
    0.0f, 1.0f, 0.0f, 0.3f, 0.3f, 1.0f,
};
constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform Ubo { mat4 mvp; };
out vec3 vColor;
void main() { gl_Position = mvp * vec4(aPos, 1.0); vColor = aColor; }
)glsl";
constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)glsl";

} // namespace

bool ResizeScene::init(gfx::IDevice* dev, platform::Window* window) {
    if (inited_) return true;
    device_ = dev;
    width_ = window->width();
    height_ = window->height();

    // Vertex buffer (interleaved pos+color, stride 24)
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kTri);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kTri), sizeof(kTri)));
    if (!vb.hasValue()) return false;
    vbPos_ = vb.value();

    // Per-object constant ring (std140: mat4 = 64 bytes)
    if (constRing_.create(*dev, 64, 16) != gfx::GfxError::None) return false;

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

void ResizeScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbPos_.isValid()) dev->destroyBuffer(vbPos_);
    constRing_.destroy(*dev);
    inited_ = false;
}

void ResizeScene::onResize(std::uint32_t width, std::uint32_t height) {
    width_ = width;
    height_ = height;
}

void ResizeScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {
    // MVP is computed in render() since it needs the command buffer for writeAndBind
}

void ResizeScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    constRing_.beginFrame();
    float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_) : 1.f;
    math::Mat4 proj = math::Mat4::perspectiveRH_ZO(1.04719758f, aspect, 0.1f, 100.f);
    math::Mat4 view = math::Mat4::lookAtRH({0.f, 0.f, 3.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    math::Mat4 mvp = proj.mul(view);
    auto slot = constRing_.writeAndBind(*device_, cb, std::span(reinterpret_cast<const std::byte*>(&mvp), sizeof(mvp)));
    if (slot.valid) {
        cb.bindPipeline(pipeline_);
        cb.bindVertexBuffer(0, vbPos_, 24);
        cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
        cb.draw(3);
    }
}

} // namespace f4::demo
