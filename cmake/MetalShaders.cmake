find_program(F4_XCRUN_EXECUTABLE NAMES xcrun REQUIRED)

# Compile one MSL translation unit into one loadable library. Each translation
# unit may expose one entry point per shader stage; MetalDevice selects the
# function whose native stage matches ShaderDesc::stage.
function(f4_compile_metallib output source)
    get_filename_component(output_dir "${output}" DIRECTORY)
    set(air "${output}.air")
    add_custom_command(
        OUTPUT "${output}"
        BYPRODUCTS "${air}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
        COMMAND "${F4_XCRUN_EXECUTABLE}" -sdk macosx metal
                -c "${source}" -o "${air}"
        COMMAND "${F4_XCRUN_EXECUTABLE}" -sdk macosx metallib
                "${air}" -o "${output}"
        DEPENDS "${source}"
        VERBATIM)
endfunction()
