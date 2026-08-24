#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

#include "core/logging/logger.h"

#ifndef F4_ENABLE_ASSERTS
#ifdef NDEBUG
#define F4_ENABLE_ASSERTS 0
#else
#define F4_ENABLE_ASSERTS 1
#endif
#endif

namespace f4 {

[[noreturn]] inline void assertionFailed(std::string_view expression, std::string_view message,
                                         std::string_view file, int line) {
    std::string text;
    text.reserve(expression.size() + message.size() + 24);
    text += "Assertion failed: ";
    text += expression;
    if (!message.empty()) {
        text += " - ";
        text += message;
    }
    text += " (";
    text += file;
    text += ':';
    text += std::to_string(line);
    text += ')';
    log::write(log::Level::Error, text);
    std::abort();
}

} // namespace f4

#if F4_ENABLE_ASSERTS
#define F4_ASSERT(condition, message)                                          \
    (static_cast<bool>(condition)                                              \
         ? static_cast<void>(0)                                                \
         : ::f4::assertionFailed(#condition, message, __FILE__, __LINE__))
#else
#define F4_ASSERT(condition, message) static_cast<void>(sizeof(!(condition)))
#endif

#define F4_VERIFY(condition, message)                                          \
    (static_cast<bool>(condition)                                              \
         ? static_cast<void>(0)                                                \
         : ::f4::assertionFailed(#condition, message, __FILE__, __LINE__))
