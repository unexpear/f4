#pragma once

#include <cstdint>
#include <span>

#include "core/containers/array.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "graphics/types.h"

namespace f4::engine {

// Clustered forward point lights.
//
// The CPU bins every frame's point lights into a camera froxel grid and packs
// the result into ONE read-only storage buffer of 32-bit words that the PBR
// fragment shader walks. Every native shader (GLSL, SPIR-V, HLSL, WGSL, MSL)
// reads the same word layout, so the layout below is the cross-backend
// contract and must only change together with all PBR fragment shaders.
//
//   words[0]  light count            words[4]  first light record word
//   words[1]  clusters X             words[5]  first cluster record word
//   words[2]  clusters Y             words[6]  first light-index word
//   words[3]  clusters Z (slices)    words[7]  total light-index count
//   words[8]  slice scale (f32 bits) words[9]  slice bias (f32 bits)
//   words[10..14] point-light shadows (see point_shadows.h), words[15] zero
//   light record (8 words): position.xyz, range, radiance.rgb, shadow slot + 1
//   cluster record (2 words): first index word, index count
//   light index list: one light number per word
//
// Cluster lookup (identical on CPU and GPU):
//   clip  = viewProj * worldPosition
//   tileX = clamp(floor((clip.x / clip.w * 0.5 + 0.5) * X), 0, X - 1)
//   tileY = clamp(floor((clip.y / clip.w * 0.5 + 0.5) * Y), 0, Y - 1)
//   slice = clamp(floor(log2(max(clip.w, 1e-4)) * scale + bias), 0, Z - 1)
// clip.w is the positive view depth for the engine's perspectiveRH_ZO camera.
inline constexpr std::uint32_t kPointLightHeaderWords = 16;
inline constexpr std::uint32_t kPointLightRecordWords = 8;
inline constexpr std::uint32_t kPointLightClusterWords = 2;
// Shader binding contract: one read-only storage buffer in bind point 0.
// Slot 7 sits past the PBR texture slots 0..6, so DX12's shared SRV register
// space (t7) never aliases a texture. Metal exposes only seven resource slots
// per bind point but keeps buffers in their own namespace, so it uses slot 1
// (buffer index 16 + 1 = 17), which aliases nothing there.
//   OpenGL: SSBO binding 7   Vulkan/WebGPU: binding 7 * 4 + 3 = 31
//   DX12: register(t7, space0)   Metal: [[buffer(17)]]
[[nodiscard]] constexpr auto pointLightStorageSlot(gfx::Backend backend) -> std::uint32_t {
    return backend == gfx::Backend::Metal ? 1u : 7u;
}

struct ClusterGridConfig {
    std::uint32_t tilesX = 16;
    std::uint32_t tilesY = 9;
    std::uint32_t slices = 24;
    // Hard cap on light references across all clusters; references beyond it
    // are dropped (and counted) instead of growing without bound.
    std::uint32_t maxIndexCount = 1u << 20;
};

struct ClusteredLightInput {
    math::Vec3 position{};
    math::Vec3 radiance{}; // color * intensity, linear
    float range = 0.0f;
    // 0 = unshadowed, otherwise the point-shadow atlas slot + 1.
    std::uint32_t shadowSlot = 0;
};

struct ClusteredLightStats {
    std::uint32_t submitted = 0;
    std::uint32_t visible = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t droppedIndices = 0;
    std::uint32_t maxLightsPerCluster = 0;
};

struct ClusterSliceParams {
    float scale = 0.0f;
    float bias = 0.0f;
};

[[nodiscard]] auto clusterSliceParams(float nearPlane, float farPlane,
                                      std::uint32_t slices) -> ClusterSliceParams;

// The same slice function the shaders evaluate.
[[nodiscard]] auto clusterSliceForDepth(float viewDepth, const ClusterSliceParams& params,
                                        std::uint32_t slices) -> std::uint32_t;

// Packs `lights` for a camera with `view`/`projection` (perspectiveRH_ZO).
// `words` is cleared and refilled; it always holds at least a valid header.
auto buildClusteredLightBuffer(std::span<const ClusteredLightInput> lights,
                               const math::Mat4& view, const math::Mat4& projection,
                               float nearPlane, float farPlane,
                               const ClusterGridConfig& config,
                               containers::Array<std::uint32_t>& words)
    -> ClusteredLightStats;

} // namespace f4::engine
