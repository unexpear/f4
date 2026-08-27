# patch_tinyusdz.cmake — GCC 16 / MSYS2 compatibility fixes:
# 1. Remove -mbig-obj flag (clang-only, GCC doesn't support it)
# 2. Suppress -Wunused-function (io-util.cc has an unused Win32 helper)
# 3. Add #include <cstdint> to value-types.hh (GCC 16 removed transitive includes)
# 4. Fix TypeTraits member access in tiny-any.inc (GCC 16 dependent name lookup)

file(READ "${tinyusdz_SOURCE_DIR}/CMakeLists.txt" CMAKE_CONTENT)

# Remove the -mbig-obj flag for GCC on WIN32 — GCC doesn't support it
string(REPLACE
    "target_compile_options(\${TINYUSDZ_LIB_TARGET} PRIVATE \"-Wa, -mbig-obj\")"
    "target_compile_options(\${TINYUSDZ_LIB_TARGET} PRIVATE -Wno-unused-function -Wno-unused-but-set-variable)"
    CMAKE_CONTENT "${CMAKE_CONTENT}")

file(WRITE "${tinyusdz_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CONTENT}")

# Fix: value-types.hh — add <cstdint> (GCC 16 removed transitive includes)
file(READ "${tinyusdz_SOURCE_DIR}/src/value-types.hh" CONTENT)
if(NOT CONTENT MATCHES "#include <cstdint>")
    string(REPLACE "#include <vector>" "#include <vector>\n#include <cstdint>" CONTENT "${CONTENT}")
    file(WRITE "${tinyusdz_SOURCE_DIR}/src/value-types.hh" "${CONTENT}")
endif()

# Fix: tiny-any.inc — take address of TypeTraits member functions
# to help GCC 16 with dependent name lookup
file(READ "${tinyusdz_SOURCE_DIR}/src/tiny-any.inc" ANY_CONTENT)
if(NOT ANY_CONTENT MATCHES "auto fn")
    string(REPLACE
        "static uint32_t type_id() noexcept\n        {\n            return tinyusdz::value::TypeTraits<T>::type_id();\n        }"
        "static uint32_t type_id() noexcept\n        {\n            auto fn = &tinyusdz::value::TypeTraits<T>::type_id;\n            return fn();\n        }"
        ANY_CONTENT "${ANY_CONTENT}")
    string(REPLACE
        "static uint32_t underlying_type_id() noexcept\n        {\n            return tinyusdz::value::TypeTraits<T>::underlying_type_id();\n        }"
        "static uint32_t underlying_type_id() noexcept\n        {\n            auto fn = &tinyusdz::value::TypeTraits<T>::underlying_type_id;\n            return fn();\n        }"
        ANY_CONTENT "${ANY_CONTENT}")
    file(WRITE "${tinyusdz_SOURCE_DIR}/src/tiny-any.inc" "${ANY_CONTENT}")
endif()
