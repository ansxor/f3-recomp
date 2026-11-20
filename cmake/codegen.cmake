file(GLOB F3_GAME_CONFIGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/*/config.toml")
set(F3_GAME_CHOICES)
foreach(config IN LISTS F3_GAME_CONFIGS)
    get_filename_component(game_directory "${config}" DIRECTORY)
    get_filename_component(game_id "${game_directory}" NAME)
    list(APPEND F3_GAME_CHOICES "${game_id}")
endforeach()

set(F3_GAME "landmakrj" CACHE STRING "ROM set (discovered from games/*/config.toml)")
set_property(CACHE F3_GAME PROPERTY STRINGS ${F3_GAME_CHOICES})
set(F3_GAME_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/config.toml")
if(NOT F3_GAME IN_LIST F3_GAME_CHOICES OR NOT EXISTS "${F3_GAME_CONFIG}")
    message(FATAL_ERROR "Unsupported F3_GAME: ${F3_GAME}")
endif()

set(F3_GAME_EXECUTABLE "${F3_GAME}" CACHE INTERNAL "Game executable name")
set(F3_ROM_DIR "" CACHE PATH "Selected game's ROM directory; enables automatic native generation")
set(F3_GENERATED_DIR "" CACHE PATH "Pre-generated main CPU C directory (optional)")
set(F3_RECOMP_EXECUTABLE "" CACHE FILEPATH "Override the f3recomp executable for configure-time code generation")

if(F3_RECOMP_EXECUTABLE)
    get_filename_component(F3_RECOMP_COMMAND "${F3_RECOMP_EXECUTABLE}" ABSOLUTE)
else()
    find_program(F3_RECOMP_COMMAND NAMES f3recomp NO_CACHE)
    if(NOT F3_RECOMP_COMMAND)
        find_program(F3_DUNE_EXECUTABLE NAMES dune NO_CACHE)
        if(NOT F3_DUNE_EXECUTABLE)
            message(FATAL_ERROR "f3recomp and dune were not found. Run cmake inside nix develop or set F3_RECOMP_EXECUTABLE.")
        endif()

        set(_recomp_dir "${CMAKE_CURRENT_SOURCE_DIR}/recomp")
        # Watch project inputs without descending into Dune's _build directory.
        file(GLOB _recomp_root_sources LIST_DIRECTORIES false CONFIGURE_DEPENDS "${_recomp_dir}/*")
        file(GLOB_RECURSE _recomp_sources LIST_DIRECTORIES false CONFIGURE_DEPENDS
            "${_recomp_dir}/bin/*"
            "${_recomp_dir}/lib/*"
            "${_recomp_dir}/test/*"
        )
        list(APPEND _recomp_sources ${_recomp_root_sources})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_recomp_sources})
        # Dune can reuse a byte-identical executable when a source is touched.
        # Track source mtimes as well so a touch still reruns generation.
        foreach(_source IN LISTS _recomp_sources)
            file(TIMESTAMP "${_source}" _mtime "%s.%f" UTC)
            list(APPEND F3_RECOMP_SOURCE_TIMESTAMPS "${_source}:${_mtime}")
        endforeach()

        # All game build trees share Dune's source-tree build directory.
        set(_recomp_lock "${_recomp_dir}/_build/cmake-dune.lock")
        file(LOCK "${_recomp_lock}" GUARD PROCESS)
        execute_process(
            COMMAND "${F3_DUNE_EXECUTABLE}" build --root "${_recomp_dir}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMAND_ERROR_IS_FATAL ANY
        )
        file(LOCK "${_recomp_lock}" RELEASE)
        set(F3_RECOMP_COMMAND "${_recomp_dir}/_build/default/bin/main.exe")
    endif()
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${F3_RECOMP_COMMAND}")
execute_process(
    COMMAND "${F3_RECOMP_COMMAND}" check-game "${F3_GAME_CONFIG}" "${F3_GAME}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
)

function(f3_compute_generator_stamp out_stamp)
    cmake_parse_arguments(ARG "" "" "INPUT_FILES;EXTRA_DATA" ${ARGN})
    set(_hash_data "")
    set(_inputs_sorted ${ARG_INPUT_FILES})
    list(SORT _inputs_sorted)
    foreach(_in ${_inputs_sorted})
        if(EXISTS "${_in}")
            file(SHA256 "${_in}" _fhash)
            string(APPEND _hash_data "${_in}:${_fhash};")
        else()
            string(APPEND _hash_data "${_in}:MISSING;")
        endif()
    endforeach()
    foreach(_extra ${ARG_EXTRA_DATA})
        string(APPEND _hash_data "EXTRA:${_extra};")
    endforeach()
    string(SHA256 _computed "${_hash_data}")
    set(${out_stamp} "${_computed}" PARENT_SCOPE)
endfunction()

function(f3_run_generator)
    cmake_parse_arguments(ARG "" "STAMP_FILE" "INPUT_FILES;EXTRA_DATA;OUTPUT_FILES;COMMAND" ${ARGN})
    set(_outputs_exist TRUE)
    foreach(_out ${ARG_OUTPUT_FILES})
        if(NOT EXISTS "${_out}")
            set(_outputs_exist FALSE)
            break()
        endif()
    endforeach()

    f3_compute_generator_stamp(_computed_stamp
        INPUT_FILES "${F3_RECOMP_COMMAND}" ${ARG_INPUT_FILES}
        EXTRA_DATA ${F3_RECOMP_SOURCE_TIMESTAMPS} ${ARG_EXTRA_DATA}
    )

    set(_stamp_matches FALSE)
    if(EXISTS "${ARG_STAMP_FILE}")
        file(READ "${ARG_STAMP_FILE}" _existing_stamp)
        string(STRIP "${_existing_stamp}" _existing_stamp)
        if("${_existing_stamp}" STREQUAL "${_computed_stamp}")
            set(_stamp_matches TRUE)
        endif()
    endif()

    if(_outputs_exist AND _stamp_matches)
        return()
    endif()

    execute_process(
        COMMAND "${F3_RECOMP_COMMAND}" ${ARG_COMMAND}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )

    get_filename_component(_sdir "${ARG_STAMP_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_sdir}")
    file(WRITE "${ARG_STAMP_FILE}" "${_computed_stamp}\n")
endfunction()

set(F3_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_config")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${F3_GAME_CONFIGS})
set(F3_COMPILE_ROMS_ARGS
    --configs ${F3_GAME_CONFIGS}
    --output "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
    --game "${F3_GAME}"
    --game-output "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp"
)
f3_run_generator(
    STAMP_FILE "${F3_GENERATED_CONFIG_DIR}/compile_roms.stamp"
    INPUT_FILES ${F3_GAME_CONFIGS}
    EXTRA_DATA "${F3_COMPILE_ROMS_ARGS}"
    OUTPUT_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
        "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp"
    COMMAND compile-roms ${F3_COMPILE_ROMS_ARGS}
)

set(F3_COMPILE_SPRITE_UNITS_ARGS
    --config "${F3_GAME_CONFIG}"
    --output-dir "${F3_GENERATED_CONFIG_DIR}"
)
f3_run_generator(
    STAMP_FILE "${F3_GENERATED_CONFIG_DIR}/compile_sprite_units.stamp"
    INPUT_FILES "${F3_GAME_CONFIG}"
    EXTRA_DATA "${F3_COMPILE_SPRITE_UNITS_ARGS}"
    OUTPUT_FILES
        "${F3_GENERATED_CONFIG_DIR}/sprite_units.h"
        "${F3_GENERATED_CONFIG_DIR}/sprite_units.hpp"
    COMMAND sprite-units ${F3_COMPILE_SPRITE_UNITS_ARGS}
)

option(F3_RECOMP_ALLOW_UNPROVEN_IRQ "Temporarily allow unproven OCaml interrupt handlers" OFF)
option(F3_PROFILE_INSTRUMENT "Record every generated main/sound entry with --profile-out" OFF)
if(F3_PROFILE_INSTRUMENT AND NOT F3_ROM_DIR)
    message(FATAL_ERROR "F3_PROFILE_INSTRUMENT requires F3_ROM_DIR to regenerate instrumentable main/sound entries")
endif()

set(F3_PROFILE_TIERS "" CACHE FILEPATH "Execution profile for full-coverage hot -O2 / cold size-optimized code")
set(F3_PROFILE_SLIM "" CACHE FILEPATH "Explicit opt-in profile removing cold main/sound code; cold hits abort")
option(F3_PROFILE_DEFAULT_TIERS "Use the frozen full-coverage profile tiers when generating from ROMs" ON)

if(F3_ROM_DIR AND F3_PROFILE_DEFAULT_TIERS AND NOT F3_PROFILE_TIERS AND NOT F3_PROFILE_SLIM)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/${F3_GAME}.profile")
        set(F3_PROFILE_TIERS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/${F3_GAME}.profile")
    else()
        message(STATUS "profiles/${F3_GAME}.profile not found; building without profile tiers")
    endif()
endif()

if(F3_PROFILE_TIERS AND F3_PROFILE_SLIM)
    message(FATAL_ERROR "F3_PROFILE_TIERS and F3_PROFILE_SLIM are mutually exclusive")
endif()

set(F3_PROFILE_GENERATOR_ARGS)
set(F3_PROFILE_INPUTS)
foreach(mode TIERS SLIM)
    if(F3_PROFILE_${mode})
        get_filename_component(F3_PROFILE_${mode} "${F3_PROFILE_${mode}}" ABSOLUTE)
        if(NOT F3_ROM_DIR OR NOT EXISTS "${F3_PROFILE_${mode}}")
            message(FATAL_ERROR "F3_PROFILE_${mode} requires F3_ROM_DIR and an existing profile")
        endif()
        string(TOLOWER "${mode}" flag)
        list(APPEND F3_PROFILE_GENERATOR_ARGS "--profile-${flag}" "${F3_PROFILE_${mode}}")
        list(APPEND F3_PROFILE_INPUTS "${F3_PROFILE_${mode}}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${F3_PROFILE_${mode}}")
    endif()
endforeach()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(F3_PROFILE_COLD_OPT -Oz)
else()
    set(F3_PROFILE_COLD_OPT -Os)
endif()

if(F3_ROM_DIR)
    get_filename_component(F3_ROM_DIR "${F3_ROM_DIR}" ABSOLUTE)
    if(NOT F3_GENERATED_DIR)
        set(F3_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${F3_GAME}")
    endif()

    file(GLOB _rom_files LIST_DIRECTORIES false "${F3_ROM_DIR}/*")
    list(SORT _rom_files)
    set(_rom_identity "")
    foreach(_rf ${_rom_files})
        get_filename_component(_rname "${_rf}" NAME)
        file(SIZE "${_rf}" _rsize)
        file(TIMESTAMP "${_rf}" _rtime "%Y-%m-%dT%H:%M:%S" UTC)
        string(APPEND _rom_identity "${_rname}:${_rsize}:${_rtime};")
    endforeach()

    set(F3_RECOMP_EMIT_ARGS
        --config "${F3_GAME_CONFIG}"
        --rom-dir "${F3_ROM_DIR}"
        --output "${F3_GENERATED_DIR}"
        --config-output-dir "${F3_GENERATED_CONFIG_DIR}"
        ${F3_PROFILE_GENERATOR_ARGS}
    )
    if(F3_RECOMP_ALLOW_UNPROVEN_IRQ)
        list(APPEND F3_RECOMP_EMIT_ARGS --allow-unproven-irq)
    endif()
    f3_run_generator(
        STAMP_FILE "${F3_GENERATED_DIR}/recomp_emit.stamp"
        INPUT_FILES
            "${F3_GAME_CONFIG}"
            ${F3_PROFILE_INPUTS}
        EXTRA_DATA
            "${_rom_identity}"
            "${F3_RECOMP_EMIT_ARGS}"
        OUTPUT_FILES
            "${F3_GENERATED_DIR}/sources.cmake"
            "${F3_GENERATED_DIR}/program.c"
            "${F3_GENERATED_DIR}/program.h"
            "${F3_GENERATED_DIR}/program.bin"
            "${F3_GENERATED_DIR}/sprite_groups.hpp"
            "${F3_GENERATED_DIR}/sprite_groups.json"
            "${F3_GENERATED_DIR}/profile_inventory.json"
            "${F3_GENERATED_CONFIG_DIR}/enhancements.hpp"
        COMMAND emit ${F3_RECOMP_EMIT_ARGS}
    )
elseif(F3_GENERATED_DIR AND EXISTS "${F3_GENERATED_DIR}/enhancements.hpp")
    configure_file("${F3_GENERATED_DIR}/enhancements.hpp"
        "${F3_GENERATED_CONFIG_DIR}/enhancements.hpp" COPYONLY)
else()
    f3_run_generator(
        STAMP_FILE "${F3_GENERATED_CONFIG_DIR}/enhancements.stamp"
        INPUT_FILES "${F3_GAME_CONFIG}"
        OUTPUT_FILES "${F3_GENERATED_CONFIG_DIR}/enhancements.hpp"
        COMMAND enhancements --config "${F3_GAME_CONFIG}" --output-dir "${F3_GENERATED_CONFIG_DIR}"
    )
endif()
