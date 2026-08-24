#include "core/logging/logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace f4::log {
namespace {

std::atomic<int> gLevel{static_cast<int>(Level::Info)};
Sink gSink = nullptr;

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};

auto basenameOf(std::string_view path) -> std::string_view {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

auto timestamp() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return std::string(buffer);
}

void defaultSink(Level, std::string_view line) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
}

} // namespace

auto currentLevel() -> Level {
    return static_cast<Level>(gLevel.load(std::memory_order_relaxed));
}

void setLevel(Level level) {
    gLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

void setSink(Sink sink) {
    gSink = sink;
}

void write(Level level, std::string_view line) {
    if (gSink != nullptr) {
        gSink(level, line);
    } else {
        defaultSink(level, line);
    }
}

Record::Record(Level level, std::string_view file, int line)
    : active_(static_cast<int>(level) >= gLevel.load(std::memory_order_relaxed)),
      level_(level),
      file_(file),
      line_(line) {}

Record::~Record() {
    if (!active_) {
        return;
    }
    std::ostringstream out;
    out << '[' << timestamp() << "] [" << kLevelNames[static_cast<int>(level_)] << "] "
        << stream_.str() << " (" << basenameOf(file_) << ':' << line_ << ')';
    write(level_, out.str());
}

} // namespace f4::log
