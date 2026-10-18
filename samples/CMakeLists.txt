# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Samples (design document, section 11): OpenGL, OpenGL ES, Direct3D 11, Ogre,
# pure C and headless. They are added from M2 onwards; every sample needs the
# defaults module.

if(NOT TARGET vgl_defaults)
    return()
endif()

# Code that the samples share: the camera, and navigation in the style of Google
# Earth, which every interactive sample uses. It has no window or graphics API
# of its own, so samples on any backend can use it.
add_library(vgl_sample_common STATIC
    common/navigation.cpp
    common/navigation.hpp
    common/sample_camera.hpp
    common/sample_check.cpp
    common/sample_check.hpp
    common/sample_data.hpp
    common/stars.cpp
    common/stars.hpp)
target_include_directories(vgl_sample_common PUBLIC common)
target_link_libraries(vgl_sample_common PUBLIC vgl::vgl)
vgl_target_options(vgl_sample_common)

# The clouds' image, read with a private copy of stb_image, third-party code compiled without
# vgl's warning flags.
add_library(vgl_sample_clouds STATIC common/clouds.cpp common/clouds.hpp)
target_include_directories(vgl_sample_clouds PUBLIC common)
target_include_directories(vgl_sample_clouds SYSTEM PRIVATE "${VGL_STB_INCLUDE_DIR}")
target_compile_features(vgl_sample_clouds PUBLIC cxx_std_20)
target_link_libraries(vgl_sample_common PUBLIC vgl_sample_clouds)

add_subdirectory(headless)

# Dear ImGui draws the interactive samples' statistics and debug controls, in an overlay they
# share (common/overlay.hpp), with each sample's platform and renderer backends. It is a
# sample-only dependency, downloaded at a pinned release with a checked hash. The samples are
# for desktops, so Android and iOS build none.
if(VGL_FETCH_DEPENDENCIES AND NOT ANDROID AND NOT IOS AND NOT TARGET vgl_imgui)
    include(vgl_imgui)
    vgl_imgui_fetch(vgl_imgui_source vgl_imgui_dir)
    add_library(vgl_imgui STATIC
        "${vgl_imgui_dir}/imgui.cpp"
        "${vgl_imgui_dir}/imgui_draw.cpp"
        "${vgl_imgui_dir}/imgui_tables.cpp"
        "${vgl_imgui_dir}/imgui_widgets.cpp")
    target_include_directories(vgl_imgui SYSTEM PUBLIC "${vgl_imgui_dir}" "${vgl_imgui_dir}/backends")
    target_compile_features(vgl_imgui PUBLIC cxx_std_11)
    set_target_properties(vgl_imgui PROPERTIES FOLDER third_party POSITION_INDEPENDENT_CODE ON)
    message(STATUS "vgl: using downloaded Dear ImGui ${VGL_IMGUI_VERSION} for the interactive samples")

    add_library(vgl_sample_overlay STATIC common/overlay.cpp common/overlay.hpp)
    target_link_libraries(vgl_sample_overlay PUBLIC vgl_sample_common vgl_imgui)
    vgl_target_options(vgl_sample_overlay)

    # The Direct3D 11 sample's platform and renderer backends: Win32 and Direct3D 11, which
    # compiles its shaders at runtime with d3dcompiler_47, a part of Windows.
    if(WIN32 AND TARGET vgl_d3d11)
        add_library(vgl_imgui_win32_dx11 STATIC
            "${vgl_imgui_dir}/backends/imgui_impl_win32.cpp"
            "${vgl_imgui_dir}/backends/imgui_impl_dx11.cpp")
        target_link_libraries(vgl_imgui_win32_dx11 PUBLIC vgl_imgui d3d11 d3dcompiler dxgi dwmapi)
        set_target_properties(vgl_imgui_win32_dx11 PROPERTIES FOLDER third_party)
    endif()
endif()

if(TARGET vgl_d3d11)
    add_subdirectory(d3d11)
endif()

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

    # The OpenGL samples' Dear ImGui backends: GLFW and OpenGL 3.
    if(TARGET glfw AND TARGET vgl_imgui AND NOT TARGET vgl_imgui_glfw_gl3)
        add_library(vgl_imgui_glfw_gl3 STATIC
            "${vgl_imgui_dir}/backends/imgui_impl_glfw.cpp"
            "${vgl_imgui_dir}/backends/imgui_impl_opengl3.cpp")
        target_link_libraries(vgl_imgui_glfw_gl3 PUBLIC vgl_imgui glfw)
        # Its OpenGL backend loads its own functions; without this, GLFW's header would include
        # the system's GL/gl.h, which not every machine has.
        target_compile_definitions(vgl_imgui_glfw_gl3 PRIVATE GLFW_INCLUDE_NONE)
        set_target_properties(vgl_imgui_glfw_gl3 PROPERTIES FOLDER third_party POSITION_INDEPENDENT_CODE ON)
    endif()

    # The controls of the samples in a GLFW window.
    if(TARGET glfw AND NOT TARGET vgl_sample_glfw)
        add_library(vgl_sample_glfw STATIC common/glfw_controls.cpp common/glfw_controls.hpp)
        target_link_libraries(vgl_sample_glfw PUBLIC vgl_sample_common glfw)
        vgl_target_options(vgl_sample_glfw)
    endif()

    if(TARGET glfw)
        # The check needs a display and a driver, which not every build machine
        # has, so it is a test only on request. CI runs it under Xvfb with Mesa.
        option(VGL_TEST_GL "Run the OpenGL samples' checks as tests (needs a display)" OFF)
        add_subdirectory(gl)
        add_subdirectory(c)
        if(TARGET vgl_ogre)
            add_subdirectory(ogre)
        endif()
    else()
        message(STATUS "vgl: GLFW not found and VGL_FETCH_DEPENDENCIES is off; skipping the OpenGL sample")
    endif()
endif()
