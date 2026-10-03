#include <doctest/doctest.h>

#include <fstream>
#include <algorithm>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "backends/null/null_device.h"
#include "demos/streaming_demo_state.h"
#include "demos/streaming_demo_io.h"

namespace {
using namespace f4;

auto settle(world::WorldStreamer& world, math::Vec3 camera, std::size_t expected) -> bool {
    for (unsigned frame = 0; frame < 128; ++frame) {
        world.update(camera);
        world.waitForIdle();
        if (world.residentCount() == expected && !world.hasPendingWork()) return true;
    }
    return false;
}

struct IoFixture {
    std::filesystem::path root = std::filesystem::current_path() /
        ("streaming-io-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    IoFixture() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot create I/O test fixture");
    }
    ~IoFixture() {
        // Only these task-created fixture files/directories are removed.
        std::error_code ignored;
        for (const char* name : {"world.f4sd", "world.f4sd.tmp", "invalid", "oversized",
                                 "blocked/keep", "blocked", "blocked.tmp"}) {
            std::filesystem::remove(root / name, ignored);
        }
        std::filesystem::remove(root, ignored);
    }
};

auto finishIo(demo::StreamingDemoIo& io) -> std::optional<demo::StreamingIoResult> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = io.poll()) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}
} // namespace

TEST_CASE("Streaming surface views share accepted mesh provenance with queries") {
    auto config = demo::streamingDemoConfig();
    config.horizontalRadius = 0;
    world::WorldStreamer streamer(config);
    REQUIRE(settle(streamer, {}, 1));
    const auto info = streamer.residentChunks()[0];
    const auto* mesh = streamer.mesh(info.key);
    REQUIRE(mesh);
    std::vector<world::StreamingSurfaceSource> sources{{info.key, info.handle, info.meshVersion,
        info.meshRegionRevision, streamer.meshOrigin(info.key), streamer.identityToken(),
        std::make_shared<const world::MeshData>(*mesh)}};
    auto built = world::StreamingSurfaceView::build(sources);
    REQUIRE(built.hasValue());
    auto view = std::move(built).value();
    CHECK(view.current(streamer));
    REQUIRE(streamer.publishSurfaceView(view));
    REQUIRE(streamer.acceptedSurfaceView());
    const auto accepted = view.raycast({32, 100, 32}, {0, -1, 0}, 200, true);
    const auto published = streamer.raycastPublishedTerrain({32, 100, 32}, {0, -1, 0}, 200, true);
    const auto acceptedByWorld = streamer.raycastAcceptedTerrain({32, 100, 32}, {0, -1, 0}, 200, true);
    REQUIRE(accepted); REQUIRE(published);
    REQUIRE(acceptedByWorld);
    CHECK(accepted->source.key == published->key);
    CHECK(accepted->source.surface.distance == published->surface.distance);
    CHECK(acceptedByWorld->source.surface.distance == accepted->source.surface.distance);

    auto duplicate = sources;
    duplicate.push_back(sources.front());
    CHECK(world::StreamingSurfaceView::build(duplicate).error() ==
        world::StreamingSurfaceViewError::DuplicateSource);
    REQUIRE(streamer.edit(info.key, world::heightSampleIndex(0, 0),
        world::CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
    CHECK_FALSE(view.current(streamer));
    CHECK_FALSE(streamer.publishSurfaceView(view));
    CHECK_FALSE(streamer.acceptedSurfaceView()->current(streamer));
    CHECK(streamer.raycastAcceptedTerrain({32, 100, 32}, {0, -1, 0}, 200, true).has_value());
    streamer.clearSurfaceView();
    CHECK_FALSE(streamer.acceptedSurfaceView());
}

TEST_CASE("Streaming demo: mixed policy mutations preserve edited meshes against a fresh reference") {
    using namespace world;
    for (const unsigned radius : {1u, 5u}) {
        for (const auto [workers, meters] : {std::pair{1u, false}, std::pair{4u, false},
                                             std::pair{1u, true}, std::pair{4u, true}}) {
            CAPTURE(radius);
            CAPTURE(workers);
            CAPTURE(meters);
            auto config = demo::streamingMixedDemoConfig();
            config.horizontalRadius = radius; // Radius five uses off-thread planning.
            if (meters) config.horizontalHalfExtentMeters = radius == 1 ? 96.f : 160.f;
            config.workerCount = workers;
            config.maxLoadsPerFrame = 2;
            config.maxUnloadsPerFrame = 2;
            config.maxPublicationsPerFrame = 2;
            config.maxMeshJobsPerFrame = 2;
            WorldStreamer reference(config), live(config);
            demo::setStreamingMixedRegions(reference.regions());
            demo::setStreamingMixedRegions(live.regions());
            const auto drain = [](WorldStreamer& world) {
                for (unsigned frame = 0; frame < 256; ++frame) {
                    world.update({});
                    world.waitForIdle();
                    if (!world.hasPendingWork()) return true;
                }
                return false;
            };
            REQUIRE(drain(reference));
            REQUIRE(drain(live));
            const StreamingChunkKey keys[] = {{PolicyKind::Height, {0, 0, -1}},
                {PolicyKind::Voxel, {-1, 0, 0}}, {PolicyKind::Hex, {0, 0, 0}}};
            const ChunkDelta edits[] = {{heightSampleIndex(32, 32), CellValue::heightValue({38.f, 2, {0, 0, 255, 0}})},
                {voxelIndex(16, 31, 16), CellValue::voxelValue(1)},
                {12, CellValue::hexValue({3, 0})}};
            for (unsigned i = 0; i < 3; ++i) {
                REQUIRE(reference.edit(keys[i], edits[i].index, edits[i].value));
                REQUIRE(live.edit(keys[i], edits[i].index, edits[i].value));
            }
            REQUIRE(drain(reference));
            REQUIRE(drain(live));
            const auto expected = reference.residentChunks();
            if (radius == 1) {
                REQUIRE(expected.size() == (meters ? 14u : 11u));
                unsigned counts[3]{};
                for (const auto& chunk : expected) ++counts[static_cast<unsigned>(chunk.key.policy)];
                CHECK(counts[0] == (meters ? 4u : 2u));
                CHECK(counts[1] == (meters ? 8u : 7u));
                CHECK(counts[2] == 2);
            }
            gfx::NullDevice device;
            const auto baseline = device.stats();
            {
                demo::StreamingDemoView view(device);
                REQUIRE(view.init());
                REQUIRE(view.sync(live, 25));
                REQUIRE(view.caughtUp(live));
                // Guarantee real eviction before the burst, even if worker timing
                // causes every intermediate plan in the burst to be superseded.
                REQUIRE(settle(live, {512.f, 0.f, 0.f}, 8));
                for (const auto& key : keys) CHECK_FALSE(live.chunk(key).has_value());
                REQUIRE(view.sync(live, 25));
                REQUIRE(view.caughtUp(live));
                unsigned pendingPlans = 0;
                for (unsigned step = 0; step < 90; ++step) {
                    demo::setStreamingMixedRegions(live.regions(), step);
                    const auto frame = live.update({step % 2 == 0 ? 0.f : 512.f, 0.f, 0.f});
                    pendingPlans += frame.planningPending ? 1u : 0u;
                    CHECK(frame.loaded <= 2);
                    CHECK(frame.unloaded <= 2);
                    CHECK(live.residentCount() <= 24);
                    REQUIRE(view.sync(live, 2));
                    // Deliberately no waitForIdle/settle between revisions.
                }
                if (radius == 5) CHECK(pendingPlans > 0);
                demo::setStreamingMixedRegions(live.regions());
                REQUIRE(drain(live));
                REQUIRE(view.sync(live, 25));
                CHECK(view.caughtUp(live));
                REQUIRE(live.residentCount() == expected.size());
                for (const auto& chunk : expected) {
                    CAPTURE(chunk.key.policy);
                    CAPTURE(chunk.key.coord.x);
                    CAPTURE(chunk.key.coord.z);
                    REQUIRE(live.chunk(chunk.key).has_value());
                    const auto* actualMesh = live.mesh(chunk.key);
                    REQUIRE(actualMesh != nullptr);
                    const auto actual = toResourceMesh(*actualMesh);
                    const auto wanted = toResourceMesh(*reference.mesh(chunk.key));
                    REQUIRE(actual.streams.size() == wanted.streams.size());
                    CHECK(std::equal(actual.indices.begin(), actual.indices.end(), wanted.indices.begin(), wanted.indices.end()));
                    for (std::size_t stream = 0; stream < actual.streams.size(); ++stream) {
                        const auto& a = actual.streams[stream].data;
                        const auto& b = wanted.streams[stream].data;
                        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
                    }
                }
                for (unsigned i = 0; i < 3; ++i) {
                    const auto snapshot = live.snapshot(keys[i]);
                    REQUIRE(snapshot.has_value());
                    CHECK(std::find(snapshot->deltas.begin(), snapshot->deltas.end(), edits[i]) != snapshot->deltas.end());
                }
                CHECK(live.stats().failedCompletions == 0);
            }
            CHECK(device.stats().bufferCount == baseline.bufferCount);
            CHECK(device.stats().bufferBytes == baseline.bufferBytes);
            CHECK(device.stats().textureCount == baseline.textureCount);
            CHECK(device.stats().pipelineCount == baseline.pipelineCount);
        }
    }
}

TEST_CASE("Streaming demo: asynchronous saves snapshot edits and serialize replacement/load/retirement") {
    IoFixture files;
    const auto path = files.root / "world.f4sd";
    demo::StreamingDemoIo io;
    CHECK_FALSE(io.busy());
    CHECK_FALSE(io.poll().has_value());
    auto patch = demo::streamingDemoPatch();
    const auto expected = patch;
    REQUIRE(io.save(path, patch));
    patch.clear(); // Worker must own a copy, not a reference to live edits.
    CHECK(io.busy());
    CHECK_FALSE(io.save(path, {}));
    CHECK_FALSE(io.load(path));
    auto oldWorld = std::make_unique<world::WorldStreamer>(demo::streamingDemoConfig());
    const auto* identity = oldWorld.get();
    CHECK_FALSE(io.retire(oldWorld));
    CHECK(oldWorld.get() == identity);
    auto saved = finishIo(io);
    REQUIRE(saved.has_value());
    CHECK(saved->kind == demo::StreamingIoKind::Save);
    CHECK_FALSE(static_cast<bool>(saved->error));
    CHECK_FALSE(io.busy());
    const auto disk = demo::readStreamingDemoSave(path);
    REQUIRE(disk.size() == expected.size());
    for (std::size_t i = 0; i < disk.size(); ++i) CHECK(disk[i] == expected[i]);
    CHECK_FALSE(std::filesystem::exists(files.root / "world.f4sd.tmp"));

    REQUIRE(io.save(path, {}));
    auto replaced = finishIo(io);
    REQUIRE(replaced.has_value());
    CHECK_FALSE(static_cast<bool>(replaced->error));
    CHECK(demo::readStreamingDemoSave(path).isEmpty());
    REQUIRE(io.load(path));
    auto loaded = finishIo(io);
    REQUIRE(loaded.has_value());
    CHECK(loaded->kind == demo::StreamingIoKind::Load);
    REQUIRE_FALSE(static_cast<bool>(loaded->error));
    REQUIRE(loaded->world != nullptr);
    CHECK(loaded->edits.isEmpty());
    REQUIRE(settle(*loaded->world, {}, 25)); // Ownership transferred to this thread.
    oldWorld->update({}); // Retirement must safely drain in-flight CPU jobs.
    REQUIRE(io.retire(oldWorld));
    CHECK(oldWorld == nullptr);
    auto retired = finishIo(io);
    REQUIRE(retired.has_value());
    CHECK(retired->kind == demo::StreamingIoKind::Retire);
    CHECK_FALSE(static_cast<bool>(retired->error));
    io.shutdown();
    CHECK_FALSE(io.load(path));
    CHECK_FALSE(io.save(path, {}));
}

TEST_CASE("Streaming demo: asynchronous I/O failures preserve saved data and allow retry") {
    IoFixture files;
    const auto path = files.root / "world.f4sd";
    demo::StreamingDemoIo io;
    REQUIRE(io.save(path, {}));
    auto initial = finishIo(io);
    REQUIRE(initial.has_value());
    REQUIRE_FALSE(static_cast<bool>(initial->error));
    auto invalid = demo::streamingDemoPatch();
    invalid.push({world::kHeightSampleCount, world::CellValue::voxelValue(1)});
    REQUIRE(io.save(path, invalid));
    auto failed = finishIo(io);
    REQUIRE(failed.has_value());
    CHECK(static_cast<bool>(failed->error));
    CHECK(demo::readStreamingDemoSave(path).isEmpty());
    CHECK_FALSE(std::filesystem::exists(files.root / "world.f4sd.tmp"));

    REQUIRE(io.save(files.root / "missing-parent" / "world.f4sd", {}));
    auto openFailed = finishIo(io);
    REQUIRE(openFailed.has_value());
    CHECK(static_cast<bool>(openFailed->error));
    REQUIRE(std::filesystem::create_directory(files.root / "blocked"));
    { std::ofstream sentinel(files.root / "blocked" / "keep"); sentinel << "keep"; }
    REQUIRE(io.save(files.root / "blocked", {}));
    auto renameFailed = finishIo(io);
    REQUIRE(renameFailed.has_value());
    CHECK(static_cast<bool>(renameFailed->error));
    CHECK(std::filesystem::exists(files.root / "blocked" / "keep"));
    CHECK_FALSE(std::filesystem::exists(files.root / "blocked.tmp"));

    { std::ofstream corrupt(files.root / "invalid", std::ios::binary); corrupt << "invalid"; }
    {
        std::ofstream oversized(files.root / "oversized", std::ios::binary);
        oversized.seekp(128 * 1024); oversized.put('x');
    }
    for (const char* name : {"missing", "invalid", "oversized"}) {
        REQUIRE(io.load(files.root / name));
        auto rejected = finishIo(io);
        REQUIRE(rejected.has_value());
        CHECK(static_cast<bool>(rejected->error));
        CHECK(rejected->world == nullptr);
    }
    REQUIRE(io.load(path));
    auto recovered = finishIo(io);
    REQUIRE(recovered.has_value());
    CHECK_FALSE(static_cast<bool>(recovered->error));
    CHECK(recovered->world != nullptr);
}

TEST_CASE("Streaming demo: shutdown drains accepted saves without dropping completion errors") {
    IoFixture files;
    const auto path = files.root / "world.f4sd";
    {
        demo::StreamingDemoIo io;
        REQUIRE(io.save(path, {}));
    }
    CHECK(demo::readStreamingDemoSave(path).isEmpty());
    demo::StreamingDemoIo io;
    REQUIRE(io.load(files.root / "missing"));
    io.shutdown();
    auto result = io.poll();
    REQUIRE(result.has_value());
    CHECK(static_cast<bool>(result->error));
    CHECK_FALSE(io.busy());
    CHECK_FALSE(io.poll().has_value());
}

TEST_CASE("Streaming demo: save codec rejects invalid identity and malformed data") {
    const auto patch = demo::streamingDemoPatch();
    const auto bytes = demo::encodeStreamingDemoSave(patch);
    REQUIRE(bytes.has_value());
    const auto decoded = demo::decodeStreamingDemoSave(*bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == patch.size());
    for (std::size_t i = 0; i < patch.size(); ++i) CHECK((*decoded)[i] == patch[i]);
    for (std::size_t size = 0; size < bytes->size(); ++size) {
        auto truncated = *bytes;
        truncated.resize(size);
        CHECK_FALSE(demo::decodeStreamingDemoSave(truncated).has_value());
    }
    auto wrongSeed = *bytes;
    wrongSeed[16] ^= std::byte{1};
    CHECK_FALSE(demo::decodeStreamingDemoSave(wrongSeed).has_value());
    auto wrongVersion = *bytes;
    wrongVersion[8] ^= std::byte{1};
    CHECK_FALSE(demo::decodeStreamingDemoSave(wrongVersion).has_value());
    auto trailing = *bytes;
    trailing.push(std::byte{0});
    CHECK_FALSE(demo::decodeStreamingDemoSave(trailing).has_value());
    auto invalid = patch;
    invalid[0].index = world::kHeightSampleCount;
    CHECK_FALSE(demo::encodeStreamingDemoSave(invalid).has_value());
    const auto empty = demo::encodeStreamingDemoSave({});
    REQUIRE(empty.has_value());
    const auto emptyDecoded = demo::decodeStreamingDemoSave(*empty);
    REQUIRE(emptyDecoded.has_value());
    CHECK(emptyDecoded->isEmpty());
}

TEST_CASE("Streaming demo: height skirt masks retain only requested boundary geometry") {
    const auto tile = world::generateHeightTile(demo::kStreamingDemoSeed, {}, {});
    for (unsigned mask = 0; mask < 16; ++mask) {
        world::HeightMeshingOptions options;
        options.skirtEdges = static_cast<std::uint8_t>(mask);
        const auto mesh = world::meshHeightChunk(tile, {}, options);
        REQUIRE(mesh.hasValue());
        unsigned edges = 0;
        for (unsigned bit = 0; bit < 4; ++bit) edges += (mask >> bit) & 1u;
        CHECK(mesh.value().indices.size() == world::kHeightQuads * world::kHeightQuads * 6u +
              edges * world::kHeightQuads * 6u);
        CHECK(mesh.value().vertices.size() == world::kHeightSampleCount +
              edges * world::kHeightSamples * 2u);
    }
    world::HeightMeshingOptions invalid;
    invalid.skirtEdges = 16;
    CHECK_FALSE(world::meshHeightChunk(tile, {}, invalid).hasValue());
}

TEST_CASE("Streaming demo: neighbor unload restores the exposed height skirt") {
    auto config = demo::streamingDemoConfig();
    config.horizontalRadius = 1;
    world::WorldStreamer streamer(config);
    REQUIRE(settle(streamer, {}, 9));
    const auto topIndices = world::kHeightQuads * world::kHeightQuads * 6u;
    REQUIRE(streamer.mesh(demo::kEditedChunk) != nullptr);
    CHECK(streamer.mesh(demo::kEditedChunk)->indices.size() == topIndices);
    REQUIRE(settle(streamer, {64.f, 0.f, 0.f}, 9));
    REQUIRE(streamer.mesh(demo::kEditedChunk) != nullptr);
    CHECK(streamer.mesh(demo::kEditedChunk)->indices.size() == topIndices + world::kHeightQuads * 6u);
    REQUIRE(settle(streamer, {}, 9));
    CHECK(streamer.mesh(demo::kEditedChunk)->indices.size() == topIndices);
}

TEST_CASE("Streaming demo: invalid overlays do not partially edit the live world") {
    auto config = demo::streamingDemoConfig();
    config.horizontalRadius = 0;
    world::WorldStreamer streamer(config);
    REQUIRE(settle(streamer, {}, 1));
    const auto before = streamer.sampleHeightTerrain(32.f, 32.f);
    REQUIRE(before.has_value());
    auto edits = demo::streamingDemoPatch();
    edits.push({world::kHeightSampleCount, world::CellValue::voxelValue(1)});
    CHECK_FALSE(demo::applyStreamingDemoEdits(streamer, edits));
    CHECK(streamer.sampleHeightTerrain(32.f, 32.f) == before);
    REQUIRE(demo::applyStreamingDemoEdits(streamer, demo::streamingDemoPatch()));
    CHECK(streamer.sampleHeightTerrain(32.f, 32.f)->height == 38.f);
}

TEST_CASE("Streaming demo: upload caps, dirty-version retention, and GPU retirement") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        auto config = demo::streamingDemoConfig();
        config.horizontalRadius = 1;
        world::WorldStreamer streamer(config);
        demo::StreamingDemoView view(device);
        REQUIRE(view.init());
        const auto preparedPipelines = device.stats().pipelineCount;
        CHECK(preparedPipelines == baseline.pipelineCount + 3); // Fixed policy groups, all prepared before streaming.
        REQUIRE(settle(streamer, {}, 9));
        REQUIRE(view.sync(streamer, 0));
        CHECK(view.residentCount() == 0);
        REQUIRE(view.sync(streamer, 2));
        CHECK(view.residentCount() == 2);
        CHECK(view.uploadStats().uploaded == 2);
        CHECK(view.uploadStats().bytes > 0);
        REQUIRE(view.sync(streamer, 25));
        CHECK(view.caughtUp(streamer));
        CHECK(view.acceptedSurfaceCurrent(streamer));
        gfx::FramebufferDesc targetDesc;
        targetDesc.extent = {64, 64};
        targetDesc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(targetDesc);
        REQUIRE(target.hasValue());
        engine::Camera camera;
        camera.eye = {32.f, 92.f, 108.f};
        camera.center = {32.f, 20.f, 32.f};
        REQUIRE(view.render(target.value(), camera, 1.f) == gfx::GfxError::None);
        CHECK(view.renderStats().draws > 0);
        CHECK(device.stats().pipelineCount == preparedPipelines);
        const auto residentBytes = device.stats().bufferBytes;
        REQUIRE(view.sync(streamer));
        CHECK(view.uploadStats().uploaded == 0);
        REQUIRE(demo::applyStreamingDemoEdits(streamer, demo::streamingDemoPatch()));
        REQUIRE(view.sync(streamer));
        CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.residentCount() == 9);
        CHECK(device.stats().bufferBytes == residentBytes);
        CHECK_FALSE(view.caughtUp(streamer));
        CHECK_FALSE(view.acceptedSurfaceCurrent(streamer));
        REQUIRE(settle(streamer, {}, 9));
        REQUIRE(view.sync(streamer, 25));
        CHECK(view.uploadStats().uploaded == 1);
        CHECK(view.caughtUp(streamer));
        CHECK(view.acceptedSurfaceCurrent(streamer));
        REQUIRE(view.render(target.value(), camera, 1.f) == gfx::GfxError::None);
        CHECK(device.stats().pipelineCount == preparedPipelines);
        REQUIRE(settle(streamer, {512.f, 0.f, 0.f}, 9));
        REQUIRE(view.sync(streamer, 0));
        CHECK(view.uploadStats().retired == 9);
        CHECK(view.residentCount() == 0);
        REQUIRE(view.sync(streamer, 25));
        CHECK(view.caughtUp(streamer));
        CHECK(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().bufferCount == baseline.bufferCount);
    CHECK(device.stats().textureCount == baseline.textureCount);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: shared deadline bounds uploads without starving progress or retirement") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        auto config = demo::streamingDemoConfig();
        CHECK(config.frameBudget > std::chrono::microseconds::zero());
        CHECK(config.frameBudget + demo::kStreamingDemoUploadReserve == demo::kStreamingDemoFrameBudget);
        CHECK_FALSE(config.deterministicAdmission);
        config.horizontalRadius = 1;
        world::WorldStreamer streamer(config);
        REQUIRE(settle(streamer, {}, 9));
        demo::StreamingDemoView view(device);
        REQUIRE(view.init());
        // Fixed extreme deadlines test admission, not machine-dependent latency.
        const auto expired = std::chrono::steady_clock::time_point::min();
        const auto available = std::chrono::steady_clock::time_point::max();
        REQUIRE(view.sync(streamer, 0, expired));
        CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.uploadStats().deferred == 9);
        CHECK(view.uploadStats().budgetDeferred == 0);
        CHECK_FALSE(view.uploadStats().progressAfterDeadline);
        for (std::uint32_t count = 1; count <= 9; ++count) {
            REQUIRE(view.sync(streamer, 25, expired));
            CHECK(view.residentCount() == count);
            CHECK(view.uploadStats().uploaded == 1);
            CHECK(view.uploadStats().deferred == 9 - count);
            CHECK(view.uploadStats().budgetDeferred == 9 - count);
            CHECK(view.uploadStats().progressAfterDeadline);
        }
        CHECK(view.caughtUp(streamer));
        REQUIRE(view.sync(streamer, 25, expired));
        CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.uploadStats().deferred == 0);
        CHECK(view.uploadStats().budgetDeferred == 0);
        CHECK_FALSE(view.uploadStats().progressAfterDeadline);

        view.clear(streamer);
        REQUIRE(view.sync(streamer, 2, available));
        CHECK(view.uploadStats().uploaded == 2);
        CHECK(view.uploadStats().deferred == 7);
        CHECK(view.uploadStats().budgetDeferred == 0);
        CHECK_FALSE(view.uploadStats().progressAfterDeadline);
        REQUIRE(view.sync(streamer, 25, available));
        CHECK(view.caughtUp(streamer));
        const auto residentBytes = device.stats().bufferBytes;
        REQUIRE(demo::applyStreamingDemoEdits(streamer, demo::streamingDemoPatch()));
        REQUIRE(view.sync(streamer, 25, expired));
        CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.uploadStats().deferred == 0); // Dirty CPU data is not ready.
        CHECK(device.stats().bufferBytes == residentBytes);
        REQUIRE(settle(streamer, {}, 9));
        REQUIRE(view.sync(streamer, 0, expired));
        CHECK(view.uploadStats().deferred == 1);
        CHECK(device.stats().bufferBytes == residentBytes);
        CHECK_FALSE(view.caughtUp(streamer));
        REQUIRE(view.sync(streamer, 25, expired));
        CHECK(view.uploadStats().uploaded == 1);
        CHECK(view.uploadStats().progressAfterDeadline);
        CHECK(view.caughtUp(streamer));

        REQUIRE(settle(streamer, {512.f, 0.f, 0.f}, 9));
        REQUIRE(view.sync(streamer, 0, expired));
        CHECK(view.uploadStats().retired == 9);
        CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.uploadStats().deferred == 9);
        CHECK(view.residentCount() == 0);
        REQUIRE(view.sync(streamer, 25, available));
        CHECK(view.caughtUp(streamer));
    }
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().bufferCount == baseline.bufferCount);
    CHECK(device.stats().textureCount == baseline.textureCount);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: direct replay validation preserves codec rules and rejects before mutation") {
    auto config = demo::streamingDemoConfig();
    config.horizontalRadius = 0;
    world::WorldStreamer streamer(config);
    const auto patch = demo::streamingDemoPatch();
    CHECK_FALSE(demo::applyStreamingDemoEdits(streamer, {}));
    CHECK_FALSE(demo::applyStreamingDemoEdits(streamer, patch));
    REQUIRE(settle(streamer, {}, 1));
    const auto version = streamer.chunk(demo::kEditedChunk)->version;
    const auto before = streamer.sampleHeightTerrain(32.f, 32.f);
    containers::Array<world::ChunkDelta> invalid;
    invalid.push({world::kHeightSampleCount, patch[0].value});
    invalid.push({0, world::CellValue::voxelValue(1)});
    for (const float height : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
        invalid.push({0, world::CellValue::heightValue({height, 0, {255, 0, 0, 0}})});
    }
    for (const auto weights : {std::array<std::uint8_t, 4>{0, 0, 0, 0},
                               std::array<std::uint8_t, 4>{254, 0, 0, 0},
                               std::array<std::uint8_t, 4>{255, 1, 0, 0},
                               std::array<std::uint8_t, 4>{255, 255, 255, 255}}) {
        invalid.push({0, world::CellValue::heightValue({1.f, 0, weights})});
    }
    for (const auto& bad : invalid) {
        auto edits = patch;
        edits.push(bad);
        CHECK_FALSE(demo::encodeStreamingDemoSave(edits).has_value());
        CHECK_FALSE(demo::applyStreamingDemoEdits(streamer, edits));
        CHECK(streamer.chunk(demo::kEditedChunk)->version == version);
        CHECK(streamer.sampleHeightTerrain(32.f, 32.f) == before);
    }
    REQUIRE(demo::applyStreamingDemoEdits(streamer, {}));
    CHECK(streamer.chunk(demo::kEditedChunk)->version == version);
    auto duplicates = patch;
    duplicates.push({world::heightSampleIndex(32, 32),
        world::CellValue::heightValue({-12.f, 65535, {1, 2, 3, 249}})});
    const auto encoded = demo::encodeStreamingDemoSave(duplicates);
    REQUIRE(encoded.has_value());
    REQUIRE(demo::applyStreamingDemoEdits(streamer, duplicates));
    CHECK(streamer.sampleHeightTerrain(32.f, 32.f)->height == -12.f);
    CHECK(streamer.chunk(demo::kEditedChunk)->version == version + duplicates.size());
    REQUIRE(settle(streamer, {512.f, 0.f, 0.f}, 1));
    REQUIRE(settle(streamer, {}, 1));
    CHECK(streamer.sampleHeightTerrain(32.f, 32.f)->height == -12.f);
}

TEST_CASE("Streaming demo: reload detaches drawing immediately and bounds GPU retirement") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        auto config = demo::streamingDemoConfig();
        config.horizontalRadius = 1;
        world::WorldStreamer world(config);
        world::WorldStreamer emptyWorld(config);
        REQUIRE(settle(world, {}, 9));
        demo::StreamingDemoView view(device);
        REQUIRE(view.init());
        REQUIRE(view.sync(world, 25));
        gfx::FramebufferDesc desc;
        desc.extent = {64, 64};
        desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc);
        REQUIRE(target.hasValue());
        engine::Camera camera;
        camera.eye = {32.f, 92.f, 108.f};
        camera.center = {32.f, 20.f, 32.f};
        REQUIRE(view.render(target.value(), camera, 1.f) == gfx::GfxError::None);
        CHECK(view.renderStats().draws > 0);
        const auto beforeDetach = device.stats().bufferBytes;
        REQUIRE(view.raycastDisplayedTerrain({32, 100, 32}, {0, -1, 0}, 200));
        REQUIRE(world.raycastAcceptedTerrain({32, 100, 32}, {0, -1, 0}, 200));
        REQUIRE(view.beginReload(world));
        CHECK_FALSE(view.raycastDisplayedTerrain({32, 100, 32}, {0, -1, 0}, 200));
        CHECK_FALSE(world.raycastAcceptedTerrain({32, 100, 32}, {0, -1, 0}, 200));
        CHECK_FALSE(view.acceptedSurfaceCurrent(world));
        CHECK(view.residentCount() == 0);
        CHECK(view.pendingRetirements() == 9);
        CHECK(device.stats().bufferBytes == beforeDetach);
        CHECK_FALSE(view.beginReload());
        CHECK_FALSE(view.caughtUp(emptyWorld));
        REQUIRE(view.render(target.value(), camera, 1.f) == gfx::GfxError::None);
        CHECK(view.renderStats().draws == 0);
        REQUIRE(view.sync(emptyWorld, 0, std::chrono::steady_clock::time_point::min()));
        CHECK(view.uploadStats().reloadRetired == 1);
        CHECK(view.pendingRetirements() == 8);
        REQUIRE(view.sync(world, 25, std::chrono::steady_clock::time_point::max()));
        CHECK(view.uploadStats().reloadRetired == 2);
        CHECK(view.residentCount() == 9);
        CHECK(view.pendingRetirements() == 6);
        CHECK_FALSE(view.caughtUp(world));
        for (unsigned remaining : {4u, 2u, 0u}) {
            REQUIRE(view.sync(world));
            CHECK(view.uploadStats().reloadRetired == 2);
            CHECK(view.uploadStats().uploaded == 0);
            CHECK(view.pendingRetirements() == remaining);
        }
        CHECK(view.caughtUp(world));
        REQUIRE(view.beginReload());
        SUBCASE("explicit clear drains pending GPU resources") {
            view.clear(world);
            CHECK(view.pendingRetirements() == 0);
            CHECK(view.residentCount() == 0);
        }
        SUBCASE("destruction drains pending GPU resources") {
            CHECK(view.pendingRetirements() == 9);
        }
        CHECK(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().bufferCount == baseline.bufferCount);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
    CHECK(device.stats().textureCount == baseline.textureCount);
}

TEST_CASE("Streaming demo: policy mesh origins preserve signed coordinates and hex scale") {
    auto config = demo::streamingDemoConfig();
    config.policyParams.hex.radius = 2;
    config.hexRadius = 2.f;
    config.policyParams.hex.layout = world::HexLayout::LegacySpaced;
    world::WorldStreamer streamer(config);
    const world::ChunkCoord coord{-2, 3, 4};
    CHECK(streamer.meshOrigin({world::PolicyKind::Height, coord}) == math::Vec3{-128.f, 0.f, 256.f});
    CHECK(streamer.meshOrigin({world::PolicyKind::Voxel, coord}) == math::Vec3{-64.f, 96.f, 128.f});
    const auto hex = streamer.meshOrigin({world::PolicyKind::Hex, coord});
    CHECK(hex.x == doctest::Approx(0.f));
    CHECK(hex.y == 0.f);
    CHECK(hex.z == doctest::Approx(60.f));
    const auto negative = streamer.meshOrigin({world::PolicyKind::Hex, {0, 0, -1}});
    CHECK(negative.x == doctest::Approx(-8.660254f));
    CHECK(negative.z == doctest::Approx(-15.f));
}

TEST_CASE("Streaming demo: empty voxel versions converge without GPU meshes and can become visible") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        auto config = demo::streamingDemoConfig();
        config.defaultPolicy = world::PolicyKind::Voxel;
        config.horizontalRadius = 0;
        world::WorldStreamer streamer(config);
        demo::StreamingDemoView view(device);
        REQUIRE(view.init());
        const auto emptyBytes = device.stats().bufferBytes;
        const math::Vec3 anchor{0.f, 64.f, 0.f};
        const world::StreamingChunkKey key{world::PolicyKind::Voxel, {0, 2, 0}};
        REQUIRE(settle(streamer, anchor, 1));
        REQUIRE(streamer.mesh(key) != nullptr);
        REQUIRE(streamer.mesh(key)->indices.isEmpty());
        REQUIRE(view.sync(streamer, 0));
        CHECK_FALSE(view.caughtUp(streamer));
        REQUIRE(view.sync(streamer));
        CHECK(view.caughtUp(streamer));
        CHECK(view.residentCount() == 1);
        CHECK(view.uploadStats().bytes == 0);
        CHECK(device.stats().bufferBytes == emptyBytes);
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            REQUIRE(streamer.edit(key, 0, world::CellValue::voxelValue(1)));
            REQUIRE(settle(streamer, anchor, 1));
            REQUIRE(view.sync(streamer));
            CHECK(view.caughtUp(streamer));
            CHECK(device.stats().bufferBytes > emptyBytes);
            REQUIRE(streamer.edit(key, 0, world::CellValue::voxelValue(0)));
            REQUIRE(settle(streamer, anchor, 1));
            REQUIRE(view.sync(streamer));
            CHECK(view.caughtUp(streamer));
            CHECK(device.stats().bufferBytes == emptyBytes);
        }
        REQUIRE(view.beginReload());
        CHECK(view.pendingRetirements() == 0);
        CHECK(view.residentCount() == 0);
    }
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: stationary region switches draw every surface and retire old policies") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        auto config = demo::streamingDemoConfig();
        config.horizontalRadius = 0;
        config.policyParams.hex.radius = 2;
        world::WorldStreamer streamer(config);
        demo::StreamingDemoView view(device);
        REQUIRE(view.init());
        const auto pipelines = device.stats().pipelineCount;
        gfx::FramebufferDesc desc;
        desc.extent = {64, 64};
        desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc);
        REQUIRE(target.hasValue());
        engine::Camera camera;
        camera.eye = {32.f, 160.f, 100.f};
        camera.center = {16.f, 0.f, 16.f};
        camera.farPlane = 600.f;
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            for (const auto policy : {world::PolicyKind::Voxel, world::PolicyKind::Hex,
                                      world::PolicyKind::Height}) {
                streamer.regions().set({0, 0}, policy);
                REQUIRE(settle(streamer, {}, 1));
                const world::StreamingChunkKey key{policy, {}};
                if (policy == world::PolicyKind::Hex) {
                    REQUIRE(streamer.edit(key, 12, world::CellValue::hexValue({200, 0})));
                    REQUIRE(settle(streamer, {}, 1));
                }
                const auto* mesh = streamer.mesh(key);
                REQUIRE(mesh != nullptr);
                // Multiple terrain materials must not drop every surface after the first.
                if (policy != world::PolicyKind::Height) CHECK(mesh->surfaces.size() > 1);
                REQUIRE(view.sync(streamer));
                CHECK(view.caughtUp(streamer));
                CHECK(view.residentCount() == 1);
                device.resetFrameStats();
                REQUIRE(view.render(target.value(), camera, 1.f) == gfx::GfxError::None);
                CHECK(view.renderStats().draws == 1);
                CHECK(device.stats().vertices == mesh->indices.size());
                CHECK(device.stats().pipelineCount == pipelines);
            }
        }
        CHECK(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount);
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
    CHECK(device.stats().textureCount == baseline.textureCount);
}

TEST_CASE("Streaming demo: failed terrain pipeline preparation releases temporary resources") {
    gfx::NullDevice device;
    const auto baseline = device.stats();
    {
        device.failNextPipelineCreation(gfx::GfxError::OutOfMemory);
        demo::StreamingDemoView view(device);
        CHECK_FALSE(view.init());
        CHECK(view.residentCount() == 0);
        CHECK(device.stats().pipelineCount == baseline.pipelineCount);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount);
    CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount);
    CHECK(device.stats().shaderCount == baseline.shaderCount);
    CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: composed corner dependencies and GPU/query installation remain coordinated") {
    auto config = demo::streamingMixedDemoConfig();
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true;
    world::WorldStreamer streamer(config); demo::setStreamingMixedRegions(streamer.regions());
    REQUIRE(settle(streamer, {}, 22));
    const std::array requests{
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, 64}},
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, 64}}};
    std::array<world::StreamingBoundarySnapshot, 2> captures;
    std::size_t totalSources = 0;
    for (unsigned i = 0; i < 2; ++i) {
        auto snapshot = streamer.captureBoundary(requests[i]); REQUIRE(snapshot.hasValue());
        captures[i] = std::move(snapshot).value();
        totalSources += captures[i].sources(false).size() + captures[i].sources(true).size();
    }
    std::optional<world::StreamingBoundaryComposition> packet;
    std::thread worker([&] { auto built = world::StreamingBoundaryComposition::build(captures); if (built) packet = std::move(built).value(); });
    worker.join(); REQUIRE(packet); CHECK(streamer.boundaryCurrent(*packet));
    CHECK(packet->sources().size() < totalSources);
    CHECK_FALSE(world::StreamingBoundaryComposition::build({}));
    CHECK_FALSE(world::StreamingBoundaryComposition::build(captures, {}, 0));
    world::BoundaryCompositionLimits limits; limits.mesh.maxSources = packet->sources().size() - 1;
    CHECK_FALSE(world::StreamingBoundaryComposition::build(captures, limits));
    const std::array invalid{world::StreamingBoundarySnapshot{}};
    CHECK_FALSE(world::StreamingBoundaryComposition::build(invalid));
    world::WorldStreamer another(config); CHECK_FALSE(another.boundaryCurrent(*packet));
    demo::setStreamingMixedRegions(another.regions()); REQUIRE(settle(another, {}, 22));
    const auto otherCapture = another.captureBoundary(requests[1]); REQUIRE(otherCapture.hasValue());
    const std::array mixedOwners{captures[0], otherCapture.value()};
    CHECK_FALSE(world::StreamingBoundaryComposition::build(mixedOwners));
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        REQUIRE(view.sync(streamer, 25)); REQUIRE(view.caughtUp(streamer));
        const auto ordinary = device.stats();
        const auto* mesh = packet->geometry(); REQUIRE(mesh);
        std::size_t wall = 0;
        while (wall < mesh->triangles.size() && !mesh->triangles[wall].wall) ++wall;
        REQUIRE(wall < mesh->triangles.size());
        const auto a = mesh->mesh.vertices[mesh->mesh.indices[wall * 3]],
            b = mesh->mesh.vertices[mesh->mesh.indices[wall * 3 + 1]],
            c = mesh->mesh.vertices[mesh->mesh.indices[wall * 3 + 2]];
        const auto point = (a.position + b.position + c.position) / 3.f;
        const auto eye = point + a.normal * 0.25f;
        const auto old = view.raycastDisplayedTerrain(eye, -a.normal, 0.5f, true);
        for (const std::size_t successfulBuffers : {0u, 1u, 2u, 3u}) {
            // Two canonical buffers/policy: count 2 fails after the first complete mesh is staged.
            device.failBufferCreationAfter(successfulBuffers);
            CHECK_FALSE(view.installBoundaryComposition(streamer, *packet));
            CHECK_FALSE(view.hasBoundaryComposition()); CHECK(view.caughtUp(streamer));
            CHECK(device.stats().bufferCount == ordinary.bufferCount);
            CHECK(device.stats().bufferBytes == ordinary.bufferBytes);
            const auto retained = view.raycastDisplayedTerrain(eye, -a.normal, 0.5f, true);
            CHECK(retained.has_value() == old.has_value());
            if (old && retained) CHECK(retained->source.surface.distance == old->source.surface.distance);
        }
        REQUIRE(view.installBoundaryComposition(streamer, *packet)); CHECK(view.hasBoundaryComposition());
        CHECK(view.acceptedSurfaceCurrent(streamer));
        const auto installed = view.raycastDisplayedTerrain(eye, -a.normal, 0.5f, true);
        REQUIRE(installed); CHECK(installed->boundary.has_value());
        CHECK(installed->source.surface.distance == doctest::Approx(0.25));
        CHECK(installed->source.surface.triangleIndex == mesh->triangles[installed->composedTriangle].sourceTriangle);
        gfx::FramebufferDesc desc; desc.extent = {64, 64}; desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc); REQUIRE(target.hasValue());
        engine::Camera camera; camera.eye = {32, 150, 100}; camera.center = {32, 0, 32}; camera.farPlane = 600;
        REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None);
        CHECK(view.renderStats().candidates == 22 - packet->sources().size() + 2);
        const auto vertices = device.stats().vertices;
        const auto acceptedBytes = device.stats().bufferBytes;
        // A north-only source change invalidates the union, even if the east capture is current.
        REQUIRE(streamer.edit({world::PolicyKind::Height, {0, 0, 1}}, world::heightSampleIndex(32, 0),
            world::CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
        CHECK(streamer.boundaryCurrent(captures[0])); CHECK_FALSE(streamer.boundaryCurrent(*packet));
        REQUIRE(settle(streamer, {}, 22)); REQUIRE(view.sync(streamer, 25)); CHECK_FALSE(view.caughtUp(streamer));
        CHECK_FALSE(view.acceptedSurfaceCurrent(streamer));
        const auto newNorth = streamer.captureBoundary(requests[1]); REQUIRE(newNorth.hasValue());
        const std::array freshUnion{captures[0], newNorth.value()};
        const auto rebuilt = world::StreamingBoundaryComposition::build(freshUnion);
        REQUIRE(rebuilt.hasValue()); CHECK(streamer.boundaryCurrent(rebuilt.value()));
        CHECK(device.stats().bufferBytes == acceptedBytes);
        const auto frozen = view.raycastDisplayedTerrain(eye, -a.normal, 0.5f, true); REQUIRE(frozen);
        CHECK(frozen->source.meshVersion == installed->source.meshVersion);
        CHECK(frozen->source.surface.distance == installed->source.surface.distance);
        device.resetFrameStats(); REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None);
        CHECK(device.stats().vertices == vertices);
        view.removeBoundaryComposition(streamer); CHECK_FALSE(view.hasBoundaryComposition());
        CHECK_FALSE(view.installBoundaryComposition(streamer, *packet));
        REQUIRE(view.sync(streamer, 25)); CHECK(view.caughtUp(streamer));
        CHECK(view.acceptedSurfaceCurrent(streamer));
        REQUIRE(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: live composed replacement rolls back all stages and follows edits") {
    auto config = demo::streamingMixedDemoConfig();
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true;
    world::WorldStreamer streamer(config); demo::setStreamingMixedRegions(streamer.regions());
    REQUIRE(settle(streamer, {}, 22));
    const std::array requests{
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, 64}},
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, 64}}};
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool); REQUIRE(job.request(requests));
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        // Full-reupload reference keeps fault injection at every allocation stage.
        const auto sync = [&] { return view.syncBoundaryComposition(streamer, job, {32, 32, {}, false}); };
        REQUIRE(sync()); pool.waitIdle();
        REQUIRE(sync()); REQUIRE(view.caughtUp(streamer));
        REQUIRE(view.hasBoundaryComposition());
        const auto originalBytes = device.stats().bufferBytes;
        const auto old = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(old);
        const world::StreamingChunkKey key{world::PolicyKind::Height, {0, 0, 1}};
        REQUIRE(streamer.edit(key, world::heightSampleIndex(32, 0), world::CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
        CHECK_FALSE(view.caughtUp(streamer));
        REQUIRE(sync());
        CHECK(view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200)->source.surface.distance == old->source.surface.distance);
        REQUIRE(settle(streamer, {}, 22));
        job.update(streamer); pool.waitIdle(); REQUIRE(job.update(streamer).state == world::BoundaryCompositionState::Ready);
        const auto stable = device.stats();
        std::size_t sourceBuffers = 0;
        for (const auto& chunk : streamer.residentChunks()) if (!streamer.mesh(chunk.key)->indices.isEmpty()) sourceBuffers += 2;
        // Fail before/within originals, then within/between the complete policy meshes.
        for (const auto count : {std::size_t{0}, std::size_t{1}, sourceBuffers, sourceBuffers + 1, sourceBuffers + 2, sourceBuffers + 3}) {
            device.failBufferCreationAfter(count); CHECK_FALSE(sync());
            CHECK(view.hasBoundaryComposition()); CHECK_FALSE(view.caughtUp(streamer));
            CHECK(device.stats().bufferCount == stable.bufferCount); CHECK(device.stats().bufferBytes == stable.bufferBytes);
            const auto hit = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(hit);
            CHECK(hit->source.surface.distance == old->source.surface.distance);
            CHECK(hit->source.meshVersion == old->source.meshVersion);
        }
        REQUIRE(sync()); CHECK(view.caughtUp(streamer));
        const auto changed = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(changed);
        CHECK(changed->source.surface.position.y == doctest::Approx(45));
        const auto currentBytes = device.stats().bufferBytes;
        for (unsigned i = 0; i < 3; ++i) REQUIRE(sync());
        CHECK(device.stats().bufferBytes == currentBytes); CHECK(job.stats().submitted == 2);
        // Moving residency away makes the requested corner incomplete: keep the accepted view.
        for (unsigned i = 0; i < 128 && (i == 0 || streamer.hasPendingWork()); ++i) {
            streamer.update({512, 0, 512}); streamer.waitForIdle();
        }
        CHECK_FALSE(view.caughtUp(streamer)); job.update(streamer); pool.waitIdle();
        CHECK_FALSE(sync()); CHECK(view.hasBoundaryComposition());
        CHECK(device.stats().bufferBytes == currentBytes);
        REQUIRE(settle(streamer, {}, 22));
        REQUIRE(sync()); pool.waitIdle();
        REQUIRE(sync()); CHECK(view.caughtUp(streamer));
        CHECK(device.stats().bufferBytes == currentBytes); // Edit survives eviction/reload.
        CHECK(originalBytes > 0);
        view.removeBoundaryComposition(streamer); CHECK(view.caughtUp(streamer));
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: incremental corner uploads stay hidden and stale residents cancel staging") {
    // Full-reupload reference: exercise every original incremental failure stage.
    auto config = demo::streamingMixedDemoConfig();
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true;
    world::WorldStreamer streamer(config); demo::setStreamingMixedRegions(streamer.regions());
    REQUIRE(settle(streamer, {}, 22));
    const std::array requests{
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, 64}},
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, 64}}};
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool); REQUIRE(job.request(requests));
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, {}, false})); pool.waitIdle();
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, {}, false})); REQUIRE(view.caughtUp(streamer));
        gfx::FramebufferDesc desc; desc.extent = {64, 64}; desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc); REQUIRE(target.hasValue());
        engine::Camera camera; camera.eye = {32, 150, 100}; camera.center = {32, 0, 32}; camera.farPlane = 600;
        const auto render = [&] {
            device.resetFrameStats(); REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None);
            return device.stats().vertices;
        };
        const auto oldVertices = render(); const auto oldCandidates = view.renderStats().candidates;
        const auto old = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(old);
        REQUIRE(streamer.edit({world::PolicyKind::Height, {0, 0, 1}}, world::heightSampleIndex(32, 0),
            world::CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
        REQUIRE(settle(streamer, {}, 22)); job.update(streamer); pool.waitIdle();
        REQUIRE(job.update(streamer).state == world::BoundaryCompositionState::Ready);
        const auto bytes = device.stats().bufferBytes;
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 0, {}, false})); CHECK_FALSE(view.boundaryUploadPending());
        const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, expired, false}));
        CHECK(device.stats().bufferBytes == bytes); CHECK(view.uploadStats().uploaded == 0);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {4, 0, {}, false})); REQUIRE(view.boundaryUploadPending());
        CHECK(view.uploadStats().uploaded == 4); CHECK_FALSE(view.beginReload());
        CHECK_FALSE(view.replaceBoundaryComposition(streamer, *job.result(streamer)));
        REQUIRE(view.sync(streamer, 25)); CHECK(render() == oldVertices); CHECK(view.renderStats().candidates == oldCandidates);
        // The composed dependency set alone is insufficient: an ordinary resident
        // outside that set can also change while its next view is being uploaded.
        std::optional<world::StreamingChunkKey> outside;
        for (const auto& chunk : streamer.residentChunks()) {
            if (chunk.key.policy == world::PolicyKind::Height &&
                std::none_of(job.result(streamer)->sources().begin(), job.result(streamer)->sources().end(),
                    [&](const auto& source) { return source.key == chunk.key; })) { outside = chunk.key; break; }
        }
        REQUIRE(outside);
        REQUIRE(streamer.edit(*outside, world::heightSampleIndex(32, 32), world::CellValue::heightValue({33, 0, {255, 0, 0, 0}})));
        REQUIRE(job.result(streamer)); // Corner sources themselves still match.
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 0, {}, false}));
        CHECK_FALSE(view.boundaryUploadPending()); REQUIRE(view.pendingRetirements() > 0);
        const auto retired = view.pendingRetirements();
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, expired, false})); CHECK(view.pendingRetirements() == retired);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 1, {}, false})); CHECK(view.pendingRetirements() == retired - 1);
        CHECK(view.uploadStats().retired == 1); CHECK(view.uploadStats().uploaded == 0);
        REQUIRE(settle(streamer, {}, 22));
        while (view.pendingRetirements()) REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 1, {}, false}));
        // A late upload failure cancels hidden resources but preserves the accepted view.
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false}));
        device.failBufferCreationAfter(0);
        CHECK_FALSE(view.syncBoundaryComposition(streamer, job, {32, 0, {}, false}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK(render() == oldVertices);
        while (view.pendingRetirements()) REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 1, {}, false}));
        unsigned ticks = 0;
        do {
            REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false})); ++ticks;
            CHECK(view.uploadStats().uploaded <= 1); CHECK(view.uploadStats().retired == 0);
            if (view.boundaryUploadPending()) {
                CHECK(render() == oldVertices); CHECK(view.renderStats().candidates == oldCandidates);
                const auto hit = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(hit);
                CHECK(hit->source.surface.distance == old->source.surface.distance);
            }
        } while (view.boundaryUploadPending() && ticks < 32);
        CHECK(ticks == 25); // 22 resident units plus three policy-group units.
        CHECK_FALSE(view.boundaryUploadPending()); REQUIRE(view.pendingRetirements() > 0);
        const auto changed = view.raycastDisplayedTerrain({32, 100, 64}, {0, -1, 0}, 200); REQUIRE(changed);
        CHECK(changed->source.surface.position.y == doctest::Approx(45));
        const auto newVertices = render(); const auto newCandidates = view.renderStats().candidates;
        while (view.pendingRetirements()) {
            const auto count = view.pendingRetirements();
            REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 1, {}, false}));
            CHECK(view.pendingRetirements() == count - 1); CHECK(view.uploadStats().retired == 1);
            CHECK(view.uploadStats().uploaded == 0); CHECK(render() == newVertices);
            CHECK(view.renderStats().candidates == newCandidates);
        }
        CHECK(view.caughtUp(streamer));
        // Clear while staging must release both active and pending resources.
        REQUIRE(job.request(requests)); job.update(streamer); pool.waitIdle();
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false})); REQUIRE(view.boundaryUploadPending());
        view.clear(streamer); CHECK_FALSE(view.boundaryUploadPending()); CHECK(view.pendingRetirements() == 0);
        REQUIRE(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: reused residents survive composed staging failure and transfer exactly once") {
    auto config = demo::streamingMixedDemoConfig();
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true;
    world::WorldStreamer streamer(config); demo::setStreamingMixedRegions(streamer.regions());
    REQUIRE(settle(streamer, {}, 22));
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool);
    REQUIRE_FALSE(job.requestWindow(streamer.regions(), {0, 96, 0, 96}));
    job.update(streamer); pool.waitIdle(); REQUIRE(job.update(streamer).state == world::BoundaryCompositionState::Ready);
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        REQUIRE(view.sync(streamer, 25));
        gfx::FramebufferDesc desc; desc.extent = {64, 64}; desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc); REQUIRE(target);
        engine::Camera camera; camera.eye = {32, 150, 100}; camera.center = {32, 0, 32}; camera.farPlane = 600;
        const auto render = [&] {
            device.resetFrameStats(); REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None);
            return device.stats().vertices;
        };
        const auto oldVertices = render();
        // Rendering may lazily create frame resources; compare after that
        // initialization, not against the pre-render device snapshot.
        const auto original = device.stats();
        const auto old = view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true);
        // All original GPU meshes are borrowed; the empty voxel group consumes
        // the first work unit. No allocation or visibility change is permitted.
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}}));
        REQUIRE(view.boundaryUploadPending()); CHECK(view.uploadStats().reused == 22);
        CHECK(view.uploadStats().uploaded == 1); CHECK(view.uploadStats().bytes == 0);
        CHECK(device.stats().bufferBytes == original.bufferBytes);
        device.failBufferCreationAfter(1); // Fail midway through the first composed policy mesh.
        CHECK_FALSE(view.syncBoundaryComposition(streamer, job, {32, 32, {}}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK_FALSE(view.hasBoundaryComposition());
        CHECK(device.stats().bufferBytes == original.bufferBytes); CHECK(view.pendingRetirements() == 0);
        CHECK(render() == oldVertices);
        const auto retained = view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true);
        CHECK(retained.has_value() == old.has_value());
        if (retained && old) CHECK(retained->source.surface.distance == old->source.surface.distance);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, {}}));
        CHECK(view.uploadStats().reused == 22); CHECK(view.uploadStats().uploaded == 3);
        CHECK(view.uploadStats().retired == 0); REQUIRE(view.hasBoundaryComposition());
        const auto wall = view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true);
        REQUIRE(wall); CHECK(wall->boundary.has_value());
        // Retirement needs no original upload when all versions already match.
        job.requestRetirement(); REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK_FALSE(view.hasBoundaryComposition());
        CHECK(view.uploadStats().reused == 22); CHECK(view.uploadStats().uploaded == 0);
        CHECK(view.pendingRetirements() == 2);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 2, {}})); CHECK(view.caughtUp(streamer));
        CHECK(view.uploadStats().retired == 2); CHECK(view.pendingRetirements() == 0);
        // Cancellation with borrowed-only staging must not enqueue those handles.
        REQUIRE_FALSE(job.requestWindow(streamer.regions(), {0, 96, 0, 96}));
        job.update(streamer); pool.waitIdle(); job.update(streamer);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}})); REQUIRE(view.boundaryUploadPending());
        job.clear(); REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 0, {}}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK(view.pendingRetirements() == 0);
        REQUIRE_FALSE(job.requestWindow(streamer.regions(), {0, 96, 0, 96}));
        job.update(streamer); pool.waitIdle(); job.update(streamer);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}})); REQUIRE(view.boundaryUploadPending());
        view.clear(streamer); CHECK(view.residentCount() == 0); CHECK(view.pendingRetirements() == 0);
        REQUIRE(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: recycled world stamps cannot reuse a resident or its old transform") {
    auto config = demo::streamingDemoConfig(); config.frameBudget = std::chrono::seconds(1);
    config.defaultPolicy = world::PolicyKind::Hex; config.horizontalRadius = 0;
    config.chunkCapacityPerPolicy = 1; config.policyParams.hex.radius = 2; config.hexRadius = 1;
    world::WorldStreamer first(config); REQUIRE(settle(first, {100, 0, 100}, 1));
    config.hexRadius = 16;
    world::WorldStreamer second(config); REQUIRE(settle(second, {1600, 0, 1600}, 1));
    world::WorldStreamer third(config); REQUIRE(settle(third, {1600, 0, 1600}, 1));
    const auto a = first.residentChunks()[0], b = second.residentChunks()[0], c = third.residentChunks()[0];
    REQUIRE(a.key == b.key); REQUIRE(a.handle == b.handle); REQUIRE(a.meshVersion == b.meshVersion);
    REQUIRE(b.key == c.key); REQUIRE(b.handle == c.handle); REQUIRE(b.meshVersion == c.meshVersion);
    const auto origin = second.meshOrigin(b.key);
    REQUIRE(origin.x != first.meshOrigin(a.key).x);
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool); job.requestRetirement();
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init()); REQUIRE(view.sync(first, 1));
        REQUIRE(view.caughtUp(first)); CHECK_FALSE(view.caughtUp(second));
        REQUIRE(view.sync(second, 1)); CHECK(view.uploadStats().uploaded == 1); CHECK(view.caughtUp(second));
        gfx::FramebufferDesc desc; desc.extent = {64, 64}; desc.attachments.push(gfx::AttachmentDesc{});
        const auto target = device.createFramebuffer(desc); REQUIRE(target);
        engine::Camera camera; camera.eye = origin + math::Vec3{0, 200, 50}; camera.center = origin; camera.farPlane = 500;
        REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None);
        CHECK(view.renderStats().visible == 1); // An old origin is outside this camera's far plane.
        CHECK(view.raycastDisplayedTerrain(origin + math::Vec3{0, 100, 0}, {0, -1, 0}, 200).has_value());
        CHECK_FALSE(view.caughtUp(third));
        REQUIRE(view.syncBoundaryComposition(third, job, {1, 1, {}}));
        CHECK(view.uploadStats().reused == 0); CHECK(view.uploadStats().uploaded == 1);
        CHECK(view.caughtUp(third)); CHECK_FALSE(view.caughtUp(second));
        REQUIRE(view.render(target.value(), camera, 1) == gfx::GfxError::None); CHECK(view.renderStats().visible == 1);
        REQUIRE(device.destroyFramebuffer(target.value()) == gfx::GfxError::None);
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: staged moving terrain reuses current residents but never another world") {
    auto config = demo::streamingDemoConfig(); config.frameBudget = std::chrono::seconds(1);
    world::WorldStreamer streamer(config); REQUIRE(settle(streamer, {}, 25));
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool); job.requestRetirement();
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        REQUIRE(view.syncBoundaryComposition(streamer, job, {32, 32, {}})); REQUIRE(view.caughtUp(streamer));
        const auto previous = streamer.residentChunks();
        REQUIRE(settle(streamer, {64, 0, 0}, 25));
        unsigned expectedReuse = 0;
        for (const auto& chunk : streamer.residentChunks()) {
            if (std::any_of(previous.begin(), previous.end(), [&](const auto& old) {
                return old.key == chunk.key && old.handle == chunk.handle && old.meshVersion == chunk.meshVersion;
            })) ++expectedReuse;
        }
        REQUIRE(expectedReuse > 0); REQUIRE(expectedReuse < 25);
        const auto old = view.raycastDisplayedTerrain({32, 100, 32}, {0, -1, 0}, 200); REQUIRE(old);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}})); REQUIRE(view.boundaryUploadPending());
        CHECK(view.raycastDisplayedTerrain({32, 100, 32}, {0, -1, 0}, 200)->source.meshVersion == old->source.meshVersion);
        unsigned uploaded = view.uploadStats().uploaded, reused = view.uploadStats().reused;
        unsigned ticks = 1;
        while (view.boundaryUploadPending() && ticks < 32) {
            REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}})); ++ticks;
            uploaded += view.uploadStats().uploaded; reused += view.uploadStats().reused;
            CHECK(view.uploadStats().uploaded <= 1);
        }
        CHECK_FALSE(view.boundaryUploadPending()); CHECK(uploaded == 25 - expectedReuse); CHECK(reused == expectedReuse);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 32, {}})); REQUIRE(view.caughtUp(streamer));
        // Identical keys/handles/versions from a different world must be uploaded,
        // not borrowed. The accepted query continues to reference the old world
        // until the replacement transaction commits.
        world::WorldStreamer other(config); REQUIRE(settle(other, {64, 0, 0}, 25));
        REQUIRE(view.syncBoundaryComposition(other, job, {1, 0, {}})); REQUIRE(view.boundaryUploadPending());
        CHECK(view.uploadStats().reused == 0); CHECK(view.uploadStats().uploaded == 1);
        job.clear(); REQUIRE(view.syncBoundaryComposition(other, job, {0, 0, {}}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK(view.pendingRetirements() == 1);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 32, {}})); CHECK(view.caughtUp(streamer));
        CHECK_FALSE(view.caughtUp(other));
        job.requestRetirement(); REQUIRE(view.syncBoundaryComposition(other, job, {32, 32, {}}));
        CHECK(view.uploadStats().reused == 0); CHECK(view.uploadStats().uploaded == 25); CHECK(view.caughtUp(other));
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}

TEST_CASE("Streaming demo: explicit retirement and retargeting preserve transactional visibility") {
    auto config = demo::streamingMixedDemoConfig();
    config.horizontalHalfExtentMeters = 96.f; config.chunkCapacityPerPolicy = 25;
    config.regionalSurfaceOwnership = true; config.regionalHexCutWalls = true;
    world::WorldStreamer streamer(config); demo::setStreamingMixedRegions(streamer.regions()); REQUIRE(settle(streamer, {}, 22));
    const std::array requests{
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::X, 64, 0, 64}},
        world::StreamingBoundaryRequest{{world::TerrainBoundaryAxis::Z, 64, 0, 64}}};
    jobs::JobSystem pool(1); world::BoundaryCompositionJob job(pool); REQUIRE(job.request(requests));
    gfx::NullDevice device; const auto baseline = device.stats();
    {
        demo::StreamingDemoView view(device); REQUIRE(view.init());
        const auto converge = [&](world::WorldStreamer& target) {
            for (unsigned tick = 0; tick < 128; ++tick) {
                if (!view.syncBoundaryComposition(target, job, {2, 2, {}})) return false;
                if (view.caughtUp(target) && !job.inFlight()) return true;
                pool.waitIdle();
            }
            return false;
        };
        REQUIRE(converge(streamer)); REQUIRE(view.hasBoundaryComposition());
        const auto wall = view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true); REQUIRE(wall);
        REQUIRE(wall->boundary.has_value()); const auto stableBytes = device.stats().bufferBytes;
        job.clear(); REQUIRE(view.syncBoundaryComposition(streamer, job)); CHECK(view.hasBoundaryComposition());
        // Force full reupload to retain a partially allocated retirement target.
        job.requestRetirement(); REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false}));
        REQUIRE(view.boundaryUploadPending()); CHECK(view.hasBoundaryComposition());
        REQUIRE(view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true)->boundary.has_value());
        // Retarget while plain uploads are pending: discard those hidden handles.
        REQUIRE(job.request(requests)); REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 0, {}}));
        CHECK_FALSE(view.boundaryUploadPending()); CHECK(view.pendingRetirements() > 0);
        pool.waitIdle(); REQUIRE(converge(streamer)); CHECK(view.hasBoundaryComposition());
        CHECK(device.stats().bufferBytes == stableBytes);
        // Region changes invalidate a plain transaction even before update.
        job.requestRetirement(); REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false}));
        streamer.regions().set({50, 50}, world::PolicyKind::Voxel);
        REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 0, {}})); CHECK_FALSE(view.boundaryUploadPending());
        REQUIRE(settle(streamer, {}, 22));
        while (view.pendingRetirements()) REQUIRE(view.syncBoundaryComposition(streamer, job, {0, 1, {}}));
        device.failBufferCreationAfter(0); CHECK_FALSE(view.syncBoundaryComposition(streamer, job, {32, 32, {}}));
        CHECK(view.hasBoundaryComposition()); REQUIRE(converge(streamer)); CHECK_FALSE(view.hasBoundaryComposition());
        CHECK(view.caughtUp(streamer));
        const auto plain = view.raycastDisplayedTerrain({62, 10, 32}, {1, 0, 0}, 4, true);
        CHECK_FALSE((plain && plain->boundary.has_value()));
        REQUIRE(job.request(requests)); REQUIRE(converge(streamer)); CHECK(view.hasBoundaryComposition());
        // A different world can recycle the same chunk handles/versions. Identity
        // must invalidate hidden plain uploads even when those stamps happen to match.
        job.requestRetirement(); REQUIRE(view.syncBoundaryComposition(streamer, job, {1, 0, {}, false}));
        world::WorldStreamer other(config); demo::setStreamingMixedRegions(other.regions());
        other.regions().set({50, 50}, world::PolicyKind::Voxel); REQUIRE(settle(other, {}, 22));
        CHECK(other.identityToken() != streamer.identityToken());
        REQUIRE(view.syncBoundaryComposition(other, job, {0, 0, {}})); CHECK_FALSE(view.boundaryUploadPending());
        CHECK(view.hasBoundaryComposition()); REQUIRE(converge(other)); CHECK_FALSE(view.hasBoundaryComposition());
        CHECK_FALSE(view.caughtUp(streamer)); CHECK(view.caughtUp(other));
        other.regions().set({51, 50}, world::PolicyKind::Voxel); CHECK_FALSE(view.caughtUp(other));
    }
    CHECK(device.stats().bufferCount == baseline.bufferCount); CHECK(device.stats().bufferBytes == baseline.bufferBytes);
    CHECK(device.stats().textureCount == baseline.textureCount); CHECK(device.stats().pipelineCount == baseline.pipelineCount);
}
