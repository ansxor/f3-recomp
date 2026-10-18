cmake_minimum_required(VERSION 3.20)
project(f3hle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(F3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(f3hle
    ${F3_ROOT}/runtime/audio/hle/wasm_api.cpp
    ${F3_ROOT}/runtime/audio/hle/core.cpp
    ${F3_ROOT}/runtime/audio/hle/replay.cpp
    ${F3_ROOT}/runtime/audio/hle/sequencer.cpp
    ${F3_ROOT}/runtime/audio/hle/synth.cpp
)

target_include_directories(f3hle PRIVATE
    ${F3_ROOT}/include
    ${F3_ROOT}/runtime
    ${F3_ROOT}/runtime/third_party/faust
)

target_compile_options(f3hle PRIVATE
    -O3
    -fwasm-exceptions
)

target_link_options(f3hle PRIVATE
    -O3
    -fwasm-exceptions
    -sSTANDALONE_WASM=1
    --no-entry
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1048576
    "-sEXPORTED_FUNCTIONS=[_f3hle_abi_version,_f3hle_last_error,_f3hle_alloc,_f3hle_free,_f3hle_create,_f3hle_destroy,_f3hle_start,_f3hle_rewind,_f3hle_render,_f3hle_skip,_f3hle_position_frames,_f3hle_position_ticks]"
)

set_target_properties(f3hle PROPERTIES
    SUFFIX ".wasm"
)
