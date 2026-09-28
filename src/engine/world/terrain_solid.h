#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "engine/world/terrain_boundaries.h"

namespace f4::world {

// Reference volume/collision ownership for the columns this tree can support.
// Not a surface ray, not voxel DDA, and not bilinear height sampling. Those
// APIs stay unchanged. Callers supply the columns; this function does not read
// WorldStreamer residency or published meshes.
//
// Height columns are closed at the top and open downward. The top is a
// published-triangle height, which differs from bilinear sampling on a
// nonplanar quad. Hex columns are closed prisms [floor, top] with a finite
// floor strictly below the top. On a boundary or corner, every incident column
// that contains the sample competes: the greatest top wins, and equal tops
// keep the canonical sector (negative, then positive; NE, NW, SW, SE). The
// wall band is the closed interval from the lowest strictly shorter incident
// top through the owner's top. That is the higher-side transition, including
// the lower edge of meshTerrainBoundaryWalls.
//
// Positive-area transition triangles already come from complete paired
// profiles. A shared breakpoint whose endpoint heights differ has no positive
// area, so a sample there is DiscontinuousHeight rather than a guessed column.
// Unsupported intersections return an error and no owner.

enum class TerrainSolidError : std::uint8_t {
    InvalidData,
    IncompleteProfile,
    PrecisionLoss,
    DiscontinuousHeight,
    NonmanifoldCorner,
    InconsistentSector,
    HexFloorInversion,
    UnsupportedUnderside,
    UnsupportedVoxel,
    VoxelCavity,
    VoxelOverhang,
};

struct TerrainSolidSample {
    bool solid = false;
    PolicyKind policy = PolicyKind::Height;
    std::uint8_t sector = 0;
    bool wallBand = false;
};

struct TerrainSolidColumn {
    PolicyKind policy = PolicyKind::Height;
    double top = 0;
    // Required for Hex, forbidden for Height. Voxel is never a column.
    std::optional<double> floor;
};

struct TerrainBoundarySolidQuery {
    TerrainBoundaryLine line;
    std::span<const TerrainBoundarySpan> profile;
    PolicyKind negative = PolicyKind::Height;
    PolicyKind positive = PolicyKind::Hex;
    std::optional<double> negativeFloor;
    std::optional<double> positiveFloor;
    double tangent = 0;
    double height = 0;
};

// Interior column, off any boundary plane. Empty above a Height top and
// outside a Hex prism is a successful non-solid sample.
[[nodiscard]] auto classifyTerrainColumn(TerrainSolidColumn column, double height)
    -> Result<TerrainSolidSample, TerrainSolidError>;

// Point on a boundary plane. `tangent` is world Z on an X plane and world X
// on a Z plane. The plane coordinate is `line.plane`; this does not test
// horizontal distance off the plane.
[[nodiscard]] auto classifyTerrainBoundarySolid(const TerrainBoundarySolidQuery& query)
    -> Result<TerrainSolidSample, TerrainSolidError>;

// Sectors are NE, NW, SW, SE. Alternating solid/empty bands, including bands
// opened by finite hex floors, are NonmanifoldCorner for the whole corner.
[[nodiscard]] auto classifyTerrainCornerSolid(std::span<const TerrainSolidColumn, 4> sectors, double height)
    -> Result<TerrainSolidSample, TerrainSolidError>;

// Directions match boundary junctions: 0 E, 1 N, 2 W, 3 S. Missing arms must
// continue one column height. Conflicting sector heights are InconsistentSector.
// Distinct heights that collapse to one float are PrecisionLoss.
struct TerrainCornerArm {
    unsigned direction = 0;
    double negativeHeight = 0;
    double positiveHeight = 0;
};
[[nodiscard]] auto resolveTerrainCornerHeights(std::span<const TerrainCornerArm> arms)
    -> Result<std::array<double, 4>, TerrainSolidError>;

// Upward-triangle height at an XZ point inside the closed triangle. Downward
// winding is UnsupportedUnderside. This is the nonplanar top used by columns;
// it is not the bilinear height sample.
struct TerrainTopSample {
    double x = 0, y = 0, z = 0;
};
[[nodiscard]] auto sampleUpwardTriangleHeight(const TerrainTopSample& a, const TerrainTopSample& b,
                                              const TerrainTopSample& c, double x, double z)
    -> Result<double, TerrainSolidError>;

// Names a vertical voxel stack and always fails closed. Bottom to top.
// Two or more solid runs: VoxelOverhang. One run that starts above an air
// cell: VoxelCavity. A bottom-anchored run, or no solid cells: UnsupportedVoxel.
// No occupancy result is returned.
[[nodiscard]] auto classifyVoxelStack(std::span<const bool> solidFromBottom)
    -> Result<TerrainSolidSample, TerrainSolidError>;

} // namespace f4::world
