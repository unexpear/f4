add_library(f4_core STATIC
    core/logging/logger.cpp
)

target_include_directories(f4_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

f4_set_warnings(f4_core)
f4_enable_sanitizers(f4_core)
