# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# OpenGL sample (design document, section 11): the OpenGL backend in a GLFW
# window, on OpenGL 3.3 or, with --es, on OpenGL ES 3.0, with an overlay of
# statistics and debug views. With --check, it is also the test of the backend.

# Its overlay of statistics and debug controls uses Dear ImGui.
if(NOT TARGET vgl_imgui_glfw_gl3)
    message(STATUS "vgl: Dear ImGui is not available (VGL_FETCH_DEPENDENCIES is off); skipping the OpenGL sample")
    return()
endif()

add_executable(vgl_gl_sample main.cpp)
# The executable's import library takes another name than the backend's library: with the
# same, MSBuild takes the backend's library for the executable's own output, and stops relinking
# the executable when it changes.
set_target_properties(vgl_gl_sample PROPERTIES OUTPUT_NAME vgl-gl ARCHIVE_OUTPUT_NAME vgl-gl-sample)
target_link_libraries(vgl_gl_sample PRIVATE
    vgl_sample_glfw vgl_sample_common vgl_sample_overlay vgl::vgl vgl::defaults vgl::gl glfw vgl_imgui_glfw_gl3)
target_compile_definitions(vgl_gl_sample PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data")
vgl_target_options(vgl_gl_sample)

if(VGL_BUILD_TESTS AND VGL_TEST_GL)
    # The checks expect the sample set, wherever VGL_SAMPLE_DATA points.
    add_test(NAME gl COMMAND vgl_gl_sample --check --data "${PROJECT_SOURCE_DIR}/samples/data")
    add_test(NAME gl_es
        COMMAND vgl_gl_sample --check --es --data "${PROJECT_SOURCE_DIR}/samples/data")
endif()
