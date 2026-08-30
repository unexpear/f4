// demo_ecs — ECS-driven renderer proof: Registry + Camera (orbit) +
// MeshRenderer + WorldMatrix -> Renderer -> RenderGraph -> IDevice.
// Proves MeshRenderer references logical handles, renderer owns GPU.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <cmath>
#include <cstring>

#include "core/diagnostics/diagnostics.h"
#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/memory/memory.h"
#include "core/resources/mesh.h"
#include "demos/demo_support.h"
#include "engine/cameras/camera.h"
#include "engine/rendering/renderer.h"
#include "graphics/framebuffer/framebuffer_desc.h"

using namespace f4;

static resources::Mesh makeTriangleMesh() {
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = 36;
    struct V { float px, py, pz, nx, ny, nz, u, v; std::uint8_t r, g, b, a; };
    const V verts[3] = {
        {-0.5f, -0.5f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 255, 128, 128, 255},
        {0.5f, -0.5f, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 128, 255, 128, 255},
        {0.f, 0.5f, 0.f, 0.f, 0.f, 1.f, 0.5f, 1.f, 128, 128, 255, 255},
    };
    stream.data.resize(sizeof(verts));
    std::memcpy(stream.data.data(), verts, sizeof(verts));
    mesh.streams.push(std::move(stream));
    mesh.attributes.push({resources::VertexSemantic::Position, resources::VertexFormat::F32_3, 0, 0});
    mesh.attributes.push({resources::VertexSemantic::Normal, resources::VertexFormat::F32_3, 0, 12});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0, resources::VertexFormat::F32_2, 0, 24});
    mesh.attributes.push({resources::VertexSemantic::Color0, resources::VertexFormat::UNORM8_4, 0, 32});
    mesh.indexFormat = resources::IndexFormat::U16;
    const std::uint16_t idx[3] = {0, 1, 2};
    mesh.indices.resize(sizeof(idx));
    std::memcpy(mesh.indices.data(), idx, sizeof(idx));
    mesh.submeshes.push({0, 3});
    return mesh;
}

int main() {
    demo::DemoWindow demo("demo_ecs");
    if (!demo.ok()) return 1;
    platform::Window* window = demo.window();
    gfx::IDevice* dev = demo.device();
    const std::uint32_t width = window->width();
    const std::uint32_t height = window->height();

    gfx::FramebufferDesc fbd;
    fbd.extent = {width, height};
    gfx::AttachmentDesc color;
    color.type = gfx::AttachmentType::Color;
    color.clearOnLoad = true;
    color.clear.color[0] = 0.06f; color.clear.color[1] = 0.06f; color.clear.color[2] = 0.08f; color.clear.color[3] = 1.f;
    fbd.attachments.push(color);
    gfx::AttachmentDesc depth;
    depth.type = gfx::AttachmentType::Depth;
    depth.clearOnLoad = true;
    depth.clear.depth = 1.f;
    fbd.attachments.push(depth);
    auto fbr = dev->createFramebuffer(fbd);
    if (!fbr.hasValue()) return 1;
    gfx::FramebufferHandle fb = fbr.value();

    ecs::Registry registry(64);
    engine::Renderer renderer(registry, *dev);
    if (renderer.init() != gfx::GfxError::None) return 1;

    auto triMesh = makeTriangleMesh();
    auto meshH = renderer.createMesh(triMesh);
    if (!meshH.isValid()) return 1;
    engine::StandardMaterialDesc matDesc;
    matDesc.tint = math::Vec4(1.f, 1.f, 1.f, 1.f);
    auto matH = renderer.createMaterial(matDesc);
    if (renderer.ensureMaterialPipeline(matH) != gfx::GfxError::None) return 1;

    ecs::Entity camE = registry.create();
    engine::Camera cam;
    cam.fovYRadians = 1.04719758f;
    cam.nearPlane = 0.1f; cam.farPlane = 100.f;
    cam.center = math::Vec3(0.f, 0.f, 0.f);
    registry.emplace<engine::Camera>(camE, cam);

    ecs::Entity triE = registry.create();
    auto& lt = registry.emplace<ecs::LocalTransform>(triE);
    lt.position = math::Vec3(0.f, 0.f, 0.f);
    lt.rotation = math::Quat::identity();
    lt.scale = math::Vec3(1.f, 1.f, 1.f);
    registry.emplace<engine::MeshRenderer>(triE, engine::MeshRenderer{meshH, matH, 0, false, false});
    ecs::updateWorldTransforms(registry);

    demo::Hud hud(dev);
    if (!hud.init()) return 1;

    f4::memory::enableHeapTracking(true);
    diag::Diagnostics diagnostics;
    diagnostics.setSampleSink(&demo::Hud::onSample, &hud);
    const std::uint32_t idFps = diagnostics.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    const std::uint32_t idFrameMs = diagnostics.metrics().declare({"frame_ms", diag::MetricType::Double, "frame", "ms"});
    const std::uint32_t idDraw = diagnostics.metrics().declare({"draw_calls", diag::MetricType::UInt, "gpu", "calls"});
    const std::uint32_t idTris = diagnostics.metrics().declare({"triangles", diag::MetricType::UInt, "gpu", "count"});
    const std::uint32_t idShaderWarn = diagnostics.metrics().declare({"shader_warnings", diag::MetricType::UInt, "gpu", "count"});
    const std::uint32_t idShaderErr = diagnostics.metrics().declare({"shader_errors", diag::MetricType::UInt, "gpu", "count"});
    const std::uint32_t idLiveBytes = diagnostics.metrics().declare({"live_bytes", diag::MetricType::UInt, "memory", "B"});
    const std::uint32_t idAllocs = diagnostics.metrics().declare({"allocs/frame", diag::MetricType::UInt, "memory", "count"});
    const std::uint32_t idPos = diagnostics.metrics().declare({"positions", diag::MetricType::String, "telemetry", nullptr});
    const std::uint32_t idCpuUs = diagnostics.metrics().declare({"cpu_zones_us", diag::MetricType::UInt, "profiler", "us"});

    bool running = true;
    Uint64 prev = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();
    double t = 0.0;
    float angle = 0.f;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
        }
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(static_cast<double>(now - prev) / static_cast<double>(freq));
        prev = now;
        t += dt;
        angle += dt * 0.5f;
        diagnostics.metrics()[idFps].set(dt > 0 ? 1.0 / dt : 0.0);
        diagnostics.metrics()[idFrameMs].set(dt * 1000.0);

        diagnostics.profiler().beginFrame();
        {
            diag::ScopedZone zone(diagnostics.profiler(), "frame");
            if (auto* c = registry.get<engine::Camera>(camE)) {
                c->eye = engine::orbitEye(2.2f, angle, 0.8f, c->center);
            }
            if (auto* lt2 = registry.get<ecs::LocalTransform>(triE)) {
                lt2->rotation = math::Quat::fromAxisAngle(math::Vec3(0.f, 1.f, 0.f), angle);
                ecs::tagTransformDirty(registry, triE);
            }
            ecs::updateWorldTransforms(registry);

            const std::uint32_t curW = window->width();
            const std::uint32_t curH = window->height();
            float aspect = curH > 0 ? static_cast<float>(curW) / static_cast<float>(curH) : 1.f;
            {
                gfx::CommandBuffer cb;
                cb.begin();
                cb.beginRenderPass(fb);
                renderer.record(cb, camE, aspect);
                hud.render(cb, curW, curH);
                cb.endRenderPass();
                cb.end();
                if (auto err = dev->submit(cb); err != gfx::GfxError::None) return 1;
                auto st = dev->stats();
                diagnostics.metrics()[idDraw].set(st.drawCalls + st.drawIndexedCalls);
                diagnostics.metrics()[idTris].set(st.primitives);
                diagnostics.metrics()[idShaderWarn].set(st.shaderWarnings);
                diagnostics.metrics()[idShaderErr].set(st.shaderFailed);
                diagnostics.metrics()[idLiveBytes].set(st.bufferBytes + st.textureBytes);
                diagnostics.metrics()[idAllocs].set(f4::memory::heapTracker().frame().allocations);
                if (auto* wm = registry.get<ecs::WorldMatrix>(triE)) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", wm->matrix.col[3].x, wm->matrix.col[3].y, wm->matrix.col[3].z);
                    diagnostics.metrics()[idPos].set(buf);
                }
            }
        }
        diagnostics.profiler().endFrame();
        {
            std::uint64_t cpuUs = 0;
            for (auto& z : diagnostics.profiler().zones()) cpuUs += (std::uint64_t)(z.totalNanos / 1000);
            diagnostics.metrics()[idCpuUs].set(cpuUs);
        }
        diagnostics.update(t);
        window->swap();
    }
    dev->waitIdle();
    return 0;
}
