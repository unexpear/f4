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
    // Required for Hex, forbidden for Height. A voxel stack is not one top;
    // classifyVoxelColumn owns its published solid spans.
    std::optional<double> floor;
};

// One closed solid run of a voxel column, bottom then top. Air between runs
// is empty. The runs are the published stack, including a cave or overhang.
struct VoxelSolidSpan {
    double floor = 0;
    double top = 0;
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
// Never returns an owner. This is not a streaming column walker.
[[nodiscard]] auto classifyVoxelStack(std::span<const bool> solidFromBottom)
    -> Result<TerrainSolidSample, TerrainSolidError>;

// Published voxel column. Spans are closed, ordered, and non-overlapping.
// A sample inside a span is solid and owned by Voxel. A sample outside every
// span is empty and still names Voxel. An empty span list is an all-air column.
[[nodiscard]] auto classifyVoxelColumn(std::span<const VoxelSolidSpan> spans, double height)
    -> Result<TerrainSolidSample, TerrainSolidError>;

// One region plane. The voxel side is the published closed spans. The other
// side is one Height column, open downward, or one Hex prism. Containing
// columns compete by the greatest top; a voxel sample uses the top of the
// span that contains it. Equal tops keep the negative side (sector 0). The
// wall band runs from the greatest strictly shorter incident top through the
// owner's top. With one other column that is the same band as a Height/Hex
// boundary. Neither side containing the sample is an empty success. This does
// not read meshes and does not turn a voxel stack into one top-and-floor.
struct VoxelPlaneSolidQuery {
    bool voxelOnNegative = true;
    std::span<const VoxelSolidSpan> spans;
    PolicyKind other = PolicyKind::Height;
    double otherTop = 0;
    std::optional<double> otherFloor;
};
[[nodiscard]] auto classifyVoxelPlaneSolid(const VoxelPlaneSolidQuery& query, double height)
    -> Result<TerrainSolidSample, TerrainSolidError>;

} // namespace f4::world
