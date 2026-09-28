#include <array>
#include <cmath>
#include <limits>

#include <doctest/doctest.h>

#include "engine/world/terrain_boundary_walls.h"
#include "engine/world/terrain_queries.h"

namespace {
using namespace f4;
using namespace f4::world;
auto span(double begin, double end, double n0, double n1, double p0, double p1) -> TerrainBoundarySpan {
    return {begin, end, 1, 1, TerrainBoundarySample{n0, n1, 4, 12}, TerrainBoundarySample{p0, p1, 7, 21}};
}
auto area(const MeshData& mesh) -> double {
    double result = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto a = mesh.vertices[mesh.indices[i]].position;
        const auto b = mesh.vertices[mesh.indices[i + 1]].position;
        const auto c = mesh.vertices[mesh.indices[i + 2]].position;
        const double x = (double(b.y) - a.y) * (double(c.z) - a.z) - (double(b.z) - a.z) * (double(c.y) - a.y);
        const double z = (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
        CHECK(x * mesh.vertices[mesh.indices[i]].normal.x + z * mesh.vertices[mesh.indices[i]].normal.z > 0);
        result += std::sqrt(x * x + z * z) * 0.5;
    }
    return result;
}
void probe(const MeshData& mesh, TerrainBoundaryAxis axis, float plane, float t, float y, bool positiveOwner) {
    const float sign = positiveOwner ? -1.f : 1.f;
    const auto eye = axis == TerrainBoundaryAxis::X ? math::Vec3{plane + 2 * sign, y, t} : math::Vec3{t, y, plane + 2 * sign};
    const auto direction = axis == TerrainBoundaryAxis::X ? math::Vec3{-sign, 0, 0} : math::Vec3{0, 0, -sign};
    const auto hit = raycastTerrainMesh(mesh, eye, direction, 4, {}, true);
    REQUIRE(hit); CHECK(hit->frontFace); CHECK(hit->distance == doctest::Approx(2));
    CHECK((axis == TerrainBoundaryAxis::X ? hit->normal.x : hit->normal.z) == sign);
    const auto behind = axis == TerrainBoundaryAxis::X ? math::Vec3{plane - 2 * sign, y, t} : math::Vec3{t, y, plane - 2 * sign};
    const auto away = axis == TerrainBoundaryAxis::X ? math::Vec3{sign, 0, 0} : math::Vec3{0, 0, sign};
    CHECK_FALSE(raycastTerrainMesh(mesh, behind, away, 4, {}, true));
    CHECK(raycastTerrainMesh(mesh, behind, away, 4));
}
}

TEST_CASE("Boundary walls: both axes and owners have outward winding, area, materials and ray parity") {
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        for (const bool positive : {false, true}) {
            const std::array profile{positive ? span(-2, 2, 0, 0, 2, 4) : span(-2, 2, 2, 4, 0, 0)};
            const auto built = meshTerrainBoundaryWalls(profile, {axis, -64, -2, 2}, 13, 29);
            REQUIRE(built.hasValue());
            const auto& mesh = built.value().mesh;
            REQUIRE(built.value().triangles.size() == 2);
            CHECK(mesh.vertices.size() == 6); CHECK(mesh.indices.size() == 6);
            CHECK(area(mesh) == doctest::Approx(12));
            REQUIRE(mesh.surfaces.size() == 1);
            CHECK(mesh.surfaces[0].firstIndex == 0); CHECK(mesh.surfaces[0].indexCount == 6);
            CHECK(mesh.surfaces[0].material == (positive ? 29 : 13));
            for (const auto& triangle : built.value().triangles) {
                CHECK(triangle.spanIndex == 0); CHECK(triangle.positiveOwner == positive);
            }
            for (const auto& v : mesh.vertices) {
                CHECK(v.material == (positive ? 29 : 13));
                CHECK(v.uv.x == (axis == TerrainBoundaryAxis::X ? v.position.z : v.position.x));
                CHECK(v.uv.y == v.position.y);
            }
            for (const float t : {-1.5f, -0.5f, 0.5f, 1.5f}) probe(mesh, axis, -64, t, 1, positive);
        }
    }
}

TEST_CASE("Boundary walls: crossings share one point, flip ownership and omit only zero-area faces") {
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        const std::array profile{span(-2, 2, 0, 4, 3, 1)};
        const auto built = meshTerrainBoundaryWalls(profile, {axis, 64, -2, 2}, 13, 29);
        REQUIRE(built.hasValue()); REQUIRE(built.value().triangles.size() == 2);
        REQUIRE(built.value().topEdges.size() == 4);
        CHECK(built.value().topEdges[0].end == 0); CHECK(built.value().topEdges[0].endHeight == 2);
        CHECK(built.value().topEdges[1].endHeight == 2);
        CHECK(built.value().topEdges[2].begin == 0); CHECK(built.value().topEdges[3].beginHeight == 2);
        CHECK(built.value().triangles[0].positiveOwner); CHECK_FALSE(built.value().triangles[1].positiveOwner);
        CHECK(area(built.value().mesh) == doctest::Approx(6));
        REQUIRE(built.value().mesh.surfaces.size() == 2);
        CHECK(built.value().mesh.surfaces[1].firstIndex == 3);
        unsigned crossings = 0;
        for (const auto& v : built.value().mesh.vertices) {
            const auto tangent = axis == TerrainBoundaryAxis::X ? v.position.z : v.position.x;
            if (tangent == 0) { CHECK(v.position.y == 2); ++crossings; }
        }
        CHECK(crossings == 2);
        probe(built.value().mesh, axis, 64, -1, 1.5f, true);
        probe(built.value().mesh, axis, 64, 1, 2, false);
    }
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, 0, 2};
    for (const auto& s : {span(0, 2, 0, 2, 0, 0), span(0, 2, 2, 0, 0, 0)}) {
        const std::array profile{s};
        const auto built = meshTerrainBoundaryWalls(profile, line, 0, 0, 1);
        REQUIRE(built.hasValue()); CHECK(built.value().triangles.size() == 1);
        CHECK(area(built.value().mesh) == doctest::Approx(2));
    }
    const std::array equal{span(0, 2, 2, 4, 2, 4)};
    const auto empty = meshTerrainBoundaryWalls(equal, line);
    REQUIRE(empty.hasValue()); CHECK(empty.value().mesh.indices.isEmpty()); CHECK(empty.value().mesh.surfaces.isEmpty());
    CHECK(empty.value().topEdges.size() == 2);
}

TEST_CASE("Boundary walls: partitions retain discontinuities and source spans without smoothing") {
    const std::array profile{span(0, 1, 0, 0, 2, 2), span(1, 2, 4, 4, 2, 2)};
    const auto built = meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::Z, -8, 0, 2}, 13, 29, 4);
    REQUIRE(built.hasValue()); REQUIRE(built.value().triangles.size() == 4);
    CHECK(area(built.value().mesh) == doctest::Approx(4));
    for (std::size_t i = 0; i < 4; ++i) CHECK(built.value().triangles[i].spanIndex == i / 2);
    probe(built.value().mesh, TerrainBoundaryAxis::Z, -8, 0.5f, 1, true);
    probe(built.value().mesh, TerrainBoundaryAxis::Z, -8, 1.5f, 3, false);
    CHECK(meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::Z, -8, 0, 2}, 0, 0, 3).error() ==
          TerrainBoundaryWallError::TriangleLimitExceeded);
}

TEST_CASE("Boundary walls: invalid, incomplete and float-collapsed inputs fail without partial geometry") {
    using Error = TerrainBoundaryWallError;
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, 0, 2};
    std::array profile{span(0, 2, 0, 0, 2, 2)};
    CHECK(meshTerrainBoundaryWalls({}, line).error() == Error::IncompleteProfile);
    CHECK(meshTerrainBoundaryWalls(profile, line, 0, 0, 0).error() == Error::InvalidData);
    CHECK(meshTerrainBoundaryWalls(profile, line, 0, 0, 131073).error() == Error::InvalidData);
    CHECK(meshTerrainBoundaryWalls(profile, line, 0, 0, 1).error() == Error::TriangleLimitExceeded);
    CHECK(meshTerrainBoundaryWalls(profile, line, 0, 0, 2).hasValue());
    profile[0].positiveCount = 2;
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::IncompleteProfile);
    profile[0].positiveCount = 1; profile[0].positive.reset();
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::IncompleteProfile);
    profile[0] = span(0, 1, 0, 0, 2, 2);
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::IncompleteProfile);
    profile[0].begin = -1;
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::InvalidData);
    profile[0] = span(0, 2, 0, 0, INFINITY, 2);
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::InvalidData);
    profile[0].positive->beginHeight = std::numeric_limits<double>::quiet_NaN();
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::InvalidData);
    profile[0] = span(0, 2, 0, 0, 2, 2);
    CHECK(meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::X, 0.1, 0, 2}).error() == Error::PrecisionLoss);
    CHECK(meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::X, 1e100, 0, 2}).error() == Error::InvalidData);
    CHECK(meshTerrainBoundaryWalls(profile, {static_cast<TerrainBoundaryAxis>(5), 0, 0, 2}).error() == Error::InvalidData);
    profile[0] = span(16777216, 16777217, 0, 0, 2, 2);
    CHECK(meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::X, 0, 16777216, 16777217}).error() == Error::PrecisionLoss);
    profile[0] = span(0, 2, 1, 1, 1 + 1e-10, 1 + 1e-10);
    CHECK(meshTerrainBoundaryWalls(profile, line).error() == Error::PrecisionLoss);
    // A crossing too close to an endpoint to survive float rounding is not dropped.
    profile[0] = span(1, 2, -1e-10, 1, 0, 0);
    CHECK(meshTerrainBoundaryWalls(profile, {TerrainBoundaryAxis::X, 0, 1, 2}).error() == Error::PrecisionLoss);
    const std::array overlap{span(0, 1.5, 0, 0, 2, 2), span(1, 2, 0, 0, 2, 2)};
    CHECK(meshTerrainBoundaryWalls(overlap, line).error() == Error::InvalidData);
}
