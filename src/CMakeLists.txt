include(FetchContent)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

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
    SYSTEM
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(zlib)

FetchContent_Declare(basis_universal
    SYSTEM
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
    GIT_TAG v2_50
    GIT_SHALLOW TRUE
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
    SYSTEM
    EXCLUDE_FROM_ALL
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
set(USE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Alembic
    SYSTEM
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
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
    GIT_TAG v0.9.1
    PATCH_COMMAND ${CMAKE_COMMAND} -Dtinyusdz_SOURCE_DIR=${CMAKE_BINARY_DIR}/_deps/tinyusdz-src -P ${CMAKE_SOURCE_DIR}/cmake/patch_tinyusdz.cmake
)
FetchContent_MakeAvailable(tinyusdz)

# Hybrid-GPU laptops (NVIDIA Optimus, AMD PowerXpress) run OpenGL on the
# integrated GPU unless the executable itself exports a high-performance hint.
# Every f4 executable that can open a GL context gets the exports.
function(f4_prefer_high_performance_gpu target)
    if(WIN32)
        target_sources(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/src/platform/high_performance_gpu.cpp")
    endif()
endfunction()

add_library(f4_core STATIC
    core/jobs/job_system.cpp
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

target_link_libraries(f4_core PUBLIC
    Threads::Threads zlibstatic Alembic::Alembic tinyusdz::tinyusdz_static)
target_include_directories(f4_core SYSTEM PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

target_compile_definitions(f4_core PUBLIC BASISU_DISABLE_ANDROID_ASTC_DECOMP=1)
f4_set_warnings(f4_core)
f4_set_float_precision(f4_core)
f4_enable_sanitizers(f4_core)

add_library(f4_graphics STATIC
    graphics/device/device.cpp
    graphics/command/command_buffer.cpp
    graphics/gpu_selection.cpp
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
    engine/lighting/clustered_lights.cpp
    engine/lighting/point_shadows.cpp
    engine/world/world_data.cpp
    engine/world/terrain_meshing.cpp
    engine/world/world_streaming.cpp
    engine/rendering/pbr.cpp
    engine/rendering/material.cpp
    engine/rendering/gpu_mesh.cpp
    engine/rendering/render_queue.cpp
    engine/rendering/renderer.cpp
    engine/rendering/world_grid.cpp
    engine/rendering/shader_constants.cpp
    engine/rendering/effect_shaders.cpp
    engine/rendering/shader_parity.cpp
)

target_include_directories(f4_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f4_engine PUBLIC f4_core f4_graphics)
target_compile_definitions(f4_engine PRIVATE
    F4_PBR_OPENGL_VERTEX="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_opengl.vert"
    F4_PBR_OPENGL_INSTANCED_VERTEX="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_instanced_opengl.vert"
    F4_PBR_OPENGL_FRAGMENT="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_opengl.frag"
    F4_SHADOW_OPENGL_VERTEX="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_opengl.vert"
    F4_SHADOW_OPENGL_FRAGMENT="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_opengl.frag"
    F4_POINT_SHADOW_OPENGL_VERTEX="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_opengl.vert"
    F4_POINT_SHADOW_OPENGL_FRAGMENT="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_opengl.frag"
    F4_BLOOM_OPENGL_VERTEX="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_opengl.vert"
    F4_BLOOM_OPENGL_FRAGMENT="${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_opengl.frag")
f4_set_warnings(f4_engine)
f4_set_float_precision(f4_engine)
f4_enable_sanitizers(f4_engine)

# --- SDL platform + OpenGL backend (M7) -----------------------------------
# The SDL window seam is shared by OpenGL and Vulkan. Epoxy is discovered
# separately so a machine with SDL + Vulkan can build windowed Vulkan without
# also installing the OpenGL backend's loader.
find_package(SDL2 CONFIG QUIET)

set(F4_GRAPHICS_HAS_OPENGL OFF)
# The SDL2 config installed by MSYS2 describes MinGW libraries and headers; it
# may still be discoverable from an MSVC configure, but it is not ABI/toolchain
# compatible with cl.exe. Keep MSVC on the documented headless path.
if(TARGET SDL2::SDL2 AND NOT MSVC)
    add_library(f4_platform STATIC
        platform/window.cpp
    )
    target_include_directories(f4_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(f4_platform PRIVATE f4_core SDL2::SDL2)
    f4_set_warnings(f4_platform)
    f4_set_float_precision(f4_platform)
    f4_enable_sanitizers(f4_platform)
endif()

get_filename_component(F4_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(F4_COMPILER_PREFIX "${F4_COMPILER_BIN_DIR}" DIRECTORY)
find_path(F4_EPOXY_INCLUDE_DIR epoxy/gl.h HINTS "${F4_COMPILER_PREFIX}/include")
find_library(F4_EPOXY_LIBRARY NAMES epoxy HINTS "${F4_COMPILER_PREFIX}/lib")
if(TARGET f4_platform AND F4_EPOXY_INCLUDE_DIR AND F4_EPOXY_LIBRARY)
    add_library(f4_epoxy UNKNOWN IMPORTED GLOBAL)
    set_target_properties(f4_epoxy PROPERTIES
        IMPORTED_LOCATION "${F4_EPOXY_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${F4_EPOXY_INCLUDE_DIR}")
    set(F4_GRAPHICS_HAS_OPENGL ON)
    target_sources(f4_platform PRIVATE platform/gl_context.cpp)
    target_link_libraries(f4_platform PRIVATE f4_epoxy)

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
    f4_prefer_high_performance_gpu(f4_app)

    # Focused one-capability demos.
    add_subdirectory(demos)
endif()
set(F4_GRAPHICS_HAS_OPENGL ${F4_GRAPHICS_HAS_OPENGL} PARENT_SCOPE)

# --- Vulkan backend (M9) ---------------------------------------------------
# Vulkan is optional for the existing build matrix. When an SDK/loader is
# present, compile the backend and its direct conformance tests even if the
# selected createDevice() backend remains null or OpenGL.
find_package(Vulkan 1.3 QUIET)
set(F4_GRAPHICS_HAS_VULKAN OFF)
set(F4_GRAPHICS_HAS_VULKAN_WINDOW OFF)
if(Vulkan_FOUND)
    set(F4_GRAPHICS_HAS_VULKAN ON)
    target_sources(f4_graphics PRIVATE backends/vulkan/vulkan_device.cpp)
    target_link_libraries(f4_graphics PRIVATE Vulkan::Vulkan)
    target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_HAS_VULKAN=1)
    if(F4_VULKAN_VALIDATION)
        target_compile_definitions(f4_graphics PRIVATE
            $<$<CONFIG:Debug>:F4_VULKAN_VALIDATION=1>)
    endif()

    if(TARGET Vulkan::glslangValidator)
        set(F4_PBR_VK_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/pbr-vulkan-shaders")
        set(F4_PBR_VK_VERTEX "${F4_PBR_VK_SHADER_DIR}/pbr.vert.spv")
        set(F4_PBR_VK_INSTANCED_VERTEX "${F4_PBR_VK_SHADER_DIR}/pbr_instanced.vert.spv")
        set(F4_PBR_VK_FRAGMENT "${F4_PBR_VK_SHADER_DIR}/pbr.frag.spv")
        set(F4_SHADOW_VK_VERTEX "${F4_PBR_VK_SHADER_DIR}/shadow.vert.spv")
        set(F4_SHADOW_VK_FRAGMENT "${F4_PBR_VK_SHADER_DIR}/shadow.frag.spv")
        set(F4_POINT_SHADOW_VK_VERTEX "${F4_PBR_VK_SHADER_DIR}/point_shadow.vert.spv")
        set(F4_POINT_SHADOW_VK_FRAGMENT "${F4_PBR_VK_SHADER_DIR}/point_shadow.frag.spv")
        set(F4_BLOOM_VK_VERTEX "${F4_PBR_VK_SHADER_DIR}/post_bloom.vert.spv")
        set(F4_BLOOM_VK_FRAGMENT "${F4_PBR_VK_SHADER_DIR}/post_bloom.frag.spv")
        function(f4_compile_pbr_spirv output source stage)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_PBR_VK_SHADER_DIR}"
                COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                        -V --target-env vulkan1.3 --target-env spirv1.5
                        --spirv-val -S "${stage}"
                        -o "${output}" "${source}"
                DEPENDS "${source}"
                VERBATIM)
        endfunction()
        f4_compile_pbr_spirv("${F4_PBR_VK_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_vulkan.vert" vert)
        f4_compile_pbr_spirv("${F4_PBR_VK_INSTANCED_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_instanced_vulkan.vert" vert)
        f4_compile_pbr_spirv("${F4_PBR_VK_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_vulkan.frag" frag)
        f4_compile_pbr_spirv("${F4_SHADOW_VK_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_vulkan.vert" vert)
        f4_compile_pbr_spirv("${F4_SHADOW_VK_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_vulkan.frag" frag)
        f4_compile_pbr_spirv("${F4_POINT_SHADOW_VK_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_vulkan.vert" vert)
        f4_compile_pbr_spirv("${F4_POINT_SHADOW_VK_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_vulkan.frag" frag)
        f4_compile_pbr_spirv("${F4_BLOOM_VK_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_vulkan.vert" vert)
        f4_compile_pbr_spirv("${F4_BLOOM_VK_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_vulkan.frag" frag)
        add_custom_target(f4_pbr_vk_shaders DEPENDS
            "${F4_PBR_VK_VERTEX}" "${F4_PBR_VK_INSTANCED_VERTEX}"
            "${F4_PBR_VK_FRAGMENT}"
            "${F4_SHADOW_VK_VERTEX}" "${F4_SHADOW_VK_FRAGMENT}"
            "${F4_POINT_SHADOW_VK_VERTEX}" "${F4_POINT_SHADOW_VK_FRAGMENT}"
            "${F4_BLOOM_VK_VERTEX}" "${F4_BLOOM_VK_FRAGMENT}")
        add_dependencies(f4_engine f4_pbr_vk_shaders)
        target_compile_definitions(f4_engine PRIVATE
            F4_PBR_VULKAN_VERTEX="${F4_PBR_VK_VERTEX}"
            F4_PBR_VULKAN_INSTANCED_VERTEX="${F4_PBR_VK_INSTANCED_VERTEX}"
            F4_PBR_VULKAN_FRAGMENT="${F4_PBR_VK_FRAGMENT}"
            F4_SHADOW_VULKAN_VERTEX="${F4_SHADOW_VK_VERTEX}"
            F4_SHADOW_VULKAN_FRAGMENT="${F4_SHADOW_VK_FRAGMENT}"
            F4_POINT_SHADOW_VULKAN_VERTEX="${F4_POINT_SHADOW_VK_VERTEX}"
            F4_POINT_SHADOW_VULKAN_FRAGMENT="${F4_POINT_SHADOW_VK_FRAGMENT}"
            F4_BLOOM_VULKAN_VERTEX="${F4_BLOOM_VK_VERTEX}"
            F4_BLOOM_VULKAN_FRAGMENT="${F4_BLOOM_VK_FRAGMENT}")
    endif()

    if(TARGET f4_platform)
        set(F4_GRAPHICS_HAS_VULKAN_WINDOW ON)
        target_link_libraries(f4_platform PRIVATE Vulkan::Headers)
        target_compile_definitions(f4_platform PRIVATE F4_PLATFORM_HAS_VULKAN=1)
        target_compile_definitions(f4_graphics PRIVATE F4_VULKAN_HAS_PLATFORM=1)
        target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_HAS_VULKAN_WINDOW=1)

        if(TARGET Vulkan::glslangValidator)
            set(F4_VK_APP_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/vulkan-app-shaders")
            set(F4_VK_APP_VERTEX_SPV "${F4_VK_APP_SHADER_DIR}/vulkan_triangle.vert.spv")
            set(F4_VK_APP_FRAGMENT_SPV "${F4_VK_APP_SHADER_DIR}/vulkan_triangle.frag.spv")
            add_custom_command(
                OUTPUT "${F4_VK_APP_VERTEX_SPV}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_APP_SHADER_DIR}"
                COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                        -V --target-env vulkan1.3 --spirv-val -S vert
                        -o "${F4_VK_APP_VERTEX_SPV}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/vulkan_triangle.vert"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/vulkan_triangle.vert"
                VERBATIM)
            add_custom_command(
                OUTPUT "${F4_VK_APP_FRAGMENT_SPV}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_APP_SHADER_DIR}"
                COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                        -V --target-env vulkan1.3 --spirv-val -S frag
                        -o "${F4_VK_APP_FRAGMENT_SPV}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/vulkan_triangle.frag"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/vulkan_triangle.frag"
                VERBATIM)
            add_custom_target(f4_vk_app_shaders
                DEPENDS "${F4_VK_APP_VERTEX_SPV}" "${F4_VK_APP_FRAGMENT_SPV}")
            add_executable(f4_vulkan_app app/f4_vulkan_app.cpp)
            add_dependencies(f4_vulkan_app f4_vk_app_shaders)
            target_compile_definitions(f4_vulkan_app PRIVATE
                F4_VK_APP_VERTEX_SPV="${F4_VK_APP_VERTEX_SPV}"
                F4_VK_APP_FRAGMENT_SPV="${F4_VK_APP_FRAGMENT_SPV}")
            target_include_directories(f4_vulkan_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_link_libraries(f4_vulkan_app PRIVATE
                f4_graphics f4_platform f4_core SDL2::SDL2)
            f4_set_warnings(f4_vulkan_app)
            f4_set_float_precision(f4_vulkan_app)
            f4_enable_sanitizers(f4_vulkan_app)
            f4_prefer_high_performance_gpu(f4_vulkan_app)
        endif()
    endif()
elseif(F4_GRAPHICS_BACKEND STREQUAL "vulkan")
    message(FATAL_ERROR "F4_GRAPHICS_BACKEND=vulkan requires Vulkan 1.3 headers and loader")
endif()
set(F4_GRAPHICS_HAS_VULKAN ${F4_GRAPHICS_HAS_VULKAN} PARENT_SCOPE)
set(F4_GRAPHICS_HAS_VULKAN_WINDOW ${F4_GRAPHICS_HAS_VULKAN_WINDOW} PARENT_SCOPE)

# --- DirectX 12 backend (M10) ----------------------------------------------
set(F4_GRAPHICS_HAS_DX12 OFF)
set(F4_GRAPHICS_HAS_DX12_WINDOW OFF)
if(WIN32)
    set(F4_GRAPHICS_HAS_DX12 ON)
    target_sources(f4_graphics PRIVATE backends/dx12/dx12_device.cpp)
    target_link_libraries(f4_graphics PRIVATE d3d12 dxgi dxguid)
    target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_HAS_DX12=1)

    find_program(F4_DXC_EXECUTABLE NAMES dxc dxc.exe
        HINTS "$ENV{VULKAN_SDK}/Bin"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64")
    if(F4_DXC_EXECUTABLE)
        set(F4_PBR_DX12_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/pbr-dx12-shaders")
        set(F4_PBR_DX12_VERTEX "${F4_PBR_DX12_SHADER_DIR}/pbr.vert.dxil")
        set(F4_PBR_DX12_INSTANCED_VERTEX "${F4_PBR_DX12_SHADER_DIR}/pbr_instanced.vert.dxil")
        set(F4_PBR_DX12_FRAGMENT "${F4_PBR_DX12_SHADER_DIR}/pbr.frag.dxil")
        set(F4_SHADOW_DX12_VERTEX "${F4_PBR_DX12_SHADER_DIR}/shadow.vert.dxil")
        set(F4_SHADOW_DX12_FRAGMENT "${F4_PBR_DX12_SHADER_DIR}/shadow.frag.dxil")
        set(F4_POINT_SHADOW_DX12_VERTEX "${F4_PBR_DX12_SHADER_DIR}/point_shadow.vert.dxil")
        set(F4_POINT_SHADOW_DX12_FRAGMENT "${F4_PBR_DX12_SHADER_DIR}/point_shadow.frag.dxil")
        set(F4_BLOOM_DX12_VERTEX "${F4_PBR_DX12_SHADER_DIR}/post_bloom.vert.dxil")
        set(F4_BLOOM_DX12_FRAGMENT "${F4_PBR_DX12_SHADER_DIR}/post_bloom.frag.dxil")
        function(f4_compile_pbr_dxil output source profile entry)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_PBR_DX12_SHADER_DIR}"
                COMMAND "${F4_DXC_EXECUTABLE}" -T "${profile}" -E "${entry}"
                        -Ges -WX -Fo "${output}" "${source}"
                DEPENDS "${source}"
                VERBATIM)
        endfunction()
        f4_compile_pbr_dxil("${F4_PBR_DX12_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_dx12.hlsl"
            vs_6_0 VSMain)
        f4_compile_pbr_dxil("${F4_PBR_DX12_INSTANCED_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_dx12.hlsl"
            vs_6_0 VSMainInstanced)
        f4_compile_pbr_dxil("${F4_PBR_DX12_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_dx12.hlsl"
            ps_6_0 PSMain)
        f4_compile_pbr_dxil("${F4_SHADOW_DX12_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_dx12.hlsl"
            vs_6_0 VSMain)
        f4_compile_pbr_dxil("${F4_SHADOW_DX12_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_dx12.hlsl"
            ps_6_0 PSMain)
        f4_compile_pbr_dxil("${F4_POINT_SHADOW_DX12_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_dx12.hlsl"
            vs_6_0 VSMain)
        f4_compile_pbr_dxil("${F4_POINT_SHADOW_DX12_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_dx12.hlsl"
            ps_6_0 PSMain)
        f4_compile_pbr_dxil("${F4_BLOOM_DX12_VERTEX}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_dx12.hlsl"
            vs_6_0 VSMain)
        f4_compile_pbr_dxil("${F4_BLOOM_DX12_FRAGMENT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_dx12.hlsl"
            ps_6_0 PSMain)
        add_custom_target(f4_pbr_dx12_shaders DEPENDS
            "${F4_PBR_DX12_VERTEX}" "${F4_PBR_DX12_INSTANCED_VERTEX}"
            "${F4_PBR_DX12_FRAGMENT}"
            "${F4_SHADOW_DX12_VERTEX}" "${F4_SHADOW_DX12_FRAGMENT}"
            "${F4_POINT_SHADOW_DX12_VERTEX}" "${F4_POINT_SHADOW_DX12_FRAGMENT}"
            "${F4_BLOOM_DX12_VERTEX}" "${F4_BLOOM_DX12_FRAGMENT}")
        add_dependencies(f4_engine f4_pbr_dx12_shaders)
        target_compile_definitions(f4_engine PRIVATE
            F4_PBR_DX12_VERTEX="${F4_PBR_DX12_VERTEX}"
            F4_PBR_DX12_INSTANCED_VERTEX="${F4_PBR_DX12_INSTANCED_VERTEX}"
            F4_PBR_DX12_FRAGMENT="${F4_PBR_DX12_FRAGMENT}"
            F4_SHADOW_DX12_VERTEX="${F4_SHADOW_DX12_VERTEX}"
            F4_SHADOW_DX12_FRAGMENT="${F4_SHADOW_DX12_FRAGMENT}"
            F4_POINT_SHADOW_DX12_VERTEX="${F4_POINT_SHADOW_DX12_VERTEX}"
            F4_POINT_SHADOW_DX12_FRAGMENT="${F4_POINT_SHADOW_DX12_FRAGMENT}"
            F4_BLOOM_DX12_VERTEX="${F4_BLOOM_DX12_VERTEX}"
            F4_BLOOM_DX12_FRAGMENT="${F4_BLOOM_DX12_FRAGMENT}")
    endif()
    if(TARGET f4_platform)
        set(F4_GRAPHICS_HAS_DX12_WINDOW ON)
        target_compile_definitions(f4_graphics PRIVATE F4_DX12_HAS_PLATFORM=1)
        target_compile_definitions(f4_graphics PUBLIC F4_GRAPHICS_HAS_DX12_WINDOW=1)
        target_link_libraries(f4_graphics PRIVATE f4_platform)

        if(F4_DXC_EXECUTABLE)
            set(F4_DX12_APP_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/dx12-app-shaders")
            set(F4_DX12_APP_VERTEX_DXIL "${F4_DX12_APP_SHADER_DIR}/dx12_triangle.vert.dxil")
            set(F4_DX12_APP_FRAGMENT_DXIL "${F4_DX12_APP_SHADER_DIR}/dx12_triangle.frag.dxil")
            add_custom_command(
                OUTPUT "${F4_DX12_APP_VERTEX_DXIL}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_DX12_APP_SHADER_DIR}"
                COMMAND "${F4_DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Ges -WX
                        -Fo "${F4_DX12_APP_VERTEX_DXIL}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/dx12_triangle.vert.hlsl"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/dx12_triangle.vert.hlsl"
                VERBATIM)
            add_custom_command(
                OUTPUT "${F4_DX12_APP_FRAGMENT_DXIL}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_DX12_APP_SHADER_DIR}"
                COMMAND "${F4_DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Ges -WX
                        -Fo "${F4_DX12_APP_FRAGMENT_DXIL}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/dx12_triangle.frag.hlsl"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/dx12_triangle.frag.hlsl"
                VERBATIM)
            add_custom_target(f4_dx12_app_shaders DEPENDS
                "${F4_DX12_APP_VERTEX_DXIL}" "${F4_DX12_APP_FRAGMENT_DXIL}")
            add_executable(f4_dx12_app app/f4_dx12_app.cpp)
            add_dependencies(f4_dx12_app f4_dx12_app_shaders)
            target_compile_definitions(f4_dx12_app PRIVATE
                F4_DX12_APP_VERTEX_DXIL="${F4_DX12_APP_VERTEX_DXIL}"
                F4_DX12_APP_FRAGMENT_DXIL="${F4_DX12_APP_FRAGMENT_DXIL}")
            target_include_directories(f4_dx12_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_link_libraries(f4_dx12_app PRIVATE
                f4_graphics f4_platform f4_core SDL2::SDL2)
            f4_set_warnings(f4_dx12_app)
            f4_set_float_precision(f4_dx12_app)
            f4_enable_sanitizers(f4_dx12_app)
            f4_prefer_high_performance_gpu(f4_dx12_app)
        endif()
    endif()
elseif(F4_GRAPHICS_BACKEND STREQUAL "dx12")
    message(FATAL_ERROR "F4_GRAPHICS_BACKEND=dx12 is only available on Windows")
endif()
set(F4_GRAPHICS_HAS_DX12 ${F4_GRAPHICS_HAS_DX12} PARENT_SCOPE)
set(F4_GRAPHICS_HAS_DX12_WINDOW ${F4_GRAPHICS_HAS_DX12_WINDOW} PARENT_SCOPE)

# --- Native Metal backend (M17) ---------------------------------------------
set(F4_GRAPHICS_HAS_METAL OFF)
set(F4_GRAPHICS_HAS_METAL_WINDOW OFF)
if(APPLE)
    if(NOT TARGET f4_platform)
        message(FATAL_ERROR "The Metal backend requires SDL2 and f4_platform")
    endif()
    find_library(F4_METAL_FRAMEWORK Metal REQUIRED)
    find_library(F4_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    set(F4_GRAPHICS_HAS_METAL ON)
    set(F4_GRAPHICS_HAS_METAL_WINDOW ON)
    target_sources(f4_graphics PRIVATE backends/metal/metal_device.mm)
    target_compile_options(f4_graphics PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(f4_graphics PUBLIC
        F4_GRAPHICS_HAS_METAL=1 F4_GRAPHICS_HAS_METAL_WINDOW=1)
    target_link_libraries(f4_graphics PRIVATE
        f4_platform ${F4_METAL_FRAMEWORK} ${F4_QUARTZCORE_FRAMEWORK})

    include(${CMAKE_SOURCE_DIR}/cmake/MetalShaders.cmake)
    set(F4_PBR_METAL_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/pbr-metal-shaders")
    set(F4_PBR_METAL_VERTEX "${F4_PBR_METAL_SHADER_DIR}/pbr.vert.metallib")
    set(F4_PBR_METAL_INSTANCED_VERTEX
        "${F4_PBR_METAL_SHADER_DIR}/pbr_instanced.vert.metallib")
    set(F4_PBR_METAL_FRAGMENT "${F4_PBR_METAL_SHADER_DIR}/pbr.frag.metallib")
    set(F4_SHADOW_METAL_VERTEX "${F4_PBR_METAL_SHADER_DIR}/shadow.vert.metallib")
    set(F4_SHADOW_METAL_FRAGMENT "${F4_PBR_METAL_SHADER_DIR}/shadow.frag.metallib")
    set(F4_POINT_SHADOW_METAL_VERTEX "${F4_PBR_METAL_SHADER_DIR}/point_shadow.vert.metallib")
    set(F4_POINT_SHADOW_METAL_FRAGMENT "${F4_PBR_METAL_SHADER_DIR}/point_shadow.frag.metallib")
    set(F4_BLOOM_METAL_VERTEX "${F4_PBR_METAL_SHADER_DIR}/post_bloom.vert.metallib")
    set(F4_BLOOM_METAL_FRAGMENT "${F4_PBR_METAL_SHADER_DIR}/post_bloom.frag.metallib")
    f4_compile_metallib("${F4_PBR_METAL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_metal.vert.metal")
    f4_compile_metallib("${F4_PBR_METAL_INSTANCED_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_instanced_metal.vert.metal")
    f4_compile_metallib("${F4_PBR_METAL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/pbr_metal.frag.metal")
    f4_compile_metallib("${F4_SHADOW_METAL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_metal.vert.metal")
    f4_compile_metallib("${F4_SHADOW_METAL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/shadow_metal.frag.metal")
    f4_compile_metallib("${F4_POINT_SHADOW_METAL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_metal.vert.metal")
    f4_compile_metallib("${F4_POINT_SHADOW_METAL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/point_shadow_metal.frag.metal")
    f4_compile_metallib("${F4_BLOOM_METAL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_metal.vert.metal")
    f4_compile_metallib("${F4_BLOOM_METAL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/rendering/shaders/post_bloom_metal.frag.metal")
    add_custom_target(f4_pbr_metal_shaders DEPENDS
        "${F4_PBR_METAL_VERTEX}" "${F4_PBR_METAL_INSTANCED_VERTEX}"
        "${F4_PBR_METAL_FRAGMENT}" "${F4_SHADOW_METAL_VERTEX}"
        "${F4_SHADOW_METAL_FRAGMENT}"
        "${F4_POINT_SHADOW_METAL_VERTEX}" "${F4_POINT_SHADOW_METAL_FRAGMENT}"
        "${F4_BLOOM_METAL_VERTEX}" "${F4_BLOOM_METAL_FRAGMENT}")
    add_dependencies(f4_engine f4_pbr_metal_shaders)
    target_compile_definitions(f4_engine PRIVATE
        F4_PBR_METAL_VERTEX="${F4_PBR_METAL_VERTEX}"
        F4_PBR_METAL_INSTANCED_VERTEX="${F4_PBR_METAL_INSTANCED_VERTEX}"
        F4_PBR_METAL_FRAGMENT="${F4_PBR_METAL_FRAGMENT}"
        F4_SHADOW_METAL_VERTEX="${F4_SHADOW_METAL_VERTEX}"
        F4_SHADOW_METAL_FRAGMENT="${F4_SHADOW_METAL_FRAGMENT}"
        F4_POINT_SHADOW_METAL_VERTEX="${F4_POINT_SHADOW_METAL_VERTEX}"
        F4_POINT_SHADOW_METAL_FRAGMENT="${F4_POINT_SHADOW_METAL_FRAGMENT}"
        F4_BLOOM_METAL_VERTEX="${F4_BLOOM_METAL_VERTEX}"
        F4_BLOOM_METAL_FRAGMENT="${F4_BLOOM_METAL_FRAGMENT}")

    set(F4_METAL_APP_SHADER "${CMAKE_CURRENT_BINARY_DIR}/metal-app-shaders/triangle.metallib")
    f4_compile_metallib("${F4_METAL_APP_SHADER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/metal_triangle.metal")
    add_custom_target(f4_metal_app_shaders DEPENDS "${F4_METAL_APP_SHADER}")
    add_executable(f4_metal_app app/f4_metal_app.cpp)
    add_dependencies(f4_metal_app f4_metal_app_shaders)
    target_compile_definitions(f4_metal_app PRIVATE
        F4_METAL_APP_SHADER="${F4_METAL_APP_SHADER}")
    target_include_directories(f4_metal_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(f4_metal_app PRIVATE
        f4_graphics f4_platform f4_core SDL2::SDL2)
    f4_set_warnings(f4_metal_app)
    f4_set_float_precision(f4_metal_app)
    f4_enable_sanitizers(f4_metal_app)

    set(F4_METAL_VALIDATION_SHADER_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/metal-validation-shaders")
    set(F4_CUBE_METAL_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/cube.vert.metallib")
    set(F4_CUBE_METAL_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/cube.frag.metallib")
    set(F4_VALIDATION_METAL_CORNELL_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/cornell.vert.metallib")
    set(F4_VALIDATION_METAL_CORNELL_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/cornell.frag.metallib")
    set(F4_VALIDATION_METAL_DEFERRED_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/deferred.frag.metallib")
    set(F4_VALIDATION_METAL_FULLSCREEN_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/fullscreen.vert.metallib")
    set(F4_VALIDATION_METAL_GBUFFER_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/gbuffer.frag.metallib")
    set(F4_VALIDATION_METAL_GBUFFER_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/gbuffer.vert.metallib")
    set(F4_VALIDATION_METAL_MIP_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/mip.frag.metallib")
    set(F4_VALIDATION_METAL_NORMAL_MAP_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/normal_map.frag.metallib")
    set(F4_VALIDATION_METAL_PBR_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/pbr.frag.metallib")
    set(F4_VALIDATION_METAL_PBR_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/pbr.vert.metallib")
    set(F4_VALIDATION_METAL_SCREEN_COLOR_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/screen_color.vert.metallib")
    set(F4_VALIDATION_METAL_TERRAIN_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/terrain.frag.metallib")
    set(F4_VALIDATION_METAL_TERRAIN_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/terrain.vert.metallib")
    set(F4_VALIDATION_METAL_VERTEX_COLOR_FRAGMENT
        "${F4_METAL_VALIDATION_SHADER_DIR}/vertex_color.frag.metallib")
    set(F4_VALIDATION_METAL_WORLD_COLOR_VERTEX
        "${F4_METAL_VALIDATION_SHADER_DIR}/world_color.vert.metallib")

    f4_compile_metallib("${F4_CUBE_METAL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/cube_metal.vert.metal")
    f4_compile_metallib("${F4_CUBE_METAL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/cube_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_CORNELL_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_cornell_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_CORNELL_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_cornell_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_DEFERRED_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_deferred_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_FULLSCREEN_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_fullscreen_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_GBUFFER_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_gbuffer_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_GBUFFER_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_gbuffer_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_MIP_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_mip_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_NORMAL_MAP_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_normal_map_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_PBR_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_pbr_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_PBR_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_pbr_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_SCREEN_COLOR_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_screen_color_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_TERRAIN_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_terrain_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_TERRAIN_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_terrain_metal.vert.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_VERTEX_COLOR_FRAGMENT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_vertex_color_metal.frag.metal")
    f4_compile_metallib("${F4_VALIDATION_METAL_WORLD_COLOR_VERTEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/demos/shaders/validation_world_color_metal.vert.metal")
    add_custom_target(f4_metal_validation_shaders DEPENDS
        "${F4_CUBE_METAL_VERTEX}"
        "${F4_CUBE_METAL_FRAGMENT}"
        "${F4_VALIDATION_METAL_CORNELL_VERTEX}"
        "${F4_VALIDATION_METAL_CORNELL_FRAGMENT}"
        "${F4_VALIDATION_METAL_DEFERRED_FRAGMENT}"
        "${F4_VALIDATION_METAL_FULLSCREEN_VERTEX}"
        "${F4_VALIDATION_METAL_GBUFFER_FRAGMENT}"
        "${F4_VALIDATION_METAL_GBUFFER_VERTEX}"
        "${F4_VALIDATION_METAL_MIP_FRAGMENT}"
        "${F4_VALIDATION_METAL_NORMAL_MAP_FRAGMENT}"
        "${F4_VALIDATION_METAL_PBR_FRAGMENT}"
        "${F4_VALIDATION_METAL_PBR_VERTEX}"
        "${F4_VALIDATION_METAL_SCREEN_COLOR_VERTEX}"
        "${F4_VALIDATION_METAL_TERRAIN_FRAGMENT}"
        "${F4_VALIDATION_METAL_TERRAIN_VERTEX}"
        "${F4_VALIDATION_METAL_VERTEX_COLOR_FRAGMENT}"
        "${F4_VALIDATION_METAL_WORLD_COLOR_VERTEX}")
    add_executable(f4_metal_validation_app
        app/f4_metal_validation_app.cpp
        demos/demo_cornell_box.cpp
        demos/demo_cube.cpp
        demos/demo_render_validation.cpp
        demos/demo_terrain.cpp)
    add_dependencies(f4_metal_validation_app f4_metal_validation_shaders)
    target_compile_definitions(f4_metal_validation_app PRIVATE
        F4_CUBE_METAL_VERTEX="${F4_CUBE_METAL_VERTEX}"
        F4_CUBE_METAL_FRAGMENT="${F4_CUBE_METAL_FRAGMENT}"
        F4_VALIDATION_METAL_CORNELL_VERTEX="${F4_VALIDATION_METAL_CORNELL_VERTEX}"
        F4_VALIDATION_METAL_CORNELL_FRAGMENT="${F4_VALIDATION_METAL_CORNELL_FRAGMENT}"
        F4_VALIDATION_METAL_DEFERRED_FRAGMENT="${F4_VALIDATION_METAL_DEFERRED_FRAGMENT}"
        F4_VALIDATION_METAL_FULLSCREEN_VERTEX="${F4_VALIDATION_METAL_FULLSCREEN_VERTEX}"
        F4_VALIDATION_METAL_GBUFFER_FRAGMENT="${F4_VALIDATION_METAL_GBUFFER_FRAGMENT}"
        F4_VALIDATION_METAL_GBUFFER_VERTEX="${F4_VALIDATION_METAL_GBUFFER_VERTEX}"
        F4_VALIDATION_METAL_MIP_FRAGMENT="${F4_VALIDATION_METAL_MIP_FRAGMENT}"
        F4_VALIDATION_METAL_NORMAL_MAP_FRAGMENT="${F4_VALIDATION_METAL_NORMAL_MAP_FRAGMENT}"
        F4_VALIDATION_METAL_PBR_FRAGMENT="${F4_VALIDATION_METAL_PBR_FRAGMENT}"
        F4_VALIDATION_METAL_PBR_VERTEX="${F4_VALIDATION_METAL_PBR_VERTEX}"
        F4_VALIDATION_METAL_SCREEN_COLOR_VERTEX="${F4_VALIDATION_METAL_SCREEN_COLOR_VERTEX}"
        F4_VALIDATION_METAL_TERRAIN_FRAGMENT="${F4_VALIDATION_METAL_TERRAIN_FRAGMENT}"
        F4_VALIDATION_METAL_TERRAIN_VERTEX="${F4_VALIDATION_METAL_TERRAIN_VERTEX}"
        F4_VALIDATION_METAL_VERTEX_COLOR_FRAGMENT="${F4_VALIDATION_METAL_VERTEX_COLOR_FRAGMENT}"
        F4_VALIDATION_METAL_WORLD_COLOR_VERTEX="${F4_VALIDATION_METAL_WORLD_COLOR_VERTEX}")
    target_include_directories(f4_metal_validation_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(f4_metal_validation_app PRIVATE
        f4_engine f4_graphics f4_platform f4_core SDL2::SDL2)
    f4_set_warnings(f4_metal_validation_app)
    f4_set_float_precision(f4_metal_validation_app)
    f4_enable_sanitizers(f4_metal_validation_app)
elseif(F4_GRAPHICS_BACKEND STREQUAL "metal")
    message(FATAL_ERROR "F4_GRAPHICS_BACKEND=metal is only available on Apple platforms")
endif()
set(F4_GRAPHICS_HAS_METAL ${F4_GRAPHICS_HAS_METAL} PARENT_SCOPE)
set(F4_GRAPHICS_HAS_METAL_WINDOW ${F4_GRAPHICS_HAS_METAL_WINDOW} PARENT_SCOPE)
