#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/diagnostics/cpu_profiler.h"
#include "core/diagnostics/metric_registry.h"

namespace f4::diag {

// Formats a single metric as "<name> = <value><unit>" (e.g. "fps = 60.00
// 1/s"). Category is not included; a consumer may prefix it. Shared by the
// console sink and external HUD sinks.
[[nodiscard]] std::string formatMetric(const Metric& metric);

// Invoked each sample with the current registry contents. A consumer (e.g. the
// HUD overlay) installs itself as this sink; the console sink is built in.
using SampleSink = void (*)(const MetricRegistry& metrics, void* userData);

// High-level diagnostics coordinator. Renderer-independent: it owns the metric
// registry and CPU profiler and emits samples to (a) a built-in console sink
// (via the log) and (b) an optional external sink (the HUD overlay). Producers
// register named metrics with metrics() and push values; the renderer never
// owns the registry - it only supplies metrics.
//
// Sampling period is independent of the render rate: update() may be called
// every frame, but console/HUD output only refreshes when a sample interval
// elapses.
class Diagnostics {
public:
    Diagnostics();

    void setHudEnabled(bool enabled);
    [[nodiscard]] bool hudEnabled() const { return hudEnabled_; }
    void setConsoleEnabled(bool enabled);
    [[nodiscard]] bool consoleEnabled() const { return consoleEnabled_; }

    // Minimum wall-clock time between samples (seconds). Independent of the
    // frame rate at which update() is called.
    void setSamplingInterval(double seconds);
    [[nodiscard]] double samplingInterval() const { return samplingInterval_; }

    // Who is notified with each sample's metrics (e.g. the HUD overlay).
    void setSampleSink(SampleSink sink, void* userData);

    // Advance diagnostics for the current frame. Call with the current time in
    // seconds (any monotonic clock). Emits console output and notifies the
    // sample sink when a sampling period has elapsed.
    void update(double nowSeconds);

    [[nodiscard]] auto metrics() -> MetricRegistry& { return metrics_; }
    [[nodiscard]] auto profiler() -> CpuProfiler& { return profiler_; }

private:
    void emitSample();

    MetricRegistry metrics_;
    CpuProfiler profiler_;
    SampleSink sampleSink_ = nullptr;
    void* sampleUser_ = nullptr;
    double samplingInterval_ = 0.25;
    double lastSampleTime_ = -1.0;
    bool hudEnabled_ = true;
    bool consoleEnabled_ = true;
};

} // namespace f4::diag
