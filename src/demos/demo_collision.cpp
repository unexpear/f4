// demo_collision — simple 2D collision in NDC: colored squares bounce off the
// screen edges and off one another. Demonstrates simulation-owned state
// (position/velocity is owned by this demo's physics, not the registry) with
// diagnostics observing via telemetry metrics, plus a couple of String metrics
// to show the registry's generic type coverage.
//
// Controls: Esc closes the window.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/diagnostics/diagnostics.h"
#include "demos/demo_support.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/framebuffer/framebuffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;
using namespace f4::gfx;

namespace {
constexpr int kBodies = 8;
constexpr float kQuad[] = {
    -0.5f, -0.5f, 0.0f,
    0.5f, -0.5f, 0.0f,
    -0.5f, 0.5f, 0.0f,
    0.5f, 0.5f, 0.0f,
};
constexpr std::uint16_t kIndices[] = {0, 1, 2, 2, 1, 3};

struct Instance {
    float x, y, size; // center + half-extent
    float r, g, b;    // color
};
static_assert(sizeof(Instance) == 24);

struct Body {
    float x, y, vx, vy;
    float r, g, b;
    float size;
};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;      // unit quad, -0.5..0.5
layout(location = 1) in vec3 aCenter;   // center (xy) + size (z)
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

int main() {
    demo::DemoWindow demo("demo_collision");
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
    BufferDesc instDesc;
    instDesc.sizeBytes = kBodies * sizeof(Instance);
    instDesc.usage = BufferUsage::Vertex;
    auto inst = dev->createBuffer(instDesc, {});

    auto makeShader = [&](ShaderStage stage, const char* src) {
        ShaderDesc d;
        d.stage = stage;
        d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size());
        return dev->createShader(d);
    };
    auto vs = makeShader(ShaderStage::Vertex, kVs);
    auto fs = makeShader(ShaderStage::Fragment, kFs);
    if (!vb.hasValue() || !ib.hasValue() || !inst.hasValue() || !vs.hasValue() || !fs.hasValue()) {
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
        b.strideBytes = 12; // pos only (colors are per-instance)
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
        a1.format = VertexFormat::Float32x3;
        a1.offsetBytes = 0;
        pd.vertexInput.attributes.push(a1);
        VertexInputLayout::VertexAttribute a2;
        a2.binding = 1;
        a2.location = 2;
        a2.format = VertexFormat::Float32x3;
        a2.offsetBytes = 12;
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
    const std::uint32_t idBodies = diagnostics.metrics().declare({"bodies", diag::MetricType::UInt, "telemetry", "count"});
    const std::uint32_t idBounces = diagnostics.metrics().declare({"bounces", diag::MetricType::UInt, "telemetry", "count"});
    const std::uint32_t idSamplePos = diagnostics.metrics().declare({"sample_positions", diag::MetricType::String, "telemetry", nullptr});

    // Physics state lives here (simulation-owned); metrics only observe it.
    std::array<Body, kBodies> bodies{};
    for (int i = 0; i < kBodies; ++i) {
        const float a = static_cast<float>(i) * 0.9f;
        bodies[i] = {std::cos(a) * 0.4f, std::sin(a) * 0.4f,
                     std::cos(a * 1.7f) * 0.4f, std::sin(a * 2.3f) * 0.4f,
                     0.5f + 0.5f * std::sin(a),
                     0.5f + 0.5f * std::sin(a + 2.1f),
                     0.5f + 0.5f * std::sin(a + 4.2f), 0.10f + 0.05f * (i % 3)};
    }
    std::uint32_t bounces = 0;

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
        const float h = static_cast<float>(dt);
        if (h <= 0.0f) {
            continue;
        }

        // Step physics (a handful of substeps for stability at low FPS).
        for (int s = 0; s < 4; ++s) {
            const float dts = h / 4.0f;
            for (Body& b : bodies) {
                b.x += b.vx * dts;
                b.y += b.vy * dts;
                const float bound = 1.0f - b.size;
                if (b.x < -bound) { b.x = -bound; b.vx = std::abs(b.vx); ++bounces; }
                if (b.x > bound) { b.x = bound; b.vx = -std::abs(b.vx); ++bounces; }
                if (b.y < -bound) { b.y = -bound; b.vy = std::abs(b.vy); ++bounces; }
                if (b.y > bound) { b.y = bound; b.vy = -std::abs(b.vy); ++bounces; }
            }
            for (int i = 0; i < kBodies; ++i) {
                for (int j = i + 1; j < kBodies; ++j) {
                    Body& a = bodies[i];
                    Body& b = bodies[j];
                    const float dx = b.x - a.x;
                    const float dy = b.y - a.y;
                    const float minD = a.size + b.size;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < minD * minD) {
                        const float d = std::sqrt(d2);
                        float nx = d > 0.0f ? dx / d : 1.0f;
                        float ny = d > 0.0f ? dy / d : 0.0f;
                        const float push = (minD - d) * 0.5f;
                        a.x -= nx * push; a.y -= ny * push;
                        b.x += nx * push; b.y += ny * push;
                        const float rel = 2.0f * ((a.vx - b.vx) * nx + (a.vy - b.vy) * ny) * 0.5f;
                        a.vx -= rel * nx; a.vy -= rel * ny;
                        b.vx += rel * nx; b.vy += rel * ny;
                        ++bounces;
                    }
                }
            }
        }

        // Telemetry.
        diagnostics.metrics()[idFps].set(dt > 0.0 ? 1.0 / dt : 0.0);
        diagnostics.metrics()[idBodies].set(static_cast<std::uint64_t>(kBodies));
        diagnostics.metrics()[idBounces].set(static_cast<std::uint64_t>(bounces));
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f) (%.2f, %.2f)",
                          static_cast<double>(bodies[0].x), static_cast<double>(bodies[0].y),
                          static_cast<double>(bodies[1].x), static_cast<double>(bodies[1].y));
            diagnostics.metrics()[idSamplePos].set(buf);
        }
        diagnostics.update(t);

        // Upload per-instance transforms.
        Instance instances[kBodies];
        for (int i = 0; i < kBodies; ++i) {
            instances[i] = {bodies[i].x, bodies[i].y, bodies[i].size, bodies[i].r, bodies[i].g,
                            bodies[i].b};
        }
        dev->writeBuffer(inst.value(), 0,
                         std::span(reinterpret_cast<const std::byte*>(instances), sizeof(instances)));

        {
            CommandBuffer cb;
            cb.begin();
            cb.beginRenderPass(fb);
            cb.setViewport({width, height});
            cb.bindPipeline(pipeline.value());
            cb.bindVertexBuffer(0, vb.value(), 12);
            cb.bindVertexBuffer(1, inst.value(), sizeof(Instance));
            cb.bindIndexBuffer(ib.value(), IndexFormat::U16);
            cb.drawIndexed(6, kBodies);
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
