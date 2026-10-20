include(FetchContent)

FetchContent_Declare(doctest
    SYSTEM
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
)
FetchContent_MakeAvailable(doctest)
FetchContent_GetProperties(ufbx)
FetchContent_GetProperties(tinyusdz)

add_executable(f4_tests
    main.cpp
    stb_image_write_impl.cpp
    core/test_logger.cpp
    core/test_assert_enabled.cpp
    core/test_numerics.cpp
    core/test_vec.cpp
    core/test_quat.cpp
    core/test_quat_properties.cpp
    core/test_memory.cpp
    core/test_array.cpp
    core/test_ids.cpp
    core/test_string.cpp
    core/test_hashmap.cpp
    core/test_serialization.cpp
    core/test_diagnostics.cpp
    core/test_ecs.cpp
    core/test_ecs_transform.cpp
    core/test_noise.cpp
    core/test_resources.cpp
    core/test_image_loaders.cpp
    core/test_gltf_loader.cpp
    core/test_exr_loader.cpp
    core/test_bc_decode.cpp
    core/test_ktx2_loader.cpp
    core/test_astc_loader.cpp
    core/test_obj_loader.cpp
    core/test_fbx_loader.cpp
    core/test_alembic_loader.cpp
    core/test_usd_loader.cpp
    core/test_mat4.cpp
    core/test_transform.cpp
    core/test_shapes.cpp
    core/test_frustum.cpp
    graphics/test_gpu_selection.cpp
    graphics/test_rhi.cpp
    engine/test_m8.cpp
    engine/test_m11.cpp
    engine/test_m12.cpp
    engine/test_m13.cpp
    engine/test_m14.cpp
    engine/test_m15.cpp
    engine/test_m16.cpp
    engine/test_point_lights.cpp
)
target_include_directories(f4_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f4_tests PRIVATE f4_core f4_graphics f4_engine doctest::doctest)
target_compile_definitions(f4_tests PRIVATE
    F4_UFBX_TEST_DATA_DIR="${ufbx_SOURCE_DIR}/data"
    F4_TINYUSDZ_TEST_DATA_DIR="${tinyusdz_SOURCE_DIR}/models"
)
f4_set_warnings(f4_tests)
f4_set_float_precision(f4_tests)
f4_enable_sanitizers(f4_tests)
f4_prefer_high_performance_gpu(f4_tests)

# Headless OpenGL conformance: only on toolchains where the GL backend exists
# (MSYS2 ucrt64 / GCC). F4_GRAPHICS_HAS_OPENGL is set by src/CMakeLists.txt and
# propagates up to this scope. The MSVC presets stay headless (NullDevice only).
# SDL2::SDL2 is an ALIAS target that is directory-scoped, so re-locate it here.
find_package(SDL2 CONFIG QUIET)
if(F4_GRAPHICS_HAS_OPENGL AND TARGET SDL2::SDL2)
    target_sources(f4_tests PRIVATE graphics/test_gl_device.cpp)
    target_link_libraries(f4_tests PRIVATE f4_epoxy SDL2::SDL2)
    target_compile_definitions(f4_tests PRIVATE F4_GRAPHICS_HAS_OPENGL=1)
    target_compile_definitions(f4_tests PRIVATE
        F4_M13_OPENGL_SHADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp")
endif()

add_executable(f4_tests_stripped
    main.cpp
    core/test_min_log_level.cpp
    core/test_assert_disabled.cpp
)
target_compile_definitions(f4_tests_stripped PRIVATE
    F4_MIN_LOG_LEVEL=2
    F4_ENABLE_ASSERTS=0
)
target_include_directories(f4_tests_stripped PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f4_tests_stripped PRIVATE f4_core doctest::doctest)
f4_set_warnings(f4_tests_stripped)
f4_set_float_precision(f4_tests_stripped)
f4_enable_sanitizers(f4_tests_stripped)

if(F4_ENABLE_SANITIZERS AND MSVC)
    # MSVC ASan reserves a multi-terabyte shadow region. Loading the
    # unsanitized vendor Vulkan driver can then fail with a shadow-memory
    # VirtualAlloc error before any test executes; Vulkan remains covered by
    # the Debug/Release validation-layer gates.
    add_test(NAME core COMMAND f4_tests "--test-case-exclude=*Vulkan*,*DX12*,M10:*")
elseif(F4_GRAPHICS_HAS_METAL)
    # Standard hosted macOS runners build Metal but do not guarantee GPU
    # passthrough. Keep native execution in the separately labelled gate.
    add_test(NAME core COMMAND f4_tests "--test-case-exclude=Metal hardware:*")
else()
    add_test(NAME core COMMAND f4_tests)
endif()

if(F4_GRAPHICS_HAS_DX12)
    target_sources(f4_tests PRIVATE graphics/test_dx12_device.cpp)
    target_link_libraries(f4_tests PRIVATE d3d12 dxgi dxguid)

    find_program(F4_DXC_EXECUTABLE NAMES dxc dxc.exe
        HINTS "$ENV{VULKAN_SDK}/Bin"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
              "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64")
    if(F4_DXC_EXECUTABLE)
        set(F4_DX12_TEST_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/dx12-shaders")
        function(f4_compile_dxil output source profile entry)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_DX12_TEST_SHADER_DIR}"
                COMMAND "${F4_DXC_EXECUTABLE}"
                        -T "${profile}" -E "${entry}" -Ges -WX
                        -Fo "${output}" "${source}"
                DEPENDS "${source}"
                VERBATIM)
        endfunction()
        set(F4_DX12_TEST_VERTEX_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_triangle.vert.dxil")
        set(F4_DX12_TEST_FRAGMENT_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_triangle.frag.dxil")
        set(F4_DX12_TEST_BIND_VERTEX_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_bind.vert.dxil")
        set(F4_DX12_TEST_BIND_FRAGMENT_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_bind.frag.dxil")
        set(F4_DX12_TEST_PARITY_VERTEX_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_parity.vert.dxil")
        set(F4_DX12_TEST_PARITY_FRAGMENT_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_parity.frag.dxil")
        set(F4_DX12_TEST_IMAGE_FRAGMENT_DXIL "${F4_DX12_TEST_SHADER_DIR}/m10_image.frag.dxil")
        set(F4_M13_DX12_COMPUTE_DXIL "${F4_DX12_TEST_SHADER_DIR}/m13_noise.comp.dxil")
        f4_compile_dxil("${F4_DX12_TEST_VERTEX_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_triangle.vert.hlsl" vs_6_0 VSMain)
        f4_compile_dxil("${F4_DX12_TEST_FRAGMENT_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_triangle.frag.hlsl" ps_6_0 PSMain)
        f4_compile_dxil("${F4_DX12_TEST_BIND_VERTEX_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_bind.vert.hlsl" vs_6_0 VSMain)
        f4_compile_dxil("${F4_DX12_TEST_BIND_FRAGMENT_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_bind.frag.hlsl" ps_6_0 PSMain)
        f4_compile_dxil("${F4_DX12_TEST_PARITY_VERTEX_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_parity.vert.hlsl" vs_6_0 VSMain)
        f4_compile_dxil("${F4_DX12_TEST_PARITY_FRAGMENT_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_parity.frag.hlsl" ps_6_0 PSMain)
        f4_compile_dxil("${F4_DX12_TEST_IMAGE_FRAGMENT_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m10_image.frag.hlsl" ps_6_0 PSMain)
        add_custom_command(
            OUTPUT "${F4_M13_DX12_COMPUTE_DXIL}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_DX12_TEST_SHADER_DIR}"
            COMMAND "${F4_DXC_EXECUTABLE}"
                    -T cs_6_0 -E CSMain -Ges -Gis -WX
                    -Fo "${F4_M13_DX12_COMPUTE_DXIL}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp.hlsl"
            VERBATIM)
        add_custom_target(f4_dx12_test_shaders DEPENDS
            "${F4_DX12_TEST_VERTEX_DXIL}"
            "${F4_DX12_TEST_FRAGMENT_DXIL}"
            "${F4_DX12_TEST_BIND_VERTEX_DXIL}"
            "${F4_DX12_TEST_BIND_FRAGMENT_DXIL}"
            "${F4_DX12_TEST_PARITY_VERTEX_DXIL}"
            "${F4_DX12_TEST_PARITY_FRAGMENT_DXIL}"
            "${F4_DX12_TEST_IMAGE_FRAGMENT_DXIL}"
            "${F4_M13_DX12_COMPUTE_DXIL}")
        add_dependencies(f4_tests f4_dx12_test_shaders)
        target_compile_definitions(f4_tests PRIVATE
            F4_DX12_TEST_VERTEX_DXIL="${F4_DX12_TEST_VERTEX_DXIL}"
            F4_DX12_TEST_FRAGMENT_DXIL="${F4_DX12_TEST_FRAGMENT_DXIL}"
            F4_DX12_TEST_BIND_VERTEX_DXIL="${F4_DX12_TEST_BIND_VERTEX_DXIL}"
            F4_DX12_TEST_BIND_FRAGMENT_DXIL="${F4_DX12_TEST_BIND_FRAGMENT_DXIL}"
            F4_DX12_TEST_PARITY_VERTEX_DXIL="${F4_DX12_TEST_PARITY_VERTEX_DXIL}"
            F4_DX12_TEST_PARITY_FRAGMENT_DXIL="${F4_DX12_TEST_PARITY_FRAGMENT_DXIL}"
            F4_DX12_TEST_IMAGE_FRAGMENT_DXIL="${F4_DX12_TEST_IMAGE_FRAGMENT_DXIL}"
            F4_M13_DX12_COMPUTE_DXIL="${F4_M13_DX12_COMPUTE_DXIL}")
    endif()
endif()
add_test(NAME core_stripped COMMAND f4_tests_stripped)
add_test(NAME determinism COMMAND f4_tests "--test-case=golden vectors are bit-exact")
add_test(NAME world_determinism COMMAND f4_tests
    "--test-case=M14: generated policy outputs match cross-toolchain golden digests")
add_test(NAME terrain_determinism COMMAND f4_tests
    "--test-case=M15: policy mesh dumps retain deterministic golden digests")
add_test(NAME streaming_determinism COMMAND f4_tests
    "--test-case=M16: regional policies and asynchronous meshes converge deterministically")
set_tests_properties(determinism world_determinism terrain_determinism streaming_determinism
    PROPERTIES LABELS determinism)

if(F4_ENABLE_SANITIZERS AND MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    get_filename_component(F4_ASAN_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core core_stripped determinism world_determinism terrain_determinism
                         streaming_determinism
        PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${F4_ASAN_BIN_DIR}")
endif()

if(F4_GRAPHICS_HAS_VULKAN)
    # FindVulkan imported targets are directory-scoped; re-locate the target in
    # the test directory just as the OpenGL path does for SDL2 above.
    find_package(Vulkan 1.3 REQUIRED)
    target_sources(f4_tests PRIVATE graphics/test_vk_device.cpp)
    target_link_libraries(f4_tests PRIVATE Vulkan::Vulkan)

    if(TARGET Vulkan::glslangValidator)
        set(F4_VK_TEST_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders")
        set(F4_VK_TEST_VERTEX_SPV "${F4_VK_TEST_SHADER_DIR}/m9_triangle.vert.spv")
        set(F4_VK_TEST_FRAGMENT_SPV "${F4_VK_TEST_SHADER_DIR}/m9_triangle.frag.spv")
        set(F4_VK_TEST_BIND_VERTEX_SPV "${F4_VK_TEST_SHADER_DIR}/m9_bind.vert.spv")
        set(F4_VK_TEST_BIND_FRAGMENT_SPV "${F4_VK_TEST_SHADER_DIR}/m9_bind.frag.spv")
        set(F4_VK_TEST_PARITY_VERTEX_SPV "${F4_VK_TEST_SHADER_DIR}/m9_parity.vert.spv")
        set(F4_VK_TEST_PARITY_FRAGMENT_SPV "${F4_VK_TEST_SHADER_DIR}/m9_parity.frag.spv")
        set(F4_VK_TEST_IMAGE_FRAGMENT_SPV "${F4_VK_TEST_SHADER_DIR}/m9_image.frag.spv")
        set(F4_M13_VK_COMPUTE_SPV "${F4_VK_TEST_SHADER_DIR}/m13_noise.comp.spv")
        add_custom_command(
            OUTPUT "${F4_VK_TEST_VERTEX_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S vert
                    -o "${F4_VK_TEST_VERTEX_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_triangle.vert"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_triangle.vert"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_FRAGMENT_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S frag
                    -o "${F4_VK_TEST_FRAGMENT_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_triangle.frag"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_triangle.frag"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_BIND_VERTEX_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S vert
                    -o "${F4_VK_TEST_BIND_VERTEX_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_bind.vert"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_bind.vert"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_BIND_FRAGMENT_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S frag
                    -o "${F4_VK_TEST_BIND_FRAGMENT_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_bind.frag"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_bind.frag"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_PARITY_VERTEX_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S vert
                    -o "${F4_VK_TEST_PARITY_VERTEX_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_parity.vert"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_parity.vert"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_PARITY_FRAGMENT_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S frag
                    -o "${F4_VK_TEST_PARITY_FRAGMENT_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_parity.frag"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_parity.frag"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_VK_TEST_IMAGE_FRAGMENT_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S frag
                    -o "${F4_VK_TEST_IMAGE_FRAGMENT_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_image.frag"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m9_image.frag"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F4_M13_VK_COMPUTE_SPV}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${F4_VK_TEST_SHADER_DIR}"
            COMMAND $<TARGET_FILE:Vulkan::glslangValidator>
                    -V --target-env vulkan1.3 --spirv-val -S comp
                    -DF4_VULKAN=1
                    -o "${F4_M13_VK_COMPUTE_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp"
            VERBATIM)
        add_custom_target(f4_vk_test_shaders
            DEPENDS
                "${F4_VK_TEST_VERTEX_SPV}"
                "${F4_VK_TEST_FRAGMENT_SPV}"
                "${F4_VK_TEST_BIND_VERTEX_SPV}"
                "${F4_VK_TEST_BIND_FRAGMENT_SPV}"
                "${F4_VK_TEST_PARITY_VERTEX_SPV}"
                "${F4_VK_TEST_PARITY_FRAGMENT_SPV}"
                "${F4_VK_TEST_IMAGE_FRAGMENT_SPV}"
                "${F4_M13_VK_COMPUTE_SPV}")
        add_dependencies(f4_tests f4_vk_test_shaders)
        target_compile_definitions(f4_tests PRIVATE
            F4_VK_TEST_VERTEX_SPV="${F4_VK_TEST_VERTEX_SPV}"
            F4_VK_TEST_FRAGMENT_SPV="${F4_VK_TEST_FRAGMENT_SPV}"
            F4_VK_TEST_BIND_VERTEX_SPV="${F4_VK_TEST_BIND_VERTEX_SPV}"
            F4_VK_TEST_BIND_FRAGMENT_SPV="${F4_VK_TEST_BIND_FRAGMENT_SPV}"
            F4_VK_TEST_PARITY_VERTEX_SPV="${F4_VK_TEST_PARITY_VERTEX_SPV}"
            F4_VK_TEST_PARITY_FRAGMENT_SPV="${F4_VK_TEST_PARITY_FRAGMENT_SPV}"
            F4_VK_TEST_IMAGE_FRAGMENT_SPV="${F4_VK_TEST_IMAGE_FRAGMENT_SPV}"
            F4_M13_VK_COMPUTE_SPV="${F4_M13_VK_COMPUTE_SPV}")
    endif()
endif()

if(F4_GRAPHICS_HAS_METAL)
    target_sources(f4_tests PRIVATE graphics/test_metal_device.cpp)
    include(${CMAKE_SOURCE_DIR}/cmake/MetalShaders.cmake)
    set(F4_METAL_TEST_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal-shaders")
    set(F4_METAL_TEST_TRIANGLE_SHADER
        "${F4_METAL_TEST_SHADER_DIR}/m17_triangle.metallib")
    set(F4_METAL_TEST_BIND_SHADER
        "${F4_METAL_TEST_SHADER_DIR}/m17_bind.metallib")
    set(F4_M13_METAL_COMPUTE
        "${F4_METAL_TEST_SHADER_DIR}/m13_noise.comp.metallib")
    f4_compile_metallib("${F4_METAL_TEST_TRIANGLE_SHADER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m17_triangle.metal")
    f4_compile_metallib("${F4_METAL_TEST_BIND_SHADER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m17_bind.metal")
    f4_compile_metallib("${F4_M13_METAL_COMPUTE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/m13_noise.comp.metal")
    add_custom_target(f4_metal_test_shaders DEPENDS
        "${F4_METAL_TEST_TRIANGLE_SHADER}"
        "${F4_METAL_TEST_BIND_SHADER}"
        "${F4_M13_METAL_COMPUTE}")
    add_dependencies(f4_tests f4_metal_test_shaders)
    target_compile_definitions(f4_tests PRIVATE
        F4_METAL_TEST_TRIANGLE_SHADER="${F4_METAL_TEST_TRIANGLE_SHADER}"
        F4_METAL_TEST_BIND_SHADER="${F4_METAL_TEST_BIND_SHADER}"
        F4_M13_METAL_COMPUTE="${F4_M13_METAL_COMPUTE}"
        F4_M11_REFERENCE_DEFLATE_BASE64="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/m11_reference.deflate.base64")
    add_test(NAME metal_hardware COMMAND f4_tests "--test-case=Metal hardware:*")
    set_tests_properties(metal_hardware PROPERTIES LABELS metal-hardware)
    if(TARGET f4_metal_validation_app)
        add_test(NAME metal_visual_capture
            COMMAND f4_metal_validation_app
                    --capture-dir "${CMAKE_BINARY_DIR}/m17-metal-captures")
        set_tests_properties(metal_visual_capture PROPERTIES
            LABELS metal-hardware
            RUN_SERIAL TRUE)
    endif()
endif()
