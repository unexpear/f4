#include "demos/demo_collision.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr float kQuad[] = {
    -0.5f, -0.5f, 0.0f,
    0.5f, -0.5f, 0.0f,
    -0.5f, 0.5f, 0.0f,
    0.5f, 0.5f, 0.0f,
};
constexpr std::uint16_t kIndices[] = {0, 1, 2, 2, 1, 3};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aCenter;
layout(location = 2) in vec3 aColor;
out vec3 vColor;
void main() {
    gl_Position = vec4(aCenter.xy + aPos.xy * aCenter.z, 0.0, 1.0);
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

bool CollisionScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Quad vertex buffer
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kQuad);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kQuad), sizeof(kQuad)));
    if (!vb.hasValue()) return false;
    vbQuad_ = vb.value();

    // Index buffer
    gfx::BufferDesc ibd;
    ibd.sizeBytes = sizeof(kIndices);
    ibd.usage = gfx::BufferUsage::Index;
    auto ib = dev->createBuffer(ibd, std::span(reinterpret_cast<const std::byte*>(kIndices), sizeof(kIndices)));
    if (!ib.hasValue()) return false;
    ibQuad_ = ib.value();

    // Instance buffer (alloc)
    gfx::BufferDesc id;
    id.sizeBytes = kBodies * sizeof(Instance);
    id.usage = gfx::BufferUsage::Vertex;
    auto iib = dev->createBuffer(id, {});
    if (!iib.hasValue()) return false;
    vbInstance_ = iib.value();

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
    // Binding 0: quad vertices
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
    // Binding 1: per-instance center+size + color
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 1;
        b.strideBytes = sizeof(Instance);
        b.divisor = 1;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 1;
        ca.location = 1;
        ca.format = gfx::VertexFormat::Float32x3;
        ca.offsetBytes = 0;
        pd.vertexInput.attributes.push(ca);
        gfx::VertexInputLayout::VertexAttribute cb;
        cb.binding = 1;
        cb.location = 2;
        cb.format = gfx::VertexFormat::Float32x3;
        cb.offsetBytes = 12;
        pd.vertexInput.attributes.push(cb);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    // Initialize bodies
    for (int i = 0; i < kBodies; ++i) {
        bodies_[i].x = (i % 4) * 0.4f - 0.6f;
        bodies_[i].y = (i / 4) * 0.4f - 0.2f;
        bodies_[i].vx = (i % 2 == 0 ? 0.3f : -0.4f);
        bodies_[i].vy = (i < 4 ? 0.25f : -0.35f);
        bodies_[i].size = 0.08f + i * 0.01f;
        float h = i * 0.4f;
        bodies_[i].r = 0.5f + 0.5f * std::sin(h);
        bodies_[i].g = 0.5f + 0.5f * std::sin(h + 2.1f);
        bodies_[i].b = 0.5f + 0.5f * std::sin(h + 4.2f);
    }

    inited_ = true;
    return true;
}

void CollisionScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbInstance_.isValid()) dev->destroyBuffer(vbInstance_);
    if (ibQuad_.isValid()) dev->destroyBuffer(ibQuad_);
    if (vbQuad_.isValid()) dev->destroyBuffer(vbQuad_);
    inited_ = false;
}

void CollisionScene::onResize(std::uint32_t, std::uint32_t) {
    // NDC demo, no resize handling needed
}

void CollisionScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    if (!inited_) return;

    // Simple 2D collision simulation in NDC space
    for (int i = 0; i < kBodies; ++i) {
        bodies_[i].x += bodies_[i].vx * dt;
        bodies_[i].y += bodies_[i].vy * dt;

        // Wall bounce
        float half = bodies_[i].size;
        if (bodies_[i].x - half < -1.f) { bodies_[i].x = -1.f + half; bodies_[i].vx = -bodies_[i].vx; }
        if (bodies_[i].x + half > 1.f) { bodies_[i].x = 1.f - half; bodies_[i].vx = -bodies_[i].vx; }
        if (bodies_[i].y - half < -1.f) { bodies_[i].y = -1.f + half; bodies_[i].vy = -bodies_[i].vy; }
        if (bodies_[i].y + half > 1.f) { bodies_[i].y = 1.f - half; bodies_[i].vy = -bodies_[i].vy; }

        // Body-body collision (simple elastic)
        for (int j = i + 1; j < kBodies; ++j) {
            float dx = bodies_[j].x - bodies_[i].x;
            float dy = bodies_[j].y - bodies_[i].y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float minDist = bodies_[i].size + bodies_[j].size;
            if (dist < minDist && dist > 0.001f) {
                float nx = dx / dist, ny = dy / dist;
                float dvx = bodies_[j].vx - bodies_[i].vx;
                float dvy = bodies_[j].vy - bodies_[i].vy;
                float dp = dvx * nx + dvy * ny;
                if (dp < 0) {
                    float impulse = dp / (1.f + 1.f); // equal mass
                    bodies_[i].vx += impulse * nx;
                    bodies_[i].vy += impulse * ny;
                    bodies_[j].vx -= impulse * nx;
                    bodies_[j].vy -= impulse * ny;
                    // Separate slightly
                    float overlap = minDist - dist;
                    bodies_[i].x -= nx * overlap * 0.5f;
                    bodies_[i].y -= ny * overlap * 0.5f;
                    bodies_[j].x += nx * overlap * 0.5f;
                    bodies_[j].y += ny * overlap * 0.5f;
                }
            }
        }
    }

    // Update instance buffer
    Instance instances[kBodies];
    for (int i = 0; i < kBodies; ++i) {
        instances[i].x = bodies_[i].x;
        instances[i].y = bodies_[i].y;
        instances[i].size = bodies_[i].size;
        instances[i].r = bodies_[i].r;
        instances[i].g = bodies_[i].g;
        instances[i].b = bodies_[i].b;
    }
    if (device_) {
        device_->writeBuffer(vbInstance_, 0, std::span(reinterpret_cast<const std::byte*>(instances), sizeof(instances)));
    }
}

void CollisionScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbQuad_, 12);
    cb.bindVertexBuffer(1, vbInstance_, sizeof(Instance));
    cb.bindIndexBuffer(ibQuad_, gfx::IndexFormat::U16);
    cb.drawIndexed(6, kBodies);
}

} // namespace f4::demo