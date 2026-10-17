# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Dear ImGui, for the interactive samples' overlay: a sample-only dependency,
# downloaded at a pinned release with a checked hash. vgl_imgui_fetch(<name>
# <variable>) makes a copy of the sources available under the FetchContent name
# and sets the variable to its directory. A fetched Ogre gets a copy of its own,
# because its overlay writes its own imconfig.h into the sources it compiles.

include(FetchContent)

set(VGL_IMGUI_VERSION 1.92.9b)

function(vgl_imgui_fetch name variable)
    FetchContent_Declare(${name}
        URL "https://github.com/ocornut/imgui/archive/refs/tags/v${VGL_IMGUI_VERSION}.tar.gz"
        URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
        SOURCE_SUBDIR do-not-build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(${name})
    set(${variable} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
