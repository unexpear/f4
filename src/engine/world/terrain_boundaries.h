#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "engine/world/terrain_meshing.h"

namespace f4::world {

enum class TerrainBoundaryAxis { X, Z };
struct TerrainBoundaryLine {
    TerrainBoundaryAxis axis = TerrainBoundaryAxis::X;
    double plane = 0;
    double begin = 0, end = 0; // World Z for an X plane, world X for a Z plane.
};
struct TerrainBoundaryMesh {
    const MeshData* mesh = nullptr;
    math::Vec3 translation{};
};
struct TerrainBoundarySample {
    double beginHeight = 0, endHeight = 0;
    std::size_t sourceIndex = 0, triangleIndex = 0;
};
struct TerrainBoundarySpan {
    double begin = 0, end = 0;
    std::size_t negativeCount = 0, positiveCount = 0;
    // A sample exists only for exactly one covering top triangle on that side.
    std::optional<TerrainBoundarySample> negative, positive;
};
enum class TerrainBoundaryError { InvalidData, SegmentLimitExceeded };

// Main-thread/offline reference profile of actual upward-wound triangles.
// Negative inputs must approach the plane from below X/Z, positive from above.
// Walls, undersides and point-only contacts do not contribute. Each output span
// has constant coverage on its OPEN interior; zero/multiple counts expose gaps
// or overlaps, never choose a topmost layer or interpolate across missing data.
// Canonical double intersections retain exact breakpoints without epsilon welds.
// Sources must remain immutable for this call; output owns its scalar data.
// Source order defines provenance indices. Callers retain mesh version/region
// stamps separately; this function provides no worker dependency/publication gate.
// The combined extracted segment limit is explicit (1..65536), never truncates.
[[nodiscard]] auto compareTerrainBoundaryTops(std::span<const TerrainBoundaryMesh> negative,
                                             std::span<const TerrainBoundaryMesh> positive,
                                             TerrainBoundaryLine line,
                                             std::size_t segmentLimit = 65536)
    -> Result<containers::Array<TerrainBoundarySpan>, TerrainBoundaryError>;

} // namespace f4::world
