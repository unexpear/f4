#include "engine/world/boundary_discovery.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f4::world {

auto discoverTerrainBoundaries(const RegionMap& regions, TerrainFootprint f, BoundaryDiscoveryLimits limits,
                               std::optional<std::pair<PolicyKind, PolicyKind>> scope)
    -> Result<std::vector<StreamingBoundaryRequest>, BoundaryDiscoveryError> {
    constexpr double low = std::numeric_limits<std::int32_t>::min();
    constexpr double high = double(std::numeric_limits<std::int32_t>::max()) + 1;
    if (!std::isfinite(f.minX) || !std::isfinite(f.maxX) || !std::isfinite(f.minZ) || !std::isfinite(f.maxZ) ||
        f.minX >= f.maxX || f.minZ >= f.maxZ || f.minX < low || f.minZ < low || f.maxX > high || f.maxZ > high)
        return BoundaryDiscoveryError::InvalidWindow;
    if (!limits.maxTiles || limits.maxTiles > 4096 || !limits.maxBoundaries || limits.maxBoundaries > 256)
        return BoundaryDiscoveryError::InvalidLimits;
    if (scope && (scope->first == scope->second ||
                  !((scope->first == PolicyKind::Height && scope->second == PolicyKind::Hex) ||
                    (scope->first == PolicyKind::Hex && scope->second == PolicyKind::Height))))
        return BoundaryDiscoveryError::UnsupportedTransition;
    const auto extent = regions.tileExtentMeters();
    const auto regionAt = [extent](double coordinate) {
        auto index = static_cast<std::int64_t>(std::floor(coordinate / extent));
        if (double(index * extent) > coordinate) --index;
        return index;
    };
    const auto x0 = regionAt(f.minX), z0 = regionAt(f.minZ);
    auto x1 = regionAt(f.maxX), z1 = regionAt(f.maxZ);
    if (double(x1 * extent) == f.maxX) --x1;
    if (double(z1 * extent) == f.maxZ) --z1;
    const auto nx = static_cast<std::size_t>(x1 - x0 + 1), nz = static_cast<std::size_t>(z1 - z0 + 1);
    if (nx > limits.maxTiles || nz > limits.maxTiles / nx) return BoundaryDiscoveryError::LimitExceeded;
    std::vector<PolicyKind> policies;
    policies.reserve(nx * nz);
    for (auto z = z0; z <= z1; ++z) {
        for (auto x = x0; x <= x1; ++x) {
            policies.push_back(regions.policyAt(static_cast<std::int32_t>(std::max(low, double(x * extent))),
                                               static_cast<std::int32_t>(std::max(low, double(z * extent)))));
        }
    }
    std::vector<StreamingBoundaryRequest> result;
    const auto append = [&](TerrainBoundaryLine line, PolicyKind negative, PolicyKind positive)
        -> std::optional<BoundaryDiscoveryError> {
        if (negative == positive) return {};
        const bool supported = (negative == PolicyKind::Height && positive == PolicyKind::Hex) ||
                               (negative == PolicyKind::Hex && positive == PolicyKind::Height);
        if (!supported) return BoundaryDiscoveryError::UnsupportedTransition;
        if (scope && !((negative == scope->first && positive == scope->second) ||
                       (negative == scope->second && positive == scope->first))) return {};
        if (result.size() == limits.maxBoundaries) return BoundaryDiscoveryError::LimitExceeded;
        result.push_back({line, negative, positive});
        return {};
    };
    for (std::size_t z = 0; z < nz; ++z) {
        for (std::size_t x = 0; x < nx; ++x) {
            const auto tileX = x0 + static_cast<std::int64_t>(x), tileZ = z0 + static_cast<std::int64_t>(z);
            const auto policy = policies[z * nx + x];
            if (x + 1 < nx) {
                const auto error = append({TerrainBoundaryAxis::X, double((tileX + 1) * extent),
                    std::max(f.minZ, double(tileZ * extent)), std::min(f.maxZ, double((tileZ + 1) * extent))},
                    policy, policies[z * nx + x + 1]);
                if (error) return *error;
            }
            if (z + 1 < nz) {
                const auto error = append({TerrainBoundaryAxis::Z, double((tileZ + 1) * extent),
                    std::max(f.minX, double(tileX * extent)), std::min(f.maxX, double((tileX + 1) * extent))},
                    policy, policies[(z + 1) * nx + x]);
                if (error) return *error;
            }
        }
    }
    return result;
}

auto policyPairCompositionWindow(const RegionMap& regions, TerrainFootprint mesh, PolicyKind first,
                                 PolicyKind second) -> TerrainFootprint {
    constexpr double low = std::numeric_limits<std::int32_t>::min();
    constexpr double high = double(std::numeric_limits<std::int32_t>::max()) + 1;
    const auto extent = regions.tileExtentMeters();
    if (extent <= 0 || first == second || !std::isfinite(mesh.minX) || !std::isfinite(mesh.maxX) ||
        !std::isfinite(mesh.minZ) || !std::isfinite(mesh.maxZ) || mesh.minX >= mesh.maxX || mesh.minZ >= mesh.maxZ ||
        mesh.minX < low || mesh.minZ < low || mesh.maxX > high || mesh.maxZ > high)
        return mesh;
    const auto regionAt = [extent](double coordinate) {
        auto index = static_cast<std::int64_t>(std::floor(coordinate / extent));
        if (double(index * extent) > coordinate) --index;
        return index;
    };
    const auto x0 = regionAt(mesh.minX), z0 = regionAt(mesh.minZ);
    auto x1 = regionAt(mesh.maxX), z1 = regionAt(mesh.maxZ);
    if (double(x1 * extent) == mesh.maxX) --x1;
    if (double(z1 * extent) == mesh.maxZ) --z1;
    if (x1 < x0 || z1 < z0) return mesh;
    const auto nx = x1 - x0 + 1, nz = z1 - z0 + 1;
    if (nx <= 0 || nz <= 0 || nx > 4096 || nz > 4096 / nx) return mesh;
    const auto policyOf = [&](std::int64_t tileX, std::int64_t tileZ) {
        return regions.policyAt(static_cast<std::int32_t>(std::max(low, double(tileX * extent))),
                                static_cast<std::int32_t>(std::max(low, double(tileZ * extent))));
    };
    const auto pairEdge = [&](PolicyKind negative, PolicyKind positive) {
        return (negative == first && positive == second) || (negative == second && positive == first);
    };
    bool any = false;
    std::int64_t tx0 = 0, tx1 = 0, tz0 = 0, tz1 = 0;
    const auto include = [&](std::int64_t tileX, std::int64_t tileZ) {
        if (!any) { tx0 = tx1 = tileX; tz0 = tz1 = tileZ; any = true; return; }
        tx0 = std::min(tx0, tileX); tx1 = std::max(tx1, tileX);
        tz0 = std::min(tz0, tileZ); tz1 = std::max(tz1, tileZ);
    };
    for (auto z = z0; z <= z1; ++z) {
        for (auto x = x0; x <= x1; ++x) {
            const auto policy = policyOf(x, z);
            if (x + 1 <= x1 && pairEdge(policy, policyOf(x + 1, z))) { include(x, z); include(x + 1, z); }
            if (z + 1 <= z1 && pairEdge(policy, policyOf(x, z + 1))) { include(x, z); include(x, z + 1); }
        }
    }
    if (!any) {
        for (auto z = z0; z <= z1 && !any; ++z) {
            for (auto x = x0; x <= x1; ++x) {
                const auto policy = policyOf(x, z);
                if (policy == first || policy == second) { include(x, z); break; }
            }
        }
    }
    if (!any) return mesh;
    return {double(tx0 * extent), double((tx1 + 1) * extent), double(tz0 * extent), double((tz1 + 1) * extent)};
}
} // namespace f4::world
