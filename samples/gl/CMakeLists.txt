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
vgl_target_options(vgl_gl_sample)

if(EMSCRIPTEN)
    # A page (web/README.md): vgl-gl.html with its script and WebAssembly. The sample set's
    # packs are fetched from data/packs next to the page, where web/Caddyfile serves them; the
    # clouds come in the page's package, at the same path.
    set_target_properties(vgl_gl_sample PROPERTIES SUFFIX ".html")
    target_compile_definitions(vgl_gl_sample PRIVATE VGL_SAMPLE_DATA_DIR="data" VGL_SAMPLE_PAGE)
    target_link_options(vgl_gl_sample PRIVATE
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sGL_ENABLE_GET_PROC_ADDRESS"
        # The thread pool's workers, started with the page.
        "-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency"
        # The context's CPU budget, the meshes and the clouds; growing memory with threads is
        # slow, so the page starts with what it needs.
        "-sINITIAL_MEMORY=768MB"
        "-sSTACK_SIZE=1MB"
        "-sENVIRONMENT=web,worker"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
        "--preload-file=${PROJECT_SOURCE_DIR}/samples/data/clouds.jpg@/data/clouds.jpg")
    set_property(TARGET vgl_gl_sample APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html" "${PROJECT_SOURCE_DIR}/samples/data/clouds.jpg")
else()
    target_compile_definitions(vgl_gl_sample PRIVATE
        VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data")
endif()

if(VGL_BUILD_TESTS AND VGL_TEST_GL)
    # The checks expect the sample set, wherever VGL_SAMPLE_DATA points.
    add_test(NAME gl COMMAND vgl_gl_sample --check --data "${PROJECT_SOURCE_DIR}/samples/data")
    add_test(NAME gl_es
        COMMAND vgl_gl_sample --check --es --data "${PROJECT_SOURCE_DIR}/samples/data")
endif()
