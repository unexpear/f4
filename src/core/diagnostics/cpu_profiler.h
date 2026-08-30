#pragma once

#include <cstdint>

#include "core/containers/array.h"

namespace f4::diag {

// Per-zone timing for one profiled frame.
struct ZoneSample {
    const char* name = nullptr;
    std::uint64_t callCount = 0;  // how many times the zone ran this frame
    std::uint64_t totalNanos = 0; // summed duration of all calls
    std::uint64_t maxNanos = 0;   // slowest single call
};

// Frame-time histogram with exponentially-sized buckets from 0.1 ms upward.
// Bucket i spans [0.1 * 2^i, 0.1 * 2^(i+1)) ms; the last bucket also absorbs
// anything larger.
struct FrameHistogram {
    static constexpr std::uint32_t kBins = 40;
    std::uint64_t bins[kBins] = {};
    std::uint64_t count = 0;
    double minMs = 0.0;
    double maxMs = 0.0;
};

// Minimal single-threaded CPU profiler for diagnostics. beginFrame/endFrame
// delimit "this frame"; beginZone/endZone accumulate named, nestable timings
// into a per-frame snapshot, and the overall frame time feeds a histogram.
// Not thread-safe by design (producers call from one thread per frame).
class CpuProfiler {
public:
    static constexpr std::uint32_t kMaxDepth = 64;

    CpuProfiler();

    void beginFrame();
    void endFrame();

    void beginZone(const char* name);
    void endZone();

    // Zones observed in the last completed frame (name-sorted by first use).
    [[nodiscard]] auto zones() const -> const containers::Array<ZoneSample>& {
        return zoneSamples_;
    }

    [[nodiscard]] auto histogram() const -> const FrameHistogram& { return histogram_; }

private:
    struct Zone {
        const char* name = nullptr;
        std::uint64_t total = 0;
        std::uint64_t max = 0;
        std::uint64_t calls = 0;
    };

    auto indexOf(const char* name) const -> std::uint32_t;

    containers::Array<Zone> zones_;
    containers::Array<ZoneSample> zoneSamples_;
    std::uint64_t stack_[kMaxDepth] = {};
    const char* stackName_[kMaxDepth] = {};
    std::uint32_t depth_ = 0;

    long long frameStartTicks_ = 0;
    FrameHistogram histogram_;
};

// RAII: timed over the enclosing scope.
class ScopedZone {
public:
    ScopedZone(CpuProfiler& profiler, const char* name) : profiler_(profiler) {
        profiler_.beginZone(name);
    }
    ~ScopedZone() { profiler_.endZone(); }

    ScopedZone(const ScopedZone&) = delete;
    auto operator=(const ScopedZone&) -> ScopedZone& = delete;

private:
    CpuProfiler& profiler_;
};

} // namespace f4::diag
