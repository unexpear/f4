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

// Enumerate every Height/Hex ownership edge strictly inside the supplied XZ
// window, clipped to its tangent range. Split at every region-grid vertex so
// junctions only meet at endpoints. Deterministic Z/X tile order, X edge first.
// An empty success means no transition INSIDE this window, not source coverage.
// This pure snapshot operation neither examines residency nor selects a view.
// Unsupported policy pairs fail the entire result, never return a partial set.
// When scope is supplied, only that explicit policy pair is enumerated; this
// is a caller-selected composition scope, not a claim that other transitions
// in the window are supported or covered.
[[nodiscard]] auto discoverTerrainBoundaries(const RegionMap& regions, TerrainFootprint window,
                                            BoundaryDiscoveryLimits limits = {},
                                            std::optional<std::pair<PolicyKind, PolicyKind>> scope = {})
    -> Result<std::vector<StreamingBoundaryRequest>, BoundaryDiscoveryError>;

} // namespace f4::world
