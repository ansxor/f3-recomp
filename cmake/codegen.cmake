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

find_program(UV_EXECUTABLE uv REQUIRED)
set(F3_UV_PYTHON "${UV_EXECUTABLE}" run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups python)

execute_process(
    COMMAND ${F3_UV_PYTHON} -c
        "import pathlib, sys, tomllib; p = pathlib.Path(sys.argv[1]); identity = tomllib.loads(p.read_text())['game']['id']; sys.exit(0 if identity == sys.argv[2] else 'Configured game id does not match directory: ' + str(p))"
        "${F3_GAME_CONFIG}" "${F3_GAME}"
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
        INPUT_FILES ${ARG_INPUT_FILES}
        EXTRA_DATA ${ARG_EXTRA_DATA}
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
        COMMAND ${ARG_COMMAND}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY
    )

    get_filename_component(_sdir "${ARG_STAMP_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_sdir}")
    file(WRITE "${ARG_STAMP_FILE}" "${_computed_stamp}\n")
endfunction()

set(F3_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_config")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${F3_GAME_CONFIGS}
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/roms.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/roms.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock"
)
set(F3_COMPILE_ROMS_ARGS
    --configs ${F3_GAME_CONFIGS}
    --output "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
    --game "${F3_GAME}"
    --game-output "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp"
)
f3_run_generator(
    STAMP_FILE "${F3_GENERATED_CONFIG_DIR}/compile_roms.stamp"
    INPUT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/roms.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/roms.py"
        ${F3_GAME_CONFIGS}
        "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock"
    EXTRA_DATA "${F3_COMPILE_ROMS_ARGS}"
    OUTPUT_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
        "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp"
    COMMAND ${F3_UV_PYTHON} recomp/compile/roms.py ${F3_COMPILE_ROMS_ARGS}
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/sprite_units.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/sprite_units.py"
)
set(F3_COMPILE_SPRITE_UNITS_ARGS
    --config "${F3_GAME_CONFIG}"
    --output-dir "${F3_GENERATED_CONFIG_DIR}"
)
f3_run_generator(
    STAMP_FILE "${F3_GENERATED_CONFIG_DIR}/compile_sprite_units.stamp"
    INPUT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/sprite_units.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/sprite_units.py"
        "${F3_GAME_CONFIG}"
        "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock"
    EXTRA_DATA "${F3_COMPILE_SPRITE_UNITS_ARGS}"
    OUTPUT_FILES
        "${F3_GENERATED_CONFIG_DIR}/sprite_units.h"
        "${F3_GENERATED_CONFIG_DIR}/sprite_units.hpp"
    COMMAND ${F3_UV_PYTHON} recomp/compile/sprite_units.py ${F3_COMPILE_SPRITE_UNITS_ARGS}
)

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

    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.csv"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${RECOMP_SOURCES} "${F3_GAME_CONFIG}"
    )

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
        ${F3_PROFILE_GENERATOR_ARGS}
    )
    f3_run_generator(
        STAMP_FILE "${F3_GENERATED_DIR}/recomp_emit.stamp"
        INPUT_FILES
            ${RECOMP_SOURCES}
            "${F3_GAME_CONFIG}"
            ${F3_PROFILE_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock"
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
        COMMAND ${F3_UV_PYTHON} -m recomp emit ${F3_RECOMP_EMIT_ARGS}
    )
endif()
