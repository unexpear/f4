#pragma once

#include <cstddef>

#include "core/assert.h"
#include "core/memory/memory.h"

namespace f4::memory {

// Fixed-chunk pool allocator with an intrusive free list.
//
// Contract:
//  - allocate() returns nullptr once every chunk is outstanding; no growth.
//  - deallocate() returns chunks to the front of the free list (LIFO reuse).
//  - Stride is rounded up to at least sizeof(void*) so freed chunks can hold
//    the next-free link without extra memory.
//  - Double free and foreign-pointer free are UB, exactly like operator
//    delete; sanitizer presets are the safety net. contains() exists for
//    diagnostics and tests.
class Pool {
public:
    Pool() = default;

    Pool(std::size_t elementSize, std::size_t elementAlignment, std::size_t capacityChunks)
        : stride_(alignUp(elementSize < sizeof(void*) ? sizeof(void*) : elementSize,
                         elementAlignment < sizeof(void*) ? sizeof(void*) : elementAlignment)),
          alignment_(elementAlignment),
          capacity_(capacityChunks) {
        F4_ASSERT(elementSize > 0, "pool element size must be non-zero");
        F4_ASSERT(isPowerOfTwo(elementAlignment), "element alignment must be a power of two");
        F4_ASSERT(capacityChunks > 0, "pool needs a non-zero capacity");
        buffer_ = static_cast<std::byte*>(heapAllocate(stride_ * capacity_, alignment_));
        freeHead_ = reinterpret_cast<FreeNode*>(buffer_);
        FreeNode* node = freeHead_;
        for (std::size_t i = 1; i < capacity_; ++i) {
            node->next = reinterpret_cast<FreeNode*>(buffer_ + i * stride_);
            node = node->next;
        }
        node->next = nullptr;
        freeCount_ = capacity_;
    }

    ~Pool() {
        if (buffer_ != nullptr) {
            heapFree(buffer_, alignment_);
        }
    }

    Pool(const Pool&) = delete;
    auto operator=(const Pool&) -> Pool& = delete;

    Pool(Pool&& other) noexcept
        : buffer_(other.buffer_), freeHead_(other.freeHead_), stride_(other.stride_),
          alignment_(other.alignment_), capacity_(other.capacity_), freeCount_(other.freeCount_) {
        other.buffer_ = nullptr;
        other.freeHead_ = nullptr;
        other.capacity_ = 0;
        other.freeCount_ = 0;
    }

    auto operator=(Pool&& other) noexcept -> Pool& {
        if (this != &other) {
            if (buffer_ != nullptr) {
                heapFree(buffer_, alignment_);
            }
            buffer_ = other.buffer_;
            freeHead_ = other.freeHead_;
            stride_ = other.stride_;
            alignment_ = other.alignment_;
            capacity_ = other.capacity_;
            freeCount_ = other.freeCount_;
            other.buffer_ = nullptr;
            other.freeHead_ = nullptr;
            other.capacity_ = 0;
            other.freeCount_ = 0;
        }
        return *this;
    }

    [[nodiscard]] auto allocate() -> void* {
        if (freeHead_ == nullptr) {
            return nullptr;
        }
        FreeNode* node = freeHead_;
        freeHead_ = node->next;
        --freeCount_;
        return node;
    }

    void deallocate(void* pointer) {
        F4_ASSERT(pointer != nullptr, "cannot return null to a pool");
        F4_ASSERT(contains(pointer), "pointer does not belong to this pool");
        FreeNode* node = static_cast<FreeNode*>(pointer);
        node->next = freeHead_;
        freeHead_ = node;
        ++freeCount_;
    }

    [[nodiscard]] auto contains(const void* pointer) const -> bool {
        const auto* p = static_cast<const std::byte*>(pointer);
        if (buffer_ == nullptr || p < buffer_ || p >= buffer_ + stride_ * capacity_) {
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(p - buffer_);
        return offset % stride_ == 0;
    }

    [[nodiscard]] auto strideBytes() const -> std::size_t { return stride_; }
    [[nodiscard]] auto capacityChunks() const -> std::size_t { return capacity_; }
    [[nodiscard]] auto freeChunks() const -> std::size_t { return freeCount_; }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    std::byte* buffer_ = nullptr;
    FreeNode* freeHead_ = nullptr;
    std::size_t stride_ = 0;
    std::size_t alignment_ = kDefaultAlignment;
    std::size_t capacity_ = 0;
    std::size_t freeCount_ = 0;
};

// Typed wrapper adding construct/destroy discipline on top of a byte pool.
template <typename T>
class TypedPool {
public:
    explicit TypedPool(std::size_t capacityChunks) : pool_(sizeof(T), alignof(T), capacityChunks) {}

    template <typename... Args>
    [[nodiscard]] auto acquire(Args&&... args) -> T* {
        void* slot = pool_.allocate();
        if (slot == nullptr) {
            return nullptr;
        }
        return new (slot) T(static_cast<Args&&>(args)...);
    }

    void release(T* object) {
        F4_ASSERT(object != nullptr, "cannot release null from a typed pool");
        F4_ASSERT(pool_.contains(object), "object does not belong to this pool");
        object->~T();
        pool_.deallocate(object);
    }

    [[nodiscard]] auto contains(const T* object) const -> bool { return pool_.contains(object); }
    [[nodiscard]] auto freeChunks() const -> std::size_t { return pool_.freeChunks(); }
    [[nodiscard]] auto capacityChunks() const -> std::size_t { return pool_.capacityChunks(); }

private:
    Pool pool_;
};

} // namespace f4::memory