include(FetchContent)

FetchContent_Declare(doctest
    SYSTEM
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
)
FetchContent_MakeAvailable(doctest)

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
    core/test_mat4.cpp
    core/test_transform.cpp
    core/test_shapes.cpp
    core/test_frustum.cpp
)
target_include_directories(f4_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(f4_tests PRIVATE f4_core doctest::doctest)
f4_set_warnings(f4_tests)
f4_set_float_precision(f4_tests)
f4_enable_sanitizers(f4_tests)

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

add_test(NAME core COMMAND f4_tests)
add_test(NAME core_stripped COMMAND f4_tests_stripped)

if(F4_ENABLE_SANITIZERS AND MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    get_filename_component(F4_ASAN_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(core core_stripped PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${F4_ASAN_BIN_DIR}")
endif()
