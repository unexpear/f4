#pragma once

#include <cstdint>
#include <span>

#include "core/containers/array.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"
#include "graphics/types.h"

namespace f4::engine {

// Shadowed point lights.
//
// A small budget of point lights (flagged PointLight::castShadows, ranked by
// how bright and how close to the camera they are) render their six cube faces
// into ONE 2D atlas: six face columns by `budget` light rows, each tile
// `tileSize` texels square. Each face is a 90 degree perspectiveRH_ZO view from
// the light. The shadow pass stores the linear distance to the light divided
// by its range (0..1, cleared to 1) in an R16_FLOAT colour target, so every
// backend compares the same quantity regardless of its depth conventions.
//
// Each face renders into its tile through a per-tile viewport + scissor
// (CommandBuffer::setViewport/setScissor origins), so the hardware clips the
// geometry to the face frustum. pointShadowTileTransform() describes the same
// placement in atlas NDC for the PBR lookup; the shadow vertex shader also
// accepts it as clip.xy = clip.xy * tile.xy + tile.zw * clip.w (the renderer
// passes the identity), and the fragment shader discards anything outside
// the face's own [-1, 1] square as a guard.
// Casters are instanced: one draw per (light, face, mesh/material group), with
// the caster models in the same instance stream layout as the PBR pass.
//
// The PBR shader finds a fragment's face by the major axis of
// (fragment - light): +X, -X, +Y, -Y, +Z, -Z in that order, multiplies by the
// face matrix stored in the clustered light buffer, and does 3x3 PCF clamped
// inside the tile. Clustered-buffer additions (see clustered_lights.h):
//   light record word 7   shadow slot + 1 (0 = unshadowed)
//   words[10]  first face-matrix word (0 = no shadowed lights this frame)
//   words[11]  shadowed light count
//   words[12]  atlas rows (the light budget)
//   words[13]  depth bias in normalized distance units (f32 bits)
//   words[14]  one texel in face NDC units, 2 / tileSize (f32 bits)
//   face matrices: 6 column-major 4x4 matrices (96 words) per shadow slot
inline constexpr std::uint32_t kPointShadowFaces = 6;
inline constexpr std::uint32_t kPointShadowMatrixWords = 16;
inline constexpr std::uint32_t kPointShadowMaxBudget = 16;

// Shader binding of the atlas. DX12 shares t-registers between textures and
// the clustered light SRV (t7), so the atlas takes slot 8 in bind point 0.
// Metal exposes seven texture slots per bind point, so it uses bind point 1,
// slot 0 (texture/sampler index 7).
//   OpenGL: uTex8 / unit 8   Vulkan/WebGPU: bindings 33 (texture), 34 (sampler)
//   DX12: register(t8)/(s8)  Metal: [[texture(7)]] / [[sampler(7)]]
struct PointShadowBinding {
    std::uint32_t bindPoint = 0;
    std::uint32_t slot = 8;
};
[[nodiscard]] constexpr auto pointShadowAtlasBinding(gfx::Backend backend)
    -> PointShadowBinding {
    return backend == gfx::Backend::Metal ? PointShadowBinding{1u, 0u}
                                          : PointShadowBinding{0u, 8u};
}

struct PointShadowCandidate {
    math::Vec3 position{};
    math::Vec3 radiance{};
    float range = 0.0f;
};

// View-projection for one cube face (0..5 = +X, -X, +Y, -Y, +Z, -Z).
[[nodiscard]] auto pointShadowFaceViewProj(const math::Vec3& position, float range,
                                           std::uint32_t face) -> math::Mat4;

// Near plane used by every face matrix.
[[nodiscard]] auto pointShadowNearPlane(float range) -> float;

// Tile placement in atlas NDC: (scale.x, scale.y, offset.x, offset.y).
[[nodiscard]] auto pointShadowTileTransform(std::uint32_t slot, std::uint32_t face,
                                            std::uint32_t rows) -> math::Vec4;

// The face the shaders pick for a fragment at `toFragment` = fragment - light.
[[nodiscard]] auto pointShadowFaceFor(const math::Vec3& toFragment) -> std::uint32_t;

// Ranks `candidates` (already filtered to flagged lights) and writes up to
// `budget` indices into `selected`, best first. Score is peak radiance over
// (1 + d^2), d = distance from `eye` to the light's sphere (0 inside it).
// Ties keep input order, so the choice is deterministic.
void selectShadowedPointLights(std::span<const PointShadowCandidate> candidates,
                               const math::Vec3& eye, std::uint32_t budget,
                               containers::Array<std::uint32_t>& selected);

// Stable atlas slots. `owners[s]` is the key of the light that held slot `s`
// last frame (kPointShadowFreeSlot when free); `keys` are this frame's
// selected lights. A light that already owns a slot keeps it, so its six
// tiles stay where they are and can be reused by the shadow cache; the rest
// take the lowest free slots in selection order. `slots[i]` receives the slot
// of keys[i] and `owners` is rewritten for this frame (unselected slots are
// freed). `owners.size()` is the slot count and must be >= keys.size().
inline constexpr std::uint64_t kPointShadowFreeSlot = ~0ull;
void assignStablePointShadowSlots(std::span<const std::uint64_t> keys,
                                  containers::Array<std::uint64_t>& owners,
                                  containers::Array<std::uint32_t>& slots);

} // namespace f4::engine
