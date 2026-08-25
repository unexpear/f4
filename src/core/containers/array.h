#pragma once

#include <cstddef>
#include <utility>

#include "core/assert.h"
#include "core/memory/memory.h"

namespace f4::containers {

// Dynamic contiguous array over the f4::memory heap seam.
//
// Contract:
//  - Growth doubles capacity starting at 4. Out-of-memory surfaces as the
//    underlying aligned operator new's std::bad_alloc (deliberate: unlike
//    arena/pool exhaustion this is a system-level failure).
//  - Basic exception safety: failed growth leaves the array unchanged.
//  - removeSwap(index) is O(1) and scrambles order - the engine-default way
//    to erase from unordered arrays.
//  - Works with move-only T; copying an Array copies its elements.
template <typename T>
class Array {
public:
    Array() = default;

    explicit Array(std::size_t initialCapacity) {
        if (initialCapacity > 0) {
            growTo(initialCapacity);
        }
    }

    ~Array() {
        destroyElements();
        deallocate();
    }

    Array(const Array& other) : capacity_(other.size_), size_(other.size_) {
        if (capacity_ > 0) {
            data_ = allocate(capacity_);
            constructRange(other.data_, size_);
        }
    }

    auto operator=(const Array& other) -> Array& {
        if (this != &other) {
            Array tmp(other);
            swapWith(tmp);
        }
        return *this;
    }

    Array(Array&& other) noexcept
        : data_(other.data_), capacity_(other.capacity_), size_(other.size_) {
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
    }

    auto operator=(Array&& other) noexcept -> Array& {
        if (this != &other) {
            destroyElements();
            deallocate();
            data_ = other.data_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
        }
        return *this;
    }

    template <typename... Args>
    auto emplaceBack(Args&&... args) -> T& {
        if (size_ == capacity_) {
            growTo(capacity_ == 0 ? 4 : capacity_ * 2);
        }
        T* slot = data_ + size_;
        new (slot) T(static_cast<Args&&>(args)...);
        ++size_;
        return *slot;
    }

    auto push(const T& value) -> T& { return emplaceBack(value); }
    auto push(T&& value) -> T& { return emplaceBack(static_cast<T&&>(value)); }

    void pop() {
        F4_ASSERT(size_ > 0, "pop on empty array");
        --size_;
        data_[size_].~T();
    }

    void clear() {
        destroyElements();
        size_ = 0;
    }

    void resize(std::size_t newSize) {
        if (newSize > size_) {
            reserve(newSize);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
            for (std::size_t i = size_; i < newSize; ++i) {
                new (data_ + i) T();
            }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        } else {
            for (std::size_t i = newSize; i < size_; ++i) {
                data_[i].~T();
            }
        }
        size_ = newSize;
    }

    void reserve(std::size_t minCapacity) {
        if (minCapacity > capacity_) {
            growTo(minCapacity);
        }
    }

    void removeSwap(std::size_t index) {
        F4_ASSERT(index < size_, "removeSwap index out of range");
        if (index != size_ - 1) {
            data_[index] = static_cast<T&&>(data_[size_ - 1]);
        }
        --size_;
        data_[size_].~T();
    }

    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }
    [[nodiscard]] auto isEmpty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto data() -> T* { return data_; }
    [[nodiscard]] auto data() const -> const T* { return data_; }

    [[nodiscard]] auto operator[](std::size_t index) -> T& {
        F4_ASSERT(index < size_, "array index out of range");
        return data_[index];
    }

    [[nodiscard]] auto operator[](std::size_t index) const -> const T& {
        F4_ASSERT(index < size_, "array index out of range");
        return data_[index];
    }

    [[nodiscard]] auto front() -> T& { return (*this)[0]; }
    [[nodiscard]] auto back() -> T& { return (*this)[size_ - 1]; }

    [[nodiscard]] auto begin() -> T* { return data_; }
    [[nodiscard]] auto end() -> T* { return data_ + size_; }
    [[nodiscard]] auto begin() const -> const T* { return data_; }
    [[nodiscard]] auto end() const -> const T* { return data_ + size_; }

private:
    [[nodiscard]] static auto allocate(std::size_t elementCount) -> T* {
        return static_cast<T*>(memory::heapAllocate(elementCount * sizeof(T), alignof(T)));
    }

    void deallocate() {
        if (data_ != nullptr) {
            memory::heapFree(data_, alignof(T));
        }
    }

    void constructRange(const T* source, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            new (data_ + i) T(source[i]);
        }
    }

    void destroyElements() {
        for (std::size_t i = 0; i < size_; ++i) {
            data_[i].~T();
        }
    }

    void growTo(std::size_t newCapacity) {
        T* newData = allocate(newCapacity);
        const std::size_t oldSize = size_;
        std::size_t constructed = 0;
        try {
            for (; constructed < oldSize; ++constructed) {
                // move_if_noexcept keeps growth non-throwing for well-behaved
                // element types and falls back to copying otherwise.
                new (newData + constructed) T(std::move_if_noexcept(data_[constructed]));
            }
        } catch (...) {
            for (std::size_t i = 0; i < constructed; ++i) {
                newData[i].~T();
            }
            memory::heapFree(newData, alignof(T));
            throw;
        }
        T* oldData = data_;
        for (std::size_t i = 0; i < oldSize; ++i) {
            oldData[i].~T();
        }
        memory::heapFree(oldData, alignof(T));
        data_ = newData;
        capacity_ = newCapacity;
    }

    void swapWith(Array& other) noexcept {
        T* tmpData = data_;
        data_ = other.data_;
        other.data_ = tmpData;
        const std::size_t tmpCapacity = capacity_;
        capacity_ = other.capacity_;
        other.capacity_ = tmpCapacity;
        const std::size_t tmpSize = size_;
        size_ = other.size_;
        other.size_ = tmpSize;
    }

    T* data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace f4::containers