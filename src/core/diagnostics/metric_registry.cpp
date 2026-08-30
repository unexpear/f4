#include "core/diagnostics/metric_registry.h"

#include <cstring>

namespace f4::diag {

Metric::Metric(MetricDecl decl) : decl_(decl) {
    switch (decl_.type) {
    case MetricType::Int: value_.i = 0; break;
    case MetricType::UInt: value_.u = 0; break;
    case MetricType::Double: value_.d = 0.0; break;
    case MetricType::String: break;
    }
}

Metric::Metric(MetricDecl decl, std::int64_t value) : Metric(decl) { set(value); }
Metric::Metric(MetricDecl decl, std::uint64_t value) : Metric(decl) { set(value); }
Metric::Metric(MetricDecl decl, double value) : Metric(decl) { set(value); }
Metric::Metric(MetricDecl decl, StringView value) : Metric(decl) { set(value); }

Metric::Metric(Metric&& other) noexcept
    : decl_(other.decl_), str_(static_cast<containers::String&&>(other.str_)), value_(other.value_) {}

auto Metric::operator=(Metric&& other) noexcept -> Metric& {
    if (this != &other) {
        release();
        decl_ = other.decl_;
        str_ = static_cast<containers::String&&>(other.str_);
        value_ = other.value_;
    }
    return *this;
}

Metric::~Metric() { release(); }

void Metric::release() {
    if (decl_.type == MetricType::String && !str_.isEmpty()) {
        str_ = containers::String(); // frees the owned buffer
    }
}

void Metric::assignString(StringView value) {
    if (decl_.type == MetricType::String) {
        str_ = containers::String(value);
    }
}

auto Metric::asInt() const -> std::int64_t {
    return decl_.type == MetricType::Int ? value_.i : 0;
}

auto Metric::asUInt() const -> std::uint64_t {
    return decl_.type == MetricType::UInt ? value_.u : 0;
}

auto Metric::asDouble() const -> double {
    return decl_.type == MetricType::Double ? value_.d : 0.0;
}

auto Metric::asString() const -> StringView {
    return decl_.type == MetricType::String ? str_.view() : StringView();
}

void Metric::set(std::int64_t value) {
    if (decl_.type == MetricType::Int) {
        value_.i = value;
    }
}

void Metric::set(std::uint64_t value) {
    if (decl_.type == MetricType::UInt) {
        value_.u = value;
    }
}

void Metric::set(double value) {
    if (decl_.type == MetricType::Double) {
        value_.d = value;
    }
}

void Metric::set(StringView value) { assignString(value); }

auto MetricRegistry::declare(MetricDecl decl) -> std::uint32_t {
    if (decl.name != nullptr) {
        if (const Metric* existing = find(decl.name)) {
            return static_cast<std::uint32_t>(existing - begin());
        }
    }
    metrics_.emplaceBack(Metric(decl));
    return static_cast<std::uint32_t>(metrics_.size() - 1);
}

auto MetricRegistry::operator[](std::uint32_t id) -> Metric& {
    return metrics_[id];
}

auto MetricRegistry::operator[](std::uint32_t id) const -> const Metric& {
    return metrics_[id];
}

auto MetricRegistry::find(const char* name) const -> const Metric* {
    const StringView target(name);
    for (const Metric& m : metrics_) {
        const MetricDecl& d = m.decl();
        if (d.name != nullptr && StringView(d.name) == target) {
            return &m;
        }
    }
    return nullptr;
}

void MetricRegistry::clear() {
    metrics_.clear();
}

} // namespace f4::diag
