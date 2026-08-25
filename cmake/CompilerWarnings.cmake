function(f4_set_warnings target)
    if(MSVC)
        # /Zc:preprocessor: conformant preprocessor - required for GNU-style
        # , ##__VA_ARGS__ comma elimination used by F4_CHECK_NEAR.
        # /wd4324: over-aligned structs pad intentionally (SIMD/cache-line).
        # /wd4723: potential divide by 0 - deliberate IEEE semantics are
        # pinned by tests (signed inf / NaN propagation contracts).
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor /wd4456 /wd4458 /wd4459 /wd4324 /wd4723)
        if(F4_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(F4_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
