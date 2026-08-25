cmake_minimum_required(VERSION 3.21)

project(f4_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(F4_BUILD_TESTS "Build unit tests" ON)
option(F4_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(F4_ENABLE_SANITIZERS "Enable address/UB sanitizers" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FloatPrecision.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)

add_subdirectory(src)

if(F4_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
