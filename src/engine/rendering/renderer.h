#pragma once

#include "core/containers/hashmap.h"
#include "core/ecs/registry.h"
#include "core/resources/mesh.h"
#include "core/resources/resource_ids.h"
#include "core/resources/texture.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/clustered_lights.h"
#include "engine/lighting/point_shadows.h"
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
    // Clustered point lights: submitted ECS lights, lights surviving camera
    // culling, total cluster references, and references dropped by the cap.
    std::uint32_t pointLights = 0;
    std::uint32_t pointLightsVisible = 0;
    std::uint32_t pointLightReferences = 0;
    std::uint32_t pointLightReferencesDropped = 0;
    std::uint32_t maxPointLightsPerCluster = 0;
    // Point-light shadows: lights given an atlas slot this frame and the face
    // draws recorded for them.
    std::uint32_t pointShadowLights = 0;
    std::uint32_t pointShadowDraws = 0;
    // Shadowed lights whose six tiles were re-rendered this frame; the rest
    // reused last frame's tiles (see PbrRenderSettings::pointShadowCaching).
    std::uint32_t pointShadowLightsRedrawn = 0;
    // Fullscreen bloom passes (prefilter, downsamples, upsamples, composite).
    std::uint32_t bloomPasses = 0;
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
    bool pointLightsEnabled = true;
    ClusterGridConfig pointLightClusters{};

    // HDR bloom. Off by default so existing frames stay byte-identical. When
    // on, the PBR pass writes linear HDR (exposure applied) into an
    // RGBA16_FLOAT target; a threshold + downsample/upsample chain blurs the
    // bright parts and a composite adds bloom * intensity before the same ACES
    // + sRGB encode. render() then needs the target extent.
    bool bloomEnabled = false;
    float bloomThreshold = 1.0f;   // exposed linear brightness where bloom starts
    float bloomKnee = 0.5f;        // soft-threshold width
    float bloomIntensity = 0.6f;   // composite weight of the summed blur levels
    std::uint32_t bloomLevels = 5; // half-resolution steps, 1..8
    math::Vec4 bloomClearColor{0.0f, 0.0f, 0.0f, 1.0f}; // linear HDR background

    // Point-light shadows: up to `pointShadowBudget` lights flagged
    // PointLight::castShadows (brightest/closest first) render cube shadows
    // into one atlas; the rest stay unshadowed. See point_shadows.h.
    bool pointShadowsEnabled = true;
    std::uint32_t pointShadowBudget = 8;     // 0..16
    std::uint32_t pointShadowTileSize = 256; // texels per cube face
    float pointShadowBias = 0.02f;           // in units of the light's range
    // Reuse a shadowed light's six tiles when neither the light nor any
    // caster within its range changed since the frame that drew them. Each
    // frame the renderer builds a signature per light from its position,
    // range and tile size and, for every caster inside its sphere, the mesh,
    // material (alpha inputs), submesh and world matrix; a light keeps its
    // atlas slot while it stays selected. The cached image is identical to
    // the uncached one (false redraws every tile every frame).
    bool pointShadowCaching = true;
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

    // Logical texture creation uploads every uncompressed mip. Materials keep
    // logical handles and never own backend handles directly.
    [[nodiscard]] auto createTexture(const resources::Texture& texture)
        -> resources::TextureHandle;
    void destroyTexture(resources::TextureHandle h);

    // Material helpers delegate to MaterialCache.
    [[nodiscard]] auto createMaterial(const StandardMaterialDesc& desc) -> resources::MaterialHandle;
    [[nodiscard]] auto createMaterialFromGltf(
        const resources::GltfMaterial& material,
        std::span<const resources::TextureHandle> textures)
        -> Result<resources::MaterialHandle, MaterialImportError>;
    [[nodiscard]] auto ensureMaterialPipeline(resources::MaterialHandle h) -> gfx::GfxError;

    [[nodiscard]] auto materialCache() -> MaterialCache& { return materials_; }
    [[nodiscard]] auto materialCache() const -> const MaterialCache& { return materials_; }
    [[nodiscard]] auto pbrSettings() -> PbrRenderSettings& { return pbrSettings_; }
    [[nodiscard]] auto pbrSettings() const -> const PbrRenderSettings& { return pbrSettings_; }

    // Main render: builds a RenderQueue from MeshRenderer+WorldMatrix, sorts,
    // declares a single "main" pass in the RenderGraph that writes to `fb`,
    // and executes it. Caller owns fb (window framebuffer) and swap.
    // `cameraEntity` must have a Camera component (orbit).
    // `extent` is the size of `fb`; it is only required with bloom enabled.
    [[nodiscard]] auto render(gfx::FramebufferHandle fb, ecs::Entity cameraEntity, float aspect,
                              gfx::Extent extent = {}) -> gfx::GfxError;

    // Records draws for the current queue into `cb` assuming `cb` is already
    // inside a `beginRenderPass(fb)` block. Useful for single-pass HUD composition.
    void record(gfx::CommandBuffer& cb, ecs::Entity cameraEntity, float aspect);

    [[nodiscard]] auto stats() const -> FrameStats { return stats_; }
    // Word buffer most recently uploaded for clustered point lights.
    [[nodiscard]] auto pointLightWords() const -> std::span<const std::uint32_t> {
        return {pointLightWords_.data(), pointLightWords_.size()};
    }
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
    void recordPointShadows(gfx::CommandBuffer& cb);
    void gatherPointShadowCasters();
    [[nodiscard]] auto pointShadowSignature(const math::Vec3& position, float range) const
        -> std::uint64_t;
    [[nodiscard]] auto ensurePointShadowResetMesh() -> gfx::GfxError;
    void gatherPointLights();
    [[nodiscard]] auto preparePointLights(const Camera& camera, float aspect)
        -> gfx::BufferHandle;
    [[nodiscard]] auto preparePointShadows(const Camera& camera) -> gfx::GfxError;
    [[nodiscard]] auto ensurePointShadowAtlas(std::uint32_t rows) -> gfx::GfxError;
    void releasePointShadowAtlas();
    [[nodiscard]] auto ensureBloomResources(gfx::Extent extent) -> gfx::GfxError;
    void releaseBloomTargets();
    void declareBloomPasses(std::uint32_t hdrImage, std::uint32_t target, gfx::Extent extent);
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
    // Read-only storage buffer per frame holding the clustered point lights.
    DynamicBufferRing pointLightBuffers_;
    containers::Array<ClusteredLightInput> pointLightInputs_;
    containers::Array<std::uint8_t> pointLightCastsShadow_;
    containers::Array<std::uint64_t> pointLightKeys_; // entity index << 32 | generation
    bool pointLightsGathered_ = false;
    containers::Array<std::uint32_t> pointLightWords_;
    gfx::BufferHandle framePointLights_{};
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

    // Point-light shadow atlas and this frame's selection.
    // Indexed by atlas slot; inactive slots hold no light this frame.
    struct ShadowedPointLight {
        math::Vec3 position{};
        float range = 0.0f;
        bool active = false;
        bool dirty = false;
        std::uint64_t signature = 0;
    };
    // Per atlas slot: which light owns it and the signature its tiles were
    // drawn with (valid only after a successful submit).
    containers::Array<std::uint64_t> pointShadowSlotOwners_;
    containers::Array<std::uint64_t> pointShadowSlotSignatures_;
    containers::Array<std::uint8_t> pointShadowSlotValid_;
    containers::Array<std::uint32_t> shadowSlots_;
    containers::Array<std::uint64_t> shadowSlotKeys_;
    ConstantRing pointShadowConstants_;
    // Shadow casters this frame, grouped into one instanced draw per
    // (light, face, mesh/material) with the models in an instance stream.
    struct PointShadowCaster {
        resources::MeshHandle mesh{};
        resources::MaterialHandle material{};
        std::uint32_t subMesh = 0;
        math::Mat4 model = math::Mat4::identity();
        math::AABB bounds{};
        bool cullable = false;
    };
    containers::Array<PointShadowCaster> pointShadowCasters_;
    containers::Array<RenderInstance> pointShadowInstanceStaging_;
    DynamicBufferRing pointShadowInstanceBuffers_;
    containers::Array<PointShadowCandidate> shadowCandidates_;
    containers::Array<std::uint32_t> shadowCandidateSources_;
    containers::Array<std::uint32_t> shadowSelection_;
    containers::Array<ShadowedPointLight> frameShadowedLights_;
    containers::Array<math::Mat4> frameShadowFaces_;
    gfx::TextureHandle pointShadowColorTex_{};
    gfx::TextureHandle pointShadowDepthTex_{};
    gfx::FramebufferHandle pointShadowFramebuffer_{};     // clears colour + depth
    gfx::FramebufferHandle pointShadowLoadFramebuffer_{}; // keeps colour, clears depth
    resources::MeshHandle pointShadowResetMesh_{};
    std::uint32_t pointShadowTileSize_ = 0;
    std::uint32_t pointShadowRows_ = 0;
    std::uint32_t pointShadowDraws_ = 0;
    std::uint32_t pointShadowRedrawn_ = 0;
    bool framePointShadowsActive_ = false;
    bool framePointShadowsDirty_ = false;    // at least one slot is re-rendered
    bool framePointShadowsFullClear_ = true; // every active slot is re-rendered

    // HDR bloom chain.
    struct BloomTarget {
        gfx::TextureHandle texture{};
        gfx::FramebufferHandle framebuffer{};
        gfx::Extent extent{};
    };
    ConstantRing bloomConstants_;
    containers::Array<BloomTarget> bloomDown_;
    containers::Array<BloomTarget> bloomUp_;
    gfx::TextureHandle hdrColorTex_{};
    gfx::TextureHandle hdrDepthTex_{};
    gfx::FramebufferHandle hdrFramebuffer_{};
    gfx::Extent bloomExtent_{};
    std::uint32_t bloomLevelCount_ = 0;
    math::Vec4 hdrClearColor_{0.0f, 0.0f, 0.0f, 1.0f};
    gfx::ShaderHandle bloomVs_{};
    gfx::ShaderHandle bloomFs_{};
    gfx::PipelineHandle bloomHdrPipeline_{};
    gfx::PipelineHandle bloomCompositePipeline_{};
    gfx::BufferHandle bloomTriangle_{};
    gfx::SamplerHandle bloomSampler_{};
    std::uint32_t bloomPasses_ = 0;
    bool frameBloomActive_ = false;
    gfx::TextureFormat recordColorFormat_ = gfx::TextureFormat::RGBA8_UNORM;

    bool inited_ = false;
};

} // namespace f4::engine
