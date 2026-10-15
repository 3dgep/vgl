/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * Checks that the C API headers compile as C99 and that the base functions and
 * types follow docs/c-api.md.
 */
#include <vgl/defaults.h>
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
    CHECK(sizeof(vgl_ellipsoid) == 2 * sizeof(double));
    CHECK(sizeof(vgl_geodetic) == 3 * sizeof(double));
    CHECK(offsetof(vgl_allocator_interface, user_data) == 8);
    CHECK(sizeof(vgl_allocator_interface) == 8 + 3 * sizeof(void*));
    CHECK(sizeof(vgl_log_level) == 4);
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

static void check_geodesy(void)
{
    vgl_ellipsoid wgs84;
    vgl_geodetic position;
    vgl_dvec3 ecef;
    vgl_geodetic back;

    position.latitude = VGL_DEGREES_TO_RADIANS(52.1);
    position.longitude = VGL_DEGREES_TO_RADIANS(5.1);
    position.height = 10.0;

    CHECK(vgl_ellipsoid_wgs84_get(&wgs84) == VGL_RESULT_SUCCESS);
    CHECK(vgl_ellipsoid_geodetic_to_ecef(&wgs84, &position, &ecef) == VGL_RESULT_SUCCESS);
    CHECK(ecef.x > 3910649.03 && ecef.x < 3910649.04);
    CHECK(vgl_ellipsoid_ecef_to_geodetic(&wgs84, &ecef, &back) == VGL_RESULT_SUCCESS);
    CHECK(back.height > 9.999999 && back.height < 10.000001);
    CHECK(vgl_ellipsoid_enu_frame_get(&wgs84, &position, NULL, NULL) ==
          VGL_RESULT_INVALID_ARGUMENT);
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

/* I/O that has no files: every read fails at once. */
static void read_nothing(void* user_data, vgl_io_request* request, const vgl_io_read* read)
{
    int* reads = (int*)user_data;
    (void)read;
    ++*reads;
    vgl_io_request_complete(request, VGL_RESULT_NOT_FOUND, NULL, 0);
}

static void check_context(void)
{
    int reads = 0;
    vgl_io_interface io = {.version = VGL_IO_INTERFACE_VERSION};
    vgl_context_create_info info = {.struct_size = sizeof(info)};
    vgl_pack_create_info pack_info = {.struct_size = sizeof(pack_info)};
    vgl_context* context = NULL;
    vgl_pack* pack = NULL;
    vgl_pack_state state = VGL_PACK_STATE_LOADING;
    vgl_result failure = VGL_RESULT_SUCCESS;
    double height = 0.0;

    io.user_data = &reads;
    io.read = read_nothing;
    info.io = &io;
    CHECK(vgl_context_create(&info, &context) == VGL_RESULT_SUCCESS);
    pack_info.uri = "nowhere";
    CHECK(vgl_pack_create(context, &pack_info, &pack) == VGL_RESULT_SUCCESS);
    CHECK(reads == 1);
    CHECK(vgl_context_update(context, NULL) == VGL_RESULT_SUCCESS);
    CHECK(vgl_pack_state_get(pack, &state, &failure) == VGL_RESULT_SUCCESS);
    CHECK(state == VGL_PACK_STATE_FAILED);
    CHECK(failure == VGL_RESULT_NOT_FOUND);
    CHECK(vgl_context_height_query(context, 0.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &height) ==
          VGL_RESULT_NOT_FOUND);
    CHECK(vgl_pack_destroy(pack) == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_destroy(context) == VGL_RESULT_SUCCESS);
}

int main(void)
{
    check_types();
    check_results();
    check_geodesy();
    check_version();
    check_context();

    if (failures != 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("vgl %s: all checks passed\n", VGL_VERSION_STRING);
    return 0;
}
