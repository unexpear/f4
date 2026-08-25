#pragma once

#include <cstdint>
#include <functional>

#include "core/containers/string.h"

namespace f4::containers {

// FNV-1a 64-bit. One shared implementation guarantees that a String and a
// StringView over equal contents always produce equal hashes - so
// HashMap<StringView> lookups can interoperate with String keys.
[[nodiscard]] constexpr auto fnv1a64(const char* data, std::size_t size) -> std::uint64_t {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffsetBasis;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint8_t>(data[i]);
        hash *= kPrime;
    }
    return hash;
}

} // namespace f4::containers

template <>
struct std::hash<f4::containers::StringView> {
    auto operator()(const f4::containers::StringView& view) const noexcept -> std::size_t {
        return static_cast<std::size_t>(f4::containers::fnv1a64(view.data(), view.size()));
    }
};

template <>
struct std::hash<f4::containers::String> {
    auto operator()(const f4::containers::String& s) const noexcept -> std::size_t {
        return static_cast<std::size_t>(f4::containers::fnv1a64(s.c_str(), s.size()));
    }
};