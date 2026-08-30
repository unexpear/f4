# Focused manual demos (M7 diagnostics follow-up). Each is a single small
# executable exercising one capability; the HUD overlay is demonstrated by
# demo_hud last. Not part of the headless 5-preset gate (a window is required).
if(TARGET SDL2::SDL2 AND NOT MSVC)
    add_library(demo_support STATIC demo_support.cpp)
    target_include_directories(demo_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(demo_support PUBLIC f4_graphics f4_platform f4_core SDL2::SDL2)
    f4_set_warnings(demo_support)
    f4_set_float_precision(demo_support)
    f4_enable_sanitizers(demo_support)

    foreach(demo IN ITEMS demo_triangle demo_instancing demo_resize demo_collision
                         demo_assets demo_hud)
        add_executable(${demo} ${demo}.cpp)
        target_include_directories(${demo} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(${demo} PRIVATE demo_support f4_graphics f4_platform f4_core
                                            SDL2::SDL2)
        f4_set_warnings(${demo})
        f4_set_float_precision(${demo})
        f4_enable_sanitizers(${demo})
    endforeach()
endif()
