cmake_minimum_required(VERSION 3.24)
project(f3rt VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(F3RT_SDL "Build SDL3 frontend" ON)
set(F3_GENERATED_DIR "" CACHE PATH "Generated Land Maker C directory")
set(F3_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../recomp" CACHE PATH "Recompiler source root (for generated helper headers)")

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
        include(${F3_GENERATED_DIR}/sources.cmake)
        target_sources(f3rt-run PRIVATE ${F3_GENERATED_SOURCES})
        target_include_directories(f3rt-run PRIVATE ${F3_GENERATED_DIR} ${F3_RECOMP_ROOT})
        target_compile_definitions(f3rt-run PRIVATE F3RT_GENERATED=1)
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
