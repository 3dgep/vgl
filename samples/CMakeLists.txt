# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Samples (design document, section 11): OpenGL, OpenGL ES, Direct3D 11, Ogre,
# pure C and headless. They are added from M2 onwards; the headless sample
# needs the defaults module.

if(TARGET vgl_defaults)
    add_subdirectory(headless)
endif()
