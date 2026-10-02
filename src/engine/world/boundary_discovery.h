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
// An empty success means no supported transition INSIDE this window, not source
// coverage. This pure snapshot operation neither examines residency nor selects
// a view. An unsupported policy pair inside the window fails the entire result
// and never returns a partial set, whether or not the caller passed a scope.
// A scope enumerates only that supported pair. It does not omit an unsupported
// edge, and it is not coverage of anything outside the window.
[[nodiscard]] auto discoverTerrainBoundaries(const RegionMap& regions, TerrainFootprint window,
                                            BoundaryDiscoveryLimits limits = {},
                                            std::optional<std::pair<PolicyKind, PolicyKind>> scope = {})
    -> Result<std::vector<StreamingBoundaryRequest>, BoundaryDiscoveryError>;

} // namespace f4::world
