#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "engine/world/terrain_meshing.h"

namespace f4::world {

struct TerrainMeshRayHit {
    double distance = 0.0; // World distance, independent of direction magnitude.
    math::Vec3 position{};
    math::Vec3 normal{}; // Unit geometric normal, preserving triangle winding.
    std::array<double, 3> barycentric{};
    std::size_t triangleIndex = 0;
    bool frontFace = false;
};

// Reference surface query: O(triangles), no allocation or acceleration structure.
// Closed interval [0,maxDistance]; exact distance ties select the lowest triangle
// index. Coplanar/degenerate triangles do not hit. Backfaces hit unless culled.
// Invalid rays, translations, indices or vertex positions return no hit. This
// inspects mesh surfaces, not solid-cell occupancy, materials or regional masks.
[[nodiscard]] auto raycastTerrainMesh(const MeshData& mesh, math::Vec3 origin,
                                      math::Vec3 direction, float maxDistance,
                                      math::Vec3 translation = {}, bool cullBackFaces = false)
    -> std::optional<TerrainMeshRayHit>;

} // namespace f4::world
