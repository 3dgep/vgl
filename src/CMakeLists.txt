# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# The core library (design document, section 6.2). The modules below all build
# into the single vgl target, because the C API is one ABI and one binary. The
# optional defaults module is a separate target, since it creates threads and
# the core never does (NFR-02).

add_library(vgl)
add_library(vgl::vgl ALIAS vgl)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/include/vgl/version.h"
    @ONLY)

target_include_directories(vgl
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

if(BUILD_SHARED_LIBS)
    target_compile_definitions(vgl PUBLIC VGL_SHARED PRIVATE VGL_BUILDING)
endif()

set_target_properties(vgl PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

vgl_target_options(vgl)
vgl_disable_exceptions(vgl)

add_subdirectory(foundation)
add_subdirectory(geodesy)
add_subdirectory(tiling)
add_subdirectory(cache)
add_subdirectory(providers)
add_subdirectory(decoders)
add_subdirectory(mesh)
add_subdirectory(render)

if(VGL_BUILD_DEFAULTS)
    add_subdirectory(defaults)
endif()
