# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Ogre sample (design document, section 11): the globe in an Ogre scene, in a
# GLFW window, with the overlay of the other samples drawn by Ogre's Dear ImGui
# overlay. With --check, it is also the test of the backend.

if(NOT TARGET glfw OR NOT TARGET OgreOverlay OR NOT TARGET OgreBites)
    message(STATUS "vgl: GLFW or Ogre's overlay and Bites are not available; skipping the Ogre sample")
    return()
endif()

# The overlay and Dear ImGui's GLFW backend compile against Ogre's Dear ImGui, the one its
# overlay draws: a second copy would clash with it. The backend is third-party code, without
# vgl's warnings.
add_library(vgl_ogre_imgui_glfw STATIC "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp")
target_include_directories(vgl_ogre_imgui_glfw SYSTEM PUBLIC "${IMGUI_DIR}/backends")
target_link_libraries(vgl_ogre_imgui_glfw PUBLIC OgreOverlay glfw)
target_compile_definitions(vgl_ogre_imgui_glfw PRIVATE GLFW_INCLUDE_NONE)
set_target_properties(vgl_ogre_imgui_glfw PROPERTIES FOLDER third_party)

add_executable(vgl_ogre_sample main.cpp ../common/overlay.cpp)
# The executable's import library takes another name than the backend's library: with the
# same, MSBuild takes the backend's library for the executable's own output, and stops relinking
# the executable when it changes.
set_target_properties(vgl_ogre_sample PROPERTIES OUTPUT_NAME vgl-ogre ARCHIVE_OUTPUT_NAME vgl-ogre-sample)
target_link_libraries(vgl_ogre_sample PRIVATE
    vgl_sample_glfw vgl_sample_common vgl::vgl vgl::defaults vgl::ogre vgl_ogre_imgui_glfw glfw
    OgreMain OgreRTShaderSystem OgreOverlay OgreBites)
target_compile_definitions(vgl_ogre_sample PRIVATE
    VGL_SAMPLE_DATA_DIR="${PROJECT_SOURCE_DIR}/samples/data"
    VGL_OGRE_MEDIA_DIR="${VGL_OGRE_MEDIA_DIR}")
vgl_target_options(vgl_ogre_sample)

if(VGL_BUILD_TESTS AND VGL_TEST_GL)
    # The checks need a display, like the OpenGL samples'. They expect the sample set.
    add_test(NAME ogre_gl COMMAND vgl_ogre_sample --check --rs gl --data "${PROJECT_SOURCE_DIR}/samples/data")
    if(WIN32)
        add_test(NAME ogre_d3d11 COMMAND vgl_ogre_sample --check --rs d3d11 --data "${PROJECT_SOURCE_DIR}/samples/data")
    endif()
endif()
