#include <doctest/doctest.h>

#include "core/assert.h"

namespace {

auto countedSideEffect(int& counter) -> int {
    ++counter;
    return 7;
}

} // namespace

#if F4_ENABLE_ASSERTS

TEST_CASE("asserts are enabled in this configuration") {
    static_assert(F4_ENABLE_ASSERTS == 1);
}

TEST_CASE("enabled asserts evaluate their condition") {
    int counter = 0;
    F4_ASSERT(countedSideEffect(counter) == 7, "must be evaluated");
    CHECK(counter == 1);
}

#else

TEST_CASE("asserts follow NDEBUG in this configuration") {
    static_assert(F4_ENABLE_ASSERTS == 0);
}

TEST_CASE("disabled asserts do not evaluate their condition") {
    int counter = 0;
    F4_ASSERT(countedSideEffect(counter) == 7, "never checked");
    CHECK(counter == 0);
}

#endif

TEST_CASE("F4_VERIFY evaluates regardless of assert configuration") {
    int counter = 0;
    F4_VERIFY(countedSideEffect(counter) == 7, "verification must run");
    CHECK(counter == 1);
}
