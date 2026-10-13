/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * Checks that the C API headers compile as C99 and that the base functions and
 * types follow docs/c-api.md.
 */
#include <vgl/vgl.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void check_types(void)
{
    /* Sizes the ABI relies on (sections 4 and 8.2). */
    CHECK(sizeof(vgl_bool) == 4);
    CHECK(sizeof(vgl_result) == 4);
    CHECK(sizeof(vgl_dvec2) == 2 * sizeof(double));
    CHECK(sizeof(vgl_dvec3) == 3 * sizeof(double));
    CHECK(sizeof(vgl_dvec4) == 4 * sizeof(double));
    CHECK(sizeof(vgl_dmat4) == 16 * sizeof(double));
}

static void check_results(void)
{
    const char* name = NULL;

    CHECK(VGL_SUCCEEDED(VGL_RESULT_SUCCESS));
    CHECK(VGL_SUCCEEDED(VGL_RESULT_INCOMPLETE));
    CHECK(VGL_SUCCEEDED(VGL_RESULT_NOT_READY));
    CHECK(VGL_FAILED(VGL_RESULT_INVALID_ARGUMENT));
    CHECK(VGL_FAILED(VGL_RESULT_INTERNAL_ERROR));

    CHECK(vgl_result_name_get(VGL_RESULT_OUT_OF_MEMORY, &name) == VGL_RESULT_SUCCESS);
    CHECK(name != NULL && strcmp(name, "VGL_RESULT_OUT_OF_MEMORY") == 0);

    CHECK(vgl_result_name_get(VGL_RESULT_INCOMPLETE, &name) == VGL_RESULT_SUCCESS);
    CHECK(name != NULL && strcmp(name, "VGL_RESULT_INCOMPLETE") == 0);

    CHECK(vgl_result_name_get((vgl_result)-1000, &name) == VGL_RESULT_NOT_FOUND);
    CHECK(name != NULL && strcmp(name, "VGL_RESULT_UNKNOWN") == 0);

    CHECK(vgl_result_name_get(VGL_RESULT_SUCCESS, NULL) == VGL_RESULT_INVALID_ARGUMENT);
}

static void check_version(void)
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    CHECK(vgl_version_get(&major, &minor, &patch) == VGL_RESULT_SUCCESS);
    CHECK(major == VGL_VERSION_MAJOR);
    CHECK(minor == VGL_VERSION_MINOR);
    CHECK(patch == VGL_VERSION_PATCH);
    CHECK(vgl_version_get(NULL, &minor, &patch) == VGL_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    check_types();
    check_results();
    check_version();

    if (failures != 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("vgl %s: all checks passed\n", VGL_VERSION_STRING);
    return 0;
}
