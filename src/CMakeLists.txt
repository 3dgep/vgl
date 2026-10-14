# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# The core library (design document, section 6.2). The modules below all build
# into one object library, vgl_objects, from which the single vgl library is
# made: the C API is one ABI and one binary. Internal unit tests link
# vgl_objects directly, because a shared vgl exports only the C API.
#
# The optional defaults module is a separate target, since it creates threads
# and the core never does (NFR-02).

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/include/vgl/version.h"
    @ONLY)

add_library(vgl_objects OBJECT)

target_include_directories(vgl_objects
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")

if(BUILD_SHARED_LIBS)
    # Exporting the C API from the objects. Internal tests that link them define
    # the same macros, so the declarations match the definitions.
    target_compile_definitions(vgl_objects PUBLIC VGL_SHARED VGL_BUILDING)
endif()

set_target_properties(vgl_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

vgl_target_options(vgl_objects)
vgl_disable_exceptions(vgl_objects)

add_subdirectory(foundation)
add_subdirectory(geodesy)
add_subdirectory(tiling)
add_subdirectory(cache)
add_subdirectory(providers)
add_subdirectory(decoders)
add_subdirectory(mesh)
add_subdirectory(render)

add_library(vgl)
add_library(vgl::vgl ALIAS vgl)

target_sources(vgl PRIVATE $<TARGET_OBJECTS:vgl_objects>)

target_include_directories(vgl
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

if(BUILD_SHARED_LIBS)
    target_compile_definitions(vgl INTERFACE VGL_SHARED)
endif()

# vgl uses the C math library, and a static vgl also needs the C++ runtime. A C
# host links with the C linker, which adds neither, so both are part of the link
# interface (and of Libs.private in vgl.pc).
if(UNIX AND NOT APPLE)
    target_link_libraries(vgl PRIVATE m)
    if(NOT BUILD_SHARED_LIBS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_libraries(vgl PRIVATE stdc++)
    endif()
endif()

set_target_properties(vgl PROPERTIES
    LINKER_LANGUAGE CXX
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

if(VGL_BUILD_DEFAULTS)
    add_subdirectory(defaults)
endif()
