include(FetchContent)

FetchContent_Declare(stb
    SYSTEM
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(cgltf
    SYSTEM
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG 85cd62382dfea638278962690cf515023f33ed00
)
FetchContent_MakeAvailable(cgltf)

FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(zlib)

FetchContent_Declare(basis_universal
    SYSTEM
    GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
    GIT_TAG v2_50
)
set(BASISU_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BASISU_ZSTD OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(basis_universal)

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
    core/resources/cgltf_impl.cpp
    core/resources/gltf_loader.cpp
    core/resources/exr_loader.cpp
    core/resources/bc_decode.cpp
    core/resources/bc67_decode.cpp
    core/resources/dds_loader.cpp
    core/resources/basisu_transcoder_impl.cpp
    core/resources/ktx2_loader.cpp
)

target_include_directories(f4_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${stb_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${basis_universal_SOURCE_DIR}/transcoder)

target_link_libraries(f4_core PUBLIC zlibstatic)
target_include_directories(f4_core SYSTEM PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

f4_set_warnings(f4_core)
f4_set_float_precision(f4_core)
f4_enable_sanitizers(f4_core)
