#pragma once

#include <optional>
#include <utility>
#include <vector>
#include "engine/world/terrain_regions.h"
#include "engine/world/world_streaming.h"

namespace f4::world {

struct BoundaryDiscoveryLimits {
    std::size_t maxTiles = 4096; // 1..4096 ownership samples.
    std::size_t maxBoundaries = 64; // 1..256; never silently truncate.
};
enum class BoundaryDiscoveryError {
    InvalidWindow,
    InvalidLimits,
    LimitExceeded,
    UnsupportedTransition,
    NoResidentCoverage,
};

// Enumerate Height/Hex and Voxel/Hex ownership edges strictly inside the
// supplied XZ window, clipped to its tangent range. Split at every region-grid
// vertex so junctions only meet at endpoints. Deterministic Z/X tile order,
// X edge first. An empty success means no such edge INSIDE this window, not
// source coverage. This pure snapshot operation neither examines residency nor
// selects a view. Any other policy pair inside the window, including
// Height/Voxel, fails the entire result and is never omitted. A scope
// enumerates only that pair and fails the window if the other enumerated pair
// is inside it. It does not omit that edge, and it is not coverage outside
// the window. A returned Voxel/Hex edge is not a voxel column owner.
[[nodiscard]] auto discoverTerrainBoundaries(const RegionMap& regions, TerrainFootprint window,
                                            BoundaryDiscoveryLimits limits = {},
                                            std::optional<std::pair<PolicyKind, PolicyKind>> scope = {})
    -> Result<std::vector<StreamingBoundaryRequest>, BoundaryDiscoveryError>;

// Rectangle of tiles that share a resident edge of this policy pair inside
// meshBounds. A foreign tile in that rectangle is left inside it so discovery
// fails closed. When the rectangle contains no pair edge, one in-pair tile is
// returned so a uniform interior stays an empty success. meshBounds itself is
// returned when it names no usable tile.
[[nodiscard]] auto policyPairCompositionWindow(const RegionMap& regions, TerrainFootprint meshBounds,
                                               PolicyKind first, PolicyKind second) -> TerrainFootprint;

} // namespace f4::world
