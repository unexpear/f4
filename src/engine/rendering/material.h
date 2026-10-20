#pragma once

#include <cstdint>
#include <span>

#include "core/containers/array.h"
#include "core/containers/hashmap.h"
#include "core/ids/generational_arena.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"
#include "core/resources/gltf_loader.h"
#include "core/resources/resource_ids.h"
#include "core/result.h"
#include "graphics/device/device.h"
#include "graphics/gfx_error.h"
#include "graphics/pipeline/pipeline_state.h"

namespace f4::engine {

using containers::Array;
using containers::HashMap;

enum class MaterialAlphaMode : std::uint8_t {
    Opaque,
    Mask,
    Blend,
};

enum class MaterialImportError : std::uint8_t {
    None,
    TextureIndexOutOfRange,
    InvalidTextureHandle,
    UnsupportedTexCoord,
};

// Core glTF 2.0 metallic-roughness material. Invalid texture handles resolve
// to semantic defaults, keeping one shader path without feature variants.
struct StandardMaterialDesc {
    math::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f}; // baseColorFactor
    math::Vec3 emissiveFactor{0.0f, 0.0f, 0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float normalYSign = 1.0f; // +1 is the glTF/OpenGL convention
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;

    resources::TextureHandle albedo{}; // base color, sampled as sRGB
    resources::TextureHandle normal{}; // tangent space, sampled as linear
    resources::TextureHandle metallicRoughness{}; // G=roughness, B=metallic
    resources::TextureHandle occlusion{}; // R, indirect lighting only
    resources::TextureHandle emissive{}; // sampled as sRGB

    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;
    bool unlit = false;
    bool transparent = false; // M8 compatibility alias for Blend

    bool operator==(const StandardMaterialDesc& o) const {
        return tint == o.tint && emissiveFactor == o.emissiveFactor &&
               metallicFactor == o.metallicFactor && roughnessFactor == o.roughnessFactor &&
               normalScale == o.normalScale && normalYSign == o.normalYSign &&
               occlusionStrength == o.occlusionStrength && alphaCutoff == o.alphaCutoff &&
               albedo == o.albedo && normal == o.normal &&
               metallicRoughness == o.metallicRoughness && occlusion == o.occlusion &&
               emissive == o.emissive && alphaMode == o.alphaMode &&
               doubleSided == o.doubleSided && unlit == o.unlit &&
               transparent == o.transparent;
    }
    bool operator!=(const StandardMaterialDesc& o) const { return !(*this == o); }
};

struct MaterialPipelineVariant {
    std::uint64_t vertexLayoutHash = 0;
    gfx::VertexInputLayout vertexInput;
    // Colour target format: RGBA8 for the tone-mapped pass, RGBA16_FLOAT when
    // the PBR pass writes linear HDR for bloom.
    gfx::TextureFormat colorFormat = gfx::TextureFormat::RGBA8_UNORM;
    gfx::PipelineHandle pipeline{};
};

// Converts loader-owned glTF indices into renderer-owned texture handles.
// M11 supports TEXCOORD_0; rejecting other sets prevents silent UV misuse.
[[nodiscard]] auto materialFromGltf(
    const resources::GltfMaterial& material,
    std::span<const resources::TextureHandle> textures)
    -> Result<StandardMaterialDesc, MaterialImportError>;

// Concrete material record owned by the cache. Pipeline variants are required
// because mesh streams are not constrained to M8's original 36-byte layout.
struct StandardMaterial {
    StandardMaterialDesc desc{};
    gfx::PipelineHandle pipeline{}; // first variant, kept for M8 observability
    Array<MaterialPipelineVariant> variants;
};

// Simple cache: MaterialHandle -> StandardMaterial, deduped by desc.
// Owns the GPU Pipeline/Shader lifetime; Renderer drives eviction (clear/destroy).
class MaterialCache {
public:
    explicit MaterialCache() : arena_(64) {}

    [[nodiscard]] auto handleIsAlive(resources::MaterialHandle h) const -> bool {
        return arena_.contains(h);
    }

    [[nodiscard]] auto get(resources::MaterialHandle h) -> StandardMaterial* {
        return arena_.get(h);
    }
    [[nodiscard]] auto get(resources::MaterialHandle h) const -> const StandardMaterial* {
        return arena_.get(h);
    }

    // Find an existing material with identical desc; returns invalid handle if none.
    [[nodiscard]] auto find(const StandardMaterialDesc& desc) const -> resources::MaterialHandle;

    // Get existing or create a new StandardMaterial (pipeline lazily created by
    // caller via ensurePipeline). If newly created, returns a fresh handle with
    // empty pipeline (caller must create shaders/pipeline and assign).
    auto getOrCreate(const StandardMaterialDesc& desc) -> resources::MaterialHandle;

    // M8 compatibility overload using the canonical position/normal/UV/color
    // layout. Renderer uses the mesh-aware overload below.
    auto ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle h) -> gfx::GfxError;
    auto ensurePipeline(gfx::IDevice& dev, resources::MaterialHandle h,
                        const gfx::VertexInputLayout& vertexInput,
                        gfx::TextureFormat colorFormat = gfx::TextureFormat::RGBA8_UNORM)
        -> gfx::GfxError;
    [[nodiscard]] auto pipelineFor(resources::MaterialHandle h,
                                   const gfx::VertexInputLayout& vertexInput,
                                   gfx::TextureFormat colorFormat =
                                       gfx::TextureFormat::RGBA8_UNORM) const
        -> gfx::PipelineHandle;

    // Shadow pipelines share one shader pair and vary only by mesh layout.
    auto ensureShadowPipeline(gfx::IDevice& dev,
                              const gfx::VertexInputLayout& vertexInput) -> gfx::GfxError;
    [[nodiscard]] auto shadowPipelineFor(const gfx::VertexInputLayout& vertexInput) const
        -> gfx::PipelineHandle;
    // Point-light shadow faces (see engine/lighting/point_shadows.h): same
    // position/UV inputs as the sun shadow, distance-to-light output.
    // `tileReset` selects the variant with depth testing and depth writes off
    // that the shadow cache uses to reset one tile's colour to 1.0 (far)
    // without touching the cached tiles around it.
    auto ensurePointShadowPipeline(gfx::IDevice& dev, const gfx::VertexInputLayout& vertexInput,
                                   bool tileReset = false) -> gfx::GfxError;
    [[nodiscard]] auto pointShadowPipelineFor(const gfx::VertexInputLayout& vertexInput,
                                              bool tileReset = false) const
        -> gfx::PipelineHandle;

    auto destroy(resources::MaterialHandle h, gfx::IDevice& dev) -> gfx::GfxError;

    void clear(gfx::IDevice& dev);

    [[nodiscard]] auto liveCount() const -> std::size_t { return arena_.liveCount(); }

private:
    [[nodiscard]] static auto hashDesc(const StandardMaterialDesc& d) -> std::uint64_t;
    auto ensureShaders(gfx::IDevice& dev) -> gfx::GfxError;

    ids::GenerationalArena<StandardMaterial, resources::MaterialTag> arena_;
    // Fast dedup: hash(desc) -> handle. Collision handled by equality check; on
    // hash clash we linear probe via find(). Simple and M8-adequate.
    HashMap<std::uint64_t, resources::MaterialHandle> lookup_;
    gfx::IDevice* owner_ = nullptr;
    gfx::ShaderHandle pbrVs_{};
    gfx::ShaderHandle pbrInstancedVs_{};
    gfx::ShaderHandle pbrFs_{};
    gfx::ShaderHandle shadowVs_{};
    gfx::ShaderHandle shadowFs_{};
    Array<MaterialPipelineVariant> shadowVariants_;
    gfx::ShaderHandle pointShadowVs_{};
    gfx::ShaderHandle pointShadowFs_{};
    Array<MaterialPipelineVariant> pointShadowVariants_;
    Array<MaterialPipelineVariant> pointShadowResetVariants_;
};

} // namespace f4::engine
