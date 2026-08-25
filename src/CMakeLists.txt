include(FetchContent)

FetchContent_Declare(stb
    SYSTEM
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
)
FetchContent_MakeAvailable(stb)

add_library(f4_core STATIC
    core/logging/logger.cpp
    core/math/math.cpp
    core/memory/memory.cpp
    core/memory/serialization.cpp
    core/noise/noise.cpp
    core/resources/mesh.cpp
    core/resources/file_io.cpp
    core/resources/bmp_loader.cpp
    core/resources/tga_loader.cpp
    core/resources/stb_image_impl.cpp
    core/resources/stb_image_loader.cpp
)

target_include_directories(f4_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${stb_SOURCE_DIR})

f4_set_warnings(f4_core)
f4_set_float_precision(f4_core)
f4_enable_sanitizers(f4_core)
