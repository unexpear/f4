# Demo Bench — unified demo runner with free camera and hot-swappable scenes.
# Controls: 1-9 / Shift+1-9 = switch scene, Tab = cycle, ESC = release mouse/quit,
# WASD+Mouse = free camera, Shift = sprint, Space/Ctrl = up/down, F11 = fullscreen.
if(TARGET SDL2::SDL2 AND NOT MSVC)
    add_library(demo_support STATIC demo_support.cpp)
    target_include_directories(demo_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(demo_support PUBLIC f4_graphics f4_platform f4_core SDL2::SDL2)
    f4_set_warnings(demo_support)
    f4_set_float_precision(demo_support)
    f4_enable_sanitizers(demo_support)

    # Demo bench: single executable containing all scenes
    add_executable(demo_bench
        demo_main.cpp
        demo_bench.cpp
        demo_triangle.cpp
        demo_instancing.cpp
        demo_resize.cpp
        demo_collision.cpp
        demo_assets.cpp
        demo_hud.cpp
        demo_ecs.cpp
        demo_cube.cpp
        demo_terrain.cpp
        demo_particles.cpp
        demo_backface.cpp
        demo_gizmo.cpp
        demo_hierarchy.cpp
        demo_cornell_box.cpp
        demo_render_validation.cpp
    )
    target_include_directories(demo_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(demo_bench PRIVATE demo_support f4_graphics f4_platform f4_core f4_engine
                                        SDL2::SDL2 f4_epoxy)
    f4_set_warnings(demo_bench)
    f4_set_float_precision(demo_bench)
    f4_enable_sanitizers(demo_bench)
endif()
