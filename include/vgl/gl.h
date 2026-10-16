/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl OpenGL backend: draws a context's globe with OpenGL 3.3 or OpenGL ES 3.0, into the
 * host's framebuffer (design document, section 7.6). It is a consumer of the render interface
 * (vgl/render.h), linked separately as vgl_gl, and uses vgl only through the C API.
 *
 * The host creates the OpenGL context, makes it current and passes a function loader; the
 * backend never touches EGL, WGL or GLX. A minimal host, with GLFW:
 *
 *     static vgl_gl_proc load(void* user_data, const char* name)
 *     {
 *         (void)user_data;
 *         return (vgl_gl_proc)glfwGetProcAddress(name);
 *     }
 *
 *     vgl_gl_backend_create_info info = {.struct_size = sizeof(info)};
 *     info.get_proc_address = load;
 *     vgl_gl_backend* backend;
 *     vgl_gl_backend_create(context, &info, &backend);
 *
 *     // Each frame, in the host's render pass, after vgl_context_update:
 *     vgl_gl_backend_draw(backend, NULL);
 *
 *     vgl_gl_backend_destroy(backend);  // Before the context and the OpenGL context.
 *
 * State (NFR-08). The backend saves the following state when it draws, and restores it before
 * returning: the current program, the vertex array binding, the GL_ARRAY_BUFFER,
 * GL_COPY_WRITE_BUFFER and GL_PIXEL_UNPACK_BUFFER bindings, the active texture unit, the 2D
 * texture and sampler bindings of units 0 to 3, the unpack alignment, row length and skips,
 * GL_DEPTH_TEST, the depth function and write mask, GL_CULL_FACE, the cull face mode, the front
 * face, GL_BLEND, primitive restart (where it exists) and, for the reversed-Z convention, the
 * clip control depth mode. It draws with the
 * host's framebuffer, viewport, scissor, depth range, color and stencil state unchanged, so a
 * stencil test or color mask the host leaves enabled applies to the globe as well.
 */
#ifndef VGL_GL_H
#define VGL_GL_H

#include <vgl/base.h>
#include <vgl/context.h>
#include <vgl/render.h>

/* Symbol visibility of the backend library, which is shared when vgl is. */
#if defined(VGL_SHARED)
#    if defined(_WIN32)
#        if defined(VGL_GL_BUILDING)
#            define VGL_GL_API __declspec(dllexport)
#        else
#            define VGL_GL_API __declspec(dllimport)
#        endif
#    else
#        define VGL_GL_API __attribute__((visibility("default")))
#    endif
#else
#    define VGL_GL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* An OpenGL function, as the host's loader returns it. */
typedef void (*vgl_gl_proc)(void);

/**
 * Returns the address of an OpenGL or OpenGL ES function of the current context, or null.
 * It must also return the functions of OpenGL 1.x, as glfwGetProcAddress,
 * SDL_GL_GetProcAddress and EGL 1.5's eglGetProcAddress do (wglGetProcAddress does not).
 *
 * @param user_data  The pointer registered with the callback.
 * @param name       The function's name, such as "glDrawElements".
 *
 * @par Thread safety
 * Calling thread: inside vgl_gl_backend_create and vgl_gl_backend_reset.
 */
typedef vgl_gl_proc (*vgl_gl_get_proc_address_fn)(void* user_data, const char* name);

typedef struct vgl_gl_backend vgl_gl_backend;

/* Creation parameters of a backend. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_gl_backend_create_info
{
    uint32_t struct_size;
    /* How depth is written and compared (vgl/render.h). The default is logarithmic. The
     * reversed-Z convention needs clip control: OpenGL 4.5, GL_ARB_clip_control or, on OpenGL
     * ES, GL_EXT_clip_control. */
    vgl_depth_convention depth_convention;
    /* Required. Loads the OpenGL functions. */
    vgl_gl_get_proc_address_fn get_proc_address;
    VGL_POINTER_PADDING(get_proc_address_padding)
    void* get_proc_address_user_data;
    VGL_POINTER_PADDING(get_proc_address_user_data_padding)
} vgl_gl_backend_create_info;

/**
 * Creates a backend for a context, on the OpenGL context that is current. It loads the OpenGL
 * functions, checks the version (OpenGL 3.3 or OpenGL ES 3.0, or later), compiles its shaders
 * and tells the context which texture formats it samples. Its objects belong to the current
 * OpenGL context, or to the group of contexts that share objects with it.
 *
 * Allocations go through the context's allocator, and messages through its log. A context has
 * at most one consumer of the render interface at a time (docs/render-interface.md).
 *
 * @param context      The context. It must outlive the backend.
 * @param info         Required: the loader has no default.
 * @param out_backend  Receives the backend.
 *
 * @retval VGL_RESULT_SUCCESS           The backend was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, info is invalid, or the loader is
 *                                      missing.
 * @retval VGL_RESULT_NOT_SUPPORTED     info has fields from a newer version set; or the OpenGL
 *                                      version is too old, a function is missing, or the
 *                                      depth convention needs clip control the device lacks.
 *                                      The log names which.
 * @retval VGL_RESULT_BACKEND_ERROR     A shader failed to compile or link; the log has the
 *                                      driver's message.
 * @retval VGL_RESULT_OUT_OF_MEMORY     Allocation failed.
 *
 * @par Thread safety
 * Render thread, with the OpenGL context current.
 */
VGL_GL_API vgl_result vgl_gl_backend_create(vgl_context* context,
                                            const vgl_gl_backend_create_info* info,
                                            vgl_gl_backend** out_backend);

/**
 * Deletes the backend's OpenGL objects and frees it. The context forgets its render resources
 * (vgl_context_resources_reset), so that a backend created later receives them again.
 *
 * @param backend  The backend, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Render thread, with the backend's OpenGL context (or one sharing its objects) current, and
 * before the context is destroyed.
 */
VGL_GL_API vgl_result vgl_gl_backend_destroy(vgl_gl_backend* backend);

/* Parameters of a draw. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_gl_draw_info
{
    uint32_t struct_size;
    /* VGL_TRUE when the projection mirrors the image, as a projection with a flipped y axis
     * for rendering into a texture does. Triangles then face the other way, and the backend
     * culls the other side. */
    vgl_bool mirrored;
    /* Optional. The camera to draw from, when it differs from the view of the last update
     * (vgl_frame_begin_info.view_matrix). Null uses the context's view. */
    const vgl_dmat4* view_matrix;
    VGL_POINTER_PADDING(view_matrix_padding)
    /* Optional. From camera space to clip space. Null uses the context's view. */
    const vgl_dmat4* projection_matrix;
    VGL_POINTER_PADDING(projection_matrix_padding)
    /* Direction towards the sun, along the ECEF axes; it need not be normalized. Zero lights
     * the terrain from the camera. */
    vgl_dvec3 sun_direction;
    /* The logarithmic convention's far distance, in metres: the camera distance that maps to
     * depth 1. 0 selects VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT. Ignored by the other conventions. */
    double logarithmic_far;
    /* Bytes of resources to upload in this draw (vgl_frame_begin_info.upload_budget). 0
     * selects 16 MiB. */
    uint64_t upload_budget;
} vgl_gl_draw_info;

/**
 * Draws the globe into the current framebuffer: takes a frame from the context, applies its
 * resources (deleting and creating OpenGL objects) and draws its tiles: up to four imagery
 * layers per tile, blended over colours by height where imagery is missing, and lit by the
 * normal. It does not clear the framebuffer or present. Without a view, it draws nothing.
 *
 * Call it once per frame after vgl_context_update, inside the host's render pass. The host
 * sets up the depth buffer for the backend's depth convention (vgl/render.h).
 *
 * @param backend  The backend.
 * @param info     Optional. Null selects all defaults.
 *
 * @retval VGL_RESULT_SUCCESS            The globe was drawn.
 * @retval VGL_RESULT_INVALID_ARGUMENT   backend is null, or info is invalid: a matrix or
 *                                       direction element is not finite, or the far distance
 *                                       is negative.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_INVALID_OPERATION  A frame of the context is open already, or the last
 *                                       vgl_gl_backend_reset failed.
 *
 * @par Thread safety
 * Render thread, with the backend's OpenGL context (or one sharing its objects) current.
 */
VGL_GL_API vgl_result vgl_gl_backend_draw(vgl_gl_backend* backend, const vgl_gl_draw_info* info);

/**
 * Recovers from a lost OpenGL context, for example after an Android app returns from the
 * background: forgets the backend's OpenGL objects without deleting them, then loads the
 * functions and builds its shaders again on the OpenGL context that is current now. The
 * context forgets its render resources too, and offers them again over the next frames.
 *
 * @param backend  The backend.
 *
 * @retval VGL_RESULT_SUCCESS           The backend is ready on the new OpenGL context.
 * @retval VGL_RESULT_INVALID_ARGUMENT  backend is null.
 * @retval VGL_RESULT_NOT_SUPPORTED     The new OpenGL context cannot be used, as for
 *                                      vgl_gl_backend_create. Draws fail until a reset
 *                                      succeeds.
 * @retval VGL_RESULT_BACKEND_ERROR     A shader failed to compile or link.
 *
 * @par Thread safety
 * Render thread, with the new OpenGL context current.
 */
VGL_GL_API vgl_result vgl_gl_backend_reset(vgl_gl_backend* backend);

#ifdef __cplusplus
}
#endif

#endif /* VGL_GL_H */
