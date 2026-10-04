cmake_minimum_required(VERSION 3.24)
project(f3rt VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(F3RT_SDL "Build SDL3 frontend" ON)
set(F3_ROM_DIR "" CACHE PATH "Land Maker Japan ROM directory; enables automatic native generation")
set(F3_GENERATED_DIR "" CACHE PATH "Pre-generated Land Maker C directory (optional)")
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

file(GLOB AUDIO_SOURCES CONFIGURE_DEPENDS runtime/third_party/audio/*.cpp)
add_library(f3rt STATIC runtime/rom.cpp runtime/cpu_abi.cpp runtime/machine.cpp runtime/interpreter.cpp
    runtime/video.cpp runtime/audio.cpp ${AUDIO_SOURCES})
target_include_directories(f3rt PUBLIC include PRIVATE runtime)
target_link_libraries(f3rt PRIVATE f3rt_musashi)
target_compile_options(f3rt PRIVATE -Wall -Wextra -Wpedantic)
if(F3RT_SDL)
    find_package(SDL3 CONFIG REQUIRED)
    add_executable(f3rt-run runtime/frontend.cpp)
    target_link_libraries(f3rt-run PRIVATE f3rt SDL3::SDL3)
    if(F3_GENERATED_DIR)
        add_subdirectory(recomp)
        target_link_libraries(f3rt-run PRIVATE f3_recompiled)
        target_compile_definitions(f3rt-run PRIVATE F3RT_GENERATED=1)
        add_executable(landmakr runtime/frontend.cpp)
        target_link_libraries(landmakr PRIVATE f3rt f3_recompiled SDL3::SDL3)
        target_compile_definitions(landmakr PRIVATE
            F3RT_GENERATED=1 F3RT_LANDMAKR=1 F3RT_DEFAULT_ROM_DIR="${F3_ROM_DIR}")
    endif()
endif()
add_executable(f3rt-replay runtime/replay.cpp)
target_link_libraries(f3rt-replay PRIVATE f3rt)
include(CTest)
if(BUILD_TESTING)
    add_executable(f3rt-check runtime/check.cpp)
    target_link_libraries(f3rt-check PRIVATE f3rt)
    add_test(NAME runtime-devices COMMAND f3rt-check)
endif()
