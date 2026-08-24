#include <doctest/doctest.h>

#include "core/assert.h"

namespace {

auto countedSideEffect(int& counter) -> int {
    ++counter;
    return 7;
}

} // namespace

TEST_CASE("asserts are compiled out in this configuration") {
    static_assert(F4_ENABLE_ASSERTS == 0);
}

TEST_CASE("disabled asserts do not evaluate their condition") {
    int counter = 0;
    F4_ASSERT(countedSideEffect(counter) == 7, "never checked");
    CHECK(counter == 0);
}

TEST_CASE("F4_VERIFY remains active even when asserts are disabled") {
    int counter = 0;
    F4_VERIFY(countedSideEffect(counter) == 7, "verification must still run");
    CHECK(counter == 1);
}
