#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <doctest/doctest.h>

#include "engine/world/boundary_discovery.h"
#include "engine/world/terrain_solid.h"
#include "engine/world/world_streaming.h"

namespace {
using namespace f4;
using namespace f4::world;

auto testConfig(PolicyKind policy, std::uint32_t radius = 0) -> WorldStreamingConfig {
    WorldStreamingConfig config;
    config.worldSeed = 0x1616'cafe'1234'5678ull;
    config.defaultPolicy = policy;
    config.horizontalRadius = radius;
    config.voxelVerticalRadius = 0;
    config.workerCount = 2;
    config.frameBudget = std::chrono::seconds(1);
    config.maxLoadsPerFrame = 64;
    config.maxUnloadsPerFrame = 64;
    config.maxPublicationsPerFrame = 64;
    config.maxMeshJobsPerFrame = 64;
    config.policyParams.hex.radius = 2;
    return config;
}

auto settle(WorldStreamer& streamer, math::Vec3 camera, std::size_t expected, std::uint32_t maxFrames = 64)
    -> bool {
    for (std::uint32_t frame = 0; frame < maxFrames; ++frame) {
        streamer.update(camera);
        streamer.waitForIdle();
        const auto chunks = streamer.residentChunks();
        bool meshed = chunks.size() == expected;
        for (const StreamingChunkInfo& chunk : chunks) {
            meshed = meshed && chunk.state == ChunkState::Meshed && chunk.hasMesh;
        }
        if (meshed && !streamer.hasPendingWork()) return true;
    }
    return false;
}

auto settleReady(WorldStreamer& streamer, math::Vec3 camera, std::uint32_t maxFrames = 64) -> bool {
    for (std::uint32_t frame = 0; frame < maxFrames; ++frame) {
        streamer.update(camera);
        streamer.waitForIdle();
        if (streamer.residentCount() > 0 && !streamer.hasPendingWork()) return true;
    }
    return false;
}

struct ColumnProbe { double x, y, z; };

auto upwardCentroid(const MeshData& mesh, math::Vec3 origin) -> std::optional<ColumnProbe> {
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const auto ia = mesh.indices[index];
        const auto ib = mesh.indices[index + 1];
        const auto ic = mesh.indices[index + 2];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
        const auto sample = [&](std::uint32_t id) {
            const auto& position = mesh.vertices[id].position;
            return TerrainTopSample{double(position.x) + origin.x, double(position.y) + origin.y,
                                    double(position.z) + origin.z};
        };
        const auto a = sample(ia), b = sample(ib), c = sample(ic);
        const double area = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        if (!(area > 0)) continue;
        const double x = (a.x + b.x + c.x) / 3.0;
        const double z = (a.z + b.z + c.z) / 3.0;
        if (x == 0.0 || z == 0.0) continue;
        const auto height = sampleUpwardTriangleHeight(a, b, c, x, z);
        if (!height.hasValue()) continue;
        return ColumnProbe{x, height.value(), z};
    }
    return std::nullopt;
}
} // namespace

TEST_CASE("Streaming solid: height columns use the published triangle, including across edits") {
    WorldStreamer world(testConfig(PolicyKind::Height));
    CHECK(world.classifyPublishedSolid(0.5, 0.0, 0.5).error() == StreamingSolidError::NoPublishedSurface);
    CHECK(world.classifyPublishedSolid(std::nan(""), 0.0, 0.5).error() == StreamingSolidError::InvalidSample);
    const math::Vec3 camera{32.f, 0.f, 32.f};
    REQUIRE(settle(world, camera, 1));

    const StreamingChunkKey key{PolicyKind::Height, {}};
    const auto editQuad = [&](float h00, float h10, float h01, float h11) {
        const float heights[]{h00, h10, h01, h11};
        const std::uint32_t indices[]{heightSampleIndex(0, 0), heightSampleIndex(1, 0),
                                      heightSampleIndex(0, 1), heightSampleIndex(1, 1)};
        for (int i = 0; i < 4; ++i) {
            HeightSample sample;
            sample.height = heights[i];
            REQUIRE(world.edit(key, indices[i], CellValue::heightValue(sample)));
        }
    };
    const auto beforeEdit = world.classifyPublishedSolid(0.2, 0.35, 0.7);
    REQUIRE(beforeEdit.hasValue());
    editQuad(0.f, 0.f, 0.f, 2.f);
    const auto editedCells = world.sampleHeightTerrain(0.2f, 0.7f);
    REQUIRE(editedCells.has_value());
    CHECK(editedCells->height == doctest::Approx(0.28f));
    const auto retained = world.classifyPublishedSolid(0.2, 0.35, 0.7);
    REQUIRE(retained.hasValue());
    CHECK(retained.value().solid == beforeEdit.value().solid);
    REQUIRE(settle(world, camera, 1));

    const auto bilinear = world.sampleHeightTerrain(0.2f, 0.7f);
    REQUIRE(bilinear.has_value());
    CHECK(bilinear->height == doctest::Approx(0.28f));
    const auto triangleTop = sampleUpwardTriangleHeight({0, 0, 0}, {0, 0, 1}, {1, 2, 1}, 0.2, 0.7);
    REQUIRE(triangleTop.hasValue());
    CHECK(double(bilinear->height) < triangleTop.value());
    const double between = (double(bilinear->height) + triangleTop.value()) * 0.5;
    const auto inside = world.classifyPublishedSolid(0.2, between, 0.7);
    REQUIRE(inside.hasValue());
    CHECK(inside.value().solid);
    CHECK(inside.value().policy == PolicyKind::Height);
    CHECK_FALSE(inside.value().wallBand);
    const auto closedTop = world.classifyPublishedSolid(0.2, triangleTop.value(), 0.7);
    REQUIRE(closedTop.hasValue());
    CHECK(closedTop.value().solid);
    const auto above = world.classifyPublishedSolid(0.2, triangleTop.value() + 0.05, 0.7);
    REQUIRE(above.hasValue());
    CHECK_FALSE(above.value().solid);
    const auto buried = world.classifyPublishedSolid(0.2, -1000.0, 0.7);
    REQUIRE(buried.hasValue());
    CHECK(buried.value().solid);

    const auto beforeLower = world.classifyPublishedSolid(0.2, 0.0, 0.7);
    REQUIRE(beforeLower.hasValue());
    CHECK(beforeLower.value().solid);
    editQuad(-5.f, -5.f, -5.f, -5.f);
    const auto during = world.classifyPublishedSolid(0.2, 0.0, 0.7);
    REQUIRE(during.hasValue());
    CHECK(during.value().solid == beforeLower.value().solid);
    REQUIRE(settle(world, camera, 1));
    const auto lowered = world.classifyPublishedSolid(0.2, 0.0, 0.7);
    REQUIRE(lowered.hasValue());
    CHECK_FALSE(lowered.value().solid);
    CHECK(world.sampleHeightTerrain(0.2f, 0.7f)->height == doctest::Approx(-5.f));
}

TEST_CASE("Streaming solid: camera travel keeps a resident column and drops it on unload") {
    WorldStreamer world(testConfig(PolicyKind::Height, 1));
    const math::Vec3 home{32.f, 0.f, 32.f};
    REQUIRE(settle(world, home, 9));
    const auto before = world.classifyPublishedSolid(32.0, -1000.0, 32.0);
    REQUIRE(before.hasValue());
    CHECK(before.value().solid);
    CHECK(before.value().policy == PolicyKind::Height);
    for (float x = 0.f; x <= 96.f; x += 16.f) {
        world.update({x, 0.f, 32.f});
        world.waitForIdle();
        if (!world.chunk({PolicyKind::Height, {}})) continue;
        const auto moving = world.classifyPublishedSolid(32.0, -1000.0, 32.0);
        REQUIRE(moving.hasValue());
        CHECK(moving.value().solid);
        CHECK(moving.value().policy == PolicyKind::Height);
    }
    REQUIRE(settle(world, {400.f, 0.f, 32.f}, 9));
    CHECK_FALSE(world.chunk({PolicyKind::Height, {}}));
    CHECK(world.classifyPublishedSolid(32.0, -1000.0, 32.0).error() == StreamingSolidError::NoPublishedSurface);
}

TEST_CASE("Streaming solid: a region change during travel does not keep the old owner") {
    WorldStreamer world(testConfig(PolicyKind::Height));
    const math::Vec3 camera{32.f, 0.f, 32.f};
    REQUIRE(settle(world, camera, 1));
    const auto height = world.classifyPublishedSolid(32.0, -10.0, 32.0);
    REQUIRE(height.hasValue());
    CHECK(height.value().policy == PolicyKind::Height);
    CHECK(height.value().solid);
    world.regions().set({0, 0}, PolicyKind::Hex);
    world.update(camera);
    const auto transitioning = world.classifyPublishedSolid(32.0, -10.0, 32.0);
    if (transitioning.hasValue()) CHECK(transitioning.value().policy == PolicyKind::Hex);
    REQUIRE(settle(world, camera, 1));
    const auto floor = double(testConfig(PolicyKind::Height).hexFloorHeight);
    const auto hex = world.classifyPublishedSolid(32.0, floor, 32.0);
    REQUIRE(hex.hasValue());
    CHECK(hex.value().policy == PolicyKind::Hex);
    CHECK(hex.value().solid);
    const auto under = world.classifyPublishedSolid(32.0, floor - 1.0, 32.0);
    REQUIRE(under.hasValue());
    CHECK_FALSE(under.value().solid);
}

TEST_CASE("Streaming solid: hex prisms are closed at the floor") {
    WorldStreamer hexes(testConfig(PolicyKind::Hex));
    const math::Vec3 camera{8.f, 0.f, 8.f};
    REQUIRE(settle(hexes, camera, 1));
    const auto resident = hexes.residentChunks();
    REQUIRE(resident.size() == 1);
    const MeshData* mesh = hexes.mesh(resident[0].key);
    REQUIRE(mesh != nullptr);
    const auto centroid = upwardCentroid(*mesh, hexes.meshOrigin(resident[0].key));
    REQUIRE(centroid.has_value());
    const double floor = double(testConfig(PolicyKind::Hex).hexFloorHeight);
    const auto top = hexes.classifyPublishedSolid(centroid->x, centroid->y, centroid->z);
    REQUIRE(top.hasValue());
    CHECK(top.value().policy == PolicyKind::Hex);
    CHECK(top.value().solid);
    const auto above = hexes.classifyPublishedSolid(centroid->x, centroid->y + 1.0, centroid->z);
    REQUIRE(above.hasValue());
    CHECK_FALSE(above.value().solid);
    const auto atFloor = hexes.classifyPublishedSolid(centroid->x, floor, centroid->z);
    REQUIRE(atFloor.hasValue());
    CHECK(atFloor.value().solid);
    const auto below = hexes.classifyPublishedSolid(centroid->x, floor - 1.0, centroid->z);
    REQUIRE(below.hasValue());
    CHECK_FALSE(below.value().solid);
}

TEST_CASE("Streaming solid: published voxel columns own solid empty and overhang spans") {
    WorldStreamer voxels(testConfig(PolicyKind::Voxel));
    const math::Vec3 probe{1.5, 2.5, 1.5};
    CHECK(voxels.classifyPublishedSolid(probe.x, probe.y, probe.z).error() == StreamingSolidError::NoPublishedSurface);
    REQUIRE(settle(voxels, {8.f, 8.f, 8.f}, 1));
    const auto residents = voxels.residentChunks();
    REQUIRE(residents.size() == 1);
    REQUIRE(residents[0].key.policy == PolicyKind::Voxel);

    const auto solid = voxels.classifyPublishedSolid(probe.x, 1.5, probe.z);
    const auto cave = voxels.classifyPublishedSolid(probe.x, probe.y, probe.z);
    const auto roof = voxels.classifyPublishedSolid(probe.x, 3.5, probe.z);
    const auto sky = voxels.classifyPublishedSolid(probe.x, 40.0, probe.z);
    REQUIRE(solid.hasValue());
    REQUIRE(cave.hasValue());
    REQUIRE(roof.hasValue());
    REQUIRE(sky.hasValue());
    CHECK(solid.value().solid);
    CHECK(solid.value().policy == PolicyKind::Voxel);
    CHECK(solid.value().sector == 0);
    CHECK(cave.value().solid);
    CHECK(cave.value().policy == PolicyKind::Voxel);
    CHECK(roof.value().solid);
    CHECK(roof.value().policy == PolicyKind::Voxel);
    CHECK_FALSE(sky.value().solid);
    CHECK(sky.value().policy == PolicyKind::Voxel);

    REQUIRE(voxels.edit(residents[0].key, voxelIndex(1, 2, 1), CellValue::voxelValue(0)));
    const auto retained = voxels.classifyPublishedSolid(probe.x, probe.y, probe.z);
    REQUIRE(retained.hasValue());
    CHECK(retained.value().solid);
    CHECK(retained.value().policy == PolicyKind::Voxel);
    REQUIRE(settle(voxels, {8.f, 8.f, 8.f}, 1));

    const auto belowHole = voxels.classifyPublishedSolid(probe.x, 1.5, probe.z);
    const auto hole = voxels.classifyPublishedSolid(probe.x, probe.y, probe.z);
    const auto aboveHole = voxels.classifyPublishedSolid(probe.x, 3.5, probe.z);
    const auto stillSky = voxels.classifyPublishedSolid(probe.x, 40.0, probe.z);
    REQUIRE(belowHole.hasValue());
    REQUIRE(hole.hasValue());
    REQUIRE(aboveHole.hasValue());
    REQUIRE(stillSky.hasValue());
    CHECK(belowHole.value().solid);
    CHECK(belowHole.value().policy == PolicyKind::Voxel);
    CHECK_FALSE(hole.value().solid);
    CHECK(hole.value().policy == PolicyKind::Voxel);
    CHECK(aboveHole.value().solid);
    CHECK(aboveHole.value().policy == PolicyKind::Voxel);
    CHECK_FALSE(stillSky.value().solid);
    CHECK(stillSky.value().policy == PolicyKind::Voxel);
}

TEST_CASE("Streaming solid: height/hex boundary and corner agree with the reference classifiers") {
    auto config = testConfig(PolicyKind::Height);
    config.horizontalHalfExtentMeters = 96.f;
    config.chunkCapacityPerPolicy = 25;
    config.regionExtentMeters = 64;
    config.hexRadius = 8.f;
    config.regionalSurfaceOwnership = true;
    WorldStreamer world(config);
    world.regions().set({0, 0}, PolicyKind::Hex);
    world.regions().set({-1, 0}, PolicyKind::Voxel);
    const math::Vec3 camera{32.f, 0.f, 32.f};
    REQUIRE(settleReady(world, camera));

    const auto full = world.residentTerrainFootprint();
    REQUIRE(full.has_value());
    const auto scope = std::pair{PolicyKind::Height, PolicyKind::Hex};
    CHECK(discoverTerrainBoundaries(world.regions(), *full).error() == BoundaryDiscoveryError::UnsupportedTransition);
    CHECK(discoverTerrainBoundaries(world.regions(), *full, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    const auto voxelHexMesh = world.residentTerrainFootprint(PolicyKind::Voxel, PolicyKind::Hex);
    REQUIRE(voxelHexMesh.has_value());
    const auto voxelHexWindow = policyPairCompositionWindow(
        world.regions(), *voxelHexMesh, PolicyKind::Voxel, PolicyKind::Hex);
    const auto voxelHex = discoverTerrainBoundaries(
        world.regions(), voxelHexWindow, {}, std::pair{PolicyKind::Voxel, PolicyKind::Hex});
    REQUIRE(voxelHex.hasValue());
    const StreamingBoundaryRequest* voxelEdge = nullptr;
    for (const auto& edge : voxelHex.value()) {
        const bool pair = (edge.negative == PolicyKind::Voxel && edge.positive == PolicyKind::Hex) ||
                          (edge.negative == PolicyKind::Hex && edge.positive == PolicyKind::Voxel);
        CHECK(pair);
        if (edge.line.axis == TerrainBoundaryAxis::X && edge.line.plane == 0.0) voxelEdge = &edge;
    }
    REQUIRE(voxelEdge != nullptr);
    CHECK(voxelHexWindow.minX < voxelEdge->line.plane);
    CHECK(voxelHexWindow.maxX > voxelEdge->line.plane);
    const double voxelTangent = (voxelEdge->line.begin + voxelEdge->line.end) * 0.5;
    CHECK(world.classifyPublishedSolid(voxelEdge->line.plane, 0.0, voxelTangent).error() ==
          StreamingSolidError::UnsupportedVoxel);
    CHECK(discoverTerrainBoundaries(world.regions(), voxelHexWindow, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    const auto scoped = world.residentTerrainFootprint(PolicyKind::Height, PolicyKind::Hex);
    REQUIRE(scoped.has_value());
    const TerrainFootprint containingVoxel{std::min(scoped->minX, -32.0), std::max(scoped->maxX, 96.0),
                                           std::min(scoped->minZ, 0.0), std::max(scoped->maxZ, 64.0)};
    CHECK(discoverTerrainBoundaries(world.regions(), containingVoxel).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    CHECK(discoverTerrainBoundaries(world.regions(), containingVoxel, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    const auto unscopedResident = discoverTerrainBoundaries(world.regions(), *scoped);
    const auto residentScoped = discoverTerrainBoundaries(world.regions(), *scoped, {}, scope);
    if (!unscopedResident.hasValue()) {
        CHECK(unscopedResident.error() == BoundaryDiscoveryError::UnsupportedTransition);
        REQUIRE_FALSE(residentScoped.hasValue());
        CHECK(residentScoped.error() == BoundaryDiscoveryError::UnsupportedTransition);
    } else {
        REQUIRE(residentScoped.hasValue());
        CHECK(residentScoped.value().size() == unscopedResident.value().size());
    }
    const auto edges = discoverTerrainBoundaries(world.regions(), {32.0, 128.0, 0.0, 96.0}, {}, scope);
    REQUIRE(edges.hasValue());
    const StreamingBoundaryRequest* boundary = nullptr;
    for (const auto& edge : edges.value()) {
        if (edge.line.axis == TerrainBoundaryAxis::X && edge.line.plane == 64.0 &&
            edge.line.begin <= 32.0 && edge.line.end >= 32.0) {
            boundary = &edge;
        }
    }
    REQUIRE(boundary != nullptr);
    const StreamingBoundaryRequest fullEdge{{TerrainBoundaryAxis::X, 64, 0, 64}, boundary->negative, boundary->positive};
    const auto captured = world.captureBoundary(fullEdge);
    REQUIRE(captured.hasValue());
    const auto profile = captured.value().profile();
    REQUIRE(profile.hasValue());
    const TerrainBoundarySpan* span = nullptr;
    for (const auto& candidate : profile.value()) {
        if (candidate.begin <= 32.0 && candidate.end >= 32.0) span = &candidate;
    }
    REQUIRE(span != nullptr);
    REQUIRE(span->negative.has_value());
    REQUIRE(span->positive.has_value());
    const double tangent = 32.0;
    const auto lerp = [&](double h0, double h1) {
        if (tangent == span->begin) return h0;
        if (tangent == span->end) return h1;
        return std::lerp(h0, h1, (tangent - span->begin) / (span->end - span->begin));
    };
    const double negativeTop = lerp(span->negative->beginHeight, span->negative->endHeight);
    const double positiveTop = lerp(span->positive->beginHeight, span->positive->endHeight);
    const double lower = std::min(negativeTop, positiveTop);
    const double upper = std::max(negativeTop, positiveTop);
    const double band = lower < upper ? (lower + upper) * 0.5 : lower;
    const auto floorFor = [&](PolicyKind policy) -> std::optional<double> {
        return policy == PolicyKind::Hex ? std::optional<double>{double(config.hexFloorHeight)} : std::nullopt;
    };
    const auto reference = classifyTerrainBoundarySolid(TerrainBoundarySolidQuery{
        fullEdge.line, std::span<const TerrainBoundarySpan>(profile.value().data(), profile.value().size()),
        fullEdge.negative, fullEdge.positive, floorFor(fullEdge.negative), floorFor(fullEdge.positive),
        tangent, band});
    const auto streamed = world.classifyPublishedSolid(64.0, band, tangent);
    REQUIRE(reference.hasValue());
    REQUIRE(streamed.hasValue());
    CHECK(streamed.value().solid == reference.value().solid);
    CHECK(streamed.value().policy == reference.value().policy);
    CHECK(streamed.value().sector == reference.value().sector);
    CHECK(streamed.value().wallBand == reference.value().wallBand);
    CHECK(streamed.value().solid);
    if (lower < upper) CHECK(streamed.value().wallBand);

    const auto above = world.classifyPublishedSolid(64.0, upper + 10.0, tangent);
    REQUIRE(above.hasValue());
    CHECK_FALSE(above.value().solid);
    const auto voxelChunk = world.chunk(StreamingChunkKey{PolicyKind::Voxel, {-1, 0, 0}});
    const auto voxelInterior = world.classifyPublishedSolid(-31.5, 1.0, 31.5);
    if (voxelChunk && voxelChunk->hasMesh) {
        REQUIRE(voxelInterior.hasValue());
        CHECK(voxelInterior.value().policy == PolicyKind::Voxel);
        CHECK(voxelInterior.value().solid);
    } else {
        CHECK(voxelInterior.error() == StreamingSolidError::NoPublishedSurface);
    }

    const PolicyKind sectorPolicy[]{world.regions().policyAt(64, 64), world.regions().policyAt(63, 64),
                                    world.regions().policyAt(63, 63), world.regions().policyAt(64, 63)};
    std::array<std::optional<double>, 4> sectorTops;
    for (const auto& chunk : world.residentChunks()) {
        const MeshData* mesh = world.mesh(chunk.key);
        if (mesh == nullptr) continue;
        const auto origin = world.meshOrigin(chunk.key);
        for (std::size_t index = 0; index + 2 < mesh->indices.size(); index += 3) {
            const auto ia = mesh->indices[index], ib = mesh->indices[index + 1], ic = mesh->indices[index + 2];
            if (ia >= mesh->vertices.size() || ib >= mesh->vertices.size() || ic >= mesh->vertices.size()) continue;
            const auto position = [&](std::uint32_t id) {
                const auto& vertex = mesh->vertices[id].position;
                return TerrainTopSample{double(vertex.x) + origin.x, double(vertex.y) + origin.y,
                                        double(vertex.z) + origin.z};
            };
            const auto a = position(ia), b = position(ib), c = position(ic);
            const auto sampled = sampleUpwardTriangleHeight(a, b, c, 64.0, 64.0);
            if (!sampled.hasValue()) continue;
            const double cx = (a.x + b.x + c.x) / 3.0;
            const double cz = (a.z + b.z + c.z) / 3.0;
            int sector = -1;
            if (cx > 64.0 && cz > 64.0) sector = 0;
            else if (cx < 64.0 && cz > 64.0) sector = 1;
            else if (cx < 64.0 && cz < 64.0) sector = 2;
            else if (cx > 64.0 && cz < 64.0) sector = 3;
            if (sector < 0 || chunk.key.policy != sectorPolicy[sector]) continue;
            if (sectorTops[static_cast<std::size_t>(sector)] &&
                *sectorTops[static_cast<std::size_t>(sector)] != sampled.value()) {
                sectorTops[static_cast<std::size_t>(sector)].reset();
                break;
            }
            sectorTops[static_cast<std::size_t>(sector)] = sampled.value();
        }
    }
    const bool complete = std::all_of(sectorTops.begin(), sectorTops.end(), [](const auto& top) { return top.has_value(); });
    const auto corner = world.classifyPublishedSolid(64.0, band, 64.0);
    if (!complete) {
        CHECK(corner.error() == StreamingSolidError::NoPublishedSurface);
    } else {
        std::array<TerrainSolidColumn, 4> sectors;
        for (std::size_t i = 0; i < 4; ++i) {
            sectors[i] = {sectorPolicy[i], *sectorTops[i],
                          sectorPolicy[i] == PolicyKind::Hex ? std::optional<double>{double(config.hexFloorHeight)}
                                                             : std::nullopt};
        }
        const auto referenceCorner = classifyTerrainCornerSolid(sectors, band);
        REQUIRE(referenceCorner.hasValue());
        REQUIRE(corner.hasValue());
        CHECK(corner.value().solid == referenceCorner.value().solid);
        CHECK(corner.value().policy == referenceCorner.value().policy);
        CHECK(corner.value().sector == referenceCorner.value().sector);
        CHECK(corner.value().wallBand == referenceCorner.value().wallBand);
    }
}
