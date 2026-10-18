# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Install rules: library, headers, CMake package and pkg-config file (section 12.1).

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(VGL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/vgl")

install(TARGETS vgl
    EXPORT vgl-targets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The defaults module, as vgl::defaults in the same package.
if(TARGET vgl_defaults)
    install(TARGETS vgl_defaults
        EXPORT vgl-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

# The OpenGL backend, as vgl::gl in the same package. Each backend's headers live with the
# backend and are installed only with it.
if(TARGET vgl_gl)
    install(TARGETS vgl_gl
        EXPORT vgl-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/backends/gl/include/vgl"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# The Direct3D 11 backend, as vgl::d3d11 in the same package. Windows has no pkg-config
# convention, so it has no .pc file.
if(TARGET vgl_d3d11)
    install(TARGETS vgl_d3d11
        EXPORT vgl-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/backends/d3d11/include/vgl"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# The Ogre backend, as vgl::ogre, only with an installed Ogre: a downloaded one is built into
# vgl's build, not installed, so the package could not name it.
if(TARGET vgl_ogre AND VGL_OGRE_INSTALLED)
    install(TARGETS vgl_ogre
        EXPORT vgl-targets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/backends/ogre/include/vgl"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/vgl"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Licenses (design document, section 12.3): vgl's, and those of the code compiled into it:
# stb_image always, Zstandard when it was downloaded, and curl when a downloaded one is linked
# into a shared defaults module. stb_image's license is the comment that ends its header.
set(VGL_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/vgl")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${VGL_INSTALL_DOCDIR}")
if(VGL_ZSTD_BUNDLED)
    install(FILES "${vgl_zstd_source_SOURCE_DIR}/LICENSE"
        DESTINATION "${VGL_INSTALL_DOCDIR}" RENAME LICENSE.zstd)
endif()
if(TARGET libcurl_static AND BUILD_SHARED_LIBS)
    FetchContent_GetProperties(vgl_curl_source)
    install(FILES "${vgl_curl_source_SOURCE_DIR}/COPYING"
        DESTINATION "${VGL_INSTALL_DOCDIR}" RENAME LICENSE.curl)
endif()
file(READ "${VGL_STB_INCLUDE_DIR}/stb_image.h" vgl_stb_header)
string(FIND "${vgl_stb_header}" "This software is available under 2 licenses" vgl_stb_license_start)
if(vgl_stb_license_start GREATER -1)
    string(FIND "${vgl_stb_header}" "*/" vgl_stb_license_end REVERSE)
    math(EXPR vgl_stb_license_length "${vgl_stb_license_end} - ${vgl_stb_license_start}")
    string(SUBSTRING "${vgl_stb_header}" ${vgl_stb_license_start} ${vgl_stb_license_length}
        vgl_stb_license)
    file(WRITE "${PROJECT_BINARY_DIR}/LICENSE.stb" "${vgl_stb_license}")
    install(FILES "${PROJECT_BINARY_DIR}/LICENSE.stb" DESTINATION "${VGL_INSTALL_DOCDIR}")
endif()
install(DIRECTORY "${PROJECT_BINARY_DIR}/include/vgl"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT vgl-targets
    NAMESPACE vgl::
    DESTINATION "${VGL_INSTALL_CMAKEDIR}")

# A static vgl linked to an installed Zstandard passes that dependency on to its
# consumers. A downloaded Zstandard is compiled into vgl and needs nothing.
set(VGL_CONFIG_DEPENDENCIES "")
set(VGL_PC_REQUIRES_PRIVATE "")
if(NOT VGL_ZSTD_BUNDLED)
    set(VGL_PC_REQUIRES_PRIVATE "libzstd")
    if(NOT BUILD_SHARED_LIBS)
        set(VGL_CONFIG_DEPENDENCIES "find_dependency(zstd CONFIG)")
    endif()
endif()

# A static defaults module passes its threads on, as Threads::Threads.
if(TARGET vgl_defaults AND NOT BUILD_SHARED_LIBS)
    string(APPEND VGL_CONFIG_DEPENDENCIES "
find_dependency(Threads)")
endif()

# A static defaults module with the HTTP reader passes libcurl on, found as CURL::libcurl.
if(TARGET vgl_defaults AND VGL_DEFAULTS_HTTP AND NOT BUILD_SHARED_LIBS)
    string(APPEND VGL_CONFIG_DEPENDENCIES "
find_dependency(CURL)")
endif()

# vgl::ogre links Ogre's OgreMain, which its consumers find as vgl's build did.
if(TARGET vgl_ogre AND VGL_OGRE_INSTALLED)
    string(APPEND VGL_CONFIG_DEPENDENCIES "
find_dependency(OGRE 14 CONFIG)")
endif()

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/vgl-config.cmake.in"
    "${PROJECT_BINARY_DIR}/vgl-config.cmake"
    INSTALL_DESTINATION "${VGL_INSTALL_CMAKEDIR}")

# Before 1.0, minor versions may break compatibility (section 13.3).
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(VGL_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set(VGL_VERSION_COMPATIBILITY SameMajorVersion)
endif()
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/vgl-config-version.cmake"
    COMPATIBILITY ${VGL_VERSION_COMPATIBILITY})

install(FILES
    "${PROJECT_BINARY_DIR}/vgl-config.cmake"
    "${PROJECT_BINARY_DIR}/vgl-config-version.cmake"
    DESTINATION "${VGL_INSTALL_CMAKEDIR}")

# pkg-config, since C projects often use it instead of CMake packages. The
# prefix is derived from the .pc file location, so the install stays relocatable.
file(RELATIVE_PATH VGL_PC_RELATIVE_PREFIX
    "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
string(REGEX REPLACE "/$" "" VGL_PC_RELATIVE_PREFIX "${VGL_PC_RELATIVE_PREFIX}")
if(BUILD_SHARED_LIBS)
    set(VGL_PC_CFLAGS "-DVGL_SHARED")
else()
    set(VGL_PC_CFLAGS "")
endif()
# A static vgl is C++, so C consumers linking it need the C++ runtime. On
# Android that is the NDK's libc++, which its toolchain links.
if(ANDROID)
    set(VGL_PC_LIBS_PRIVATE "-lm")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    set(VGL_PC_LIBS_PRIVATE "-lstdc++ -lm")
else()
    set(VGL_PC_LIBS_PRIVATE "")
endif()
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/vgl.pc.in"
    "${PROJECT_BINARY_DIR}/vgl.pc"
    @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/vgl.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(TARGET vgl_defaults)
    # The defaults module uses threads; a static build passes that on.
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        set(VGL_DEFAULTS_PC_LIBS_PRIVATE "-pthread")
    else()
        set(VGL_DEFAULTS_PC_LIBS_PRIVATE "")
    endif()
    if(VGL_DEFAULTS_HTTP)
        set(VGL_DEFAULTS_PC_REQUIRES_PRIVATE "libcurl")
    else()
        set(VGL_DEFAULTS_PC_REQUIRES_PRIVATE "")
    endif()
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/vgl-defaults.pc.in"
        "${PROJECT_BINARY_DIR}/vgl-defaults.pc"
        @ONLY)
    install(FILES "${PROJECT_BINARY_DIR}/vgl-defaults.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()

if(TARGET vgl_gl)
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/vgl-gl.pc.in"
        "${PROJECT_BINARY_DIR}/vgl-gl.pc"
        @ONLY)
    install(FILES "${PROJECT_BINARY_DIR}/vgl-gl.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()
