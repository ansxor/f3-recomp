# Nintendo 3DS packaging, included from the top level when the devkitPro toolchain sets
# NINTENDO_3DS. Produces the 3dsx (part of ALL) and a <game>-cia target.
#
# The 3DS build is core-only: F3RT_SDL/GPU/NETPLAY/HOST_TOOLS/SOUND_NATIVE and BUILD_TESTING are
# off, so this file owns the frontend executable, its RomFS and the CTR formats.

set(F3_N3DS_META_DIR "${CMAKE_CURRENT_SOURCE_DIR}/games/${F3_GAME}/n3ds")
if(NOT EXISTS "${F3_N3DS_META_DIR}/meta.cmake")
    message(FATAL_ERROR "The 3DS build needs games/${F3_GAME}/n3ds/meta.cmake (title metadata)")
endif()
include("${F3_N3DS_META_DIR}/meta.cmake")
foreach(_var IN ITEMS F3_N3DS_TITLE F3_N3DS_DESCRIPTION F3_N3DS_AUTHOR F3_N3DS_PRODUCT_CODE
        F3_N3DS_UNIQUE_ID)
    if(NOT DEFINED "${_var}" OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "games/${F3_GAME}/n3ds/meta.cmake must set ${_var}")
    endif()
endforeach()
string(LENGTH "${F3_N3DS_UNIQUE_ID}" _unique_id_length)
if(NOT F3_N3DS_UNIQUE_ID MATCHES "^0x[0-9A-Fa-f]+$" OR _unique_id_length GREATER 10)
    message(FATAL_ERROR "F3_N3DS_UNIQUE_ID must be a 32-bit hex literal: ${F3_N3DS_UNIQUE_ID}")
endif()

set(F3_N3DS_ICON "${F3_N3DS_META_DIR}/icon.png")
set(F3_N3DS_BANNER_IMAGE "${F3_N3DS_META_DIR}/banner.png")
set(F3_N3DS_BANNER_AUDIO "${F3_N3DS_META_DIR}/banner.wav")
foreach(_asset IN ITEMS "${F3_N3DS_ICON}" "${F3_N3DS_BANNER_IMAGE}" "${F3_N3DS_BANNER_AUDIO}")
    if(NOT EXISTS "${_asset}")
        message(FATAL_ERROR "Missing 3DS asset: ${_asset}")
    endif()
endforeach()

# ---- RomFS: the chips rom_files.txt names, so the PAL files never enter the image ----
set(F3_N3DS_ROM_FILES_LIST "${CMAKE_CURRENT_BINARY_DIR}/rom_files.txt")
if(NOT EXISTS "${F3_N3DS_ROM_FILES_LIST}")
    message(FATAL_ERROR "rom_files.txt is missing; recomp/compile/roms.py --files-output did not run")
endif()
file(STRINGS "${F3_N3DS_ROM_FILES_LIST}" F3_N3DS_ROM_NAMES)
set(F3_N3DS_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
set(F3_N3DS_ROMFS_GAME_DIR "${F3_N3DS_ROMFS_DIR}/${F3_GAME}")
# ctr_create_3dsx validates the ROMFS directory while configuring, so it must exist now.
file(MAKE_DIRECTORY "${F3_N3DS_ROMFS_GAME_DIR}")
set(F3_N3DS_ROMFS_OUTPUTS "")
set(F3_N3DS_ROMFS_INPUTS "")
foreach(_name IN LISTS F3_N3DS_ROM_NAMES)
    set(_source "${F3_ROM_DIR}/${_name}")
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR "rom_files.txt names ${_name}, which is missing from F3_ROM_DIR")
    endif()
    list(APPEND F3_N3DS_ROMFS_INPUTS "${_source}")
    list(APPEND F3_N3DS_ROMFS_OUTPUTS "${F3_N3DS_ROMFS_GAME_DIR}/${_name}")
endforeach()
add_custom_command(
    OUTPUT ${F3_N3DS_ROMFS_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${F3_N3DS_ROMFS_GAME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy ${F3_N3DS_ROMFS_INPUTS} "${F3_N3DS_ROMFS_GAME_DIR}"
    DEPENDS ${F3_N3DS_ROMFS_INPUTS}
    COMMENT "Staging ROM chips for RomFS"
    VERBATIM)
add_custom_target(f3rt-n3ds-romfs DEPENDS ${F3_N3DS_ROMFS_OUTPUTS})

# ---- frontend, vendored ftpd ImGui backends and the picasso shader blob ----
file(GLOB F3_N3DS_FRONTEND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/frontend/n3ds/*.cpp")
if(NOT F3_N3DS_FRONTEND_SOURCES)
    message(FATAL_ERROR "runtime/frontend/n3ds/*.cpp is missing")
endif()
# Dev-only scripted run for the frame-budget bench (scripts/n3ds-bench.sh): reads
# sdmc:/3ds/f3-recomp/<set>/bench.txt, logs scenario markers and ignores settings.cfg. Off in the
# shipped build, where bench.cpp is not even compiled.
option(F3_N3DS_BENCH "Build the 3DS frontend with the scripted bench input" OFF)
if(NOT F3_N3DS_BENCH)
    list(REMOVE_ITEM F3_N3DS_FRONTEND_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/frontend/n3ds/bench.cpp")
endif()
file(GLOB F3_N3DS_FTPD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/ftpd/*.cpp")
if(NOT F3_N3DS_FTPD_SOURCES)
    message(FATAL_ERROR "runtime/third_party/ftpd/*.cpp is missing")
endif()
set(F3_N3DS_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/ftpd/vshader.v.pica")
if(NOT EXISTS "${F3_N3DS_SHADER}")
    message(FATAL_ERROR "Missing 3DS ImGui shader: ${F3_N3DS_SHADER}")
endif()
# The blob header is named after the compiled binary: vshader.shbin -> vshader_shbin.h.
ctr_add_shader_library(f3rt-n3ds-shaders
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vshader.shbin" "${F3_N3DS_SHADER}")
dkp_add_embedded_binary_library(f3rt-n3ds-shader-blobs f3rt-n3ds-shaders)

# The pthread subset has to be an object of the executable, not an archive member: libstdc++'s
# references to it are weak, so the linker would never pull it out of a static library.
set(F3_N3DS_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform/pthread_ctru.cpp")

add_executable(${F3_GAME_EXECUTABLE} ${F3_N3DS_FRONTEND_SOURCES} ${F3_N3DS_FTPD_SOURCES}
    ${F3_N3DS_PLATFORM_SOURCES})
target_include_directories(${F3_GAME_EXECUTABLE} PRIVATE
    runtime runtime/frontend runtime/frontend/n3ds
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/ftpd" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(${F3_GAME_EXECUTABLE} PRIVATE
    f3rt f3_recompiled f3rt-frontend imgui f3rt-n3ds-shader-blobs citro3d ctru m)
target_compile_options(${F3_GAME_EXECUTABLE} PRIVATE -ffunction-sections -fdata-sections)
if(F3_N3DS_BENCH)
    target_compile_definitions(${F3_GAME_EXECUTABLE} PRIVATE F3_N3DS_BENCH)
endif()
target_link_options(${F3_GAME_EXECUTABLE} PRIVATE -Wl,--gc-sections)

# ---- 3dsx: built by every default build; embeds the RomFS ----
ctr_generate_smdh("${CMAKE_CURRENT_BINARY_DIR}/${F3_GAME}.smdh"
    NAME "${F3_N3DS_TITLE}"
    DESCRIPTION "${F3_N3DS_DESCRIPTION}"
    AUTHOR "${F3_N3DS_AUTHOR}"
    ICON "${F3_N3DS_ICON}")
ctr_create_3dsx(${F3_GAME_EXECUTABLE}
    SMDH "${CMAKE_CURRENT_BINARY_DIR}/${F3_GAME}.smdh"
    ROMFS "${F3_N3DS_ROMFS_DIR}"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${F3_GAME}.3dsx")
# ctr_create_3dsx only orders the executable and the SMDH; the staged chips are ours to order.
add_dependencies(${F3_GAME_EXECUTABLE}_3dsx f3rt-n3ds-romfs)

# ---- CIA: an explicitly requested target, since it also needs makerom and bannertool ----
set(F3_N3DS_RSF "${CMAKE_CURRENT_BINARY_DIR}/n3ds/app.rsf")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/n3ds/app.rsf.in" "${F3_N3DS_RSF}" @ONLY)

set(F3_N3DS_CIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/n3ds")
set(F3_N3DS_BANNER "${F3_N3DS_CIA_DIR}/banner.bnr")
set(F3_N3DS_SMDH_ICON "${F3_N3DS_CIA_DIR}/icon.icn")
if(NOT F3_MAKEROM_EXECUTABLE OR NOT F3_BANNERTOOL_EXECUTABLE)
    message(FATAL_ERROR "The ${F3_GAME}-cia target needs -DF3_MAKEROM_EXECUTABLE and "
        "-DF3_BANNERTOOL_EXECUTABLE")
endif()
add_custom_command(
    OUTPUT "${F3_N3DS_BANNER}" "${F3_N3DS_SMDH_ICON}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${F3_N3DS_CIA_DIR}"
    COMMAND "${F3_BANNERTOOL_EXECUTABLE}" makebanner
        -i "${F3_N3DS_BANNER_IMAGE}" -a "${F3_N3DS_BANNER_AUDIO}" -o "${F3_N3DS_BANNER}"
    COMMAND "${F3_BANNERTOOL_EXECUTABLE}" makesmdh
        -s "${F3_N3DS_TITLE}" -l "${F3_N3DS_DESCRIPTION}" -p "${F3_N3DS_AUTHOR}"
        -i "${F3_N3DS_ICON}" -o "${F3_N3DS_SMDH_ICON}"
    DEPENDS "${F3_N3DS_BANNER_IMAGE}" "${F3_N3DS_BANNER_AUDIO}" "${F3_N3DS_ICON}"
    COMMENT "Building the 3DS banner and icon"
    VERBATIM)

add_custom_target(${F3_GAME}-cia
    COMMAND "${F3_MAKEROM_EXECUTABLE}" -f cia -o "${CMAKE_CURRENT_BINARY_DIR}/${F3_GAME}.cia"
        -target t -exefslogo -elf "$<TARGET_FILE:${F3_GAME_EXECUTABLE}>"
        -rsf "${F3_N3DS_RSF}" -banner "${F3_N3DS_BANNER}" -icon "${F3_N3DS_SMDH_ICON}"
        -DAPP_ROMFS=${F3_N3DS_ROMFS_DIR} -major 1 -minor 0 -micro 0
    DEPENDS "${F3_N3DS_BANNER}" "${F3_N3DS_SMDH_ICON}"
    COMMENT "Building ${F3_GAME}.cia"
    VERBATIM)
add_dependencies(${F3_GAME}-cia ${F3_GAME_EXECUTABLE} f3rt-n3ds-romfs)
