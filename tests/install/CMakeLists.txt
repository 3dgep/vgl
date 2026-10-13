# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Consumer project that checks the installed CMake package. CI configures it
# with CMAKE_PREFIX_PATH pointing at a vgl install; it is not part of the main
# build.

cmake_minimum_required(VERSION 3.24)
project(vgl_install_test LANGUAGES C)

find_package(vgl REQUIRED)

add_executable(vgl_install_test "${CMAKE_CURRENT_SOURCE_DIR}/../api_c.c")
target_link_libraries(vgl_install_test PRIVATE vgl::vgl)
