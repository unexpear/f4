#include "core/diagnostics/cpu_profiler.h"

#include <chrono>
#include <cmath>

namespace f4::diag {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] auto nowNanos() -> long long {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] auto histogramBin(double frameMs) -> std::uint32_t {
    if (frameMs <= 0.0) {
        return 0;
    }
    // Bucket i spans [0.1 * 2^i, 0.1 * 2^(i+1)) ms.
    const double ratio = frameMs / 0.1;
    const double lg = std::log2(ratio);
    std::uint32_t bin = lg <= 0.0 ? 0 : static_cast<std::uint32_t>(lg);
    if (bin >= FrameHistogram::kBins) {
        bin = FrameHistogram::kBins - 1;
    }
    return bin;
}

} // namespace

CpuProfiler::CpuProfiler() : zones_(8), zoneSamples_(16) {}

void CpuProfiler::beginFrame() {
    frameStartTicks_ = nowNanos();
    zones_.clear();
    depth_ = 0;
}

void CpuProfiler::endFrame() {
    const long long end = nowNanos();
    const double frameMs = static_cast<double>(end - frameStartTicks_) * 1e-6;
    FrameHistogram& hist = histogram_;
    ++hist.bins[histogramBin(frameMs)];
    ++hist.count;
    if (hist.count == 1) {
        hist.minMs = frameMs;
        hist.maxMs = frameMs;
    } else {
        if (frameMs < hist.minMs) {
            hist.minMs = frameMs;
        }
        if (frameMs > hist.maxMs) {
            hist.maxMs = frameMs;
        }
    }

    zoneSamples_.clear();
    zoneSamples_.reserve(zones_.size());
    for (const Zone& z : zones_) {
        zoneSamples_.push(ZoneSample{z.name, z.calls, z.total, z.max});
    }
}

void CpuProfiler::beginZone(const char* name) {
    if (depth_ >= kMaxDepth) {
        return;
    }
    stackName_[depth_] = name;
    stack_[depth_] = nowNanos();
    ++depth_;
}

void CpuProfiler::endZone() {
    if (depth_ == 0) {
        return;
    }
    --depth_;
    const std::uint64_t elapsed = static_cast<std::uint64_t>(nowNanos() - stack_[depth_]);
    const char* name = stackName_[depth_];
    std::uint32_t index = indexOf(name);
    if (index == zones_.size()) {
        zones_.push(Zone{name, 0, 0, 0});
        index = static_cast<std::uint32_t>(zones_.size() - 1);
    }
    Zone& z = zones_[index];
    z.total += elapsed;
    z.calls += 1;
    if (elapsed > z.max) {
        z.max = elapsed;
    }
}

auto CpuProfiler::indexOf(const char* name) const -> std::uint32_t {
    std::uint32_t index = 0;
    for (const Zone& z : zones_) {
        if (z.name == name) {
            return index;
        }
        ++index;
    }
    return index; // == zones_.size() if not found
}

} // namespace f4::diag
