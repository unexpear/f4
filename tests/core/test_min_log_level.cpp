#include <doctest/doctest.h>

#include "capture.hpp"
#include "core/logging/logger.h"

TEST_CASE("trace and debug are stripped by F4_MIN_LOG_LEVEL") {
    static_assert(F4_MIN_LOG_LEVEL == 2);

    f4test::beginCapture();

    int evaluations = 0;
    F4_LOG_TRACE() << (evaluations += 1);
    F4_LOG_DEBUG() << (evaluations += 10);

    CHECK(evaluations == 11);
    CHECK(f4test::lines().empty());

    SUBCASE("info survives stripping") {
        F4_LOG_INFO() << "info survives stripping";
        CHECK(f4test::linesContaining("info survives stripping") == 1);
    }

    f4test::endCapture();
}
