/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * Checks that the C API headers compile as C99 and that the base functions and
 * types follow docs/c-api.md.
 */
#include <vgl/defaults.h>
#include <vgl/vgl.h>
#ifdef VGL_TEST_GL_HEADER
#    include <vgl/gl.h>
#endif

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
    CHECK(offsetof(vgl_view, view_matrix) == 8);
    CHECK(sizeof(vgl_view) == 8 + 2 * sizeof(vgl_dmat4) + 8 + 8);
    /* Render interface structures: the same layout on 32-bit and 64-bit targets. */
    CHECK(sizeof(vgl_tile_id) == 16);
    CHECK(sizeof(vgl_index_list_data) == 24);
    CHECK(sizeof(vgl_mesh_data) == 104);
    CHECK(sizeof(vgl_texture_data) == 40);
    CHECK(sizeof(vgl_draw_layer) == 40);
    CHECK(sizeof(vgl_draw_item) == 192);
    CHECK(offsetof(vgl_draw_item, geoid_heights) == 168);
    CHECK(offsetof(vgl_draw_item, model_view) == 64);
    CHECK(sizeof(vgl_frame_begin_info) == 24);
    CHECK(sizeof(vgl_depth_convention) == 4);
#ifdef VGL_TEST_GL_HEADER
    /* The OpenGL backend's structures (vgl/gl.h). */
    CHECK(sizeof(vgl_gl_backend_create_info) == 24);
    CHECK(sizeof(vgl_gl_draw_info) == 88);
    CHECK(offsetof(vgl_gl_draw_info, sun_direction) == 24);
#endif
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
    vgl_view view = {.struct_size = sizeof(view)};
    vgl_frame* frame = NULL;
    const vgl_draw_item* items = NULL;
    uint32_t item_count = 0;
    vgl_view got = {.struct_size = sizeof(got)};
    vgl_allocator_interface allocator;
    void* memory = NULL;

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

    /* The context's allocator, and its log, for libraries working on its behalf. */
    CHECK(vgl_context_allocator_get(context, &allocator) == VGL_RESULT_SUCCESS);
    CHECK(allocator.version == VGL_ALLOCATOR_INTERFACE_VERSION);
    memory = allocator.allocate(allocator.user_data, 64, 16);
    CHECK(memory != NULL);
    if (memory != NULL)
    {
        allocator.deallocate(allocator.user_data, memory, 64, 16);
    }
    CHECK(vgl_context_log(context, VGL_LOG_LEVEL_INFO, "api_c: a message") == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_log(context, VGL_LOG_LEVEL_DEFAULT, "api_c: no level") ==
          VGL_RESULT_INVALID_ARGUMENT);
    CHECK(vgl_context_view_get(context, &got) == VGL_RESULT_NOT_FOUND);

    /* An identity view looks down -z from the centre of the Earth, which is valid. */
    view.view_matrix.m[0] = view.view_matrix.m[5] = view.view_matrix.m[10] = 1.0;
    view.view_matrix.m[15] = 1.0;
    view.projection_matrix = view.view_matrix;
    view.viewport_width = 640;
    view.viewport_height = 480;
    CHECK(vgl_context_view_set(context, &view) == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_view_get(context, &got) == VGL_RESULT_SUCCESS);
    CHECK(got.struct_size == sizeof(got) && got.viewport_width == 640 &&
          got.projection_matrix.m[15] == 1.0);
    CHECK(vgl_context_update(context, NULL) == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_frame_begin(context, NULL, &frame) == VGL_RESULT_SUCCESS);
    CHECK(vgl_frame_draw_list_get(frame, &items, &item_count) == VGL_RESULT_SUCCESS);
    CHECK(vgl_frame_end(frame) == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_resources_reset(context) == VGL_RESULT_SUCCESS);
    CHECK(vgl_context_view_set(context, NULL) == VGL_RESULT_SUCCESS);

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
