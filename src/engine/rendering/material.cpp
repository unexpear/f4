#include "engine/rendering/material.h"

#include <cstring>

#include "core/ids/handle.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::engine {

namespace {

auto tintBits(const math::Vec4& v) -> std::uint64_t {
    std::uint32_t bits[4];
    std::memcpy(&bits[0], &v.x, 4);
    std::memcpy(&bits[1], &v.y, 4);
    std::memcpy(&bits[2], &v.z, 4);
    std::memcpy(&bits[3], &v.w, 4);
    std::uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4; ++i) {
        h ^= bits[i];
        h *= 1099511628211ull;
    }
    return h;
}

constexpr const char* kStandardVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(std140, binding = 0) uniform SceneUbo { mat4 viewProj; mat4 model; vec4 tint; };
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
void main() {
    vNormal = aNormal;
    vUV = aUV;
    vColor = aColor * tint;
    gl_Position = viewProj * model * vec4(aPos, 1.0);
}
)glsl";

constexpr const char* kStandardFs = R"glsl(
#version 430 core
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    vec4 tex = texture(uTex, vUV);
    fragColor = vColor * tex;
}
)glsl";

void fillBlob(gfx::ShaderDesc& d, const char* src) {
    d.blob.resize(std::strlen(src));
    std::memcpy(d.blob.data(), src, d.blob.size());
}

} // namespace

auto MaterialCache::hashDesc(const StandardMaterialDesc& d) -> std::uint64_t {
    std::uint64_t h = tintBits(d.tint);
    h ^= static_cast<std::uint64_t>(d.albedo.index) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(d.albedo.generation) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= d.transparent ? 1ull : 0ull;
    h *= 1099511628211ull;
    return h;
}

auto MaterialCache::find(const StandardMaterialDesc& desc) const -> resources::MaterialHandle {
    const std::uint64_t h = hashDesc(desc);
    const auto* slot = lookup_.find(h);
    if (slot == nullptr) return resources::MaterialHandle{};
    const StandardMaterial* m = arena_.get(*slot);
    if (m != nullptr && m->desc == desc) return *slot;
    return resources::MaterialHandle{};
}

auto MaterialCache::getOrCreate(const StandardMaterialDesc& desc) -> resources::MaterialHandle {
    if (auto existing = find(desc); existing.isValid()) return existing;
    StandardMaterial mat; mat.desc = desc;
    auto h = arena_.acquire(mat);
    lookup_.set(hashDesc(desc), h);
    return h;
}

auto MaterialCache::ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle h) -> gfx::GfxError {
    StandardMaterial* mat = arena_.get(h);
    if (mat == nullptr) return gfx::GfxError::InvalidHandle;
    if (mat->pipeline.isValid()) return gfx::GfxError::None;
    gfx::ShaderDesc vsDesc; vsDesc.stage = gfx::ShaderStage::Vertex; fillBlob(vsDesc, kStandardVs);
    auto vsRes = dev.createShader(vsDesc);
    if (!vsRes.hasValue()) return vsRes.error();
    gfx::ShaderDesc fsDesc; fsDesc.stage = gfx::ShaderStage::Fragment; fillBlob(fsDesc, kStandardFs);
    auto fsRes = dev.createShader(fsDesc);
    if (!fsRes.hasValue()) { dev.destroyShader(vsRes.value()); return fsRes.error(); }
    gfx::PipelineStateDesc pd;
    pd.vertexShader = vsRes.value(); pd.fragmentShader = fsRes.value();
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.raster.cullMode = gfx::CullMode::None;
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = true;
    pd.depthStencil.depthCompare = gfx::CompareOp::Less;
    if (mat->desc.transparent) {
        gfx::ColorAttachmentState ca; ca.blendEnable = true;
        ca.srcColor = gfx::BlendFactor::SrcAlpha; ca.dstColor = gfx::BlendFactor::OneMinusSrcAlpha;
        ca.srcAlpha = gfx::BlendFactor::One; ca.dstAlpha = gfx::BlendFactor::OneMinusSrcAlpha;
        pd.colorAttachments.push(ca);
    }
    {
        gfx::VertexInputLayout::VertexBinding b; b.binding = 0; b.strideBytes = 36;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute a0; a0.binding=0; a0.location=0; a0.format=gfx::VertexFormat::Float32x3; a0.offsetBytes=0; pd.vertexInput.attributes.push(a0);
        gfx::VertexInputLayout::VertexAttribute a1; a1.binding=0; a1.location=1; a1.format=gfx::VertexFormat::Float32x3; a1.offsetBytes=12; pd.vertexInput.attributes.push(a1);
        gfx::VertexInputLayout::VertexAttribute a2; a2.binding=0; a2.location=2; a2.format=gfx::VertexFormat::Float32x2; a2.offsetBytes=24; pd.vertexInput.attributes.push(a2);
        gfx::VertexInputLayout::VertexAttribute a3; a3.binding=0; a3.location=3; a3.format=gfx::VertexFormat::UNorm8x4; a3.offsetBytes=32; pd.vertexInput.attributes.push(a3);
    }
    {
        gfx::BindGroup g; g.bindPoint=0;
        gfx::BindingSlot ubo; ubo.slot=0; ubo.type=gfx::BindingType::UniformBuffer; g.slots.push(ubo);
        gfx::BindingSlot tex; tex.slot=0; tex.type=gfx::BindingType::Texture; g.slots.push(tex);
        gfx::BindingSlot smp; smp.slot=0; smp.type=gfx::BindingType::Sampler; g.slots.push(smp);
        pd.layout.groups.push(g);
    }
    auto pRes = dev.createPipeline(pd);
    if (!pRes.hasValue()) { dev.destroyShader(vsRes.value()); dev.destroyShader(fsRes.value()); return pRes.error(); }
    mat->vs = vsRes.value(); mat->fs = fsRes.value(); mat->pipeline = pRes.value();
    return gfx::GfxError::None;
}

auto MaterialCache::destroy(resources::MaterialHandle h, gfx::IDevice& dev) -> gfx::GfxError {
    StandardMaterial* m = arena_.get(h);
    if (m == nullptr) return gfx::GfxError::InvalidHandle;
    if (m->pipeline.isValid()) dev.destroyPipeline(m->pipeline);
    if (m->vs.isValid()) dev.destroyShader(m->vs);
    if (m->fs.isValid()) dev.destroyShader(m->fs);
    lookup_.erase(hashDesc(m->desc));
    arena_.release(h);
    return gfx::GfxError::None;
}

void MaterialCache::clear(gfx::IDevice& dev) {
    arena_.forEach([&](StandardMaterial* m) {
        if (m->pipeline.isValid()) dev.destroyPipeline(m->pipeline);
        if (m->vs.isValid()) dev.destroyShader(m->vs);
        if (m->fs.isValid()) dev.destroyShader(m->fs);
    });
    lookup_.clear(); arena_.clear();
}

} // namespace f4::engine
