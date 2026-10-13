// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include <vgl/base.h>

extern "C" VGL_API vgl_result vgl_version_get(uint32_t* out_major,
                                              uint32_t* out_minor,
                                              uint32_t* out_patch)
{
    if (out_major == nullptr || out_minor == nullptr || out_patch == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }

    *out_major = VGL_VERSION_MAJOR;
    *out_minor = VGL_VERSION_MINOR;
    *out_patch = VGL_VERSION_PATCH;
    return VGL_RESULT_SUCCESS;
}
