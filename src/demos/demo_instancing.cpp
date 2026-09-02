#include "demos/demo_instancing.h"

#include <cmath>
#include <cstring>

#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr float kTriangleVerts[] = {
    0.0f, 0.08f, 0.0f,
    -0.07f, -0.06f, 0.0f,
    0.07f, -0.06f, 0.0f,
};

struct Instance {
    float ox, oy;
    float r, g, b;
};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aOffset;
layout(location = 2) in vec3 aColor;
out vec3 vColor;
void main() {
    gl_Position = vec4(aPos.xy + aOffset.xy, 0.0, 1.0);
    vColor = aColor;
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)glsl";

} // namespace

bool InstancingScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Triangle vertex buffer (binding 0)
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kTriangleVerts);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kTriangleVerts), sizeof(kTriangleVerts)));
    if (!vb.hasValue()) return false;
    vbPos_ = vb.value();

    // Instance buffer (binding 1, divisor 1) - allocate storage
    gfx::BufferDesc id;
    id.sizeBytes = kInstanceCount * sizeof(Instance);
    id.usage = gfx::BufferUsage::Vertex;
    auto ib = dev->createBuffer(id, {});
    if (!ib.hasValue()) return false;
    vbInstance_ = ib.value();

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
    // Binding 0: per-vertex position (divisor 0)
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 12;
        b.divisor = 0;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = gfx::VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
    }
    // Binding 1: per-instance offset + color (divisor 1)
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 1;
        b.strideBytes = sizeof(Instance);
        b.divisor = 1;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute oa;
        oa.binding = 1;
        oa.location = 1;
        oa.format = gfx::VertexFormat::Float32x2;
        oa.offsetBytes = 0;
        pd.vertexInput.attributes.push(oa);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 1;
        ca.location = 2;
        ca.format = gfx::VertexFormat::Float32x3;
        ca.offsetBytes = 8;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    // Initial instance data upload
    std::vector<Instance> instances(kInstanceCount);
    float spacing = 0.20f;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            int i = y * kGrid + x;
            instances[i].ox = (x - (kGrid - 1) * 0.5f) * spacing;
            instances[i].oy = (y - (kGrid - 1) * 0.5f) * spacing;
            float h = (x * 13 + y * 7) * 0.01f;
            instances[i].r = 0.5f + 0.5f * std::sin(h);
            instances[i].g = 0.5f + 0.5f * std::sin(h + 2.1f);
            instances[i].b = 0.5f + 0.5f * std::sin(h + 4.2f);
        }
    }
    dev->writeBuffer(vbInstance_, 0, std::span(reinterpret_cast<const std::byte*>(instances.data()), instances.size() * sizeof(Instance)));

    inited_ = true;
    return true;
}

void InstancingScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbInstance_.isValid()) dev->destroyBuffer(vbInstance_);
    if (vbPos_.isValid()) dev->destroyBuffer(vbPos_);
    inited_ = false;
}

void InstancingScene::onResize(std::uint32_t, std::uint32_t) {
    // Nothing special
}

void InstancingScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    time_ += dt;
    // Update instance colors over time (animate)
    std::vector<Instance> instances(kInstanceCount);
    float spacing = 0.20f;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) {
            int i = y * kGrid + x;
            instances[i].ox = (x - (kGrid - 1) * 0.5f) * spacing;
            instances[i].oy = (y - (kGrid - 1) * 0.5f) * spacing;
            float h = (x * 13 + y * 7) * 0.01f + time_;
            instances[i].r = 0.5f + 0.5f * std::sin(h);
            instances[i].g = 0.5f + 0.5f * std::sin(h + 2.1f);
            instances[i].b = 0.5f + 0.5f * std::sin(h + 4.2f);
        }
    }
    if (device_) {
        device_->writeBuffer(vbInstance_, 0, std::span(reinterpret_cast<const std::byte*>(instances.data()), instances.size() * sizeof(Instance)));
    }
}

void InstancingScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbPos_, 12);
    cb.bindVertexBuffer(1, vbInstance_, sizeof(Instance));
    cb.draw(3, kInstanceCount);
}

} // namespace f4::demo
