#include <cmath>
#include <limits>
#include <doctest/doctest.h>
#include "engine/world/boundary_discovery.h"

namespace {
using namespace f4::world;
void checkEdge(const StreamingBoundaryRequest& r, TerrainBoundaryAxis axis, double plane, double begin, double end,
               PolicyKind negative, PolicyKind positive) {
    CHECK(r.line.axis == axis); CHECK(r.line.plane == plane);
    CHECK(r.line.begin == begin); CHECK(r.line.end == end);
    CHECK(r.negative == negative); CHECK(r.positive == positive);
}
}

TEST_CASE("Boundary discovery: isolated tiles enumerate all four directed edges") {
    RegionMap map(64); map.set({0, 0}, PolicyKind::Hex);
    auto found = discoverTerrainBoundaries(map, {-32, 96, -32, 96});
    REQUIRE(found); REQUIRE(found.value().size() == 4);
    checkEdge(found.value()[0], TerrainBoundaryAxis::Z, 0, 0, 64, PolicyKind::Height, PolicyKind::Hex);
    checkEdge(found.value()[1], TerrainBoundaryAxis::X, 0, 0, 64, PolicyKind::Height, PolicyKind::Hex);
    checkEdge(found.value()[2], TerrainBoundaryAxis::X, 64, 0, 64, PolicyKind::Hex, PolicyKind::Height);
    checkEdge(found.value()[3], TerrainBoundaryAxis::Z, 64, 0, 64, PolicyKind::Hex, PolicyKind::Height);
    auto inside = discoverTerrainBoundaries(map, {0, 64, 0, 64});
    REQUIRE(inside); CHECK(inside.value().empty());
    auto clipped = discoverTerrainBoundaries(map, {32, 96, 12.5, 40.25});
    REQUIRE(clipped); REQUIRE(clipped.value().size() == 1);
    checkEdge(clipped.value()[0], TerrainBoundaryAxis::X, 64, 12.5, 40.25, PolicyKind::Hex, PolicyKind::Height);
}

TEST_CASE("Boundary discovery: grid crossings split into four endpoint arms") {
    RegionMap map(64); map.set({-1, -1}, PolicyKind::Hex); map.set({0, 0}, PolicyKind::Hex);
    auto found = discoverTerrainBoundaries(map, {-32, 32, -32, 32});
    REQUIRE(found); REQUIRE(found.value().size() == 4);
    checkEdge(found.value()[0], TerrainBoundaryAxis::X, 0, -32, 0, PolicyKind::Hex, PolicyKind::Height);
    checkEdge(found.value()[1], TerrainBoundaryAxis::Z, 0, -32, 0, PolicyKind::Hex, PolicyKind::Height);
    checkEdge(found.value()[2], TerrainBoundaryAxis::Z, 0, 0, 32, PolicyKind::Height, PolicyKind::Hex);
    checkEdge(found.value()[3], TerrainBoundaryAxis::X, 0, 0, 32, PolicyKind::Height, PolicyKind::Hex);
    RegionMap reordered(64); reordered.set({0, 0}, PolicyKind::Hex); reordered.set({-1, -1}, PolicyKind::Hex);
    auto again = discoverTerrainBoundaries(reordered, {-32, 32, -32, 32});
    REQUIRE(again); REQUIRE(again.value().size() == found.value().size());
    for (std::size_t i = 0; i < found.value().size(); ++i) {
        const auto& a = found.value()[i];
        checkEdge(again.value()[i], a.line.axis, a.line.plane, a.line.begin, a.line.end, a.negative, a.positive);
    }
}

TEST_CASE("Boundary discovery: limits and unsupported transitions never return partial success") {
    RegionMap map(64); map.set({0, 0}, PolicyKind::Hex);
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {8, 64}).error() == BoundaryDiscoveryError::LimitExceeded);
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {9, 3}).error() == BoundaryDiscoveryError::LimitExceeded);
    REQUIRE(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {9, 4}));
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {0, 64}).error() == BoundaryDiscoveryError::InvalidLimits);
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {4097, 64}).error() == BoundaryDiscoveryError::InvalidLimits);
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}, {9, 257}).error() == BoundaryDiscoveryError::InvalidLimits);
    map.set({1, 0}, PolicyKind::Voxel);
    CHECK(discoverTerrainBoundaries(map, {-32, 96, -32, 96}).error() == BoundaryDiscoveryError::UnsupportedTransition);
    auto uniformVoxel = discoverTerrainBoundaries(map, {64, 96, 0, 64});
    REQUIRE(uniformVoxel); CHECK(uniformVoxel.value().empty());
    RegionMap uniform(1);
    REQUIRE(discoverTerrainBoundaries(uniform, {0, 64, 0, 64}));
    CHECK(discoverTerrainBoundaries(uniform, {0, 65, 0, 64}).error() == BoundaryDiscoveryError::LimitExceeded);
}

TEST_CASE("Boundary discovery: scoped Height/Hex fails closed on unsupported edges") {
    RegionMap map(64); map.set({0, 0}, PolicyKind::Hex); map.set({1, 0}, PolicyKind::Voxel);
    const auto scope = std::pair{PolicyKind::Height, PolicyKind::Hex};
    const auto reversed = std::pair{PolicyKind::Hex, PolicyKind::Height};
    const auto onlyHeightHex = discoverTerrainBoundaries(map, {-32, 32, 0, 64}, {}, scope);
    REQUIRE(onlyHeightHex); REQUIRE(onlyHeightHex.value().size() == 1);
    checkEdge(onlyHeightHex.value()[0], TerrainBoundaryAxis::X, 0, 0, 64, PolicyKind::Height, PolicyKind::Hex);
    const auto reversedOnly = discoverTerrainBoundaries(map, {-32, 32, 0, 64}, {}, reversed);
    REQUIRE(reversedOnly); REQUIRE(reversedOnly.value().size() == 1);
    checkEdge(reversedOnly.value()[0], TerrainBoundaryAxis::X, 0, 0, 64, PolicyKind::Height, PolicyKind::Hex);
    CHECK(discoverTerrainBoundaries(map, {-64, 192, 0, 64}).error() == BoundaryDiscoveryError::UnsupportedTransition);
    CHECK(discoverTerrainBoundaries(map, {-64, 192, 0, 64}, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    CHECK(discoverTerrainBoundaries(map, {-64, 192, 0, 64}, {}, reversed).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    CHECK(discoverTerrainBoundaries(map, {-32, 32, 0, 64}, {},
                                    std::pair{PolicyKind::Height, PolicyKind::Voxel}).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    auto uniformVoxel = discoverTerrainBoundaries(map, {64, 128, 0, 64}, {}, scope);
    REQUIRE(uniformVoxel); CHECK(uniformVoxel.value().empty());
    auto perimeter = discoverTerrainBoundaries(map, {64, 128, 0, 64});
    REQUIRE(perimeter); CHECK(perimeter.value().empty());
}

TEST_CASE("Boundary discovery: pair window covers height/hex edges and still fails a foreign tile") {
    RegionMap corner(64); corner.set({0, 0}, PolicyKind::Hex); corner.set({-1, 0}, PolicyKind::Voxel);
    const TerrainFootprint mesh{-128, 128, -128, 128};
    const auto scope = std::pair{PolicyKind::Height, PolicyKind::Hex};
    CHECK(discoverTerrainBoundaries(corner, mesh, {}, scope).error() == BoundaryDiscoveryError::UnsupportedTransition);
    const auto window = policyPairCompositionWindow(corner, mesh, PolicyKind::Height, PolicyKind::Hex);
    const auto edges = discoverTerrainBoundaries(corner, window, {}, scope);
    REQUIRE(edges);
    bool east = false, north = false, south = false;
    for (const auto& edge : edges.value()) {
        const bool supported = (edge.negative == PolicyKind::Height && edge.positive == PolicyKind::Hex) ||
                               (edge.negative == PolicyKind::Hex && edge.positive == PolicyKind::Height);
        CHECK(supported);
        if (edge.line.axis == TerrainBoundaryAxis::X && edge.line.plane == 64) east = true;
        if (edge.line.axis == TerrainBoundaryAxis::Z && edge.line.plane == 64) north = true;
        if (edge.line.axis == TerrainBoundaryAxis::Z && edge.line.plane == 0) south = true;
    }
    CHECK(east); CHECK(north); CHECK(south);
    const TerrainFootprint containingVoxel{std::min(window.minX, -32.0), window.maxX, window.minZ, window.maxZ};
    CHECK(discoverTerrainBoundaries(corner, containingVoxel, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);

    RegionMap split(64); split.set({-2, 0}, PolicyKind::Hex); split.set({0, 0}, PolicyKind::Hex); split.set({-1, 0}, PolicyKind::Voxel);
    const auto splitWindow = policyPairCompositionWindow(split, mesh, PolicyKind::Height, PolicyKind::Hex);
    CHECK(discoverTerrainBoundaries(split, splitWindow, {}, scope).error() == BoundaryDiscoveryError::UnsupportedTransition);

    RegionMap heightBesideVoxel(64); heightBesideVoxel.set({-1, 0}, PolicyKind::Voxel);
    const auto uniform = policyPairCompositionWindow(heightBesideVoxel, {-64, 64, 0, 64}, PolicyKind::Height, PolicyKind::Hex);
    const auto noEdge = discoverTerrainBoundaries(heightBesideVoxel, uniform, {}, scope);
    REQUIRE(noEdge); CHECK(noEdge.value().empty());
    CHECK(discoverTerrainBoundaries(heightBesideVoxel, {-64, 64, 0, 64}, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
}

TEST_CASE("Boundary discovery: signed domain and adjacent floating endpoints are exact") {
    constexpr double low = std::numeric_limits<std::int32_t>::min();
    constexpr double high = double(std::numeric_limits<std::int32_t>::max()) + 1;
    RegionMap map(3); map.set({-1, -1}, PolicyKind::Hex);
    auto negative = discoverTerrainBoundaries(map, {-4, -1, -2, -1});
    REQUIRE(negative); REQUIRE(negative.value().size() == 1);
    checkEdge(negative.value()[0], TerrainBoundaryAxis::X, -3, -2, -1, PolicyKind::Height, PolicyKind::Hex);
    REQUIRE(discoverTerrainBoundaries(map, {low, low + 1, low, low + 1}));
    REQUIRE(discoverTerrainBoundaries(map, {high - 1, high, high - 1, high}));
    CHECK(discoverTerrainBoundaries(map, {low - 1, low + 1, 0, 1}).error() == BoundaryDiscoveryError::InvalidWindow);
    CHECK(discoverTerrainBoundaries(map, {0, high + 1, 0, 1}).error() == BoundaryDiscoveryError::InvalidWindow);
    CHECK(discoverTerrainBoundaries(map, {0, 0, 0, 1}).error() == BoundaryDiscoveryError::InvalidWindow);
    CHECK(discoverTerrainBoundaries(map, {0, 1, 0, std::numeric_limits<double>::infinity()}).error() == BoundaryDiscoveryError::InvalidWindow);
    CHECK(discoverTerrainBoundaries(map, {std::numeric_limits<double>::quiet_NaN(), 1, 0, 1}).error() == BoundaryDiscoveryError::InvalidWindow);
    RegionMap origin(3); origin.set({-1, 0}, PolicyKind::Hex);
    const auto tiny = std::nextafter(0.0, -1.0);
    auto nearZero = discoverTerrainBoundaries(origin, {tiny, 1, 0, 1});
    REQUIRE(nearZero); REQUIRE(nearZero.value().size() == 1);
    checkEdge(nearZero.value()[0], TerrainBoundaryAxis::X, 0, 0, 1, PolicyKind::Hex, PolicyKind::Height);
}

TEST_CASE("Boundary discovery: all two-policy neighborhoods preserve ownership and snapshot isolation") {
    for (unsigned mask = 0; mask < 512; ++mask) {
        RegionMap map(32);
        for (unsigned bit = 0; bit < 9; ++bit) {
            if (mask & (1u << bit)) map.set({static_cast<std::int32_t>(bit % 3) - 1,
                                           static_cast<std::int32_t>(bit / 3) - 1}, PolicyKind::Hex);
        }
        const RegionMap snapshot = map;
        map.set({0, 0}, PolicyKind::Voxel);
        auto found = discoverTerrainBoundaries(snapshot, {-32, 64, -32, 64});
        REQUIRE(found);
        std::size_t expected = 0;
        for (unsigned bit = 0; bit < 9; ++bit) {
            if (bit % 3 < 2 && ((mask >> bit) & 1u) != ((mask >> (bit + 1)) & 1u)) ++expected;
            if (bit / 3 < 2 && ((mask >> bit) & 1u) != ((mask >> (bit + 3)) & 1u)) ++expected;
        }
        REQUIRE(found.value().size() == expected);
        for (const auto& r : found.value()) {
            const auto p = static_cast<std::int32_t>(r.line.plane);
            const auto t = static_cast<std::int32_t>(r.line.begin);
            CHECK(r.line.end - r.line.begin == 32);
            if (r.line.axis == TerrainBoundaryAxis::X) {
                CHECK(snapshot.policyAt(p - 1, t) == r.negative);
                CHECK(snapshot.policyAt(p, t) == r.positive);
            } else {
                CHECK(snapshot.policyAt(t, p - 1) == r.negative);
                CHECK(snapshot.policyAt(t, p) == r.positive);
            }
        }
        bool unsupported = false;
        std::size_t edges = 0;
        const auto policyAt = [&](unsigned bit) {
            if (bit == 4) return PolicyKind::Voxel;
            return (mask & (1u << bit)) != 0 ? PolicyKind::Hex : PolicyKind::Height;
        };
        const auto consider = [&](unsigned a, unsigned b) {
            const auto left = policyAt(a);
            const auto right = policyAt(b);
            if (left == right) return;
            const bool voxelHex = (left == PolicyKind::Voxel && right == PolicyKind::Hex) ||
                                  (left == PolicyKind::Hex && right == PolicyKind::Voxel);
            const bool heightHex = (left == PolicyKind::Height && right == PolicyKind::Hex) ||
                                   (left == PolicyKind::Hex && right == PolicyKind::Height);
            if (!voxelHex && !heightHex) unsupported = true;
            else ++edges;
        };
        for (unsigned bit = 0; bit < 9; ++bit) {
            if (bit % 3 < 2) consider(bit, bit + 1);
            if (bit / 3 < 2) consider(bit, bit + 3);
        }
        const auto withVoxel = discoverTerrainBoundaries(map, {-32, 64, -32, 64});
        const auto heightHexScope = discoverTerrainBoundaries(map, {-32, 64, -32, 64}, {},
                                                              std::pair{PolicyKind::Height, PolicyKind::Hex});
        if (unsupported) {
            CHECK(withVoxel.error() == BoundaryDiscoveryError::UnsupportedTransition);
            CHECK(heightHexScope.error() == BoundaryDiscoveryError::UnsupportedTransition);
        } else {
            REQUIRE(withVoxel);
            CHECK(withVoxel.value().size() == edges);
            CHECK(heightHexScope.error() == BoundaryDiscoveryError::UnsupportedTransition);
        }
    }
}

TEST_CASE("Boundary discovery: a window that contains voxel/hex returns that edge") {
    RegionMap map(64);
    map.set({0, 0}, PolicyKind::Hex);
    map.set({1, 0}, PolicyKind::Voxel);
    const TerrainFootprint window{0, 128, 0, 64};
    const auto scope = std::pair{PolicyKind::Voxel, PolicyKind::Hex};
    const auto found = discoverTerrainBoundaries(map, window, {}, scope);
    REQUIRE(found);
    REQUIRE(found.value().size() == 1);
    checkEdge(found.value()[0], TerrainBoundaryAxis::X, 64, 0, 64, PolicyKind::Hex, PolicyKind::Voxel);
    const auto unscoped = discoverTerrainBoundaries(map, window);
    REQUIRE(unscoped);
    REQUIRE(unscoped.value().size() == 1);
    checkEdge(unscoped.value()[0], TerrainBoundaryAxis::X, 64, 0, 64, PolicyKind::Hex, PolicyKind::Voxel);
    CHECK(discoverTerrainBoundaries(map, window, {}, std::pair{PolicyKind::Height, PolicyKind::Hex}).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    map.set({1, 1}, PolicyKind::Height);
    CHECK(discoverTerrainBoundaries(map, {0, 128, 0, 128}, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    auto interior = discoverTerrainBoundaries(map, {64, 128, 0, 64}, {}, scope);
    REQUIRE(interior);
    CHECK(interior.value().empty());
}
