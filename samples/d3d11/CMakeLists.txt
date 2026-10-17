# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Direct3D 11 sample (design document, section 11): the Direct3D 11 backend in a
# plain Win32 window, with the overlay of statistics and debug views that the
# OpenGL sample has. With --check, it is also the test of the backend.

if(NOT TARGET vgl_imgui_win32_dx11)
    message(STATUS "vgl: Dear ImGui is not available (VGL_FETCH_DEPENDENCIES is off); skipping the Direct3D 11 sample")
    return()
endif()

add_executable(vgl_d3d11_sample main.cpp)
# The executable's import library takes another name than the backend's library: with the
# same, MSBuild takes the backend's library for the executable's own output, and stops relinking
# the executable when it changes.
set_target_properties(vgl_d3d11_sample PROPERTIES OUTPUT_NAME vgl-d3d11 ARCHIVE_OUTPUT_NAME vgl-d3d11-sample)
target_link_libraries(vgl_d3d11_sample PRIVATE
    vgl_sample_common vgl_sample_overlay vgl::vgl vgl::defaults vgl::d3d11 vgl_imgui_win32_dx11
    dxgi)
target_compile_definitions(vgl_d3d11_sample PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data" NOMINMAX WIN32_LEAN_AND_MEAN)
vgl_target_options(vgl_d3d11_sample)

if(VGL_BUILD_TESTS)
    # The check draws offscreen, on the hardware device or else on WARP, which every Windows
    # machine has, so it needs no display. It expects the sample set.
    add_test(NAME d3d11 COMMAND vgl_d3d11_sample --check --data "${PROJECT_SOURCE_DIR}/samples/data")
endif()
