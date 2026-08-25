#pragma once

#include <cstdint>

namespace f4::resources {

// Shared error codes for all image loaders in the M5.x ladder.
enum class LoadError : std::uint8_t {
    None,
    Truncated,
    BadSignature,
    UnsupportedCompression,
    UnsupportedBpp,
    BadDimensions,
};

} // namespace f4::resources
