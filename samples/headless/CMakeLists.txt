# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Headless sample (design document, section 11): a scripted camera, loading,
# selection and frames without a GPU, with statistics. With --check, it is also
# a test of the whole runtime on the sample data.

add_executable(vgl_headless main.cpp)
set_target_properties(vgl_headless PROPERTIES OUTPUT_NAME vgl-headless)
target_link_libraries(vgl_headless PRIVATE vgl_sample_common vgl::vgl vgl::defaults)
target_compile_definitions(vgl_headless PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data")
vgl_target_options(vgl_headless)
if(EMSCRIPTEN)
    # A program for Node.js: its main on a worker, so that it may wait for its threads, and the
    # real filesystem for the sample data.
    target_link_options(vgl_headless PRIVATE
        "-sPROXY_TO_PTHREAD" "-sPTHREAD_POOL_SIZE=8" "-sEXIT_RUNTIME" "-sINITIAL_MEMORY=512MB"
        "-sNODERAWFS")
endif()

if(VGL_BUILD_TESTS)
    # The check expects the sample set, wherever VGL_SAMPLE_DATA points.
    add_test(NAME headless
        COMMAND vgl_headless --check --data "${PROJECT_SOURCE_DIR}/samples/data")
endif()
