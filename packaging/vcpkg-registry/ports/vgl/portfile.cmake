# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 3dgep/vgl
    REF "v${VERSION}"
    SHA512 439cf702f05ffde7c0556933c2cbd39ef346284c78e490c62ebfc87c29f7cf0f4136f0569c07adefbf3508e540847a8ed3f8933baae229ac51f40df3f3b20d50
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        defaults VGL_BUILD_DEFAULTS
        http     VGL_DEFAULTS_HTTP
        gl       VGL_BACKEND_GL
        d3d11    VGL_BACKEND_D3D11
        ogre     VGL_BACKEND_OGRE
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DVGL_BUILD_TESTS=OFF
        -DVGL_BUILD_SAMPLES=OFF
        -DVGL_BUILD_BAKER=OFF
        -DVGL_INSTALL=ON
        -DVGL_FETCH_DEPENDENCIES=OFF
        -DVGL_ZSTD_PROVIDER=SYSTEM
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/vgl")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/doc"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
