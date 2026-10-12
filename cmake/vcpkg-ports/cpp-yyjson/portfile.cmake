vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO yosh-matsuda/cpp-yyjson
    REF v1.1.0
    SHA512 17450480c859db95d33a816c4e10a3e90b8d373ff638dd777f98f2f8d5a5d61db6ad31f0b0ade7832b56e83cde6793d6f276987cf0802117616dcf1d076fb9bc
    HEAD_REF main
)

# Use the vcpkg yyjson port instead of the bundled copy.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCPPYYJSON_USE_BUNDLED_YYJSON=OFF
        -DCPPYYJSON_BUILD_TEST=OFF
        -DCPPYYJSON_BUILD_BENCH=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME cpp_yyjson CONFIG_PATH lib/cmake/cpp_yyjson)

# Header-only: no compiled libraries, and debug headers are duplicates.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug" "${CURRENT_PACKAGES_DIR}/lib")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
