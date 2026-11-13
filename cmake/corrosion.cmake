# The Rust runtime crates (audio, eeprom) reach C++ through one staticlib, crates/f3rt-abi: each
# staticlib embeds its own copy of std, so linking two would duplicate symbols. The other crates are
# rlib dependencies of f3rt-abi. Nix supplies the Corrosion source via CPM_Corrosion_SOURCE.
CPMAddPackage(NAME Corrosion GITHUB_REPOSITORY corrosion-rs/corrosion VERSION 0.6.1)

get_filename_component(F3RT_CARGO_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../Cargo.toml" ABSOLUTE)

# Imports the target `f3rt_abi`.
corrosion_import_crate(MANIFEST_PATH "${F3RT_CARGO_MANIFEST}" CRATES f3rt-abi)
