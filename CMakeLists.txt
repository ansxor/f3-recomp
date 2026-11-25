cmake_minimum_required(VERSION 3.24)

option(F3RT_SDL "Build SDL3 frontend" ON)
option(F3RT_GPU "Build SDL3 GPU presentation (glslang and SPIRV-Cross are built if not installed)" ON)
option(F3RT_NETPLAY "Build the netplay client and relay server (Yojimbo and httplib)" ON)
option(F3RT_HOST_TOOLS "Build the host tools: f3rt-tool, f3rt-runner and f3rt-replay" ON)
option(F3RT_TRACE_STACKED_SR "Record work RAM words written by main CPU exception entry" OFF)
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

# Games opt into custom boot/netplay implementations via games/<game>/{boot,netplay}/.

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
    runtime/renderer/game/oracle.cpp
    runtime/renderer/game/bridge_video_host.cpp
    runtime/renderer/decode.cpp
    runtime/renderer/graphics_assets.cpp
    runtime/renderer/game/scene_snapshot.cpp
    ${F3RT_GAME_BOOT_SOURCES}
    ${F3RT_GAME_NETPLAY_SOURCES}
    runtime/debug/profile.cpp
    runtime/input.cpp
    runtime/debug/input_script.cpp
    runtime/sprites/enhancements.cpp
    runtime/renderer/game/enhancements.cpp
    runtime/renderer/game/flicker_hold.cpp
    runtime/renderer/game/full_detail.cpp
    runtime/sprites/groups.cpp
    runtime/sprites/sprite_system.cpp
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
if(F3_GENERATED_DIR)
    target_include_directories(f3rt PRIVATE "${F3_GENERATED_DIR}")
endif()

find_package(Threads REQUIRED)
target_link_libraries(f3rt
    PUBLIC f3rt_abi f3rt_video_cxx f3rt_sprites_cxx
    PRIVATE Threads::Threads ZLIB::ZLIB cpp_yyjson::cpp_yyjson
)
# f3rt_video_cxx calls host adaptors defined in f3rt (bridge_video_host.cpp); record the mutual
# static-library dependency so single-pass linkers (GNU ld.bfd) rescan libf3rt.a after it.
target_link_libraries(f3rt_video_cxx PRIVATE f3rt)
if(WIN32)
    target_link_libraries(f3rt PRIVATE ws2_32)
endif()

target_compile_options(f3rt PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(f3rt PUBLIC F3RT_DEFAULT_SET="${F3_GAME}")
if(F3RT_TRACE_STACKED_SR)
    target_compile_definitions(f3rt PUBLIC F3RT_TRACE_STACKED_SR)
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

    # `f3rt-tool motion` drives f3rt-gpu, which only exists with F3RT_SDL and F3RT_GPU. Without it the
    # command is a stub, so f3rt-tool (and its mcp server) still builds, e.g. in the GPU-less CI job.
    if(F3RT_SDL AND F3RT_GPU)
        set(F3RT_TOOL_MOTION_SOURCE tools/commands/motion.cpp)
    else()
        set(F3RT_TOOL_MOTION_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/tool_motion_stub.cpp")
        file(WRITE "${F3RT_TOOL_MOTION_SOURCE}"
            "#include \"commands.hpp\"\n"
            "namespace f3rt::tool {\n"
            "int cmd_motion(int, char **) {\n"
            "    std::cerr << \"motion needs a build with F3RT_SDL=ON and F3RT_GPU=ON\\n\";\n"
            "    return 2;\n"
            "}\n"
            "}\n")
    endif()

    add_executable(f3rt-tool
        tools/tool.cpp
        tools/commands/gameplay.cpp
        "${F3RT_TOOL_MOTION_SOURCE}"
        tools/commands/sprite_check.cpp
        tools/commands/netplay.cpp
        tools/commands/netplay_replay.cpp
        tools/commands/mcp.cpp
    )
    target_include_directories(f3rt-tool PRIVATE tools tools/commands)
    target_link_libraries(f3rt-tool PRIVATE f3rt f3rt-runner mcp::mcp ZLIB::ZLIB)
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

        find_program(UV_EXECUTABLE NAMES uv REQUIRED)
        set(VIDEO_SHADER_RS "${CMAKE_CURRENT_BINARY_DIR}/generated/video_shaders.rs")
        add_custom_command(
            OUTPUT "${VIDEO_SHADER_RS}"
            COMMAND "${UV_EXECUTABLE}" run --project "${CMAKE_CURRENT_SOURCE_DIR}" --locked --no-default-groups python "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders/compile.py"
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/shaders"
                --output "${VIDEO_SHADER_RS}"
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
                runtime/renderer/shaders/rotate.frag
                runtime/renderer/shaders/postprocess.frag
                runtime/renderer/gpu/scene_layout.h
                ${F3_SHADER_TOOL_TARGETS}
            VERBATIM
        )
        add_custom_target(f3rt_video_shaders DEPENDS "${VIDEO_SHADER_RS}")
        add_dependencies(cargo-prebuild_f3rt_abi f3rt_video_shaders)
        corrosion_set_env_vars(f3rt_abi "F3_VIDEO_SHADERS_RS=${VIDEO_SHADER_RS}")

        add_library(f3rt-gpu STATIC
            runtime/renderer/gpu/video.cpp
        )
        target_include_directories(f3rt-gpu
            PUBLIC include runtime runtime/frontend "${CMAKE_CURRENT_BINARY_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/generated"
        )
        target_link_libraries(f3rt-gpu PUBLIC f3rt SDL3::SDL3)
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

    add_test(NAME f3rt-video-cargo-test
        COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
            cargo test -p f3rt-video
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(NAME f3rt-sprites-cargo-test
        COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
            cargo test -p f3rt-sprites
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    include(GoogleTest)

    if(F3RT_NETPLAY)
        add_executable(f3-netplay-relay-test netplay/server/tests/relay_test.cpp)
        target_link_libraries(f3-netplay-relay-test PRIVATE f3-netplay-relay GTest::gtest_main)
        gtest_discover_tests(f3-netplay-relay-test TEST_PREFIX "netplay-relay.")
    endif()

    add_library(f3rt-test-support STATIC
        runtime/tests/support.cpp
    )
    target_include_directories(f3rt-test-support PUBLIC runtime runtime/tests)
    target_link_libraries(f3rt-test-support PUBLIC f3rt f3rt-runner GTest::gtest rapidcheck_gtest)
    if(F3_ROM_DIR)
        target_compile_definitions(f3rt-test-support PUBLIC F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()

    foreach(area video input input_script cpu sprites state netplay bytes)
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

    # MCP tests (f3recomp mcp, f3rt-tool mcp and the bridge between them). The OCaml test executable is
    # built by dune at configure time, next to f3recomp. f3-mcp-smoke is ROM-free (protocol, tool list,
    # pre-init errors, bad init, fixture-game bridge errors) and runs wherever f3rt-tool is built;
    # F3RT_REQUIRE_TOOL makes a missing binary a failure instead of a skip. f3-mcp-e2e runs every case,
    # including the ones that boot the game, and needs F3_ROM_DIR.
    if(F3_RECOMP_COMMAND)
        get_filename_component(_recomp_exe_dir "${F3_RECOMP_COMMAND}" DIRECTORY)
        get_filename_component(_mcp_test_dir "${_recomp_exe_dir}/../test/api" ABSOLUTE)
        if(EXISTS "${_mcp_test_dir}/mcp_test.exe")
            add_test(NAME f3-mcp-smoke COMMAND "${_mcp_test_dir}/mcp_test.exe" test "^(f3recomp|smoke)$"
                WORKING_DIRECTORY "${_mcp_test_dir}")
            set_tests_properties(f3-mcp-smoke PROPERTIES
                ENVIRONMENT "F3RT_TOOL=$<TARGET_FILE:f3rt-tool>;F3RT_REQUIRE_TOOL=1;F3RT_ROM_DIR=;F3RT_TEST_GAME=${F3_GAME}")
            if(F3_ROM_DIR)
                add_test(NAME f3-mcp-e2e COMMAND "${_mcp_test_dir}/mcp_test.exe" WORKING_DIRECTORY "${_mcp_test_dir}")
                set_tests_properties(f3-mcp-e2e PROPERTIES
                    ENVIRONMENT "F3RT_TOOL=$<TARGET_FILE:f3rt-tool>;F3RT_ROM_DIR=${F3_ROM_DIR};F3RT_TEST_GAME=${F3_GAME}")
            endif()
        endif()
    endif()

    if(F3_ROM_DIR)
        add_test(NAME sprite-groups COMMAND f3rt-test-sprites --gtest_filter=SpriteSystem.RomGroupCoverage)
    endif()

    if(TARGET f3rt-ui)
        add_executable(f3rt-test-frontend runtime/tests/frontend.cpp)
        target_link_libraries(f3rt-test-frontend PRIVATE f3rt-test-support f3rt-ui GTest::gtest_main)
        gtest_discover_tests(f3rt-test-frontend TEST_PREFIX "runtime-frontend.")
    endif()
endif()
