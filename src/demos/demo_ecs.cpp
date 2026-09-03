#include "demos/demo_ecs.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/diagnostics/diagnostics.h"
#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/memory/memory.h"
#include "core/resources/mesh.h"
#include "engine/cameras/camera.h"
#include "engine/rendering/renderer.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::demo {

struct EcsScene::Impl {
    ecs::Registry registry{512};
    engine::Renderer renderer;
    ecs::Entity camE;
    ecs::Entity triE;
    float angle = 0.f;
    diag::Diagnostics diagnostics;
    std::uint32_t idFps, idFrameMs, idDraw, idTris, idShaderWarn, idShaderErr, idLiveBytes, idAllocs, idPos, idCpuUs;
    double t = 0.0;

    Impl(gfx::IDevice* dev) : renderer(registry, *dev) {}
};

static resources::Mesh makeTriangleMesh() {
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = 36;
    struct V { float px, py, pz, nx, ny, nz, u, v; std::uint8_t r, g, b, a; };
    const V verts[3] = {
        {-0.5f, -0.5f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 255, 0, 0, 255},
        {0.5f, -0.5f, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0, 255, 0, 255},
        {0.f, 0.5f, 0.f, 0.f, 0.f, 1.f, 0.5f, 1.f, 0, 0, 255, 255},
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

bool EcsScene::init(gfx::IDevice* dev, platform::Window*) {
    impl_ = new Impl(dev);
    if (impl_->renderer.init() != gfx::GfxError::None) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    auto triMesh = makeTriangleMesh();
    auto meshH = impl_->renderer.createMesh(triMesh);
    if (!meshH.isValid()) return false;

    engine::StandardMaterialDesc matDesc;
    matDesc.tint = math::Vec4(1.f, 1.f, 1.f, 1.f);
    matDesc.doubleSided = true;
    auto matH = impl_->renderer.createMaterial(matDesc);
    if (impl_->renderer.ensureMaterialPipeline(matH) != gfx::GfxError::None) return false;

    impl_->camE = impl_->registry.create();
    engine::Camera cam;
    cam.fovYRadians = 1.04719758f;
    cam.nearPlane = 0.1f; cam.farPlane = 100.f;
    cam.center = math::Vec3(0.f, 0.f, 0.f);
    impl_->registry.emplace<engine::Camera>(impl_->camE, cam);

    constexpr int columns = 21;
    constexpr int rows = 13;
    constexpr float spacing = 1.15f;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const ecs::Entity entity = impl_->registry.create();
            auto& transform = impl_->registry.emplace<ecs::LocalTransform>(entity);
            transform.position = {
                static_cast<float>(column - columns / 2) * spacing,
                static_cast<float>(row - rows / 2) * spacing,
                0.0f};
            transform.rotation = math::Quat::identity();
            transform.scale = math::Vec3(0.82f);
            impl_->registry.emplace<engine::MeshRenderer>(
                entity, engine::MeshRenderer{meshH, matH, 0, false, false});
            if (row == rows / 2 && column == columns / 2) impl_->triE = entity;
        }
    }
    ecs::updateWorldTransforms(impl_->registry);

    f4::memory::enableHeapTracking(true);
    // Bench handles HUD, don't set sample sink here

    impl_->idFps = impl_->diagnostics.metrics().declare({"fps", diag::MetricType::Double, "frame", "1/s"});
    impl_->idFrameMs = impl_->diagnostics.metrics().declare({"frame_ms", diag::MetricType::Double, "frame", "ms"});
    impl_->idDraw = impl_->diagnostics.metrics().declare({"draw_calls", diag::MetricType::UInt, "gpu", "calls"});
    impl_->idTris = impl_->diagnostics.metrics().declare({"triangles", diag::MetricType::UInt, "gpu", "count"});
    impl_->idShaderWarn = impl_->diagnostics.metrics().declare({"shader_warnings", diag::MetricType::UInt, "gpu", "count"});
    impl_->idShaderErr = impl_->diagnostics.metrics().declare({"shader_errors", diag::MetricType::UInt, "gpu", "count"});
    impl_->idLiveBytes = impl_->diagnostics.metrics().declare({"live_bytes", diag::MetricType::UInt, "memory", "B"});
    impl_->idAllocs = impl_->diagnostics.metrics().declare({"allocs/frame", diag::MetricType::UInt, "memory", "count"});
    impl_->idPos = impl_->diagnostics.metrics().declare({"positions", diag::MetricType::String, "telemetry", nullptr});
    impl_->idCpuUs = impl_->diagnostics.metrics().declare({"cpu_zones_us", diag::MetricType::UInt, "profiler", "us"});

    return true;
}

void EcsScene::shutdown(gfx::IDevice* dev) {
    if (!impl_) return;
    dev->waitIdle();
    delete impl_;
    impl_ = nullptr;
}

void EcsScene::onResize(std::uint32_t, std::uint32_t) {}

void EcsScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    if (!impl_) return;
    impl_->angle += dt * 0.5f;
    impl_->t += dt;

    impl_->diagnostics.profiler().beginFrame();
    {
        diag::ScopedZone zone(impl_->diagnostics.profiler(), "frame");

        // One independently moving instance verifies that batching preserves
        // per-object transforms while the surrounding grid remains one batch.
        if (auto* lt2 = impl_->registry.get<ecs::LocalTransform>(impl_->triE)) {
            lt2->rotation = math::Quat::fromAxisAngle(math::Vec3(0.f, 1.f, 0.f), impl_->angle);
            ecs::tagTransformDirty(impl_->registry, impl_->triE);
        }
        ecs::updateWorldTransforms(impl_->registry);
    }
    impl_->diagnostics.profiler().endFrame();

    impl_->diagnostics.metrics()[impl_->idFps].set(dt > 0 ? 1.0 / dt : 0.0);
    impl_->diagnostics.metrics()[impl_->idFrameMs].set(dt * 1000.0);
    {
        std::uint64_t cpuUs = 0;
        for (auto& z : impl_->diagnostics.profiler().zones()) cpuUs += (std::uint64_t)(z.totalNanos / 1000);
        impl_->diagnostics.metrics()[impl_->idCpuUs].set(cpuUs);
    }
    impl_->diagnostics.update(impl_->t);
}

void EcsScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) {
    if (!impl_) return;

    // Renderer::record derives viewProj from these fields, so synchronize the
    // complete camera rather than writing a matrix that record will replace.
    if (auto* c = impl_->registry.get<engine::Camera>(impl_->camE)) {
        const float cy = std::cos(camera.yaw);
        const float sy = std::sin(camera.yaw);
        const float cp = std::cos(camera.pitch);
        const float sp = std::sin(camera.pitch);
        const math::Vec3 forward{-cy * cp, -sp, -sy * cp};
        c->eye = camera.position;
        c->center = camera.position + forward;
        c->up = math::Vec3(0.f, 1.f, 0.f);
    }

    impl_->renderer.record(cb, impl_->camE, height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f);

    // Copy device stats to scene diagnostics
    // (bench will also do this, but we can add scene-specific metrics here)
}

} // namespace f4::demo
