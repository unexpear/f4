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

TEST_CASE("Boundary discovery: explicit composition scope excludes unrelated policy edges") {
    RegionMap map(64); map.set({0, 0}, PolicyKind::Hex); map.set({1, 0}, PolicyKind::Voxel);
    CHECK(discoverTerrainBoundaries(map, {-64, 192, 0, 64}).error() == BoundaryDiscoveryError::UnsupportedTransition);
    const auto scoped = discoverTerrainBoundaries(map, {-64, 192, 0, 64}, {},
                                                    std::pair{PolicyKind::Height, PolicyKind::Hex});
    REQUIRE(scoped); REQUIRE(scoped.value().size() == 1);
    checkEdge(scoped.value()[0], TerrainBoundaryAxis::X, 0, 0, 64, PolicyKind::Height, PolicyKind::Hex);
    CHECK(discoverTerrainBoundaries(map, {-64, 192, 0, 64}, {},
                                    std::pair{PolicyKind::Height, PolicyKind::Voxel}).error() ==
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
        CHECK(discoverTerrainBoundaries(map, {-32, 64, -32, 64}).error() == BoundaryDiscoveryError::UnsupportedTransition);
    }
}
