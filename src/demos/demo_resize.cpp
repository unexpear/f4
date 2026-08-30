// demo_resize — a resizable window whose framebuffer, viewport and aspect
// ratio all track the live drawable size. A perspective triangle shows the
// aspect tracking; the HUD reports the current pixel size so resize is visible
// in the metrics too.
//
// Controls: resize the window, or press Esc to close.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <array>
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
using namespace f4::math;

namespace {
constexpr float kTri[] = {
    0.0f, 0.0f, 0.0f, 1.0f, 0.3f, 0.3f,
    1.0f, 0.0f, 0.0f, 0.3f, 1.0f, 0.3f,
    0.0f, 1.0f, 0.0f, 0.3f, 0.3f, 1.0f,
};
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

int main() {
    // Note: resizable window, larger initial size.
    demo::DemoWindow demo("demo_resize", 1000, 640, true);
    if (!demo.ok()) {
        return 1;
    }
    platform::Window* window = demo.window();
    IDevice* dev = demo.device();

    auto vb = dev->createBuffer(
        []() {
            BufferDesc d;
            d.sizeBytes = sizeof(kTri);
            d.usage = BufferUsage::Vertex;
            return d;
        }(),
        std::span(reinterpret_cast<const std::byte*>(kTri), sizeof(kTri)));
    if (!vb.hasValue()) {
        return 1;
    }

    BufferDesc ubd;
    ubd.sizeBytes = sizeof(Mat4);
    ubd.usage = BufferUsage::Uniform;
    auto ubo = dev->createBuffer(ubd, {});
    if (!ubo.hasValue()) {
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
        b.strideBytes = 24; // pos(12) + color(12)
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
    const std::uint32_t idFps = diagnostics.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    const std::uint32_t idW = diagnostics.metrics().declare({"width", diag::MetricType::UInt, "swapchain", "px"});
    const std::uint32_t idH = diagnostics.metrics().declare({"height", diag::MetricType::UInt, "swapchain", "px"});

    FramebufferHandle fb{};
    std::uint32_t fbW = 0;
    std::uint32_t fbH = 0;
    Mat4 proj = Mat4::identity();
    auto rebuild = [&]() -> bool {
        const std::uint32_t w = window->width();
        const std::uint32_t h = window->height();
        if (w == 0 || h == 0) {
            return false;
        }
        if (fb.index != 0) {
            dev->destroyFramebuffer(fb);
        }
        FramebufferDesc fbd;
        fbd.extent = {w, h};
        AttachmentDesc color;
        color.type = AttachmentType::Color;
        color.clearOnLoad = true;
        color.clear.color[2] = 0.06f;
        fbd.attachments.push(color);
        auto r = dev->createFramebuffer(fbd);
        if (!r.hasValue()) {
            return false;
        }
        fb = r.value();
        fbW = w;
        fbH = h;
        proj = Mat4::perspectiveRH_ZO(0.9f, static_cast<float>(w) / static_cast<float>(h), 0.1f, 10.0f);
        return true;
    };
    if (!rebuild()) {
        return 1;
    }

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 prev = SDL_GetPerformanceCounter();
    double t = 0.0;
    bool running = true;

    // Spin angle.
    float angle = 0.0f;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT &&
                       (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                if (!rebuild()) {
                    return 1;
                }
            }
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - prev) / static_cast<double>(perfFreq);
        prev = now;
        t += dt;
        angle += static_cast<float>(dt) * 1.2f;

        diagnostics.metrics()[idFps].set(dt > 0.0 ? 1.0 / dt : 0.0);
        diagnostics.metrics()[idW].set(static_cast<std::uint64_t>(fbW));
        diagnostics.metrics()[idH].set(static_cast<std::uint64_t>(fbH));
        diagnostics.update(t);

        const Mat4 mvp = proj.mul(Mat4::rotationZ(angle)).mul(Mat4::translation({0.0f, 0.0f, -3.0f}));
        dev->writeBuffer(ubo.value(), 0, std::span(reinterpret_cast<const std::byte*>(&mvp), sizeof(mvp)));

        {
            CommandBuffer cb;
            cb.begin();
            cb.beginRenderPass(fb);
            cb.setViewport({fbW, fbH});
            cb.bindPipeline(pipeline.value());
            cb.bindResource(0, ubo.value().index, ubo.value().generation);
            cb.bindVertexBuffer(0, vb.value(), 24);
            cb.draw(3);
            hud.render(cb, fbW, fbH);
            cb.endRenderPass();
            cb.end();
            dev->submit(cb);
        }
        window->swap();
    }

    dev->waitIdle();
    return 0;
}
