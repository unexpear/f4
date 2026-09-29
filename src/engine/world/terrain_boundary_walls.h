#pragma once

#include "engine/world/terrain_boundaries.h"

namespace f4::world {

enum class TerrainBoundaryWallError { InvalidData, IncompleteProfile, PrecisionLoss, TriangleLimitExceeded };
struct TerrainBoundaryWallTriangle {
    std::size_t spanIndex = 0;
    bool positiveOwner = false;
};
struct TerrainBoundaryWallEdge {
    std::size_t spanIndex = 0;
    bool positive = false;
    double begin = 0, end = 0, beginHeight = 0, endHeight = 0;
};
struct TerrainBoundaryWallBuild {
    MeshData mesh;
    // One record per output triangle. Source/triangle provenance is in the input span.
    containers::Array<TerrainBoundaryWallTriangle> triangles;
    // Both top edges per piece, including crossings and equal-height pieces.
    // Cast coordinates once to float to obtain the wall's canonical vertices.
    containers::Array<TerrainBoundaryWallEdge> topEdges;
};

// Pure, worker-safe geometry builder for a complete ordered top-profile partition.
// Solid-column interpretation only: the higher side owns each outward-facing wall.
// Crossings split ownership at one shared point; equal tops generate no face.
// Materials are explicit per-side wall ids, not inferred from top source indices.
// Positions are world-space floats; unrepresentable planes, collapsed intervals
// or height order are errors, never epsilon-welded or silently skipped.
// Does NOT remove existing coplanar skirts/caps, publish geometry, enforce global
// boundary ownership, or validate snapshot freshness. Do not overlay on old walls.
[[nodiscard]] auto meshTerrainBoundaryWalls(std::span<const TerrainBoundarySpan> profile,
                                           TerrainBoundaryLine line,
                                           std::uint16_t negativeMaterial = 0,
                                           std::uint16_t positiveMaterial = 0,
                                           std::size_t triangleLimit = 131072)
    -> Result<TerrainBoundaryWallBuild, TerrainBoundaryWallError>;

} // namespace f4::world
