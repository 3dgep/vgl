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

# The defaults module and the backends are in the package too, under the names the build
# tree's aliases have: every preset builds them, and the Direct3D 11 backend on Windows.
set(vgl_expected_targets vgl::defaults vgl::gl)
if(WIN32)
    list(APPEND vgl_expected_targets vgl::d3d11)
endif()
foreach(target IN LISTS vgl_expected_targets)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "The installed vgl package has no ${target}")
    endif()
    target_link_libraries(vgl_install_test PRIVATE ${target})
endforeach()
