cmake_minimum_required(VERSION 3.24)

option(F3RT_SDL "Build SDL3 frontend" ON)
option(F3RT_GPU "Build SDL3 GPU presentation (glslang and SPIRV-Cross are built if not installed)" ON)
option(F3RT_NETPLAY "Build the netplay client and relay server (Yojimbo and httplib)" ON)
option(F3RT_HOST_TOOLS "Build the host tools: f3rt-tool, f3rt-runner and f3rt-replay" ON)
option(BUILD_TESTING "Build the unit tests" ON)

project(f3rt VERSION 0.1 LANGUAGES C CXX)

# compile_commands.json for clangd and other LSP servers; `make compdb` links it to the root.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(F3RT_SDL AND NOT F3RT_NETPLAY)
    message(FATAL_ERROR "F3RT_SDL requires F3RT_NETPLAY: the SDL frontend drives netplay sessions")
endif()
if(BUILD_TESTING AND NOT F3RT_HOST_TOOLS)
    message(FATAL_ERROR "BUILD_TESTING requires F3RT_HOST_TOOLS: the tests link f3rt-runner")
endif()

include(cmake/dependencies.cmake)
include(cmake/corrosion.cmake)
include(cmake/codegen.cmake)

# --- Core runtime (f3rt) ----------------------------------------------------

# Games opt into custom scene/boot/netplay implementations via games/<game>/{video,boot,netplay}/.
file(GLOB GAME_VIDEO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/video/*.cpp")
if(GAME_VIDEO_SOURCES)
    set(F3RT_GAME_VIDEO ON)
    set(F3RT_GAME_VIDEO_SOURCES ${GAME_VIDEO_SOURCES})
else()
    set(F3RT_GAME_VIDEO_SOURCES runtime/renderer/game/video_generic.cpp)
endif()

file(GLOB GAME_BOOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/boot/*.cpp")
if(GAME_BOOT_SOURCES)
    set(F3RT_GAME_BOOT_SOURCES ${GAME_BOOT_SOURCES})
else()
    set(F3RT_GAME_BOOT_SOURCES runtime/stubs/boot.cpp)
endif()

file(GLOB GAME_NETPLAY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/netplay/*.cpp")
if(GAME_NETPLAY_SOURCES)
    set(F3RT_GAME_NETPLAY_SOURCES ${GAME_NETPLAY_SOURCES})
else()
    set(F3RT_GAME_NETPLAY_SOURCES runtime/stubs/netplay.cpp)
endif()

set(F3RT_NETPLAY_SOURCES "")
if(F3RT_NETPLAY)
    set(F3RT_NETPLAY_SOURCES
        runtime/netplay/client.cpp
        runtime/netplay/session.cpp
        runtime/netplay/recorder.cpp
    )
endif()

set(F3RT_CORE_SOURCES
    runtime/third_party/sha1dc/sha1.c
    runtime/third_party/sha1dc/ubc_check.c
    runtime/rom.cpp
    runtime/cpu_abi.cpp
    runtime/machine.cpp
    runtime/renderer/fdp/video.cpp
    runtime/renderer/game/video.cpp
    runtime/renderer/game/video_log.cpp
    runtime/renderer/decode.cpp
    runtime/renderer/game/tiles.cpp
    runtime/renderer/game/text.cpp
    runtime/renderer/game/sprites.cpp
    runtime/renderer/game/lines.cpp
    runtime/renderer/game/row_workers.cpp
    runtime/renderer/game/indexed_compositor.cpp
    runtime/renderer/game/scene_snapshot.cpp
    ${F3RT_GAME_VIDEO_SOURCES}
    ${F3RT_GAME_BOOT_SOURCES}
    ${F3RT_GAME_NETPLAY_SOURCES}
    runtime/debug/profile.cpp
    runtime/input.cpp
    runtime/debug/input_script.cpp
    runtime/sprites/units.cpp
    runtime/sprites/behaviours.cpp
    runtime/debug/discovery.cpp
    ${F3RT_NETPLAY_SOURCES}
)

# Build identity hashes all simulation sources (Rust crates, ABI headers, C++ core, generated program).
file(GLOB_RECURSE F3_RUST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/crates/f3rt-*/src/*.rs")
file(GLOB F3_ABI_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/abi/*.h")
set(F3_SIMULATION_SOURCES
    "${F3_GAME_CONFIG}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    ${F3_RUST_SOURCES}
    ${F3_ABI_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/machine.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/rom.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/input.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/video.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/f3rt/game_video.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/state_io.hpp"
)
foreach(_src IN LISTS F3RT_CORE_SOURCES)
    get_filename_component(_abs "${_src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND F3_SIMULATION_SOURCES "${_abs}")
endforeach()
if(F3_GENERATED_DIR)
    list(APPEND F3_SIMULATION_SOURCES
        "${F3_GENERATED_DIR}/program.c"
        "${F3_GENERATED_DIR}/program.bin"
    )
endif()

string(REPLACE ";" "\n" _inputs_txt "${F3_SIMULATION_SOURCES}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/build_identity_inputs.txt" CONTENT "${_inputs_txt}\n" @ONLY)

set(F3_BUILD_IDENTITY_ARGS
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DGAME_ID=${F3_GAME}
    -DINPUT_FILES_LIST=${CMAKE_CURRENT_BINARY_DIR}/build_identity_inputs.txt
    -DTEMPLATE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_identity.hpp.in
    -DOUTPUT_FILE=${F3_GENERATED_CONFIG_DIR}/build_identity.hpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_identity.cmake
)
add_custom_command(
    OUTPUT "${F3_GENERATED_CONFIG_DIR}/build_identity.hpp"
    COMMAND ${CMAKE_COMMAND} ${F3_BUILD_IDENTITY_ARGS}
    DEPENDS
        ${F3_SIMULATION_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_identity.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_identity.hpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/build_identity_inputs.txt
    COMMENT "Generating build_identity.hpp"
    VERBATIM
)
add_custom_target(f3rt_build_identity DEPENDS "${F3_GENERATED_CONFIG_DIR}/build_identity.hpp")
execute_process(COMMAND ${CMAKE_COMMAND} ${F3_BUILD_IDENTITY_ARGS})

add_library(f3rt STATIC ${F3RT_CORE_SOURCES})
add_dependencies(f3rt f3rt_build_identity)

# Generated headers are reached as "generated_config/<name>"; runtime sprite headers
# use "sprites/<name>" to avoid basename ambiguity.
target_include_directories(f3rt
    PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}"
    PRIVATE runtime "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Header-only sprite behaviour registry: sprites/behaviours.cpp includes it directly
# so static registrars cannot be dead-stripped from the archive.
set(F3_SPRITE_BEHAVIOURS_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/sprites/behaviours.hpp")
if(EXISTS "${F3_SPRITE_BEHAVIOURS_HEADER}")
    target_compile_definitions(f3rt PRIVATE F3RT_SPRITE_BEHAVIOURS_HEADER="${F3_SPRITE_BEHAVIOURS_HEADER}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${F3_SPRITE_BEHAVIOURS_HEADER}")
endif()

find_package(Threads REQUIRED)
target_link_libraries(f3rt
    PUBLIC f3rt_abi
    PRIVATE Threads::Threads ZLIB::ZLIB cpp_yyjson::cpp_yyjson
)
if(WIN32)
    target_link_libraries(f3rt PRIVATE ws2_32)
endif()

target_compile_options(f3rt PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(f3rt PUBLIC F3RT_DEFAULT_SET="${F3_GAME}")
if(F3RT_GAME_VIDEO)
    target_compile_definitions(f3rt PUBLIC F3RT_GAME_VIDEO=1)
endif()
if(F3_PROFILE_INSTRUMENT)
    target_compile_definitions(f3rt PUBLIC F3_PROFILE_INSTRUMENT=1)
endif()
if(F3_PROFILE_SLIM)
    target_compile_definitions(f3rt PUBLIC F3_PROFILE_SLIM_ENABLED=1)
endif()

# --- Netplay ----------------------------------------------------------------

if(F3RT_NETPLAY)
    add_library(f3-netplay-shared STATIC netplay/shared/contract.cpp)
    target_include_directories(f3-netplay-shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(f3-netplay-shared PUBLIC Yojimbo::yojimbo httplib::httplib)
    target_compile_options(f3-netplay-shared PRIVATE -Wall -Wextra -Wpedantic)

    add_library(f3-netplay-relay STATIC
        netplay/server/relay.cpp
        netplay/server/state.cpp
        netplay/server/log_writer.cpp
        netplay/server/token_service.cpp
        netplay/server/socket_transport.cpp
    )
    target_include_directories(f3-netplay-relay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" netplay/server)
    target_link_libraries(f3-netplay-relay PUBLIC f3-netplay-shared cpp_yyjson::cpp_yyjson)
    target_compile_options(f3-netplay-relay PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(f3-netplay-server netplay/server/main.cpp)
    target_link_libraries(f3-netplay-server PRIVATE f3-netplay-relay)

    target_link_libraries(f3rt PUBLIC f3-netplay-shared)
endif()

# --- Recompiled game --------------------------------------------------------

if(F3_GENERATED_DIR)
    add_subdirectory(recomp)
endif()

# --- Host tools -------------------------------------------------------------

if(F3RT_HOST_TOOLS)
    add_library(f3rt-runner STATIC
        tools/runner/runner.cpp
        tools/runner/inputs.cpp
        tools/runner/sprite_check.cpp
    )
    target_include_directories(f3rt-runner PUBLIC tools/runner runtime)
    target_link_libraries(f3rt-runner PUBLIC f3rt)
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-runner PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()

    add_executable(f3rt-tool
        tools/tool.cpp
        tools/commands/gameplay.cpp
        tools/commands/motion.cpp
        tools/commands/sprite_check.cpp
        tools/commands/netplay.cpp
        tools/commands/netplay_replay.cpp
    )
    target_include_directories(f3rt-tool PRIVATE tools tools/commands)
    target_link_libraries(f3rt-tool PRIVATE f3rt f3rt-runner)
    target_compile_definitions(f3rt-tool PRIVATE F3RT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-tool PRIVATE F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()

    if(TARGET f3_recompiled)
        target_link_libraries(f3rt-runner PRIVATE f3_recompiled)
        target_link_libraries(f3rt-tool PRIVATE f3_recompiled)
    endif()

    add_executable(f3rt-replay runtime/replay.cpp)
    target_link_libraries(f3rt-replay PRIVATE f3rt)
endif()

# --- Frontend & GPU ---------------------------------------------------------

add_library(f3rt-frontend STATIC
    runtime/frontend/frame_slots.cpp
    runtime/frontend/audio_pacer.cpp
    runtime/frontend/fast_boot.cpp
    runtime/frontend/config_file.cpp
)
target_include_directories(f3rt-frontend PUBLIC runtime runtime/frontend)
target_link_libraries(f3rt-frontend PUBLIC f3rt)
target_compile_options(f3rt-frontend PRIVATE -Wall -Wextra -Wpedantic)

if(F3RT_SDL)
    add_library(f3rt-ui STATIC
        runtime/frontend/settings.cpp
        runtime/frontend/ui.cpp
    )
    target_include_directories(f3rt-ui PUBLIC runtime runtime/frontend)
    target_link_libraries(f3rt-ui PUBLIC SDL3::SDL3 imgui::imgui f3rt-frontend)

    if(F3_GENERATED_DIR)
        add_executable(${F3_GAME_EXECUTABLE} runtime/frontend/frontend.cpp)
        target_link_libraries(${F3_GAME_EXECUTABLE} PRIVATE f3rt f3_recompiled f3rt-ui)
        target_compile_definitions(${F3_GAME_EXECUTABLE} PRIVATE
            F3RT_GAME=1
            F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}"
        )
    endif()

    if(F3RT_GPU)
        target_compile_definitions(f3rt-ui PRIVATE F3RT_GPU=1)

        set(VIDEO_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/video_shaders.hpp")
        add_custom_command(
            OUTPUT "${VIDEO_SHADER_HEADER}"
            COMMAND ${F3_UV_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders/compile.py"
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders"
                --output "${VIDEO_SHADER_HEADER}"
                --glslang "${F3_GLSLANG}"
                --spirv-cross "${F3_SPIRV_CROSS}"
            DEPENDS
                runtime/renderer/shaders/compile.py
                runtime/renderer/shaders/scene.glsl
                runtime/renderer/shaders/fullscreen.vert
                runtime/renderer/shaders/sprite.vert
                runtime/renderer/shaders/sprite.frag
                runtime/renderer/shaders/scene.frag
                runtime/renderer/shaders/scene_interp.frag
                runtime/renderer/shaders/scene_body.glsl
                runtime/renderer/shaders/postprocess.frag
                runtime/renderer/gpu/scene_layout.h
                ${F3_SHADER_TOOL_TARGETS}
            VERBATIM
        )

        add_library(f3rt-gpu STATIC
            runtime/renderer/gpu/video.cpp
            runtime/renderer/gpu/encode.cpp
            runtime/renderer/gpu/interp.cpp
            runtime/renderer/gpu/motion.cpp
            "${VIDEO_SHADER_HEADER}"
        )
        if(APPLE)
            enable_language(OBJCXX)
            target_sources(f3rt-gpu PRIVATE runtime/renderer/gpu/pacing_macos.mm)
            set_source_files_properties(runtime/renderer/gpu/pacing_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            target_link_libraries(f3rt-gpu PRIVATE "-framework AppKit" "-framework QuartzCore")
        endif()
        target_include_directories(f3rt-gpu
            PUBLIC include runtime runtime/frontend "${CMAKE_CURRENT_BINARY_DIR}"
            PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
        )
        target_link_libraries(f3rt-gpu PUBLIC SDL3::SDL3)
        target_compile_definitions(f3rt-gpu INTERFACE F3RT_GPU=1)
        target_compile_options(f3rt-gpu PRIVATE -Wall -Wextra -Wpedantic)

        foreach(frontend f3rt-tool ${F3_GAME_EXECUTABLE})
            if(TARGET ${frontend})
                target_link_libraries(${frontend} PRIVATE f3rt-gpu)
            endif()
        endforeach()
    endif()
endif()

# --- Tests ------------------------------------------------------------------

include(CTest)
if(BUILD_TESTING)
    add_test(NAME game-geometry-literals
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_game_geometry_literals.cmake
    )

    include(GoogleTest)

    if(F3RT_NETPLAY)
        add_executable(f3-netplay-relay-test netplay/server/tests/relay_test.cpp)
        target_link_libraries(f3-netplay-relay-test PRIVATE f3-netplay-relay GTest::gtest_main)
        gtest_discover_tests(f3-netplay-relay-test TEST_PREFIX "netplay-relay.")
    endif()

    add_library(f3rt-test-support STATIC
        runtime/tests/support.cpp
        runtime/renderer/gpu/motion.cpp
    )
    target_include_directories(f3rt-test-support PUBLIC runtime runtime/tests)
    target_link_libraries(f3rt-test-support PUBLIC f3rt f3rt-runner GTest::gtest rapidcheck_gtest)
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-test-support PUBLIC F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()

    foreach(area video input input_script cpu sprites motion_interp state netplay bytes)
        file(GLOB GAME_AREA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/tests/${area}*.cpp")
        add_executable(f3rt-test-${area} runtime/tests/${area}.cpp ${GAME_AREA_TEST_SOURCES})
        target_link_libraries(f3rt-test-${area} PRIVATE f3rt-test-support GTest::gtest_main)
        gtest_discover_tests(f3rt-test-${area} TEST_PREFIX "runtime-${area}.")
    endforeach()

    if(TARGET f3rt-gpu)
        target_link_libraries(f3rt-test-video PRIVATE f3rt-gpu)
    endif()
    if(TARGET f3_recompiled)
        target_link_libraries(f3rt-test-sprites PRIVATE f3_recompiled)
    endif()

    # Emit-unit replay check (needs the game ROMs and a game that declares emit units).
    file(READ "${F3_GAME_CONFIG}" F3_GAME_CONFIG_TEXT)
    string(FIND "${F3_GAME_CONFIG_TEXT}" "[[video.emit_units]]" F3_EMIT_UNITS_POS)
    if(F3_ROM_DIR AND NOT F3_EMIT_UNITS_POS EQUAL -1)
        add_test(NAME sprite-units COMMAND f3rt-test-sprites --gtest_filter=SpriteUnits.RomReplayAndInvariants)
    endif()

    if(TARGET f3rt-ui)
        add_executable(f3rt-test-frontend runtime/tests/frontend.cpp)
        target_link_libraries(f3rt-test-frontend PRIVATE f3rt-test-support f3rt-ui GTest::gtest_main)
        gtest_discover_tests(f3rt-test-frontend TEST_PREFIX "runtime-frontend.")
    endif()
endif()
