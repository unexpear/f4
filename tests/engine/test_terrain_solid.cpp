#include <array>

#include <doctest/doctest.h>

#include "engine/world/terrain_boundary_walls.h"
#include "engine/world/terrain_solid.h"

namespace {
using namespace f4;
using namespace f4::world;

auto span(double begin, double end, double n0, double n1, double p0, double p1) -> TerrainBoundarySpan {
    return {begin, end, 1, 1, TerrainBoundarySample{n0, n1, 4, 12}, TerrainBoundarySample{p0, p1, 7, 21}};
}

auto boundary(std::span<const TerrainBoundarySpan> profile, TerrainBoundaryLine line, double tangent, double height,
              PolicyKind negative = PolicyKind::Height, PolicyKind positive = PolicyKind::Height,
              std::optional<double> negativeFloor = {}, std::optional<double> positiveFloor = {})
    -> Result<TerrainSolidSample, TerrainSolidError> {
    return classifyTerrainBoundarySolid(
        {line, profile, negative, positive, negativeFloor, positiveFloor, tangent, height});
}

auto heightColumn(double top) -> TerrainSolidColumn { return {PolicyKind::Height, top, {}}; }
auto hexColumn(double top, double floor) -> TerrainSolidColumn { return {PolicyKind::Hex, top, floor}; }
} // namespace

TEST_CASE("Solid columns: transition walls and boundary volume share higher-side ownership") {
    const std::array profile{span(-2, 2, 0, 4, 3, 1)};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 64, -2, 2};
    const auto built = meshTerrainBoundaryWalls(profile, line, 13, 29);
    REQUIRE(built.hasValue());
    REQUIRE(built.value().triangles.size() == 2);
    const auto& mesh = built.value().mesh;
    for (std::size_t i = 0; i < built.value().triangles.size(); ++i) {
        double tangent = 0, height = 0;
        for (int corner = 0; corner < 3; ++corner) {
            const auto& position = mesh.vertices[mesh.indices[i * 3 + static_cast<std::size_t>(corner)]].position;
            tangent += position.z;
            height += position.y;
        }
        const auto sample = boundary(profile, line, tangent / 3.0, height / 3.0);
        REQUIRE(sample.hasValue());
        CHECK(sample.value().solid);
        CHECK(sample.value().wallBand);
        CHECK(sample.value().sector == (built.value().triangles[i].positiveOwner ? 1 : 0));
        CHECK(sample.value().policy == PolicyKind::Height);
    }

    const auto left = boundary(profile, line, -1, 2);
    REQUIRE(left.hasValue());
    CHECK(left.value().sector == 1);
    CHECK(left.value().wallBand);
    const auto right = boundary(profile, line, 1, 2);
    REQUIRE(right.hasValue());
    CHECK(right.value().sector == 0);
    CHECK(right.value().wallBand);
    const auto buried = boundary(profile, line, -1, -1);
    REQUIRE(buried.hasValue());
    CHECK(buried.value().solid);
    CHECK_FALSE(buried.value().wallBand);
    CHECK(buried.value().sector == 1);
    const auto above = boundary(profile, line, -1, 4);
    REQUIRE(above.hasValue());
    CHECK_FALSE(above.value().solid);

    const std::array equal{span(0, 2, 3, 3, 3, 3)};
    const auto tie = boundary(equal, {TerrainBoundaryAxis::Z, -8, 0, 2}, 1, 3);
    REQUIRE(tie.hasValue());
    CHECK(tie.value().solid);
    CHECK_FALSE(tie.value().wallBand);
    CHECK(tie.value().sector == 0);
    CHECK_FALSE(boundary(equal, {TerrainBoundaryAxis::Z, -8, 0, 2}, 1, 3.5).value().solid);

    const auto interior = classifyTerrainColumn(heightColumn(0), 1);
    REQUIRE(interior.hasValue());
    CHECK_FALSE(interior.value().solid);
    const std::array step{span(0, 2, 0, 0, 2, 2)};
    const auto onPlane = boundary(step, {TerrainBoundaryAxis::X, 0, 0, 2}, 1, 1);
    REQUIRE(onPlane.hasValue());
    CHECK(onPlane.value().solid);
    CHECK(onPlane.value().sector == 1);
}

TEST_CASE("Solid columns: discontinuous breakpoints fail closed and hex floors stay finite") {
    const std::array jumped{span(0, 1, 0, 0, 2, 2), span(1, 2, 4, 4, 2, 2)};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::Z, -8, 0, 2};
    REQUIRE(meshTerrainBoundaryWalls(jumped, line).hasValue());
    CHECK(boundary(jumped, line, 1, 3).error() == TerrainSolidError::DiscontinuousHeight);
    const auto left = boundary(jumped, line, 0.5, 1);
    REQUIRE(left.hasValue());
    CHECK(left.value().sector == 1);
    CHECK(left.value().wallBand);
    const auto right = boundary(jumped, line, 1.5, 3);
    REQUIRE(right.hasValue());
    CHECK(right.value().sector == 0);
    CHECK(right.value().wallBand);

    const std::array prism{span(0, 4, 2, 2, 5, 5)};
    const TerrainBoundaryLine hexLine{TerrainBoundaryAxis::X, 0, 0, 4};
    const auto belowFloor = boundary(prism, hexLine, 1, -1, PolicyKind::Height, PolicyKind::Hex, {}, 0);
    REQUIRE(belowFloor.hasValue());
    CHECK(belowFloor.value().solid);
    CHECK(belowFloor.value().sector == 0);
    CHECK_FALSE(belowFloor.value().wallBand);
    const auto shared = boundary(prism, hexLine, 1, 1, PolicyKind::Height, PolicyKind::Hex, {}, 0);
    REQUIRE(shared.hasValue());
    CHECK(shared.value().sector == 1);
    CHECK_FALSE(shared.value().wallBand);
    const auto band = boundary(prism, hexLine, 1, 3, PolicyKind::Height, PolicyKind::Hex, {}, 0);
    REQUIRE(band.hasValue());
    CHECK(band.value().policy == PolicyKind::Hex);
    CHECK(band.value().sector == 1);
    CHECK(band.value().wallBand);
    CHECK_FALSE(boundary(prism, hexLine, 1, 6, PolicyKind::Height, PolicyKind::Hex, {}, 0).value().solid);
    CHECK(boundary(prism, hexLine, 1, 3, PolicyKind::Height, PolicyKind::Hex, {}, 6).error() ==
          TerrainSolidError::HexFloorInversion);

    const auto inside = classifyTerrainColumn(hexColumn(5, -8), -8);
    REQUIRE(inside.hasValue());
    CHECK(inside.value().solid);
    CHECK_FALSE(inside.value().wallBand);
    CHECK_FALSE(classifyTerrainColumn(hexColumn(5, -8), -8.5).value().solid);
    CHECK(classifyTerrainColumn(hexColumn(5, 5), 5).error() == TerrainSolidError::HexFloorInversion);
    CHECK(classifyTerrainColumn(TerrainSolidColumn{PolicyKind::Height, 1, 0}, 0).error() ==
          TerrainSolidError::InvalidData);
    CHECK(classifyTerrainColumn(TerrainSolidColumn{PolicyKind::Voxel, 1, {}}, 0).error() ==
          TerrainSolidError::UnsupportedVoxel);
}

TEST_CASE("Solid columns: corners resolve sector heights and reject nonmanifold joins") {
    const std::array<TerrainCornerArm, 2> step{{
        {1, 2, 5},
        {3, 2, 5},
    }};
    const auto heights = resolveTerrainCornerHeights(step);
    REQUIRE(heights.hasValue());
    CHECK(heights.value() == (std::array<double, 4>{5, 2, 2, 5}));
    std::array<TerrainSolidColumn, 4> sectors{
        heightColumn(heights.value()[0]), heightColumn(heights.value()[1]),
        heightColumn(heights.value()[2]), heightColumn(heights.value()[3])};
    const auto high = classifyTerrainCornerSolid(sectors, 3);
    REQUIRE(high.hasValue());
    CHECK(high.value().solid);
    CHECK(high.value().sector == 0);
    CHECK(high.value().wallBand);
    const auto low = classifyTerrainCornerSolid(sectors, 1);
    REQUIRE(low.hasValue());
    CHECK(low.value().sector == 0);
    CHECK_FALSE(low.value().wallBand);
    CHECK_FALSE(classifyTerrainCornerSolid(sectors, 6).value().solid);

    std::array<TerrainSolidColumn, 4> saddle{
        heightColumn(0), heightColumn(2), heightColumn(0), heightColumn(2)};
    CHECK(classifyTerrainCornerSolid(saddle, 1).error() == TerrainSolidError::NonmanifoldCorner);
    CHECK(classifyTerrainCornerSolid(saddle, -1).error() == TerrainSolidError::NonmanifoldCorner);

    std::array<TerrainSolidColumn, 4> punched{
        heightColumn(5), hexColumn(5, 0), heightColumn(5), hexColumn(5, 0)};
    CHECK(classifyTerrainCornerSolid(punched, -1).error() == TerrainSolidError::NonmanifoldCorner);

    std::array<TerrainSolidColumn, 4> prism{{
        heightColumn(1), heightColumn(1), hexColumn(4, 0), hexColumn(4, 0)}};
    const auto cap = classifyTerrainCornerSolid(prism, 2);
    REQUIRE(cap.hasValue());
    CHECK(cap.value().policy == PolicyKind::Hex);
    CHECK(cap.value().sector == 2);
    CHECK(cap.value().wallBand);
    const auto under = classifyTerrainCornerSolid(prism, -1);
    REQUIRE(under.hasValue());
    CHECK(under.value().sector == 0);
    CHECK_FALSE(under.value().wallBand);

    const std::array<TerrainCornerArm, 2> mismatch{{
        {0, 1, 4},
        {1, 4, 4},
    }};
    CHECK(resolveTerrainCornerHeights(mismatch).error() == TerrainSolidError::InconsistentSector);
    const std::array<TerrainCornerArm, 1> flat{{{1, 3, 3}}};
    const auto filled = resolveTerrainCornerHeights(flat);
    REQUIRE(filled.hasValue());
    CHECK(filled.value() == (std::array<double, 4>{3, 3, 3, 3}));
    const std::array<TerrainCornerArm, 2> collapsed{{
        {1, 1, 1 + 1e-10},
        {3, 1, 1 + 1e-10},
    }};
    CHECK(resolveTerrainCornerHeights(collapsed).error() == TerrainSolidError::PrecisionLoss);
    CHECK(resolveTerrainCornerHeights(std::array<TerrainCornerArm, 2>{{{0, 1, 1}, {0, 2, 2}}}).error() ==
          TerrainSolidError::InvalidData);
}

TEST_CASE("Solid columns: nonplanar tops, voxel stacks and unsupported profiles fail closed") {
    const TerrainTopSample a{0, 0, 0}, c{0, 0, 1}, d{1, 2, 1}, b{1, 0, 0};
    const auto top = sampleUpwardTriangleHeight(a, c, d, 0.5, 0.5);
    REQUIRE(top.hasValue());
    CHECK(top.value() == doctest::Approx(1));
    const double bilinear = 0.5 * 0.5 * 2;
    CHECK(top.value() != bilinear);
    const auto column = classifyTerrainColumn(heightColumn(top.value()), 0.75);
    REQUIRE(column.hasValue());
    CHECK(column.value().solid);
    CHECK_FALSE(classifyTerrainColumn(heightColumn(bilinear), 0.75).value().solid);
    const auto other = sampleUpwardTriangleHeight(d, b, a, 0.75, 0.25);
    REQUIRE(other.hasValue());
    CHECK(other.value() == doctest::Approx(0.5));
    CHECK(sampleUpwardTriangleHeight(a, d, c, 0.25, 0.75).error() == TerrainSolidError::UnsupportedUnderside);
    CHECK(sampleUpwardTriangleHeight(a, c, d, -1, 0.5).error() == TerrainSolidError::InvalidData);

    CHECK(classifyVoxelStack(std::array<bool, 3>{true, false, true}).error() == TerrainSolidError::VoxelOverhang);
    CHECK(classifyVoxelStack(std::array<bool, 3>{false, true, true}).error() == TerrainSolidError::VoxelCavity);
    CHECK(classifyVoxelStack(std::array<bool, 3>{true, true, false}).error() == TerrainSolidError::UnsupportedVoxel);
    CHECK(classifyVoxelStack(std::array<bool, 2>{false, false}).error() == TerrainSolidError::UnsupportedVoxel);
    CHECK(classifyVoxelStack(std::span<const bool>{}).error() == TerrainSolidError::InvalidData);
    const std::array<VoxelSolidSpan, 2> cave{{{0, 2}, {3, 6}}};
    const auto lower = classifyVoxelColumn(cave, 1);
    const auto pocket = classifyVoxelColumn(cave, 2.5);
    const auto upper = classifyVoxelColumn(cave, 4);
    REQUIRE(lower.hasValue());
    REQUIRE(pocket.hasValue());
    REQUIRE(upper.hasValue());
    CHECK(lower.value().solid);
    CHECK(lower.value().policy == PolicyKind::Voxel);
    CHECK_FALSE(pocket.value().solid);
    CHECK(pocket.value().policy == PolicyKind::Voxel);
    CHECK(upper.value().solid);
    CHECK(upper.value().policy == PolicyKind::Voxel);
    CHECK(classifyVoxelColumn(cave, 2).value().solid);
    CHECK(classifyVoxelColumn(cave, 3).value().solid);
    const auto sky = classifyVoxelColumn(cave, 7);
    REQUIRE(sky.hasValue());
    CHECK_FALSE(sky.value().solid);
    CHECK(sky.value().policy == PolicyKind::Voxel);
    const auto air = classifyVoxelColumn({}, 1);
    REQUIRE(air.hasValue());
    CHECK_FALSE(air.value().solid);
    CHECK(air.value().policy == PolicyKind::Voxel);
    CHECK(classifyVoxelColumn(std::array<VoxelSolidSpan, 2>{{{0, 2}, {1, 3}}}, 1).error() ==
          TerrainSolidError::InvalidData);
    CHECK(classifyVoxelColumn(std::array<VoxelSolidSpan, 1>{{{2, 2}}}, 2).error() == TerrainSolidError::InvalidData);

    const std::array profile{span(0, 2, 0, 0, 2, 2)};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, 0, 2};
    CHECK(boundary({}, line, 1, 1).error() == TerrainSolidError::IncompleteProfile);
    auto broken = profile;
    broken[0].positiveCount = 2;
    CHECK(boundary(broken, line, 1, 1).error() == TerrainSolidError::IncompleteProfile);
    CHECK(boundary(std::array{span(0, 1, 0, 0, 2, 2)}, line, 0.5, 1).error() == TerrainSolidError::IncompleteProfile);
    CHECK(boundary(profile, {TerrainBoundaryAxis::X, 0.1, 0, 2}, 1, 1).error() == TerrainSolidError::PrecisionLoss);
    CHECK(boundary(std::array{span(0, 2, 1, 1, 1 + 1e-10, 1 + 1e-10)}, line, 1, 1).error() ==
          TerrainSolidError::PrecisionLoss);
    CHECK(boundary(profile, line, 1, 1, PolicyKind::Voxel, PolicyKind::Height).error() ==
          TerrainSolidError::UnsupportedVoxel);
    CHECK(boundary(profile, line, 3, 1).error() == TerrainSolidError::InvalidData);
    const auto zAxis = boundary(profile, {TerrainBoundaryAxis::Z, 0, 0, 2}, 1, 1);
    REQUIRE(zAxis.hasValue());
    CHECK(zAxis.value().sector == 1);
    CHECK(zAxis.value().wallBand);
}
