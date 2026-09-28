#include "demos/demo_particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

constexpr float kQuad[] = {
    -0.5f, -0.5f, 0.f,  0.f, 0.f,
     0.5f, -0.5f, 0.f,  1.f, 0.f,
    -0.5f,  0.5f, 0.f,  0.f, 1.f,
     0.5f,  0.5f, 0.f,  1.f, 1.f,
};
constexpr std::uint16_t kIndices[] = {0, 1, 2, 2, 1, 3};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aCenter;
layout(location = 3) in vec4 aColorSize; // xyz = color, w = size
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    vec3 cameraRight;
    vec3 cameraUp;
};
out vec2 vUV;
out vec4 vColor;
void main() {
    vec3 center = aCenter;
    float size = aColorSize.w;
    vec3 pos = center + aPos.x * cameraRight * size + aPos.y * cameraUp * size;
    gl_Position = viewProj * vec4(pos, 1.0);
    vUV = aUV;
    vColor = aColorSize;
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
void main() {
    float d = length(vUV - vec2(0.5));
    float alpha = 1.0 - smoothstep(0.0, 0.5, d);
    fragColor = vec4(vColor.xyz, vColor.w * alpha);
}
)glsl";

} // namespace

bool ParticleScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Quad
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kQuad);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kQuad), sizeof(kQuad)));
    if (!vb.hasValue()) return false;
    vbQuad_ = vb.value();

    gfx::BufferDesc ibd;
    ibd.sizeBytes = sizeof(kIndices);
    ibd.usage = gfx::BufferUsage::Index;
    auto ib = dev->createBuffer(ibd, std::span(reinterpret_cast<const std::byte*>(kIndices), sizeof(kIndices)));
    if (!ib.hasValue()) return false;
    ibQuad_ = ib.value();

    // Instance buffer (center + colorSize per particle)
    struct Instance { math::Vec3 center; math::Vec4 colorSize; };
    gfx::BufferDesc id;
    id.sizeBytes = kMaxParticles * sizeof(Instance);
    id.usage = gfx::BufferUsage::Vertex;
    auto iib = dev->createBuffer(id, {});
    if (!iib.hasValue()) return false;
    vbInstance_ = iib.value();

    // Per-object constant ring (std140: mat4 64 + vec4 16 + vec4 16 = 96)
    if (constRing_.create(*dev, 96, 16) != gfx::GfxError::None) return false;

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
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = false; // particles don't write depth
    pd.depthStencil.depthCompare = gfx::CompareOp::Less;
    gfx::ColorAttachmentState cas;
    cas.blendEnable = true;
    cas.srcColor = gfx::BlendFactor::SrcAlpha;
    cas.dstColor = gfx::BlendFactor::OneMinusSrcAlpha;
    cas.colorOp = gfx::BlendOp::Add;
    cas.srcAlpha = gfx::BlendFactor::One;
    cas.dstAlpha = gfx::BlendFactor::OneMinusSrcAlpha;
    cas.alphaOp = gfx::BlendOp::Add;
    cas.writeMask = gfx::ColorWriteMask::All;
    pd.colorAttachments.push(cas);

    // Binding 0: quad
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0; b.strideBytes = 20; b.divisor = 0;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0; pa.location = 0; pa.format = gfx::VertexFormat::Float32x3; pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ua;
        ua.binding = 0; ua.location = 1; ua.format = gfx::VertexFormat::Float32x2; ua.offsetBytes = 12;
        pd.vertexInput.attributes.push(ua);
    }
    // Binding 1: instance (center vec3 + colorSize vec4 = 28 B tightly packed)
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 1; b.strideBytes = 28; b.divisor = 1;
        pd.vertexInput.bindings.push(b);
        // aCenter
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 1; ca.location = 2; ca.format = gfx::VertexFormat::Float32x3; ca.offsetBytes = 0;
        pd.vertexInput.attributes.push(ca);
        // aColorSize
        gfx::VertexInputLayout::VertexAttribute cb;
        cb.binding = 1; cb.location = 3; cb.format = gfx::VertexFormat::Float32x4; cb.offsetBytes = 12;
        pd.vertexInput.attributes.push(cb);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    // Initialize particles
    aliveCount_ = 0;
    spawnTimer_ = 0.f;
    for (int i = 0; i < kMaxParticles; ++i) {
        particles_[i].pos = {0.f, 0.f, 0.f};
        particles_[i].vel = {0.f, 0.f, 0.f};
        particles_[i].life = 0.f;
        particles_[i].maxLife = 0.f;
        particles_[i].size = 0.f;
        particles_[i].color = {0.f, 0.f, 0.f};
    }

    // Start with a deterministic slice of the emitter already in flight so a
    // short smoke run exercises instancing, billboarding, and alpha blending.
    constexpr int kInitialParticles = 128;
    aliveCount_ = kInitialParticles;
    for (int i = 0; i < aliveCount_; ++i) {
        spawnParticle(i);
        const float phase = static_cast<float>(i) /
                            static_cast<float>(kInitialParticles - 1);
        const float age = std::min(phase * 1.25f, particles_[i].life * 0.75f);
        particles_[i].pos = particles_[i].vel * age;
        particles_[i].pos.y -= 1.5f * age * age;
        particles_[i].vel.y -= 3.f * age;
        particles_[i].life -= age;
    }

    inited_ = true;
    return true;
}

void ParticleScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (vbInstance_.isValid()) dev->destroyBuffer(vbInstance_);
    if (ibQuad_.isValid()) dev->destroyBuffer(ibQuad_);
    if (vbQuad_.isValid()) dev->destroyBuffer(vbQuad_);
    constRing_.destroy(*dev);
    inited_ = false;
}

void ParticleScene::onResize(std::uint32_t, std::uint32_t) {}

void ParticleScene::spawnParticle(int i) {
    std::mt19937 rng(0xC0FFEE + i);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::uniform_real_distribution<float> distVel(0.5f, 2.f);
    std::uniform_real_distribution<float> distLife(1.f, 3.f);
    std::uniform_real_distribution<float> distSize(0.1f, 0.3f);
    std::uniform_real_distribution<float> distHue(0.f, 6.28f);

    particles_[i].pos = {0.f, 0.f, 0.f};
    float angle = dist(rng) * 6.28f;
    float pitch = dist(rng) * 0.5f;
    float speed = distVel(rng);
    particles_[i].vel = {
        std::cos(angle) * std::cos(pitch) * speed,
        std::sin(pitch) * speed + 1.f,
        std::sin(angle) * std::cos(pitch) * speed
    };
    particles_[i].life = distLife(rng);
    particles_[i].maxLife = particles_[i].life;
    particles_[i].size = distSize(rng);
    float hue = distHue(rng);
    particles_[i].color = {
        0.5f + 0.5f * std::sin(hue),
        0.5f + 0.5f * std::sin(hue + 2.1f),
        0.5f + 0.5f * std::sin(hue + 4.2f)
    };
}

void ParticleScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    if (!inited_) return;

    spawnTimer_ += dt;
    while (spawnTimer_ > 0.02f && aliveCount_ < kMaxParticles) {
        spawnParticle(aliveCount_);
        aliveCount_++;
        spawnTimer_ -= 0.02f;
    }

    for (int i = 0; i < aliveCount_; ++i) {
        particles_[i].pos.x += particles_[i].vel.x * dt;
        particles_[i].pos.y += particles_[i].vel.y * dt;
        particles_[i].pos.z += particles_[i].vel.z * dt;
        particles_[i].vel.y -= 3.f * dt; // gravity
        particles_[i].life -= dt;
        if (particles_[i].life <= 0.f) {
            // Respawn
            spawnParticle(i);
        }
    }
}

bool ParticleScene::prepareStaticCapture(FreeCamera& camera) {
    camera.position = {0.f, 0.f, 6.f};
    camera.yaw = 1.57079633f;
    camera.pitch = std::atan2(1.5f, 6.f);
    return true;
}

void ParticleScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) {
    if (!inited_ || aliveCount_ == 0) return;
    constRing_.beginFrame();

    float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
    math::Mat4 view = camera.viewMatrix();
    math::Mat4 viewProj = camera.viewProjMatrix(aspect);
    const bool atDefault = camera.position.x == 0.f && camera.position.y == 5.f
                           && camera.position.z == 8.f && camera.yaw == 1.57079633f
                           && camera.pitch == 0.55f;
    if (atDefault) {
        view = math::Mat4::lookAtRH({0.f, 2.5f, 5.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f});
        viewProj = math::Mat4::perspectiveRH_ZO(1.04719758f, aspect, 0.1f, 100.f).mul(view);
    }
    math::Vec3 cameraRight{view(0, 0), view(0, 1), view(0, 2)};
    math::Vec3 cameraUp{view(1, 0), view(1, 1), view(1, 2)};

    struct UboData {
        math::Mat4 viewProj;
        math::Vec3 cameraRight;
        float pad1;
        math::Vec3 cameraUp;
        float pad2;
    };
    UboData ubo;
    ubo.viewProj = viewProj;
    ubo.cameraRight = cameraRight; ubo.pad1 = 0;
    ubo.cameraUp = cameraUp; ubo.pad2 = 0;

    auto slot = constRing_.writeAndBind(*device_, cb, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
    if (!slot.valid) return;

    // Build instance data
    struct Instance { math::Vec3 center; math::Vec4 colorSize; };
    std::vector<Instance> instances(aliveCount_);
    for (int i = 0; i < aliveCount_; ++i) {
        instances[i].center = particles_[i].pos;
        float alpha = particles_[i].life / particles_[i].maxLife;
        instances[i].colorSize = {particles_[i].color.x, particles_[i].color.y, particles_[i].color.z, particles_[i].size * alpha};
    }
    if (device_) {
        device_->writeBuffer(vbInstance_, 0, std::span(reinterpret_cast<const std::byte*>(instances.data()), instances.size() * sizeof(Instance)));
    }

    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbQuad_, 20);
    cb.bindVertexBuffer(1, vbInstance_, 28);
    cb.bindIndexBuffer(ibQuad_, gfx::IndexFormat::U16);
    cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
    cb.drawIndexed(6, static_cast<std::uint32_t>(aliveCount_));
}

} // namespace f4::demo
