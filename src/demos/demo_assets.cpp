#include "demos/demo_assets.h"

#include <cstring>
#include <vector>

#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/texture/texture_desc.h"

namespace f4::demo {

namespace {

constexpr float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    -1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
};
constexpr std::uint16_t kIndices[] = {0, 1, 2, 2, 1, 3};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() { gl_Position = vec4(aPos, 1.0); vUV = aUV; }
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main() { fragColor = texture(uTex, vUV); }
)glsl";

} // namespace

bool AssetsScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Quad vertex buffer (pos+uv, stride 20)
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

    // Procedural texture (checkerboard)
    std::vector<std::uint8_t> pixels(kTexW * kTexH * 4);
    for (std::uint32_t y = 0; y < kTexH; ++y) {
        for (std::uint32_t x = 0; x < kTexW; ++x) {
            bool check = ((x / 16) + (y / 16)) % 2 == 0;
            std::uint8_t v = check ? 200 : 50;
            std::size_t idx = (y * kTexW + x) * 4;
            pixels[idx + 0] = v;
            pixels[idx + 1] = v / 2;
            pixels[idx + 2] = v / 3;
            pixels[idx + 3] = 255;
        }
    }
    gfx::TextureDesc td;
    td.width = kTexW;
    td.height = kTexH;
    td.format = gfx::TextureFormat::RGBA8_UNORM;
    td.mipLevels = 1;
    auto texRes = dev->createTexture(td);
    if (!texRes.hasValue()) return false;
    tex_ = texRes.value();
    if (auto err = dev->writeTexture(tex_, 0, std::span(reinterpret_cast<const std::byte*>(pixels.data()), pixels.size()));
        err != gfx::GfxError::None) return false;

    // Sampler
    auto sampRes = dev->createSampler(gfx::SamplerDesc{});
    if (!sampRes.hasValue()) return false;
    samp_ = sampRes.value();

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
        b.strideBytes = 20;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = gfx::VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ua;
        ua.binding = 0;
        ua.location = 1;
        ua.format = gfx::VertexFormat::Float32x2;
        ua.offsetBytes = 12;
        pd.vertexInput.attributes.push(ua);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    inited_ = true;
    return true;
}

void AssetsScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (samp_.isValid()) dev->destroySampler(samp_);
    if (tex_.isValid()) dev->destroyTexture(tex_);
    if (ibQuad_.isValid()) dev->destroyBuffer(ibQuad_);
    if (vbQuad_.isValid()) dev->destroyBuffer(vbQuad_);
    inited_ = false;
}

void AssetsScene::onResize(std::uint32_t, std::uint32_t) {}

void AssetsScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void AssetsScene::render(gfx::CommandBuffer& cb, const FreeCamera&, std::uint32_t, std::uint32_t) {
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vbQuad_, 20);
    cb.bindIndexBuffer(ibQuad_, gfx::IndexFormat::U16);
    cb.bindResource(0, gfx::BindingType::Texture, tex_.index, tex_.generation);
    cb.bindResource(0, gfx::BindingType::Sampler, samp_.index, samp_.generation);
    cb.drawIndexed(6, 1);
}

} // namespace f4::demo