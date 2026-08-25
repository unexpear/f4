#pragma once

#include <cstddef>

namespace f4::memory {

constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);

[[nodiscard]] constexpr auto isPowerOfTwo(std::size_t value) -> bool {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr auto alignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr auto isAlignedTo(std::size_t value, std::size_t alignment) -> bool {
    return (value & (alignment - 1)) == 0;
}

// Single seam for all engine heap allocations. Containers and allocators go
// through these wrappers only; replacing the backing (custom allocator,
// tracking layer, console malloc) means touching this file alone.
//
// Alignment must be a power of two. Zero-size requests are invalid
// (asserted); aligned operator new would throw or return a useless block.

auto heapAllocate(std::size_t size, std::size_t alignment = kDefaultAlignment) -> void*;

void heapFree(void* pointer, std::size_t alignment = kDefaultAlignment) noexcept;

} // namespace f4::memory