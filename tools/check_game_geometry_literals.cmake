# CTest guard: the game renderer stack takes its scanout geometry from the compile-time
# f3rt::geometry / game_config::video (games/<game>/config.toml [video]). No source in it may
# spell the Land Maker window (first visible line 24, height 232) as a literal.
# Usage: cmake -DSOURCE_DIR=<repo> -P tools/check_game_geometry_literals.cmake
file(GLOB_RECURSE files
    "${SOURCE_DIR}/runtime/renderer/game/*" "${SOURCE_DIR}/runtime/renderer/gpu/*"
    "${SOURCE_DIR}/runtime/renderer/shaders/scene*" "${SOURCE_DIR}/runtime/renderer/shaders/sprite*"
    "${SOURCE_DIR}/games/*/video/*.cpp")
list(APPEND files "${SOURCE_DIR}/runtime/renderer/scale.hpp" "${SOURCE_DIR}/include/f3rt/game_video.hpp")
set(failures "")
set(count 0)
foreach(file IN LISTS files)
    file(STRINGS "${file}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        string(REGEX REPLACE "(<<|>>)[ ]*24" "" code "${code}")  # bit shifts are not geometry
        if(code MATCHES "(^|[^0-9A-Za-z_.])(24|232)([^0-9A-Za-z_]|$)")
            string(APPEND failures "${file}:${number}: ${line}\n")
        endif()
    endforeach()
    math(EXPR count "${count} + 1")
endforeach()
if(count EQUAL 0)
    message(FATAL_ERROR "geometry literal guard scanned no files")
endif()
if(failures)
    message(FATAL_ERROR "Hard-coded 24/232 scanout geometry in the game renderer:\n${failures}")
endif()
message(STATUS "geometry literal guard: ${count} files clean")
