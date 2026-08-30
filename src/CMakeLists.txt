include(FetchContent)

# Backend compiled into this build. 'null' is a software RHI device with no
# window/GPU dependency (the M6 headless workhorse and conformance reference);
# opengl/vulkan/dx12 activate in M7/M9/M10.
set(F4_GRAPHICS_BACKEND "null" CACHE STRING "RHI backend: null|opengl|vulkan|dx12")
set_property(CACHE F4_GRAPHICS_BACKEND PROPERTY STRINGS null opengl vulkan dx12)

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
set(BASISU_DISABLE_ANDROID_ASTC_DECOMP ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(basis_universal)

FetchContent_Declare(ufbx
    SYSTEM
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG v0.23.0
)
FetchContent_MakeAvailable(ufbx)

FetchContent_Declare(Imath
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/Imath.git
    GIT_TAG v3.2.0
)
set(IMATH_INSTALL OFF CACHE BOOL "" FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Imath)

set(USE_HDF5 OFF CACHE BOOL "" FORCE)
set(USE_PYALEMBIC OFF CACHE BOOL "" FORCE)
set(USE_MAYA OFF CACHE BOOL "" FORCE)
set(ALEMBIC_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ALEMBIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Alembic
    GIT_REPOSITORY https://github.com/alembic/alembic.git
    GIT_TAG 1.8.12
    EXCLUDE_FROM_ALL
)
# Alembic has unconditional export()/install() calls that fail when Imath
# is not in the same export set.  Override export() as a no-op during
# FetchContent configure — we never export Alembic targets.
macro(export)
endmacro()
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(Alembic)
set(CMAKE_SKIP_INSTALL_RULES OFF)

# Alembic expects #include <Imath/half.h> but Imath's build-tree INTERFACE include
# dir points at src/Imath/ (inside the headers).  Add the parent so the path resolves.
if(TARGET Imath)
    target_include_directories(Imath INTERFACE
        $<BUILD_INTERFACE:${imath_SOURCE_DIR}/src>)
endif()

FetchContent_Declare(tinyusdz
    SYSTEM
    GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
    GIT_TAG v0.9.1
    PATCH_COMMAND ${CMAKE_COMMAND} -Dtinyusdz_SOURCE_DIR=${CMAKE_BINARY_DIR}/_deps/tinyusdz-src -P ${CMAKE_SOURCE_DIR}/cmake/patch_tinyusdz.cmake
)
FetchContent_MakeAvailable(tinyusdz)

add_library(f4_core STATIC
    core/logging/logger.cpp
    core/math/math.cpp
    core/memory/memory.cpp
    core/memory/serialization.cpp
    core/noise/noise.cpp
    core/diagnostics/metric_registry.cpp
    core/diagnostics/cpu_profiler.cpp
    core/diagnostics/diagnostics.cpp
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
    core/resources/astc_loader.cpp
    core/resources/obj_loader.cpp
    core/resources/ufbx_impl.cpp
    core/resources/fbx_loader.cpp
    core/resources/alembic_loader.cpp
    core/resources/usd_loader.cpp
)

target_include_directories(f4_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${stb_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${basis_universal_SOURCE_DIR}/transcoder)
target_include_directories(f4_core SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
target_include_directories(f4_core SYSTEM PUBLIC ${tinyusdz_SOURCE_DIR}/src)

target_link_libraries(f4_core PUBLIC zlibstatic Alembic::Alembic tinyusdz::tinyusdz_static)
target_include_directories(f4_core SYSTEM PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

target_compile_definitions(f4_core PUBLIC BASISU_DISABLE_ANDROID_ASTC_DECOMP=1)
f4_set_warnings(f4_core)
f4_set_float_precision(f4_core)
f4_enable_sanitizers(f4_core)

add_library(f4_graphics STATIC
    graphics/device/device.cpp
    graphics/command/command_buffer.cpp
    graphics/handle.cpp
    graphics/hud/hud_font.cpp
    graphics/hud/hud_overlay.cpp
    graphics/render_graph/render_graph.cpp
    backends/null/null_device.cpp
)

target_include_directories(f4_graphics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_BACKEND_${F4_GRAPHICS_BACKEND}=1)
target_link_libraries(f4_graphics PUBLIC f4_core)
f4_set_warnings(f4_graphics)
f4_set_float_precision(f4_graphics)
f4_enable_sanitizers(f4_graphics)

add_library(f4_engine STATIC
    engine/rendering/material.cpp
    engine/rendering/gpu_mesh.cpp
    engine/rendering/render_queue.cpp
    engine/rendering/renderer.cpp
)

target_include_directories(f4_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f4_engine PUBLIC f4_core f4_graphics)
f4_set_warnings(f4_engine)
f4_set_float_precision(f4_engine)
f4_enable_sanitizers(f4_engine)

# --- OpenGL backend (M7) ---------------------------------------------------
# Requires SDL2 + libepoxy, which are present on the MSYS2 ucrt64 (GCC)
# toolchain. On toolchains without them (e.g. the MSVC presets) the graphics
# library stays headless (NullDevice) and F4_GRAPHICS_HAS_OPENGL is OFF, so
# f4_platform, the GL device, the windowed demo, and the GL-conformance tests
# are all skipped. The 5-preset headless gate is therefore unchanged.
find_package(SDL2 CONFIG QUIET)

set(F4_GRAPHICS_HAS_OPENGL OFF)
# The SDL2 config installed by MSYS2 describes MinGW libraries and headers; it
# may still be discoverable from an MSVC configure, but it is not ABI/toolchain
# compatible with cl.exe. Keep MSVC on the documented headless path.
if(TARGET SDL2::SDL2 AND NOT MSVC)
    set(F4_GRAPHICS_HAS_OPENGL ON)
    add_library(f4_platform STATIC
        platform/window.cpp
        platform/gl_context.cpp
    )
    target_include_directories(f4_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(f4_platform PRIVATE f4_core SDL2::SDL2 epoxy)
    f4_set_warnings(f4_platform)
    f4_set_float_precision(f4_platform)
    f4_enable_sanitizers(f4_platform)

    target_sources(f4_graphics PRIVATE backends/opengl/opengl_device.cpp)
    target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_HAS_OPENGL=1)
    target_link_libraries(f4_graphics PRIVATE f4_platform)

    # Windowed demo (manual smoke test, not part of the headless gate).
    add_executable(f4_app app/f4_app.cpp)
    target_include_directories(f4_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(f4_app PRIVATE f4_graphics f4_platform f4_core SDL2::SDL2)
    f4_set_warnings(f4_app)
    f4_set_float_precision(f4_app)
    f4_enable_sanitizers(f4_app)

    # Focused one-capability demos.
    add_subdirectory(demos)
endif()
set(F4_GRAPHICS_HAS_OPENGL ${F4_GRAPHICS_HAS_OPENGL} PARENT_SCOPE)
