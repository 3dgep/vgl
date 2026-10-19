# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

cmake_minimum_required(VERSION 3.24)

project(vgl
    VERSION 1.0.0
    DESCRIPTION "Lightweight library for rendering a virtual globe"
    HOMEPAGE_URL "https://github.com/3dgep/vgl"
    LANGUAGES C CXX)

include(CMakeDependentOption)
include(GNUInstallDirs)

# The shared libraries' ABI version: the major version, which keeps the C ABI compatible
# (docs/c-api.md, section 14); before 1.0, the minor version too.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(VGL_SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
    set(VGL_SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# Build options. Tests, samples and installation default to on only when vgl is
# the top-level project, so that embedding it with add_subdirectory stays cheap.
option(VGL_BUILD_TESTS "Build the vgl tests" ${PROJECT_IS_TOP_LEVEL})
option(VGL_BUILD_SAMPLES "Build the vgl samples" ${PROJECT_IS_TOP_LEVEL})
option(VGL_BUILD_BAKER "Build the baking tool (requires GDAL)" OFF)
option(VGL_BUILD_DEFAULTS "Build the optional defaults module (thread pool, file reader, disk cache)" ON)
option(VGL_DEFAULTS_HTTP "Give the defaults module an HTTP reader, on libcurl" OFF)
option(VGL_INSTALL "Generate install rules, the CMake package and the pkg-config file" ${PROJECT_IS_TOP_LEVEL})

# Backends (design document, section 7.6).
option(VGL_BACKEND_GL "Build the OpenGL 3.3 / OpenGL ES 3.0 backend" ON)
cmake_dependent_option(VGL_BACKEND_D3D11 "Build the Direct3D 11 backend" ON "WIN32" OFF)
option(VGL_BACKEND_OGRE "Build the Ogre backend (classic Ogre, 1.x)" OFF)

# Library rules (NFR-05) and CI strictness.
option(VGL_DISABLE_EXCEPTIONS "Build vgl without exceptions and RTTI" OFF)
option(VGL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(vgl_target_options)
include(vgl_dependencies)

if(PROJECT_IS_TOP_LEVEL)
    # Keep executables and shared libraries together so tests and samples find
    # the vgl DLL on Windows without extra setup.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
endif()

add_subdirectory(src)
add_subdirectory(backends)

if(VGL_BUILD_BAKER OR VGL_BUILD_TESTS)
    add_subdirectory(tools/baker)
endif()

# Before the samples, which also add tests (the headless sample checks itself).
if(VGL_BUILD_TESTS)
    enable_testing()
endif()

if(VGL_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(VGL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(VGL_INSTALL)
    include(vgl_install)
endif()
