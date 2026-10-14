# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Test harness: a minimal framework with no dependencies (harness/vgl_test.hpp).
add_library(vgl_test_main STATIC harness/vgl_test_main.cpp)
target_include_directories(vgl_test_main PUBLIC harness)
vgl_target_options(vgl_test_main)

# vgl_add_internal_test(<name> <sources>...)
#
# Unit test of internal C++ code. It links the library's object files directly,
# because a shared vgl exports only the C API.
function(vgl_add_internal_test name)
    add_executable(vgl_test_${name} ${ARGN})
    target_link_libraries(vgl_test_${name} PRIVATE vgl_objects vgl_test_main)
    target_include_directories(vgl_test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    vgl_target_options(vgl_test_${name})
    add_test(NAME ${name} COMMAND vgl_test_${name})
endfunction()

vgl_add_internal_test(foundation foundation_test.cpp)
vgl_add_internal_test(geodesy geodesy_test.cpp data/geographiclib_reference.inc)
vgl_add_internal_test(tiling tiling_test.cpp)

# Public API smoke tests: the C API header compiles as C99, the C++ wrapper
# compiles as C++, and both link against the library itself.
add_executable(vgl_test_api_c api_c.c)
target_link_libraries(vgl_test_api_c PRIVATE vgl::vgl)
set_target_properties(vgl_test_api_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
vgl_target_options(vgl_test_api_c)
add_test(NAME api_c COMMAND vgl_test_api_c)

add_executable(vgl_test_api_cpp api_cpp.cpp)
target_link_libraries(vgl_test_api_cpp PRIVATE vgl::vgl)
vgl_target_options(vgl_test_api_cpp)
add_test(NAME api_cpp COMMAND vgl_test_api_cpp)
