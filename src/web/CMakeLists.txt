if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The browser WebGPU target requires Emscripten")
endif()

set(F4_M13_WEBGPU_SHADER
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/m13_noise_webgpu.wgsl")
set(F4_PBR_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/pbr_webgpu.vert.wgsl")
set(F4_PBR_WEBGPU_INSTANCED_VERTEX
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/pbr_instanced_webgpu.vert.wgsl")
set(F4_PBR_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/pbr_webgpu.frag.wgsl")
set(F4_SHADOW_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/shadow_webgpu.vert.wgsl")
set(F4_SHADOW_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/engine/rendering/shaders/shadow_webgpu.frag.wgsl")
set(F4_VALIDATION_PBR_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_pbr_webgpu.vert.wgsl")
set(F4_VALIDATION_PBR_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_pbr_webgpu.frag.wgsl")
set(F4_VALIDATION_FULLSCREEN_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_fullscreen_webgpu.vert.wgsl")
set(F4_VALIDATION_MIP_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_mip_webgpu.frag.wgsl")
set(F4_VALIDATION_WORLD_COLOR_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_world_color_webgpu.vert.wgsl")
set(F4_VALIDATION_VERTEX_COLOR_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_vertex_color_webgpu.frag.wgsl")
set(F4_VALIDATION_SCREEN_COLOR_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_screen_color_webgpu.vert.wgsl")
set(F4_VALIDATION_NORMAL_MAP_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_normal_map_webgpu.frag.wgsl")
set(F4_VALIDATION_GBUFFER_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_gbuffer_webgpu.vert.wgsl")
set(F4_VALIDATION_GBUFFER_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_gbuffer_webgpu.frag.wgsl")
set(F4_VALIDATION_DEFERRED_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_deferred_webgpu.frag.wgsl")
set(F4_VALIDATION_CORNELL_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_cornell_webgpu.vert.wgsl")
set(F4_VALIDATION_CORNELL_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_cornell_webgpu.frag.wgsl")
set(F4_VALIDATION_TERRAIN_WEBGPU_VERTEX
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_terrain_webgpu.vert.wgsl")
set(F4_VALIDATION_TERRAIN_WEBGPU_FRAGMENT
    "${PROJECT_SOURCE_DIR}/src/demos/shaders/validation_terrain_webgpu.frag.wgsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${F4_M13_WEBGPU_SHADER}"
    "${F4_PBR_WEBGPU_VERTEX}"
    "${F4_PBR_WEBGPU_INSTANCED_VERTEX}"
    "${F4_PBR_WEBGPU_FRAGMENT}"
    "${F4_SHADOW_WEBGPU_VERTEX}"
    "${F4_SHADOW_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_PBR_WEBGPU_VERTEX}"
    "${F4_VALIDATION_PBR_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_FULLSCREEN_WEBGPU_VERTEX}"
    "${F4_VALIDATION_MIP_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_WORLD_COLOR_WEBGPU_VERTEX}"
    "${F4_VALIDATION_VERTEX_COLOR_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_SCREEN_COLOR_WEBGPU_VERTEX}"
    "${F4_VALIDATION_NORMAL_MAP_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_GBUFFER_WEBGPU_VERTEX}"
    "${F4_VALIDATION_GBUFFER_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_DEFERRED_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_CORNELL_WEBGPU_VERTEX}"
    "${F4_VALIDATION_CORNELL_WEBGPU_FRAGMENT}"
    "${F4_VALIDATION_TERRAIN_WEBGPU_VERTEX}"
    "${F4_VALIDATION_TERRAIN_WEBGPU_FRAGMENT}")
file(READ "${F4_M13_WEBGPU_SHADER}" F4_M13_WEBGPU_WGSL)
file(READ "${F4_PBR_WEBGPU_VERTEX}" F4_PBR_WEBGPU_VERTEX_WGSL)
file(READ "${F4_PBR_WEBGPU_INSTANCED_VERTEX}" F4_PBR_WEBGPU_INSTANCED_VERTEX_WGSL)
file(READ "${F4_PBR_WEBGPU_FRAGMENT}" F4_PBR_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_SHADOW_WEBGPU_VERTEX}" F4_SHADOW_WEBGPU_VERTEX_WGSL)
file(READ "${F4_SHADOW_WEBGPU_FRAGMENT}" F4_SHADOW_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_PBR_WEBGPU_VERTEX}" F4_VALIDATION_PBR_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_PBR_WEBGPU_FRAGMENT}" F4_VALIDATION_PBR_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_FULLSCREEN_WEBGPU_VERTEX}" F4_VALIDATION_FULLSCREEN_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_MIP_WEBGPU_FRAGMENT}" F4_VALIDATION_MIP_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_WORLD_COLOR_WEBGPU_VERTEX}" F4_VALIDATION_WORLD_COLOR_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_VERTEX_COLOR_WEBGPU_FRAGMENT}" F4_VALIDATION_VERTEX_COLOR_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_SCREEN_COLOR_WEBGPU_VERTEX}" F4_VALIDATION_SCREEN_COLOR_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_NORMAL_MAP_WEBGPU_FRAGMENT}" F4_VALIDATION_NORMAL_MAP_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_GBUFFER_WEBGPU_VERTEX}" F4_VALIDATION_GBUFFER_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_GBUFFER_WEBGPU_FRAGMENT}" F4_VALIDATION_GBUFFER_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_DEFERRED_WEBGPU_FRAGMENT}" F4_VALIDATION_DEFERRED_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_CORNELL_WEBGPU_VERTEX}" F4_VALIDATION_CORNELL_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_CORNELL_WEBGPU_FRAGMENT}" F4_VALIDATION_CORNELL_WEBGPU_FRAGMENT_WGSL)
file(READ "${F4_VALIDATION_TERRAIN_WEBGPU_VERTEX}" F4_VALIDATION_TERRAIN_WEBGPU_VERTEX_WGSL)
file(READ "${F4_VALIDATION_TERRAIN_WEBGPU_FRAGMENT}" F4_VALIDATION_TERRAIN_WEBGPU_FRAGMENT_WGSL)
configure_file(webgpu_shaders.h.in generated/webgpu_shaders.h @ONLY)

add_executable(f4_webgpu_app
    ../app/f4_webgpu_app.cpp
    ../backends/webgpu/webgpu_device.cpp
    ../core/logging/logger.cpp
    ../core/memory/memory.cpp
    ../core/noise/noise.cpp
    ../core/resources/mesh.cpp
    ../demos/demo_cornell_box.cpp
    ../demos/demo_render_validation.cpp
    ../demos/demo_terrain.cpp
    ../engine/lighting/clustered_lights.cpp
    ../engine/rendering/gpu_mesh.cpp
    ../engine/rendering/material.cpp
    ../engine/rendering/render_queue.cpp
    ../engine/rendering/renderer.cpp
    ../engine/rendering/shader_constants.cpp
    ../engine/rendering/shader_parity.cpp
    ../engine/world/terrain_meshing.cpp
    ../engine/world/world_data.cpp
    ../graphics/command/command_buffer.cpp
    ../graphics/device/device.cpp
    ../graphics/handle.cpp
    ../graphics/render_graph/render_graph.cpp
)

target_include_directories(f4_webgpu_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(f4_webgpu_app PRIVATE
    F4_GRAPHICS_BACKEND_webgpu=1
    F4_WEBGPU_SHADER_HEADER="webgpu_shaders.h")
target_compile_options(f4_webgpu_app PRIVATE
    "--use-port=emdawnwebgpu:cpp_bindings=false"
    "--use-port=sdl2")
target_link_options(f4_webgpu_app PRIVATE
    "--use-port=emdawnwebgpu:cpp_bindings=false"
    "--use-port=sdl2"
    "-sASYNCIFY=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sENVIRONMENT=web"
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/webgpu_shell.html")
set_target_properties(f4_webgpu_app PROPERTIES SUFFIX ".html")
set_property(TARGET f4_webgpu_app APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webgpu_shell.html")

f4_set_warnings(f4_webgpu_app)
f4_set_float_precision(f4_webgpu_app)

if(F4_BUILD_TESTS)
    if(DEFINED ENV{EMSDK_NODE} AND EXISTS "$ENV{EMSDK_NODE}")
        set(F4_WEBGPU_NODE "$ENV{EMSDK_NODE}")
    else()
        find_program(F4_WEBGPU_NODE NAMES node REQUIRED)
    endif()

    add_test(NAME webgpu-browser-smoke
        COMMAND "${F4_WEBGPU_NODE}"
            "${PROJECT_SOURCE_DIR}/cmake/webgpu_browser_smoke.mjs"
            "$<TARGET_FILE:f4_webgpu_app>"
            "${CMAKE_CURRENT_BINARY_DIR}/webgpu-captures")
    set_tests_properties(webgpu-browser-smoke PROPERTIES
        LABELS "graphics;webgpu"
        TIMEOUT 45)
endif()
