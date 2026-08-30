// demo_assets — exercises the RHI texture path end-to-end: a procedural RGBA
// asset is generated on the CPU, uploaded with IDevice::writeTexture, and drawn
// as a textured quad through the new Texture/Sampler resource binding (the
// fragment shader samples `uTex` at slot 0). Reports texture footprint metrics
// so the asset accounting is visible. The same code path loads any decoded
// image (e.g. the KTX/PNG loaders) — this keeps the demo file-loader-free.
//
// Controls: Esc closes the window.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/diagnostics/diagnostics.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;
using namespace f4::gfx;

namespace {
constexpr std::uint32_t kTexW = 128;
constexpr std::uint32_t kTexH = 128;

// Fullscreen textured quad (pos + uv), interleaved, 20-byte stride.
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

int main() {
    demo::DemoWindow demo("demo_assets");
    if (!demo.ok()) {
        return 1;
    }
    platform::Window* window = demo.window();
    IDevice* dev = demo.device();
    const std::uint32_t width = demo::DemoWindow::kWidth;
    const std::uint32_t height = demo::DemoWindow::kHeight;

    FramebufferDesc fbd;
    fbd.extent = {width, height};
    AttachmentDesc color;
    color.type = AttachmentType::Color;
    color.clearOnLoad = true;
    color.clear.color[2] = 0.06f;
    fbd.attachments.push(color);
    auto fbr = dev->createFramebuffer(fbd);
    if (!fbr.hasValue()) {
        return 1;
    }
    FramebufferHandle fb = fbr.value();

    auto vb = dev->createBuffer(
        []() {
            BufferDesc d;
            d.sizeBytes = sizeof(kQuad);
            d.usage = BufferUsage::Vertex;
            return d;
        }(),
        std::span(reinterpret_cast<const std::byte*>(kQuad), sizeof(kQuad)));
    auto ib = dev->createBuffer(
        []() {
            BufferDesc d;
            d.sizeBytes = sizeof(kIndices);
            d.usage = BufferUsage::Index;
            return d;
        }(),
        std::span(reinterpret_cast<const std::byte*>(kIndices), sizeof(kIndices)));

    // Procedural RGBA asset (smooth color field with a brighter ring).
    std::vector<std::byte> texPixels(kTexW * kTexH * 4);
    for (std::uint32_t y = 0; y < kTexH; ++y) {
        for (std::uint32_t x = 0; x < kTexW; ++x) {
            const float u = static_cast<float>(x) / kTexW;
            const float v = static_cast<float>(y) / kTexH;
            const float cx = u - 0.5f;
            const float cy = v - 0.5f;
            const float ring = std::max(0.0f, 1.0f - std::abs(std::sqrt(cx * cx + cy * cy) * 2.0f - 0.7f) * 6.0f);
            std::size_t i = (static_cast<std::size_t>(y) * kTexW + x) * 4;
            texPixels[i + 0] = std::byte(static_cast<unsigned char>(u * 255.0f + ring * 120.0f));
            texPixels[i + 1] = std::byte(static_cast<unsigned char>(v * 255.0f));
            texPixels[i + 2] = std::byte(static_cast<unsigned char>(ring * 255.0f));
            texPixels[i + 3] = std::byte(0xFF);
        }
    }
    TextureDesc td;
    td.width = kTexW;
    td.height = kTexH;
    td.format = TextureFormat::RGBA8_UNORM;
    auto tex = dev->createTexture(td);
    auto samp = dev->createSampler(SamplerDesc{});
    if (!vb.hasValue() || !ib.hasValue() || !tex.hasValue() || !samp.hasValue()) {
        return 1;
    }
    if (dev->writeTexture(tex.value(), 0, std::span(texPixels.data(), texPixels.size())) != GfxError::None) {
        return 1;
    }

    auto makeShader = [&](ShaderStage stage, const char* src) {
        ShaderDesc d;
        d.stage = stage;
        d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size());
        return dev->createShader(d);
    };
    auto vs = makeShader(ShaderStage::Vertex, kVs);
    auto fs = makeShader(ShaderStage::Fragment, kFs);
    if (!vs.hasValue() || !fs.hasValue()) {
        return 1;
    }
    PipelineStateDesc pd;
    pd.vertexShader = vs.value();
    pd.fragmentShader = fs.value();
    pd.topology = PrimitiveTopology::TriangleList;
    pd.raster.cullMode = CullMode::None;
    {
        VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 20;
        pd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        VertexInputLayout::VertexAttribute ua;
        ua.binding = 0;
        ua.location = 1;
        ua.format = VertexFormat::Float32x2;
        ua.offsetBytes = 12;
        pd.vertexInput.attributes.push(ua);
    }
    auto pipeline = dev->createPipeline(pd);
    if (!pipeline.hasValue()) {
        return 1;
    }

    diag::Diagnostics diagnostics;
    demo::Hud hud(dev);
    if (!hud.init()) {
        return 1;
    }
    diagnostics.setSampleSink(&demo::Hud::onSample, &hud);
    const std::uint32_t idFps = diagnostics.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    const std::uint32_t idBytes = diagnostics.metrics().declare({"texture_bytes", diag::MetricType::UInt, "assets", "B"});
    const std::uint32_t idCount = diagnostics.metrics().declare({"textures", diag::MetricType::UInt, "assets", "count"});

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 prev = SDL_GetPerformanceCounter();
    double t = 0.0;
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - prev) / static_cast<double>(perfFreq);
        prev = now;
        t += dt;

        diagnostics.metrics()[idFps].set(dt > 0.0 ? 1.0 / dt : 0.0);
        diagnostics.metrics()[idBytes].set(dev->stats().textureBytes);
        diagnostics.metrics()[idCount].set(dev->stats().textureCount);
        diagnostics.update(t);

        {
            CommandBuffer cb;
            cb.begin();
            cb.beginRenderPass(fb);
            cb.setViewport({width, height});
            cb.bindPipeline(pipeline.value());
            cb.bindResource(0, BindingType::Texture, tex.value().index, tex.value().generation);
            cb.bindResource(0, BindingType::Sampler, samp.value().index, samp.value().generation);
            cb.bindVertexBuffer(0, vb.value(), 20);
            cb.bindIndexBuffer(ib.value(), IndexFormat::U16);
            cb.drawIndexed(6);
            hud.render(cb, width, height);
            cb.endRenderPass();
            cb.end();
            dev->submit(cb);
        }
        window->swap();
    }

    dev->waitIdle();
    return 0;
}
