#include "core/memory/memory.h"

#include <new>

#include "core/assert.h"

namespace f4::memory {

auto heapAllocate(std::size_t size, std::size_t alignment) -> void* {
    F4_ASSERT(size > 0, "zero-size heap allocation");
    F4_ASSERT(isPowerOfTwo(alignment), "alignment must be a power of two");
    return ::operator new(size, std::align_val_t(alignment));
}

void heapFree(void* pointer, std::size_t alignment) noexcept {
    ::operator delete(pointer, std::align_val_t(alignment));
}

} // namespace f4::memory