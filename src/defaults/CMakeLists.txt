# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Optional defaults: thread pool and filesystem reader, used by the samples and
# available to hosts. Built as a separate vgl_defaults target linking vgl, so
# the core library itself never creates threads (NFR-02). It uses vgl only
# through the C API, so it works with a shared vgl as well.

find_package(Threads REQUIRED)

add_library(vgl_defaults
    ../common/host_memory.hpp
    file_reader.cpp
    thread_pool.cpp)
add_library(vgl::defaults ALIAS vgl_defaults)

target_include_directories(vgl_defaults PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(vgl_defaults PUBLIC vgl PRIVATE Threads::Threads)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(vgl_defaults PRIVATE VGL_DEFAULTS_BUILDING)
endif()

set_target_properties(vgl_defaults PROPERTIES
    OUTPUT_NAME vgl-defaults
    EXPORT_NAME defaults
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

vgl_target_options(vgl_defaults)
vgl_disable_exceptions(vgl_defaults)
