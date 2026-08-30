#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/containers/string.h"

namespace f4::diag {

using containers::Array;
using containers::String;
using containers::StringView;

// A metric carries one of a small, fixed set of value types. Consumers (console
// sink, HUD sink) switch on type() to render.
enum class MetricType : std::uint8_t {
    Int,
    UInt,
    Double,
    String,
};

// Declaration of a single metric. `name` is the stable identity used by declare
// for dedup and by sinks for display. `category` groups metrics (e.g. "gpu",
// "memory", "telemetry") and `unit` is a display suffix ("ms", "MB", "count").
// Callers keep the referenced string literals alive for the registry's lifetime
// (static storage is the norm).
struct MetricDecl {
    const char* name = nullptr;
    MetricType type = MetricType::Int;
    const char* category = nullptr;
    const char* unit = nullptr;
};

// One declared metric with its current value. Owns storage only for String
// metrics; numeric values live in a union. Movable (Array stores by move).
class Metric {
public:
    Metric() = default;

    explicit Metric(MetricDecl decl);
    Metric(MetricDecl decl, std::int64_t value);
    Metric(MetricDecl decl, std::uint64_t value);
    Metric(MetricDecl decl, double value);
    Metric(MetricDecl decl, StringView value);

    Metric(const Metric&) = delete;
    auto operator=(const Metric&) -> Metric& = delete;
    Metric(Metric&& other) noexcept;
    auto operator=(Metric&& other) noexcept -> Metric&;
    ~Metric();

    [[nodiscard]] auto decl() const -> const MetricDecl& { return decl_; }
    [[nodiscard]] auto type() const -> MetricType { return decl_.type; }

    [[nodiscard]] auto asInt() const -> std::int64_t;
    [[nodiscard]] auto asUInt() const -> std::uint64_t;
    [[nodiscard]] auto asDouble() const -> double;
    [[nodiscard]] auto asString() const -> StringView;

    void set(std::int64_t value);
    void set(std::uint64_t value);
    void set(double value);
    void set(StringView value);

private:
    void release();
    void assignString(StringView value);

    MetricDecl decl_{};
    containers::String str_; // storage only for String metrics
    union {
        std::int64_t i = 0;
        std::uint64_t u;
        double d;
    } value_;
};

// Generic, type-agnostic metric store. Nothing here knows about FPS, draw
// calls, or any renderer-specific concept - producers declare named metrics and
// push values; sinks enumerate. This keeps Diagnostics renderer-independent and
// lets demos define whatever they wish to observe.
class MetricRegistry {
public:
    // Declares a metric (dedup by name: returns the existing id) and returns
    // its stable id for later set() calls.
    auto declare(MetricDecl decl) -> std::uint32_t;

    [[nodiscard]] auto operator[](std::uint32_t id) -> Metric&;
    [[nodiscard]] auto operator[](std::uint32_t id) const -> const Metric&;
    [[nodiscard]] auto find(const char* name) const -> const Metric*;

    [[nodiscard]] auto count() const -> std::size_t { return metrics_.size(); }
    [[nodiscard]] auto begin() -> Metric* { return metrics_.data(); }
    [[nodiscard]] auto end() -> Metric* { return metrics_.data() + metrics_.size(); }
    [[nodiscard]] auto begin() const -> const Metric* { return metrics_.data(); }
    [[nodiscard]] auto end() const -> const Metric* {
        return metrics_.data() + metrics_.size();
    }

    void clear();

private:
    containers::Array<Metric> metrics_;
};

} // namespace f4::diag
