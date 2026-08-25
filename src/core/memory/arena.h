#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/assert.h"
#include "core/memory/memory.h"

namespace f4::memory {

// Linear (bump) allocator over one contiguous buffer.
//
// Contract:
//  - allocate() returns nullptr on exhaustion; callers decide policy. Sizing
//    bugs surface in tests instead of hiding behind growth.
//  - reset() rewinds the bump pointer WITHOUT running destructors. Types
//    placed via create<T>() must not own resources that outlive the arena,
//    or must be torn down explicitly before reset.
//  - Zero-size requests return nullptr.
//
// Move-only; the buffer is freed exactly once.
class Arena {
public:
    Arena() = default;

    explicit Arena(std::size_t capacityBytes, std::size_t blockAlignment = kDefaultAlignment)
        : capacity_(capacityBytes), alignment_(blockAlignment) {
        F4_ASSERT(capacityBytes > 0, "arena needs a non-zero capacity");
        F4_ASSERT(isPowerOfTwo(blockAlignment), "block alignment must be a power of two");
        buffer_ = static_cast<std::byte*>(heapAllocate(capacity_, alignment_));
    }

    ~Arena() {
        if (buffer_ != nullptr) {
            heapFree(buffer_, alignment_);
        }
    }

    Arena(const Arena&) = delete;
    auto operator=(const Arena&) -> Arena& = delete;

    Arena(Arena&& other) noexcept
        : buffer_(other.buffer_), capacity_(other.capacity_), offset_(other.offset_), alignment_(other.alignment_) {
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.offset_ = 0;
        other.alignment_ = kDefaultAlignment;
    }

    auto operator=(Arena&& other) noexcept -> Arena& {
        if (this != &other) {
            if (buffer_ != nullptr) {
                heapFree(buffer_, alignment_);
            }
            buffer_ = other.buffer_;
            capacity_ = other.capacity_;
            offset_ = other.offset_;
            alignment_ = other.alignment_;
            other.buffer_ = nullptr;
            other.capacity_ = 0;
            other.offset_ = 0;
            other.alignment_ = kDefaultAlignment;
        }
        return *this;
    }

    [[nodiscard]] auto allocate(std::size_t size, std::size_t alignment = kDefaultAlignment) -> void* {
        if (buffer_ == nullptr || size == 0) {
            return nullptr;
        }
        F4_ASSERT(isPowerOfTwo(alignment), "alignment must be a power of two");
        // Align the RESULTING ADDRESS, not just the offset: the buffer's own
        // base alignment can be lower than a requested over-alignment.
        std::size_t candidate = offset_;
        const std::size_t addressMod =
            (reinterpret_cast<std::uintptr_t>(buffer_) + candidate) & (alignment - 1);
        if (addressMod != 0) {
            candidate += alignment - addressMod;
        }
        if (candidate > capacity_ || size > capacity_ - candidate) {
            return nullptr;
        }
        offset_ = candidate + size;
        return buffer_ + candidate;
    }

    // Placement-constructs T inside the arena. Destructor is NOT registered
    // anywhere: see the contract above.
    template <typename T, typename... Args>
    [[nodiscard]] auto create(Args&&... args) -> T* {
        void* slot = allocate(sizeof(T), alignof(T));
        if (slot == nullptr) {
            return nullptr;
        }
        return new (slot) T(std::forward<Args>(args)...);
    }

    void reset() { offset_ = 0; }

    [[nodiscard]] auto usedBytes() const -> std::size_t { return offset_; }
    [[nodiscard]] auto capacityBytes() const -> std::size_t { return capacity_; }
    [[nodiscard]] auto remainingBytes() const -> std::size_t { return capacity_ - offset_; }

private:
    std::byte* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t offset_ = 0;
    std::size_t alignment_ = kDefaultAlignment;
};

} // namespace f4::memory