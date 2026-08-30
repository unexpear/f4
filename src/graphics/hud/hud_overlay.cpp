#include "graphics/hud/hud_overlay.h"

#include <cstring>

#include "core/containers/array.h"
#include "core/math/mat4.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"

namespace f4::gfx::hud {

namespace {

// Synthetic glyph scale — one texture pixel = this many logical pixels.
constexpr std::uint32_t kGlyphW = 5;
constexpr std::uint32_t kGlyphH = 7;

void fillBlob(Array<std::byte>& blob, const char* text) {
    blob.resize(std::strlen(text));
    std::memcpy(blob.data(), text, blob.size());
}

// std140-compatible camera/scale block: proj (64) + vec2 glyphScale (8).
struct alignas(16) Ubo {
    math::Mat4 proj;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
};
static_assert(sizeof(Ubo) == 80, "Ubo must stay std140-sized (80 bytes)");

// One glyph quad. Binding 1 (divisor 1), 10 floats = 40 bytes.
struct OverlayInstance {
    float px = 0.0f;
    float py = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};
static_assert(sizeof(OverlayInstance) == 40, "OverlayInstance must be 40 bytes");

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec2 aQuadPos;   // unit corner, binding 0
layout(location = 1) in vec2 aPos;       // binding 1 (per instance): px, py
layout(location = 2) in vec4 aUV;        // u0 v0 u1 v1
layout(location = 3) in vec4 aColor;
layout(std140, binding = 0) uniform HudUbo { mat4 proj; vec2 glyphScale; };
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 pos = aPos + aQuadPos * glyphScale;
    gl_Position = proj * vec4(pos, 0.0, 1.0);
    vUV = mix(vec2(aUV.x, aUV.y), vec2(aUV.z, aUV.w), aQuadPos);
    vColor = aColor;
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor.rgb, vColor.a * texture(uTex, vUV).a);
}
)glsl";

} // namespace

Overlay::~Overlay() {
    if (dev_ == nullptr) {
        return;
    }
    if (pipeline_.index != 0) {
        dev_->destroyPipeline(pipeline_);
    }
    if (sampler_.index != 0) {
        dev_->destroySampler(sampler_);
    }
    if (ubo_.index != 0) {
        dev_->destroyBuffer(ubo_);
    }
    if (instBuf_.index != 0) {
        dev_->destroyBuffer(instBuf_);
    }
    if (quadIb_.index != 0) {
        dev_->destroyBuffer(quadIb_);
    }
    if (quadVb_.index != 0) {
        dev_->destroyBuffer(quadVb_);
    }
}

GfxError Overlay::init(std::uint32_t maxGlyphs, float scale) {
    if (dev_ == nullptr || maxGlyphs == 0) {
        return GfxError::BadDescriptor;
    }
    maxGlyphs_ = maxGlyphs;
    scale_ = scale;

    GfxError err = font_.init();
    if (err != GfxError::None) {
        return err;
    }

    // Unit quad for a glyph (binding 0, per-vertex).
    constexpr float kQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::uint16_t kQuadIdx[] = {0, 1, 2, 2, 1, 3};
    {
        auto r = dev_->createBuffer(
            []() {
                BufferDesc d;
                d.sizeBytes = sizeof(kQuad);
                d.usage = BufferUsage::Vertex;
                return d;
            }(),
            std::span(reinterpret_cast<const std::byte*>(kQuad), sizeof(kQuad)));
        if (!r.hasValue()) {
            return r.error();
        }
        quadVb_ = r.value();
    }
    {
        auto r = dev_->createBuffer(
            []() {
                BufferDesc d;
                d.sizeBytes = sizeof(kQuadIdx);
                d.usage = BufferUsage::Index;
                return d;
            }(),
            std::span(reinterpret_cast<const std::byte*>(kQuadIdx), sizeof(kQuadIdx)));
        if (!r.hasValue()) {
            return r.error();
        }
        quadIb_ = r.value();
    }

    // Persistent per-glyph instance buffer (no per-frame allocation).
    {
        BufferDesc d;
        d.sizeBytes = maxGlyphs_ * sizeof(OverlayInstance);
        d.usage = BufferUsage::Vertex;
        auto r = dev_->createBuffer(d, {});
        if (!r.hasValue()) {
            return r.error();
        }
        instBuf_ = r.value();
    }

    // Camera/scale UBO.
    {
        BufferDesc d;
        d.sizeBytes = sizeof(Ubo);
        d.usage = BufferUsage::Uniform;
        auto r = dev_->createBuffer(d, {});
        if (!r.hasValue()) {
            return r.error();
        }
        ubo_ = r.value();
    }

    // Clamp-to-edge linear sampler (matches the default but explicit).
    {
        auto r = dev_->createSampler(SamplerDesc{});
        if (!r.hasValue()) {
            return r.error();
        }
        sampler_ = r.value();
    }

    auto makeShader = [&](ShaderStage stage, const char* src) -> Result<ShaderHandle, GfxError> {
        ShaderDesc d;
        d.stage = stage;
        fillBlob(d.blob, src);
        return dev_->createShader(d);
    };
    auto vs = makeShader(ShaderStage::Vertex, kVs);
    if (!vs.hasValue()) {
        return vs.error();
    }
    auto fs = makeShader(ShaderStage::Fragment, kFs);
    if (!fs.hasValue()) {
        return fs.error();
    }

    PipelineStateDesc pd;
    pd.vertexShader = vs.value();
    pd.fragmentShader = fs.value();
    pd.topology = PrimitiveTopology::TriangleList;
    pd.raster.cullMode = CullMode::None;
    pd.depthStencil.depthTestEnable = false;
    pd.depthStencil.depthWriteEnable = false;

    VertexInputLayout::VertexBinding bb;
    bb.binding = 0;
    bb.strideBytes = 8;
    bb.divisor = 0;
    pd.vertexInput.bindings.push(bb);
    VertexInputLayout::VertexAttribute qa;
    qa.binding = 0;
    qa.location = 0;
    qa.format = VertexFormat::Float32x2;
    qa.offsetBytes = 0;
    pd.vertexInput.attributes.push(qa);

    VertexInputLayout::VertexBinding ib;
    ib.binding = 1;
    ib.strideBytes = sizeof(OverlayInstance);
    ib.divisor = 1;
    pd.vertexInput.bindings.push(ib);
    struct Attr {
        std::uint32_t location;
        std::uint32_t offset;
        VertexFormat format;
    };
    constexpr Attr kAttrs[] = {
        {1, 0, VertexFormat::Float32x2}, // px, py
        {2, 8, VertexFormat::Float32x4}, // u0 v0 u1 v1
        {3, 24, VertexFormat::Float32x4}, // r g b a
    };
    for (const Attr& a : kAttrs) {
        VertexInputLayout::VertexAttribute va;
        va.binding = 1;
        va.location = a.location;
        va.format = a.format;
        va.offsetBytes = a.offset;
        pd.vertexInput.attributes.push(va);
    }

    ColorAttachmentState ca;
    ca.blendEnable = true;
    ca.srcColor = BlendFactor::SrcAlpha;
    ca.dstColor = BlendFactor::OneMinusSrcAlpha;
    ca.srcAlpha = BlendFactor::One;
    ca.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    pd.colorAttachments.push(ca);

    auto pipeline = dev_->createPipeline(pd);
    if (!pipeline.hasValue()) {
        return pipeline.error();
    }
    pipeline_ = pipeline.value();
    ready_ = true;
    return GfxError::None;
}

std::uint32_t Overlay::setLines(std::span<const HudLine> lines) {
    pendingGlyphs_ = 0;
    if (!ready_ || maxGlyphs_ == 0) {
        return 0;
    }
    containers::Array<OverlayInstance> staging;
    staging.resize(maxGlyphs_);
    std::uint32_t used = 0;
    for (const HudLine& line : lines) {
        if (line.text == nullptr) {
            continue;
        }
        float penX = line.x;
        for (const char* p = line.text; *p != '\0' && used < maxGlyphs_; ++p) {
            const Glyph& g = font_.glyph(*p);
            OverlayInstance& inst = staging[used++];
            inst.px = penX;
            inst.py = line.y;
            inst.u0 = g.u0;
            inst.v0 = g.v0;
            inst.u1 = g.u1;
            inst.v1 = g.v1;
            inst.r = line.r;
            inst.g = line.g;
            inst.b = line.b;
            inst.a = line.a;
            penX += g.advance * scale_;
        }
    }
    pendingGlyphs_ = used;
    if (used != 0) {
        dev_->writeBuffer(instBuf_, 0,
                          std::span(reinterpret_cast<const std::byte*>(staging.data()),
                                    static_cast<std::size_t>(used) * sizeof(OverlayInstance)));
    }
    return used;
}

void Overlay::render(CommandBuffer& cb, std::uint32_t width, std::uint32_t height) {
    if (!ready_ || width == 0 || height == 0) {
        return;
    }
    Ubo u;
    u.proj = math::Mat4::orthoRH_ZO(0.0f, static_cast<float>(width),
                                    static_cast<float>(height), 0.0f, -1.0f, 1.0f);
    u.scaleX = static_cast<float>(kGlyphW) * scale_;
    u.scaleY = static_cast<float>(kGlyphH) * scale_;
    dev_->writeBuffer(ubo_, 0, std::span(reinterpret_cast<const std::byte*>(&u), sizeof(u)));

    cb.bindPipeline(pipeline_);
    cb.bindResource(0, BindingType::UniformBuffer, ubo_.index, ubo_.generation);
    cb.bindResource(0, BindingType::Texture, font_.texture().index, font_.texture().generation);
    cb.bindResource(0, BindingType::Sampler, sampler_.index, sampler_.generation);
    cb.bindVertexBuffer(0, quadVb_, 8);
    cb.bindVertexBuffer(1, instBuf_, sizeof(OverlayInstance));
    cb.bindIndexBuffer(quadIb_, IndexFormat::U16);
    cb.drawIndexed(6, pendingGlyphs_);
}

} // namespace f4::gfx::hud
