#include <array>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>

#include "engine/world/terrain_boundaries.h"
#include "engine/world/terrain_queries.h"

namespace {
using namespace f4;
using namespace f4::world;
auto patch(float x0, float x1, float z0, float z1, float height, float slope = 0) -> MeshData {
    MeshData mesh;
    for (const auto p : {math::Vec3{x0, 0, z0}, math::Vec3{x0, 0, z1}, math::Vec3{x1, 0, z1}, math::Vec3{x1, 0, z0}})
        mesh.vertices.push({{p.x, height + slope * p.z, p.z}});
    for (auto i : {0u, 1u, 2u, 2u, 3u, 0u}) mesh.indices.push(i);
    return mesh;
}
} // namespace

TEST_CASE("Terrain boundaries: paired piecewise tops match triangle rays and signed translations") {
    for (const auto offset : {math::Vec3{}, math::Vec3{-64, 8, 128}}) {
        auto left = patch(-2, 0, 0, 4, 2, 0.5f);
        auto low = patch(0, 2, 0, 1, 5, -0.25f);
        auto high = patch(0, 2, 1, 4, 5, -0.25f);
        std::array negative{TerrainBoundaryMesh{&left, offset}};
        std::array positive{TerrainBoundaryMesh{&low, offset}, TerrainBoundaryMesh{&high, offset}};
        const auto profile = compareTerrainBoundaryTops(negative, positive, {TerrainBoundaryAxis::X, offset.x, offset.z, offset.z + 4});
        REQUIRE(profile.hasValue());
        REQUIRE(profile.value().size() == 2);
        CHECK(profile.value()[0].end == offset.z + 1);
        double length = 0;
        for (const auto& s : profile.value()) {
            REQUIRE(s.negative.has_value()); REQUIRE(s.positive.has_value());
            CHECK(s.negativeCount == 1); CHECK(s.positiveCount == 1);
            CHECK(s.negative->beginHeight == doctest::Approx(offset.y + 2 + (s.begin - offset.z) * 0.5));
            CHECK(s.negative->endHeight == doctest::Approx(offset.y + 2 + (s.end - offset.z) * 0.5));
            CHECK(s.positive->beginHeight == doctest::Approx(offset.y + 5 - (s.begin - offset.z) * 0.25));
            CHECK(s.positive->endHeight == doctest::Approx(offset.y + 5 - (s.end - offset.z) * 0.25));
            const float t = static_cast<float>((s.begin + s.end) * 0.5);
            for (const bool side : {false, true}) {
                const auto& sample = side ? *s.positive : *s.negative;
                const auto& source = side ? positive[sample.sourceIndex] : negative[sample.sourceIndex];
                const auto hit = raycastTerrainMesh(*source.mesh, {offset.x, offset.y + 20, t}, {0, -1, 0}, 30, offset, true);
                REQUIRE(hit.has_value());
                CHECK(hit->position.y == doctest::Approx((sample.beginHeight + sample.endHeight) * 0.5));
                CHECK(hit->triangleIndex == sample.triangleIndex);
            }
            length += s.end - s.begin;
        }
        CHECK(length == 4);
    }
}

TEST_CASE("Terrain boundaries: gaps and overlapping layers are explicit without extrapolation") {
    auto left = patch(-2, 0, 0, 4, 0);
    auto a = patch(0, 2, 1, 2, 2);
    auto b = patch(0, 2, 1.5f, 3, 4);
    std::array negative{TerrainBoundaryMesh{&left}};
    std::array positive{TerrainBoundaryMesh{&a}, TerrainBoundaryMesh{&b}};
    const auto profile = compareTerrainBoundaryTops(negative, positive, {TerrainBoundaryAxis::X, 0, 0, 4});
    REQUIRE(profile.hasValue()); REQUIRE(profile.value().size() == 5);
    const std::array<std::size_t, 5> counts{0, 1, 2, 1, 0};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        CHECK(profile.value()[i].positiveCount == counts[i]);
        CHECK(profile.value()[i].positive.has_value() == (counts[i] == 1));
        CHECK(profile.value()[i].negativeCount == 1);
    }
    // Identical duplicates are still ambiguous, not silently deduplicated.
    positive[1] = positive[0];
    const auto duplicate = compareTerrainBoundaryTops(negative, positive, {TerrainBoundaryAxis::X, 0, 1, 2});
    REQUIRE(duplicate.hasValue());
    CHECK(duplicate.value()[0].positiveCount == 2);
    CHECK_FALSE(duplicate.value()[0].positive);
    const auto empty = compareTerrainBoundaryTops({}, {}, {TerrainBoundaryAxis::X, 0, 0, 4});
    REQUIRE(empty.hasValue()); REQUIRE(empty.value().size() == 1);
    CHECK(empty.value()[0].begin == 0); CHECK(empty.value()[0].end == 4);
    CHECK(empty.value()[0].negativeCount == 0); CHECK(empty.value()[0].positiveCount == 0);
}

TEST_CASE("Terrain boundaries: interior cuts, Z axis, tangent contacts and geometric winding") {
    auto mesh = patch(-2, 2, -2, 2, 3, 0.5f);
    // Geometry, not smoothed vertex normals, defines a top.
    for (auto& v : mesh.vertices) v.normal = {0, -1, 0};
    std::array sources{TerrainBoundaryMesh{&mesh}};
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        const auto result = compareTerrainBoundaryTops(sources, sources, {axis, 0, -2, 2});
        REQUIRE(result.hasValue()); REQUIRE(result.value().size() == 2);
        CHECK(result.value()[0].end == 0); // Both triangle breakpoints retained.
        for (const auto& s : result.value()) {
            REQUIRE(s.negative); REQUIRE(s.positive);
            CHECK(s.negativeCount == 1); CHECK(s.positiveCount == 1);
            CHECK(s.negative->beginHeight == s.positive->beginHeight);
            CHECK(s.negative->endHeight == s.positive->endHeight);
        }
    }
    // No positive-side area at the right edge. No epsilon nudges into a triangle.
    const auto tangent = compareTerrainBoundaryTops({}, sources, {TerrainBoundaryAxis::X, 2, -2, 2});
    REQUIRE(tangent.hasValue()); CHECK(tangent.value()[0].positiveCount == 0);
    std::swap(mesh.indices[0], mesh.indices[1]); std::swap(mesh.indices[3], mesh.indices[4]);
    const auto underside = compareTerrainBoundaryTops(sources, {}, {TerrainBoundaryAxis::X, 0, -2, 2});
    REQUIRE(underside.hasValue()); CHECK(underside.value()[0].negativeCount == 0);
    for (auto& v : mesh.vertices) v.position.x = 0;
    const auto wall = compareTerrainBoundaryTops(sources, sources, {TerrainBoundaryAxis::X, 0, -2, 2});
    REQUIRE(wall.hasValue()); CHECK(wall.value()[0].negativeCount == 0); CHECK(wall.value()[0].positiveCount == 0);
}

TEST_CASE("Terrain boundaries: invalid input and output limits fail without partial results") {
    auto mesh = patch(-2, 2, -2, 2, 0);
    std::array sources{TerrainBoundaryMesh{&mesh}};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, -2, 2};
    CHECK(compareTerrainBoundaryTops(sources, {}, line, 1).error() == TerrainBoundaryError::SegmentLimitExceeded);
    CHECK(compareTerrainBoundaryTops(sources, sources, line, 3).error() == TerrainBoundaryError::SegmentLimitExceeded);
    CHECK(compareTerrainBoundaryTops(sources, sources, line, 4).hasValue());
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line, 0));
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line, 65537));
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, {TerrainBoundaryAxis::X, 0, 2, 2}));
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, {TerrainBoundaryAxis::X, INFINITY, 0, 2}));
    sources[0].translation.x = INFINITY;
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line));
    sources[0].translation = {};
    mesh.indices[0] = 999;
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line));
    mesh.indices[0] = 0; mesh.indices.push(0);
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line));
    mesh.indices.pop(); mesh.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line));
    sources[0].mesh = nullptr;
    CHECK_FALSE(compareTerrainBoundaryTops(sources, {}, line));
}

TEST_CASE("Terrain boundaries: nonplanar triangle profiles and subpixel gaps are not smoothed") {
    auto saddle = patch(-2, 2, -2, 2, 0);
    saddle.vertices[2].position.y = 4;
    std::array sources{TerrainBoundaryMesh{&saddle}};
    const auto result = compareTerrainBoundaryTops(sources, {}, {TerrainBoundaryAxis::X, 0, -2, 2});
    REQUIRE(result.hasValue()); REQUIRE(result.value().size() == 2);
    // This diagonal gives y=2 at the center, not the bilinear value y=1.
    REQUIRE(result.value()[0].negative); REQUIRE(result.value()[1].negative);
    CHECK(result.value()[0].negative->endHeight == 2);
    CHECK(result.value()[1].negative->beginHeight == 2);
    CHECK(result.value()[0].negative->beginHeight == 0);
    CHECK(result.value()[1].negative->endHeight == 2);

    const float afterOne = std::nextafter(1.f, 2.f);
    auto a = patch(0, 2, 0, 1, 3);
    auto b = patch(0, 2, afterOne, 2, 5);
    std::array positive{TerrainBoundaryMesh{&a}, TerrainBoundaryMesh{&b}};
    const auto gap = compareTerrainBoundaryTops({}, positive, {TerrainBoundaryAxis::X, 0, 0, 2});
    REQUIRE(gap.hasValue()); REQUIRE(gap.value().size() == 3);
    CHECK(gap.value()[1].begin == 1);
    CHECK(gap.value()[1].end == afterOne);
    CHECK(gap.value()[1].positiveCount == 0);
    CHECK_FALSE(gap.value()[1].positive);
    std::swap(positive[0], positive[1]);
    const auto reordered = compareTerrainBoundaryTops({}, positive, {TerrainBoundaryAxis::X, 0, 0, 2});
    REQUIRE(reordered.hasValue()); REQUIRE(reordered.value().size() == gap.value().size());
    for (std::size_t i = 0; i < gap.value().size(); ++i) {
        const auto& before = gap.value()[i]; const auto& after = reordered.value()[i];
        CHECK(before.begin == after.begin); CHECK(before.end == after.end);
        CHECK(before.positiveCount == after.positiveCount);
        if (before.positive && after.positive) {
            CHECK(before.positive->beginHeight == after.positive->beginHeight);
            CHECK(before.positive->endHeight == after.positive->endHeight);
            CHECK(before.positive->sourceIndex == 1 - after.positive->sourceIndex);
        }
    }
}
