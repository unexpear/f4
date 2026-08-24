#pragma once

#include <cstddef>
#include <sstream>
#include <string_view>

namespace f4::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

auto currentLevel() -> Level;
void setLevel(Level level);

using Sink = void (*)(Level level, std::string_view line);

void setSink(Sink sink);
void write(Level level, std::string_view line);

class Record {
public:
    Record(Level level, std::string_view file, int line);
    ~Record();

    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    template <typename T>
    auto operator<<(const T& value) -> Record& {
        if (active_) {
            stream_ << value;
        }
        return *this;
    }

private:
    bool active_;
    Level level_;
    std::string_view file_;
    int line_;
    std::ostringstream stream_;
};

class NullRecord {
public:
    template <typename T>
    auto operator<<(const T&) -> NullRecord& {
        return *this;
    }
};

} // namespace f4::log

#ifndef F4_MIN_LOG_LEVEL
#define F4_MIN_LOG_LEVEL 0
#endif

#define F4_LOG(level) ::f4::log::Record(::f4::log::Level::level, __FILE__, __LINE__)

#if F4_MIN_LOG_LEVEL <= 0
#define F4_LOG_TRACE() F4_LOG(Trace)
#else
#define F4_LOG_TRACE() ::f4::log::NullRecord()
#endif

#if F4_MIN_LOG_LEVEL <= 1
#define F4_LOG_DEBUG() F4_LOG(Debug)
#else
#define F4_LOG_DEBUG() ::f4::log::NullRecord()
#endif

#define F4_LOG_INFO() F4_LOG(Info)
#define F4_LOG_WARNING() F4_LOG(Warning)
#define F4_LOG_ERROR() F4_LOG(Error)
