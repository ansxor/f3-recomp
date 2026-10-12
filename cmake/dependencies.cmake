# Third-party dependencies via CPM.cmake (cmake/CPM.cmake, vendored v0.43.2).
# Installed packages win: CPMFindPackage tries find_package(<name> <VERSION>) first, so
# system / Homebrew / distro builds are linked as prebuilt binaries. Only missing (or too
# old) packages are fetched at the pinned GIT_TAG and built from source as subprojects.
# Sources are cached in CPM_SOURCE_CACHE (default ~/.cache/CPM), shared by all build dirs.
# Force everything from source with -DCPM_DOWNLOAD_ALL=ON (or per package: -DCPM_DOWNLOAD_<name>=ON).
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    if(DEFINED ENV{XDG_CACHE_HOME})
        set(CPM_SOURCE_CACHE "$ENV{XDG_CACHE_HOME}/CPM" CACHE PATH "CPM.cmake source cache")
    elseif(DEFINED ENV{HOME})
        set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/CPM" CACHE PATH "CPM.cmake source cache")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")

# Source fallbacks build as static libraries and stay out of `install`.
set(BUILD_SHARED_LIBS OFF)

# Source fallback is zlib-ng in zlib-compatible mode: faster, and unlike madler/zlib its
# CMake build does not rewrite zconf.h inside the shared source cache.
CPMFindPackage(NAME ZLIB GITHUB_REPOSITORY zlib-ng/zlib-ng GIT_TAG 2.3.3 VERSION 1.2.11
    OPTIONS "ZLIB_COMPAT ON" "ZLIB_ENABLE_TESTS OFF" "ZLIBNG_ENABLE_TESTS OFF" "WITH_GTEST OFF"
        "SKIP_INSTALL_ALL ON")
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib-ng)
endif()

# Google Highway: runtime-dispatched SIMD for the HLE voice kernel.
CPMFindPackage(NAME hwy GITHUB_REPOSITORY google/highway GIT_TAG 1.4.0 VERSION 1.4.0
    OPTIONS "HWY_ENABLE_TESTS OFF" "HWY_ENABLE_EXAMPLES OFF" "HWY_ENABLE_CONTRIB OFF"
        "HWY_ENABLE_INSTALL OFF")
if(NOT TARGET hwy::hwy)
    add_library(hwy::hwy ALIAS hwy)
endif()

CPMFindPackage(NAME yyjson GITHUB_REPOSITORY ibireme/yyjson GIT_TAG 0.12.0 VERSION 0.12.0
    OPTIONS "YYJSON_BUILD_TESTS OFF" "YYJSON_INSTALL OFF")
if(NOT TARGET yyjson::yyjson)
    add_library(yyjson::yyjson ALIAS yyjson)
endif()
# cpp-yyjson is header-only and unpackaged; wire it to the yyjson above instead of its copy.
CPMAddPackage(NAME cpp_yyjson GITHUB_REPOSITORY yosh-matsuda/cpp-yyjson GIT_TAG v1.1.0 DOWNLOAD_ONLY YES)
add_library(cpp_yyjson INTERFACE)
target_include_directories(cpp_yyjson SYSTEM INTERFACE "${cpp_yyjson_SOURCE_DIR}/include")
target_link_libraries(cpp_yyjson INTERFACE yyjson::yyjson)
target_compile_features(cpp_yyjson INTERFACE cxx_std_20)
add_library(cpp_yyjson::cpp_yyjson ALIAS cpp_yyjson)

CPMFindPackage(NAME httplib GITHUB_REPOSITORY yhirose/cpp-httplib GIT_TAG v0.51.0 VERSION 0.14
    OPTIONS "HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF" "HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF"
        "HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF" "HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF"
        "HTTPLIB_COMPILE OFF" "HTTPLIB_TEST OFF" "HTTPLIB_INSTALL OFF")

# Yojimbo with its bundled libsodium and TLSF (unpackaged; pinned commit).
CPMAddPackage(NAME Yojimbo GITHUB_REPOSITORY mas-bandwidth/yojimbo
    GIT_TAG 272153a10f32135bb44bb60e7467072baf48f762
    OPTIONS "YOJIMBO_SYSTEM_SODIUM OFF" "YOJIMBO_BUILD_TESTS OFF" "YOJIMBO_INSTALL OFF")

if(F3RT_SDL)
    CPMFindPackage(NAME SDL3 GITHUB_REPOSITORY libsdl-org/SDL GIT_TAG release-3.4.12 VERSION 3.4.0
        OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_EXAMPLES OFF"
            "SDL_TESTS OFF" "SDL_INSTALL OFF")
    # Dear ImGui ships no CMake build and distro packages omit the SDL3 backends.
    CPMAddPackage(NAME imgui GITHUB_REPOSITORY ocornut/imgui GIT_TAG v1.92.8 DOWNLOAD_ONLY YES)
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp")
    target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
    target_link_libraries(imgui PUBLIC SDL3::SDL3)
    add_library(imgui::imgui ALIAS imgui)

    if(F3RT_GPU)
        # Offline shader tools: installed binaries (Vulkan SDK, distro packages) first.
        find_program(F3_GLSLANG NAMES glslangValidator glslang)
        find_program(F3_SPIRV_CROSS spirv-cross)
        if(NOT F3_GLSLANG)
            CPMAddPackage(NAME glslang GITHUB_REPOSITORY KhronosGroup/glslang GIT_TAG 16.4.0
                EXCLUDE_FROM_ALL YES
                OPTIONS "ENABLE_OPT OFF" "ENABLE_HLSL OFF" "ENABLE_SPVREMAPPER OFF"
                    "GLSLANG_TESTS OFF" "GLSLANG_ENABLE_INSTALL OFF" "BUILD_EXTERNAL OFF")
            set(F3_GLSLANG "$<TARGET_FILE:glslang-standalone>")
            set(F3_SHADER_TOOL_TARGETS glslang-standalone)
        endif()
        if(NOT F3_SPIRV_CROSS)
            CPMAddPackage(NAME spirv_cross GITHUB_REPOSITORY KhronosGroup/SPIRV-Cross
                GIT_TAG vulkan-sdk-1.4.350.1 EXCLUDE_FROM_ALL YES
                OPTIONS "SPIRV_CROSS_CLI ON" "SPIRV_CROSS_ENABLE_TESTS OFF" "SPIRV_CROSS_SHARED OFF"
                    "SPIRV_CROSS_SKIP_INSTALL ON" "SPIRV_CROSS_ENABLE_C_API OFF")
            set(F3_SPIRV_CROSS "$<TARGET_FILE:spirv-cross>")
            list(APPEND F3_SHADER_TOOL_TARGETS spirv-cross)
        endif()
    endif()
endif()

if(BUILD_TESTING)
    CPMFindPackage(NAME GTest GITHUB_REPOSITORY google/googletest GIT_TAG v1.17.0 VERSION 1.14
        OPTIONS "INSTALL_GTEST OFF" "BUILD_GMOCK OFF" "gtest_force_shared_crt ON")
    # RapidCheck is rarely packaged; built from the vcpkg-era pinned commit.
    CPMAddPackage(NAME rapidcheck GITHUB_REPOSITORY emil-e/rapidcheck
        GIT_TAG ff6af6fc683159deb51c543b065eba14dfcf329b
        OPTIONS "RC_ENABLE_GTEST ON" "RC_ENABLE_TESTS OFF" "RC_ENABLE_EXAMPLES OFF")
endif()
