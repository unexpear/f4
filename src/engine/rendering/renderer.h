#pragma once

#include "core/containers/hashmap.h"
#include "core/ecs/registry.h"
#include "core/resources/mesh.h"
#include "core/resources/resource_ids.h"
#include "core/resources/texture.h"
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
    std::uint32_t candidates = 0;
    std::uint32_t visible = 0;
    std::uint32_t frustumCulled = 0;
    std::uint32_t draws = 0;
    std::uint32_t instances = 0;
    // Opt-in CPU/driver wall times, not GPU execution durations.
    double mainRecordUs = 0;
    double materialPipelineUs = 0;
    double graphExecuteUs = 0;
};

struct PbrRenderSettings {
    resources::TextureHandle environment{}; // linear equirectangular HDR texture
    float exposure = 1.0f;
    float ambientIntensity = 0.35f;
    float shadowBias = 0.002f;
    float shadowExtent = 12.0f;
    float shadowNear = 0.1f;
    float shadowFar = 50.0f;
    std::uint32_t shadowResolution = 512;
    bool shadowsEnabled = true;
    bool frustumCullingEnabled = true;
    bool batchingEnabled = true;
    bool profileTimings = false;
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
    // Returns invalid on upload failure or exhaustion of the 64 live mesh slots.
    [[nodiscard]] auto createMesh(const resources::Mesh& mesh) -> resources::MeshHandle;
    void destroyMesh(resources::MeshHandle h);

    // Logical texture creation uploads every uncompressed mip. Materials keep
    // logical handles and never own backend handles directly.
    // Returns invalid on upload failure or exhaustion of the 128 live texture slots.
    [[nodiscard]] auto createTexture(const resources::Texture& texture)
        -> resources::TextureHandle;
    void destroyTexture(resources::TextureHandle h);

    // Material helpers delegate to MaterialCache (64 distinct live materials).
    // createMaterial returns invalid on exhaustion; glTF import returns CapacityExceeded.
    [[nodiscard]] auto createMaterial(const StandardMaterialDesc& desc) -> resources::MaterialHandle;
    [[nodiscard]] auto createMaterialFromGltf(
        const resources::GltfMaterial& material,
        std::span<const resources::TextureHandle> textures)
        -> Result<resources::MaterialHandle, MaterialImportError>;
    [[nodiscard]] auto ensureMaterialPipeline(resources::MaterialHandle h) -> gfx::GfxError;
    // Prepare the exact main-pass variant used by an uploaded mesh. The legacy
    // overload above prepares only the canonical non-instanced layout.
    [[nodiscard]] auto ensureMaterialPipeline(resources::MaterialHandle h,
                                               resources::MeshHandle mesh, bool mirrored = false)
        -> gfx::GfxError;

    [[nodiscard]] auto materialCache() -> MaterialCache& { return materials_; }
    [[nodiscard]] auto materialCache() const -> const MaterialCache& { return materials_; }
    [[nodiscard]] auto pbrSettings() -> PbrRenderSettings& { return pbrSettings_; }
    [[nodiscard]] auto pbrSettings() const -> const PbrRenderSettings& { return pbrSettings_; }

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
        gfx::VertexInputLayout instancedLayout;
        std::uint32_t instanceBinding = 0;
    };

    struct RenderInstance {
        math::Mat4 model;
        math::Vec4 normalColumns[3];
    };

    struct TextureEntry {
        resources::Texture texture;
        gfx::TextureHandle gpu;
    };

    void recordShadow(gfx::CommandBuffer& cb);
    void recordMain(gfx::CommandBuffer& cb, ecs::Entity cameraEntity, float aspect);
    void releaseInitializationResources();
    [[nodiscard]] auto resolveTexture(resources::TextureHandle texture,
                                      gfx::TextureHandle fallback) const
        -> gfx::TextureHandle;

    ecs::Registry& registry_;
    gfx::IDevice& device_;
    MaterialCache materials_;
    gfx::RenderGraph graph_;
    RenderQueue queue_;
    Array<RenderBatch> batches_;
    Array<RenderInstance> instanceStaging_;
    FrameStats stats_{};

    // One UBO per queued draw prevents deferred command replay from observing
    // only the final model/tint upload.
    ConstantRing sceneConstants_;
    ConstantRing shadowConstants_;
    DynamicBufferRing sceneInstances_;
    gfx::TextureHandle whiteTex_{};
    gfx::TextureHandle neutralNormalTex_{};
    gfx::TextureHandle blackTex_{};
    gfx::TextureHandle defaultEnvironmentTex_{};
    gfx::TextureHandle shadowColorTex_{};
    gfx::TextureHandle shadowDepthTex_{};
    gfx::SamplerHandle surfaceSampler_{};
    gfx::SamplerHandle environmentSampler_{};
    gfx::SamplerHandle shadowSampler_{};
    gfx::FramebufferHandle shadowFramebuffer_{};
    std::uint32_t defaultEnvironmentMipCount_ = 1;
    std::uint32_t shadowResolution_ = 512;

    containers::HashMap<resources::MeshHandle, MeshEntry> meshMap_;
    ids::GenerationalArena<resources::Mesh, resources::MeshTag> meshArena_{64};
    containers::HashMap<resources::TextureHandle, TextureEntry> textureMap_;
    ids::GenerationalArena<resources::Texture, resources::TextureTag> textureArena_{128};

    PbrRenderSettings pbrSettings_{};
    math::Mat4 frameLightViewProj_ = math::Mat4::identity();
    bool frameShadowsActive_ = false;
    gfx::GfxError recordError_ = gfx::GfxError::None;

    bool inited_ = false;
};

} // namespace f4::engine
