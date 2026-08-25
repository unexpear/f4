#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace f4::ids {

constexpr std::uint32_t kInvalidHandleIndex = 0xFFFFFFFFu;

// Tag-typed opaque handle: {index, generation}. The tag type makes distinct
// handle families mutually incompatible at compile time (BufferHandle is not
// a TextureHandle) even though every family shares this layout.
//
// generation starts at 0 and increments on slot reuse, so a stale copy of an
// old handle can never alias a newer object (ABA-safe). Null state is
// index == kInvalidHandleIndex.
template <typename Tag>
struct Handle {
    std::uint32_t index = kInvalidHandleIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr auto isValid() const -> bool { return index != kInvalidHandleIndex; }

    friend constexpr auto operator==(const Handle& a, const Handle& b) -> bool {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr auto operator!=(const Handle& a, const Handle& b) -> bool { return !(a == b); }
};

} // namespace f4::ids

template <typename Tag>
struct std::hash<f4::ids::Handle<Tag>> {
    auto operator()(const f4::ids::Handle<Tag>& h) const noexcept -> std::size_t {
        const std::uint64_t combined =
            (static_cast<std::uint64_t>(h.generation) << 32) | static_cast<std::uint64_t>(h.index);
        return std::hash<std::uint64_t>{}(combined);
    }
};