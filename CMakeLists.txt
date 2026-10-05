cmake_minimum_required(VERSION 3.24)
project(f3rt VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(F3RT_SDL "Build SDL3 frontend" ON)
option(F3RT_GPU "Build SDL3 GPU presentation (offline SPIR-V/MSL shader tools required)" ON)
set(F3_ROM_DIR "" CACHE PATH "Land Maker Japan ROM directory; enables automatic native generation")
set(F3_GENERATED_DIR "" CACHE PATH "Pre-generated Land Maker C directory (optional)")
set(F3_SOUND_GENERATED_DIR "" CACHE PATH "Pre-generated Land Maker sound driver C directory (optional)")
option(F3_PROFILE_INSTRUMENT "Record every generated main/sound entry with --profile-out" OFF)
if(F3_PROFILE_INSTRUMENT AND NOT F3_ROM_DIR)
    message(FATAL_ERROR "F3_PROFILE_INSTRUMENT requires F3_ROM_DIR to regenerate instrumentable main/sound entries")
endif()
set(F3_PROFILE_TIERS "" CACHE FILEPATH "Execution profile for full-coverage hot -O2 / cold size-optimized code")
set(F3_PROFILE_SLIM "" CACHE FILEPATH "Explicit opt-in profile removing cold main/sound code; cold hits abort")
option(F3_PROFILE_DEFAULT_TIERS "Use the frozen full-coverage landmakrj tiers when generating from ROMs" ON)
if(F3_ROM_DIR AND F3_PROFILE_DEFAULT_TIERS AND NOT F3_PROFILE_TIERS AND NOT F3_PROFILE_SLIM)
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
        set(F3_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/landmakrj")
    endif()
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/recomp/*.csv")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${RECOMP_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/games/landmakrj/config.toml")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:$ENV{PYTHONPATH}"
            ${Python3_EXECUTABLE} -m recomp emit
            --config games/landmakrj/config.toml --rom-dir "${F3_ROM_DIR}" --output "${F3_GENERATED_DIR}"
            ${F3_PROFILE_GENERATOR_ARGS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT F3_SOUND_GENERATED_DIR)
        set(F3_SOUND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sound-landmakrj")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_sound.py")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:$ENV{PYTHONPATH}"
            ${Python3_EXECUTABLE} tools/compile_sound.py
            --rom-dir "${F3_ROM_DIR}" --output "${F3_SOUND_GENERATED_DIR}"
            --config games/landmakrj/config.toml
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
add_library(f3rt STATIC runtime/rom.cpp runtime/cpu_abi.cpp runtime/machine.cpp runtime/interpreter.cpp
    runtime/video.cpp runtime/game_video.cpp runtime/game_tiles.cpp runtime/game_text.cpp
    runtime/game_sprites.cpp runtime/game_lines.cpp runtime/game_compositor.cpp runtime/audio.cpp
    runtime/sound_trace.cpp runtime/sound_native.cpp runtime/block_profile.cpp runtime/netplay.cpp
    runtime/netplay_transport.cpp runtime/netplay_session.cpp ${AUDIO_SOURCES})
target_include_directories(f3rt PUBLIC include PRIVATE runtime "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(f3rt PRIVATE f3rt_musashi ZLIB::ZLIB)
target_compile_options(f3rt PRIVATE -Wall -Wextra -Wpedantic)
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
if(F3_GENERATED_DIR)
    add_subdirectory(recomp)
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
if(F3RT_SDL)
    find_package(SDL3 CONFIG REQUIRED)
    set(IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/imgui")
    add_library(f3rt-ui STATIC runtime/frontend_settings.cpp runtime/frontend_ui.cpp
        ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(f3rt-ui PUBLIC runtime PRIVATE ${IMGUI} ${IMGUI}/backends)
    target_link_libraries(f3rt-ui PUBLIC SDL3::SDL3)
    if(F3_SOUND_GENERATED_DIR)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_SOUND_GENERATED=1)
    endif()
    add_executable(f3rt-run runtime/frontend.cpp)
    target_link_libraries(f3rt-run PRIVATE f3rt f3rt-ui SDL3::SDL3)
    if(F3_GENERATED_DIR)
        target_link_libraries(f3rt-run PRIVATE f3_recompiled)
        target_compile_definitions(f3rt-run PRIVATE F3RT_GENERATED=1)
        add_executable(landmakr runtime/frontend.cpp)
        target_link_libraries(landmakr PRIVATE f3rt f3_recompiled f3rt-ui SDL3::SDL3)
        target_compile_definitions(landmakr PRIVATE
            F3RT_GENERATED=1 F3RT_LANDMAKR=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
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
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/shaders"
                --output "${VIDEO_SHADER_HEADER}" --glslang "${F3_GLSLANG}" --spirv-cross "${F3_SPIRV_CROSS}"
            DEPENDS tools/compile_video_shaders.py runtime/shaders/scene.glsl
                runtime/shaders/fullscreen.vert runtime/shaders/sprite.vert
                runtime/shaders/sprite.frag runtime/shaders/scene.frag
                runtime/shaders/scene_interp.frag runtime/shaders/scene_body.glsl
                runtime/shaders/postprocess.frag
            VERBATIM)
        add_library(f3rt-gpu STATIC runtime/gpu_video.cpp runtime/gpu_interp.cpp "${VIDEO_SHADER_HEADER}")
        target_include_directories(f3rt-gpu PUBLIC include runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(f3rt-gpu PUBLIC SDL3::SDL3)
        target_compile_options(f3rt-gpu PRIVATE -Wall -Wextra -Wpedantic)
        foreach(frontend f3rt-run landmakr)
            if(TARGET ${frontend})
                target_link_libraries(${frontend} PRIVATE f3rt-gpu)
                target_compile_definitions(${frontend} PRIVATE F3RT_GPU=1)
            endif()
        endforeach()
        if(F3_GENERATED_DIR)
            add_executable(f3rt-gpu-regression tools/gpu_video_regression.cpp)
            target_link_libraries(f3rt-gpu-regression PRIVATE f3rt f3_recompiled f3rt-gpu)
            target_compile_definitions(f3rt-gpu-regression PRIVATE
                F3RT_GENERATED=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
        endif()
    endif()
endif()
if(F3_SOUND_GENERATED_DIR)
    foreach(frontend f3rt-sound-extract f3rt-gameplay-regression f3rt-netplay-oracle f3rt-run landmakr f3rt-gpu-regression)
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
    add_executable(f3rt-check runtime/check.cpp)
    target_link_libraries(f3rt-check PRIVATE f3rt)
    add_test(NAME runtime-devices COMMAND f3rt-check)
    add_executable(f3rt-state-check runtime/state_check.cpp)
    target_link_libraries(f3rt-state-check PRIVATE f3rt)
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
    recomp/*.py recomp/*.h recomp/*.csv games/*.toml)
list(APPEND NETPLAY_IDENTITY_FILES CMakeLists.txt tools/compile_sound.py tools/netplay_build_id.cmake)
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
string(APPEND NETPLAY_IDENTITY ";profile:${F3_PROFILE_INSTRUMENT};${F3_PROFILE_MODE};${F3_PROFILE_COLD_OPT}")
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
