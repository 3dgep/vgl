# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# OpenGL backend: the common subset of OpenGL 3.3 core and OpenGL ES 3.0
# (design document, section 7.6). Context creation belongs to the host, which
# passes a function loader, so the backend links no OpenGL library. Like the
# defaults module, it uses vgl only through the C API, so it works with a
# shared vgl as well.

add_library(vgl_gl
    backend.cpp
    gl_api.cpp
    gl_api.hpp
    shaders.hpp
    "${PROJECT_SOURCE_DIR}/src/common/host_memory.hpp"
    "${PROJECT_SOURCE_DIR}/src/common/pointer_map.hpp")
add_library(vgl::gl ALIAS vgl_gl)

target_include_directories(vgl_gl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vgl_gl PUBLIC vgl)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(vgl_gl PRIVATE VGL_GL_BUILDING)
endif()

set_target_properties(vgl_gl PROPERTIES
    OUTPUT_NAME vgl-gl
    EXPORT_NAME gl
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

vgl_target_options(vgl_gl)
vgl_disable_exceptions(vgl_gl)
