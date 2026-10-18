# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Ogre backend, for classic Ogre 14, not Ogre-Next (design document, section
# 7.6). An installed Ogre 14 is used if CMake finds one (OGRE_DIR or
# CMAKE_PREFIX_PATH); otherwise, with VGL_FETCH_DEPENDENCIES, Ogre 14.6.1 is
# downloaded at a pinned hash and built here, statically, with only what the
# backend and its sample need.

set(VGL_OGRE_VERSION 14.6.1)
set(VGL_OGRE_URL "https://github.com/OGRECave/ogre/archive/refs/tags/v${VGL_OGRE_VERSION}.tar.gz")
set(VGL_OGRE_SHA256 ec95dfd25c01904baa254316d9416c5acc348f58fde7e16570f698b0fdb1c87e)

find_package(OGRE 14 CONFIG QUIET)
# Whether the backend links an installed Ogre, which the install rules need: only then is
# vgl::ogre installed (cmake/vgl_install.cmake), since a downloaded Ogre is not.
set(VGL_OGRE_INSTALLED ${OGRE_FOUND} CACHE INTERNAL "vgl::ogre links an installed Ogre")
if(OGRE_FOUND)
    set(VGL_OGRE_MEDIA_DIR "${OGRE_MEDIA_DIR}" CACHE INTERNAL "Ogre's media")
    message(STATUS "vgl: using installed Ogre ${OGRE_VERSION}")
elseif(VGL_FETCH_DEPENDENCIES)
    # Only what the backend and the sample use: the core, the OpenGL 3+ and Direct3D 11 render
    # systems, the shader system, Bites, the overlay with Dear ImGui, and the stb image codec.
    # Ogre's own download of its dependencies is off; none of them is needed.
    set(ogre_on
        OGRE_STATIC
        OGRE_BUILD_RENDERSYSTEM_GL3PLUS
        OGRE_BUILD_COMPONENT_BITES
        OGRE_BUILD_COMPONENT_OVERLAY
        OGRE_BUILD_COMPONENT_OVERLAY_IMGUI
        OGRE_BUILD_COMPONENT_RTSHADERSYSTEM
        OGRE_BUILD_PLUGIN_STBI)
    set(ogre_off
        OGRE_BUILD_DEPENDENCIES
        OGRE_CONFIG_ENABLE_ZIP
        OGRE_BUILD_SAMPLES
        OGRE_BUILD_TESTS
        OGRE_BUILD_TOOLS
        OGRE_INSTALL_SAMPLES
        OGRE_INSTALL_DOCS
        OGRE_BUILD_RENDERSYSTEM_GL
        OGRE_BUILD_RENDERSYSTEM_GLES2
        OGRE_BUILD_RENDERSYSTEM_D3D9
        OGRE_BUILD_RENDERSYSTEM_VULKAN
        OGRE_BUILD_RENDERSYSTEM_METAL
        OGRE_BUILD_RENDERSYSTEM_TINY)
    foreach(p BSP OCTREE PCZ PFX DOT_SCENE ASSIMP GLSLANG EXRCODEC CG FREEIMAGE RSIMAGE)
        list(APPEND ogre_off OGRE_BUILD_PLUGIN_${p})
    endforeach()
    foreach(c TERRAIN PAGING VOLUME PROPERTY MESHLODGEN PYTHON JAVA CSHARP BULLET)
        list(APPEND ogre_off OGRE_BUILD_COMPONENT_${c})
    endforeach()
    foreach(option IN LISTS ogre_on)
        set(${option} ON CACHE BOOL "" FORCE)
    endforeach()
    foreach(option IN LISTS ogre_off)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(OGRE_BUILD_RENDERSYSTEM_D3D11 ${WIN32} CACHE BOOL "" FORCE)
    # The overlay compiles a copy of vgl's pinned Dear ImGui of its own, into which it writes
    # its own imconfig.h.
    include(vgl_imgui)
    vgl_imgui_fetch(vgl_ogre_imgui_source ogre_imgui_dir)
    set(IMGUI_DIR "${ogre_imgui_dir}" CACHE PATH "" FORCE)

    FetchContent_Declare(vgl_ogre_source
        URL "${VGL_OGRE_URL}"
        URL_HASH SHA256=${VGL_OGRE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # Ogre's own cache variables and build settings stay Ogre's, and its targets are third party.
    set(vgl_saved_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(vgl_ogre_source)
    set(BUILD_SHARED_LIBS ${vgl_saved_shared})
    set(VGL_OGRE_MEDIA_DIR "${vgl_ogre_source_SOURCE_DIR}/Media" CACHE INTERNAL "Ogre's media")

    # Ogre's headers are third party: their warnings are not vgl's, which treats its own as
    # errors.
    foreach(target OgreMain OgreRTShaderSystem OgreOverlay OgreBites RenderSystem_GL3Plus
                   RenderSystem_Direct3D11 Codec_STBI)
        if(TARGET ${target})
            get_target_property(includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
            if(includes)
                set_target_properties(${target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${includes}" FOLDER third_party/ogre)
            endif()
        endif()
    endforeach()
    message(STATUS "vgl: using downloaded Ogre ${VGL_OGRE_VERSION}, built statically")
else()
    message(WARNING "vgl: Ogre 14 was not found and VGL_FETCH_DEPENDENCIES is off; skipping the Ogre backend")
    return()
endif()

# A static library whatever vgl is, so that it shares the host's Ogre instead of holding a
# second copy of Ogre's singletons. It uses vgl only through the C API, and Ogre throws, so it
# keeps exceptions, catching them at its interface.
add_library(vgl_ogre STATIC
    include/vgl/ogre.hpp
    globe.cpp
    shaders.hpp
    "${PROJECT_SOURCE_DIR}/src/common/host_memory.hpp"
    "${PROJECT_SOURCE_DIR}/src/common/pointer_map.hpp")
add_library(vgl::ogre ALIAS vgl_ogre)
target_include_directories(vgl_ogre
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src" "${VGL_GENERATED_BACKEND_DIR}" "${PROJECT_SOURCE_DIR}")
target_link_libraries(vgl_ogre PUBLIC vgl OgreMain)
set_target_properties(vgl_ogre PROPERTIES OUTPUT_NAME vgl-ogre EXPORT_NAME ogre)
vgl_target_options(vgl_ogre)
