#pragma once

#include <cstddef>
#include <cstring>

#include "core/memory/memory.h"

namespace f4::containers {

// Non-owning, read-only view over caller-owned character data.
//
// Lifetime contract: the view NEVER owns; whoever constructs it guarantees
// the backing storage outlives every use of the view. Views must not be
// stored longer than the backing object's lifetime (no escape hatch here on
// purpose - use String for ownership).
//
// Null vs empty: a default-constructed view is NULL (data == nullptr). An
// empty view may point at valid zero-length data. isValid() and empty() are
// therefore independent predicates.
class StringView {
public:
    constexpr StringView() = default;

    constexpr StringView(const char* text) // NOLINT(google-explicit-constructor)
        : data_(text), size_(f4Length(text)) {}

    constexpr StringView(const char* text, std::size_t length) : data_(text), size_(length) {}

    [[nodiscard]] constexpr auto data() const -> const char* { return data_; }
    [[nodiscard]] constexpr auto size() const -> std::size_t { return size_; }
    [[nodiscard]] constexpr auto isEmpty() const -> bool { return size_ == 0; }
    [[nodiscard]] constexpr auto isValid() const -> bool { return data_ != nullptr; }

    friend constexpr auto operator==(const StringView& a, const StringView& b) -> bool {
        if (a.size_ != b.size_) {
            return false;
        }
        if (a.data_ == b.data_) {
            return true;
        }
        for (std::size_t i = 0; i < a.size_; ++i) {
            if (a.data_[i] != b.data_[i]) {
                return false;
            }
        }
        return true;
    }

    friend constexpr auto operator!=(const StringView& a, const StringView& b) -> bool {
        return !(a == b);
    }

private:
    static constexpr auto f4Length(const char* text) -> std::size_t {
        if (text == nullptr) {
            return 0;
        }
        std::size_t n = 0;
        while (text[n] != '\0') {
            ++n;
        }
        return n;
    }

    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

// Owning, heap-backed, always-null-terminated string.
//
// Allocation behavior: capacity grows by doubling from 16 bytes; the buffer
// lives through the f4::memory seam. Copy = deep copy. Move = steal; the
// moved-from string is left empty and reusable.
class String {
public:
    String() = default;

    String(const char* text) // NOLINT(google-explicit-constructor)
        : size_(stringLength(text)), capacity_(growCapacity(size_ + 1)),
          data_(static_cast<char*>(allocate(capacity_))) {
        if (size_ > 0) {
            std::memcpy(data_, text, size_);
        }
        data_[size_] = '\0';
    }

    explicit String(StringView view)
        : size_(view.size()), capacity_(growCapacity(size_ + 1)),
          data_(static_cast<char*>(allocate(capacity_))) {
        if (size_ > 0) {
            std::memcpy(data_, view.data(), size_);
        }
        data_[size_] = '\0';
    }

    ~String() { deallocate(); }

    String(const String& other) {
        if (other.data_ == nullptr) {
            return; // members are already default-empty
        }
        size_ = other.size_;
        capacity_ = other.capacity_;
        data_ = allocate(capacity_);
        std::memcpy(data_, other.data_, size_ + 1);
    }

    auto operator=(const String& other) -> String& {
        if (this != &other) {
            String tmp(other);
            stealFrom(tmp);
        }
        return *this;
    }

    String(String&& other) noexcept
        : size_(other.size_), capacity_(other.capacity_), data_(other.data_) {
        other.resetToEmpty();
    }

    auto operator=(String&& other) noexcept -> String& {
        if (this != &other) {
            deallocate();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.resetToEmpty();
        }
        return *this;
    }

    [[nodiscard]] auto c_str() const -> const char* { return data_ != nullptr ? data_ : ""; }
    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto isEmpty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }

    [[nodiscard]] auto view() const -> StringView { return StringView(data_, size_); }

    void append(StringView suffix) {
        if (suffix.isEmpty()) {
            return;
        }
        const std::size_t newSize = size_ + suffix.size();
        if (newSize + 1 > capacity_) {
            grow(growCapacity(newSize + 1));
        }
        std::memcpy(data_ + size_, suffix.data(), suffix.size());
        size_ = newSize;
        data_[size_] = '\0';
    }

    void append(const char* text) { append(StringView(text)); }

    auto operator+=(StringView suffix) -> String& {
        append(suffix);
        return *this;
    }

    friend auto operator+(const String& a, StringView b) -> String {
        String result = a;
        result.append(b);
        return result;
    }

    friend auto operator==(const String& a, const String& b) -> bool { return a.view() == b.view(); }
    friend auto operator==(const String& a, const char* b) -> bool { return a.view() == StringView(b); }
    friend auto operator!=(const String& a, const String& b) -> bool { return !(a == b); }
    friend auto operator!=(const String& a, const char* b) -> bool { return !(a == b); }

private:
    [[nodiscard]] static auto stringLength(const char* text) -> std::size_t {
        return text != nullptr ? std::strlen(text) : 0;
    }

    [[nodiscard]] static auto growCapacity(std::size_t minimumBytes) -> std::size_t {
        std::size_t capacity = kMinBufferBytes;
        while (capacity < minimumBytes) {
            capacity *= 2;
        }
        return capacity;
    }

    [[nodiscard]] static auto allocate(std::size_t bytes) -> char* {
        return static_cast<char*>(memory::heapAllocate(bytes));
    }

    void deallocate() {
        if (data_ != nullptr) {
            memory::heapFree(data_);
        }
    }

    void grow(std::size_t newCapacity) {
        char* newData = allocate(newCapacity);
        if (data_ != nullptr) {
            std::memcpy(newData, data_, size_ + 1);
        } else {
            newData[0] = '\0';
        }
        deallocate();
        data_ = newData;
        capacity_ = newCapacity;
    }

    void resetToEmpty() {
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    // Takes ownership of other's buffer, leaving it empty and reusable.
    void stealFrom(String& other) noexcept {
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.resetToEmpty();
    }

    static constexpr std::size_t kMinBufferBytes = 16;

    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    char* data_ = nullptr;
};

} // namespace f4::containers
