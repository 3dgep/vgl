# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Third-party dependencies (design document, section 12.1). Each dependency is
# found first, as an installed package (system packages, vcpkg, conda), and is
# otherwise downloaded at a pinned version with a checked hash, unless
# VGL_FETCH_DEPENDENCIES is off.
#
# Zstandard (runtime): provides the target vgl_zstd, an interface library that
# carries the include directories and, for an installed Zstandard, the link to
# it. A downloaded Zstandard is compiled into vgl itself, with hidden symbols:
# the decompressor into the runtime (vgl_zstd_decompress_objects) and the
# compressor only into the baker (vgl_zstd_compress_objects).
#
# stb_image (runtime): decodes JPEG and PNG imagery tiles. It is one header,
# used from an installed copy (such as Debian's libstb-dev or vcpkg's stb) or
# downloaded at a pinned commit. Its implementation is compiled into vgl as the
# object library vgl_stb_objects, in C and without vgl's warning flags.
#
# GDAL (baker only) is never downloaded: building it is out of scope here.

include(FetchContent)

option(VGL_FETCH_DEPENDENCIES "Download dependencies that are not installed" ON)
set(VGL_ZSTD_PROVIDER "AUTO" CACHE STRING
    "Where Zstandard comes from: AUTO (installed, else downloaded), SYSTEM or BUNDLED")
set_property(CACHE VGL_ZSTD_PROVIDER PROPERTY STRINGS AUTO SYSTEM BUNDLED)

set(VGL_ZSTD_VERSION 1.5.7)
set(VGL_ZSTD_URL "https://github.com/facebook/zstd/releases/download/v${VGL_ZSTD_VERSION}/zstd-${VGL_ZSTD_VERSION}.tar.gz")
set(VGL_ZSTD_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

add_library(vgl_zstd INTERFACE)
set(VGL_ZSTD_BUNDLED OFF)

if(NOT VGL_ZSTD_PROVIDER STREQUAL "BUNDLED")
    find_package(zstd 1.4 CONFIG QUIET)
endif()

if(zstd_FOUND AND NOT VGL_ZSTD_PROVIDER STREQUAL "BUNDLED")
    # Zstandard's package exports zstd::libzstd from 1.5.6, and the shared and
    # static targets separately before that.
    if(TARGET zstd::libzstd)
        set(VGL_ZSTD_TARGET zstd::libzstd)
    elseif(TARGET zstd::libzstd_shared)
        set(VGL_ZSTD_TARGET zstd::libzstd_shared)
    else()
        set(VGL_ZSTD_TARGET zstd::libzstd_static)
    endif()
    target_link_libraries(vgl_zstd INTERFACE ${VGL_ZSTD_TARGET})
    message(STATUS "vgl: using installed Zstandard ${zstd_VERSION} (${VGL_ZSTD_TARGET})")
elseif(VGL_ZSTD_PROVIDER STREQUAL "SYSTEM")
    message(FATAL_ERROR "vgl: VGL_ZSTD_PROVIDER is SYSTEM, but no installed Zstandard was found. "
                        "Install it (libzstd-dev, vcpkg or conda) or set VGL_ZSTD_PROVIDER to AUTO.")
elseif(NOT VGL_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "vgl: Zstandard was not found and VGL_FETCH_DEPENDENCIES is off. "
                        "Install it (libzstd-dev, vcpkg or conda) or turn VGL_FETCH_DEPENDENCIES on.")
else()
    # SOURCE_SUBDIR names a directory without a CMakeLists.txt, so that
    # FetchContent only downloads: the sources are compiled below, into vgl.
    FetchContent_Declare(vgl_zstd_source
        URL "${VGL_ZSTD_URL}"
        URL_HASH SHA256=${VGL_ZSTD_SHA256}
        SOURCE_SUBDIR do-not-build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vgl_zstd_source)
    set(VGL_ZSTD_BUNDLED ON)
    set(zstd_lib "${vgl_zstd_source_SOURCE_DIR}/lib")

    file(GLOB zstd_common_sources CONFIGURE_DEPENDS "${zstd_lib}/common/*.c")
    file(GLOB zstd_decompress_sources CONFIGURE_DEPENDS "${zstd_lib}/decompress/*.c")
    file(GLOB zstd_compress_sources CONFIGURE_DEPENDS "${zstd_lib}/compress/*.c")

    # ZSTD_DISABLE_ASM: no assembly, so the build is the same on every
    # platform. No ZSTD_MULTITHREAD: the library never creates threads (NFR-02).
    # ZSTDLIB_VISIBLE empty: Zstandard's symbols stay hidden in a shared vgl.
    foreach(part decompress compress)
        set(objects vgl_zstd_${part}_objects)
        add_library(${objects} OBJECT)
        target_include_directories(${objects} PRIVATE "${zstd_lib}" "${zstd_lib}/common")
        target_compile_definitions(${objects} PRIVATE
            ZSTD_DISABLE_ASM ZSTDLIB_VISIBLE= ZSTDERRORLIB_VISIBLE= XXH_NAMESPACE=VGL_ZSTD_)
        set_target_properties(${objects} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
            FOLDER third_party)
    endforeach()
    target_sources(vgl_zstd_decompress_objects PRIVATE ${zstd_common_sources} ${zstd_decompress_sources})
    target_sources(vgl_zstd_compress_objects PRIVATE ${zstd_compress_sources})

    # SYSTEM: Zstandard's headers do not trigger vgl's warnings.
    target_include_directories(vgl_zstd SYSTEM INTERFACE "${zstd_lib}")
    target_compile_definitions(vgl_zstd INTERFACE ZSTDLIB_VISIBLE=)
    message(STATUS "vgl: using downloaded Zstandard ${VGL_ZSTD_VERSION}, compiled into vgl")
endif()

# stb_image: an installed header, or the pinned download.
set(VGL_STB_COMMIT 013ac3beddff3dbffafd5177e7972067cd2b5083) # stb_image 2.30
set(VGL_STB_URL "https://raw.githubusercontent.com/nothings/stb/${VGL_STB_COMMIT}/stb_image.h")
set(VGL_STB_SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)

find_path(VGL_STB_SYSTEM_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb)
if(VGL_STB_SYSTEM_INCLUDE_DIR)
    set(VGL_STB_INCLUDE_DIR "${VGL_STB_SYSTEM_INCLUDE_DIR}")
    message(STATUS "vgl: using installed stb_image (${VGL_STB_INCLUDE_DIR})")
elseif(VGL_FETCH_DEPENDENCIES)
    FetchContent_Declare(vgl_stb_source
        URL "${VGL_STB_URL}"
        URL_HASH SHA256=${VGL_STB_SHA256}
        DOWNLOAD_NO_EXTRACT TRUE
        DOWNLOAD_NAME stb_image.h
        SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(vgl_stb_source)
    set(VGL_STB_INCLUDE_DIR "${vgl_stb_source_SOURCE_DIR}")
    message(STATUS "vgl: using downloaded stb_image 2.30, compiled into vgl")
else()
    message(FATAL_ERROR "vgl: stb_image.h was not found and VGL_FETCH_DEPENDENCIES is off. "
                        "Install it (libstb-dev or vcpkg's stb) or turn VGL_FETCH_DEPENDENCIES on.")
endif()

# The implementation, in C. Its allocations go to vgl's allocator through
# vgl_stbi_allocate and friends (src/decoders/image_decoder.cpp).
add_library(vgl_stb_objects OBJECT "${PROJECT_SOURCE_DIR}/src/decoders/stb_image.c")
target_include_directories(vgl_stb_objects SYSTEM PRIVATE "${VGL_STB_INCLUDE_DIR}")
set_target_properties(vgl_stb_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    FOLDER third_party)

# vgl_link_bundled_objects(<target> [COMPRESS])
#
# For executables that link vgl_objects directly (tests, the baker): adds the
# objects compiled into vgl from third-party sources, which vgl_objects cannot
# carry itself: stb_image, and a downloaded Zstandard.
function(vgl_link_bundled_objects target)
    target_link_libraries(${target} PRIVATE vgl_zstd)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:vgl_stb_objects>)
    if(VGL_ZSTD_BUNDLED)
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:vgl_zstd_decompress_objects>)
        if("COMPRESS" IN_LIST ARGN)
            target_sources(${target} PRIVATE $<TARGET_OBJECTS:vgl_zstd_compress_objects>)
        endif()
    endif()
endfunction()
