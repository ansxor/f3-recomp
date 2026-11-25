# The Rust runtime crates (audio, eeprom) reach C++ through one staticlib, crates/f3rt-abi: each
# staticlib embeds its own copy of std, so linking two would duplicate symbols. The other crates are
# rlib dependencies of f3rt-abi. Nix supplies the Corrosion source via CPM_Corrosion_SOURCE.
CPMAddPackage(NAME Corrosion GITHUB_REPOSITORY corrosion-rs/corrosion VERSION 0.6.1)

get_filename_component(F3RT_CARGO_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../Cargo.toml" ABSOLUTE)

# Imports the target `f3rt_abi`.
corrosion_import_crate(MANIFEST_PATH "${F3RT_CARGO_MANIFEST}" CRATES f3rt-abi)
corrosion_add_cxxbridge(f3rt_video_cxx CRATE f3rt_abi FILES video.rs)
corrosion_add_cxxbridge(f3rt_sprites_cxx CRATE f3rt_abi FILES sprites.rs)

# The renderer links SDL3 directly (sdl3-sys with `no-link`); CMake provides the library so every
# binary linked against f3rt_abi resolves SDL_*. Corrosion does not propagate Rust
# `native-static-libs`, so the Objective-C frameworks used by the display-link pacer are named
# explicitly.
if(F3RT_SDL AND F3RT_GPU)
    corrosion_set_features(f3rt_abi FEATURES gpu)
    target_link_libraries(f3rt_abi INTERFACE SDL3::SDL3)
    if(APPLE)
        target_link_libraries(f3rt_abi INTERFACE "-framework AppKit" "-framework QuartzCore" "-framework CoreFoundation")
    endif()
endif()

# The generated bridge header includes the host adaptor declarations, which pull in the
# renderer headers they wrap.
target_include_directories(f3rt_video_cxx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_include_directories(f3rt_sprites_cxx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
