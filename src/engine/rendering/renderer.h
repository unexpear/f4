#pragma once

#include "core/containers/hashmap.h"
#include "core/ecs/registry.h"
#include "core/resources/mesh.h"
#include "core/resources/resource_ids.h"
#include "engine/cameras/camera.h"
#include "engine/rendering/gpu_mesh.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/render_queue.h"
#include "engine/rendering/shader_constants.h"
#include "graphics/device/device.h"
#include "graphics/render_graph/render_graph.h"

namespace f4::engine {

struct FrameStats {
    std::uint32_t draws = 0;
    std::uint32_t instances = 0;
};

// Renderer: owns GPU representations (GpuMesh, Material pipelines, UBO, fallback
// white texture). MeshRenderer/ResourceHandle<MeshTag> are logical; renderer
// translates them. No ECS component owns a BufferHandle directly.
class Renderer {
public:
    explicit Renderer(ecs::Registry& registry, gfx::IDevice& device);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Must be called once after construction; creates per-draw constant buffers
    // and the fallback white texture/sampler. Returns error on failure.
    [[nodiscard]] auto init() -> gfx::GfxError;

    // Logical mesh creation — stores a copy of the mesh and creates its GpuMesh
    // immediately on the device. Returns a handle for MeshRenderer::mesh.
    [[nodiscard]] auto createMesh(const resources::Mesh& mesh) -> resources::MeshHandle;
    void destroyMesh(resources::MeshHandle h);

    // Material helpers delegate to MaterialCache.
    [[nodiscard]] auto createMaterial(const StandardMaterialDesc& desc) -> resources::MaterialHandle;
    [[nodiscard]] auto ensureMaterialPipeline(resources::MaterialHandle h) -> gfx::GfxError;

    [[nodiscard]] auto materialCache() -> MaterialCache& { return materials_; }
    [[nodiscard]] auto materialCache() const -> const MaterialCache& { return materials_; }

    // Main render: builds a RenderQueue from MeshRenderer+WorldMatrix, sorts,
    // declares a single "main" pass in the RenderGraph that writes to `fb`,
    // and executes it. Caller owns fb (window framebuffer) and swap.
    // `cameraEntity` must have a Camera component (orbit).
    [[nodiscard]] auto render(gfx::FramebufferHandle fb, ecs::Entity cameraEntity, float aspect) -> gfx::GfxError;

    // Records draws for the current queue into `cb` assuming `cb` is already
    // inside a `beginRenderPass(fb)` block. Useful for single-pass HUD composition.
    void record(gfx::CommandBuffer& cb, ecs::Entity cameraEntity, float aspect);

    [[nodiscard]] auto stats() const -> FrameStats { return stats_; }
    [[nodiscard]] auto graph() -> gfx::RenderGraph& { return graph_; }

private:
    struct MeshEntry {
        resources::Mesh mesh;
        GpuMesh gpu;
    };

    ecs::Registry& registry_;
    gfx::IDevice& device_;
    MaterialCache materials_;
    gfx::RenderGraph graph_;
    RenderQueue queue_;
    FrameStats stats_{};

    // One UBO per queued draw prevents deferred command replay from observing
    // only the final model/tint upload.
    ConstantRing sceneConstants_;
    gfx::TextureHandle whiteTex_{};
    gfx::SamplerHandle whiteSmp_{};

    containers::HashMap<resources::MeshHandle, MeshEntry> meshMap_;
    ids::GenerationalArena<resources::Mesh, resources::MeshTag> meshArena_{64};

    bool inited_ = false;
};

} // namespace f4::engine
