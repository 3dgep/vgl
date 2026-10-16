# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# OpenGL sample (design document, section 11): the OpenGL backend in a GLFW
# window, on OpenGL 3.3 or, with --es, on OpenGL ES 3.0. With --check, it is
# also the test of the backend.

add_executable(vgl_gl_sample main.cpp ../common/sample_camera.hpp)
set_target_properties(vgl_gl_sample PROPERTIES OUTPUT_NAME vgl-gl)
target_include_directories(vgl_gl_sample PRIVATE ../common)
target_link_libraries(vgl_gl_sample PRIVATE vgl::vgl vgl::defaults vgl::gl glfw)
target_compile_definitions(vgl_gl_sample PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data")
vgl_target_options(vgl_gl_sample)

if(VGL_BUILD_TESTS AND VGL_TEST_GL)
    add_test(NAME gl COMMAND vgl_gl_sample --check)
    add_test(NAME gl_es COMMAND vgl_gl_sample --check --es)
endif()
