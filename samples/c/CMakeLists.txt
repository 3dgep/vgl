# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Pure-C sample (design document, section 11): the OpenGL sample's core in
# C99, as a C host integrates vgl. CI builds it and runs its check.

add_executable(vgl_c_sample main.c)
set_target_properties(vgl_c_sample PROPERTIES
    OUTPUT_NAME vgl-c
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)
target_link_libraries(vgl_c_sample PRIVATE vgl::vgl vgl::defaults vgl::gl glfw)
if(UNIX)
    target_link_libraries(vgl_c_sample PRIVATE m)
endif()
target_compile_definitions(vgl_c_sample PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data")
vgl_target_options(vgl_c_sample)

if(VGL_BUILD_TESTS AND VGL_TEST_GL)
    # The check expects the sample set, wherever VGL_SAMPLE_DATA points.
    add_test(NAME gl_c COMMAND vgl_c_sample --check --data "${PROJECT_SOURCE_DIR}/samples/data")
endif()
