# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Baking tool (design document, section 8.2). The pack writers have no GDAL
# dependency, so they are also built for the tests when the baker itself is off.

# Pack format writers. An object library, so that executables can link it next
# to vgl_objects without duplicating the library's object files.
add_library(vgl_pack_writer OBJECT
    pack_writer.cpp
    pack_writer.hpp)
target_include_directories(vgl_pack_writer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vgl_pack_writer PUBLIC vgl_objects vgl_zstd)
vgl_target_options(vgl_pack_writer)

if(NOT VGL_BUILD_BAKER)
    return()
endif()

# GDAL reads the source formats (NFR-07: the only place GDAL is used). It is
# found, never downloaded: on Linux from system packages, on Windows from
# conda-forge (see tools/baker/README.md).
find_package(GDAL 3.4 CONFIG QUIET)
if(NOT GDAL_FOUND)
    find_package(GDAL 3.4 MODULE)
endif()
if(NOT GDAL_FOUND)
    message(FATAL_ERROR "vgl: the baker needs GDAL 3.4 or later. Install it (Ubuntu: libgdal-dev; "
                        "Windows: conda-forge libgdal-core, see tools/baker/README.md) and point "
                        "CMAKE_PREFIX_PATH at it, or turn VGL_BUILD_BAKER off.")
endif()
message(STATUS "vgl: baker uses GDAL ${GDAL_VERSION}")

# nlohmann/json reads bake configurations: installed, else downloaded.
find_package(nlohmann_json 3.10 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    if(NOT VGL_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "vgl: the baker needs nlohmann/json and VGL_FETCH_DEPENDENCIES is off")
    endif()
    FetchContent_Declare(vgl_nlohmann_json
        URL "https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip"
        URL_HASH SHA256=a22461d13119ac5c78f205d3df1db13403e58ce1bb1794edc9313677313f4a9d
        SOURCE_SUBDIR do-not-build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vgl_nlohmann_json)
    add_library(vgl_nlohmann_json INTERFACE)
    target_include_directories(vgl_nlohmann_json SYSTEM INTERFACE "${vgl_nlohmann_json_SOURCE_DIR}/single_include")
    add_library(nlohmann_json::nlohmann_json ALIAS vgl_nlohmann_json)
    message(STATUS "vgl: baker uses downloaded nlohmann/json 3.11.3")
endif()

# The baker as a library, so the tests can use it, and the vgl-bake command.
add_library(vgl_bake OBJECT
    bake_config.cpp
    bake_config.hpp
    baker.cpp
    baker.hpp
    inspect.cpp
    inspect.hpp
    pack_file.cpp
    pack_file.hpp
    source.cpp
    source.hpp)
target_include_directories(vgl_bake PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vgl_bake PUBLIC vgl_pack_writer vgl_objects GDAL::GDAL nlohmann_json::nlohmann_json)
vgl_target_options(vgl_bake)

add_executable(vgl-bake main.cpp)
target_link_libraries(vgl-bake PRIVATE vgl_bake vgl_pack_writer vgl_objects)
vgl_link_zstd_objects(vgl-bake COMPRESS)
vgl_target_options(vgl-bake)

if(VGL_INSTALL)
    install(TARGETS vgl-bake RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
