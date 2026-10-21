#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <map>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "core/jobs/job_system.h"
#include "engine/world/world_streaming.h"
#include "engine/world/terrain_boundaries.h"

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

auto settle(WorldStreamer& streamer, math::Vec3 camera, std::size_t expected,
            std::uint32_t maxFrames = 64) -> bool {
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

void hashValue(std::uint64_t& hash, std::uint64_t value, std::uint32_t bytes) {
    for (std::uint32_t byte = 0; byte < bytes; ++byte) {
        hash ^= (value >> (byte * 8u)) & 0xffu;
        hash *= 1099511628211ull;
    }
}

auto meshDigest(const MeshData& mesh) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    for (const TerrainVertex& vertex : mesh.vertices) {
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.position.x), 4);
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.position.y), 4);
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.position.z), 4);
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.normal.x), 4);
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.normal.y), 4);
        hashValue(hash, std::bit_cast<std::uint32_t>(vertex.normal.z), 4);
    }
    for (std::uint32_t index : mesh.indices) hashValue(hash, index, 4);
    return hash;
}

} // namespace

TEST_CASE("M16: published terrain rays preserve mesh provenance through edits and eviction") {
    auto config = testConfig(PolicyKind::Height);
    WorldStreamer streamer(config);
    const StreamingChunkKey key{PolicyKind::Height, {}};
    const math::Vec3 eye{32.25f, 100.f, 32.75f};
    CHECK_FALSE(streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f));
    REQUIRE(settle(streamer, {}, 1));
    const auto before = streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f, true);
    REQUIRE(before.has_value());
    CHECK(before->key == key);
    CHECK(before->meshVersion == streamer.chunk(key)->version);
    CHECK(before->meshVersion == streamer.chunk(key)->meshVersion);
    CHECK(before->handle == streamer.chunk(key)->handle);
    REQUIRE(streamer.edit(key, heightSampleIndex(33, 33), CellValue::heightValue({60.f, 0, {255, 0, 0, 0}})));
    REQUIRE(streamer.chunk(key)->version > before->meshVersion);
    const auto retained = streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f);
    REQUIRE(retained.has_value());
    CHECK(retained->meshVersion == before->meshVersion);
    CHECK(retained->surface.distance == before->surface.distance);
    REQUIRE(settle(streamer, {}, 1));
    const auto edited = streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f);
    REQUIRE(edited.has_value());
    CHECK(edited->meshVersion == streamer.chunk(key)->version);
    CHECK(edited->meshVersion > before->meshVersion);
    CHECK(edited->surface.distance < before->surface.distance);
    REQUIRE(settle(streamer, {512, 0, 0}, 1));
    CHECK_FALSE(streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f));
    REQUIRE(settle(streamer, {}, 1));
    const auto returned = streamer.raycastPublishedTerrain(eye, {0, -1, 0}, 200.f);
    REQUIRE(returned.has_value());
    CHECK(returned->handle != before->handle);
    CHECK(returned->surface.distance == edited->surface.distance);
}

TEST_CASE("M16: published terrain rays cover voxel and signed scaled hex surfaces") {
    for (const auto policy : {PolicyKind::Voxel, PolicyKind::Hex}) {
        for (const unsigned workers : {1u, 4u}) {
            auto config = testConfig(policy);
            config.workerCount = workers;
            config.hexRadius = 3.f;
            WorldStreamer streamer(config);
            const StreamingChunkKey key{policy, {-3, 0, 2}};
            const auto origin = streamer.meshOrigin(key);
            const math::Vec3 anchor = policy == PolicyKind::Hex ? origin : origin + math::Vec3{16.5f, 0, 16.5f};
            REQUIRE(settle(streamer, anchor, 1));
            const auto hit = streamer.raycastPublishedTerrain(anchor + math::Vec3{0, 100, 0}, {0, -9, 0}, 200.f, true);
            REQUIRE(hit.has_value());
            CHECK(hit->key == key);
            CHECK(hit->surface.normal.y > 0.f);
            CHECK(hit->meshVersion == streamer.chunk(key)->version);
            const auto direct = raycastTerrainMesh(*streamer.mesh(key), anchor + math::Vec3{0, 100, 0},
                                                   {0, -9, 0}, 200.f, origin, true);
            REQUIRE(direct.has_value());
            CHECK(direct->distance == hit->surface.distance);
            CHECK(direct->triangleIndex == hit->surface.triangleIndex);
            if (policy == PolicyKind::Voxel) {
                const auto cells = streamer.raycastVoxelTerrain(anchor + math::Vec3{0, 100, 0}, {0, -1, 0}, 200.f);
                REQUIRE(cells.has_value());
                CHECK(cells->t == doctest::Approx(hit->surface.distance));
            } else {
                const auto underside = streamer.raycastPublishedTerrain(anchor + math::Vec3{0, -20, 0}, {0, 1, 0}, 40.f, true);
                REQUIRE(underside.has_value());
                CHECK(underside->surface.position.y == config.hexFloorHeight);
                CHECK(underside->surface.normal.y == -1.f);
            }
        }
    }
}

TEST_CASE("M16: exact published surface ties use chunk keys rather than publication order") {
    for (const unsigned workers : {1u, 4u}) {
        auto config = testConfig(PolicyKind::Height, 1);
        config.workerCount = workers;
        WorldStreamer streamer(config);
        REQUIRE(settle(streamer, {}, 9));
        // A shared corner has the same position and height in four chunks.
        const auto hit = streamer.raycastPublishedTerrain({0, 100, 0}, {0, -1, 0}, 200.f, true);
        REQUIRE(hit.has_value());
        CHECK(hit->key == StreamingChunkKey{PolicyKind::Height, {-1, 0, -1}});
        CHECK(hit->meshVersion == streamer.chunk(hit->key)->version);
    }
}

TEST_CASE("M16: regional surface ownership fills hex coverage and survives map edits") {
    const auto sameMesh = [](const MeshData& a, const MeshData& b) {
        if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size() || a.surfaces.size() != b.surfaces.size()) return false;
        for (std::size_t i = 0; i < a.vertices.size(); ++i) {
            const auto& u = a.vertices[i]; const auto& v = b.vertices[i];
            if (u.position != v.position || u.normal != v.normal || u.uv != v.uv || u.color != v.color ||
                u.materialWeights != v.materialWeights || u.material != v.material || u.biome != v.biome ||
                u.ambientOcclusion != v.ambientOcclusion) return false;
        }
        for (std::size_t i = 0; i < a.surfaces.size(); ++i) {
            const auto& u = a.surfaces[i]; const auto& v = b.surfaces[i];
            if (u.firstIndex != v.firstIndex || u.indexCount != v.indexCount || u.material != v.material) return false;
        }
        return std::equal(a.indices.begin(), a.indices.end(), b.indices.begin());
    };
    for (const auto& [workers, cutWalls] : {std::pair{1u, false}, std::pair{4u, false}, std::pair{1u, true}, std::pair{4u, true}}) {
        auto config = testConfig(PolicyKind::Height);
        config.workerCount = workers;
        config.horizontalHalfExtentMeters = 96.f;
        config.chunkCapacityPerPolicy = 25;
        config.regionExtentMeters = 64;
        config.hexRadius = 8.f;
        config.regionalSurfaceOwnership = true;
        config.regionalHexCutWalls = cutWalls;
        WorldStreamer world(config);
        world.regions().set({-1, 0}, PolicyKind::Voxel);
        world.regions().set({0, 0}, PolicyKind::Hex);
        REQUIRE(settle(world, {}, 22));
        CHECK(world.chunk({PolicyKind::Hex, {-1, 0, 1}}).has_value());
        CHECK(world.chunk({PolicyKind::Hex, {1, 0, 0}}).has_value());
        for (int z = 4; z < 64; z += 8) {
            for (int x = 4; x < 64; x += 8) {
                const auto hit = world.raycastPublishedTerrain({float(x), 100, float(z)}, {0, -1, 0}, 200, true);
                REQUIRE(hit.has_value());
                CHECK(hit->key.policy == PolicyKind::Hex);
                CHECK(hit->meshRegionRevision == world.regions().revision());
            }
        }
        const auto gap = world.raycastPublishedTerrain({8, 100, 56}, {0, -1, 0}, 200, true);
        REQUIRE(gap.has_value());
        CHECK(gap->key.policy == PolicyKind::Hex);
        if (cutWalls) {
            for (int z = 4; z < 64; z += 8) {
                const auto hit = world.raycastPublishedTerrain({66, -4, float(z)}, {-1, 0, 0}, 3, true);
                REQUIRE(hit.has_value());
                CHECK(hit->key.policy == PolicyKind::Hex);
                CHECK(hit->surface.normal == math::Vec3{1, 0, 0});
                CHECK(hit->surface.distance == doctest::Approx(2).epsilon(1e-5));
                CHECK(hit->meshRegionRevision == world.regions().revision());
            }
        }
        // Every hex vertex is contained in its region closure, including undersides.
        for (const auto& chunk : world.residentChunks()) {
            REQUIRE(chunk.meshRegionRevision == world.regions().revision());
            if (chunk.key.policy != PolicyKind::Hex) continue;
            const auto origin = world.meshOrigin(chunk.key);
            for (const auto& v : world.mesh(chunk.key)->vertices) {
                CHECK(double(origin.x) + v.position.x >= -1e-5);
                CHECK(double(origin.x) + v.position.x <= 64 + 1e-5);
                CHECK(double(origin.z) + v.position.z >= -1e-5);
                CHECK(double(origin.z) + v.position.z <= 64 + 1e-5);
            }
        }
        const StreamingChunkKey key{PolicyKind::Hex, {-1, 0, 1}};
        const auto before = world.snapshot(key);
        REQUIRE(before.has_value());
        const auto& tile = std::get<HexTile>(before->base);
        const auto index = tile.index({0, 0});
        REQUIRE(index.has_value());
        REQUIRE(world.edit(key, *index, CellValue::hexValue({2, 17})));
        REQUIRE(settle(world, {}, 22));
        const auto original = *world.mesh(key);
        for (unsigned i = 0; i < 12; ++i) {
            world.regions().set({0, 0}, i % 2 ? PolicyKind::Height : PolicyKind::Hex);
            world.update({});
        }
        world.regions().erase({0, 0});
        REQUIRE(settle(world, {}, 19));
        CHECK_FALSE(world.chunk(key));
        world.regions().set({0, 0}, PolicyKind::Hex);
        REQUIRE(settle(world, {}, 22));
        CHECK(sameMesh(*world.mesh(key), original));
        const auto restored = world.snapshot(key);
        REQUIRE(restored.has_value());
        bool restoredEdit = false;
        for (const auto& delta : restored->deltas) {
            if (delta.index == *index && delta.value == CellValue::hexValue({2, 17})) restoredEdit = true;
        }
        CHECK(restoredEdit);
        WorldStreamer reference(config);
        reference.regions().set({-1, 0}, PolicyKind::Voxel);
        reference.regions().set({0, 0}, PolicyKind::Hex);
        REQUIRE(settle(reference, {}, 22));
        REQUIRE(reference.edit(key, *index, CellValue::hexValue({2, 17})));
        REQUIRE(settle(reference, {}, 22));
        for (const auto& chunk : world.residentChunks()) {
            REQUIRE(reference.mesh(chunk.key));
            CHECK(sameMesh(*world.mesh(chunk.key), *reference.mesh(chunk.key)));
            CHECK(chunk.meshRegionRevision == world.regions().revision());
        }
        CHECK(world.stats().failedCompletions == 0);
    }
}

TEST_CASE("M16: regional hex boundary profiles have no microscopic translated-chunk gaps") {
    auto config = testConfig(PolicyKind::Height);
    config.horizontalHalfExtentMeters = 96.f;
    config.chunkCapacityPerPolicy = 25;
    config.regionExtentMeters = 64;
    config.hexRadius = 8.f;
    config.regionalSurfaceOwnership = true;
    config.regionalHexCutWalls = true;
    WorldStreamer world(config);
    world.regions().set({-1, 0}, PolicyKind::Voxel);
    world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world, {}, 22));
    std::vector<TerrainBoundaryMesh> sources;
    for (const auto& chunk : world.residentChunks()) {
        if (chunk.key.policy == PolicyKind::Hex)
            sources.push_back({world.mesh(chunk.key), world.meshOrigin(chunk.key)});
    }
    for (const auto axis : {TerrainBoundaryAxis::X, TerrainBoundaryAxis::Z}) {
        const auto profile = compareTerrainBoundaryTops(sources, {}, {axis, 64, 0, 64});
        REQUIRE(profile.hasValue());
        for (const auto& span : profile.value()) {
            CAPTURE(static_cast<unsigned>(axis)); CAPTURE(span.begin); CAPTURE(span.end);
            CHECK(span.negativeCount == 1);
        }
    }
}

TEST_CASE("M16: boundary snapshots invalidate both policies and retain worker-owned geometry") {
    for (const unsigned workers : {1u, 4u}) {
        auto config = testConfig(PolicyKind::Height);
        config.workerCount = workers;
        config.horizontalHalfExtentMeters = 96.f;
        config.chunkCapacityPerPolicy = 25;
        config.regionExtentMeters = 64;
        config.hexRadius = 8;
        config.regionalSurfaceOwnership = true;
        WorldStreamer world(config);
        world.regions().set({0, 0}, PolicyKind::Hex);
        const StreamingBoundaryRequest request{{TerrainBoundaryAxis::X, 64, 0, 64}};
        const auto settleAll = [&] {
            for (unsigned i = 0; i < 64; ++i) {
                world.update({}); world.waitForIdle();
                if (world.residentCount() != 0 && !world.hasPendingWork()) return true;
            }
            return false;
        };
        REQUIRE(settleAll());
        auto captured = world.captureBoundary(request);
        REQUIRE(captured.hasValue());
        auto frozen = captured.value();
        REQUIRE(world.boundaryCurrent(frozen));
        REQUIRE_FALSE(frozen.sources(false).empty()); REQUIRE_FALSE(frozen.sources(true).empty());
        CHECK(frozen.regionRevision() == world.regions().revision());
        const auto original = frozen.profile();
        REQUIRE(original.hasValue());
        double covered = 0;
        for (const auto& span : original.value()) {
            REQUIRE(span.negative); REQUIRE(span.positive);
            covered += span.end - span.begin;
        }
        CHECK(covered == 64);
        REQUIRE(world.edit({PolicyKind::Height, {-2, 0, -2}}, heightSampleIndex(32, 32),
                           CellValue::heightValue({40, 0, {255, 0, 0, 0}})));
        CHECK(world.boundaryCurrent(frozen)); // A remote interior edit is not a dependency.
        REQUIRE(settleAll()); CHECK(world.boundaryCurrent(frozen));
        const StreamingChunkKey height{PolicyKind::Height, {1, 0, 0}};
        REQUIRE(world.edit(height, heightSampleIndex(0, 32), CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
        CHECK_FALSE(world.boundaryCurrent(frozen)); // Before update/publication.
        CHECK(world.captureBoundary(request).error() == StreamingBoundaryError::SourcesPending);
        using Profile = Result<containers::Array<TerrainBoundarySpan>, TerrainBoundaryError>;
        std::optional<Profile> workerResult;
        // Immutable snapshot copied into a worker while the main thread remeshes.
        std::thread worker([snapshot = frozen, &workerResult] { workerResult = snapshot.profile(); });
        const bool settled = settleAll();
        worker.join();
        REQUIRE(settled);
        REQUIRE(workerResult.has_value()); REQUIRE(workerResult->hasValue());
        REQUIRE(workerResult->value().size() == original.value().size());
        for (std::size_t i = 0; i < original.value().size(); ++i) {
            const auto& old = original.value()[i]; const auto& copy = workerResult->value()[i];
            CHECK(old.begin == copy.begin); CHECK(old.end == copy.end);
            REQUIRE(copy.negative); REQUIRE(copy.positive);
            CHECK(old.negative->beginHeight == copy.negative->beginHeight);
            CHECK(old.positive->endHeight == copy.positive->endHeight);
        }
        CHECK_FALSE(world.boundaryCurrent(frozen)); // Reject the stale worker result.
        captured = world.captureBoundary(request);
        REQUIRE(captured.hasValue()); REQUIRE(world.boundaryCurrent(captured.value()));
        const auto changed = captured.value().profile();
        REQUIRE(changed.hasValue());
        double maximum = -INFINITY;
        for (const auto& span : changed.value()) if (span.positive)
            maximum = std::max({maximum, span.positive->beginHeight, span.positive->endHeight});
        CHECK(maximum == 45);
        const auto hexKey = captured.value().sources(false)[0].key;
        const auto tile = std::get<HexTile>(world.snapshot(hexKey)->base);
        REQUIRE(world.edit(hexKey, *tile.index({0, 0}), CellValue::hexValue({2, 17})));
        CHECK_FALSE(world.boundaryCurrent(captured.value()));
        CHECK(world.captureBoundary(request).error() == StreamingBoundaryError::SourcesPending);
        REQUIRE(settleAll());
        captured = world.captureBoundary(request);
        REQUIRE(captured.hasValue());
        world.regions().set({10, 10}, PolicyKind::Voxel);
        CHECK_FALSE(world.boundaryCurrent(captured.value())); // Conservative global revision gate.
        REQUIRE(settleAll());
        captured = world.captureBoundary(request);
        REQUIRE(captured.hasValue());
        WorldStreamer other(config);
        other.regions().set({0, 0}, PolicyKind::Hex);
        CHECK_FALSE(other.boundaryCurrent(captured.value()));
        CHECK(world.stats().failedCompletions == 0);
    }
}

TEST_CASE("M16: boundary snapshot membership detects arrivals unloads and replacement handles") {
    auto config = testConfig(PolicyKind::Height);
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionExtentMeters = 64; config.hexRadius = 8; config.regionalSurfaceOwnership = true;
    StreamingBoundarySnapshot retained;
    const StreamingBoundaryRequest request{{TerrainBoundaryAxis::X, 64, 0, 64}};
    {
        WorldStreamer world(config);
        world.regions().set({0, 0}, PolicyKind::Hex);
        auto empty = world.captureBoundary(request);
        REQUIRE(empty.hasValue()); REQUIRE(world.boundaryCurrent(empty.value()));
        auto absent = empty.value().profile();
        REQUIRE(absent.hasValue()); REQUIRE(absent.value().size() == 1);
        CHECK(absent.value()[0].negativeCount == 0); CHECK(absent.value()[0].positiveCount == 0);
        const auto settleAt = [&](math::Vec3 camera) {
            for (unsigned i = 0; i < 64; ++i) {
                world.update(camera); world.waitForIdle();
                if (world.residentCount() && !world.hasPendingWork()) return true;
            }
            return false;
        };
        REQUIRE(settleAt({}));
        CHECK_FALSE(world.boundaryCurrent(empty.value()));
        auto ready = world.captureBoundary(request);
        REQUIRE(ready.hasValue()); retained = ready.value();
        REQUIRE(settleAt({512, 0, 512}));
        CHECK_FALSE(world.boundaryCurrent(retained));
        auto unloaded = world.captureBoundary(request);
        REQUIRE(unloaded.hasValue()); CHECK(world.boundaryCurrent(unloaded.value()));
        REQUIRE(settleAt({}));
        CHECK_FALSE(world.boundaryCurrent(retained)); // Same keys/geometry, new handles.
        CHECK_FALSE(world.boundaryCurrent(unloaded.value()));
        REQUIRE(world.captureBoundary(request).hasValue());
    }
    REQUIRE(retained.profile().hasValue()); // No live mesh pointers or owner lifetime dependency.
    const StreamingBoundarySnapshot invalid;
    CHECK_FALSE(invalid.profile()); CHECK(invalid.sources(false).empty()); CHECK_FALSE(invalid.regionRevision());
}

TEST_CASE("M16: boundary capture validates region ownership and explicit copy limits") {
    auto config = testConfig(PolicyKind::Height);
    const StreamingBoundaryRequest request{{TerrainBoundaryAxis::X, 64, 0, 64}};
    WorldStreamer legacy(config);
    CHECK(legacy.captureBoundary(request).error() == StreamingBoundaryError::Unsupported);
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionExtentMeters = 64; config.hexRadius = 8; config.regionalSurfaceOwnership = true;
    WorldStreamer world(config);
    CHECK(world.captureBoundary(request).error() == StreamingBoundaryError::InvalidBoundary);
    world.regions().set({0, 0}, PolicyKind::Hex);
    auto bad = request; bad.line.plane = 63.5;
    CHECK_FALSE(world.captureBoundary(bad));
    bad = request; bad.line.begin = bad.line.end;
    CHECK_FALSE(world.captureBoundary(bad));
    bad = request; bad.line.plane = INFINITY;
    CHECK_FALSE(world.captureBoundary(bad));
    bad = request; bad.positive = PolicyKind::Voxel;
    CHECK_FALSE(world.captureBoundary(bad));
    bad = request; bad.line.end = 128; // Next tile does not have the same policy pair.
    CHECK_FALSE(world.captureBoundary(bad));
    bad = request; bad.line.begin = -std::numeric_limits<double>::denorm_min();
    CHECK_FALSE(world.captureBoundary(bad)); // Tiny negative interval must inspect the negative tile.
    bad = request; bad.line.end = 64. * 4097;
    CHECK(world.captureBoundary(bad).error() == StreamingBoundaryError::LimitExceeded);
    bool settled = false;
    for (unsigned i = 0; i < 64 && !settled; ++i) {
        world.update({}); world.waitForIdle(); settled = world.residentCount() && !world.hasPendingWork();
    }
    REQUIRE(settled);
    CHECK(world.captureBoundary(request, {0, 64u * 1024u * 1024u}).error() == StreamingBoundaryError::LimitExceeded);
    CHECK(world.captureBoundary(request, {256, 1}).error() == StreamingBoundaryError::LimitExceeded);
    CHECK(world.captureBoundary(request).hasValue());
    const auto exact = world.captureBoundary(request);
    REQUIRE(exact.hasValue());
    std::size_t bytes = 0, count = 0;
    for (const bool positive : {false, true}) for (const auto& source : exact.value().sources(positive)) {
        const auto* mesh = world.mesh(source.key);
        bytes += mesh->vertices.size() * sizeof(TerrainVertex) + mesh->indices.size() * sizeof(std::uint32_t) +
                 mesh->surfaces.size() * sizeof(TerrainSurface);
        ++count;
    }
    REQUIRE(bytes > 0); REQUIRE(count > 0);
    CHECK(world.captureBoundary(request, {count, bytes}).hasValue());
    CHECK(world.captureBoundary(request, {count, bytes - 1}).error() == StreamingBoundaryError::LimitExceeded);
    CHECK(world.captureBoundary(request, {count - 1, bytes}).error() == StreamingBoundaryError::LimitExceeded);
    // Reversed policy sides on a signed boundary are supported.
    world.regions().set({-1, -1}, PolicyKind::Hex);
    settled = false;
    for (unsigned i = 0; i < 64 && !settled; ++i) {
        world.update({}); world.waitForIdle(); settled = world.residentCount() && !world.hasPendingWork();
    }
    REQUIRE(settled);
    CHECK(world.captureBoundary({{TerrainBoundaryAxis::Z, -64, -64, 0}, PolicyKind::Height, PolicyKind::Hex}).hasValue());
}

TEST_CASE("M16: regional cuts invalidate retained chunks and replay stored edits") {
    auto config = testConfig(PolicyKind::Height);
    config.regionExtentMeters = 16;
    config.regionalSurfaceOwnership = true;
    WorldStreamer world(config);
    const StreamingChunkKey height{PolicyKind::Height, {0, 0, 0}};
    REQUIRE(settle(world, {}, 1));
    REQUIRE(world.edit(height, heightSampleIndex(20, 20), CellValue::heightValue({60.f, 0, {255, 0, 0, 0}})));
    REQUIRE(settle(world, {}, 1));
    const auto before = world.chunk(height);
    REQUIRE(before.has_value());
    world.regions().set({1, 1}, PolicyKind::Voxel);
    world.update({});
    const auto dirty = world.chunk(height);
    REQUIRE(dirty.has_value());
    CHECK(dirty->handle == before->handle);
    CHECK(dirty->version > before->version);
    CHECK(dirty->meshRegionRevision == before->meshRegionRevision);
    REQUIRE(settle(world, {}, 2));
    auto hit = world.raycastPublishedTerrain({20, 100, 20}, {0, -1, 0}, 200, true);
    REQUIRE(hit.has_value());
    CHECK(hit->key.policy == PolicyKind::Voxel);
    world.regions().erase({1, 1});
    REQUIRE(settle(world, {}, 1));
    hit = world.raycastPublishedTerrain({20, 100, 20}, {0, -1, 0}, 200, true);
    REQUIRE(hit.has_value());
    CHECK(hit->key.policy == PolicyKind::Height);
    CHECK(hit->surface.position.y == 60.f);
    CHECK(world.chunk(height)->handle == before->handle);
    CHECK(hit->meshRegionRevision == world.regions().revision());
    config.regionExtentMeters = 1;
    CHECK_THROWS_AS((WorldStreamer{config}), std::invalid_argument);
    config = testConfig(PolicyKind::Hex);
    config.regionalHexCutWalls = true;
    CHECK_THROWS_AS((WorldStreamer{config}), std::invalid_argument);
}

TEST_CASE("M16: regional voxel cuts replay hidden edits with signed ownership and stale publication") {
    for (const int sign : {-1, 1}) {
        auto config = testConfig(PolicyKind::Height);
        config.workerCount = sign < 0 ? 1 : 4;
        config.regionExtentMeters = 16;
        config.regionalSurfaceOwnership = true;
        config.regionalVoxelCutFaces = true;
        WorldStreamer world(config);
        const int cx = sign < 0 ? -1 : 0;
        const RegionCoord region{cx * 2, 0};
        const StreamingChunkKey key{PolicyKind::Voxel, {cx, 0, 0}};
        const math::Vec3 camera{float(cx * 32 + 8), 0, 8};
        world.regions().set(region, PolicyKind::Voxel);
        REQUIRE(settle(world, camera, 2));
        for (unsigned x = 12; x < 20; ++x) {
            REQUIRE(world.edit(key, voxelIndex(x, 8, 8), CellValue::voxelValue(2)));
        }
        REQUIRE(settle(world, camera, 2));
        const auto info = world.chunk(key);
        REQUIRE(info.has_value());
        const auto origin = world.meshOrigin(key);
        auto hit = raycastTerrainMesh(*world.mesh(key), origin + math::Vec3{18, 8.5f, 8.5f}, {-1, 0, 0}, 4, origin, true);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == 2);
        CHECK(hit->normal == math::Vec3{1, 0, 0});
        // This stored edit is currently outside the owned half chunk.
        REQUIRE(world.edit(key, voxelIndex(18, 25, 8), CellValue::voxelValue(2)));
        REQUIRE(settle(world, camera, 2));
        CHECK_FALSE(raycastTerrainMesh(*world.mesh(key), origin + math::Vec3{18.5f, 30, 8.5f}, {0, -1, 0}, 6, origin));
        world.regions().set({region.x + 1, 0}, PolicyKind::Voxel);
        world.update(camera);
        REQUIRE(settle(world, camera, 2));
        CHECK(world.chunk(key)->handle == info->handle);
        CHECK(world.chunk(key)->meshRegionRevision == world.regions().revision());
        const auto published = world.raycastPublishedTerrain(origin + math::Vec3{18.5f, 30, 8.5f}, {0, -1, 0}, 6, true);
        REQUIRE(published.has_value());
        CHECK(published->key == key);
        CHECK(published->surface.position.y == 26);
        const auto retained = world.snapshot(key);
        REQUIRE(retained.has_value());
        WorldStreamer reference(config);
        reference.regions().set(region, PolicyKind::Voxel);
        reference.regions().set({region.x + 1, 0}, PolicyKind::Voxel);
        REQUIRE(settle(reference, camera, 2));
        for (const auto& delta : retained->deltas) REQUIRE(reference.edit(key, delta.index, delta.value));
        REQUIRE(settle(reference, camera, 2));
        const auto a = toResourceMesh(*world.mesh(key)), b = toResourceMesh(*reference.mesh(key));
        REQUIRE(a.streams.size() == b.streams.size());
        REQUIRE(a.streams[0].data.size() == b.streams[0].data.size());
        CHECK(std::equal(a.streams[0].data.begin(), a.streams[0].data.end(), b.streams[0].data.begin()));
        REQUIRE(a.indices.size() == b.indices.size());
        CHECK(std::equal(a.indices.begin(), a.indices.end(), b.indices.begin()));
        CHECK(world.stats().failedCompletions == 0);
    }
    auto invalid = testConfig(PolicyKind::Voxel);
    invalid.regionalVoxelCutFaces = true;
    CHECK_THROWS_AS((WorldStreamer{invalid}), std::invalid_argument);
}

TEST_CASE("M16: meter footprints match exhaustive center admission and nearest capacity") {
    for (const auto policy : {PolicyKind::Voxel, PolicyKind::Height, PolicyKind::Hex}) {
        for (const auto layout : {HexLayout::Tiled, HexLayout::LegacySpaced}) {
            if (policy != PolicyKind::Hex && layout != HexLayout::Tiled) continue;
            for (const float scale : {0.5f, 3.f}) {
                CAPTURE(policy);
                CAPTURE(layout);
                CAPTURE(scale);
                auto config = testConfig(policy, std::numeric_limits<std::uint32_t>::max());
                config.horizontalHalfExtentMeters = 48.f;
                config.chunkCapacityPerPolicy = 12;
                config.voxelVerticalRadius = 1;
                config.hexRadius = scale;
                config.policyParams.hex.layout = layout;
                WorldStreamer streamer(config);
                // Independent forward formula and generous fixed enumeration,
                // not the planner's inverse transform or camera-to-chunk helper.
                const auto position = [&](ChunkCoord coord) {
                    if (policy != PolicyKind::Hex) {
                        const long double extent = policy == PolicyKind::Voxel ? 32.L : 64.L;
                        return std::array<long double, 3>{(coord.x + 0.5L) * extent,
                            policy == PolicyKind::Voxel ? (coord.y + 0.5L) * extent : 0.L,
                            (coord.z + 0.5L) * extent};
                    }
                    const auto q = layout == HexLayout::Tiled ? 5 * coord.x + 2 * coord.z : 5 * coord.x;
                    const auto r = layout == HexLayout::Tiled ? -2 * coord.x + 3 * coord.z : 5 * coord.z;
                    return std::array<long double, 3>{(2.L * q + r) * 0.8660254037844386468L * scale,
                        0.L, r * 1.5L * scale};
                };
                for (const math::Vec3 camera : {math::Vec3{}, math::Vec3{0.5f, 0.f, 0.5f},
                        math::Vec3{-35.25f, 0.f, 19.5f}, math::Vec3{45.5f, 33.5f, -20.5f}}) {
                    std::vector<std::pair<std::uint64_t, ChunkCoord>> expected;
                    for (int x = -64; x <= 64; ++x) {
                        for (int z = -64; z <= 64; ++z) {
                            for (int y = -1; y <= 2; ++y) {
                                if (policy != PolicyKind::Voxel && y != 0) continue;
                                if (policy == PolicyKind::Voxel && std::abs(y - static_cast<int>(std::floor(camera.y / 32.f))) > 1) continue;
                                const ChunkCoord coord{x, y, z};
                                const auto p = position(coord);
                                if (std::abs(p[0] - camera.x) > 48.L || std::abs(p[2] - camera.z) > 48.L) continue;
                                const auto dx = p[0] - camera.x, dy = p[1] - camera.y, dz = p[2] - camera.z;
                                expected.push_back({static_cast<std::uint64_t>(dx * dx + dy * dy + dz * dz), coord});
                            }
                        }
                    }
                    std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
                        return std::tie(a.first, a.second.x, a.second.y, a.second.z) <
                               std::tie(b.first, b.second.x, b.second.y, b.second.z);
                    });
                    if (expected.size() > config.chunkCapacityPerPolicy) expected.resize(config.chunkCapacityPerPolicy);
                    REQUIRE(settle(streamer, camera, expected.size()));
                    const auto actual = streamer.residentChunks();
                    REQUIRE(actual.size() == expected.size());
                    for (std::size_t i = 0; i < actual.size(); ++i) {
                        CHECK(actual[i].key.policy == policy);
                        CHECK(actual[i].key.coord == expected[i].second);
                        CHECK(actual[i].distanceSquared == expected[i].first);
                    }
                    CHECK(streamer.stats().failedCompletions == 0);
                }
            }
        }
    }
}

TEST_CASE("M16: meter footprints refresh inside a chunk and handle closed edges and invalid input") {
    auto config = testConfig(PolicyKind::Height);
    config.horizontalHalfExtentMeters = 32.f;
    WorldStreamer streamer(config);
    REQUIRE(settle(streamer, {}, 4)); // Centers at both -32 and +32 are included.
    REQUIRE(settle(streamer, {0.25f, 0.f, 0.25f}, 1));
    CHECK(streamer.chunk({PolicyKind::Height, {0, 0, 0}}).has_value());
    REQUIRE(settle(streamer, {-0.25f, 0.f, -0.25f}, 1));
    CHECK(streamer.chunk({PolicyKind::Height, {-1, 0, -1}}).has_value());
    REQUIRE(settle(streamer, {}, 4));
    REQUIRE(settle(streamer, {std::numeric_limits<float>::infinity(), 0.f, 0.f}, 0));
    REQUIRE(settle(streamer, {}, 4));
    config.horizontalHalfExtentMeters = 0.f;
    WorldStreamer point(config);
    REQUIRE(settle(point, {}, 0)); // A center selection is not a mesh-intersection query.
    REQUIRE(settle(point, {32.f, 0.f, 32.f}, 1));
    REQUIRE(settle(point, {-32.f, 0.f, -32.f}, 1));
    REQUIRE(settle(point, {std::numeric_limits<float>::max(), 0.f, 0.f}, 0));
    REQUIRE(settle(point, {32.f, 0.f, 32.f}, 1));
    for (const float invalid : {-1.f, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        config.horizontalHalfExtentMeters = invalid;
        CHECK_THROWS_AS(WorldStreamer{config}, std::invalid_argument);
    }
    config.horizontalHalfExtentMeters = std::numeric_limits<float>::max();
    config.chunkCapacityPerPolicy = 1;
    config.workerCount = 1;
    WorldStreamer enormous(config);
    CHECK(enormous.update({}).planningPending);
    CHECK(enormous.update({0.5f, 0.f, 0.f}).planningPending);
    // Superseded and current integer-clamped enumerations must cancel at shutdown.
}

TEST_CASE("M16: mixed meter footprint fills the previously excluded voxel half region") {
    auto config = testConfig(PolicyKind::Height, 1);
    config.regionExtentMeters = 64;
    config.hexRadius = 8.f;
    config.horizontalHalfExtentMeters = 96.f;
    WorldStreamer streamer(config);
    streamer.regions().set({-1, 0}, PolicyKind::Voxel);
    streamer.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(streamer, {}, 20));
    unsigned counts[3]{};
    for (const auto& chunk : streamer.residentChunks()) ++counts[static_cast<unsigned>(chunk.key.policy)];
    CHECK(counts[0] == 4);
    CHECK(counts[1] == 14);
    CHECK(counts[2] == 2);
    CHECK(streamer.chunk({PolicyKind::Voxel, {-2, 0, 0}}).has_value());
    CHECK(streamer.chunk({PolicyKind::Voxel, {-2, 0, 1}}).has_value());
    streamer.regions().erase({-1, 0});
    streamer.regions().erase({0, 0});
    REQUIRE(settle(streamer, {}, 16));
    for (const auto& chunk : streamer.residentChunks()) CHECK(chunk.key.policy == PolicyKind::Height);
}

TEST_CASE("M16: tiled hex ownership covers the lattice exactly once with stable local indices") {
    for (const std::uint32_t radius : {0u, 1u, 2u, 3u, 16u}) {
        std::map<std::pair<std::int32_t, std::int32_t>, unsigned> coverage;
        HexTile shape;
        shape.radius = radius;
        shape.width = radius * 2 + 1;
        for (std::int32_t z = -4; z <= 4; ++z) {
            for (std::int32_t x = -4; x <= 4; ++x) {
                const ChunkCoord chunk{x, 0, z};
                const auto origin = hexChunkOrigin(chunk, radius, HexLayout::Tiled);
                REQUIRE(origin.has_value());
                for (const auto local : hexSpiral({}, radius)) {
                    const AxialCoord global{origin->q + local.q, origin->r + local.r};
                    CHECK(++coverage[{global.q, global.r}] == 1);
                    CHECK(hexChunkAt(global, radius, HexLayout::Tiled) == chunk);
                    REQUIRE(shape.index(local).has_value());
                }
                for (const float scale : {0.5f, 1.f, 3.f}) {
                    const math::Vec3 position{
                        (static_cast<float>(origin->q) * 1.7320508075688772f +
                         static_cast<float>(origin->r) * 0.8660254037844386f) * scale,
                        20.f, static_cast<float>(origin->r) * 1.5f * scale};
                    const auto cell = hexCellAt(position, scale);
                    REQUIRE(cell.has_value());
                    CHECK(*cell == *origin);
                }
            }
        }
        for (std::int32_t r = -3; r <= 3; ++r) {
            for (std::int32_t q = -3; q <= 3; ++q) CHECK(coverage[{q, r}] == 1);
        }
    }
    for (const float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()}) CHECK_FALSE(hexCellAt({}, invalid));
    CHECK_FALSE(hexCellAt({std::numeric_limits<float>::max(), 0.f, 0.f}, 1.f));
    CHECK_FALSE(hexChunkOrigin({std::numeric_limits<std::int32_t>::max(), 0, 1}, 2, HexLayout::Tiled));
    CHECK_FALSE(hexChunkOrigin({}, 2049, HexLayout::Tiled));
    CHECK_FALSE(hexChunkAt({}, 2, static_cast<HexLayout>(255)));
    for (const AxialCoord local : {AxialCoord{2048, 0}, AxialCoord{0, -2048}, AxialCoord{-2048, 2048}}) {
        CHECK(hexChunkAt(local, 2048, HexLayout::Tiled) == ChunkCoord{});
    }
}

TEST_CASE("M16: tiled hex saves reject legacy layouts and mismatched generation recipes") {
    PolicyParams params;
    params.hex.radius = 2;
    params.hex.layout = HexLayout::Tiled;
    ChunkStore source(1);
    const auto handle = source.create({2, 0, -1}, 123, PolicyKind::Hex, params);
    REQUIRE(source.queue(handle)); REQUIRE(source.generate(handle));
    REQUIRE(source.edit(handle, 12, CellValue::hexValue({200, 42})));
    memory::MemoryWriter writer;
    REQUIRE(source.saveDeltas(handle, writer));
    const auto saved = writer.bytes();
    memory::MemoryReader reader(saved);
    memory::ChunkView frame;
    REQUIRE(memory::checkStreamPreamble(reader)); REQUIRE(memory::readChunkView(reader, frame));
    CHECK(frame.version == 2);
    for (unsigned mismatch = 0; mismatch < 6; ++mismatch) {
        auto targetParams = params;
        if (mismatch == 1) targetParams.hex.layout = HexLayout::LegacySpaced;
        if (mismatch == 2) targetParams.hex.radius = 3;
        if (mismatch == 3) targetParams.hex.heightAmplitude = 4.f;
        if (mismatch == 4) targetParams.hex.frequency = 0.5f;
        ChunkStore target(1);
        const auto targetHandle = target.create({2, 0, -1}, mismatch == 5 ? 456 : 123, PolicyKind::Hex, targetParams);
        REQUIRE(target.queue(targetHandle)); REQUIRE(target.generate(targetHandle));
        const auto version = target.version(targetHandle);
        CHECK(target.loadDeltas(targetHandle, saved) == (mismatch == 0));
        if (mismatch == 0) CHECK(target.cell(targetHandle, 12)->hex == HexCell{200, 42});
        else CHECK(target.version(targetHandle) == version);
        if (mismatch == 1) {
            REQUIRE(target.edit(targetHandle, 12, CellValue::hexValue({201, 17})));
            REQUIRE(target.saveDeltas(targetHandle, writer));
            CHECK_FALSE(source.loadDeltas(handle, writer.bytes()));
        }
    }
    for (std::size_t size = 1; size < saved.size(); ++size) {
        auto truncated = saved;
        truncated.resize(size);
        const auto version = source.version(handle);
        CHECK_FALSE(source.loadDeltas(handle, truncated));
        CHECK(source.version(handle) == version);
    }
}

TEST_CASE("M16: hex planning follows rendered centers at signed coordinates and non-unit scales") {
    for (const float scale : {0.5f, 1.f, 3.f}) {
        auto config = testConfig(PolicyKind::Hex, 1);
        config.chunkCapacityPerPolicy = 1;
        config.hexRadius = scale;
        config.regionExtentMeters = 1;
        WorldStreamer streamer(config);
        for (const ChunkCoord coord : {ChunkCoord{7, 0, -3}, ChunkCoord{-8, 0, 4}}) {
            const StreamingChunkKey key{PolicyKind::Hex, coord};
            const auto origin = streamer.meshOrigin(key);
            const math::Vec3 camera{origin.x, 20.f, origin.z};
            REQUIRE(settle(streamer, camera, 1));
            REQUIRE(streamer.chunk(key).has_value());
            CHECK(streamer.chunk(key)->distanceSquared == 400);
            const RegionCoord region{static_cast<std::int32_t>(std::floor(origin.x)),
                                      static_cast<std::int32_t>(std::floor(origin.z))};
            streamer.regions().set(region, PolicyKind::Height);
            // With radius one another hex replaces the region-excluded nearest chunk.
            REQUIRE(settle(streamer, camera, 1));
            CHECK_FALSE(streamer.chunk(key));
            streamer.regions().erase(region);
            REQUIRE(settle(streamer, camera, 1));
            CHECK(streamer.chunk(key).has_value());
        }
    }
}

TEST_CASE("M16: tiled hex boundary edits dirty the actual owning neighbors") {
    PolicyParams params;
    params.hex.radius = 2;
    params.hex.layout = HexLayout::Tiled;
    ChunkStore store(7);
    const auto center = store.create({}, 123, PolicyKind::Hex, params);
    REQUIRE(store.queue(center)); REQUIRE(store.generate(center));
    std::vector<std::pair<ChunkCoord, ChunkHandle>> neighbors;
    for (const auto direction : kHexDirections) {
        const ChunkCoord coord{direction.q, 0, direction.r};
        const auto handle = store.create(coord, 123, PolicyKind::Hex, params);
        REQUIRE(store.queue(handle)); REQUIRE(store.generate(handle));
        neighbors.emplace_back(coord, handle);
    }
    const auto tile = generateHexTile(123, {}, params.hex);
    for (const auto local : hexSpiral({}, 2)) {
        std::map<std::pair<std::int32_t, std::int32_t>, bool> touched;
        for (const auto direction : kHexDirections) {
            const auto owner = hexChunkAt({local.q + direction.q, local.r + direction.r}, 2, HexLayout::Tiled);
            REQUIRE(owner.has_value());
            if (*owner != ChunkCoord{}) touched[{owner->x, owner->z}] = true;
        }
        std::vector<std::uint64_t> versions;
        for (const auto& [coord, handle] : neighbors) { (void)coord; versions.push_back(store.version(handle)); }
        REQUIRE(store.edit(center, *tile.index(local), CellValue::hexValue({200, 0})));
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            const auto& [coord, handle] = neighbors[i];
            CHECK(store.version(handle) == versions[i] + (touched.contains({coord.x, coord.z}) ? 1u : 0u));
        }
    }
}

TEST_CASE("M16: tiled hex neighbors share heights and suppress only internal walls") {
    auto config = testConfig(PolicyKind::Hex, 1);
    config.hexRadius = 2.f;
    WorldStreamer streamer(config);
    const auto anchor = streamer.meshOrigin({PolicyKind::Hex, {-3, 0, 2}});
    REQUIRE(settle(streamer, anchor, 9));
    std::map<std::pair<std::int32_t, std::int32_t>, bool> present;
    std::map<std::pair<std::int64_t, std::int64_t>, float> heights;
    for (const auto& chunk : streamer.residentChunks()) {
        const auto origin = hexChunkOrigin(chunk.key.coord, 2, HexLayout::Tiled);
        REQUIRE(origin.has_value());
        const auto tile = generateHexTile(config.worldSeed, chunk.key.coord, config.policyParams.hex);
        for (const auto local : hexSpiral({}, 2)) {
            const AxialCoord global{origin->q + local.q, origin->r + local.r};
            CHECK(present.emplace(std::pair{global.q, global.r}, true).second);
            for (unsigned corner = 0; corner < 6; ++corner) {
                const auto lattice = hexCorner(global, corner);
                const auto height = tile.corner(local, corner);
                REQUIRE(height.has_value());
                const auto [found, inserted] = heights.emplace(std::pair{lattice.x2, lattice.y2}, *height);
                if (!inserted) CHECK(found->second == *height);
            }
        }
    }
    for (const auto& chunk : streamer.residentChunks()) {
        const auto origin = hexChunkOrigin(chunk.key.coord, 2, HexLayout::Tiled);
        std::size_t exposed = 0;
        for (const auto local : hexSpiral({}, 2)) {
            for (const auto direction : kHexDirections) {
                if (!present.contains({origin->q + local.q + direction.q,
                                       origin->r + local.r + direction.r})) ++exposed;
            }
        }
        const auto* mesh = streamer.mesh(chunk.key);
        REQUIRE(mesh != nullptr);
        CHECK(mesh->indices.size() == 19 * 36 + exposed * 6);
    }
}

TEST_CASE("M16: voxel border fetch is relative to each signed chunk on every axis") {
    for (const ChunkCoord center : {ChunkCoord{0, 0, 0}, ChunkCoord{10, 2, -10}, ChunkCoord{-10, -2, 10}}) {
        auto config = testConfig(PolicyKind::Voxel, 1);
        config.voxelVerticalRadius = 1;
        config.policyParams.voxel.baseHeight = static_cast<float>(center.y * 32 + 16);
        config.policyParams.voxel.heightAmplitude = 0.f;
        WorldStreamer streamer(config);
        const math::Vec3 anchor{static_cast<float>(center.x * 32),
                                static_cast<float>(center.y * 32),
                                static_cast<float>(center.z * 32)};
        REQUIRE(settle(streamer, anchor, 27));
        std::vector<std::pair<ChunkCoord, VoxelCells>> cells;
        for (const auto& chunk : streamer.residentChunks()) {
            cells.emplace_back(chunk.key.coord, generateVoxelCells(config.worldSeed, chunk.key.coord,
                                                                   config.policyParams.voxel));
        }
        for (const auto& [coord, base] : cells) {
            VoxelMeshingOptions options;
            // Independent oracle: locate global cell positions within resident
            // bounding boxes, rather than repeating the streamer's quotient lookup.
            options.borderFetch = [&](std::int32_t x, std::int32_t y, std::int32_t z) -> std::uint16_t {
                for (const auto& [neighbor, values] : cells) {
                    const auto nx = x + (coord.x - neighbor.x) * 32;
                    const auto ny = y + (coord.y - neighbor.y) * 32;
                    const auto nz = z + (coord.z - neighbor.z) * 32;
                    if (nx >= 0 && nx < 32 && ny >= 0 && ny < 32 && nz >= 0 && nz < 32) {
                        return values.values[voxelIndex(static_cast<std::uint32_t>(nx),
                            static_cast<std::uint32_t>(ny), static_cast<std::uint32_t>(nz))];
                    }
                }
                return 0;
            };
            const auto expected = meshVoxelChunk(base, streamer.blocks(), options);
            REQUIRE(expected.hasValue());
            const auto* actual = streamer.mesh({PolicyKind::Voxel, coord});
            REQUIRE(actual != nullptr);
            CHECK(meshDigest(*actual) == meshDigest(expected.value()));
            REQUIRE(actual->vertices.size() == expected.value().vertices.size());
            for (std::size_t i = 0; i < actual->vertices.size(); ++i) {
                CHECK(actual->vertices[i].color == expected.value().vertices[i].color);
            }
        }
    }
}

TEST_CASE("M16: priority jobs run nearest stage first and shut down cleanly") {
    jobs::JobSystem jobs(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::vector<std::uint32_t> order;
    const std::thread::id callingThread = std::this_thread::get_id();
    std::thread::id workerThread;

    REQUIRE(jobs.submit({0, 0}, [&] {
        std::unique_lock lock(mutex);
        workerThread = std::this_thread::get_id();
        started = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
        order.push_back(0);
    }));
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return started; });
    }
    REQUIRE(jobs.submit({100, 0}, [&] { order.push_back(4); }));
    REQUIRE(jobs.submit({4, 1}, [&] { order.push_back(2); }));
    REQUIRE(jobs.submit({4, 0}, [&] { order.push_back(1); }));
    REQUIRE(jobs.submit({25, 0}, [&] { order.push_back(3); }));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    jobs.waitIdle();
    REQUIRE(order.size() == 5);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
    CHECK(order[2] == 2);
    CHECK(order[3] == 3);
    CHECK(order[4] == 4);
    CHECK(workerThread != callingThread);
    REQUIRE(jobs.submit({0, 0}, [] { throw std::runtime_error("isolated job failure"); }));
    jobs.waitIdle();
    CHECK(jobs.failedCount() == 1);
    jobs.shutdown();
    CHECK_FALSE(jobs.submit({}, [] {}));
}

TEST_CASE("M16: generation has a versioned layout and main-thread publication gate") {
    ChunkStore store(1);
    const ChunkHandle original = store.create({}, 77, PolicyKind::Voxel);
    REQUIRE(store.queue(original));
    const auto request = store.beginGeneration(original);
    REQUIRE(request.has_value());
    CHECK(store.state(original) == ChunkState::LayoutDone);
    CHECK_FALSE(store.publishGenerated(original, *request, ChunkBase{}));
    CHECK(store.state(original) == ChunkState::LayoutDone);
    auto generated = generateChunkBase(*request);
    REQUIRE(generated.has_value());
    REQUIRE(store.destroy(original));

    const ChunkHandle replacement = store.create({}, 77, PolicyKind::Voxel);
    REQUIRE(store.queue(replacement));
    CHECK_FALSE(store.publishGenerated(replacement, *request, std::move(*generated)));
    REQUIRE(store.generate(replacement));
    const std::uint64_t beforeRegenerate = store.version(replacement);
    REQUIRE(store.regenerate(replacement));
    CHECK(store.version(replacement) == beforeRegenerate + 1u);
}

TEST_CASE("M16: voxel palettes use deterministic four and eight bit storage") {
    VoxelCells cells;
    cells.values.resize(kVoxelCellCount);
    for (std::uint32_t index = 0; index < kVoxelCellCount; ++index) {
        cells.values[index] = static_cast<std::uint16_t>(index % 16u);
    }
    auto fourBit = packVoxelCells(cells);
    REQUIRE(fourBit.has_value());
    CHECK(fourBit->palette.size() == 16);
    CHECK(fourBit->bitsPerIndex == 4);
    CHECK(packedVoxelStorageBytes(*fourBit) == 16u * 1024u + 32u);
    const auto fourBitRoundTrip = unpackVoxelCells(*fourBit);
    REQUIRE(fourBitRoundTrip.has_value());
    CHECK(*fourBitRoundTrip == cells);

    for (std::uint32_t index = 0; index < kVoxelCellCount; ++index) {
        cells.values[index] = static_cast<std::uint16_t>(index % 17u);
    }
    auto eightBit = packVoxelCells(cells);
    REQUIRE(eightBit.has_value());
    CHECK(eightBit->palette.size() == 17);
    CHECK(eightBit->bitsPerIndex == 8);
    CHECK(packedVoxelStorageBytes(*eightBit) == 32u * 1024u + 34u);
    const auto eightBitRoundTrip = unpackVoxelCells(*eightBit);
    REQUIRE(eightBitRoundTrip.has_value());
    CHECK(*eightBitRoundTrip == cells);

    for (std::uint32_t index = 0; index < 257; ++index) {
        cells.values[index] = static_cast<std::uint16_t>(index);
    }
    CHECK_FALSE(packVoxelCells(cells).has_value());

    ChunkStore store(1);
    const ChunkHandle handle = store.create({}, 77, PolicyKind::Voxel);
    REQUIRE(store.queue(handle));
    REQUIRE(store.generate(handle));
    const auto snapshot = store.snapshot(handle);
    REQUIRE(snapshot.has_value());
    const PackedVoxelCells* resident = std::get_if<PackedVoxelCells>(&snapshot->base);
    REQUIRE(resident != nullptr);
    CHECK(resident->bitsPerIndex <= 4);
    CHECK(packedVoxelStorageBytes(*resident) < kVoxelCellCount * sizeof(std::uint16_t));
}

TEST_CASE("M16: neighbor snapshot queries are bounded independently of residency") {
    for (const auto policy : {PolicyKind::Height, PolicyKind::Voxel, PolicyKind::Hex}) {
        INFO("policy=" << static_cast<unsigned>(policy));
        WorldStreamer streamer(testConfig(policy, 2));
        REQUIRE(settle(streamer, {}, 25));
        const auto stats = streamer.stats();
        REQUIRE(stats.submittedMeshJobs > 0);
        const std::uint64_t queriesPerJob = policy == PolicyKind::Voxel ? 27 : 9;
        CHECK(stats.neighborQueries == stats.submittedMeshJobs * queriesPerJob);
        CHECK(stats.failedCompletions == 0);
    }
}

TEST_CASE("M16: stage profiling is opt-in and fits within total elapsed time") {
    for (const bool enabled : {false, true}) {
        auto config = testConfig(PolicyKind::Height);
        config.profileTimings = enabled;
        WorldStreamer streamer(config);
        for (unsigned frame = 0; frame < 8; ++frame) {
            const auto stats = streamer.update({});
            const auto& t = stats.timings;
            const auto total = t.planning + t.residency + t.unload + t.load + t.publication + t.mesh;
            if (enabled) {
                CHECK(total >= std::chrono::nanoseconds::zero());
                CHECK(total < stats.elapsed + std::chrono::microseconds(1));
            } else {
                CHECK(total == std::chrono::nanoseconds::zero());
            }
            streamer.waitForIdle();
        }
        CHECK(settle(streamer, {}, 1));
    }
}

TEST_CASE("M16: camera rings honor frame caps and unload obsolete residency") {
    WorldStreamingConfig config = testConfig(PolicyKind::Height, 1);
    WorldStreamingConfig stoppedConfig = config;
    stoppedConfig.frameBudget = std::chrono::microseconds(0);
    WorldStreamer stopped(stoppedConfig);
    const auto closed = stopped.update({0.0f, 0.0f, 0.0f});
    CHECK(closed.loaded == 0);
    CHECK(closed.budgetClosed);
    CHECK_FALSE(closed.budgetDeferred);
    CHECK(stopped.residentCount() == 0);

    config.maxLoadsPerFrame = 2;
    config.maxUnloadsPerFrame = 2;
    WorldStreamer streamer(config);

    const StreamingFrameStats first = streamer.update({0.0f, 0.0f, 0.0f});
    CHECK(first.loaded == 2);
    CHECK(first.loaded <= config.maxLoadsPerFrame);
    REQUIRE(streamer.residentCount() == 2);
    CHECK(streamer.residentChunks()[0].distanceSquared == 2048);
    CHECK(streamer.residentChunks()[1].distanceSquared == 2048);
    for (const StreamingChunkInfo& chunk : streamer.residentChunks()) {
        CHECK(chunk.state == ChunkState::LayoutDone);
        CHECK(chunk.generationInFlight);
    }
    REQUIRE(settle(streamer, {0.0f, 0.0f, 0.0f}, 9));
    REQUIRE(streamer.chunk({PolicyKind::Height, {-1, 0, 0}}).has_value());

    const StreamingFrameStats moved = streamer.update({192.0f, 0.0f, 0.0f});
    CHECK(moved.unloaded == 2);
    CHECK(moved.unloaded <= config.maxUnloadsPerFrame);
    REQUIRE(settle(streamer, {192.0f, 0.0f, 0.0f}, 9));
    CHECK_FALSE(streamer.chunk({PolicyKind::Height, {-1, 0, 0}}).has_value());
    CHECK(streamer.chunk({PolicyKind::Height, {3, 0, 0}}).has_value());
}

TEST_CASE("M16: deterministic admission uses work-unit caps instead of the wall clock") {
    auto config = testConfig(PolicyKind::Height, 1);
    config.frameBudget = std::chrono::microseconds(1);
    config.maxLoadsPerFrame = 4;
    config.deterministicAdmission = true;
    WorldStreamer streamer(config);
    const auto frame = streamer.update({});
    CHECK_FALSE(frame.budgetClosed);
    CHECK_FALSE(frame.budgetDeferred);
    CHECK(frame.loaded == 4);
    CHECK(streamer.residentCount() == 4);
    WorldStreamer again(config);
    const auto second = again.update({});
    CHECK(second.loaded == frame.loaded);
    CHECK(second.budgetDeferred == frame.budgetDeferred);
    CHECK_FALSE(frame.budgetOverrun);
    std::vector<ChunkCoord> firstKeys, secondKeys;
    for (const auto& chunk : streamer.residentChunks()) firstKeys.push_back(chunk.key.coord);
    for (const auto& chunk : again.residentChunks()) secondKeys.push_back(chunk.key.coord);
    const auto byCoord = [](ChunkCoord a, ChunkCoord b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.z != b.z) return a.z < b.z;
        return a.y < b.y;
    };
    std::sort(firstKeys.begin(), firstKeys.end(), byCoord);
    std::sort(secondKeys.begin(), secondKeys.end(), byCoord);
    CHECK(firstKeys == secondKeys);
}

TEST_CASE("M16: cooperative budget deferral is visible and does not preempt admitted work") {
    auto config = testConfig(PolicyKind::Height, 2);
    config.frameBudget = std::chrono::microseconds(1);
    config.maxLoadsPerFrame = 64;
    config.maxUnloadsPerFrame = 64;
    config.maxPublicationsPerFrame = 64;
    config.maxMeshJobsPerFrame = 64;
    WorldStreamer streamer(config);
    const auto frame = streamer.update({});
    CHECK_FALSE(frame.budgetClosed);
    CHECK_FALSE(frame.planningPending);
    if (frame.loaded > 0 && frame.loaded < 25 && frame.loaded < config.maxLoadsPerFrame) {
        CHECK(frame.budgetDeferred);
    }
    CHECK(frame.loaded >= 1);
    CHECK(streamer.residentCount() == frame.loaded);
    CHECK_FALSE(frame.budgetOverrun);
}

struct HardClock {
    int samples = 0;
    int earlySamples = 0;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
};

auto sampleHardClock(void* clock) -> std::chrono::steady_clock::time_point {
    auto* hard = static_cast<HardClock*>(clock);
    const int sample = hard->samples++;
    if (sample < hard->earlySamples) return hard->origin;
    return hard->origin + std::chrono::hours(1);
}

TEST_CASE("M16: hard deadline stops the next load and keeps the admitted chunk") {
    auto config = testConfig(PolicyKind::Height, 1);
    config.frameBudget = std::chrono::microseconds(1);
    config.maxLoadsPerFrame = 64;
    WorldStreamer streamer(config);
    HardClock clock;
    clock.earlySamples = 2;
    StreamingDeadline deadline;
    deadline.hard = true;
    deadline.now = sampleHardClock;
    deadline.clock = &clock;
    const auto frame = streamer.update({}, deadline);
    CHECK_FALSE(frame.budgetClosed);
    CHECK_FALSE(frame.planningPending);
    CHECK(frame.loaded == 1);
    CHECK(frame.budgetOverrun);
    CHECK(frame.budgetDeferred);
    CHECK(streamer.residentCount() == frame.loaded);

    HardClock expired;
    expired.earlySamples = 1;
    deadline.clock = &expired;
    const auto stopped = streamer.update({}, deadline);
    CHECK(stopped.loaded == 0);
    CHECK(stopped.unloaded == 0);
    CHECK(stopped.budgetDeferred);
    CHECK_FALSE(stopped.budgetOverrun);
    CHECK(streamer.residentCount() == 1);
}

TEST_CASE("M16: deterministic admission ignores a hard deadline") {
    auto config = testConfig(PolicyKind::Height, 1);
    config.frameBudget = std::chrono::microseconds(1);
    config.maxLoadsPerFrame = 4;
    config.deterministicAdmission = true;
    WorldStreamer streamer(config);
    HardClock clock;
    clock.earlySamples = 0;
    StreamingDeadline deadline;
    deadline.hard = true;
    deadline.now = sampleHardClock;
    deadline.clock = &clock;
    const auto frame = streamer.update({}, deadline);
    CHECK(frame.loaded == 4);
    CHECK_FALSE(frame.budgetOverrun);
    CHECK_FALSE(frame.budgetDeferred);
    CHECK(streamer.residentCount() == 4);
    CHECK(clock.samples == 0);
}

struct RegionClock {
    int samples = 0;
    int earlySamples = 0;
    std::chrono::steady_clock::time_point early = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point late = early + std::chrono::hours(1);
};

auto sampleRegionClock(void* clock) -> std::chrono::steady_clock::time_point {
    auto* region = static_cast<RegionClock*>(clock);
    const int sample = region->samples++;
    if (sample < region->earlySamples) return region->early;
    return region->late;
}

TEST_CASE("M16: a shared region edit keeps copied entries when the deadline passes") {
    constexpr int tiles = 32;
    RegionMap map(64, PolicyKind::Height);
    for (int i = 0; i < tiles; ++i) map.set({i, 0}, PolicyKind::Hex);
    const auto revision = map.revision();
    RegionMap snapshot = map;

    RegionClock clock;
    clock.earlySamples = 2;
    RegionBudget budget;
    budget.deadline = clock.early + std::chrono::microseconds(1);
    budget.now = sampleRegionClock;
    budget.clock = &clock;
    const auto partial = map.setBudgeted({100, 0}, PolicyKind::Voxel, budget);
    CHECK_FALSE(partial.applied);
    CHECK(partial.deferred);
    CHECK(partial.overrun);
    CHECK(partial.retained == 1);
    CHECK(map.revision() == revision);
    CHECK(map.policyAt(0, 0) == PolicyKind::Hex);
    CHECK(map.policyAt(100 * 64, 0) == PolicyKind::Height);
    CHECK(snapshot.policyAt(0, 0) == PolicyKind::Hex);
    CHECK(snapshot.policyAt(100 * 64, 0) == PolicyKind::Height);

    RegionClock resume;
    resume.earlySamples = 1000;
    budget.deadline = resume.early + std::chrono::hours(1);
    budget.clock = &resume;
    const auto done = map.setBudgeted({100, 0}, PolicyKind::Voxel, budget);
    CHECK(done.applied);
    CHECK_FALSE(done.deferred);
    CHECK_FALSE(done.overrun);
    CHECK(done.retained == static_cast<std::size_t>(tiles));
    CHECK(map.policyAt(100 * 64, 0) == PolicyKind::Voxel);
    CHECK(map.policyAt(0, 0) == PolicyKind::Hex);
    CHECK(map.policyAt((tiles - 1) * 64, 0) == PolicyKind::Hex);
    CHECK(snapshot.policyAt(0, 0) == PolicyKind::Hex);
    CHECK(snapshot.policyAt(100 * 64, 0) == PolicyKind::Height);
    const bool revisionMoved = map.revision() != revision;
    CHECK(revisionMoved);

    RegionMap pending(64, PolicyKind::Height);
    for (int i = 0; i < tiles; ++i) pending.set({i, 0}, PolicyKind::Hex);
    RegionMap pendingSnapshot = pending;
    RegionClock slicedClock;
    slicedClock.earlySamples = 2;
    budget.deadline = slicedClock.early + std::chrono::microseconds(1);
    budget.clock = &slicedClock;
    const auto sliced = pending.setBudgeted({100, 0}, PolicyKind::Voxel, budget);
    CHECK(sliced.retained == 1);
    CHECK(sliced.overrun);
    pending.set({7, 0}, PolicyKind::Voxel);
    RegionClock finish;
    finish.earlySamples = 1000;
    budget.deadline = finish.early + std::chrono::hours(1);
    budget.clock = &finish;
    const auto afterEager = pending.setBudgeted({100, 0}, PolicyKind::Voxel, budget);
    CHECK(afterEager.applied);
    CHECK(pending.policyAt(7 * 64, 0) == PolicyKind::Voxel);
    CHECK(pending.policyAt(100 * 64, 0) == PolicyKind::Voxel);
    CHECK(pending.policyAt(0, 0) == PolicyKind::Hex);
    CHECK(pendingSnapshot.policyAt(7 * 64, 0) == PolicyKind::Hex);
    CHECK(pendingSnapshot.policyAt(100 * 64, 0) == PolicyKind::Height);

    RegionMap held(64, PolicyKind::Height);
    for (int i = 0; i < tiles; ++i) held.set({i, 0}, PolicyKind::Hex);
    RegionMap hold = held;
    const auto heldRevision = held.revision();
    RegionClock expired;
    expired.earlySamples = 0;
    budget.deadline = expired.early;
    budget.clock = &expired;
    const auto refused = held.setBudgeted({3, 0}, PolicyKind::Voxel, budget);
    CHECK_FALSE(refused.applied);
    CHECK(refused.deferred);
    CHECK_FALSE(refused.overrun);
    CHECK(refused.retained == 0);
    CHECK(held.revision() == heldRevision);
    CHECK(held.policyAt(3 * 64, 0) == PolicyKind::Hex);
    CHECK(hold.policyAt(3 * 64, 0) == PolicyKind::Hex);
}

TEST_CASE("M16: large rings plan off-thread and make progress under a tiny budget") {
    auto config = testConfig(PolicyKind::Height, 128);
    config.workerCount = 1;
    config.chunkCapacityPerPolicy = 4;
    config.frameBudget = std::chrono::microseconds(1);
    config.maxLoadsPerFrame = 1;
    config.maxUnloadsPerFrame = 1;
    config.maxPublicationsPerFrame = 1;
    config.maxMeshJobsPerFrame = 1;
    WorldStreamer streamer(config);
    // Waiting for workers, not a wall-clock assertion, makes this reproducible
    // on slow/sanitized runners while exercising the same starvation path.
    REQUIRE(settle(streamer, {}, 4));
    CHECK(streamer.stats().publishedMeshes >= 4);
    for (const auto& chunk : streamer.residentChunks()) {
        CHECK(chunk.key.policy == PolicyKind::Height);
        CHECK(chunk.distanceSquared == 2048);
    }
    const math::Vec3 moved{192.f, 0.f, 0.f};
    REQUIRE(settle(streamer, moved, 4));
    CHECK_FALSE(streamer.chunk({PolicyKind::Height, {-1, 0, 0}}).has_value());
    CHECK(streamer.chunk({PolicyKind::Height, {3, 0, 0}}).has_value());

    // A saved mutable RegionMap reference must invalidate cached plans too.
    auto& regions = streamer.regions();
    regions = RegionMap(512, PolicyKind::Hex);
    streamer.update(moved);
    streamer.waitForIdle();
    REQUIRE(settle(streamer, moved, 4));
    for (const auto& chunk : streamer.residentChunks()) CHECK(chunk.key.policy == PolicyKind::Hex);
}

TEST_CASE("M16: obsolete large plans cancel and shutdown does not enumerate the full ring") {
    auto config = testConfig(PolicyKind::Height, std::numeric_limits<std::uint32_t>::max());
    config.chunkCapacityPerPolicy = 1;
    config.workerCount = 1;
    WorldStreamer streamer(config);
    CHECK(streamer.update({}).planningPending);
    auto& regions = streamer.regions();
    regions.set({0, 0}, PolicyKind::Voxel);
    CHECK(streamer.update({64.f, 0.f, 0.f}).planningPending);
    // Destructor must cancel both obsolete and current work before joining.
}

TEST_CASE("M16: region mutations invalidate residency without moving the camera") {
    auto config = testConfig(PolicyKind::Height);
    WorldStreamer streamer(config);
    auto& regions = streamer.regions();
    REQUIRE(settle(streamer, {}, 1));
    regions.set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(streamer, {}, 1));
    CHECK(streamer.chunk({PolicyKind::Hex, {0, 0, 0}}).has_value());
    REQUIRE(regions.erase({0, 0}));
    REQUIRE(settle(streamer, {}, 1));
    CHECK(streamer.chunk({PolicyKind::Height, {0, 0, 0}}).has_value());
}

TEST_CASE("M16: unloaded worker results are rejected as stale") {
    WorldStreamingConfig config = testConfig(PolicyKind::Voxel);
    config.workerCount = 1;
    config.maxLoadsPerFrame = 1;
    config.maxUnloadsPerFrame = 1;
    WorldStreamer streamer(config);

    REQUIRE(streamer.update({1.0f, 31.0f, 1.0f}).loaded == 1);
    REQUIRE(streamer.update({65.0f, 31.0f, 1.0f}).unloaded == 1);
    streamer.waitForIdle();
    for (std::uint32_t frame = 0; frame < 8; ++frame) {
        streamer.update({65.0f, 31.0f, 1.0f});
        streamer.waitForIdle();
    }
    CHECK_FALSE(streamer.chunk({PolicyKind::Voxel, {0, 0, 0}}).has_value());
    CHECK(streamer.chunk({PolicyKind::Voxel, {2, 0, 0}}).has_value());
    CHECK(streamer.stats().staleCompletions >= 1);
}

TEST_CASE("M16: obsolete mesh failures do not suppress a newer retry") {
    WorldStreamingConfig config = testConfig(PolicyKind::Voxel);
    config.workerCount = 1;
    WorldStreamer streamer(config);
    const math::Vec3 camera{1.0f, 31.0f, 1.0f};
    REQUIRE(settle(streamer, camera, 1));
    const StreamingChunkKey key{PolicyKind::Voxel, {0, 0, 0}};
    REQUIRE(streamer.edit(key, voxelIndex(1, 31, 1), CellValue::voxelValue(500)));
    streamer.update(camera);
    REQUIRE(streamer.edit(key, voxelIndex(1, 31, 1), CellValue::voxelValue(2)));
    streamer.waitForIdle();
    streamer.update(camera);
    streamer.waitForIdle();
    REQUIRE(settle(streamer, camera, 1));
    CHECK(streamer.stats().failedCompletions == 0);
    CHECK(streamer.stats().staleCompletions >= 1);
    const auto resident = streamer.chunk(key);
    REQUIRE(resident.has_value());
    CHECK(resident->state == ChunkState::Meshed);
}

TEST_CASE("M16: height edge edits survive unload and seed an absent neighbor") {
    WorldStreamer streamer(testConfig(PolicyKind::Height));
    REQUIRE(settle(streamer, {1.0f, 0.0f, 1.0f}, 1));
    HeightSample edit;
    edit.height = 91.25f;
    edit.biomeId = 7;
    edit.materialWeights = {{1, 2, 3, 249}};
    REQUIRE(streamer.edit({PolicyKind::Height, {0, 0, 0}},
                          heightSampleIndex(kHeightQuads, 10),
                          CellValue::heightValue(edit)));

    REQUIRE(settle(streamer, {65.0f, 0.0f, 10.0f}, 1));
    CHECK_FALSE(streamer.chunk({PolicyKind::Height, {0, 0, 0}}).has_value());
    const auto neighbor = streamer.snapshot({PolicyKind::Height, {1, 0, 0}});
    REQUIRE(neighbor.has_value());
    bool mirrored = false;
    for (const ChunkDelta& delta : neighbor->deltas) {
        if (delta.index == heightSampleIndex(0, 10)) {
            mirrored = delta.value.height == edit;
        }
    }
    CHECK(mirrored);
    const auto sampled = streamer.sampleHeightTerrain(64.0f, 10.0f);
    REQUIRE(sampled.has_value());
    CHECK(sampled->height == edit.height);
    CHECK(sampled->biomeId == edit.biomeId);
}

TEST_CASE("M16: resident collision hooks cross voxel chunks and sample height overlays") {
    WorldStreamer voxels(testConfig(PolicyKind::Voxel, 1));
    const math::Vec3 camera{31.0f, 31.0f, 2.0f};
    REQUIRE(settle(voxels, camera, 9));
    REQUIRE(voxels.edit({PolicyKind::Voxel, {0, 0, 0}}, voxelIndex(30, 31, 2),
                        CellValue::voxelValue(0)));
    REQUIRE(voxels.edit({PolicyKind::Voxel, {0, 0, 0}}, voxelIndex(31, 31, 2),
                        CellValue::voxelValue(0)));
    REQUIRE(voxels.edit({PolicyKind::Voxel, {1, 0, 0}}, voxelIndex(0, 31, 2),
                        CellValue::voxelValue(0)));
    REQUIRE(voxels.edit({PolicyKind::Voxel, {1, 0, 0}}, voxelIndex(1, 31, 2),
                        CellValue::voxelValue(0)));
    REQUIRE(voxels.edit({PolicyKind::Voxel, {1, 0, 0}}, voxelIndex(2, 31, 2),
                        CellValue::voxelValue(2)));
    const auto hit = voxels.raycastVoxelTerrain(
        {29.5f, 31.5f, 2.5f}, {1.0f, 0.0f, 0.0f}, 8.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->x == 34);
    CHECK(hit->t == doctest::Approx(4.5f));

    WorldStreamer heights(testConfig(PolicyKind::Height));
    REQUIRE(settle(heights, {1.0f, 0.0f, 1.0f}, 1));
    HeightSample flat;
    flat.height = 42.5f;
    flat.biomeId = 3;
    flat.materialWeights = {{0, 0, 255, 0}};
    for (std::uint32_t z = 10; z <= 11; ++z) {
        for (std::uint32_t x = 10; x <= 11; ++x) {
            REQUIRE(heights.edit({PolicyKind::Height, {0, 0, 0}},
                                 heightSampleIndex(x, z), CellValue::heightValue(flat)));
        }
    }
    const auto sampled = heights.sampleHeightTerrain(10.5f, 10.5f);
    REQUIRE(sampled.has_value());
    CHECK(sampled->height == flat.height);
    CHECK(sampled->materialWeights == flat.materialWeights);
}

TEST_CASE("M16: regional policies and asynchronous meshes converge deterministically") {
    WorldStreamingConfig config = testConfig(PolicyKind::Height);
    config.regionExtentMeters = 64;
    WorldStreamer first(config);
    WorldStreamer second(config);
    first.regions().set({0, 0}, PolicyKind::Hex);
    second.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(first, {1.0f, 0.0f, 1.0f}, 1));
    REQUIRE(settle(second, {1.0f, 0.0f, 1.0f}, 1));

    const auto firstChunks = first.residentChunks();
    const auto secondChunks = second.residentChunks();
    REQUIRE(firstChunks.size() == 1);
    REQUIRE(secondChunks.size() == 1);
    CHECK(firstChunks[0].key == secondChunks[0].key);
    CHECK(firstChunks[0].key.policy == PolicyKind::Hex);
    const MeshData* firstMesh = first.mesh(firstChunks[0].key);
    const MeshData* secondMesh = second.mesh(secondChunks[0].key);
    REQUIRE(firstMesh != nullptr);
    REQUIRE(secondMesh != nullptr);
    CHECK(meshDigest(*firstMesh) == meshDigest(*secondMesh));
    CHECK(first.stats().publishedCells == 1);
    CHECK(first.stats().publishedMeshes == 1);
}
