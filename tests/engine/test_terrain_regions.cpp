#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include "engine/world/terrain_regions.h"
#include "engine/world/terrain_queries.h"
#include "engine/world/terrain_boundary_walls.h"

namespace {
using namespace f4;
using namespace f4::world;

auto boundaryFace(TerrainBoundaryAxis axis) -> MeshData {
    MeshData mesh;
    for (const auto p : {math::Vec2{0, 0}, math::Vec2{0, 4}, math::Vec2{4, 4}, math::Vec2{4, 0}}) {
        TerrainVertex v;
        v.position = axis == TerrainBoundaryAxis::X ? math::Vec3{0, p.y, p.x} : math::Vec3{p.x, p.y, 0};
        v.normal = axis == TerrainBoundaryAxis::X ? math::Vec3{1, 0, 0} : math::Vec3{0, 0, -1};
        v.uv = p; v.material = 9; v.biome = 7;
        v.materialWeights = p.x == 0 ? std::array<std::uint8_t, 4>{255, 0, 0, 0} : std::array<std::uint8_t, 4>{0, 255, 0, 0};
        mesh.vertices.push(v);
    }
    for (const auto i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push(i);
    mesh.surfaces.push({0, 6, 19});
    return mesh;
}

auto wallArea(const MeshData& mesh, TerrainBoundaryAxis axis) -> double {
    const auto t = [&](math::Vec3 p) { return axis == TerrainBoundaryAxis::X ? p.z : p.x; };
    double area = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i + 1]].position,
                   c = mesh.vertices[mesh.indices[i + 2]].position;
        const double signedArea = (double(b.y) - a.y) * (double(t(c)) - t(a)) - (double(t(b)) - t(a)) * (double(c.y) - a.y);
        CHECK(signedArea > 0);
        area += signedArea * 0.5;
    }
    return area;
}

auto plane() -> MeshData {
    MeshData mesh;
    for (const auto p : {math::Vec3{-2, 0, -2}, math::Vec3{-2, 0, 2}, math::Vec3{2, 0, 2}, math::Vec3{2, 0, -2}}) {
        TerrainVertex v;
        v.position = {p.x, 2 + p.x * 0.25f + p.z * 0.5f, p.z};
        v.uv = {p.x, p.z};
        v.material = 9;
        v.materialWeights = p.x < 0 ? std::array<std::uint8_t, 4>{255, 0, 0, 0} : std::array<std::uint8_t, 4>{0, 255, 0, 0};
        mesh.vertices.push(v);
    }
    for (auto i : {0u, 1u, 2u, 2u, 3u, 0u}) mesh.indices.push(i);
    mesh.surfaces.push({0, 6, 9});
    return mesh;
}

auto projectedArea(const MeshData& mesh) -> double {
    double sum = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto a = mesh.vertices[mesh.indices[i]].position;
        const auto b = mesh.vertices[mesh.indices[i + 1]].position;
        const auto c = mesh.vertices[mesh.indices[i + 2]].position;
        const double signedArea = (double(b.z) - a.z) * (double(c.x) - a.x) - (double(b.x) - a.x) * (double(c.z) - a.z);
        CHECK(signedArea > 0); // Original upward winding survives every cut.
        sum += signedArea * 0.5;
    }
    return sum;
}
} // namespace

TEST_CASE("Boundary stitch: sorted canonical edge cuts preserve top winding, attributes and provenance") {
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        for (const auto offset : {math::Vec3{}, math::Vec3{-64, 8, 128}}) {
            auto source = plane();
            const double fixed = (axis == TerrainBoundaryAxis::X ? offset.x : offset.z) + 2;
            const double tangent = axis == TerrainBoundaryAxis::X ? offset.z : offset.x;
            // Right (+X) edge belongs to triangle 1; upper (+Z) edge to triangle 0.
            const std::size_t id = axis == TerrainBoundaryAxis::X ? 1 : 0;
            const auto height = [&](double t) { return offset.y + (axis == TerrainBoundaryAxis::X ? 2.5 + t * 0.5 : 3 + t * 0.25); };
            const std::array cuts{TerrainBoundaryTopCut{id, tangent + 1, height(1)}, TerrainBoundaryTopCut{id, tangent - 1, height(-1)},
                                 TerrainBoundaryTopCut{id, tangent, height(0)}, TerrainBoundaryTopCut{id, tangent, height(0)}};
            const auto result = stitchTerrainBoundaryTops(source, offset, {axis, fixed, tangent - 2, tangent + 2}, cuts);
            REQUIRE(result.hasValue()); CHECK(result.value().splitTriangles == 1);
            CHECK(result.value().mesh.vertices.size() == source.vertices.size() + 3);
            CHECK(result.value().mesh.indices.size() == 15);
            CHECK(projectedArea(result.value().mesh) == doctest::Approx(16));
            for (const auto& v : result.value().mesh.vertices) {
                CHECK(v.uv.x == v.position.x); CHECK(v.uv.y == v.position.z);
                CHECK(v.position.y == doctest::Approx(2 + v.position.x * 0.25 + v.position.z * 0.5));
                CHECK(v.material == 9);
                CHECK(unsigned(v.materialWeights[0]) + v.materialWeights[1] + v.materialWeights[2] + v.materialWeights[3] == 255);
            }
            CHECK(std::count(result.value().sourceTriangles.begin(), result.value().sourceTriangles.end(), id) == 4);
            auto reversed = cuts; std::reverse(reversed.begin(), reversed.end());
            const auto again = stitchTerrainBoundaryTops(source, offset, {axis, fixed, tangent - 2, tangent + 2}, reversed);
            REQUIRE(again.hasValue());
            for (std::size_t i = 0; i < result.value().mesh.indices.size(); ++i) CHECK(again.value().mesh.indices[i] == result.value().mesh.indices[i]);
            for (const float t : {-1.5f, -0.5f, 0.5f, 1.5f}) {
                const auto eye = offset + (axis == TerrainBoundaryAxis::X ? math::Vec3{2, 10, t} : math::Vec3{t, 10, 2});
                const auto hit = raycastTerrainMesh(result.value().mesh, eye, {0, -1, 0}, 20, offset, true);
                REQUIRE(hit); CHECK(hit->position.y == doctest::Approx(height(t)));
            }
        }
    }
}

TEST_CASE("Boundary stitch: wall crossing records produce identical vertices on both source tops") {
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        const auto top = [&](bool positive) {
            MeshData mesh;
            const float normalCoordinate = positive ? 2.f : -2.f;
            const std::array points{math::Vec3{0, positive ? 3.f : 0.f, -2},
                                    math::Vec3{0, positive ? 1.f : 4.f, 2}, math::Vec3{normalCoordinate, 2, 0}};
            for (auto p : points) { if (axis == TerrainBoundaryAxis::Z) std::swap(p.x, p.z); mesh.vertices.push({p}); }
            const bool reverse = positive == (axis == TerrainBoundaryAxis::Z);
            for (const auto i : {reverse ? 1u : 0u, reverse ? 0u : 1u, 2u}) mesh.indices.push(i);
            return mesh;
        };
        const auto negative = top(false), positive = top(true);
        const std::array negativeViews{TerrainBoundaryMesh{&negative}}, positiveViews{TerrainBoundaryMesh{&positive}};
        const TerrainBoundaryLine line{axis, 0, -2, 2};
        const auto profile = compareTerrainBoundaryTops(negativeViews, positiveViews, line);
        REQUIRE(profile.hasValue()); REQUIRE(profile.value().size() == 1);
        const auto walls = meshTerrainBoundaryWalls({profile.value().begin(), profile.value().size()}, line);
        REQUIRE(walls.hasValue()); REQUIRE(walls.value().topEdges.size() == 4);
        for (const bool side : {false, true}) {
            std::vector<TerrainBoundaryTopCut> cuts;
            for (const auto& edge : walls.value().topEdges) {
                if (edge.positive != side) continue;
                cuts.push_back({0, edge.begin, edge.beginHeight}); cuts.push_back({0, edge.end, edge.endHeight});
            }
            const auto stitched = stitchTerrainBoundaryTops(side ? positive : negative, {}, line, cuts);
            REQUIRE(stitched.hasValue()); CHECK(stitched.value().splitTriangles == 1);
            REQUIRE(stitched.value().mesh.vertices.size() == 4); CHECK(stitched.value().mesh.indices.size() == 6);
            const auto crossing = stitched.value().mesh.vertices[3].position;
            CHECK(crossing.x == 0); CHECK(crossing.y == 2); CHECK(crossing.z == 0);
            CHECK(projectedArea(stitched.value().mesh) == doctest::Approx(4));
        }
    }
}

TEST_CASE("Boundary stitch: no-op endpoints, incompatible geometry and strict precision limits") {
    using Error = TerrainBoundaryStitchError;
    const auto source = plane();
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 2, -2, 2};
    const std::array endpoints{TerrainBoundaryTopCut{1, -2, 1.5}, TerrainBoundaryTopCut{1, 2, 3.5}};
    const auto unchanged = stitchTerrainBoundaryTops(source, {}, line, endpoints);
    REQUIRE(unchanged.hasValue()); CHECK(unchanged.value().splitTriangles == 0);
    CHECK(unchanged.value().mesh.vertices.size() == source.vertices.size());
    for (std::size_t i = 0; i < source.indices.size(); ++i) CHECK(unchanged.value().mesh.indices[i] == source.indices[i]);
    std::array cuts{TerrainBoundaryTopCut{1, 0, 2.5}};
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts, 3).hasValue());
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts, 2).error() == Error::TriangleLimitExceeded);
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts, 0).error() == Error::InvalidData);
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts, 131073).error() == Error::InvalidData);
    cuts[0].height = 3;
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts).error() == Error::PrecisionLoss);
    cuts[0].height = 2.5; cuts[0].triangleIndex = 100;
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts).error() == Error::InvalidData);
    cuts[0].triangleIndex = 0;
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts).error() == Error::UnsupportedEdge);
    cuts[0].triangleIndex = 1;
    CHECK(stitchTerrainBoundaryTops(source, {}, {line.axis, 0, -2, 2}, cuts).error() == Error::UnsupportedEdge);
    cuts[0].tangent = INFINITY;
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts).error() == Error::InvalidData);
    cuts[0] = {1, 2 - 1e-10, 3.5};
    CHECK(stitchTerrainBoundaryTops(source, {}, line, cuts).error() == Error::PrecisionLoss);
    auto bad = source; bad.indices[0] = 999;
    CHECK(stitchTerrainBoundaryTops(bad, {}, line, {}).error() == Error::InvalidData);
    bad = source; bad.surfaces[0].indexCount = 9;
    CHECK(stitchTerrainBoundaryTops(bad, {}, line, {}).error() == Error::InvalidData);
    bad = source; bad.vertices[0].materialWeights = {};
    CHECK(stitchTerrainBoundaryTops(bad, {}, line, {}).error() == Error::InvalidData);
}

TEST_CASE("Boundary trim: partial coplanar strips preserve winding, attributes, provenance and outside rays") {
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        for (const auto offset : {math::Vec3{}, math::Vec3{-64, 8, 128}}) {
            const auto source = boundaryFace(axis);
            const auto planeCoordinate = axis == TerrainBoundaryAxis::X ? offset.x : offset.z;
            const auto tangentOffset = axis == TerrainBoundaryAxis::X ? offset.z : offset.x;
            const auto cut = trimTerrainBoundaryFaces(source, offset, {axis, planeCoordinate, tangentOffset + 1, tangentOffset + 3});
            REQUIRE(cut.hasValue()); CHECK(cut.value().affectedTriangles == 2);
            const auto& mesh = cut.value().mesh;
            CHECK(wallArea(mesh, axis) == doctest::Approx(8));
            CHECK(cut.value().sourceTriangles.size() == mesh.indices.size() / 3);
            for (const auto id : cut.value().sourceTriangles) CHECK(id < 2);
            REQUIRE(mesh.surfaces.size() == 1); CHECK(mesh.surfaces[0].material == 19);
            CHECK(mesh.surfaces[0].indexCount == mesh.indices.size());
            for (std::size_t i = source.vertices.size(); i < mesh.vertices.size(); ++i) {
                const auto& v = mesh.vertices[i];
                CHECK(v.uv.x == (axis == TerrainBoundaryAxis::X ? v.position.z : v.position.x));
                CHECK(v.uv.y == v.position.y); CHECK(v.material == 9); CHECK(v.biome == 7);
                CHECK(unsigned(v.materialWeights[0]) + v.materialWeights[1] + v.materialWeights[2] + v.materialWeights[3] == 255);
                CHECK(v.materialWeights[1] == doctest::Approx(v.uv.x / 4 * 255).epsilon(0.01));
            }
            for (const float t : {0.5f, 2.f, 3.5f}) {
                const auto eye = offset + (axis == TerrainBoundaryAxis::X ? math::Vec3{2, 2, t} : math::Vec3{t, 2, -2});
                const auto direction = axis == TerrainBoundaryAxis::X ? math::Vec3{-1, 0, 0} : math::Vec3{0, 0, 1};
                CHECK(raycastTerrainMesh(mesh, eye, direction, 4, offset, true).has_value() == (t != 2));
                CHECK(raycastTerrainMesh(source, eye, direction, 4, offset, true).has_value());
            }
        }
    }
}

TEST_CASE("Boundary trim: untouched faces and no-op layouts survive while full strips are removed") {
    const auto axis = TerrainBoundaryAxis::X;
    auto mesh = boundaryFace(axis);
    const auto unchanged = trimTerrainBoundaryFaces(mesh, {}, {axis, 0, 4, 8});
    REQUIRE(unchanged.hasValue()); CHECK(unchanged.value().affectedTriangles == 0);
    CHECK(unchanged.value().mesh.vertices.size() == mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) CHECK(unchanged.value().mesh.indices[i] == mesh.indices[i]);
    const auto empty = trimTerrainBoundaryFaces(mesh, {}, {axis, 0, 0, 4});
    REQUIRE(empty.hasValue()); CHECK(empty.value().mesh.indices.isEmpty()); CHECK(empty.value().mesh.surfaces.isEmpty());
    // A different plane is never selected by a near-plane epsilon.
    CHECK(trimTerrainBoundaryFaces(mesh, {}, {axis, std::nextafter(0.f, 1.f), 0, 4}).value().affectedTriangles == 0);
    mesh.vertices.push({{0, 4, 0}}); mesh.vertices.push({{1, 4, 0}}); mesh.vertices.push({{0, 4, 4}});
    for (const auto i : {4u, 5u, 6u}) mesh.indices.push(i);
    mesh.surfaces.push({6, 3, 27});
    const auto retained = trimTerrainBoundaryFaces(mesh, {}, {axis, 0, 0, 4});
    REQUIRE(retained.hasValue()); REQUIRE(retained.value().mesh.indices.size() == 3);
    CHECK(retained.value().mesh.indices[0] == 4); CHECK(retained.value().mesh.indices[1] == 5); CHECK(retained.value().mesh.indices[2] == 6);
    CHECK(retained.value().sourceTriangles[0] == 2); CHECK(retained.value().mesh.surfaces[0].material == 27);
    std::swap(mesh.indices[0], mesh.indices[1]); std::swap(mesh.indices[3], mesh.indices[4]);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, {axis, 0, 0, 4}).value().affectedTriangles == 2);
}

TEST_CASE("Boundary trim: malformed input, local precision and output limits are explicit failures") {
    using Error = TerrainBoundaryTrimError;
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, 1, 3};
    auto mesh = boundaryFace(line.axis);
    const auto reference = trimTerrainBoundaryFaces(mesh, {}, line); REQUIRE(reference.hasValue());
    const auto count = reference.value().sourceTriangles.size(); REQUIRE(count > 1);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line, count).hasValue());
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line, count - 1).error() == Error::TriangleLimitExceeded);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line, 0).error() == Error::InvalidData);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line, 131073).error() == Error::InvalidData);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, {line.axis, 0, 1, 1}).error() == Error::InvalidData);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, {line.axis, 0, 0.1, 3}).error() == Error::PrecisionLoss);
    CHECK(trimTerrainBoundaryFaces(mesh, {}, {line.axis, INFINITY, 1, 3}).error() == Error::InvalidData);
    CHECK(trimTerrainBoundaryFaces(mesh, {0, INFINITY, 0}, line).error() == Error::InvalidData);
    mesh.indices[0] = 100;
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line).error() == Error::InvalidData);
    mesh.indices[0] = 0; mesh.surfaces[0].indexCount = 9;
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line).error() == Error::InvalidData);
    mesh.surfaces[0].indexCount = 6; mesh.vertices[0].materialWeights = {};
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line).error() == Error::InvalidData);
    mesh.vertices[0].materialWeights = {255, 0, 0, 0}; mesh.vertices[0].position.y = INFINITY;
    CHECK(trimTerrainBoundaryFaces(mesh, {}, line).error() == Error::InvalidData);
    MeshData sliver;
    sliver.vertices.push({{0, 1, 0}});
    sliver.vertices.push({{0, std::nextafter(1.f, 2.f), 4}});
    sliver.vertices.push({{0, 1, 4}});
    for (const auto i : {0u, 1u, 2u}) sliver.indices.push(i);
    CHECK(trimTerrainBoundaryFaces(sliver, {}, line).error() == Error::PrecisionLoss);
}

TEST_CASE("Terrain regions: signed masks, closed geometry and interpolated attributes") {
    for (const math::Vec3 offset : {math::Vec3{}, math::Vec3{-64, 3, 128}}) {
        RegionMap regions(2, PolicyKind::Height);
        const auto region = regions.regionFor(static_cast<std::int32_t>(offset.x), static_cast<std::int32_t>(offset.z));
        regions.set(region, PolicyKind::Hex);
        const auto source = plane();
        auto clipped = clipTerrainRegions(source, offset, regions, PolicyKind::Hex);
        REQUIRE(clipped.hasValue());
        const auto& mesh = clipped.value();
        CHECK(projectedArea(mesh) == doctest::Approx(4));
        REQUIRE(mesh.surfaces.size() == 1);
        CHECK(mesh.surfaces[0].material == 9);
        CHECK(mesh.surfaces[0].indexCount == mesh.indices.size());
        for (const auto& v : mesh.vertices) {
            CHECK(v.position.x >= 0); CHECK(v.position.x <= 2);
            CHECK(v.position.z >= 0); CHECK(v.position.z <= 2);
            CHECK(v.position.y == doctest::Approx(2 + v.position.x * 0.25 + v.position.z * 0.5));
            CHECK(v.uv.x == v.position.x); CHECK(v.uv.y == v.position.z);
            CHECK(unsigned(v.materialWeights[0]) + v.materialWeights[1] + v.materialWeights[2] + v.materialWeights[3] == 255);
        }
        for (const float t : {0.f, 0.25f, 1.f, 2.f}) {
            auto hit = raycastTerrainMesh(mesh, offset + math::Vec3{t, 10, t}, {0, -1, 0}, 20, offset, true);
            REQUIRE(hit.has_value());
            CHECK(hit->position.y == doctest::Approx(offset.y + 2 + t * 0.75));
        }
        CHECK_FALSE(raycastTerrainMesh(mesh, offset + math::Vec3{-0.25f, 10, 1}, {0, -1, 0}, 20, offset));
        const auto remaining = clipTerrainRegions(source, offset, regions, PolicyKind::Height);
        REQUIRE(remaining.hasValue());
        CHECK(projectedArea(remaining.value()) == doctest::Approx(12));
        CHECK(projectedArea(mesh) + projectedArea(remaining.value()) == doctest::Approx(16));
        const auto resource = toResourceMesh(mesh);
        CHECK(resource.vertexCount() == mesh.vertices.size());
    }
}

TEST_CASE("Terrain regions: unchanged ownership, empty masks and bounded failures") {
    const auto source = plane();
    RegionMap regions(2, PolicyKind::Height);
    const auto same = clipTerrainRegions(source, {}, regions, PolicyKind::Height);
    REQUIRE(same.hasValue());
    CHECK(same.value().vertices.size() == source.vertices.size());
    CHECK(same.value().indices.size() == source.indices.size());
    for (std::size_t i = 0; i < source.indices.size(); ++i) CHECK(same.value().indices[i] == source.indices[i]);
    const auto empty = clipTerrainRegions(source, {}, regions, PolicyKind::Hex);
    REQUIRE(empty.hasValue());
    CHECK(empty.value().vertices.isEmpty()); CHECK(empty.value().indices.isEmpty()); CHECK(empty.value().surfaces.isEmpty());
    const auto exact = terrainRegionMask({-2, 0, -2, 0}, regions, PolicyKind::Height);
    REQUIRE(exact.hasValue());
    CHECK(exact.value().rectangles.size() == 1);
    CHECK(exact.value().rectangles[0].minX == -2);
    CHECK(terrainRegionMask({0, 10000, 0, 10000}, regions, PolicyKind::Height).error() == TerrainMeshError::RegionLimitExceeded);
    CHECK(terrainRegionMask({0, 1e20, 0, 1}, regions, PolicyKind::Height).error() == TerrainMeshError::CoordinateOverflow);
    CHECK(terrainRegionMask({0, 0, 0, 1}, regions, PolicyKind::Height).error() == TerrainMeshError::InvalidData);
    auto bad = source;
    bad.indices[0] = 999;
    CHECK_FALSE(clipTerrainRegions(bad, {}, regions, PolicyKind::Height));
    bad = source; bad.surfaces[0].firstIndex = 3;
    CHECK_FALSE(clipTerrainRegions(bad, {}, regions, PolicyKind::Height));
    bad = source; bad.vertices[0].materialWeights = {0, 0, 0, 0};
    CHECK_FALSE(clipTerrainRegions(bad, {}, regions, PolicyKind::Height));
    bad = source; bad.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(clipTerrainRegions(bad, {}, regions, PolicyKind::Height));
}

TEST_CASE("Terrain regions: coplanar internal walls are not duplicated") {
    auto source = plane();
    // Add a vertical face on x=0 shared by two owned negative-Z regions.
    for (auto p : {math::Vec3{0, 0, -2}, math::Vec3{0, 2, -2}, math::Vec3{0, 2, 0}}) source.vertices.push({p});
    for (auto i : {4u, 5u, 6u}) source.indices.push(i);
    source.surfaces[0].indexCount += 3;
    RegionMap regions(2, PolicyKind::Height);
    regions.set({0, 0}, PolicyKind::Hex);
    const auto clipped = clipTerrainRegions(source, {}, regions, PolicyKind::Height);
    REQUIRE(clipped.hasValue());
    unsigned vertical = 0;
    for (std::size_t i = 0; i < clipped.value().indices.size(); i += 3) {
        const auto& mesh = clipped.value();
        if (mesh.vertices[mesh.indices[i]].position.x == 0 && mesh.vertices[mesh.indices[i + 1]].position.x == 0 &&
            mesh.vertices[mesh.indices[i + 2]].position.x == 0) ++vertical;
    }
    CHECK(vertical == 1);
}

TEST_CASE("Terrain regions: hex cuts close to the actual prism floor with outward winding") {
    auto tile = generateHexTile(123, {}, {.radius = 2});
    for (int r = -2; r <= 2; ++r) {
        for (int q = -2; q <= 2; ++q) {
            if (!tile.contains({q, r})) continue;
            for (unsigned corner = 0; corner < 6; ++corner) tile.cornerHeights.set(hexCorner({q, r}, corner), 2.f);
        }
    }
    const auto built = meshHexChunk(tile, {}, {.radius = 4.f, .floorHeight = -8.f, .hasNeighbor = {}});
    REQUIRE(built.hasValue());
    for (const auto translation : {math::Vec3{}, math::Vec3{-64, 3, 128}}) {
        RegionMap regions(8, PolicyKind::Height);
        regions.set(regions.regionFor(static_cast<std::int32_t>(translation.x), static_cast<std::int32_t>(translation.z)), PolicyKind::Hex);
        const auto open = clipTerrainRegions(built.value(), translation, regions, PolicyKind::Hex);
        const auto closed = clipTerrainRegions(built.value(), translation, regions, PolicyKind::Hex, -8.f);
        REQUIRE(open.hasValue()); REQUIRE(closed.hasValue());
        CHECK_FALSE(raycastTerrainMesh(open.value(), translation + math::Vec3{10, -4, 4}, {-1, 0, 0}, 12, translation));
        for (int side = 0; side < 4; ++side) {
            const math::Vec3 normal = side == 0 ? math::Vec3{-1, 0, 0} : side == 1 ? math::Vec3{1, 0, 0} :
                side == 2 ? math::Vec3{0, 0, -1} : math::Vec3{0, 0, 1};
            for (int step = 1; step < 16; ++step) {
                const float t = float(step) * 0.5f;
                const math::Vec3 surface = side == 0 ? math::Vec3{0, -4, t} : side == 1 ? math::Vec3{8, -4, t} :
                    side == 2 ? math::Vec3{t, -4, 0} : math::Vec3{t, -4, 8};
                const auto hit = raycastTerrainMesh(closed.value(), translation + surface + normal * 2, normal * -1, 3, translation, true);
                REQUIRE(hit.has_value());
                CHECK(hit->distance == doctest::Approx(2));
                CHECK(hit->normal == normal);
                CHECK(hit->frontFace);
            }
        }
        CHECK_FALSE(raycastTerrainMesh(closed.value(), translation + math::Vec3{10, 3, 4}, {-1, 0, 0}, 12, translation));
        CHECK_FALSE(raycastTerrainMesh(closed.value(), translation + math::Vec3{10, -9, 4}, {-1, 0, 0}, 12, translation));
        const auto inside = raycastTerrainMesh(closed.value(), translation + math::Vec3{4, -4, 4}, {1, 0, 0}, 12, translation);
        REQUIRE(inside.has_value());
        CHECK_FALSE(inside->frontFace);
        CHECK_FALSE(raycastTerrainMesh(closed.value(), translation + math::Vec3{4, -4, 4}, {1, 0, 0}, 12, translation, true));
        double volume = 0;
        for (std::size_t i = 0; i < closed.value().indices.size(); i += 3) {
            const auto& mesh = closed.value();
            const auto a = mesh.vertices[mesh.indices[i]].position;
            const auto b = mesh.vertices[mesh.indices[i + 1]].position;
            const auto c = mesh.vertices[mesh.indices[i + 2]].position;
            volume += (double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) +
                       double(a.y) * (double(b.z) * c.x - double(b.x) * c.z) +
                       double(a.z) * (double(b.x) * c.y - double(b.y) * c.x)) / 6;
        }
        CHECK(volume == doctest::Approx(640).epsilon(1e-6));
    }
    RegionMap allHex(8, PolicyKind::Hex);
    const auto unchanged = clipTerrainRegions(built.value(), {}, allHex, PolicyKind::Hex, -8.f);
    REQUIRE(unchanged.hasValue());
    CHECK(unchanged.value().indices.size() == built.value().indices.size());
    CHECK_FALSE(clipTerrainRegions(built.value(), {}, allHex, PolicyKind::Height, -8.f));
    CHECK_FALSE(clipTerrainRegions(built.value(), {}, allHex, PolicyKind::Hex, INFINITY));
    CHECK_FALSE(clipTerrainRegions(built.value(), {}, allHex, PolicyKind::Hex, -10.f));
    CHECK_FALSE(clipTerrainRegions(built.value(), {}, allHex, PolicyKind::Hex, -1.f));

    RegionMap adjacent(8, PolicyKind::Height);
    adjacent.set({0, 0}, PolicyKind::Hex); adjacent.set({1, 0}, PolicyKind::Hex);
    const auto joined = clipTerrainRegions(built.value(), {}, adjacent, PolicyKind::Hex, -8.f);
    REQUIRE(joined.hasValue());
    CHECK_FALSE(raycastTerrainMesh(joined.value(), {7, -4, 4}, {1, 0, 0}, 2));
}

TEST_CASE("Terrain regions: existing coplanar hex walls are replaced without duplicate area") {
    const auto tile = generateHexTile(123, {}, {.radius = 2});
    const auto built = meshHexChunk(tile, {}, {.radius = 4.f, .floorHeight = -8.f, .hasNeighbor = {}});
    REQUIRE(built.hasValue());
    float maxX = -INFINITY;
    for (const auto& vertex : built.value().vertices) maxX = std::max(maxX, vertex.position.x);
    const math::Vec3 translation{32.f - maxX, 0, 16};
    REQUIRE(double(translation.x) + maxX == 32.0);
    RegionMap regions(32, PolicyKind::Height);
    regions.set({0, 0}, PolicyKind::Hex);
    const auto clipped = clipTerrainRegions(built.value(), translation, regions, PolicyKind::Hex, -8.f);
    REQUIRE(clipped.hasValue());
    const auto sideArea = [maxX](const MeshData& mesh) {
        double sum = 0;
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i + 1]].position,
                       c = mesh.vertices[mesh.indices[i + 2]].position;
            if (a.x != maxX || b.x != maxX || c.x != maxX) continue;
            const double area = (double(b.y) - a.y) * (double(c.z) - a.z) - (double(b.z) - a.z) * (double(c.y) - a.y);
            CHECK(area > 0);
            sum += area * 0.5;
        }
        return sum;
    };
    const auto original = sideArea(built.value());
    REQUIRE(original > 0);
    CHECK(sideArea(clipped.value()) == doctest::Approx(original).epsilon(1e-6));
}
