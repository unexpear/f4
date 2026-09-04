cmake_minimum_required(VERSION 3.29)

project(f4_engine VERSION 0.1.0 LANGUAGES CXX)

# Objective-C++ is needed only by the native Metal backend. Keep it out of
# non-Apple toolchains while enabling it at the highest directory that owns the
# Objective-C++ target, as required by CMake's language model.
if(APPLE)
    enable_language(OBJCXX)
endif()

# This C++20 project does not use modules. Avoid requiring clang-scan-deps and
# an unnecessary dependency scan on CMake 3.28+ Ninja builds.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(F4_BUILD_TESTS "Build unit tests" ON)
option(F4_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(F4_ENABLE_SANITIZERS "Enable address/UB sanitizers" OFF)
option(F4_VULKAN_VALIDATION "Enable Vulkan validation in Debug builds" ON)

# One backend is selected for createDevice(). WebGPU is intentionally available
# only through the Emscripten build path below; native desktop builds continue
# through src/CMakeLists.txt unchanged.
set(F4_GRAPHICS_BACKEND "null" CACHE STRING
    "RHI backend: null|opengl|vulkan|dx12|metal|webgpu")
set_property(CACHE F4_GRAPHICS_BACKEND PROPERTY STRINGS
    null opengl vulkan dx12 metal webgpu)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FloatPrecision.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)

# MSVC's STL emits link-time checks that require every object using annotated
# containers to be built consistently. Apply ASan at directory scope so pinned
# FetchContent dependencies are instrumented along with the engine targets.
if(F4_ENABLE_SANITIZERS AND MSVC)
    foreach(config DEBUG RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC(su|[1su])" ""
            CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    add_compile_options(/fsanitize=address)
endif()

if(F4_GRAPHICS_BACKEND STREQUAL "webgpu")
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "F4_GRAPHICS_BACKEND=webgpu requires the Emscripten toolchain")
    endif()
    if(F4_BUILD_TESTS)
        enable_testing()
    endif()
    add_subdirectory(src/web)
else()
    add_subdirectory(src)

    if(F4_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
