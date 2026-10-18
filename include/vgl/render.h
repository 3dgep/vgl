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

/**
 * Terrain vertices: VGL_TERRAIN_VERTEX_SIZE bytes each, with these attributes at these offsets.
 * The layout is fixed for the major version.
 */
#define VGL_TERRAIN_VERTEX_SIZE 24
/** Position relative to the mesh's centre, along the ECEF axes: 3 x float. */
#define VGL_TERRAIN_VERTEX_POSITION_OFFSET 0
/** Height above the ellipsoid: float. */
#define VGL_TERRAIN_VERTEX_HEIGHT_OFFSET 12
/** Texture coordinates over the tile, u east and v south: 2 x unsigned 16-bit normalized. */
#define VGL_TERRAIN_VERTEX_UV_OFFSET 16
/** Normal in octahedral encoding: 2 x signed 16-bit normalized. */
#define VGL_TERRAIN_VERTEX_NORMAL_OFFSET 20

/**
 * Terrain samples: VGL_TERRAIN_SAMPLE_SIZE bytes each, with these fields at these offsets. A
 * mesh's samples are the surface it was built from, at full resolution where its grid is
 * coarser, for shading. The layout is fixed for the major version.
 */
#define VGL_TERRAIN_SAMPLE_SIZE 8
/** Height above the ellipsoid: float. */
#define VGL_TERRAIN_SAMPLE_HEIGHT_OFFSET 0
/** Normal in octahedral encoding: 2 x signed 16-bit normalized. */
#define VGL_TERRAIN_SAMPLE_NORMAL_OFFSET 4

/**
 * A star, for the backends that draw a star field behind the sky (design document, section 7.6).
 * The host supplies the stars, from a catalogue of its choice: vgl ships none.
 */
typedef struct vgl_star
{
    /** Unit direction in the celestial frame of J2000: x towards the vernal equinox, z towards
     * the north celestial pole (vgl_earth_rotation_angle_get turns it into ECEF). */
    float direction[3];
    /** Apparent visual magnitude: about -1.5 for Sirius, 6.5 for the faintest seen by eye. */
    float magnitude;
    /** Linear RGB, 1 for the brightest channel. */
    float color[3];
    float reserved; /**< 0 */
} vgl_star;

typedef enum vgl_texture_format
{
    VGL_TEXTURE_FORMAT_RGBA8 = 1, /**< Always supported. */
    VGL_TEXTURE_FORMAT_RGBA8_SRGB = 2,
    VGL_TEXTURE_FORMAT_BC1 = 3,
    VGL_TEXTURE_FORMAT_BC7 = 4,
    VGL_TEXTURE_FORMAT_ETC2_RGB8 = 5,
    VGL_TEXTURE_FORMAT_ASTC_4X4 = 6,
    VGL_TEXTURE_FORMAT_MAX_ENUM = 0x7FFFFFFF
} vgl_texture_format;

/**
 * States which texture formats the consumer can sample. RGBA8 is always assumed. Call it when
 * the backend is created; until then, textures are RGBA8. A later call replaces the list, for
 * the textures created after it; those created before keep their format. Repeats are ignored.
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

/**
 * How the globe's depth is written and compared. The host owns the depth buffer and the
 * projection, and chooses the convention when it creates a backend; geometry it draws with
 * the globe uses the same one, so that the two depth-test against each other.
 */
typedef enum vgl_depth_convention
{
    /**
     * The default, and available everywhere. The shaders write log2(1 + w) / log2(1 + far) as
     * window depth, for the camera distance w and the far distance given when drawing
     * (VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT unless set), so the projection's near and far planes
     * do not matter. Depth is compared with LESS and cleared to 1.
     */
    VGL_DEPTH_CONVENTION_LOGARITHMIC = 0,
    /**
     * The host's projection maps the near plane to 1 and the far plane (ideally at infinity)
     * to 0, in a depth range from 0 to 1. Depth is compared with GREATER and cleared to 0,
     * ideally in a 32-bit floating-point buffer. OpenGL needs clip control for it.
     */
    VGL_DEPTH_CONVENTION_REVERSED_Z = 1,
    /** The host's projection as it is, compared with LESS. Small scenes only: at planetary
     * scale, distant terrain z-fights. */
    VGL_DEPTH_CONVENTION_CONVENTIONAL = 2,
    VGL_DEPTH_CONVENTION_MAX_ENUM = 0x7FFFFFFF
} vgl_depth_convention;

/** The far distance of the logarithmic convention when the host sets none, in metres. */
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

/** A shared index list to create. Every mesh of one grid size uses the same list. */
typedef struct vgl_index_list_data
{
    uint64_t resource;
    /** index_count indices of index_size bytes. */
    const void* indices;
    VGL_POINTER_PADDING(indices_padding)
    uint32_t index_count;
    /** 2 or 4. */
    uint32_t index_size;
} vgl_index_list_data;

/** A tile mesh to create. */
typedef struct vgl_mesh_data
{
    uint64_t resource;
    /** The index list it is drawn with. Created in the same frame or an earlier one. */
    uint64_t index_list;
    /** vertex_count vertices of VGL_TERRAIN_VERTEX_SIZE bytes. */
    const void* vertices;
    VGL_POINTER_PADDING(vertices_padding)
    uint32_t vertex_count;
    uint32_t vertex_size;
    vgl_tile_id tile;
    /** ECEF origin of the vertex positions. */
    vgl_dvec3 centre;
    double min_height;
    double max_height;
    /** sample_grid x sample_grid samples of VGL_TERRAIN_SAMPLE_SIZE bytes, in rows from the
     * north, each from the west, at texture coordinates (i, j) / (sample_grid - 1). */
    const void* samples;
    VGL_POINTER_PADDING(samples_padding)
    uint32_t sample_grid;
    uint32_t sample_size;
} vgl_mesh_data;

/** An imagery texture to create. */
typedef struct vgl_texture_data
{
    uint64_t resource;
    vgl_texture_format format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;
    /** mip_count pointers to the levels, largest first, and their sizes in bytes. */
    const void* const* mip_data;
    VGL_POINTER_PADDING(mip_data_padding)
    const size_t* mip_sizes;
    VGL_POINTER_PADDING(mip_sizes_padding)
} vgl_texture_data;

/* ---------------------------------------------------------------------------------------- */
/* Frames (sections 3 and 5)                                                                */
/* ---------------------------------------------------------------------------------------- */

typedef struct vgl_frame vgl_frame;

/** One imagery layer of a draw item. */
typedef struct vgl_draw_layer
{
    uint64_t texture;
    /** As set by vgl_frame_resource_handle_set, or 0. */
    uint64_t texture_handle;
    /** Maps the mesh's texture coordinates into the texture: uv * scale + offset, as
     * (scale u, scale v, offset u, offset v). */
    float uv_scale_offset[4];
    float opacity;
    uint32_t reserved;
} vgl_draw_layer;

/** A tile to draw. */
typedef struct vgl_draw_item
{
    vgl_tile_id tile;
    uint64_t mesh;
    /** As set by vgl_frame_resource_handle_set, or 0. */
    uint64_t mesh_handle;
    uint64_t index_list;
    uint64_t index_list_handle;
    uint32_t index_count;
    /** 2 or 4, as in the index list. */
    uint32_t index_size;
    /** The item's layers in the frame's layer list, bottom first. */
    uint32_t first_layer;
    uint32_t layer_count;
    /** Mesh coordinates to camera space, column-major: composed in double precision and
     * rounded to float at the end. */
    float model_view[16];
    /** ECEF origin of the mesh coordinates, for consumers that compose their own transforms. */
    vgl_dvec3 centre;
    double min_height;
    double max_height;
    /** The geoid's height above the ellipsoid at the tile's corners, in metres: north-west,
     * north-east, south-west and south-east. Interpolated bilinearly over the mesh's texture
     * coordinates (u east, v south), it gives mean sea level under each vertex: a vertex's
     * height above mean sea level is its height minus that, and a sea surface lies there.
     * Zero where has_geoid is VGL_FALSE: the context has no geoid pack, or it has not loaded
     * there yet. */
    float geoid_heights[4];
    vgl_bool has_geoid;
    uint32_t reserved;
} vgl_draw_item;

/** Parameters of a frame. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_frame_begin_info
{
    uint32_t struct_size;
    /**
     * The sizes of the frame's structures in the consumer's header: the frame's lists are
     * written at these strides, so that a consumer built against a later minor version, whose
     * structures have grown, reads them with its own layout, with zeros in the fields this
     * version does not have (docs/render-interface.md, section 5). 0 selects the size in this
     * header, which is also the smallest accepted.
     */
    uint32_t draw_item_size;       /**< sizeof(vgl_draw_item) */
    uint32_t draw_layer_size;      /**< sizeof(vgl_draw_layer) */
    uint32_t index_list_data_size; /**< sizeof(vgl_index_list_data) */
    uint32_t mesh_data_size;       /**< sizeof(vgl_mesh_data) */
    uint32_t texture_data_size;    /**< sizeof(vgl_texture_data) */
    /** Bytes of resources the consumer may create in this frame. 0 selects
     * VGL_UPLOAD_BUDGET_DEFAULT. At least one resource is always offered. */
    uint64_t upload_budget;
    /** Optional. The camera to compose model-view matrices for, when it differs from the view
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
 * Gets the resources to destroy, which come first: a view into library memory, valid until
 * vgl_frame_end.
 *
 * @param frame      The frame.
 * @param out_resources Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_destroys_get(const vgl_frame* frame,
                                          const uint64_t** out_resources,
                                          uint32_t* out_count);

/**
 * Gets the index lists to create: a view into library memory, valid until vgl_frame_end. Elements
 * are index_list_data_size bytes apart (vgl_frame_begin_info).
 *
 * @param frame      The frame.
 * @param out_index_lists Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_index_lists_get(const vgl_frame* frame,
                                             const vgl_index_list_data** out_index_lists,
                                             uint32_t* out_count);

/**
 * Gets the meshes to create: a view into library memory, valid until vgl_frame_end. Elements are
 * mesh_data_size bytes apart (vgl_frame_begin_info).
 *
 * @param frame      The frame.
 * @param out_meshes Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_meshes_get(const vgl_frame* frame,
                                        const vgl_mesh_data** out_meshes,
                                        uint32_t* out_count);

/**
 * Gets the textures to create: a view into library memory, valid until vgl_frame_end. Elements are
 * texture_data_size bytes apart (vgl_frame_begin_info).
 *
 * @param frame      The frame.
 * @param out_textures Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_textures_get(const vgl_frame* frame,
                                          const vgl_texture_data** out_textures,
                                          uint32_t* out_count);

/**
 * Gets the tiles to draw, nearest first: a view into library memory, valid until vgl_frame_end.
 * Elements are draw_item_size bytes apart (vgl_frame_begin_info). Read it after applying the
 * frame's creates: items may draw resources created in this frame, and carry the handles set for
 * them. Skip an item whose mesh or index list could not be created.
 *
 * @param frame      The frame.
 * @param out_items Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_draw_list_get(const vgl_frame* frame,
                                           const vgl_draw_item** out_items,
                                           uint32_t* out_count);

/**
 * Gets the imagery layers that draw items refer to: a view into library memory, valid until
 * vgl_frame_end. Elements are draw_layer_size bytes apart (vgl_frame_begin_info); a draw item's
 * layers are layer_count of them from first_layer.
 *
 * @param frame      The frame.
 * @param out_layers Receives the address of the first element.
 * @param out_count  Receives the number of elements; 0 for an empty list.
 *
 * @retval VGL_RESULT_SUCCESS           The list was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_frame_draw_layers_get(const vgl_frame* frame,
                                             const vgl_draw_layer** out_layers,
                                             uint32_t* out_count);

/**
 * Sets a handle that later draw items carry for a resource, such as a pointer to the
 * consumer's own buffer object. Optional.
 *
 * @param frame     The frame.
 * @param resource  The resource, offered in this frame or an earlier one.
 * @param handle    The handle; any value, which the library only passes on.
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
 * @param frame     The frame.
 * @param resource  The resource offered in this frame.
 * @param reason    Why it could not be created: an error, such as VGL_RESULT_OUT_OF_MEMORY.
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
 * @param context  The context.
 *
 * @retval VGL_RESULT_SUCCESS            The resources were forgotten.
 * @retval VGL_RESULT_INVALID_ARGUMENT   context is null.
 * @retval VGL_RESULT_INVALID_OPERATION  A frame is open.
 *
 * @par Thread safety
 * Render thread.
 */
VGL_API vgl_result vgl_context_resources_reset(vgl_context* context);

/* ---------------------------------------------------------------------------------------- */
/* What backends draw                                                                       */
/* ---------------------------------------------------------------------------------------- */

/**
 * What a backend draws besides the terrain and its imagery: a combination of vgl_draw_feature
 * values in each backend's draw settings (vgl_gl_draw_info.features, vgl_d3d11_draw_info.features,
 * vgl::ogre::globe_settings::features).
 */
typedef uint32_t vgl_draw_features;

typedef enum vgl_draw_feature
{
    /** A sea surface: terrain below mean sea level is raised to it and shaded as water, for
     * applications that want a flat sea rather than the ocean floor. It needs a geoid pack
     * (vgl_draw_item.has_geoid); without one, mean sea level is the ellipsoid. */
    VGL_DRAW_FEATURE_SEA_SURFACE = 0x1,
    /** The atmosphere (design document, section 7.6): the sky, the glow around the globe seen
     * from space, and the haze between the camera and the terrain, lit by the sun (the draw's
     * sun direction, or straight above the camera when that is zero), which then also reddens
     * and dims the sunlight on the terrain and leaves the night side dark. The sky is drawn
     * where nothing was drawn before it, at the far depth: blended over what the render target
     * holds there, which shows through where the sky is thin, as in space. */
    VGL_DRAW_FEATURE_ATMOSPHERE = 0x2,
    /** The stars set with the backend's stars_set, behind the sky: where nothing was drawn
     * before them, at the far depth, added to what the render target holds there. With the
     * atmosphere they fade where the sky is bright, as by day. Each is drawn as a small quad of
     * two triangles, not as a point. */
    VGL_DRAW_FEATURE_STARS = 0x4,
    /** With the atmosphere, the clouds set with the backend's clouds_set: a shell at the draw's
     * cloud height above the ellipsoid, over the terrain and in the sky, lit by the sun, which
     * they shade the terrain from. */
    VGL_DRAW_FEATURE_CLOUDS = 0x8,
    /** Contour lines every contour interval metres above mean sea level, every fifth one
     * heavier. Without a geoid pack, heights are above the ellipsoid, which differs from mean
     * sea level by up to about 100 m. */
    VGL_DRAW_FEATURE_CONTOURS = 0x10,
    VGL_DRAW_FEATURE_MAX_ENUM = 0x7FFFFFFF
} vgl_draw_feature;

/**
 * Views for inspecting level of detail and loading: a combination of vgl_debug_view values in
 * each backend's draw settings. 0 draws normally.
 */
typedef uint32_t vgl_debug_views;

typedef enum vgl_debug_view
{
    /** The mesh's grid cells as bright cyan lines a pixel wide over the shading, without the
     * diagonals that split them into triangles. Where cells are closer than four pixels, every
     * second, fourth, eighth line and so on, which keeps them that far apart. The tiles'
     * boundaries are always drawn, in yellow and about two pixels wide. */
    VGL_DEBUG_VIEW_WIREFRAME = 0x1,
    /** Each tile tinted by its level, with its borders outlined. */
    VGL_DEBUG_VIEW_LEVEL_COLORS = 0x2,
    /** Tiles drawn with imagery coarser than themselves tinted orange, more strongly the more
     * levels coarser it is, and tiles without imagery magenta. */
    VGL_DEBUG_VIEW_IMAGERY_SOURCE = 0x4,
    /** Colours by height above mean sea level, from the deep ocean floor to snow, instead of
     * the imagery. */
    VGL_DEBUG_VIEW_HEIGHT_COLORS = 0x8,
    VGL_DEBUG_VIEW_MAX_ENUM = 0x7FFFFFFF
} vgl_debug_view;

/** Bytes of resources a frame may create when the consumer gives no upload budget. */
#define VGL_UPLOAD_BUDGET_DEFAULT (16ull * 1024 * 1024)
/** Height between contour lines when a draw gives none, in metres. */
#define VGL_CONTOUR_INTERVAL_DEFAULT 500.0
/** The cloud shell's height above the ellipsoid when a draw gives none, in metres: low and
 * middle clouds. */
#define VGL_CLOUD_HEIGHT_DEFAULT 6000.0
/** The most stars a backend takes, which keeps its vertex buffer under 100 MiB. */
#define VGL_STAR_COUNT_MAX 400000u
/** Limits of a cloud coverage image, in pixels, which every backend's textures take. */
#define VGL_CLOUD_IMAGE_SIZE_MIN   2u
#define VGL_CLOUD_IMAGE_WIDTH_MAX  16384u
#define VGL_CLOUD_IMAGE_HEIGHT_MAX 8192u

#ifdef __cplusplus
}
#endif

#endif /* VGL_RENDER_H */
