cmake_minimum_required(VERSION 3.24)
project(f3rt VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(F3RT_SDL "Build SDL3 frontend" ON)
option(F3RT_GPU "Build SDL3 GPU presentation (offline SPIR-V/MSL shader tools required)" ON)
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
if(F3_GAME STREQUAL "landmakrj" OR F3_GAME STREQUAL "landmakr")
    set(F3_GAME_EXECUTABLE landmakr)
else()
    set(F3_GAME_EXECUTABLE "${F3_GAME}")
endif()
set(F3_GAME_EXECUTABLE "${F3_GAME_EXECUTABLE}" CACHE INTERNAL "Game executable name")
set(F3_ROM_DIR "" CACHE PATH "Selected game's ROM directory; enables automatic native generation")
set(F3_GENERATED_DIR "" CACHE PATH "Pre-generated main CPU C directory (optional)")
set(F3_SOUND_GENERATED_DIR "" CACHE PATH "Pre-generated sound CPU C directory (optional)")
find_program(UV_EXECUTABLE uv REQUIRED)
execute_process(COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
    python -c
    "import pathlib, sys, tomllib; p = pathlib.Path(sys.argv[1]); identity = tomllib.loads(p.read_text())['game']['id']; sys.exit(0 if identity == sys.argv[2] else 'Configured game id does not match directory: ' + str(p))"
    "${F3_GAME_CONFIG}" "${F3_GAME}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
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
        EXTRA_DATA ${ARG_EXTRA_DATA})

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
        COMMAND_ERROR_IS_FATAL ANY)

    get_filename_component(_sdir "${ARG_STAMP_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_sdir}")
    file(WRITE "${ARG_STAMP_FILE}" "${_computed_stamp}\n")
endfunction()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${F3_GAME_CONFIGS} "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/roms.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/roms.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock")
set(F3_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_config")
set(F3_COMPILE_ROMS_ARGS
    --configs ${F3_GAME_CONFIGS} --output "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
    --game "${F3_GAME}" --game-output "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp")
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
    COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
        python recomp/compile/roms.py ${F3_COMPILE_ROMS_ARGS})
# Emit-unit headers come from the config alone (no ROM); recomp emit reads the same
# config and checks the digest guard it plants against these headers.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/sprite_units.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/sprite_units.py")
set(F3_COMPILE_SPRITE_UNITS_ARGS
    --config "${F3_GAME_CONFIG}" --output-dir "${F3_GENERATED_CONFIG_DIR}")
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
    COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
        python recomp/compile/sprite_units.py ${F3_COMPILE_SPRITE_UNITS_ARGS})
option(F3_PROFILE_INSTRUMENT "Record every generated main/sound entry with --profile-out" OFF)
if(F3_PROFILE_INSTRUMENT AND NOT F3_ROM_DIR)
    message(FATAL_ERROR "F3_PROFILE_INSTRUMENT requires F3_ROM_DIR to regenerate instrumentable main/sound entries")
endif()
set(F3_PROFILE_TIERS "" CACHE FILEPATH "Execution profile for full-coverage hot -O2 / cold size-optimized code")
set(F3_PROFILE_SLIM "" CACHE FILEPATH "Explicit opt-in profile removing cold main/sound code; cold hits abort")
option(F3_PROFILE_DEFAULT_TIERS "Use the frozen full-coverage landmakrj tiers when generating from ROMs" ON)
if(F3_ROM_DIR AND F3_GAME STREQUAL "landmakrj" AND F3_PROFILE_DEFAULT_TIERS AND NOT F3_PROFILE_TIERS AND NOT F3_PROFILE_SLIM)
    set(F3_PROFILE_TIERS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/landmakrj.profile")
endif()
if(F3_PROFILE_TIERS AND F3_PROFILE_SLIM)
    message(FATAL_ERROR "F3_PROFILE_TIERS and F3_PROFILE_SLIM are mutually exclusive")
endif()
set(F3_PROFILE_GENERATOR_ARGS)
foreach(mode TIERS SLIM)
    if(F3_PROFILE_${mode})
        get_filename_component(F3_PROFILE_${mode} "${F3_PROFILE_${mode}}" ABSOLUTE)
        if(NOT F3_ROM_DIR OR NOT EXISTS "${F3_PROFILE_${mode}}")
            message(FATAL_ERROR "F3_PROFILE_${mode} requires F3_ROM_DIR and an existing profile")
        endif()
        string(TOLOWER "${mode}" flag)
        list(APPEND F3_PROFILE_GENERATOR_ARGS "--profile-${flag}" "${F3_PROFILE_${mode}}")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.csv")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${RECOMP_SOURCES} "${F3_GAME_CONFIG}")

    file(GLOB _rom_files LIST_DIRECTORIES false "${F3_ROM_DIR}/*")
    list(SORT _rom_files)
    set(_rom_identity "")
    foreach(_rf ${_rom_files})
        get_filename_component(_rname "${_rf}" NAME)
        file(SIZE "${_rf}" _rsize)
        file(TIMESTAMP "${_rf}" _rtime "%Y-%m-%dT%H:%M:%S" UTC)
        string(APPEND _rom_identity "${_rname}:${_rsize}:${_rtime};")
    endforeach()

    set(F3_PROFILE_INPUTS "")
    foreach(mode TIERS SLIM)
        if(F3_PROFILE_${mode})
            list(APPEND F3_PROFILE_INPUTS "${F3_PROFILE_${mode}}")
        endif()
    endforeach()

    set(F3_RECOMP_EMIT_ARGS
        --config "${F3_GAME_CONFIG}" --rom-dir "${F3_ROM_DIR}" --output "${F3_GENERATED_DIR}"
        ${F3_PROFILE_GENERATOR_ARGS})
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
        COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
            python -m recomp emit ${F3_RECOMP_EMIT_ARGS})

    if(NOT F3_SOUND_GENERATED_DIR)
        set(F3_SOUND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sound-${F3_GAME}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/sound.py")
    set(F3_COMPILE_SOUND_ARGS
        --rom-dir "${F3_ROM_DIR}" --output "${F3_SOUND_GENERATED_DIR}"
        --config "${F3_GAME_CONFIG}"
        ${F3_PROFILE_GENERATOR_ARGS})
    f3_run_generator(
        STAMP_FILE "${F3_SOUND_GENERATED_DIR}/compile_sound.stamp"
        INPUT_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/recomp/compile/sound.py"
            ${RECOMP_SOURCES}
            "${F3_GAME_CONFIG}"
            ${F3_PROFILE_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/uv.lock"
        EXTRA_DATA
            "${_rom_identity}"
            "${F3_COMPILE_SOUND_ARGS}"
        OUTPUT_FILES
            "${F3_SOUND_GENERATED_DIR}/sources.cmake"
            "${F3_SOUND_GENERATED_DIR}/sound_program.c"
            "${F3_SOUND_GENERATED_DIR}/sound_program.h"
        COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
            python recomp/compile/sound.py ${F3_COMPILE_SOUND_ARGS})
endif()

set(MUSASHI ${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/musashi)
add_executable(f3rt_m68kmake ${MUSASHI}/m68kmake.c)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/musashi)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/musashi/m68kops.c ${CMAKE_CURRENT_BINARY_DIR}/musashi/m68kops.h
    COMMAND f3rt_m68kmake ${CMAKE_CURRENT_BINARY_DIR}/musashi ${MUSASHI}/m68k_in.c
    DEPENDS f3rt_m68kmake ${MUSASHI}/m68k_in.c
    VERBATIM)
add_library(f3rt_musashi STATIC ${MUSASHI}/m68kcpu.c ${MUSASHI}/softfloat/softfloat.c
    ${CMAKE_CURRENT_BINARY_DIR}/musashi/m68kops.c runtime/core_state.c)
target_include_directories(f3rt_musashi PUBLIC ${MUSASHI} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/musashi runtime)
target_compile_definitions(f3rt_musashi PRIVATE M68K_EMULATE_030=0 M68K_EMULATE_040=0
    M68K_EMULATE_INT_ACK=1 M68K_EMULATE_TRACE=1 M68K_EMULATE_RESET=1 M68K_EMULATE_ADDRESS_ERROR=1)
# m68kcpu.c includes the generated header, not only the generated C file.
add_custom_target(f3rt_musashi_generated DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/musashi/m68kops.h)
add_dependencies(f3rt_musashi f3rt_musashi_generated)

find_package(ZLIB REQUIRED)
file(GLOB AUDIO_SOURCES CONFIGURE_DEPENDS runtime/third_party/audio/*.cpp)
# A game opts into the de-HLE'd GameVideo scene by providing games/<game>/video/.
file(GLOB GAME_VIDEO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/video/*.cpp")
if(GAME_VIDEO_SOURCES)
    set(F3RT_GAME_VIDEO ON)
    set(F3RT_GAME_VIDEO_SOURCES ${GAME_VIDEO_SOURCES})
else()
    # Games without a scene still link GameVideo (never instantiated for them).
    set(F3RT_GAME_VIDEO_SOURCES runtime/renderer/game/video_generic.cpp)
endif()
# A game opts into fast boot by providing games/<game>/boot/.
file(GLOB GAME_BOOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/boot/*.cpp")
if(GAME_BOOT_SOURCES)
    set(F3RT_GAME_BOOT_SOURCES ${GAME_BOOT_SOURCES})
else()
    # Games with no boot predicate never turbo-boot; the entry point still resolves.
    set(F3RT_GAME_BOOT_SOURCES runtime/game_boot_generic.cpp)
endif()
add_library(f3rt STATIC runtime/rom.cpp runtime/cpu_abi.cpp runtime/machine.cpp runtime/interpreter.cpp
    runtime/renderer/fdp/video.cpp runtime/renderer/game/video.cpp runtime/renderer/game/video_log.cpp runtime/renderer/decode.cpp
    runtime/renderer/game/tiles.cpp runtime/renderer/game/text.cpp
    runtime/renderer/game/sprites.cpp runtime/renderer/game/lines.cpp runtime/renderer/game/compositor.cpp ${F3RT_GAME_VIDEO_SOURCES} ${F3RT_GAME_BOOT_SOURCES} runtime/audio/audio.cpp
    runtime/audio/reference/engine.cpp
    runtime/audio/sound_trace.cpp runtime/audio/reference/native/sound_native.cpp runtime/debug/block_profile.cpp runtime/input.cpp
    runtime/audio/hle/audio.cpp runtime/audio/hle/sequencer.cpp runtime/audio/hle/synth.cpp runtime/audio/hle/voice_kernel.cpp
    runtime/sprites/units.cpp runtime/sprites/behaviours.cpp runtime/discovery_log.cpp
    ${AUDIO_SOURCES})
# Generated headers are reached as "generated_config/<name>"; runtime sprite headers
# use "sprites/<name>" to avoid basename ambiguity.
target_include_directories(f3rt PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}"
    PRIVATE runtime runtime/audio "${CMAKE_CURRENT_SOURCE_DIR}")
# A game opts into sprite behaviours by providing games/<game>/sprites/behaviours.hpp.
# Header-only registry: sprites/behaviours.cpp includes it, so no static registrar
# can be dead-stripped from the archive. Not under video/, which would enable GameVideo.
set(F3_SPRITE_BEHAVIOURS_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/sprites/behaviours.hpp")
if(EXISTS "${F3_SPRITE_BEHAVIOURS_HEADER}")
    target_compile_definitions(f3rt PRIVATE F3RT_SPRITE_BEHAVIOURS_HEADER="${F3_SPRITE_BEHAVIOURS_HEADER}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${F3_SPRITE_BEHAVIOURS_HEADER}")
endif()
find_package(Threads REQUIRED)
# Google Highway: runtime-dispatched SIMD for the HLE voice kernel.
find_package(hwy 1.0 CONFIG QUIET)
if(NOT TARGET hwy::hwy)
    include(FetchContent)
    set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(highway GIT_REPOSITORY https://github.com/google/highway.git GIT_TAG 1.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(highway)
    add_library(hwy::hwy ALIAS hwy)
endif()
target_link_libraries(f3rt PRIVATE f3rt_musashi Threads::Threads ZLIB::ZLIB hwy::hwy)
# Musashi calls the runtime bus; static consumers need both archives rescanned.
target_link_libraries(f3rt_musashi PRIVATE f3rt)
target_compile_options(f3rt PRIVATE -Wall -Wextra -Wpedantic)
if(F3RT_GAME_VIDEO)
    # GameVideo (non-FDP scene) is only built for games with games/<game>/video/.
    target_compile_definitions(f3rt PUBLIC F3RT_GAME_VIDEO=1)
endif()
if(F3_PROFILE_INSTRUMENT)
    target_compile_definitions(f3rt PUBLIC F3_PROFILE_INSTRUMENT=1)
endif()
if(F3_PROFILE_SLIM)
    target_compile_definitions(f3rt PUBLIC F3_PROFILE_SLIM_ENABLED=1)
endif()
if(F3_SOUND_GENERATED_DIR)
    include("${F3_SOUND_GENERATED_DIR}/sources.cmake")
    add_library(f3_sound_recompiled STATIC ${F3_SOUND_GENERATED_SOURCES})
    target_include_directories(f3_sound_recompiled PUBLIC
        include "${CMAKE_CURRENT_SOURCE_DIR}" "${F3_SOUND_GENERATED_DIR}")
    target_link_libraries(f3_sound_recompiled PUBLIC f3rt)
    if(F3_PROFILE_TIERS)
        set_source_files_properties(${F3_SOUND_GENERATED_HOT_SOURCES} PROPERTIES COMPILE_OPTIONS -O2)
        if(F3_SOUND_GENERATED_COLD_SOURCES)
            set_source_files_properties(${F3_SOUND_GENERATED_COLD_SOURCES} PROPERTIES COMPILE_OPTIONS "${F3_PROFILE_COLD_OPT}")
        endif()
    endif()
endif()
add_library(f3rt-runner STATIC tools/runner/runner.cpp tools/runner/inputs.cpp tools/runner/sprite_check.cpp)
target_include_directories(f3rt-runner PUBLIC tools/runner runtime include "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(f3rt-runner PUBLIC f3rt)
if(F3_ROM_DIR)
    target_compile_definitions(f3rt-runner PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
endif()
target_compile_definitions(f3rt-runner PRIVATE F3RT_DEFAULT_SET="${F3_GAME}")

add_executable(f3rt-tool tools/tool.cpp tools/commands/gameplay.cpp tools/commands/sound_extract.cpp tools/commands/motion.cpp tools/commands/gpu_compare.cpp tools/commands/sprite_check.cpp)
target_include_directories(f3rt-tool PRIVATE tools tools/runner tools/commands runtime include "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(f3rt-tool PRIVATE f3rt f3rt-runner)
target_compile_definitions(f3rt-tool PRIVATE F3RT_DEFAULT_SET="${F3_GAME}")
if(F3_ROM_DIR)
    target_compile_definitions(f3rt-tool PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
endif()
if(F3_GENERATED_DIR)
    add_subdirectory(recomp)
    target_link_libraries(f3rt-runner PRIVATE f3_recompiled)
    target_compile_definitions(f3rt-runner PRIVATE F3RT_GENERATED=1)
    target_link_libraries(f3rt-tool PRIVATE f3_recompiled)
    target_compile_definitions(f3rt-tool PRIVATE F3RT_GENERATED=1)
endif()
if(F3RT_SDL)
    find_package(SDL3 CONFIG REQUIRED)
    set(IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/imgui")
    add_library(f3rt-ui STATIC runtime/frontend/settings.cpp runtime/frontend/ui.cpp
        ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(f3rt-ui PUBLIC runtime runtime/frontend include PRIVATE runtime ${IMGUI} ${IMGUI}/backends)
    target_link_libraries(f3rt-ui PUBLIC SDL3::SDL3)
    if(F3_SOUND_GENERATED_DIR)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_SOUND_GENERATED=1)
    endif()
    add_executable(f3rt-run runtime/frontend/frontend.cpp)
    target_include_directories(f3rt-run PRIVATE runtime)
    target_link_libraries(f3rt-run PRIVATE f3rt f3rt-ui SDL3::SDL3)
    target_compile_definitions(f3rt-run PRIVATE F3RT_DEFAULT_SET="${F3_GAME}")
    if(F3_GENERATED_DIR)
        target_link_libraries(f3rt-run PRIVATE f3_recompiled)
        target_compile_definitions(f3rt-run PRIVATE F3RT_GENERATED=1)
        add_executable(${F3_GAME_EXECUTABLE} runtime/frontend/frontend.cpp)
        target_include_directories(${F3_GAME_EXECUTABLE} PRIVATE runtime)
        target_link_libraries(${F3_GAME_EXECUTABLE} PRIVATE f3rt f3_recompiled f3rt-ui SDL3::SDL3)
        target_compile_definitions(${F3_GAME_EXECUTABLE} PRIVATE
            F3RT_GENERATED=1 F3RT_GAME=1 F3RT_DEFAULT_SET="${F3_GAME}" F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
    if(F3RT_GPU)
        target_sources(f3rt-ui PRIVATE ${IMGUI}/backends/imgui_impl_sdlgpu3.cpp)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_GPU=1)
        find_program(F3_GLSLANG glslangValidator REQUIRED)
        find_program(F3_SPIRV_CROSS spirv-cross REQUIRED)
        set(VIDEO_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/video_shaders.hpp")
        add_custom_command(OUTPUT "${VIDEO_SHADER_HEADER}"
            COMMAND ${UV_EXECUTABLE} run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups
                python "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders/compile.py"
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders"
                --output "${VIDEO_SHADER_HEADER}" --glslang "${F3_GLSLANG}" --spirv-cross "${F3_SPIRV_CROSS}"
            DEPENDS runtime/renderer/shaders/compile.py runtime/renderer/shaders/scene.glsl
                runtime/renderer/shaders/fullscreen.vert runtime/renderer/shaders/sprite.vert
                runtime/renderer/shaders/sprite.frag runtime/renderer/shaders/scene.frag
                runtime/renderer/shaders/scene_interp.frag runtime/renderer/shaders/scene_body.glsl
                runtime/renderer/shaders/postprocess.frag runtime/renderer/gpu/scene_layout.h
            VERBATIM)
        add_library(f3rt-gpu STATIC runtime/renderer/gpu/video.cpp runtime/renderer/gpu/encode.cpp runtime/renderer/gpu/interp.cpp runtime/renderer/gpu/motion.cpp "${VIDEO_SHADER_HEADER}")
        if(APPLE)
            enable_language(OBJCXX)
            target_sources(f3rt-gpu PRIVATE runtime/renderer/gpu/pacing_macos.mm)
            set_source_files_properties(runtime/renderer/gpu/pacing_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            target_link_libraries(f3rt-gpu PRIVATE "-framework AppKit" "-framework QuartzCore")
        endif()
        # The build dir carries generated_config/ (compile-time game video geometry).
        target_include_directories(f3rt-gpu PUBLIC include runtime runtime/frontend "${CMAKE_CURRENT_BINARY_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(f3rt-gpu PUBLIC SDL3::SDL3)
        target_compile_options(f3rt-gpu PRIVATE -Wall -Wextra -Wpedantic)
        foreach(frontend f3rt-tool f3rt-run ${F3_GAME_EXECUTABLE})
            if(TARGET ${frontend})
                target_link_libraries(${frontend} PRIVATE f3rt-gpu SDL3::SDL3)
                target_compile_definitions(${frontend} PRIVATE F3RT_GPU=1)
            endif()
        endforeach()
    endif()
endif()
if(F3_SOUND_GENERATED_DIR)
    foreach(frontend f3rt-tool f3rt-run ${F3_GAME_EXECUTABLE})
        if(TARGET ${frontend})
            target_link_libraries(${frontend} PRIVATE f3_sound_recompiled)
            target_compile_definitions(${frontend} PRIVATE F3RT_SOUND_GENERATED=1)
        endif()
    endforeach()
    if(TARGET f3rt-runner)
        target_link_libraries(f3rt-runner PRIVATE f3_sound_recompiled)
        target_compile_definitions(f3rt-runner PRIVATE F3RT_SOUND_GENERATED=1)
    endif()
endif()
add_executable(f3rt-replay runtime/replay.cpp)
target_link_libraries(f3rt-replay PRIVATE f3rt)
include(CTest)
if(BUILD_TESTING)
    add_test(NAME game-geometry-literals COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_game_geometry_literals.cmake)
    find_package(GTest QUIET)
    if(NOT TARGET GTest::gtest)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.18.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()
    include(GoogleTest)

    find_path(RAPIDCHECK_GTEST_INCLUDE_DIR "rapidcheck/gtest.h")
    if(RAPIDCHECK_GTEST_INCLUDE_DIR)
        find_package(rapidcheck CONFIG QUIET)
        if(TARGET rapidcheck AND NOT TARGET rapidcheck_gtest)
            add_library(rapidcheck_gtest INTERFACE)
            target_link_libraries(rapidcheck_gtest INTERFACE rapidcheck)
            target_include_directories(rapidcheck_gtest INTERFACE "${RAPIDCHECK_GTEST_INCLUDE_DIR}")
        endif()
    endif()
    if(NOT TARGET rapidcheck_gtest)
        include(FetchContent)
        set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
        set(RC_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            rapidcheck
            GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
            GIT_TAG 2c3c4365aca21ef4e612768fdc95b1ce8b39a651
        )
        FetchContent_MakeAvailable(rapidcheck)
    endif()

    add_library(f3rt-test-support STATIC runtime/tests/support.cpp runtime/renderer/gpu/motion.cpp)
    target_link_libraries(f3rt-test-support PUBLIC f3rt f3rt_musashi f3rt-runner GTest::gtest rapidcheck_gtest)
    target_include_directories(f3rt-test-support PUBLIC runtime runtime/tests "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(f3rt-test-support PUBLIC F3RT_DEFAULT_SET="${F3_GAME}")
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-test-support PUBLIC F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
    foreach(area video input eeprom cpu sprites audio motion_interp state)
        add_executable(f3rt-test-${area} runtime/tests/${area}.cpp)
        target_link_libraries(f3rt-test-${area} PRIVATE f3rt-test-support GTest::gtest_main)
        gtest_discover_tests(f3rt-test-${area} TEST_PREFIX "runtime-${area}.")
    endforeach()
    if(TARGET f3_recompiled)
        target_link_libraries(f3rt-test-sprites PRIVATE f3_recompiled)
        target_compile_definitions(f3rt-test-sprites PRIVATE F3RT_GENERATED=1)
    endif()
    # Emit-unit replay check (needs the game ROMs and a game that declares emit units).
    file(READ "${F3_GAME_CONFIG}" F3_GAME_CONFIG_TEXT)
    string(FIND "${F3_GAME_CONFIG_TEXT}" "[[video.emit_units]]" F3_EMIT_UNITS_POS)
    if(F3_ROM_DIR AND NOT F3_EMIT_UNITS_POS EQUAL -1)
        add_test(NAME sprite-units COMMAND f3rt-test-sprites --gtest_filter=SpriteUnits.RomReplayAndInvariants)
    endif()
    if(F3_ROM_DIR AND F3_GAME STREQUAL "landmakrj")
        add_test(NAME hle-audio COMMAND f3rt-test-audio --gtest_filter=HleAudio.LandmakrStateAndSequencing)
    endif()
    if(TARGET f3rt-ui)
        add_executable(f3rt-test-frontend runtime/tests/frontend.cpp)
        target_link_libraries(f3rt-test-frontend PRIVATE f3rt-test-support f3rt-ui GTest::gtest_main)
        gtest_discover_tests(f3rt-test-frontend TEST_PREFIX "runtime-frontend.")
    endif()
endif()
