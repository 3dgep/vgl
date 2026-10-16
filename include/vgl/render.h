/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * The render interface (docs/render-interface.md): the contract between the core and whatever
 * draws the globe. Each frame, on the render thread, a backend or host takes the resources to
 * destroy and create, and the list of tiles to draw; the core never calls into it.
 */
#ifndef VGL_RENDER_H
#define VGL_RENDER_H

#include <vgl/base.h>
#include <vgl/context.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------- */
/* Data formats (sections 6 and 7)                                                          */
/* ---------------------------------------------------------------------------------------- */

/*
 * Terrain vertices: VGL_TERRAIN_VERTEX_SIZE bytes each, with these attributes at these offsets.
 * The layout is fixed for the major version.
 */
#define VGL_TERRAIN_VERTEX_SIZE 24
/* Position relative to the mesh's centre, along the ECEF axes: 3 x float. */
#define VGL_TERRAIN_VERTEX_POSITION_OFFSET 0
/* Height above the ellipsoid: float. */
#define VGL_TERRAIN_VERTEX_HEIGHT_OFFSET 12
/* Texture coordinates over the tile, u east and v south: 2 x unsigned 16-bit normalized. */
#define VGL_TERRAIN_VERTEX_UV_OFFSET 16
/* Normal in octahedral encoding: 2 x signed 16-bit normalized. */
#define VGL_TERRAIN_VERTEX_NORMAL_OFFSET 20

typedef enum vgl_texture_format
{
    VGL_TEXTURE_FORMAT_RGBA8 = 1, /* Always supported. */
    VGL_TEXTURE_FORMAT_RGBA8_SRGB = 2,
    VGL_TEXTURE_FORMAT_BC1 = 3,
    VGL_TEXTURE_FORMAT_BC7 = 4,
    VGL_TEXTURE_FORMAT_ETC2_RGB8 = 5,
    VGL_TEXTURE_FORMAT_ASTC_4X4 = 6,
    VGL_TEXTURE_FORMAT_MAX_ENUM = 0x7FFFFFFF
} vgl_texture_format;

/**
 * States which texture formats the consumer can sample. RGBA8 is always assumed. Call it once,
 * when the backend is created; until then, textures are RGBA8.
 *
 * @param context       The context.
 * @param formats       The formats, most preferred first. May be null if format_count is 0.
 * @param format_count  Number of formats.
 *
 * @retval VGL_RESULT_SUCCESS           The formats were set.
 * @retval VGL_RESULT_INVALID_ARGUMENT  context is null, formats is null with a non-zero count,
 *                                      or a format is unknown.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_texture_formats_set(vgl_context* context,
                                                   const vgl_texture_format* formats,
                                                   uint32_t format_count);

/* ---------------------------------------------------------------------------------------- */
/* Depth conventions (section 8)                                                            */
/* ---------------------------------------------------------------------------------------- */

/*
 * How the globe's depth is written and compared. The host owns the depth buffer and the
 * projection, and chooses the convention when it creates a backend; geometry it draws with
 * the globe uses the same one, so that the two depth-test against each other.
 */
typedef enum vgl_depth_convention
{
    /*
     * The default, and available everywhere. The shaders write log2(1 + w) / log2(1 + far) as
     * window depth, for the camera distance w and the far distance given when drawing
     * (VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT unless set), so the projection's near and far planes
     * do not matter. Depth is compared with LESS and cleared to 1.
     */
    VGL_DEPTH_CONVENTION_LOGARITHMIC = 0,
    /*
     * The host's projection maps the near plane to 1 and the far plane (ideally at infinity)
     * to 0, in a depth range from 0 to 1. Depth is compared with GREATER and cleared to 0,
     * ideally in a 32-bit floating-point buffer. OpenGL needs clip control for it.
     */
    VGL_DEPTH_CONVENTION_REVERSED_Z = 1,
    /* The host's projection as it is, compared with LESS. Small scenes only: at planetary
     * scale, distant terrain z-fights. */
    VGL_DEPTH_CONVENTION_CONVENTIONAL = 2,
    VGL_DEPTH_CONVENTION_MAX_ENUM = 0x7FFFFFFF
} vgl_depth_convention;

/* The far distance of the logarithmic convention when the host sets none, in metres. */
#define VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT 1.0e8

/* ---------------------------------------------------------------------------------------- */
/* Resources (section 4)                                                                  */
/* ---------------------------------------------------------------------------------------- */

typedef struct vgl_tile_id
{
    uint32_t level;
    uint32_t x;
    uint32_t y;
    uint32_t reserved;
} vgl_tile_id;

/* A shared index list to create. Every mesh of one grid size uses the same list. */
typedef struct vgl_index_list_data
{
    uint64_t resource;
    /* index_count indices of index_size bytes. */
    const void* indices;
    VGL_POINTER_PADDING(indices_padding)
    uint32_t index_count;
    /* 2 or 4. */
    uint32_t index_size;
} vgl_index_list_data;

/* A tile mesh to create. */
typedef struct vgl_mesh_data
{
    uint64_t resource;
    /* The index list it is drawn with. Created in the same frame or an earlier one. */
    uint64_t index_list;
    /* vertex_count vertices of VGL_TERRAIN_VERTEX_SIZE bytes. */
    const void* vertices;
    VGL_POINTER_PADDING(vertices_padding)
    uint32_t vertex_count;
    uint32_t vertex_size;
    vgl_tile_id tile;
    /* ECEF origin of the vertex positions. */
    vgl_dvec3 centre;
    double min_height;
    double max_height;
} vgl_mesh_data;

/* An imagery texture to create. Not produced yet: imagery comes later. */
typedef struct vgl_texture_data
{
    uint64_t resource;
    vgl_texture_format format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    /* mip_count pointers to the levels, largest first, and their sizes in bytes. */
    const void* const* mip_data;
    VGL_POINTER_PADDING(mip_data_padding)
    const size_t* mip_sizes;
    VGL_POINTER_PADDING(mip_sizes_padding)
} vgl_texture_data;

/* ---------------------------------------------------------------------------------------- */
/* Frames (sections 3 and 5)                                                                */
/* ---------------------------------------------------------------------------------------- */

typedef struct vgl_frame vgl_frame;

/* One imagery layer of a draw item. */
typedef struct vgl_draw_layer
{
    uint64_t texture;
    /* As set by vgl_frame_resource_handle_set, or 0. */
    uint64_t texture_handle;
    /* Maps the mesh's texture coordinates into the texture: uv * scale + offset, as
     * (scale u, scale v, offset u, offset v). */
    float uv_scale_offset[4];
    float opacity;
    uint32_t reserved;
} vgl_draw_layer;

/* A tile to draw. */
typedef struct vgl_draw_item
{
    vgl_tile_id tile;
    uint64_t mesh;
    /* As set by vgl_frame_resource_handle_set, or 0. */
    uint64_t mesh_handle;
    uint64_t index_list;
    uint64_t index_list_handle;
    uint32_t index_count;
    /* 2 or 4, as in the index list. */
    uint32_t index_size;
    /* The item's layers in the frame's layer list, bottom first. */
    uint32_t first_layer;
    uint32_t layer_count;
    /* Mesh coordinates to camera space, column-major: composed in double precision and
     * rounded to float at the end. */
    float model_view[16];
    /* ECEF origin of the mesh coordinates, for consumers that compose their own transforms. */
    vgl_dvec3 centre;
    double min_height;
    double max_height;
} vgl_draw_item;

/* Parameters of a frame. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_frame_begin_info
{
    uint32_t struct_size;
    /* sizeof(vgl_draw_item) in the consumer's header; draw items are written at this stride.
     * 0 selects the size in this header. */
    uint32_t draw_item_size;
    /* Bytes of resources the consumer may create in this frame. 0 selects 16 MiB. At least
     * one resource is always offered. */
    uint64_t upload_budget;
    /* Optional. The camera to compose model-view matrices for, when it differs from the view
     * of the last update. Null uses the view set with vgl_context_view_set. */
    const vgl_dmat4* view_matrix;
    VGL_POINTER_PADDING(view_matrix_padding)
} vgl_frame_begin_info;

/**
 * Begins a frame: decides what to destroy, create and draw. Apply the lists in this order:
 * destroy, index lists, meshes, textures; then draw. Without a view, the frame draws nothing,
 * but still destroys what is no longer needed.
 *
 * @param context    The context.
 * @param info       Optional. Null selects the defaults.
 * @param out_frame  Receives the frame, valid until vgl_frame_end.
 *
 * @retval VGL_RESULT_SUCCESS            The frame began.
 * @retval VGL_RESULT_INVALID_ARGUMENT   context or out_frame is null, or info is invalid.
 * @retval VGL_RESULT_INVALID_OPERATION  A frame of the context is already open.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_OUT_OF_MEMORY      The frame's lists could not grow. They do not grow once
 *                                       they have reached the size of the view.
 *
 * @par Thread safety
 * Render thread. Must not overlap with vgl_context_update.
 */
VGL_API vgl_result vgl_context_frame_begin(vgl_context* context,
                                           const vgl_frame_begin_info* info,
                                           vgl_frame** out_frame);

/**
 * The frame's lists: views into library memory, valid until vgl_frame_end. Each function
 * writes the address of the first element and the number of elements; an empty list has a
 * count of 0.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
/* Resources to destroy, first. */
VGL_API vgl_result vgl_frame_destroys_get(const vgl_frame* frame,
                                          const uint64_t** out_resources,
                                          uint32_t* out_count);
VGL_API vgl_result vgl_frame_index_lists_get(const vgl_frame* frame,
                                             const vgl_index_list_data** out_index_lists,
                                             uint32_t* out_count);
VGL_API vgl_result vgl_frame_meshes_get(const vgl_frame* frame,
                                        const vgl_mesh_data** out_meshes,
                                        uint32_t* out_count);
VGL_API vgl_result vgl_frame_textures_get(const vgl_frame* frame,
                                          const vgl_texture_data** out_textures,
                                          uint32_t* out_count);
/* Tiles to draw, nearest first, draw_item_size bytes apart. */
VGL_API vgl_result vgl_frame_draw_list_get(const vgl_frame* frame,
                                           const vgl_draw_item** out_items,
                                           uint32_t* out_count);
VGL_API vgl_result vgl_frame_draw_layers_get(const vgl_frame* frame,
                                             const vgl_draw_layer** out_layers,
                                             uint32_t* out_count);

/**
 * Sets a handle that later draw items carry for a resource, such as a pointer to the
 * consumer's own buffer object. Optional.
 *
 * @retval VGL_RESULT_SUCCESS           The handle was set.
 * @retval VGL_RESULT_INVALID_ARGUMENT  frame is null.
 * @retval VGL_RESULT_NOT_FOUND         The resource does not exist, or is destroyed in this
 *                                      frame.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_resource_handle_set(vgl_frame* frame,
                                                 uint64_t resource,
                                                 uint64_t handle);

/**
 * Reports that the consumer could not create a resource offered in this frame, for example for
 * lack of GPU memory. It is offered again in a later frame, and is not drawn until then.
 *
 * @retval VGL_RESULT_SUCCESS           The failure was recorded.
 * @retval VGL_RESULT_INVALID_ARGUMENT  frame is null, or reason is not an error.
 * @retval VGL_RESULT_NOT_FOUND         The resource was not offered in this frame.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_resource_failed(vgl_frame* frame,
                                             uint64_t resource,
                                             vgl_result reason);

/**
 * Ends a frame. Every resource offered in it, and not reported as failed, counts as created;
 * the CPU copies of meshes are freed.
 *
 * @param frame  The frame, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_end(vgl_frame* frame);

/**
 * Forgets every resource, because the consumer has lost them all, for example with its
 * graphics device. They are rebuilt and offered again as create events in later frames.
 * Destroying a context does not destroy the consumer's resources either; the consumer frees
 * its own objects.
 *
 * @retval VGL_RESULT_SUCCESS            The resources were forgotten.
 * @retval VGL_RESULT_INVALID_ARGUMENT   context is null.
 * @retval VGL_RESULT_INVALID_OPERATION  A frame is open.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_context_resources_reset(vgl_context* context);

#ifdef __cplusplus
}
#endif

#endif /* VGL_RENDER_H */
