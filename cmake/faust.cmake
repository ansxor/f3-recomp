# FAUST-generated DSP headers live in runtime/audio/hle/faust/generated/ and are checked in. The
# FAUST compiler is optional and only regenerates them. Each DSP is registered once in
# CMakeLists.txt with f3rt_faust_dsp().
#
# Without a compiler (FAUST_EXECUTABLE is FAUST_EXECUTABLE-NOTFOUND, the default) f3rt_faust_dsp()
# registers nothing: the headers are ordinary sources and no build ever runs faust.
#
# With a compiler (cmake -DFAUST_EXECUTABLE=<path to faust 2.x> <build-dir>, or faust on PATH when
# configuring) every header becomes the output of a build rule over its .dsp, architecture.hpp and
# DEPENDS libraries, and the compile depfiles list the headers. A normal build of any target that
# includes them (f3rt-test-audio, f3rt-tool, ...) therefore runs faust for each header older than
# one of those inputs, without being asked, and then recompiles what includes it. The output is
# deterministic; it is rewritten with identical bytes but a new mtime. Observed: touching reverb.dsp
# regenerates reverb.hpp; touching es5510.lib regenerates reverb, chorus, echo and hall; touching
# architecture.hpp regenerates all of them, voice_filter included.
#
# The faust-regen target builds the same rules on request. It forces nothing: a header newer than
# its inputs is left alone. To regenerate one anyway, touch its .dsp or delete the header, then:
#   cmake --build <build-dir> --target faust-regen

find_program(FAUST_EXECUTABLE NAMES faust
    DOC "FAUST compiler (2.x), used only to regenerate the checked-in DSP headers")

if(FAUST_EXECUTABLE)
    add_custom_target(faust-regen
        COMMENT "Regenerating FAUST headers under runtime/audio/hle/faust/generated/")
else()
    add_custom_target(faust-regen
        COMMAND ${CMAKE_COMMAND} -E echo "faust-regen: FAUST compiler not found. Configure with -DFAUST_EXECUTABLE=<path to faust>."
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()

# f3rt_faust_dsp(NAME <name> CLASS <class> [OPTIONS <faust flags>...] [DEPENDS <file>...])
# Regenerates runtime/audio/hle/faust/generated/<name>.hpp from runtime/audio/hle/faust/<name>.dsp.
# Paths are relative to the source root so the generated header is identical across checkouts.
function(f3rt_faust_dsp)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;CLASS" "OPTIONS;DEPENDS")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_NAME OR NOT ARG_CLASS)
        message(FATAL_ERROR "f3rt_faust_dsp: NAME and CLASS are required; OPTIONS and DEPENDS are optional")
    endif()
    if(NOT FAUST_EXECUTABLE)
        return()
    endif()

    set(source_rel "runtime/audio/hle/faust/${ARG_NAME}.dsp")
    set(output_rel "runtime/audio/hle/faust/generated/${ARG_NAME}.hpp")
    set(arch_rel "runtime/audio/hle/faust/architecture.hpp")
    add_custom_command(
        OUTPUT "${CMAKE_SOURCE_DIR}/${output_rel}"
        COMMAND "${FAUST_EXECUTABLE}" -a "${arch_rel}" -cn "${ARG_CLASS}" ${ARG_OPTIONS}
                -o "${output_rel}" "${source_rel}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${source_rel}" "${CMAKE_SOURCE_DIR}/${arch_rel}" ${ARG_DEPENDS}
        COMMENT "faust: ${output_rel}"
        VERBATIM)
    add_custom_target(faust-${ARG_NAME} DEPENDS "${CMAKE_SOURCE_DIR}/${output_rel}")
    add_dependencies(faust-regen faust-${ARG_NAME})
endfunction()
