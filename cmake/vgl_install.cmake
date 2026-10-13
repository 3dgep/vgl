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

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/vgl"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${PROJECT_BINARY_DIR}/include/vgl"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT vgl-targets
    NAMESPACE vgl::
    DESTINATION "${VGL_INSTALL_CMAKEDIR}")

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
# A static vgl is C++, so C consumers linking it need the C++ runtime.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
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
