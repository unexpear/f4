#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"
#include "core/result.h"

namespace f4::resources {

using containers::Array;
using containers::String;

inline constexpr std::uint32_t kInvalidGltfIndex =
    std::numeric_limits<std::uint32_t>::max();

enum class GltfAlphaMode : std::uint8_t {
    Opaque,
    Mask,
    Blend,
};

// Texture indices refer to the source glTF texture array. A missing texture
// uses kInvalidGltfIndex; scale is normal scale or occlusion strength where
// those properties apply.
struct GltfTextureRef {
    std::uint32_t textureIndex = kInvalidGltfIndex;
    std::uint32_t texCoord = 0;
    float scale = 1.0f;

    [[nodiscard]] auto hasTexture() const -> bool {
        return textureIndex != kInvalidGltfIndex;
    }
};

// CPU-side glTF core metallic-roughness data. Rendering-resource creation is
// intentionally left to the engine layer introduced in M8.
struct GltfMaterial {
    String name;
    math::Vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    math::Vec3 emissiveFactor{0.0f};
    GltfTextureRef baseColorTexture;
    GltfTextureRef metallicRoughnessTexture;
    GltfTextureRef normalTexture;
    GltfTextureRef occlusionTexture;
    GltfTextureRef emissiveTexture;
    GltfAlphaMode alphaMode = GltfAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
};

// Each emitted mesh represents one source primitive. meshMaterialIndices is
// parallel to meshes and contains kInvalidGltfIndex for the default material.
struct GltfScene {
    Array<Mesh> meshes;
    Array<GltfMaterial> materials;
    Array<std::uint32_t> meshMaterialIndices;
};

// Parse a glTF 2.0 JSON string or GLB binary blob from memory.
// Extracts mesh primitives, vertex attributes, and core PBR material records.
// Bounds are computed automatically via computeBounds().
[[nodiscard]] auto loadGltf(std::span<const std::byte> bytes)
    -> Result<GltfScene, LoadError>;

} // namespace f4::resources
