/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * Pure-C sample (design document, section 11): the OpenGL sample's core, written in C99
 * against the C API, as a C host integrates vgl. A camera circles the globe; Escape quits.
 *
 *   vgl-c [--data <directory>] [--check]
 *
 * With --check, it runs hidden until the view has loaded, checks that every selected tile is
 * drawn and that OpenGL reports no error, and exits with 1 on a failure.
 */
#define GLFW_INCLUDE_NONE
#include <vgl/defaults.h>
#include <vgl/gl.h>
#include <vgl/vgl.h>

#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif

/* The OpenGL functions the host calls itself, to clear its framebuffer. */
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000
typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned int);
typedef void (*viewport_fn)(int, int, int, int);
typedef unsigned int (*get_error_fn)(void);

static vgl_gl_proc load(void* user_data, const char* name)
{
    (void)user_data;
    return (vgl_gl_proc)glfwGetProcAddress(name);
}

static void log_message(void* user_data, vgl_log_level level, const char* message)
{
    (void)user_data;
    (void)level;
    fprintf(stderr, "vgl: %s\n", message);
}

/* A right-handed view matrix from eye towards the centre of the Earth, with z up. */
static vgl_dmat4 look_at_earth(vgl_dvec3 eye)
{
    vgl_dmat4 m = {{0.0}};
    const double l = sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z);
    const vgl_dvec3 f = {-eye.x / l, -eye.y / l, -eye.z / l};
    /* s = normalize(f x up), u = s x f, with up = +z. */
    const double sl = sqrt(f.y * f.y + f.x * f.x);
    const vgl_dvec3 s = {f.y / sl, -f.x / sl, 0.0};
    const vgl_dvec3 u = {s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x};
    m.m[0] = s.x;
    m.m[4] = s.y;
    m.m[8] = s.z;
    m.m[1] = u.x;
    m.m[5] = u.y;
    m.m[9] = u.z;
    m.m[2] = -f.x;
    m.m[6] = -f.y;
    m.m[10] = -f.z;
    m.m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    m.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    m.m[14] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
    m.m[15] = 1.0;
    return m;
}

/* An OpenGL perspective projection. With the logarithmic depth convention, the near and far
 * planes do not matter. */
static vgl_dmat4 perspective(double fovy, double aspect)
{
    vgl_dmat4 m = {{0.0}};
    const double f = 1.0 / tan(fovy / 2.0);
    const double near_plane = 1.0;
    const double far_plane = 1.0e9;
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    m.m[11] = -1.0;
    m.m[14] = 2.0 * far_plane * near_plane / (near_plane - far_plane);
    return m;
}

int main(int argc, char** argv)
{
    const char* data = VGL_SAMPLE_DATA_DIR;
    int check = 0;
    int failed = 0;
    int i;
    char terrain_uri[1024];
    char geoid_uri[1024];
    char imagery_uri[1024];
    GLFWwindow* window;
    clear_color_fn gl_clear_color;
    clear_fn gl_clear;
    viewport_fn gl_viewport;
    get_error_fn gl_get_error;
    vgl_thread_pool* pool = NULL;
    vgl_file_reader* reader = NULL;
    vgl_file_reader_create_info reader_info = {.struct_size = sizeof(reader_info)};
    vgl_task_interface tasks;
    vgl_io_interface io;
    vgl_context_create_info context_info = {.struct_size = sizeof(context_info)};
    vgl_context* context = NULL;
    vgl_pack_create_info pack_info = {.struct_size = sizeof(pack_info)};
    vgl_pack* terrain = NULL;
    vgl_pack* geoid = NULL;
    vgl_pack* imagery = NULL;
    vgl_gl_backend_create_info backend_info = {.struct_size = sizeof(backend_info)};
    vgl_gl_backend* backend = NULL;
    vgl_gl_draw_info draw_info = {.struct_size = sizeof(draw_info)};
    vgl_context_statistics statistics = {.struct_size = sizeof(statistics)};
    int frame = 0;
    int quiet = 0;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--check") == 0)
        {
            check = 1;
        }
        else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc)
        {
            data = argv[++i];
        }
        else
        {
            fprintf(stderr, "usage: vgl-c [--data <directory>] [--check]\n");
            return 2;
        }
    }

    /* The window and its OpenGL 3.3 context belong to the host. */
    if (!glfwInit())
    {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, check ? GLFW_FALSE : GLFW_TRUE);
    window = glfwCreateWindow(1280, 720, "vgl (C)", NULL, NULL);
    if (window == NULL)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    gl_clear_color = (clear_color_fn)glfwGetProcAddress("glClearColor");
    gl_clear = (clear_fn)glfwGetProcAddress("glClear");
    gl_viewport = (viewport_fn)glfwGetProcAddress("glViewport");
    gl_get_error = (get_error_fn)glfwGetProcAddress("glGetError");

    /* 1. A context, with the defaults module's thread pool and file reader. */
    if (vgl_thread_pool_create(NULL, &pool) != VGL_RESULT_SUCCESS)
    {
        return 1;
    }
    reader_info.pool = pool;
    vgl_file_reader_create(&reader_info, &reader);
    vgl_thread_pool_task_interface_get(pool, &tasks);
    vgl_file_reader_io_interface_get(reader, &io);
    context_info.tasks = &tasks;
    context_info.io = &io;
    context_info.log = log_message;
    if (vgl_context_create(&context_info, &context) != VGL_RESULT_SUCCESS)
    {
        return 1;
    }

    /* 2. The packs: terrain, imagery, and the geoid for heights above mean sea level. */
    snprintf(terrain_uri, sizeof(terrain_uri), "%s/packs/gebco", data);
    snprintf(geoid_uri, sizeof(geoid_uri), "%s/packs/egm2008", data);
    snprintf(imagery_uri, sizeof(imagery_uri), "%s/packs/bluemarble", data);
    pack_info.uri = terrain_uri;
    vgl_pack_create(context, &pack_info, &terrain);
    pack_info.uri = geoid_uri;
    vgl_pack_create(context, &pack_info, &geoid);
    pack_info.uri = imagery_uri;
    vgl_pack_create(context, &pack_info, &imagery);

    /* The OpenGL backend, on the current OpenGL context. */
    backend_info.get_proc_address = load;
    if (vgl_gl_backend_create(context, &backend_info, &backend) != VGL_RESULT_SUCCESS)
    {
        fprintf(stderr, "could not create the OpenGL backend\n");
        return 1;
    }
    draw_info.sun_direction.x = 1.0;

    while (!glfwWindowShouldClose(window) && glfwGetKey(window, GLFW_KEY_ESCAPE) != GLFW_PRESS)
    {
        int width;
        int height;
        vgl_view view = {.struct_size = sizeof(view)};
        /* Circle the globe once a minute, at 20,000 km; the check holds still. */
        const double angle = check ? 0.0 : 2.0 * 3.14159265358979 * frame / 3600.0;
        const vgl_dvec3 eye = {2.6e7 * cos(angle), 2.6e7 * sin(angle), 0.8e7};

        glfwPollEvents();
        glfwGetFramebufferSize(window, &width, &height);
        if (width == 0 || height == 0)
        {
            glfwWaitEvents();
            continue;
        }

        /* 3. The camera, then an update. */
        view.view_matrix = look_at_earth(eye);
        view.projection_matrix = perspective(1.0, (double)width / height);
        view.viewport_width = (uint32_t)width;
        view.viewport_height = (uint32_t)height;
        vgl_context_view_set(context, &view);
        vgl_context_update(context, NULL);

        /* 4. The host clears its framebuffer, and the backend draws the globe into it. */
        gl_viewport(0, 0, width, height);
        gl_clear_color(0.02f, 0.02f, 0.05f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        vgl_gl_backend_draw(backend, &draw_info);
        glfwSwapBuffers(window);
        ++frame;

        if (check)
        {
            vgl_context_statistics_get(context, &statistics);
            quiet = statistics.packs_loading == 0 && statistics.reads_outstanding == 0 &&
                            statistics.tiles_decoding == 0 && statistics.meshes_building == 0 &&
                            statistics.meshes_waiting == 0 && statistics.resources_created == 0 &&
                            statistics.tiles_drawn == statistics.tiles_selected
                        ? quiet + 1
                        : 0;
            if (quiet == 5 || frame == 3000)
            {
                break;
            }
            if (quiet == 0)
            {
                glfwWaitEventsTimeout(0.001); /* Let the pool load. */
            }
        }
    }

    if (check)
    {
        printf("%d frames, %u tiles selected, %u drawn\n",
               frame,
               statistics.tiles_selected,
               statistics.tiles_drawn);
        if (quiet < 5 || statistics.tiles_drawn == 0 ||
            statistics.tiles_drawn != statistics.tiles_selected || statistics.packs_failed != 0)
        {
            fprintf(stderr, "check failed: the view did not load and draw\n");
            failed = 1;
        }
        if (gl_get_error() != 0)
        {
            fprintf(stderr, "check failed: OpenGL reports an error\n");
            failed = 1;
        }
        printf("%s\n", failed ? "checks failed" : "all checks passed");
    }

    /* Leave in order: the backend while its OpenGL context exists, the packs and the context,
     * then the reader and the pool. */
    vgl_gl_backend_destroy(backend);
    vgl_pack_destroy(terrain);
    vgl_pack_destroy(geoid);
    vgl_pack_destroy(imagery);
    vgl_context_destroy(context);
    vgl_file_reader_destroy(reader);
    vgl_thread_pool_destroy(pool);
    glfwDestroyWindow(window);
    glfwTerminate();
    return failed;
}
