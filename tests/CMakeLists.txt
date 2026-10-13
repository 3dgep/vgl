# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Smoke tests for the build skeleton: the C API header compiles as C, the C++
# wrapper compiles as C++, and both link against the library.

add_executable(vgl_test_api_c api_c.c)
target_link_libraries(vgl_test_api_c PRIVATE vgl::vgl)
set_target_properties(vgl_test_api_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
vgl_target_options(vgl_test_api_c)
add_test(NAME api_c COMMAND vgl_test_api_c)

add_executable(vgl_test_api_cpp api_cpp.cpp)
target_link_libraries(vgl_test_api_cpp PRIVATE vgl::vgl)
vgl_target_options(vgl_test_api_cpp)
add_test(NAME api_cpp COMMAND vgl_test_api_cpp)
