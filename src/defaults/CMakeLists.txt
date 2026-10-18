# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Optional defaults: thread pool, filesystem reader, HTTP reader and disk cache, used
# by the samples and available to hosts. Built as a separate vgl_defaults target linking vgl, so
# the core library itself never creates threads (NFR-02). It uses vgl only
# through the C API, so it works with a shared vgl as well.

find_package(Threads REQUIRED)

add_library(vgl_defaults
    ../common/host_memory.hpp
    disk_cache.cpp
    file_reader.cpp
    files.cpp
    files.hpp
    http_reader.cpp
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

# The HTTP reader (VGL_DEFAULTS_HTTP), on libcurl: an installed one, or else curl 8.11.1,
# downloaded at a checked hash and built with HTTP and HTTPS only, on the system's TLS
# (Schannel on Windows, Secure Transport on Apple, OpenSSL elsewhere). Without it, the reader's
# functions report VGL_RESULT_NOT_SUPPORTED, so hosts can call them in any build.
if(VGL_DEFAULTS_HTTP AND EMSCRIPTEN)
    # In the browser, the reader is built on its fetch, which needs no library; hosts link
    # Emscripten's fetch support, which the interface passes on.
    target_compile_definitions(vgl_defaults PRIVATE VGL_DEFAULTS_HAS_FETCH=1)
    target_link_options(vgl_defaults INTERFACE "-sFETCH=1")
    message(STATUS "vgl: the HTTP reader uses the browser's fetch")
elseif(VGL_DEFAULTS_HTTP)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(vgl_curl_target CURL::libcurl)
        message(STATUS "vgl: the HTTP reader uses the installed libcurl ${CURL_VERSION_STRING}")
    elseif(VGL_FETCH_DEPENDENCIES)
        include(FetchContent)
        set(VGL_CURL_VERSION 8.11.1)
        FetchContent_Declare(vgl_curl_source
            URL "https://curl.se/download/curl-${VGL_CURL_VERSION}.tar.xz"
            URL_HASH SHA256=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            EXCLUDE_FROM_ALL)
        # A static library with only what the reader needs.
        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
        set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
        set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        set(HTTP_ONLY ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
        set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
        set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
        set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
        set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
        set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
        if(WIN32)
            set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
        elseif(APPLE)
            set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
        else()
            set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        endif()
        # curl builds its static library alongside the shared one only if asked; keep vgl's
        # BUILD_SHARED_LIBS out of it.
        set(vgl_shared_libs ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(vgl_curl_source)
        set(BUILD_SHARED_LIBS ${vgl_shared_libs})
        set_target_properties(libcurl_static PROPERTIES FOLDER third_party POSITION_INDEPENDENT_CODE ON)
        # A downloaded curl is linked into a shared vgl_defaults, and not installed: an
        # installed static vgl_defaults needs an installed libcurl, which its package finds.
        set(vgl_curl_target "$<BUILD_INTERFACE:libcurl_static>$<INSTALL_INTERFACE:CURL::libcurl>")
        message(STATUS "vgl: the HTTP reader uses downloaded curl ${VGL_CURL_VERSION}")
    else()
        message(FATAL_ERROR "vgl: VGL_DEFAULTS_HTTP needs libcurl: install it (libcurl4-openssl-dev, or "
                            "vcpkg's curl), or turn VGL_FETCH_DEPENDENCIES on.")
    endif()
    target_link_libraries(vgl_defaults PRIVATE ${vgl_curl_target})
    target_compile_definitions(vgl_defaults PRIVATE VGL_DEFAULTS_HAS_CURL=1)
    # For the HTTP reader's test, which compiles the reader in.
    set_property(GLOBAL PROPERTY VGL_CURL_TARGET "${vgl_curl_target}")
endif()
