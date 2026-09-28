#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

#include "engine/world/world_streaming.h"

namespace {
using namespace f4;
using namespace f4::world;

struct Samples {
    std::array<std::vector<double>, 8> stages;
    std::size_t overBudget = 0;
    void measure(WorldStreamer& streamer, math::Vec3 camera, std::chrono::microseconds budget) {
        const auto start = std::chrono::steady_clock::now();
        const auto frame = streamer.update(camera);
        const auto callTime = std::chrono::steady_clock::now() - start;
        const auto& t = frame.timings;
        const std::array<double, 8> values{
            static_cast<double>(frame.elapsed.count()),
            std::chrono::duration<double, std::micro>(t.planning).count(),
            std::chrono::duration<double, std::micro>(t.residency).count(),
            std::chrono::duration<double, std::micro>(t.unload).count(),
            std::chrono::duration<double, std::micro>(t.load).count(),
            std::chrono::duration<double, std::micro>(t.publication).count(),
            std::chrono::duration<double, std::micro>(t.mesh).count(),
            std::chrono::duration<double, std::micro>(callTime).count()};
        for (std::size_t i = 0; i < values.size(); ++i) stages[i].push_back(values[i]);
        if (callTime > budget) ++overBudget;
    }
    void report(const char* scenario, const char* phase) {
        constexpr const char* names[]{"total", "planning", "residency", "unload", "load", "publication", "mesh", "call"};
        for (std::size_t i = 0; i < stages.size(); ++i) {
            auto& values = stages[i];
            if (values.empty()) continue;
            std::sort(values.begin(), values.end());
            const std::size_t p95 = (values.size() * 95 + 99) / 100 - 1;
            std::printf("%s,%s,%s,%zu,%.3f,%.3f,%zu\n", scenario, phase, names[i],
                        values.size(), values[p95], values.back(), overBudget);
        }
    }
};

auto settle(WorldStreamer& streamer, math::Vec3 camera, std::size_t expected,
            Samples& samples, std::chrono::microseconds budget) -> bool {
    for (unsigned frame = 0; frame < 512; ++frame) {
        samples.measure(streamer, camera, budget);
        // Deliberately burst worker results between measured updates. This
        // profiles CPU publication/snapshot cost, not end-to-end frame pacing.
        streamer.waitForIdle();
        if (streamer.residentCount() != expected || streamer.hasPendingWork()) continue;
        bool complete = true;
        for (const auto& chunk : streamer.residentChunks()) complete &= chunk.hasMesh;
        if (complete) return true;
    }
    return false;
}

auto run(const char* name, PolicyKind policy, unsigned radius, std::size_t capacity,
         unsigned regions) -> bool {
    WorldStreamingConfig config;
    config.defaultPolicy = policy;
    config.horizontalRadius = radius;
    config.voxelVerticalRadius = 0;
    config.chunkCapacityPerPolicy = capacity;
    config.workerCount = 4;
    config.policyParams.hex.radius = 2;
    config.profileTimings = true;
    WorldStreamer streamer(config);
    for (unsigned i = 0; i < regions; ++i) {
        streamer.regions().set({static_cast<std::int32_t>(i + 100), 100}, PolicyKind::Voxel);
    }
    const std::size_t side = 2 * static_cast<std::size_t>(radius) + 1;
    const std::size_t expected = std::min(side * side, capacity);
    const auto budget = config.frameBudget;
    Samples cold, idle, move;
    if (!settle(streamer, {}, expected, cold, budget)) return false;
    for (unsigned i = 0; i < 120; ++i) idle.measure(streamer, {}, budget);
    if (!settle(streamer, {1024.f, 0.f, 1024.f}, expected, move, budget)) return false;
    cold.report(name, "cold");
    idle.report(name, "idle");
    move.report(name, "move");
    if (regions != 0) {
        // Hold a snapshot explicitly so the mutation measures detachment even
        // if an asynchronous planner would have already finished reading it.
        const RegionMap snapshot(streamer.regions());
        const auto start = std::chrono::steady_clock::now();
        streamer.regions().set({100, 100}, PolicyKind::Hex);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double micros = std::chrono::duration<double, std::micro>(elapsed).count();
        std::printf("%s,edit,region_write,1,%.3f,%.3f,%u\n", name, micros, micros,
                    elapsed > budget ? 1u : 0u);
        const auto coordinate = 100 * config.regionExtentMeters;
        if (snapshot.policyAt(coordinate, coordinate) != PolicyKind::Voxel ||
            streamer.regions().policyAt(coordinate, coordinate) != PolicyKind::Hex) return false;
        Samples edited;
        if (!settle(streamer, {1024.f, 0.f, 1024.f}, expected, edited, budget)) return false;
        edited.report(name, "edit");
    }
    const auto stats = streamer.stats();
    std::fprintf(stderr, "%s: resident=%zu mesh_jobs=%llu neighbor_queries=%llu failed=%llu\n",
                 name, streamer.residentCount(),
                 static_cast<unsigned long long>(stats.submittedMeshJobs),
                 static_cast<unsigned long long>(stats.neighborQueries),
                 static_cast<unsigned long long>(stats.failedCompletions));
    return stats.failedCompletions == 0;
}
} // namespace

int main() {
    std::puts("scenario,phase,stage,updates,p95_us,max_us,updates_over_2000us");
    if (!run("height25", PolicyKind::Height, 2, 64, 0) ||
        !run("height128", PolicyKind::Height, 128, 128, 0) ||
        !run("voxel25", PolicyKind::Voxel, 2, 64, 0) ||
        !run("hex64", PolicyKind::Hex, 16, 64, 0) ||
        !run("regions50000", PolicyKind::Height, 128, 16, 50000)) {
        std::fputs("Streaming benchmark failed to converge or reported failed work\n", stderr);
        return 1;
    }
    return 0;
}
