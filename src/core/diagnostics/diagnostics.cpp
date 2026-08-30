#include "core/diagnostics/diagnostics.h"

#include <sstream>
#include <string_view>

#include "core/logging/logger.h"

namespace f4::diag {

std::string formatMetric(const Metric& metric) {
    std::ostringstream buffer;
    buffer << (metric.decl().name != nullptr ? metric.decl().name : "?") << " = ";
    switch (metric.type()) {
    case MetricType::Int: buffer << metric.asInt(); break;
    case MetricType::UInt: buffer << metric.asUInt(); break;
    case MetricType::Double:
        buffer.setf(std::ios::fixed);
        buffer.precision(2);
        buffer << metric.asDouble();
        break;
    case MetricType::String:
        buffer << std::string_view(metric.asString().data(), metric.asString().size());
        break;
    }
    if (metric.decl().unit != nullptr) {
        buffer << ' ' << metric.decl().unit;
    }
    return buffer.str();
}

Diagnostics::Diagnostics() = default;

void Diagnostics::setHudEnabled(bool enabled) {
    hudEnabled_ = enabled;
}

void Diagnostics::setConsoleEnabled(bool enabled) {
    consoleEnabled_ = enabled;
}

void Diagnostics::setSamplingInterval(double seconds) {
    samplingInterval_ = seconds > 0.0 ? seconds : 0.0;
}

void Diagnostics::setSampleSink(SampleSink sink, void* userData) {
    sampleSink_ = sink;
    sampleUser_ = userData;
}

void Diagnostics::update(double nowSeconds) {
    const double elapsed = lastSampleTime_ < 0.0 ? 0.0 : nowSeconds - lastSampleTime_;
    const bool due = lastSampleTime_ < 0.0 || elapsed >= samplingInterval_;
    if (!due) {
        return;
    }
    lastSampleTime_ = nowSeconds;
    emitSample();
    if (hudEnabled_ && sampleSink_ != nullptr) {
        sampleSink_(metrics_, sampleUser_);
    }
}

void Diagnostics::emitSample() {
    if (!consoleEnabled_) {
        return;
    }
    std::ostringstream buffer;
    for (const Metric& m : metrics_) {
        buffer.str("");
        buffer.clear();
        if (m.decl().category != nullptr) {
            buffer << '[' << m.decl().category << "] ";
        }
        buffer << formatMetric(m);
        log::write(log::Level::Info, buffer.str());
    }
}

} // namespace f4::diag
