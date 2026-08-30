// demo_hud — the diagnostic-overlay showcase. It deliberately exercises every
// metric category the checklist calls for (position history, velocity, object
// count/sizes, FPS, draw-calls, memory, input, primitives/frame, shader compile
// errors/warnings, per-frame allocation counts, CPU timing) and renders them all
// through the HUD overlay. The CPU profiler and heap tracker are enabled so
// their categories have real data.
//
// Controls: move the mouse (input metric) or press Esc to close.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/diagnostics/cpu_profiler.h"
#include "core/diagnostics/diagnostics.h"
#include "core/math/mat4.h"
#include "core/memory/memory.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;
using namespace f4::gfx;
using namespace f4::memory;

namespace {
constexpr float kQuad[] = {
    -0.35f, -0.35f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.35f, -0.35f, 0.0f, 0.0f, 1.0f, 0.0f,
    -0.35f, 0.35f, 0.0f, 0.0f, 0.0f, 1.0f,
    0.35f, 0.35f, 0.0f, 1.0f, 1.0f, 1.0f,
};
constexpr std::uint16_t kIndices[] = {0, 1, 2, 2, 1, 3};
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

// The checklist, in metric-registration order. Shader warnings/errors are read
// from the RHI stats; the procedural quad has none, so we report the real (0)
// value - the metric still proves the category ends up in the overlay.
enum {
    ID_FPS,
    ID_FRAME_MS,
    ID_DRAW,
    ID_TRIS,
    ID_SHADER_WARN,
    ID_SHADER_ERR,
    ID_LIVE_BYTES,
    ID_ALLOCS,
    ID_OBJ_COUNT,
    ID_OBJ_SIZES,
    ID_POSITIONS,
    ID_VELOCITIES,
    ID_MOUSE_DX,
    ID_KEYS,
    ID_CPU_US,
    ID_COUNT,
};

int main() {
    demo::DemoWindow demo("demo_hud");
    if (!demo.ok()) {
        return 1;
    }
    platform::Window* window = demo.window();
    IDevice* dev = demo.device();
    const std::uint32_t width = demo::DemoWindow::kWidth;
    const std::uint32_t height = demo::DemoWindow::kHeight;

    // Enable heap tracking so per-frame allocation metrics are real.
    enableHeapTracking(true);

    FramebufferDesc fbd;
    fbd.extent = {width, height};
    AttachmentDesc color;
    color.type = AttachmentType::Color;
    color.clearOnLoad = true;
    color.clear.color[2] = 0.05f;
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
    BufferDesc ubd;
    ubd.sizeBytes = sizeof(math::Mat4);
    ubd.usage = BufferUsage::Uniform;
    auto ubo = dev->createBuffer(ubd, {});

    auto makeShader = [&](ShaderStage stage, const char* src) {
        ShaderDesc d;
        d.stage = stage;
        d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size());
        return dev->createShader(d);
    };
    auto vs = makeShader(ShaderStage::Vertex, kVs);
    auto fs = makeShader(ShaderStage::Fragment, kFs);
    if (!vb.hasValue() || !ib.hasValue() || !ubo.hasValue() || !vs.hasValue() || !fs.hasValue()) {
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
        b.strideBytes = 24; // pos + color
        pd.vertexInput.bindings.push(b);
        VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        VertexInputLayout::VertexAttribute ca;
        ca.binding = 0;
        ca.location = 1;
        ca.format = VertexFormat::Float32x3;
        ca.offsetBytes = 12;
        pd.vertexInput.attributes.push(ca);
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

    auto& m = diagnostics.metrics();
    std::uint32_t ids[ID_COUNT];
    const diag::MetricDecl decls[ID_COUNT] = {
        {"fps", diag::MetricType::Double, "frame", "1/s"},
        {"frame_ms", diag::MetricType::Double, "frame", "ms"},
        {"draw_calls", diag::MetricType::UInt, "gpu", "calls"},
        {"triangles", diag::MetricType::UInt, "gpu", "count"},
        {"shader_warnings", diag::MetricType::UInt, "gpu", "count"},
        {"shader_errors", diag::MetricType::UInt, "gpu", "count"},
        {"live_bytes", diag::MetricType::UInt, "memory", "B"},
        {"allocs/frame", diag::MetricType::UInt, "memory", "count"},
        {"object_count", diag::MetricType::UInt, "objects", "count"},
        {"object_sizes", diag::MetricType::UInt, "objects", "B"},
        {"positions", diag::MetricType::String, "telemetry", nullptr},
        {"velocities", diag::MetricType::String, "telemetry", nullptr},
        {"mouse_dx", diag::MetricType::Int, "input", "px"},
        {"keys", diag::MetricType::UInt, "input", "count"},
        {"cpu_zones_us", diag::MetricType::UInt, "profiler", "us"},
    };
    for (int i = 0; i < ID_COUNT; ++i) {
        ids[i] = m.declare(decls[i]);
    }

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 prev = SDL_GetPerformanceCounter();
    double t = 0.0;
    float angle = 0.0f;
    std::int64_t mouseDx = 0;
    std::uint64_t keyCount = 0;
    bool running = true;

    while (running) {
        diagnostics.profiler().beginFrame();
        heapTracker().beginFrame();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                keyCount++;
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                mouseDx += event.motion.xrel;
            }
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - prev) / static_cast<double>(perfFreq);
        prev = now;
        t += dt;
        angle += static_cast<float>(dt) * 1.4f;

        // What a real simulator would own: a couple of mock bodies.
        const float px = std::cos(t) * 0.4f;
        const float py = std::sin(t * 1.3f) * 0.4f;
        const float vx = -std::sin(t) * 0.4f;
        const float vy = std::cos(t * 1.3f) * 0.4f * 1.3f;

        {
            diag::ScopedZone zone(diagnostics.profiler(), "frame");
            // Record this frame's metrics (sampled only when the interval elapses).
            m[ids[ID_FPS]].set(dt > 0.0 ? 1.0 / dt : 0.0);
            m[ids[ID_FRAME_MS]].set(dt * 1000.0);
            m[ids[ID_DRAW]].set(dev->stats().drawCalls + dev->stats().drawIndexedCalls);
            m[ids[ID_TRIS]].set(dev->stats().primitives);
            m[ids[ID_SHADER_WARN]].set(dev->stats().shaderWarnings);
            m[ids[ID_SHADER_ERR]].set(dev->stats().shaderFailed);
            m[ids[ID_LIVE_BYTES]].set(dev->stats().bufferBytes + dev->stats().textureBytes);
            m[ids[ID_OBJ_COUNT]].set(static_cast<std::uint64_t>(2));
            m[ids[ID_OBJ_SIZES]].set(static_cast<std::uint64_t>(2 * 64)); // two 64B bodies
            m[ids[ID_MOUSE_DX]].set(mouseDx);
            m[ids[ID_KEYS]].set(keyCount);

            char posBuf[64];
            std::snprintf(posBuf, sizeof(posBuf), "(%.2f, %.2f)", static_cast<double>(px), static_cast<double>(py));
            m[ids[ID_POSITIONS]].set(posBuf);
            char velBuf[64];
            std::snprintf(velBuf, sizeof(velBuf), "(%.2f, %.2f)", static_cast<double>(vx), static_cast<double>(vy));
            m[ids[ID_VELOCITIES]].set(velBuf);
        }

        // Per-frame CPU time from the profiler's own zones.
        std::uint64_t cpuUs = 0;
        for (const auto& z : diagnostics.profiler().zones()) {
            cpuUs += z.totalNanos / 1000u;
        }
        m[ids[ID_CPU_US]].set(cpuUs);

        heapTracker().endFrame();
        m[ids[ID_ALLOCS]].set(heapTracker().frame().allocations);

        diagnostics.update(t);
        diagnostics.profiler().endFrame();

        const math::Mat4 mvp = math::Mat4::rotationZ(angle);
        dev->writeBuffer(ubo.value(), 0, std::span(reinterpret_cast<const std::byte*>(&mvp), sizeof(mvp)));

        {
            CommandBuffer cb;
            cb.begin();
            cb.beginRenderPass(fb);
            cb.setViewport({width, height});
            cb.bindPipeline(pipeline.value());
            cb.bindResource(0, ubo.value().index, ubo.value().generation);
            cb.bindVertexBuffer(0, vb.value(), 24);
            cb.bindIndexBuffer(ib.value(), IndexFormat::U16);
            cb.drawIndexed(6);
            hud.render(cb, width, height);
            cb.endRenderPass();
            cb.end();
            dev->submit(cb);
        }
        dev->resetFrameStats();
        window->swap();
    }

    enableHeapTracking(false);
    dev->waitIdle();
    return 0;
}
