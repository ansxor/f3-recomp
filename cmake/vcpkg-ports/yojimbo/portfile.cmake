vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mas-bandwidth/yojimbo
    REF 272153a10f32135bb44bb60e7467072baf48f762
    SHA512 8b3a9244702bb3767c2ad5501d72386a986af6e50f819c2145f0a260544e19b94af44ac55bd2a556baf442fdb975eba4384863315ec4b1cce64f953a30adf02d
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DYOJIMBO_SYSTEM_SODIUM=OFF
        -DYOJIMBO_BUILD_TESTS=OFF
        -DYOJIMBO_INSTALL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Yojimbo)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Bundle yojimbo (BSD-3-Clause), bundled libsodium (ISC), and bundled TLSF (BSD-3-Clause) licenses
file(READ "${SOURCE_PATH}/LICENCE" yojimbo_license)
file(WRITE "${CURRENT_BUILDTREES_DIR}/LICENSE"
"${yojimbo_license}

=== Bundled libsodium (sodium/sodium.c) ===

ISC License
Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED \"AS IS\" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

=== Bundled TLSF (tlsf/tlsf.h) ===

Copyright (c) 2006-2016, Matthew Conte
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.
"
)
vcpkg_install_copyright(FILE_LIST "${CURRENT_BUILDTREES_DIR}/LICENSE")
