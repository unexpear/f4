# Locked float-parity contract (plan §11.6, M13):
#  - No FMA contraction: -ffp-contract=off (GCC/Clang) / /fp:precise (MSVC).
#    Contraction differences are the #1 silent cross-toolchain divergence;
#    deterministic noise (M13+) and its bit-exact golden vectors depend on
#    this being engine-wide, not per-TU.
# Every compile target in the engine calls this alongside f4_set_warnings.
function(f4_set_float_precision target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
endfunction()
