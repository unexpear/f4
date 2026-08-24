#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/logging/logger.h"

namespace f4test {

inline auto lines() -> std::vector<std::string>& {
    static std::vector<std::string> captured;
    return captured;
}

inline void sink(f4::log::Level, std::string_view line) {
    lines().emplace_back(line);
}

inline void beginCapture() {
    lines().clear();
    f4::log::setSink(sink);
}

inline void endCapture() {
    f4::log::setSink(nullptr);
    lines().clear();
}

inline auto linesContaining(std::string_view needle) -> std::size_t {
    std::size_t count = 0;
    for (const auto& line : lines()) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

} // namespace f4test
