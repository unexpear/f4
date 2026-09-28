#include <array>
#include <algorithm>
#include <limits>
#include <map>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "engine/world/boundary_replacement.h"
#include "engine/world/world_streaming.h"

namespace {
using namespace f4;
using namespace f4::world;
auto source(bool positive) -> MeshData {
    MeshData mesh;
    const float h = positive ? 2.f : 0.f;
    for (const auto p : {math::Vec3{0, h, 0}, math::Vec3{0, h, 4}, math::Vec3{positive ? 2.f : -2.f, h, 0},
                         math::Vec3{0, -4, 0}, math::Vec3{0, h, 0}, math::Vec3{0, h, 4}, math::Vec3{0, -4, 4}})
        mesh.vertices.push({p});
    for (const auto i : {positive ? 0u : 1u, positive ? 1u : 0u, 2u, 3u, 4u, 5u, 3u, 5u, 6u}) mesh.indices.push(i);
    mesh.surfaces.push({0, 3, 7}); mesh.surfaces.push({3, 6, 9});
    return mesh;
}
auto payloadBytes(const TerrainBoundaryReplacement& r) -> std::size_t {
    return r.mesh.vertices.size() * sizeof(TerrainVertex) + r.mesh.indices.size() * sizeof(std::uint32_t) +
        r.mesh.surfaces.size() * sizeof(TerrainSurface) + r.triangles.size() * sizeof(BoundaryReplacementTriangle);
}
}

TEST_CASE("Boundary replacement: one common-frame mesh removes old walls and retains query provenance") {
    const auto n = source(false), p = source(true);
    const math::Vec3 offset{-64, 8, 128};
    const std::array negative{TerrainBoundaryMesh{&n, offset}}, positive{TerrainBoundaryMesh{&p, offset}};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, -64, 128, 132};
    const auto result = buildTerrainBoundaryReplacement(negative, positive, line, {}, 13, 29);
    REQUIRE(result.hasValue()); const auto& r = result.value();
    CHECK(r.triangles.size() == 4); CHECK(r.mesh.vertices.size() == 12);
    REQUIRE(r.mesh.surfaces.size() == 3); // Same material 7 on opposite sides stays separate.
    CHECK(r.mesh.surfaces[0].material == 7); CHECK(r.mesh.surfaces[1].material == 7); CHECK(r.mesh.surfaces[2].material == 29);
    CHECK_FALSE(r.triangles[0].positive); CHECK(r.triangles[1].positive);
    const auto wall = raycastTerrainMesh(r.mesh, {-66, 9, 129}, {1, 0, 0}, 4, {}, true);
    REQUIRE(wall); CHECK(wall->distance == 2); CHECK(wall->normal.x == -1);
    CHECK(r.triangles[wall->triangleIndex].wall); CHECK(r.triangles[wall->triangleIndex].positive);
    CHECK(r.triangles[wall->triangleIndex].sourceIndex == 0); CHECK(r.triangles[wall->triangleIndex].sourceTriangle == 0);
    CHECK_FALSE(raycastTerrainMesh(r.mesh, {-66, 6, 129}, {1, 0, 0}, 4)); // Old floor cap is gone.
    for (const bool side : {false, true}) {
        const auto hit = raycastTerrainMesh(r.mesh, {side ? -63.f : -65.f, 20, 129}, {0, -1, 0}, 20, {}, true);
        REQUIRE(hit); CHECK(hit->position.y == (side ? 10.f : 8.f));
        CHECK_FALSE(r.triangles[hit->triangleIndex].wall); CHECK(r.triangles[hit->triangleIndex].positive == side);
        CHECK(r.triangles[hit->triangleIndex].sourceTriangle == 0);
    }
    CHECK(n.indices.size() == 9); CHECK(p.indices.size() == 9); // Pure build did not edit inputs.
}

TEST_CASE("Boundary replacement: exact logical payload limits, incomplete sources and precision rejection") {
    using Error = BoundaryReplacementError;
    const auto n = source(false), p = source(true);
    const std::array negative{TerrainBoundaryMesh{&n}}, positive{TerrainBoundaryMesh{&p}};
    const TerrainBoundaryLine line{TerrainBoundaryAxis::X, 0, 0, 4};
    const auto reference = buildTerrainBoundaryReplacement(negative, positive, line); REQUIRE(reference.hasValue());
    BoundaryReplacementLimits limits;
    limits.maxBytes = payloadBytes(reference.value());
    CHECK(buildTerrainBoundaryReplacement(negative, positive, line, limits).hasValue());
    --limits.maxBytes; CHECK(buildTerrainBoundaryReplacement(negative, positive, line, limits).error() == Error::LimitExceeded);
    limits = {}; limits.maxTriangles = 4; CHECK(buildTerrainBoundaryReplacement(negative, positive, line, limits).hasValue());
    limits.maxTriangles = 3; CHECK(buildTerrainBoundaryReplacement(negative, positive, line, limits).error() == Error::LimitExceeded);
    limits = {}; limits.maxSources = 1; CHECK(buildTerrainBoundaryReplacement(negative, positive, line, limits).error() == Error::LimitExceeded);
    CHECK(buildTerrainBoundaryReplacement(negative, {}, line).error() == Error::IncompleteProfile);
    const std::array duplicate{positive[0], positive[0]};
    CHECK(buildTerrainBoundaryReplacement(negative, duplicate, line).error() == Error::IncompleteProfile);
    auto rounded = n; rounded.vertices[2].position.x = -0.1f;
    const std::array offsetNegative{TerrainBoundaryMesh{&rounded, {64, 0, 0}}}, offsetPositive{TerrainBoundaryMesh{&p, {64, 0, 0}}};
    CHECK(buildTerrainBoundaryReplacement(offsetNegative, offsetPositive, {line.axis, 64, 0, 4}).error() == Error::PrecisionLoss);
    CHECK_FALSE(StreamingBoundarySnapshot{}.buildReplacement());
    CHECK(StreamingBoundaryReplacement{}.geometry() == nullptr);
}

TEST_CASE("Boundary replacement: immutable snapshot package retains freshness and survives source destruction") {
    WorldStreamingConfig config;
    config.workerCount = 2; config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionExtentMeters = 64; config.policyParams.hex.radius = 2; config.hexRadius = 8;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true; config.frameBudget = std::chrono::seconds(1);
    auto world = std::make_unique<WorldStreamer>(config); world->regions().set({0, 0}, PolicyKind::Hex);
    for (unsigned i = 0; i < 128; ++i) { world->update({}); world->waitForIdle(); if (!world->hasPendingWork()) break; }
    REQUIRE_FALSE(world->hasPendingWork());
    const auto snapshot = world->captureBoundary({{TerrainBoundaryAxis::X, 64, 0, 64}}); REQUIRE(snapshot.hasValue());
    std::optional<StreamingBoundaryReplacement> packet;
    // Worker reads only the captured value. Thread join publishes the result.
    std::thread worker([&] { auto built = snapshot.value().buildReplacement(); if (built) packet = std::move(built).value(); });
    worker.join(); REQUIRE(packet); REQUIRE(packet->geometry()); CHECK(world->boundaryCurrent(*packet));
    CHECK_FALSE(world->boundaryCurrent(StreamingBoundaryReplacement{}));
    const auto triangles = packet->geometry()->triangles.size(); CHECK(triangles > 0);
    REQUIRE(world->edit({PolicyKind::Height, {1, 0, 0}}, heightSampleIndex(0, 32), CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
    CHECK_FALSE(world->boundaryCurrent(*packet)); CHECK(packet->geometry()->triangles.size() == triangles);
    auto stale = snapshot.value().buildReplacement(); REQUIRE(stale.hasValue()); CHECK_FALSE(world->boundaryCurrent(stale.value()));
    WorldStreamer another(config); CHECK_FALSE(another.boundaryCurrent(*packet));
    world.reset(); CHECK(packet->geometry()->triangles.size() == triangles);
    CHECK_FALSE(packet->sources(false).empty()); CHECK_FALSE(packet->sources(true).empty());
}

namespace {
auto strip(float minX, float maxX, float height, std::initializer_list<float> breaks) -> MeshData {
    MeshData mesh;
    const auto triangle = [&](math::Vec3 a, math::Vec3 b, math::Vec3 c) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto p : {a, b, c}) {
            TerrainVertex v{p}; v.uv = {p.x, p.z}; v.material = 7; v.biome = 11;
            mesh.vertices.push(v);
        }
        for (const auto i : {base, base + 1, base + 2}) mesh.indices.push(i);
    };
    const std::vector<float> z(breaks);
    for (std::size_t i = 1; i < z.size(); ++i) {
        triangle({minX, height, z[i - 1]}, {minX, height, z[i]}, {maxX, height, z[i - 1]});
        triangle({maxX, height, z[i - 1]}, {minX, height, z[i]}, {maxX, height, z[i]});
    }
    const auto tops = static_cast<std::uint32_t>(mesh.indices.size());
    for (const auto x : {minX, maxX}) {
        triangle({x, -4, z.front()}, {x, height, z.front()}, {x, height, z.back()});
        triangle({x, -4, z.front()}, {x, height, z.back()}, {x, -4, z.back()});
    }
    mesh.surfaces.push({0, tops, 7}); mesh.surfaces.push({tops, 12, 9});
    return mesh;
}
auto payloadBytes(const TerrainBoundaryComposition& r) -> std::size_t {
    return r.mesh.vertices.size() * sizeof(TerrainVertex) + r.mesh.indices.size() * sizeof(std::uint32_t) +
        r.mesh.surfaces.size() * sizeof(TerrainSurface) + r.triangles.size() * sizeof(BoundaryCompositionTriangle);
}
}

TEST_CASE("Boundary composition: shared sources retain successive cuts without duplicate geometry") {
    const auto center = strip(0, 4, 2, {0, 4}), left = strip(-4, 0, 0, {0, 1, 3, 4}), right = strip(4, 8, 4, {0, 2, 4});
    const std::array sources{TerrainBoundaryMesh{&center}, TerrainBoundaryMesh{&left}, TerrainBoundaryMesh{&right}};
    const std::array<std::size_t, 1> c{0}, l{1}, r{2};
    const TerrainBoundaryRequest west{{TerrainBoundaryAxis::X, 0, 0, 4}, l, c, 13, 29};
    const TerrainBoundaryRequest east{{TerrainBoundaryAxis::X, 4, 0, 4}, c, r, 31, 47};
    const std::array requests{west, east}, reversed{east, west};
    const auto built = composeTerrainBoundaryReplacements(sources, requests); REQUIRE(built.hasValue());
    const auto again = composeTerrainBoundaryReplacements(sources, reversed); REQUIRE(again.hasValue());
    const auto& result = built.value(); const auto& reverse = again.value();
    REQUIRE(result.triangles.size() == reverse.triangles.size());
    std::size_t centralTops = 0, walls = 0;
    double centralArea = 0;
    for (std::size_t i = 0; i < result.triangles.size(); ++i) {
        const auto& stamp = result.triangles[i]; const auto& other = reverse.triangles[i];
        CHECK(stamp.sourceIndex == other.sourceIndex); CHECK(stamp.sourceTriangle == other.sourceTriangle);
        CHECK(stamp.wall == other.wall); CHECK(stamp.positive == other.positive);
        if (stamp.wall) {
            ++walls; CHECK(stamp.boundaryIndex == 1 - other.boundaryIndex);
            CHECK(stamp.sourceIndex == (stamp.boundaryIndex == 0 ? 0 : 2));
        } else {
            CHECK(stamp.boundaryIndex == static_cast<std::size_t>(-1));
            if (stamp.sourceIndex == 0) {
                ++centralTops; CHECK(stamp.sourceTriangle < 2);
                const auto a = result.mesh.vertices[result.mesh.indices[i * 3]].position;
                const auto b = result.mesh.vertices[result.mesh.indices[i * 3 + 1]].position;
                const auto d = result.mesh.vertices[result.mesh.indices[i * 3 + 2]].position;
                centralArea += ((double(b.z) - a.z) * (double(d.x) - a.x) - (double(b.x) - a.x) * (double(d.z) - a.z)) / 2;
            }
        }
    }
    CHECK(centralTops == 5); CHECK(centralArea == 16); CHECK(walls == 10);
    REQUIRE(result.mesh.vertices.size() == reverse.mesh.vertices.size());
    for (std::size_t i = 0; i < result.mesh.vertices.size(); ++i) {
        const auto& a = result.mesh.vertices[i]; const auto& b = reverse.mesh.vertices[i];
        CHECK(a.position.x == b.position.x); CHECK(a.position.y == b.position.y); CHECK(a.position.z == b.position.z);
        CHECK(a.uv.x == b.uv.x); CHECK(a.uv.y == b.uv.y); CHECK(a.material == b.material);
        CHECK(a.biome == b.biome);
    }
    for (const float z : {0.5f, 1.5f, 2.5f, 3.5f}) {
        for (const bool eastSide : {false, true}) {
            const auto hit = raycastTerrainMesh(result.mesh, {eastSide ? 3.f : -1.f, eastSide ? 3.f : 1.f, z}, {1, 0, 0}, 2, {}, true);
            REQUIRE(hit); CHECK(hit->distance == 1); CHECK(result.triangles[hit->triangleIndex].wall);
            CHECK(result.triangles[hit->triangleIndex].boundaryIndex == std::size_t(eastSide));
            CHECK_FALSE(raycastTerrainMesh(result.mesh, {eastSide ? 3.f : -1.f, -2, z}, {1, 0, 0}, 2));
        }
        const auto top = raycastTerrainMesh(result.mesh, {2, 10, z}, {0, -1, 0}, 12, {}, true);
        REQUIRE(top); CHECK(top->position.y == 2); CHECK(result.triangles[top->triangleIndex].sourceIndex == 0);
    }
    CHECK(center.indices.size() == 18); CHECK(left.indices.size() == 30); CHECK(right.indices.size() == 24);
    BoundaryCompositionLimits limits;
    limits.mesh.maxBytes = payloadBytes(result);
    CHECK(composeTerrainBoundaryReplacements(sources, requests, limits).hasValue());
    --limits.mesh.maxBytes; CHECK(composeTerrainBoundaryReplacements(sources, requests, limits).error() == BoundaryReplacementError::LimitExceeded);
    limits = {}; limits.mesh.maxTriangles = result.triangles.size();
    CHECK(composeTerrainBoundaryReplacements(sources, requests, limits).hasValue());
    --limits.mesh.maxTriangles; CHECK(composeTerrainBoundaryReplacements(sources, requests, limits).error() == BoundaryReplacementError::LimitExceeded);
}

TEST_CASE("Boundary composition: invalid identities, junctions and budgets fail explicitly") {
    using Error = BoundaryReplacementError;
    const auto n = source(false), p = source(true);
    const std::array sources{TerrainBoundaryMesh{&n}, TerrainBoundaryMesh{&p}};
    const std::array<std::size_t, 1> negative{0}, positive{1}, invalid{2};
    TerrainBoundaryRequest request{{TerrainBoundaryAxis::X, 0, 0, 4}, negative, positive};
    const auto run = [&](std::span<const TerrainBoundaryRequest> requests, BoundaryCompositionLimits limits = {}) {
        return composeTerrainBoundaryReplacements(sources, requests, limits);
    };
    REQUIRE(run({&request, 1}).hasValue());
    CHECK(run({}).error() == Error::InvalidData);
    for (const auto line : {TerrainBoundaryLine{TerrainBoundaryAxis::X, 0, 0, 4},
                           TerrainBoundaryLine{TerrainBoundaryAxis::X, 0, 2, 6},
                           TerrainBoundaryLine{TerrainBoundaryAxis::Z, 2, -1, 1},
                           TerrainBoundaryLine{TerrainBoundaryAxis::Z, 4, -1, 1}}) {
        auto second = request; second.line = line;
        const std::array pair{request, second};
        CHECK(run(pair).error() == Error::ConflictingBoundaries);
    }
    auto bad = request; bad.positive = invalid; CHECK(run({&bad, 1}).error() == Error::InvalidData);
    bad = request; bad.positive = negative; CHECK(run({&bad, 1}).error() == Error::InvalidData);
    bad = request; bad.line.plane = std::numeric_limits<double>::quiet_NaN(); CHECK(run({&bad, 1}).error() == Error::InvalidData);
    bad = request; bad.line.axis = static_cast<TerrainBoundaryAxis>(99); CHECK(run({&bad, 1}).error() == Error::InvalidData);
    bad = request; bad.positive = {};
    CHECK(run({&bad, 1}).error() == Error::InvalidData); // Unused source is not silently carried through.
    const std::array duplicate{sources[0], sources[0]};
    CHECK(composeTerrainBoundaryReplacements(duplicate, {&request, 1}).error() == Error::InvalidData);
    const std::array nullSource{TerrainBoundaryMesh{}, sources[1]};
    CHECK(composeTerrainBoundaryReplacements(nullSource, {&request, 1}).error() == Error::InvalidData);
    const std::array<std::size_t, 2> both{0, 1};
    bad = request; bad.negative = both; bad.positive = {};
    CHECK(run({&bad, 1}).error() == Error::IncompleteProfile);
    BoundaryCompositionLimits limits; limits.maxBoundaries = 0;
    CHECK(run({&request, 1}, limits).error() == Error::InvalidData);
    limits = {}; limits.mesh.maxSources = 1; CHECK(run({&request, 1}, limits).error() == Error::LimitExceeded);
    limits = {}; limits.mesh.maxTriangles = 0; CHECK(run({&request, 1}, limits).error() == Error::LimitExceeded);
    limits = {}; limits.mesh.maxBytes = 0; CHECK(run({&request, 1}, limits).error() == Error::LimitExceeded);
    auto second = request; second.line.plane = 8;
    const std::array pair{request, second}; limits = {}; limits.maxBoundaries = 1;
    CHECK(run(pair, limits).error() == Error::LimitExceeded);
}

namespace {
struct JunctionFixture {
    // NE, NW, SW, SE; each original top consists of triangles 0 and 1.
    std::array<MeshData, 4> meshes;
    std::array<TerrainBoundaryMesh, 4> sources;
    std::array<std::array<std::size_t, 1>, 4> ids{{{0}, {1}, {2}, {3}}};
    std::array<TerrainBoundaryRequest, 4> requests;
    explicit JunctionFixture(std::array<float, 4> heights) {
        for (unsigned q = 0; q < 4; ++q) {
            const bool east = q == 0 || q == 3, north = q < 2;
            meshes[q] = strip(east ? 0.f : -4.f, east ? 4.f : 0.f, heights[q], north ?
                std::initializer_list<float>{0, 4} : std::initializer_list<float>{-4, 0});
            sources[q] = {&meshes[q]};
        }
        requests = {{
            {{TerrainBoundaryAxis::Z, 0, 0, 4}, ids[3], ids[0], 13, 29}, // E
            {{TerrainBoundaryAxis::X, 0, 0, 4}, ids[1], ids[0], 13, 29}, // N
            {{TerrainBoundaryAxis::Z, 0, -4, 0}, ids[2], ids[1], 13, 29}, // W
            {{TerrainBoundaryAxis::X, 0, -4, 0}, ids[2], ids[3], 13, 29} // S
        }};
    }
};
auto checkJunctionEdges(const TerrainBoundaryComposition& result) -> std::size_t {
    struct Count { unsigned total = 0; int orientation = 0; };
    std::map<std::pair<float, float>, Count> edges;
    for (std::size_t t = 0; t < result.triangles.size(); ++t) {
        const auto& stamp = result.triangles[t];
        if (!stamp.wall) continue;
        for (unsigned e = 0; e < 3; ++e) {
            const auto& a = result.mesh.vertices[result.mesh.indices[t * 3 + e]];
            const auto& b = result.mesh.vertices[result.mesh.indices[t * 3 + (e + 1) % 3]];
            CHECK(a.material == (stamp.positive ? 29 : 13)); CHECK(a.uv.y == a.position.y);
            if (a.position.x != 0 || a.position.z != 0 || b.position.x != 0 || b.position.z != 0) continue;
            REQUIRE(a.position.y != b.position.y);
            auto& count = edges[{std::min(a.position.y, b.position.y), std::max(a.position.y, b.position.y)}];
            ++count.total; count.orientation += a.position.y < b.position.y ? 1 : -1;
        }
    }
    for (const auto& [edge, count] : edges) { CHECK(count.total == 2); CHECK(count.orientation == 0); }
    return edges.size();
}
}

TEST_CASE("Boundary junction: T and four-arm corners split vertical edges with exact paired ownership") {
    for (unsigned variant = 0; variant < 4; ++variant) {
        const bool fourArms = (variant & 1u) != 0;
        auto heights = fourArms ? std::array{6.f, 4.f, 0.f, 2.f} : std::array{6.f, 4.f, 2.f, 6.f};
        if (variant >= 2) for (auto& h : heights) h = 10 - h; // Exercise negative-side wall ownership too.
        JunctionFixture fixture(heights);
        const std::span<const TerrainBoundaryRequest> requests{fixture.requests.data() + (fourArms ? 0 : 1), fourArms ? 4u : 3u};
        const auto built = composeTerrainBoundaryReplacements(fixture.sources, requests); REQUIRE(built.hasValue());
        const auto& result = built.value();
        CHECK(checkJunctionEdges(result) == (fourArms ? 3 : 2));
        unsigned walls = 0; double area = 0;
        for (std::size_t i = 0; i < result.triangles.size(); ++i) {
            const auto& stamp = result.triangles[i]; if (!stamp.wall) continue;
            ++walls;
            CHECK(stamp.sourceTriangle < 2);
            const auto& request = requests[stamp.boundaryIndex];
            CHECK(stamp.sourceIndex == (stamp.positive ? request.positive[0] : request.negative[0]));
            const auto a = result.mesh.vertices[result.mesh.indices[i * 3]].position,
                b = result.mesh.vertices[result.mesh.indices[i * 3 + 1]].position,
                c = result.mesh.vertices[result.mesh.indices[i * 3 + 2]].position;
            const bool xAxis = request.line.axis == TerrainBoundaryAxis::X;
            const auto tangent = [&](math::Vec3 p) { return xAxis ? p.z : p.x; };
            area += std::abs((double(b.y) - a.y) * (double(tangent(c)) - tangent(a)) -
                            (double(tangent(b)) - tangent(a)) * (double(c.y) - a.y)) * 0.5;
            const auto center = (a + b + c) / 3.f;
            const float sign = stamp.positive ? -1.f : 1.f;
            const math::Vec3 outward = xAxis ? math::Vec3{sign, 0, 0} : math::Vec3{0, 0, sign};
            const auto hit = raycastTerrainMesh(result.mesh, center + outward * 0.25f, -outward, 0.5, {}, true);
            REQUIRE(hit); CHECK(hit->distance == doctest::Approx(0.25));
            CHECK(result.triangles[hit->triangleIndex].boundaryIndex == stamp.boundaryIndex);
            CHECK_FALSE(raycastTerrainMesh(result.mesh, center - outward * 0.25f, outward, 0.5, {}, true));
        }
        CHECK(walls == (fourArms ? 10 : 7)); CHECK(area == (fourArms ? 48 : 32));
        std::vector<TerrainBoundaryRequest> reversed(requests.rbegin(), requests.rend());
        const auto again = composeTerrainBoundaryReplacements(fixture.sources, reversed); REQUIRE(again.hasValue());
        REQUIRE(again.value().mesh.vertices.size() == result.mesh.vertices.size());
        for (std::size_t i = 0; i < result.mesh.vertices.size(); ++i)
            CHECK(again.value().mesh.vertices[i].position == result.mesh.vertices[i].position);
        BoundaryCompositionLimits limits; limits.mesh.maxBytes = payloadBytes(result);
        CHECK(composeTerrainBoundaryReplacements(fixture.sources, requests, limits).hasValue());
        --limits.mesh.maxBytes; CHECK(composeTerrainBoundaryReplacements(fixture.sources, requests, limits).error() == BoundaryReplacementError::LimitExceeded);
        limits = {}; limits.mesh.maxTriangles = result.triangles.size();
        CHECK(composeTerrainBoundaryReplacements(fixture.sources, requests, limits).hasValue());
        --limits.mesh.maxTriangles; CHECK(composeTerrainBoundaryReplacements(fixture.sources, requests, limits).error() == BoundaryReplacementError::LimitExceeded);
    }
}

TEST_CASE("Boundary junction: consistent L and straight continuations pass; missing arms and saddles reject") {
    using Error = BoundaryReplacementError;
    // All sources are explicitly listed, including point-only contacts at the corner.
    JunctionFixture corner({4, 4, 2, 4});
    const std::array<std::size_t, 2> north{0, 1};
    auto west = corner.requests[2]; west.positive = north;
    const std::array lShape{west, corner.requests[3]};
    const auto l = composeTerrainBoundaryReplacements(corner.sources, lShape); REQUIRE(l.hasValue());
    CHECK(checkJunctionEdges(l.value()) == 1);
    JunctionFixture inconsistent({6, 4, 2, 5});
    CHECK(composeTerrainBoundaryReplacements(inconsistent.sources, {inconsistent.requests.data() + 1, 3}).error() == Error::JunctionRejected);
    JunctionFixture saddle({4, 0, 4, 0});
    CHECK(composeTerrainBoundaryReplacements(saddle.sources, saddle.requests).error() == Error::JunctionRejected);
    JunctionFixture flat({2, 2, 2, 2});
    const auto equal = composeTerrainBoundaryReplacements(flat.sources, flat.requests); REQUIRE(equal.hasValue());
    CHECK(std::none_of(equal.value().triangles.begin(), equal.value().triangles.end(), [](const auto& t) { return t.wall; }));
    const auto n = strip(-4, 0, 0, {-4, 0, 4}), p = strip(0, 4, 2, {-4, 0, 4});
    const std::array sources{TerrainBoundaryMesh{&n}, TerrainBoundaryMesh{&p}};
    const std::array<std::size_t, 1> negative{0}, positive{1};
    const std::array straight{
        TerrainBoundaryRequest{{TerrainBoundaryAxis::X, 0, -4, 0}, negative, positive, 13, 29},
        TerrainBoundaryRequest{{TerrainBoundaryAxis::X, 0, 0, 4}, negative, positive, 13, 29}};
    const auto joined = composeTerrainBoundaryReplacements(sources, straight); REQUIRE(joined.hasValue());
    CHECK(checkJunctionEdges(joined.value()) == 1);
}

TEST_CASE("Boundary junction: walls taper to a shared tip and unrepresentable junction positions reject") {
    JunctionFixture corner({4, 4, 2, 4});
    for (auto& mesh : corner.meshes)
        for (auto& v : mesh.vertices)
            if (v.position.x == 0 && v.position.z == 0 && v.position.y != -4) v.position.y = 2;
    const std::array<std::size_t, 2> north{0, 1};
    auto west = corner.requests[2]; west.positive = north;
    std::array lShape{west, corner.requests[3]};
    const auto tip = composeTerrainBoundaryReplacements(corner.sources, lShape); REQUIRE(tip.hasValue());
    CHECK(checkJunctionEdges(tip.value()) == 0);
    std::size_t tips = 0;
    for (std::size_t t = 0; t < tip.value().triangles.size(); ++t) {
        if (!tip.value().triangles[t].wall) continue;
        for (unsigned e = 0; e < 3; ++e) {
            const auto p = tip.value().mesh.vertices[tip.value().mesh.indices[t * 3 + e]].position;
            if (p.x == 0 && p.z == 0) { CHECK(p.y == 2); ++tips; }
        }
    }
    CHECK(tips == 2);
    for (auto& r : lShape) r.line = {r.line.axis, 16777217, 0, 16777217};
    CHECK(composeTerrainBoundaryReplacements(corner.sources, lShape).error() == BoundaryReplacementError::PrecisionLoss);
}

TEST_CASE("Boundary composition: separated strips split the same original top and retain untouched caps") {
    const auto center = strip(0, 4, 2, {0, 4}), left = strip(-4, 0, 0, {0, 0.5f, 1, 3, 3.5f, 4});
    const math::Vec3 offset{64, 8, -128};
    const std::array sources{TerrainBoundaryMesh{&center, offset}, TerrainBoundaryMesh{&left, offset}};
    const std::array<std::size_t, 1> c{0}, l{1};
    const std::array requests{
        TerrainBoundaryRequest{{TerrainBoundaryAxis::X, 64, -128, -127}, l, c},
        TerrainBoundaryRequest{{TerrainBoundaryAxis::X, 64, -125, -124}, l, c}};
    const auto built = composeTerrainBoundaryReplacements(sources, requests); REQUIRE(built.hasValue());
    const auto& result = built.value();
    std::size_t splitTop = 0;
    for (const auto& stamp : result.triangles)
        if (!stamp.wall && stamp.sourceIndex == 0 && stamp.sourceTriangle == 0) ++splitTop;
    CHECK(splitTop == 5);
    for (const float z : {-127.75f, -124.75f}) {
        const auto hit = raycastTerrainMesh(result.mesh, {63, 9, z}, {1, 0, 0}, 2, {}, true);
        REQUIRE(hit); CHECK(hit->distance == 1);
        CHECK(result.triangles[hit->triangleIndex].wall);
        CHECK(result.triangles[hit->triangleIndex].sourceTriangle == 0);
        CHECK_FALSE(raycastTerrainMesh(result.mesh, {63, 6, z}, {1, 0, 0}, 2));
    }
    const auto retained = raycastTerrainMesh(result.mesh, {63, 6, -126}, {1, 0, 0}, 2);
    REQUIRE(retained); CHECK_FALSE(result.triangles[retained->triangleIndex].wall);
    // Every generated horizontal wall edge must still have one complete source
    // top edge after BOTH splits (not just a collinear partial overlap).
    std::size_t matchesChecked = 0;
    for (std::size_t i = 0; i < result.triangles.size(); ++i) {
        if (!result.triangles[i].wall) continue;
        for (unsigned j = 0; j < 3; ++j) {
            const auto a = result.mesh.vertices[result.mesh.indices[i * 3 + j]].position;
            const auto b = result.mesh.vertices[result.mesh.indices[i * 3 + (j + 1) % 3]].position;
            if (a.y != b.y || a.z == b.z) continue;
            unsigned matches = 0;
            for (std::size_t t = 0; t < result.triangles.size(); ++t) {
                const auto& stamp = result.triangles[t];
                if (stamp.wall || (stamp.sourceIndex == 0 ? stamp.sourceTriangle >= 2 : stamp.sourceTriangle >= 10)) continue;
                for (unsigned e = 0; e < 3; ++e) {
                    const auto p = result.mesh.vertices[result.mesh.indices[t * 3 + e]].position;
                    const auto q = result.mesh.vertices[result.mesh.indices[t * 3 + (e + 1) % 3]].position;
                    if ((a == p && b == q) || (a == q && b == p)) ++matches;
                }
            }
            CHECK(matches == 1); ++matchesChecked;
        }
    }
    CHECK(matchesChecked == 8);
}
