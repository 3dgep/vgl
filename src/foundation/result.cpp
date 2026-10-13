// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include <vgl/base.h>

extern "C" VGL_API vgl_result vgl_result_name_get(vgl_result result, const char** out_name)
{
    if (out_name == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }

    switch (result)
    {
    case VGL_RESULT_SUCCESS:
        *out_name = "VGL_RESULT_SUCCESS";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_INCOMPLETE:
        *out_name = "VGL_RESULT_INCOMPLETE";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_NOT_READY:
        *out_name = "VGL_RESULT_NOT_READY";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_INVALID_ARGUMENT:
        *out_name = "VGL_RESULT_INVALID_ARGUMENT";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_OUT_OF_MEMORY:
        *out_name = "VGL_RESULT_OUT_OF_MEMORY";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_NOT_SUPPORTED:
        *out_name = "VGL_RESULT_NOT_SUPPORTED";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_INVALID_OPERATION:
        *out_name = "VGL_RESULT_INVALID_OPERATION";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_NOT_FOUND:
        *out_name = "VGL_RESULT_NOT_FOUND";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_IO_ERROR:
        *out_name = "VGL_RESULT_IO_ERROR";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_CORRUPT_DATA:
        *out_name = "VGL_RESULT_CORRUPT_DATA";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_VERSION_MISMATCH:
        *out_name = "VGL_RESULT_VERSION_MISMATCH";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_CANCELLED:
        *out_name = "VGL_RESULT_CANCELLED";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_BACKEND_ERROR:
        *out_name = "VGL_RESULT_BACKEND_ERROR";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_INTERNAL_ERROR:
        *out_name = "VGL_RESULT_INTERNAL_ERROR";
        return VGL_RESULT_SUCCESS;
    case VGL_RESULT_MAX_ENUM:
        break;
    }

    *out_name = "VGL_RESULT_UNKNOWN";
    return VGL_RESULT_NOT_FOUND;
}
