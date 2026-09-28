#pragma once

#include "engine/world/terrain_boundaries.h"

namespace f4::world {

struct BoundaryReplacementLimits {
    std::size_t maxSources = 256;
    std::size_t maxTriangles = 262144;
    std::size_t maxBytes = 128u * 1024u * 1024u;
};
enum class BoundaryReplacementError { InvalidData, IncompleteProfile, TrimRejected, StitchRejected, PrecisionLoss, LimitExceeded, ConflictingBoundaries, JunctionRejected };
struct BoundaryReplacementTriangle {
    bool wall = false, positive = false;
    std::size_t sourceIndex = 0, sourceTriangle = 0;
};
struct TerrainBoundaryReplacement {
    MeshData mesh;
    containers::Array<BoundaryReplacementTriangle> triangles;
};

// ONE complete boundary, assuming replaceable coplanar skirts/caps and solid
// columns. Includes FULL contributing meshes: consumers must suppress ALL those
// original source meshes when displaying/querying this replacement.
// Trims, stitches and packs sources/walls into one zero-origin world-space mesh.
// Exact float world conversion is required. Per-triangle provenance includes
// original source ids; wall provenance identifies the higher-side source top.
// Material ranges do not merge across policy sides. Caller resolves policy-local
// material ids. Limits bound final logical payload, not intermediate allocations.
// maxTriangles must be 1..1000000; individual trim/stitch/wall limits still apply.
// Does not compose multiple boundaries, discover them, or publish anything.
[[nodiscard]] auto buildTerrainBoundaryReplacement(std::span<const TerrainBoundaryMesh> negative,
                                                    std::span<const TerrainBoundaryMesh> positive,
                                                    TerrainBoundaryLine line,
                                                    BoundaryReplacementLimits limits = {},
                                                    std::uint16_t negativeMaterial = 0,
                                                    std::uint16_t positiveMaterial = 0)
    -> Result<TerrainBoundaryReplacement, BoundaryReplacementError>;

struct TerrainBoundaryRequest {
    TerrainBoundaryLine line;
    std::span<const std::size_t> negative, positive; // Indices in the shared source table.
    std::uint16_t negativeMaterial = 0, positiveMaterial = 0;
};
struct BoundaryCompositionLimits {
    BoundaryReplacementLimits mesh;
    std::size_t maxBoundaries = 64; // Must be 1..256.
};
struct BoundaryCompositionTriangle {
    bool wall = false, positive = false; // positive applies only to walls.
    std::size_t sourceIndex = 0, sourceTriangle = 0;
    std::size_t boundaryIndex = static_cast<std::size_t>(-1); // Input request index; walls only.
};
struct TerrainBoundaryComposition {
    MeshData mesh;
    containers::Array<BoundaryCompositionTriangle> triangles;
};

// Compose boundary segments over one shared source table, including compatible
// endpoint junctions. Split interior crossings/T contacts into endpoint requests.
// Each source must be referenced, with no repeated index within a request or
// duplicate mesh-pointer/translation entries. Caller supplies unique logical
// sources and the same solid-column/replaceable-cap contract as the single builder.
// Positive-length overlaps and interior contacts are rejected. At shared endpoints,
// sector heights must agree across incident profiles and absent boundary arms.
// Alternating solid/empty sectors (nonmanifold vertical edges) are rejected.
// Generated vertical wall edges are split at all incident sector heights. This
// validates endpoint column limits, not a general volumetric neighborhood.
// Processes lines in canonical axis/plane/begin/end order; preserves earlier cuts
// and original source provenance, emits each modified source once, then walls.
// Material ranges never cross source identities. Output is zero-origin; callers
// must suppress every original source. No dependency capture or world publication.
// Limits apply to final logical payload AND each single-boundary intermediate,
// not total working memory/time; allocation exceptions are not recovered.
[[nodiscard]] auto composeTerrainBoundaryReplacements(std::span<const TerrainBoundaryMesh> sources,
                                                       std::span<const TerrainBoundaryRequest> boundaries,
                                                       BoundaryCompositionLimits limits = {})
    -> Result<TerrainBoundaryComposition, BoundaryReplacementError>;

} // namespace f4::world
