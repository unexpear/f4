#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include <doctest/doctest.h>

#include "engine/world/boundary_profile_job.h"
#include "engine/world/boundary_composition_job.h"

namespace {
using namespace f4;
using namespace f4::world;
auto settings() -> WorldStreamingConfig {
    WorldStreamingConfig c;
    c.worldSeed = 0x1616cafe12345678ull;
    c.workerCount = 2; c.horizontalHalfExtentMeters = 96.0f;
    c.chunkCapacityPerPolicy = 25; c.regionExtentMeters = 64;
    c.policyParams.hex.radius = 2; c.hexRadius = 8;
    c.regionalSurfaceOwnership = true;
    c.frameBudget = std::chrono::seconds(1);
    return c;
}
auto settle(WorldStreamer& world, math::Vec3 eye = {}) -> bool {
    for (unsigned i = 0; i < 128; ++i) {
        world.update(eye); world.waitForIdle();
        if (world.residentCount() && !world.hasPendingWork()) return true;
    }
    return false;
}
const StreamingBoundaryRequest east{{TerrainBoundaryAxis::X, 64, 0, 64}};
const StreamingBoundaryRequest north{{TerrainBoundaryAxis::Z, 64, 0, 64}};

// Deterministic queued-work tests; destruction always unblocks the pool, even
// when a REQUIRE aborts the test. Declare this after the pool.
struct WorkerGate {
    std::mutex mutex;
    std::condition_variable ready;
    bool released = false;
    jobs::JobSystem& pool;
    explicit WorkerGate(jobs::JobSystem& jobs) : pool(jobs) {
        if (!pool.submit({0, 0}, [this] {
            std::unique_lock lock(mutex);
            ready.wait(lock, [this] { return released; });
        })) throw std::runtime_error("Cannot queue test blocker");
    }
    void release() { { std::lock_guard lock(mutex); released = true; } ready.notify_all(); }
    ~WorkerGate() { release(); pool.waitIdle(); }
};
}

TEST_CASE("Composed jobs: discovered windows retain queued work and fail closed") {
    auto config = settings(); config.regionalHexCutWalls = true;
    WorldStreamer world(config); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    jobs::JobSystem pool(1); WorkerGate gate(pool); BoundaryCompositionJob job(pool);
    REQUIRE_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
    CHECK(job.update(world).state == BoundaryCompositionState::Queued);
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
        CHECK(job.update(world).state == BoundaryCompositionState::Queued);
    }
    CHECK(job.stats().submitted == 1);
    gate.release(); pool.waitIdle();
    REQUIRE(job.update(world).state == BoundaryCompositionState::Ready);
    const auto* accepted = job.result(world); REQUIRE(accepted);
    REQUIRE_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
    CHECK(job.result(world) == accepted);
    CHECK(job.stats().discarded == 0);
    // Reject even an unrelated unsupported edge in the selected window. It
    // must not silently return the earlier valid subset or retire the view.
    world.regions().set({1, 0}, PolicyKind::Voxel);
    const auto returned = discoverTerrainBoundaries(world.regions(), {0, 96, 0, 96});
    REQUIRE(returned);
    const bool returnedHeightVoxel = std::any_of(returned.value().begin(), returned.value().end(),
        [](const StreamingBoundaryRequest& edge) {
            return (edge.negative == PolicyKind::Height && edge.positive == PolicyKind::Voxel) ||
                   (edge.negative == PolicyKind::Voxel && edge.positive == PolicyKind::Height);
        });
    CHECK(returnedHeightVoxel);
    CHECK_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
    CHECK(job.update(world).state == BoundaryCompositionState::Failed);
    CHECK(job.status().captureError == StreamingBoundaryError::InvalidBoundary);
    CHECK_FALSE(job.result(world));
    REQUIRE_FALSE(job.requestWindow(world.regions(), {512, 608, 512, 608}));
    CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);
    CHECK(job.requestWindow(world.regions(), {0, 96, 0, 96}, 1) == BoundaryDiscoveryError::LimitExceeded);
    CHECK(job.update(world).state == BoundaryCompositionState::Failed);
    job.clear(); CHECK(job.update(world).state == BoundaryCompositionState::Idle);
    world.regions().erase({1, 0}); REQUIRE(settle(world));
    {
        WorkerGate blocked(pool);
        REQUIRE_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
        CHECK(job.update(world).state == BoundaryCompositionState::Queued);
        CHECK(job.requestWindow(world.regions(), {0, 96, 0, 96}, 1) == BoundaryDiscoveryError::LimitExceeded);
        CHECK(job.update(world).state == BoundaryCompositionState::Failed);
        CHECK(job.status().discoveryError == BoundaryDiscoveryError::LimitExceeded);
        CHECK(job.inFlight());
    }
    CHECK(job.update(world).state == BoundaryCompositionState::Failed);
    CHECK(job.status().discoveryError == BoundaryDiscoveryError::LimitExceeded);
    CHECK(job.stats().discarded == 1); CHECK_FALSE(job.result(world));
    REQUIRE_FALSE(job.requestWindow(world.regions(), {0, 96, 0, 96}));
    job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::Ready);
}

TEST_CASE("Composed jobs: resident coverage derives the discovery window and fails without coverage") {
    WorldStreamer empty(settings());
    jobs::JobSystem pool(1); BoundaryCompositionJob emptyJob(pool);
    CHECK(emptyJob.requestResidentWindow(empty) == BoundaryDiscoveryError::NoResidentCoverage);
    CHECK(emptyJob.status().state == BoundaryCompositionState::Failed);
    CHECK(emptyJob.status().discoveryError == BoundaryDiscoveryError::NoResidentCoverage);

    WorldStreamer world(settings()); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    const auto footprint = world.residentTerrainFootprint();
    REQUIRE(footprint);
    CHECK(footprint->minX < footprint->maxX);
    CHECK(footprint->minZ < footprint->maxZ);
    BoundaryCompositionJob job(pool);
    CHECK_FALSE(job.requestResidentWindow(world));
    REQUIRE(job.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle();
    REQUIRE(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(job.result(world));
    BoundaryCompositionJob pairJob(pool);
    CHECK_FALSE(pairJob.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    REQUIRE(pairJob.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle(); REQUIRE(pairJob.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(pairJob.result(world));
}

TEST_CASE("Composed jobs: scoped resident window keeps the height/hex corner and rejects a voxel edge") {
    auto config = settings();
    WorldStreamer world(config);
    world.regions().set({0, 0}, PolicyKind::Hex);
    world.regions().set({-1, 0}, PolicyKind::Voxel);
    REQUIRE(settle(world));
    const auto mesh = world.residentTerrainFootprint(PolicyKind::Height, PolicyKind::Hex);
    REQUIRE(mesh);
    const auto scope = std::pair{PolicyKind::Height, PolicyKind::Hex};
    CHECK(discoverTerrainBoundaries(world.regions(), *mesh, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    const auto window = policyPairCompositionWindow(world.regions(), *mesh, PolicyKind::Height, PolicyKind::Hex);
    const auto edges = discoverTerrainBoundaries(world.regions(), window, {}, scope);
    REQUIRE(edges);
    CHECK_FALSE(edges.value().empty());
    const TerrainFootprint containingVoxel{std::min(window.minX, -32.0), std::max(window.maxX, 96.0),
                                           std::min(window.minZ, 0.0), std::max(window.maxZ, 64.0)};
    CHECK(discoverTerrainBoundaries(world.regions(), containingVoxel, {}, scope).error() ==
          BoundaryDiscoveryError::UnsupportedTransition);
    jobs::JobSystem pool(1);
    BoundaryCompositionJob job(pool);
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    CHECK(job.status().state != BoundaryCompositionState::Failed);
    CHECK(job.status().discoveryError == std::nullopt);
}

TEST_CASE("Composed jobs: resident scope retargets across camera residency movement") {
    auto config = settings();
    WorldStreamer world(config); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    jobs::JobSystem pool(1); BoundaryCompositionJob job(pool);
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    REQUIRE(job.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle(); REQUIRE(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(job.result(world));

    REQUIRE(settle(world, {512, 0, 512}));
    CHECK(job.result(world) == nullptr);
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);

    REQUIRE(settle(world));
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    REQUIRE(job.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle(); REQUIRE(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(job.result(world));
}

TEST_CASE("Composed jobs: queued resident scope follows continuous residency retargets") {
    auto config = settings();
    config.workerCount = 1;
    WorldStreamer world(config);
    world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));

    jobs::JobSystem pool(1);
    WorkerGate gate(pool);
    BoundaryCompositionJob job(pool);
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    CHECK(job.update(world).state == BoundaryCompositionState::Queued);

    // Keep the first build queued while the selected residency crosses several
    // camera positions. Each retarget must hide the old target and discard the
    // superseded worker result rather than publishing it after the move.
    for (const math::Vec3 anchor : {math::Vec3{128, 0, 128}, math::Vec3{256, 0, 256},
                                    math::Vec3{512, 0, 512}}) {
        REQUIRE(settle(world, anchor));
        CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
        CHECK(job.update(world).state != BoundaryCompositionState::Failed);
        CHECK(job.result(world) == nullptr);
    }
    gate.release();
    pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);
    CHECK(job.stats().discarded == 1);

    // Return through intermediate residency positions. A valid target may be
    // discovered at an intermediate position, but it must never survive a
    // subsequent retarget as an accepted result for the wrong source set.
    for (const math::Vec3 anchor : {math::Vec3{256, 0, 256}, math::Vec3{128, 0, 128}}) {
        REQUIRE(settle(world, anchor));
        CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
        CHECK(job.update(world).state != BoundaryCompositionState::Failed);
        pool.waitIdle();
        CHECK(job.result(world) == nullptr);
    }
    REQUIRE(settle(world));
    CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
    REQUIRE(job.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle();
    REQUIRE(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(job.result(world));
    CHECK(world.boundaryCurrent(*job.result(world)));
}

TEST_CASE("Composed jobs: moving residency preserves edits through unload and reload") {
    auto config = settings();
    config.horizontalHalfExtentMeters = 96.f;
    WorldStreamer world(config);
    world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));

    jobs::JobSystem pool(1);
    BoundaryCompositionJob job(pool);
    StreamingChunkKey editedKey{};
    constexpr std::uint32_t editedIndex = heightSampleIndex(56, 0);
    const auto editedValue = CellValue::heightValue({51.f, 7, {255, 0, 0, 0}});
    bool edited = false;

    for (const math::Vec3 anchor : {math::Vec3{128, 0, 128}, math::Vec3{256, 0, 256},
                                    math::Vec3{512, 0, 512}}) {
        REQUIRE(settle(world, anchor));
        if (anchor.x == 512.f) {
            for (const auto& chunk : world.residentChunks()) {
                if (chunk.key.policy != PolicyKind::Height || chunk.state != ChunkState::Meshed) continue;
                editedKey = chunk.key;
                REQUIRE(world.edit(editedKey, editedIndex, editedValue));
                edited = true;
                break;
            }
            REQUIRE(edited);
            REQUIRE(settle(world, anchor));
        }
        CHECK_FALSE(job.requestResidentWindow(world, PolicyKind::Height, PolicyKind::Hex));
        CHECK(job.update(world).state != BoundaryCompositionState::Failed);
        pool.waitIdle();
        const auto completed = job.update(world);
        CAPTURE(anchor.x);
        if (anchor.x == 128.f) {
            // This resident envelope includes an incompletely covered edge.
            // It must not be accepted as a partial transition or plain terrain.
            CHECK(completed.state == BoundaryCompositionState::Failed);
            CHECK(completed.buildError == BoundaryReplacementError::IncompleteProfile);
            CHECK(job.result(world) == nullptr);
        } else {
            CHECK(completed.state == BoundaryCompositionState::NoBoundaries);
        }
    }

    REQUIRE(world.mesh(editedKey));
    const auto before = toResourceMesh(*world.mesh(editedKey));
    REQUIRE(settle(world));
    CHECK_FALSE(world.chunk(editedKey).has_value());
    REQUIRE(settle(world, {512, 0, 512}));
    const auto snapshot = world.snapshot(editedKey);
    REQUIRE(snapshot.has_value());
    CHECK(std::any_of(snapshot->deltas.begin(), snapshot->deltas.end(), [&](const auto& delta) {
        return delta.index == editedIndex && delta.value == editedValue;
    }));
    REQUIRE(world.mesh(editedKey));
    const auto after = toResourceMesh(*world.mesh(editedKey));
    REQUIRE(after.streams.size() == before.streams.size());
    CHECK(std::equal(before.indices.begin(), before.indices.end(), after.indices.begin(), after.indices.end()));
    for (std::size_t i = 0; i < before.streams.size(); ++i) {
        const auto& a = before.streams[i].data;
        const auto& b = after.streams[i].data;
        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
    }
}

TEST_CASE("Boundary jobs: stale completions retry and accepted results invalidate immediately") {
    WorldStreamer world(settings()); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    jobs::JobSystem pool(1);
    WorkerGate gate(pool);
    BoundaryProfileJob job(pool);
    job.request(east);
    CHECK(job.update(world).state == BoundaryProfileState::Queued);
    REQUIRE(world.edit({PolicyKind::Height, {1, 0, 0}}, heightSampleIndex(0, 32), CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
    CHECK(job.update(world).state == BoundaryProfileState::Queued);
    CHECK(job.stats().submitted == 1);
    gate.release(); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryProfileState::WaitingSources);
    CHECK(job.stats().discarded == 1); CHECK(job.stats().accepted == 0);
    CHECK(job.result(world) == nullptr);
    REQUIRE(settle(world));
    CHECK(job.update(world).state == BoundaryProfileState::Queued);
    pool.waitIdle(); CHECK(job.update(world).state == BoundaryProfileState::Ready);
    REQUIRE(job.result(world)); CHECK(job.result(world)->complete);
    CHECK(job.stats().submitted == 2); CHECK(job.stats().accepted == 1);
    double maximum = -INFINITY;
    for (const auto& span : job.result(world)->spans) maximum = std::max({maximum, span.positive->beginHeight, span.positive->endHeight});
    CHECK(maximum == 45);
    for (unsigned i = 0; i < 4; ++i) CHECK(job.update(world).state == BoundaryProfileState::Ready);
    CHECK(job.stats().submitted == 2);
    world.regions().set({10, 10}, PolicyKind::Voxel);
    CHECK(job.result(world) == nullptr); // No update is required to hide stale data.
    CHECK(job.update(world).state == BoundaryProfileState::WaitingSources);
    REQUIRE(settle(world)); job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryProfileState::Ready);
    REQUIRE(settle(world, {512, 0, 512})); CHECK(job.result(world) == nullptr);
    job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryProfileState::Incomplete);
    REQUIRE(job.result(world)); CHECK_FALSE(job.result(world)->complete);
    const auto submitted = job.stats().submitted;
    for (unsigned i = 0; i < 4; ++i) CHECK(job.update(world).state == BoundaryProfileState::Incomplete);
    CHECK(job.stats().submitted == submitted);
    REQUIRE(settle(world)); job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryProfileState::Ready);
}

TEST_CASE("Boundary jobs: replacement clear and destruction do not grow the queue or publish old requests") {
    WorldStreamer world(settings()); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    jobs::JobSystem pool(1);
    WorkerGate gate(pool);
    BoundaryProfileJob job(pool);
    job.request(east); job.update(world);
    for (unsigned i = 0; i < 20; ++i) { job.request(i % 2 ? north : east); job.update(world); }
    CHECK(job.stats().submitted == 1);
    CHECK(pool.pendingCount() == 2); // One blocker plus one profile, not 21 profiles.
    gate.release(); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryProfileState::Queued);
    CHECK(job.stats().discarded == 1);
    pool.waitIdle(); CHECK(job.update(world).state == BoundaryProfileState::Ready);
    REQUIRE(job.result(world));
    const auto reference = world.captureBoundary(north);
    REQUIRE(reference.hasValue());
    const auto profile = reference.value().profile(); REQUIRE(profile.hasValue());
    REQUIRE(profile.value().size() == job.result(world)->spans.size());
    for (std::size_t i = 0; i < profile.value().size(); ++i) {
        CHECK(profile.value()[i].begin == job.result(world)->spans[i].begin);
        CHECK(profile.value()[i].negative->endHeight == job.result(world)->spans[i].negative->endHeight);
    }
    job.request(east); job.update(world); job.clear();
    CHECK(job.result(world) == nullptr);
    pool.waitIdle(); CHECK(job.update(world).state == BoundaryProfileState::Idle);
    CHECK_FALSE(job.inFlight()); CHECK(job.stats().discarded == 2);
    {
        auto dyingWorld = std::make_unique<WorldStreamer>(settings());
        dyingWorld->regions().set({0, 0}, PolicyKind::Hex); REQUIRE(settle(*dyingWorld));
        WorkerGate blocker(pool);
        auto dyingJob = std::make_unique<BoundaryProfileJob>(pool);
        dyingJob->request(east); dyingJob->update(*dyingWorld);
        dyingJob.reset(); dyingWorld.reset(); // Worker owns only an immutable ticket.
    }
    CHECK(pool.failedCount() == 0);
}

TEST_CASE("Boundary jobs: failures are explicit and unchanged failed inputs are not requeued") {
    WorldStreamer world(settings()); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    jobs::JobSystem pool(4);
    BoundaryProfileJob limited(pool, {}, 1);
    limited.request(east); limited.update(world); pool.waitIdle();
    auto status = limited.update(world);
    CHECK(status.state == BoundaryProfileState::Failed);
    CHECK(status.profileError == TerrainBoundaryError::SegmentLimitExceeded);
    CHECK(limited.result(world) == nullptr);
    for (unsigned i = 0; i < 4; ++i) CHECK(limited.update(world).state == BoundaryProfileState::Failed);
    CHECK(limited.stats().submitted == 1);
    limited.request(east); limited.update(world); pool.waitIdle(); limited.update(world);
    CHECK(limited.stats().submitted == 2);
    BoundaryProfileJob copyLimited(pool, {0, 0}); copyLimited.request(east);
    CHECK(copyLimited.update(world).captureError == StreamingBoundaryError::LimitExceeded);
    CHECK(copyLimited.stats().submitted == 0);
    BoundaryProfileJob invalid(pool); auto bad = east; bad.line.plane = 63.5; invalid.request(bad);
    CHECK(invalid.update(world).captureError == StreamingBoundaryError::InvalidBoundary);
    pool.shutdown();
    BoundaryProfileJob stopped(pool); stopped.request(east);
    status = stopped.update(world);
    CHECK(status.state == BoundaryProfileState::Failed); CHECK(status.submissionRejected);
    CHECK_FALSE(stopped.inFlight());
    stopped.update(world); CHECK(stopped.stats().failed == 1);
}

TEST_CASE("Composed jobs: stale workers retry and superseded requests stay bounded") {
    auto config = settings(); config.regionalHexCutWalls = true;
    WorldStreamer world(config); world.regions().set({0, 0}, PolicyKind::Hex);
    REQUIRE(settle(world));
    const std::array requests{east, north};
    jobs::JobSystem pool(1); WorkerGate gate(pool);
    BoundaryCompositionJob job(pool); REQUIRE(job.request(requests));
    CHECK(job.update(world).state == BoundaryCompositionState::Queued);
    CHECK_FALSE(job.request({})); CHECK(job.inFlight());
    REQUIRE(world.edit({PolicyKind::Height, {0, 0, 1}}, heightSampleIndex(32, 0), CellValue::heightValue({45, 0, {255, 0, 0, 0}})));
    gate.release(); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::WaitingSources);
    CHECK(job.stats().discarded == 1); CHECK(job.result(world) == nullptr);
    REQUIRE(settle(world));
    CHECK(job.update(world).state == BoundaryCompositionState::Queued);
    pool.waitIdle(); CHECK(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(job.result(world)); CHECK(world.boundaryCurrent(*job.result(world)));
    const auto accepted = *job.result(world);
    for (unsigned i = 0; i < 3; ++i) CHECK(job.update(world).state == BoundaryCompositionState::Ready);
    CHECK(job.stats().submitted == 2);
    {
        WorkerGate blocker(pool);
        for (unsigned i = 0; i < 12; ++i) { REQUIRE(job.request(requests)); job.update(world); }
        CHECK(pool.pendingCount() == 2); CHECK(job.stats().submitted == 3);
        job.clear(); CHECK(job.update(world).state == BoundaryCompositionState::Idle);
        CHECK(job.result(world) == nullptr);
    }
    CHECK(job.update(world).state == BoundaryCompositionState::Idle); CHECK_FALSE(job.inFlight());
    CHECK(job.stats().discarded == 2); CHECK(world.boundaryCurrent(accepted));
    REQUIRE(job.request(requests)); job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::Ready);
    REQUIRE(settle(world, {512, 0, 512})); CHECK(job.result(world) == nullptr);
    job.update(world); pool.waitIdle(); CHECK(job.update(world).state == BoundaryCompositionState::Failed);
    const auto submissions = job.stats().submitted;
    job.update(world); CHECK(job.stats().submitted == submissions);
    REQUIRE(settle(world)); job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::Ready);
}

TEST_CASE("Composed jobs: capture budgets build failures and shutdown are explicit") {
    auto config = settings(); config.regionalHexCutWalls = true;
    WorldStreamer world(config); world.regions().set({0, 0}, PolicyKind::Hex); REQUIRE(settle(world));
    const std::array requests{east, north}; jobs::JobSystem pool(1);
    const auto captured = world.captureBoundary(east); REQUIRE(captured.hasValue());
    CHECK(captured.value().meshBytes() > 0); CHECK(StreamingBoundarySnapshot{}.meshBytes() == 0);
    BoundaryCompositionJob limited(pool, {}, {}, captured.value().meshBytes());
    REQUIRE(limited.request(requests));
    CHECK(limited.update(world).captureError == StreamingBoundaryError::LimitExceeded);
    CHECK(limited.stats().submitted == 0);
    BoundaryCompositionLimits limits; limits.mesh.maxTriangles = 1;
    BoundaryCompositionJob rejected(pool, {}, limits); REQUIRE(rejected.request(requests));
    rejected.update(world); pool.waitIdle();
    CHECK(rejected.update(world).state == BoundaryCompositionState::Failed);
    CHECK(rejected.status().buildError.has_value()); CHECK(rejected.result(world) == nullptr);
    rejected.update(world); CHECK(rejected.stats().submitted == 1);
    REQUIRE(rejected.request(requests)); rejected.update(world); pool.waitIdle(); rejected.update(world);
    CHECK(rejected.stats().submitted == 2);
    {
        WorkerGate gate(pool);
        auto dying = std::make_unique<BoundaryCompositionJob>(pool);
        REQUIRE(dying->request(requests)); dying->update(world); dying.reset();
    }
    CHECK(pool.failedCount() == 0);
    pool.shutdown(); BoundaryCompositionJob stopped(pool); REQUIRE(stopped.request(requests));
    CHECK(stopped.update(world).submissionRejected); CHECK_FALSE(stopped.inFlight());
    stopped.update(world); CHECK(stopped.stats().failed == 1);
}

TEST_CASE("Composed jobs: retirement supersedes queued builds without treating clear as removal") {
    auto config = settings(); config.regionalHexCutWalls = true;
    WorldStreamer world(config); world.regions().set({0, 0}, PolicyKind::Hex); REQUIRE(settle(world));
    const std::array requests{east, north}; jobs::JobSystem pool(1); WorkerGate gate(pool);
    BoundaryCompositionJob job(pool); REQUIRE(job.request(requests)); job.update(world);
    REQUIRE(job.inFlight()); job.requestRetirement();
    CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);
    CHECK(job.result(world) == nullptr); CHECK(job.inFlight()); CHECK(pool.pendingCount() == 2);
    gate.release(); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);
    CHECK(job.stats().discarded == 1); CHECK(job.stats().accepted == 0);
    job.clear(); CHECK(job.update(world).state == BoundaryCompositionState::Idle);
    job.requestRetirement(); CHECK(job.update(world).state == BoundaryCompositionState::NoBoundaries);
    REQUIRE(job.request(requests)); job.update(world); pool.waitIdle();
    CHECK(job.update(world).state == BoundaryCompositionState::Ready); REQUIRE(job.result(world));
    CHECK(job.stats().submitted == 2); CHECK(job.stats().accepted == 1);
}
