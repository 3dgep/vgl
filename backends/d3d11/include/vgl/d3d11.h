/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl Direct3D 11 backend: draws a context's globe with Direct3D 11 at feature level 11_0, into
 * the host's render targets (design document, section 7.6). It is a consumer of the render
 * interface (vgl/render.h), linked separately as vgl_d3d11, and uses vgl only through the C API.
 *
 * The host creates the device and passes it, with the device context to draw with:
 *
 *     vgl_d3d11_backend_create_info info = {.struct_size = sizeof(info)};
 *     info.device = device;
 *     info.device_context = immediate_context;
 *     vgl_d3d11_backend* backend;
 *     vgl_d3d11_backend_create(context, &info, &backend);
 *
 *     // Each frame, after vgl_context_update, with the host's render target and depth buffer
 *     // bound and its viewport set:
 *     vgl_d3d11_backend_draw(backend, NULL);
 *
 *     vgl_d3d11_backend_destroy(backend);  // Before the context and the device.
 *
 * State (NFR-08). The backend saves the following state of the device context when it draws,
 * and restores it before returning: the input layout, the primitive topology, vertex buffer 0
 * and the index buffer; the vertex shader and its constant buffers 0 and 1; the pixel shader,
 * its constant buffers 0 and 1, its shader resources 0 to 4 and its sampler 0; the rasterizer
 * state; the depth-stencil state and stencil reference; and the blend state, blend factor and
 * sample mask. It draws with the host's render targets, viewports and scissor rectangles as
 * they are, with the stencil test and blending off.
 *
 * Projections map depth to [0, 1], as Direct3D does, and camera space is the view matrix's:
 * right-handed, like ECEF, looking along -z.
 */
#ifndef VGL_D3D11_H
#define VGL_D3D11_H

#include <d3d11.h>

#include <vgl/base.h>
#include <vgl/context.h>
#include <vgl/render.h>

/* Symbol visibility of the backend library, which is shared when vgl is. */
#if defined(VGL_SHARED)
#    if defined(VGL_D3D11_BUILDING)
#        define VGL_D3D11_API __declspec(dllexport)
#    else
#        define VGL_D3D11_API __declspec(dllimport)
#    endif
#else
#    define VGL_D3D11_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vgl_d3d11_backend vgl_d3d11_backend;

/** Creation parameters of a backend. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_d3d11_backend_create_info
{
    uint32_t struct_size;
    /** How depth is written and compared (vgl/render.h). The default is logarithmic; the
     * reversed-Z convention is the natural one on Direct3D 11. */
    vgl_depth_convention depth_convention;
    /** Required. The device, at feature level 11_0 or later. The backend holds a reference. */
    ID3D11Device* device;
    VGL_POINTER_PADDING(device_padding)
    /** Required. The device context to draw with, usually the immediate one. The backend holds a
     * reference. */
    ID3D11DeviceContext* device_context;
    VGL_POINTER_PADDING(device_context_padding)
} vgl_d3d11_backend_create_info;

/**
 * Creates a backend for a context on a device: checks the feature level, creates its shaders
 * and states, and tells the context which texture formats it samples.
 *
 * Allocations go through the context's allocator, and messages through its log. A context has
 * at most one consumer of the render interface at a time (docs/render-interface.md).
 *
 * @param context      The context. It must outlive the backend.
 * @param info         Required: the device has no default.
 * @param out_backend  Receives the backend.
 *
 * @retval VGL_RESULT_SUCCESS           The backend was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, info is invalid, or the device or
 *                                      device context is missing.
 * @retval VGL_RESULT_NOT_SUPPORTED     info has fields from a newer version set, or the device's
 *                                      feature level is below 11_0.
 * @retval VGL_RESULT_BACKEND_ERROR     Direct3D failed to create a shader or state; the log has
 *                                      its HRESULT.
 * @retval VGL_RESULT_OUT_OF_MEMORY     Allocation failed.
 *
 * @par Thread safety
 * Render thread, on the thread that owns the device context.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_create(vgl_context* context,
                                                  const vgl_d3d11_backend_create_info* info,
                                                  vgl_d3d11_backend** out_backend);

/**
 * Releases the backend's Direct3D objects and its references to the device and device context,
 * and frees it. The context forgets its render resources (vgl_context_resources_reset), so that
 * a backend created later receives them again.
 *
 * @param backend  The backend, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Render thread, on the thread that owns the device context, before the context is destroyed.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_destroy(vgl_d3d11_backend* backend);

/** Parameters of a draw. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_d3d11_draw_info
{
    uint32_t struct_size;
    /** VGL_TRUE when the projection mirrors the image, as a projection with a flipped y axis
     * does. Triangles then face the other way, and the backend culls the other side. */
    vgl_bool mirrored;
    /** Optional. The camera to draw from, when it differs from the view of the last update
     * (vgl_frame_begin_info.view_matrix). Null uses the context's view. */
    const vgl_dmat4* view_matrix;
    VGL_POINTER_PADDING(view_matrix_padding)
    /** Optional. From camera space to clip space, with depth from 0 to 1. Null uses the
     * context's view. */
    const vgl_dmat4* projection_matrix;
    VGL_POINTER_PADDING(projection_matrix_padding)
    /** Direction towards the sun, along the ECEF axes; it need not be normalized. Zero lights
     * the terrain from the camera. */
    vgl_dvec3 sun_direction;
    /** The logarithmic convention's far distance, in metres: the camera distance that maps to
     * depth 1. 0 selects VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT. Ignored by the other conventions. */
    double logarithmic_far;
    /** Bytes of resources to upload in this draw (vgl_frame_begin_info.upload_budget). 0
     * selects VGL_UPLOAD_BUDGET_DEFAULT. */
    uint64_t upload_budget;
    /** What to draw besides the terrain and its imagery: vgl_draw_feature values (vgl/render.h).
     * 0 draws the terrain alone. */
    vgl_draw_features features;
    /** Views for inspecting level of detail and loading: vgl_debug_view values (vgl/render.h).
     * 0 draws normally. */
    vgl_debug_views debug_views;
    /** The Earth rotation angle in radians (vgl_earth_rotation_angle_get), which turns the
     * stars' celestial frame into ECEF, for VGL_DRAW_FEATURE_STARS. */
    double earth_rotation_angle;
    /** Height between contour lines (VGL_DRAW_FEATURE_CONTOURS), in metres. 0 selects
     * VGL_CONTOUR_INTERVAL_DEFAULT. */
    double contour_interval;
    /** The cloud shell's height above the ellipsoid (VGL_DRAW_FEATURE_CLOUDS), in metres. 0
     * selects VGL_CLOUD_HEIGHT_DEFAULT. */
    double cloud_height;
} vgl_d3d11_draw_info;

/**
 * Draws the globe into the device context's bound render target: takes a frame from the
 * context, applies its resources (releasing and creating Direct3D objects) and draws its tiles:
 * up to four imagery layers per tile, blended over colours by height where imagery is missing,
 * and lit by the normal. It does not clear or present. Without a view, it draws nothing.
 *
 * Call it once per frame after vgl_context_update. The host binds a render target and a depth
 * buffer set up for the backend's depth convention (vgl/render.h), and sets the viewport.
 *
 * @param backend  The backend.
 * @param info     Optional. Null selects all defaults.
 *
 * @retval VGL_RESULT_SUCCESS            The globe was drawn.
 * @retval VGL_RESULT_INVALID_ARGUMENT   backend is null, or info is invalid: a matrix or
 *                                       direction element is not finite, features or
 *                                       debug_views has unknown bits, or earth_rotation_angle,
 *                                       logarithmic_far, contour_interval or cloud_height is
 *                                       not finite or, but for the angle, negative.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_INVALID_OPERATION  A frame of the context is open already, or the last
 *                                       vgl_d3d11_backend_device_reset failed.
 * @retval VGL_RESULT_OUT_OF_MEMORY      The frame could not be begun (vgl_context_frame_begin).
 *
 * @par Thread safety
 * Render thread, on the thread that owns the device context.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_draw(vgl_d3d11_backend* backend,
                                                const vgl_d3d11_draw_info* info);

/**
 * Sets the clouds the backend draws with VGL_DRAW_FEATURE_CLOUDS, as
 * vgl_gl_backend_clouds_set describes: a coverage image of the whole globe, copied with its mip
 * levels, and kept for vgl_d3d11_backend_device_reset.
 *
 * @param backend   The backend.
 * @param coverage  The image, width x height bytes, rows from the north; or null with width
 *                  and height 0 to remove the clouds.
 * @param width     Pixels from west to east, from 2 to 16,384.
 * @param height    Pixels from north to south, from 2 to 8,192.
 *
 * @retval VGL_RESULT_SUCCESS           The clouds were set.
 * @retval VGL_RESULT_INVALID_ARGUMENT  backend is null, or the image is null or of a size
 *                                      outside the limits, but for a removal.
 * @retval VGL_RESULT_OUT_OF_MEMORY     The copy could not be allocated; the clouds set before
 *                                      stay.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_clouds_set(vgl_d3d11_backend* backend,
                                                      const uint8_t* coverage,
                                                      uint32_t width,
                                                      uint32_t height);

/**
 * Sets the stars the backend draws with VGL_DRAW_FEATURE_STARS, replacing those
 * set before, as vgl_gl_backend_stars_set does: copied, kept for vgl_d3d11_backend_device_reset,
 * and drawn as small quads. vgl ships no stars.
 *
 * @param backend     The backend.
 * @param stars       The stars, or null with star_count 0 to remove them.
 * @param star_count  How many, at most VGL_STAR_COUNT_MAX.
 *
 * @retval VGL_RESULT_SUCCESS           The stars were set.
 * @retval VGL_RESULT_INVALID_ARGUMENT  backend is null, stars is null with a non-zero
 *                                      star_count, there are
 *                                      too many, or a star is invalid (vgl_gl_backend_stars_set).
 * @retval VGL_RESULT_OUT_OF_MEMORY     The copy could not be allocated; the stars set before
 *                                      stay.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_stars_set(vgl_d3d11_backend* backend,
                                                     const vgl_star* stars,
                                                     uint32_t star_count);

/**
 * Recovers from a removed device: forgets the backend's objects of the old device without
 * using them again, takes the new device and device context, and creates its shaders and states
 * on them. The context forgets its render resources too, and offers them again over the next
 * frames.
 *
 * @param backend         The backend.
 * @param device          The new device. The backend holds a reference.
 * @param device_context  The device context to draw with on it. The backend holds a reference.
 *
 * @retval VGL_RESULT_SUCCESS           The backend is ready on the new device.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 * @retval VGL_RESULT_NOT_SUPPORTED     The new device's feature level is below 11_0. Draws fail
 *                                      until a reset succeeds.
 * @retval VGL_RESULT_BACKEND_ERROR     Direct3D failed to create a shader or state.
 *
 * @par Thread safety
 * Render thread, on the thread that owns the new device context.
 */
VGL_D3D11_API vgl_result vgl_d3d11_backend_device_reset(vgl_d3d11_backend* backend,
                                                        ID3D11Device* device,
                                                        ID3D11DeviceContext* device_context);

#ifdef __cplusplus
}
#endif

#endif /* VGL_D3D11_H */
