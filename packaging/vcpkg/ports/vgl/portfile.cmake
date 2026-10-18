# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# vcpkg port of vgl (packaging/README.md). Zstandard and stb_image come from their own ports,
# so nothing is downloaded during the build.

# VGL_PORT_SOURCE_DIR builds a working tree instead of the release, for testing the port
# before a tag exists (with VCPKG_KEEP_ENV_VARS=VGL_PORT_SOURCE_DIR). The port submitted to
# the vcpkg registry leaves this branch out.
if(DEFINED ENV{VGL_PORT_SOURCE_DIR})
    file(TO_CMAKE_PATH "$ENV{VGL_PORT_SOURCE_DIR}" SOURCE_PATH)
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO 3dgep/vgl
        REF "v${VERSION}"
        SHA512 0
        HEAD_REF main)
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        defaults VGL_BUILD_DEFAULTS
        http     VGL_DEFAULTS_HTTP
        gl       VGL_BACKEND_GL
        d3d11    VGL_BACKEND_D3D11
        ogre     VGL_BACKEND_OGRE)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DVGL_BUILD_TESTS=OFF
        -DVGL_BUILD_SAMPLES=OFF
        -DVGL_BUILD_BAKER=OFF
        -DVGL_INSTALL=ON
        -DVGL_FETCH_DEPENDENCIES=OFF
        -DVGL_ZSTD_PROVIDER=SYSTEM)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/vgl")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
