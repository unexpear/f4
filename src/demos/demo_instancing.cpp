// demo_instancing — hardware instancing with a divisor-1 per-instance buffer.
// Draws a grid of spinning triangles; each instance carries its own color and a
// 2D offset, so one draw call renders the whole set. Exercises the instancing
// path (per-instance attribute locations) and reports instancing metrics.
//
// Controls: Esc closes the window.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <cmath>
#include <cstring>

#include "core/diagnostics/diagnostics.h"
#include "core/math/mat4.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;
using namespace f4::gfx;

namespace {
constexpr int kGrid = 10;         // kGrid x kGrid instances
constexpr int kInstanceCount = kGrid * kGrid;
constexpr float kTriangleVerts[] = {
    0.0f, 0.08f, 0.0f,
    -0.07f, -0.06f, 0.0f,
    0.07f, -0.06f, 0.0f,
};

// Per-instance: offset (xy) + color (rgb). Binding 1, divisor 1.
struct Instance {
    float ox, oy;
    float r, g, b;
};
static_assert(sizeof(Instance) == 20);

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

int main() {
    demo::DemoWindow demo("demo_instancing");
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
    color.clear.color[2] = 0.07f;
    fbd.attachments.push(color);
    auto fbr = dev->createFramebuffer(fbd);
    if (!fbr.hasValue()) {
        return 1;
    }
    FramebufferHandle fb = fbr.value();

    auto vb = dev->createBuffer(
        []() {
            BufferDesc d;
            d.sizeBytes = sizeof(kTriangleVerts);
            d.usage = BufferUsage::Vertex;
            return d;
        }(),
        std::span(reinterpret_cast<const std::byte*>(kTriangleVerts), sizeof(kTriangleVerts)));
    if (!vb.hasValue()) {
        return 1;
    }

    BufferDesc id;
    id.sizeBytes = kInstanceCount * sizeof(Instance);
    id.usage = BufferUsage::Vertex;
    auto ib = dev->createBuffer(id, {});
    if (!ib.hasValue()) {
        return 1;
    }

    // Per-instance data: one triangle per grid cell with a hue color.
    Instance instances[kInstanceCount];
    int idx = 0;
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            float ox = (static_cast<float>(gx) / (kGrid - 1) - 0.5f) * 1.6f;
            float oy = (static_cast<float>(gy) / (kGrid - 1) - 0.5f) * 1.6f;
            float h = static_cast<float>(idx) / kInstanceCount;
            instances[idx++] = {ox, oy, 0.5f + 0.5f * std::sin(h * 6.28f),
                                0.5f + 0.5f * std::sin(h * 6.28f + 2.1f),
                                0.5f + 0.5f * std::sin(h * 6.28f + 4.2f)};
        }
    }
    dev->writeBuffer(ib.value(), 0,
                     std::span(reinterpret_cast<const std::byte*>(instances), sizeof(instances)));

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
        VertexInputLayout::VertexAttribute a;
        a.binding = 0;
        a.location = 0;
        a.format = VertexFormat::Float32x3;
        pd.vertexInput.attributes.push(a);
    }
    {
        VertexInputLayout::VertexBinding b;
        b.binding = 1;
        b.strideBytes = sizeof(Instance);
        b.divisor = 1;
        pd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute a1;
        a1.binding = 1;
        a1.location = 1;
        a1.format = VertexFormat::Float32x2;
        a1.offsetBytes = 0;
        pd.vertexInput.attributes.push(a1);
        VertexInputLayout::VertexAttribute a2;
        a2.binding = 1;
        a2.location = 2;
        a2.format = VertexFormat::Float32x3;
        a2.offsetBytes = 8;
        pd.vertexInput.attributes.push(a2);
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
    const std::uint32_t idInst = diagnostics.metrics().declare({"instances", diag::MetricType::UInt, "gpu", "count"});
    const std::uint32_t idBatch = diagnostics.metrics().declare({"batches", diag::MetricType::UInt, "gpu", "count"});

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
        diagnostics.metrics()[idInst].set(static_cast<std::uint64_t>(kInstanceCount));
        diagnostics.metrics()[idBatch].set(dev->stats().batches);
        diagnostics.update(t);

        {
            CommandBuffer cb;
            cb.begin();
            cb.beginRenderPass(fb);
            cb.setViewport({width, height});
            cb.bindPipeline(pipeline.value());
            cb.bindVertexBuffer(0, vb.value(), 12);
            cb.bindVertexBuffer(1, ib.value(), sizeof(Instance));
            cb.draw(3, kInstanceCount);
            hud.render(cb, width, height);
            cb.endRenderPass();
            cb.end();
            dev->submit(cb);
        }
        dev->resetFrameStats();
        window->swap();
    }

    dev->waitIdle();
    return 0;
}
