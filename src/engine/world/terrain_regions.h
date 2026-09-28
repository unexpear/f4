#pragma once

#include "engine/world/terrain_meshing.h"
#include "engine/world/terrain_boundaries.h"

namespace f4::world {

struct TerrainFootprint {
    double minX, maxX, minZ, maxZ;
};

struct TerrainRegionMask {
    containers::Array<TerrainFootprint> rectangles;
    bool fullyOwned = false;
};

// Positive-area XZ intersections, in deterministic Z/X order. Invalid bounds,
// coordinates outside the int32 world domain, or >4096 examined region tiles
// return an error, never a silently truncated mask.
[[nodiscard]] auto terrainRegionMask(TerrainFootprint footprint,
                                     const RegionMap& regions, PolicyKind policy)
    -> Result<TerrainRegionMask, TerrainMeshError>;

// Clips existing CPU triangles to their policy's rectangular region closures.
// Preserves winding and interpolated attributes. Optional hexCutFloor caps Hex
// prism cuts to their existing local-space floor; requires a meshHexChunk mesh
// built with that same floor, not an arbitrary mesh or overhanging surface.
// Coplanar old walls at capped boundaries are replaced, not overdrawn. No
// inter-policy bridges or general solid query is generated. Null preserves
// surface-only behavior and the layout of fully owned meshes.
[[nodiscard]] auto clipTerrainRegions(const MeshData& mesh, math::Vec3 translation,
                                      const RegionMap& regions, PolicyKind policy,
                                      std::optional<float> hexCutFloor = {})
    -> Result<MeshData, TerrainMeshError>;

enum class TerrainBoundaryTrimError { InvalidData, PrecisionLoss, TriangleLimitExceeded };
struct TerrainBoundaryTrim {
    MeshData mesh;
    containers::Array<std::size_t> sourceTriangles; // One original triangle id per output triangle.
    std::size_t affectedTriangles = 0;
};

// Removes ALL coplanar face area in the open tangent interval, at ALL heights
// and in both windings. Caller must establish that these faces are replaceable
// skirts/caps, not arbitrary walls. Noncoplanar faces (including tops) and parts
// outside the interval are retained. Positions remain in the source mesh frame.
// Cut attributes use the same barycentric rules as clipTerrainRegions. Unchanged
// triangles retain original indices/vertices; new surface ranges retain materials.
// Exact local float cut planes are required; collapsed retained area is an error.
// Output triangle limit 1..131072; not a total memory bound. No world mutation,
// top-edge stitching, wall generation or freshness/publication transaction.
[[nodiscard]] auto trimTerrainBoundaryFaces(const MeshData& mesh, math::Vec3 translation,
                                            TerrainBoundaryLine line, std::size_t triangleLimit = 131072)
    -> Result<TerrainBoundaryTrim, TerrainBoundaryTrimError>;

struct TerrainBoundaryTopCut {
    std::size_t triangleIndex = 0;
    double tangent = 0, height = 0; // Exact world parameter and canonical wall height.
};
enum class TerrainBoundaryStitchError { InvalidData, UnsupportedEdge, PrecisionLoss, TriangleLimitExceeded };
struct TerrainBoundaryStitch {
    MeshData mesh;
    containers::Array<std::size_t> sourceTriangles;
    std::size_t splitTriangles = 0;
};

// Splits an EXISTING upward triangle edge on the boundary plane at supplied wall
// points. Interior-plane cuts/undersides are unsupported. Cuts may be unordered
// or repeated. Their rounded heights must match independent source-edge evaluation;
// local vertices must reconstruct the exact canonical world float coordinates.
// Uses the same attribute rules as region clipping, retains unchanged triangles,
// and maps every new triangle to its input id. No epsilon welding or publication.
// At most 524292 cuts; output triangle limit 1..131072 (not a memory/time budget).
[[nodiscard]] auto stitchTerrainBoundaryTops(const MeshData& mesh, math::Vec3 translation,
                                            TerrainBoundaryLine line, std::span<const TerrainBoundaryTopCut> cuts,
                                            std::size_t triangleLimit = 131072)
    -> Result<TerrainBoundaryStitch, TerrainBoundaryStitchError>;

} // namespace f4::world
