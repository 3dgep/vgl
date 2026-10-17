# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Backends (design document, section 7.6). Each backend is its own target
# (vgl_gl, vgl_d3d11, vgl_ogre) linking vgl, implemented against the public
# render interface like any third-party backend (section 13.1).

# The atmosphere's shader code, which every backend shares (design document, section 7.6). The
# Direct3D 11 backend includes it in its HLSL; the others build their shaders at run time and
# get it as a C++ string, in generated/atmosphere_shader.hpp under this directory's build tree.
set(VGL_ATMOSPHERE_GLSL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/common/atmosphere.glsl")
set(VGL_GENERATED_BACKEND_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VGL_ATMOSPHERE_GLSL_FILE}")
file(READ "${VGL_ATMOSPHERE_GLSL_FILE}" VGL_ATMOSPHERE_GLSL)
configure_file(common/atmosphere_shader.hpp.in
    "${VGL_GENERATED_BACKEND_DIR}/atmosphere_shader.hpp" @ONLY)

if(VGL_BACKEND_GL)
    add_subdirectory(gl)
endif()

if(VGL_BACKEND_D3D11)
    add_subdirectory(d3d11)
endif()

if(VGL_BACKEND_OGRE)
    add_subdirectory(ogre)
endif()
