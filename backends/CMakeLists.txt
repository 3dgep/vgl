# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Backends (design document, section 7.6). Each backend is its own target
# (vgl_gl, vgl_d3d11, vgl_ogre) linking vgl, implemented against the public
# render interface like any third-party backend (section 13.1).

if(VGL_BACKEND_GL)
    add_subdirectory(gl)
endif()

if(VGL_BACKEND_D3D11)
    add_subdirectory(d3d11)
endif()

if(VGL_BACKEND_OGRE)
    add_subdirectory(ogre)
endif()
