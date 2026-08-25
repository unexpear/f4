#pragma once

#include <new>
#include <type_traits>
#include <utility>

namespace f4 {

// Lightweight value-or-error type. E must be an enum or trivially copyable
// error code. No heap allocation, no exceptions, no RTTI.
//
// Usage:
//   auto r = mayFail();
//   if (!r) { handle(r.error()); }
//   T val = r.value();
template <typename T, typename E>
class Result {
    static_assert(std::is_trivially_copyable_v<E>, "error type must be trivially copyable");
    static_assert(!std::is_same_v<T, E>, "value and error types must differ");

public:
    Result(T value) // NOLINT(google-explicit-constructor)
        : hasValue_(true), value_(static_cast<T&&>(value)) {}

    Result(E error) // NOLINT(google-explicit-constructor)
        : hasValue_(false), error_(error) {}

    Result(const Result& other) : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&value_) T(other.value_);
        } else {
            new (&error_) E(other.error_);
        }
    }

    Result(Result&& other) noexcept : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&value_) T(static_cast<T&&>(other.value_));
        } else {
            new (&error_) E(other.error_);
        }
    }

    auto operator=(const Result& other) -> Result& {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&value_) T(other.value_);
            } else {
                new (&error_) E(other.error_);
            }
        }
        return *this;
    }

    auto operator=(Result&& other) noexcept -> Result& {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&value_) T(static_cast<T&&>(other.value_));
            } else {
                new (&error_) E(other.error_);
            }
        }
        return *this;
    }

    ~Result() { destroy(); }

    [[nodiscard]] explicit operator bool() const { return hasValue_; }
    [[nodiscard]] auto hasValue() const -> bool { return hasValue_; }
    [[nodiscard]] auto hasError() const -> bool { return !hasValue_; }

    [[nodiscard]] auto value() const& -> const T& { return value_; }
    [[nodiscard]] auto value() & -> T& { return value_; }
    [[nodiscard]] auto value() && -> T&& { return static_cast<T&&>(value_); }

    [[nodiscard]] auto error() const -> E { return error_; }

private:
    void destroy() {
        if (hasValue_) {
            value_.~T();
        }
    }

    bool hasValue_ = false;
    union {
        T value_;
        E error_;
    };
};

} // namespace f4
