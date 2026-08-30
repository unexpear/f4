// demo_triangle — the smallest focused demo: one colored triangle on the
// backbuffer, with the diagnostics console + HUD overlay running to show the
// metric registry in action. Everything else (window/device, overlay) is pulled
// from the shared demo scaffolding so this file is purely about the triangle.
//
// Controls: Esc closes the window.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
#include <cstring>

#include "core/diagnostics/diagnostics.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/device/device.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;
using namespace f4::gfx;

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

int main() {
    demo::DemoWindow demo("demo_triangle");
    if (!demo.ok()) {
        return 1;
    }
    platform::Window* window = demo.window();
    IDevice* dev = demo.device();

    // Framebuffer over the backbuffer (color attachment only, dark clear).
    FramebufferHandle fb{};
    FramebufferDesc fbd;
    fbd.extent = {demo::DemoWindow::kWidth, demo::DemoWindow::kHeight};
    AttachmentDesc color;
    color.type = AttachmentType::Color;
    color.clearOnLoad = true;
    color.clear.color[0] = 0.05f;
    color.clear.color[1] = 0.05f;
    color.clear.color[2] = 0.07f;
    fbd.attachments.push(color);
    auto fbRes = dev->createFramebuffer(fbd);
    if (!fbRes.hasValue()) {
        return 1;
    }
    fb = fbRes.value();

    // Triangle buffers (separate per-vertex position and color streams).
    BufferDesc vbd;
    vbd.sizeBytes = sizeof(kPositions);
    vbd.usage = BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kPositions), sizeof(kPositions)));
    if (!vb.hasValue()) {
        return 1;
    }
    BufferDesc cbd;
    cbd.sizeBytes = sizeof(kColors);
    cbd.usage = BufferUsage::Vertex;
    auto cb = dev->createBuffer(cbd, std::span(reinterpret_cast<const std::byte*>(kColors), sizeof(kColors)));
    if (!cb.hasValue()) {
        return 1;
    }

    // Pipeline.
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
        b.strideBytes = 12;
        pd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
    }
    {
        VertexInputLayout::VertexBinding b;
        b.binding = 1;
        b.strideBytes = 12;
        pd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute ca;
        ca.binding = 1;
        ca.location = 1;
        ca.format = VertexFormat::Float32x3;
        ca.offsetBytes = 0;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipeline = dev->createPipeline(pd);
    if (!pipeline.hasValue()) {
        return 1;
    }

    // Diagnostics + HUD.
    diag::Diagnostics diagnostics;
    demo::Hud hud(dev);
    if (!hud.init()) {
        return 1;
    }
    diagnostics.setSampleSink(&demo::Hud::onSample, &hud);

    const std::uint32_t idFps = diagnostics.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    const std::uint32_t idTick = diagnostics.metrics().declare({"frame_ms", diag::MetricType::Double, "frame", "ms"});
    const std::uint32_t idDraw = diagnostics.metrics().declare({"draw_calls", diag::MetricType::UInt, "gpu", "calls"});
    const std::uint32_t idTri = diagnostics.metrics().declare({"triangles", diag::MetricType::UInt, "gpu", "count"});
    const std::uint32_t idMem = diagnostics.metrics().declare({"live_bytes", diag::MetricType::UInt, "memory", "B"});

    // Simple frame clock (high-res enough for the HUD).
    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 prev = SDL_GetPerformanceCounter();
    double firstNow = 0.0;
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
        if (dt <= 0.0) {
            continue;
        }
        const double cur = firstNow + dt;
        firstNow = cur;

        // Push metrics, then sample (updates console + HUD when due).
        const double fps = dt > 0.0 ? 1.0 / dt : 0.0;
        diagnostics.metrics()[idFps].set(fps);
        diagnostics.metrics()[idTick].set(dt * 1000.0);
        diagnostics.metrics()[idDraw].set(dev->stats().drawCalls);
        diagnostics.metrics()[idTri].set(dev->stats().drawCalls * 1u);
        diagnostics.metrics()[idMem].set(dev->stats().bufferBytes + dev->stats().textureBytes);
        diagnostics.update(cur);

        {
            CommandBuffer cbcb;
            cbcb.begin();
            cbcb.beginRenderPass(fb);
            cbcb.setViewport({demo::DemoWindow::kWidth, demo::DemoWindow::kHeight});
            cbcb.bindPipeline(pipeline.value());
            cbcb.bindVertexBuffer(0, vb.value(), 12);
            cbcb.bindVertexBuffer(1, cb.value(), 12);
            cbcb.draw(3);

            // HUD overlay pass on top.
            hud.render(cbcb, demo::DemoWindow::kWidth, demo::DemoWindow::kHeight);
            cbcb.endRenderPass();
            cbcb.end();
            dev->submit(cbcb);
        }

        // Per-frame counters are reset so the next sample sees this frame only.
        dev->resetFrameStats();
        window->swap();
    }

    dev->waitIdle();
    return 0;
}
