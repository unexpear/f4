#include <doctest/doctest.h>

#include <string>

#include "capture.hpp"
#include "core/logging/logger.h"

TEST_CASE("runtime level filters records") {
    f4test::beginCapture();
    f4::log::setLevel(f4::log::Level::Info);

    F4_LOG_TRACE() << "hidden trace";
    F4_LOG_DEBUG() << "hidden debug";
    F4_LOG_INFO() << "visible info";

    CHECK(f4test::lines().size() == 1);
    CHECK(f4test::linesContaining("visible info") == 1);

    SUBCASE("lowering the minimum level lets more through") {
        f4::log::setLevel(f4::log::Level::Debug);
        F4_LOG_DEBUG() << "debug now visible";
        CHECK(f4test::linesContaining("debug now visible") == 1);
    }

    SUBCASE("raising the minimum level hides more") {
        f4::log::setLevel(f4::log::Level::Error);
        const std::size_t before = f4test::lines().size();
        F4_LOG_WARNING() << "warning hidden at error level";
        CHECK(f4test::lines().size() == before);
        CHECK(f4test::linesContaining("warning hidden") == 0);
    }

    f4::log::setLevel(f4::log::Level::Info);
    f4test::endCapture();
}

TEST_CASE("records carry level tag and source location") {
    f4test::beginCapture();

    F4_LOG_ERROR() << "something broke";

    REQUIRE(f4test::lines().size() == 1);
    const std::string& line = f4test::lines().front();
    CHECK(line.find("[ERROR]") != std::string::npos);
    CHECK(line.find("test_logger.cpp:") != std::string::npos);

    f4test::endCapture();
}

TEST_CASE("multiple values stream into one record") {
    f4test::beginCapture();

    const int value = 42;
    F4_LOG_INFO() << "value=" << value;

    CHECK(f4test::linesContaining("value=42") == 1);

    f4test::endCapture();
}
