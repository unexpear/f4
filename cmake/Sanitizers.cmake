include(CheckCXXSourceCompiles)

function(f4_enable_sanitizers target)
    if(NOT F4_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            set_property(TARGET ${target} PROPERTY VS_GLOBAL_EnableASAN true)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        else()
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /fsanitize=address /INCREMENTAL:NO)
        endif()
        return()
    endif()

    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
    check_cxx_source_compiles("int main() { return 0; }" F4_HAVE_ASAN)

    set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined")
    check_cxx_source_compiles("int main() { return 0; }" F4_HAVE_UBSAN)

    unset(CMAKE_REQUIRED_FLAGS)

    set(F4_APPLIED_SANITIZERS "")
    if(F4_HAVE_ASAN)
        list(APPEND F4_APPLIED_SANITIZERS -fsanitize=address)
    endif()
    if(F4_HAVE_UBSAN)
        list(APPEND F4_APPLIED_SANITIZERS -fsanitize=undefined)
    endif()

    if(F4_APPLIED_SANITIZERS STREQUAL "")
        message(WARNING
            "F4_ENABLE_SANITIZERS is ON but neither ASan nor UBSan runtimes are "
            "available for this compiler (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
            "MSYS2/mingw-w64 GCC does not ship sanitizer runtimes; use Clang64 or a Linux/CI "
            "toolchain for sanitized builds. Building ${target} WITHOUT sanitizers.")
        return()
    endif()

    list(JOIN F4_APPLIED_SANITIZERS " " F4_SANITIZER_STRING)
    target_compile_options(${target} PRIVATE ${F4_APPLIED_SANITIZERS} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${F4_APPLIED_SANITIZERS})
    message(STATUS "${target}: sanitizers applied [${F4_SANITIZER_STRING}]")
endfunction()
