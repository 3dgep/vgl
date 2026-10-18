# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Consumer project that checks an installed vgl: the CMake package from a vgl install, a
# vcpkg port or a Conan package, and the release archives. Configure it with
# CMAKE_PREFIX_PATH pointing at the install (or a package manager's toolchain), build it,
# and run ctest. It is not part of the main build.
#
# VGL_EXPECTED_TARGETS lists the targets the package must have besides vgl::vgl; by
# default those every preset installs: vgl::defaults and vgl::gl, and vgl::d3d11 on
# Windows. A package built with fewer passes its own list (an empty one for the core only).

cmake_minimum_required(VERSION 3.24)
project(vgl_install_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vgl REQUIRED)

set(vgl_default_targets vgl::defaults vgl::gl)
if(WIN32)
    list(APPEND vgl_default_targets vgl::d3d11)
endif()
set(VGL_EXPECTED_TARGETS "${vgl_default_targets}" CACHE STRING
    "Targets the vgl package must have besides vgl::vgl")

# The C API, through vgl::vgl and the expected targets' headers.
add_executable(vgl_install_test_c "${CMAKE_CURRENT_SOURCE_DIR}/../api_c.c")
target_link_libraries(vgl_install_test_c PRIVATE vgl::vgl)

# The C++ wrappers, which need only the core.
add_executable(vgl_install_test_cpp "${CMAKE_CURRENT_SOURCE_DIR}/../api_cpp.cpp")
target_link_libraries(vgl_install_test_cpp PRIVATE vgl::vgl)

foreach(target IN LISTS VGL_EXPECTED_TARGETS)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "The installed vgl package has no ${target}")
    endif()
    target_link_libraries(vgl_install_test_c PRIVATE ${target})
endforeach()
if(vgl::gl IN_LIST VGL_EXPECTED_TARGETS)
    target_compile_definitions(vgl_install_test_c PRIVATE VGL_TEST_GL_HEADER)
endif()

# A shared vgl on Windows: its DLLs go next to the executables, so the tests run.
get_target_property(vgl_type vgl::vgl TYPE)
if(WIN32 AND vgl_type STREQUAL "SHARED_LIBRARY")
    foreach(test vgl_install_test_c vgl_install_test_cpp)
        add_custom_command(TARGET ${test} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${test}> $<TARGET_FILE_DIR:${test}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

enable_testing()
add_test(NAME api_c COMMAND vgl_install_test_c)
add_test(NAME api_cpp COMMAND vgl_install_test_cpp)
