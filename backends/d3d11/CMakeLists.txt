# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Direct3D 11 backend, feature level 11_0 (design document, section 7.6). The
# host creates the device and passes it, so the backend links d3d11.lib only.
# Like the OpenGL backend, it uses vgl only through the C API.

# The shaders are compiled at build time with the Windows SDK's fxc, into
# headers of bytecode that the backend embeds: nothing compiles at runtime.
set(vgl_sdk_bin_hints)
if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND vgl_sdk_bin_hints "$ENV{WindowsSdkVerBinPath}/x64")
endif()
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND vgl_sdk_bin_hints
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
endif()
if(CMAKE_WINDOWS_KITS_10_DIR)
    file(GLOB vgl_sdk_bins LIST_DIRECTORIES true "${CMAKE_WINDOWS_KITS_10_DIR}/bin/10.*/x64")
else()
    file(GLOB vgl_sdk_bins LIST_DIRECTORIES true "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64")
endif()
list(SORT vgl_sdk_bins ORDER DESCENDING)
list(APPEND vgl_sdk_bin_hints ${vgl_sdk_bins})
find_program(VGL_FXC fxc HINTS ${vgl_sdk_bin_hints} DOC "The Windows SDK's HLSL compiler")
if(NOT VGL_FXC)
    message(WARNING "vgl: fxc (the Windows SDK's HLSL compiler) not found; skipping the Direct3D 11 backend")
    return()
endif()

set(vgl_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(vgl_shader_headers)
# Name, profile, entry point, and the defines of each variant.
foreach(variant
        "terrain_vs|vs_5_0|vs_main|"
        "terrain_vs_log|vs_5_0|vs_main|/DVGL_LOGARITHMIC_DEPTH=1"
        "terrain_ps|ps_5_0|ps_main|"
        "terrain_ps_log|ps_5_0|ps_main|/DVGL_LOGARITHMIC_DEPTH=1")
    string(REPLACE "|" ";" parts "${variant}")
    list(GET parts 0 name)
    list(GET parts 1 profile)
    list(GET parts 2 entry)
    list(GET parts 3 defines)
    set(header "${vgl_shader_dir}/${name}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${vgl_shader_dir}"
        COMMAND "${VGL_FXC}" /nologo /T ${profile} /E ${entry} /O3 /WX ${defines}
                /Vn g_${name} /Fh "${header}" "${CMAKE_CURRENT_SOURCE_DIR}/terrain.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/terrain.hlsl"
        COMMENT "Compiling ${name} (${profile})"
        VERBATIM)
    list(APPEND vgl_shader_headers "${header}")
endforeach()

add_library(vgl_d3d11
    include/vgl/d3d11.h
    include/vgl/d3d11.hpp
    backend.cpp
    terrain.hlsl
    ${vgl_shader_headers}
    "${PROJECT_SOURCE_DIR}/src/common/host_memory.hpp"
    "${PROJECT_SOURCE_DIR}/src/common/pointer_map.hpp")
add_library(vgl::d3d11 ALIAS vgl_d3d11)
# The HLSL is compiled by the commands above, not by Visual Studio's own rules.
set_source_files_properties(terrain.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(vgl_d3d11
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}/src" "${vgl_shader_dir}")
target_link_libraries(vgl_d3d11 PUBLIC vgl d3d11)

# Without the min and max macros of windows.h, which d3d11.h includes.
target_compile_definitions(vgl_d3d11 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(vgl_d3d11 PRIVATE VGL_D3D11_BUILDING)
endif()

set_target_properties(vgl_d3d11 PROPERTIES
    OUTPUT_NAME vgl-d3d11
    EXPORT_NAME d3d11
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

vgl_target_options(vgl_d3d11)
vgl_disable_exceptions(vgl_d3d11)
