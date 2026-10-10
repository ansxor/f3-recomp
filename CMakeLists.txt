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
set(F3_ROM_DIR "" CACHE PATH "Selected game's ROM directory; enables automatic native generation")
set(F3_GENERATED_DIR "" CACHE PATH "Pre-generated main CPU C directory (optional)")
set(F3_SOUND_GENERATED_DIR "" CACHE PATH "Pre-generated sound CPU C directory (optional)")
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} -c
    "import pathlib, sys, tomllib; p = pathlib.Path(sys.argv[1]); identity = tomllib.loads(p.read_text())['game']['id']; sys.exit(0 if identity == sys.argv[2] else 'Configured game id does not match directory: ' + str(p))"
    "${F3_GAME_CONFIG}" "${F3_GAME}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${F3_GAME_CONFIGS} "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_roms.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/roms.py")
set(F3_GENERATED_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_config")
execute_process(COMMAND ${Python3_EXECUTABLE} tools/compile_roms.py
    --configs ${F3_GAME_CONFIGS} --output "${CMAKE_CURRENT_BINARY_DIR}/rom_manifest.hpp"
    --game "${F3_GAME}" --game-output "${F3_GENERATED_CONFIG_DIR}/game_video_config.hpp"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
# Emit-unit headers come from the config alone (no ROM); recomp emit reads the same
# config and checks the digest guard it plants against these headers.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_sprite_units.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/sprite_units.py")
execute_process(COMMAND ${Python3_EXECUTABLE} tools/compile_sprite_units.py
    --config "${F3_GAME_CONFIG}" --output-dir "${F3_GENERATED_CONFIG_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
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
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT F3_GENERATED_DIR)
        set(F3_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${F3_GAME}")
    endif()
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.csv")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${RECOMP_SOURCES} "${F3_GAME_CONFIG}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:$ENV{PYTHONPATH}"
            ${Python3_EXECUTABLE} -m recomp emit
            --config "${F3_GAME_CONFIG}" --rom-dir "${F3_ROM_DIR}" --output "${F3_GENERATED_DIR}"
            ${F3_PROFILE_GENERATOR_ARGS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT F3_SOUND_GENERATED_DIR)
        set(F3_SOUND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sound-${F3_GAME}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_sound.py")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:$ENV{PYTHONPATH}"
            ${Python3_EXECUTABLE} tools/compile_sound.py
            --rom-dir "${F3_ROM_DIR}" --output "${F3_SOUND_GENERATED_DIR}"
            --config "${F3_GAME_CONFIG}"
            ${F3_PROFILE_GENERATOR_ARGS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
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
target_include_directories(f3rt_musashi PUBLIC ${MUSASHI} PRIVATE include ${CMAKE_CURRENT_BINARY_DIR}/musashi)
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
    runtime/renderer/game/sprites.cpp runtime/renderer/game/lines.cpp runtime/renderer/game/compositor.cpp ${F3RT_GAME_VIDEO_SOURCES} ${F3RT_GAME_BOOT_SOURCES} runtime/audio.cpp
    runtime/sound_trace.cpp runtime/sound_native.cpp runtime/block_profile.cpp runtime/input.cpp runtime/netplay.cpp
    runtime/hle_audio.cpp runtime/hle_sequencer.cpp runtime/hle_synth.cpp runtime/hle_voice_kernel.cpp
    runtime/netplay_transport.cpp runtime/netplay_session.cpp runtime/sprite_units.cpp runtime/sprite_behaviours.cpp runtime/discovery_log.cpp
    ${AUDIO_SOURCES})
# Generated headers are reached as "generated_config/<name>": runtime/sprite_units.hpp
# and generated_config/sprite_units.hpp share a basename.
target_include_directories(f3rt PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}"
    PRIVATE runtime "${CMAKE_CURRENT_SOURCE_DIR}")
# A game opts into sprite behaviours by providing games/<game>/sprites/behaviours.hpp.
# Header-only registry: sprite_behaviours.cpp includes it, so no static registrar
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
add_executable(f3rt-sound-extract tools/sound_extract.cpp)
target_include_directories(f3rt-sound-extract PRIVATE runtime)
target_link_libraries(f3rt-sound-extract PRIVATE f3rt)
target_compile_definitions(f3rt-sound-extract PRIVATE F3RT_DEFAULT_SET="${F3_GAME}")
add_executable(f3rt-hle-check tools/hle_audio_check.cpp)
target_include_directories(f3rt-hle-check PRIVATE runtime)
target_link_libraries(f3rt-hle-check PRIVATE f3rt)
if(F3_GENERATED_DIR)
    add_subdirectory(recomp)
    add_executable(f3rt-sprite-check tools/sprite_check.cpp)
    target_link_libraries(f3rt-sprite-check PRIVATE f3rt f3_recompiled)
    target_include_directories(f3rt-sprite-check PRIVATE runtime)
    target_compile_definitions(f3rt-sprite-check PRIVATE F3RT_GENERATED=1 F3RT_DEFAULT_SET="${F3_GAME}")
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-sprite-check PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
    if(F3_GAME STREQUAL "landmakrj")
    add_executable(f3rt-gameplay-regression tools/gameplay_regression.cpp)
    target_link_libraries(f3rt-gameplay-regression PRIVATE f3rt f3_recompiled)
    target_include_directories(f3rt-gameplay-regression PRIVATE runtime)
    target_compile_definitions(f3rt-gameplay-regression PRIVATE F3RT_GENERATED=1)
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-gameplay-regression PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
    add_executable(f3rt-netplay-oracle tools/netplay_oracle.cpp tools/sync_snapshot_proof.cpp)
    target_link_libraries(f3rt-netplay-oracle PRIVATE f3rt f3_recompiled)
    target_include_directories(f3rt-netplay-oracle PRIVATE runtime tools)
    target_compile_definitions(f3rt-netplay-oracle PRIVATE
        F3RT_GENERATED=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
endif()
if(F3RT_SDL)
    find_package(SDL3 CONFIG REQUIRED)
    set(IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/imgui")
    add_library(f3rt-ui STATIC runtime/frontend_settings.cpp runtime/frontend_ui.cpp
        ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(f3rt-ui PUBLIC runtime include PRIVATE ${IMGUI} ${IMGUI}/backends)
    target_link_libraries(f3rt-ui PUBLIC SDL3::SDL3)
    if(F3_SOUND_GENERATED_DIR)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_SOUND_GENERATED=1)
    endif()
    add_executable(f3rt-run runtime/frontend.cpp)
    target_link_libraries(f3rt-run PRIVATE f3rt f3rt-ui SDL3::SDL3)
    target_compile_definitions(f3rt-run PRIVATE F3RT_DEFAULT_SET="${F3_GAME}")
    if(F3_GENERATED_DIR)
        target_link_libraries(f3rt-run PRIVATE f3_recompiled)
        target_compile_definitions(f3rt-run PRIVATE F3RT_GENERATED=1)
        add_executable(${F3_GAME_EXECUTABLE} runtime/frontend.cpp)
        target_link_libraries(${F3_GAME_EXECUTABLE} PRIVATE f3rt f3_recompiled f3rt-ui SDL3::SDL3)
        target_compile_definitions(${F3_GAME_EXECUTABLE} PRIVATE
            F3RT_GENERATED=1 F3RT_GAME=1 F3RT_DEFAULT_SET="${F3_GAME}" F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
    if(F3RT_GPU)
        target_sources(f3rt-ui PRIVATE ${IMGUI}/backends/imgui_impl_sdlgpu3.cpp)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_GPU=1)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(F3_GLSLANG glslangValidator REQUIRED)
        find_program(F3_SPIRV_CROSS spirv-cross REQUIRED)
        set(VIDEO_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/video_shaders.hpp")
        add_custom_command(OUTPUT "${VIDEO_SHADER_HEADER}"
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_video_shaders.py"
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders"
                --output "${VIDEO_SHADER_HEADER}" --glslang "${F3_GLSLANG}" --spirv-cross "${F3_SPIRV_CROSS}"
            DEPENDS tools/compile_video_shaders.py runtime/renderer/shaders/scene.glsl
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
        target_include_directories(f3rt-gpu PUBLIC include runtime "${CMAKE_CURRENT_BINARY_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(f3rt-gpu PUBLIC SDL3::SDL3)
        target_compile_options(f3rt-gpu PRIVATE -Wall -Wextra -Wpedantic)
        foreach(frontend f3rt-run ${F3_GAME_EXECUTABLE})
            if(TARGET ${frontend})
                target_link_libraries(${frontend} PRIVATE f3rt-gpu)
                target_compile_definitions(${frontend} PRIVATE F3RT_GPU=1)
            endif()
        endforeach()
        if(F3_GENERATED_DIR AND F3RT_GAME_VIDEO)
            add_executable(f3rt-gpu-regression tools/gpu_video_regression.cpp)
            target_link_libraries(f3rt-gpu-regression PRIVATE f3rt f3_recompiled f3rt-gpu)
            target_compile_definitions(f3rt-gpu-regression PRIVATE
                F3RT_GENERATED=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
        endif()
        if(F3_GENERATED_DIR AND F3_GAME STREQUAL "landmakrj")
            add_executable(f3rt-motion-regression tools/motion_interp_regression.cpp)
            target_link_libraries(f3rt-motion-regression PRIVATE f3rt f3_recompiled f3rt-gpu)
            # runtime/ first: f3_recompiled exports generated_config, whose sprite_units.hpp
            # shares the runtime header's name.
            target_include_directories(f3rt-motion-regression PRIVATE runtime)
            target_compile_definitions(f3rt-motion-regression PRIVATE
                F3RT_GENERATED=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
        endif()
    endif()
endif()
if(F3_SOUND_GENERATED_DIR)
    foreach(frontend f3rt-sound-extract f3rt-gameplay-regression f3rt-netplay-oracle f3rt-run ${F3_GAME_EXECUTABLE} f3rt-gpu-regression f3rt-motion-regression)
        if(TARGET ${frontend})
            target_link_libraries(${frontend} PRIVATE f3_sound_recompiled)
            target_compile_definitions(${frontend} PRIVATE F3RT_SOUND_GENERATED=1)
        endif()
    endforeach()
endif()
add_executable(f3rt-replay runtime/replay.cpp)
target_link_libraries(f3rt-replay PRIVATE f3rt)
include(CTest)
if(BUILD_TESTING)
    add_executable(f3rt-motion-check runtime/motion_interp_check.cpp runtime/renderer/gpu/motion.cpp
        runtime/renderer/game/text.cpp runtime/renderer/game/tiles.cpp runtime/renderer/decode.cpp)
    target_include_directories(f3rt-motion-check PRIVATE include runtime "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME motion-interpolation-guards COMMAND f3rt-motion-check)
    add_test(NAME game-geometry-literals COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_game_geometry_literals.cmake)
    add_library(f3rt-test-support STATIC runtime/tests/support.cpp)
    target_link_libraries(f3rt-test-support PUBLIC f3rt)
    target_include_directories(f3rt-test-support PUBLIC runtime runtime/tests)
    foreach(area video input eeprom cpu sprite_units audio)
        add_executable(f3rt-test-${area} runtime/tests/${area}.cpp)
        target_link_libraries(f3rt-test-${area} PRIVATE f3rt-test-support)
        add_test(NAME runtime-${area} COMMAND f3rt-test-${area})
    endforeach()
    # Emit-unit replay check (needs the game ROMs and a game that declares emit units).
    file(READ "${F3_GAME_CONFIG}" F3_GAME_CONFIG_TEXT)
    string(FIND "${F3_GAME_CONFIG_TEXT}" "[[video.emit_units]]" F3_EMIT_UNITS_POS)
    if(TARGET f3rt-sprite-check AND F3_ROM_DIR AND NOT F3_EMIT_UNITS_POS EQUAL -1)
        add_test(NAME sprite-units COMMAND f3rt-sprite-check --rom-dir "${F3_ROM_DIR}" --frames 6000)
    endif()
    if(F3_ROM_DIR AND F3_GAME STREQUAL "landmakrj")
        add_test(NAME hle-audio COMMAND f3rt-hle-check "${F3_ROM_DIR}")
    endif()
    add_executable(f3rt-state-check runtime/state_check.cpp)
    target_link_libraries(f3rt-state-check PRIVATE f3rt_musashi f3rt)
    add_test(NAME snapshot-validation COMMAND f3rt-state-check)
    if(TARGET f3rt-ui)
        add_executable(f3rt-frontend-check runtime/frontend_check.cpp)
        target_link_libraries(f3rt-frontend-check PRIVATE f3rt-ui)
        add_test(NAME frontend-input COMMAND f3rt-frontend-check)
    endif()
endif()

# Content identity, not a branch name or dirty-tree-blind git revision. Different
# compiler/platform/options or generated code must not silently join a match.
file(GLOB_RECURSE NETPLAY_IDENTITY_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    include/*.h include/*.hpp runtime/*.c runtime/*.h runtime/*.cpp runtime/*.hpp
    recomp/*.py recomp/*.h recomp/*.csv games/*.toml games/*.hpp)
list(APPEND NETPLAY_IDENTITY_FILES CMakeLists.txt tools/compile_sound.py tools/compile_roms.py
    tools/compile_sprite_units.py tools/netplay_build_id.cmake)
list(SORT NETPLAY_IDENTITY_FILES)
set(NETPLAY_IDENTITY "${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_SIZEOF_VOID_P};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_CXX_FLAGS};${CMAKE_C_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_RELEASE}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(APPEND NETPLAY_IDENTITY ";${config}:${CMAKE_C_FLAGS_${config}};${CMAKE_CXX_FLAGS_${config}}")
endforeach()
string(APPEND NETPLAY_IDENTITY ";${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_DEPLOYMENT_TARGET};${CMAKE_SYSROOT}")
if(F3_PROFILE_SLIM)
    set(F3_PROFILE_MODE slim)
elseif(F3_PROFILE_TIERS)
    set(F3_PROFILE_MODE tiers)
else()
    set(F3_PROFILE_MODE full)
endif()
string(APPEND NETPLAY_IDENTITY ";game:${F3_GAME};profile:${F3_PROFILE_INSTRUMENT};${F3_PROFILE_MODE};${F3_PROFILE_COLD_OPT}")
set(NETPLAY_IDENTITY_DEPENDS)
foreach(source IN LISTS NETPLAY_IDENTITY_FILES)
    list(APPEND NETPLAY_IDENTITY_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()
foreach(generated_dir IN ITEMS "${F3_GENERATED_DIR}" "${F3_SOUND_GENERATED_DIR}")
    if(generated_dir)
        file(GLOB generated_files "${generated_dir}/*.c" "${generated_dir}/*.h")
        list(APPEND NETPLAY_IDENTITY_DEPENDS ${generated_files})
    endif()
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/netplay_build.hpp"
    COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DSOURCES=${NETPLAY_IDENTITY_FILES}" "-DCOMPILER_ID=${NETPLAY_IDENTITY}"
        "-DGENERATED_DIR=${F3_GENERATED_DIR}" "-DSOUND_GENERATED_DIR=${F3_SOUND_GENERATED_DIR}"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/netplay_build.hpp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/netplay_build_id.cmake"
    DEPENDS ${NETPLAY_IDENTITY_DEPENDS}
    VERBATIM)
target_sources(f3rt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/netplay_build.hpp")
