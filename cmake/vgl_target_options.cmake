# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# vgl_target_options(<target>)
#
# Applies the language level, warnings and library rules shared by every vgl
# target. Call it for each library, tool, sample and test target.
function(vgl_target_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        C_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(VGL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
        if(VGL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# vgl_disable_exceptions(<target>)
#
# Builds the target without exceptions and RTTI when VGL_DISABLE_EXCEPTIONS is
# on (NFR-05). Only library targets use this; tests and tools may use exceptions.
function(vgl_disable_exceptions target)
    if(NOT VGL_DISABLE_EXCEPTIONS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    endif()
endfunction()
