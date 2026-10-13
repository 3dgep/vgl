/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl base header: export macros, basic types, result codes and version.
 * See docs/c-api.md for the conventions every declaration follows.
 */
#ifndef VGL_BASE_H
#define VGL_BASE_H

#include <vgl/version.h>

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility. VGL_SHARED is defined for consumers of a shared build. */
#if defined(VGL_SHARED)
#    if defined(_WIN32)
#        if defined(VGL_BUILDING)
#            define VGL_API __declspec(dllexport)
#        else
#            define VGL_API __declspec(dllimport)
#        endif
#    else
#        define VGL_API __attribute__((visibility("default")))
#    endif
#else
#    define VGL_API
#endif

/* Marks a declaration as deprecated (docs/c-api.md, section 14.4). */
#if defined(_MSC_VER)
#    define VGL_DEPRECATED(message) __declspec(deprecated(message))
#elif defined(__GNUC__) || defined(__clang__)
#    define VGL_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#    define VGL_DEPRECATED(message)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Boolean. Any non-zero value is true on input; the library writes exactly 0 or 1. */
typedef uint32_t vgl_bool;

#define VGL_FALSE 0u
#define VGL_TRUE  1u

/* Double-precision vectors. */
typedef struct vgl_dvec2
{
    double x;
    double y;
} vgl_dvec2;

typedef struct vgl_dvec3
{
    double x;
    double y;
    double z;
} vgl_dvec3;

typedef struct vgl_dvec4
{
    double x;
    double y;
    double z;
    double w;
} vgl_dvec4;

/*
 * Double-precision 4x4 matrix in column-major order, multiplying column vectors.
 * Element (row r, column c) is m[c * 4 + r]; the translation is in m[12], m[13], m[14].
 */
typedef struct vgl_dmat4
{
    double m[16];
} vgl_dmat4;

/*
 * Result code returned by every vgl function. Zero is success, positive values are
 * non-error statuses and negative values are errors. Treat unknown negative codes as
 * failures and unknown positive codes as success.
 */
typedef enum vgl_result
{
    VGL_RESULT_SUCCESS = 0,
    VGL_RESULT_INCOMPLETE = 1,
    VGL_RESULT_NOT_READY = 2,
    VGL_RESULT_INVALID_ARGUMENT = -1,
    VGL_RESULT_OUT_OF_MEMORY = -2,
    VGL_RESULT_NOT_SUPPORTED = -3,
    VGL_RESULT_INVALID_OPERATION = -4,
    VGL_RESULT_NOT_FOUND = -5,
    VGL_RESULT_IO_ERROR = -6,
    VGL_RESULT_CORRUPT_DATA = -7,
    VGL_RESULT_VERSION_MISMATCH = -8,
    VGL_RESULT_CANCELLED = -9,
    VGL_RESULT_BACKEND_ERROR = -10,
    VGL_RESULT_INTERNAL_ERROR = -11,
    VGL_RESULT_MAX_ENUM = 0x7FFFFFFF
} vgl_result;

#define VGL_SUCCEEDED(result) ((result) >= 0)
#define VGL_FAILED(result)    ((result) < 0)

/**
 * Gets the name of a result code, such as "VGL_RESULT_OUT_OF_MEMORY".
 *
 * @param result    The result code.
 * @param out_name  Receives a static string, valid for the lifetime of the process.
 *                  For an unknown code this is "VGL_RESULT_UNKNOWN".
 *
 * @retval VGL_RESULT_SUCCESS           The name was written.
 * @retval VGL_RESULT_NOT_FOUND         The code is unknown; "VGL_RESULT_UNKNOWN" was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  out_name is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_result_name_get(vgl_result result, const char** out_name);

/**
 * Gets the version of the vgl binary, which may differ from the VGL_VERSION_* macros
 * the caller was compiled against.
 *
 * @param out_major  Receives the major version.
 * @param out_minor  Receives the minor version.
 * @param out_patch  Receives the patch version.
 *
 * @retval VGL_RESULT_SUCCESS           The version was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  An output pointer is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_version_get(uint32_t* out_major, uint32_t* out_minor, uint32_t* out_patch);

#ifdef __cplusplus
}
#endif

#endif /* VGL_BASE_H */
