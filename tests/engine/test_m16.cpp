#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "core/jobs/job_system.h"
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

TEST_CASE("M16: camera rings honor frame caps and unload obsolete residency") {
    WorldStreamingConfig config = testConfig(PolicyKind::Height, 1);
    WorldStreamingConfig stoppedConfig = config;
    stoppedConfig.frameBudget = std::chrono::microseconds(0);
    WorldStreamer stopped(stoppedConfig);
    CHECK(stopped.update({0.0f, 0.0f, 0.0f}).loaded == 0);
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
