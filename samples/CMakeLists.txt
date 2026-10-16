# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Samples (design document, section 11): OpenGL, OpenGL ES, Direct3D 11, Ogre,
# pure C and headless. They are added from M2 onwards; every sample needs the
# defaults module.

if(NOT TARGET vgl_defaults)
    return()
endif()

add_subdirectory(headless)

# The OpenGL samples need GLFW for their window and context. GLFW is used only
# by the samples: the library never creates windows or contexts. It is found
# first, and otherwise downloaded at a pinned version with a checked hash. GLFW
# is for desktops, so Android and iOS build no OpenGL sample.
if(TARGET vgl_gl AND NOT ANDROID AND NOT IOS)
    set(VGL_GLFW_VERSION 3.4)
    set(VGL_GLFW_URL "https://github.com/glfw/glfw/releases/download/${VGL_GLFW_VERSION}/glfw-${VGL_GLFW_VERSION}.zip")
    set(VGL_GLFW_SHA256 b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9)

    find_package(glfw3 3.3 CONFIG QUIET)
    if(glfw3_FOUND)
        message(STATUS "vgl: using installed GLFW ${glfw3_VERSION} for the samples")
    elseif(VGL_FETCH_DEPENDENCIES)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(vgl_glfw
            URL "${VGL_GLFW_URL}"
            URL_HASH SHA256=${VGL_GLFW_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        # GLFW is built as a static library whatever vgl is, so the samples need
        # no extra DLL beside them.
        set(vgl_saved_shared ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(vgl_glfw)
        set(BUILD_SHARED_LIBS ${vgl_saved_shared})
        set_target_properties(glfw PROPERTIES FOLDER third_party)
        message(STATUS "vgl: using downloaded GLFW ${VGL_GLFW_VERSION} for the samples")
    endif()

    if(TARGET glfw)
        # The check needs a display and a driver, which not every build machine
        # has, so it is a test only on request. CI runs it under Xvfb with Mesa.
        option(VGL_TEST_GL "Run the OpenGL samples' checks as tests (needs a display)" OFF)
        add_subdirectory(gl)
        add_subdirectory(c)
    else()
        message(STATUS "vgl: GLFW not found and VGL_FETCH_DEPENDENCIES is off; skipping the OpenGL sample")
    endif()
endif()
