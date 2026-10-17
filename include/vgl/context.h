/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl context: the object that holds all library state (NFR-06), the packs it reads, and
 * height queries (FR-03, FR-05, FR-06).
 *
 * A minimal host creates a context with an I/O interface, adds packs, and calls
 * vgl_context_update once per frame. Packs load asynchronously; height queries start the loads
 * they need and report VGL_RESULT_NOT_READY until finer data has arrived.
 */
#ifndef VGL_CONTEXT_H
#define VGL_CONTEXT_H

#include <vgl/base.h>
#include <vgl/foundation.h>
#include <vgl/geodesy.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vgl_context vgl_context;

/* Creation parameters of a context. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_context_create_info
{
    uint32_t struct_size;
    /* Minimum level of messages passed to log. VGL_LOG_LEVEL_DEFAULT is warnings. */
    vgl_log_level log_level;
    /* Memory for decoded tiles and pack metadata, in bytes. 0 selects 256 MiB. */
    uint64_t cpu_budget;
    /* Optional. Every allocation of the context goes through it; null uses the C heap. */
    const vgl_allocator_interface* allocator;
    /* Optional. How the context reads packs. Without it, packs cannot be created. */
    const vgl_io_interface* io;
    /* Optional. Where decoding runs. Without it, vgl_context_update decodes. */
    const vgl_task_interface* tasks;
    /* Optional. Reference ellipsoid; null selects WGS84. Packs must use the same. */
    const vgl_ellipsoid* ellipsoid;
    /* Optional. Receives log messages; without it, the context is silent. */
    vgl_log_fn log;
    void* log_user_data;
    /* GPU memory for the resources of the render interface, in bytes. 0 selects 512 MiB
     * (docs/render-interface.md, section 4.3). */
    uint64_t gpu_budget;
} vgl_context_create_info;

/**
 * Creates a context.
 *
 * The interface tables are copied; their user_data pointers must stay valid until the context
 * is destroyed, and the allocator must outlive every request and task of the context.
 *
 * @param info         Optional. Null selects all defaults (and no I/O).
 * @param out_context  Receives the context.
 *
 * @retval VGL_RESULT_SUCCESS           The context was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT  out_context is null, or info or one of its interfaces
 *                                      is invalid.
 * @retval VGL_RESULT_NOT_SUPPORTED     info uses fields of a newer version of vgl.
 * @retval VGL_RESULT_VERSION_MISMATCH  An interface has a version this vgl does not know.
 * @retval VGL_RESULT_OUT_OF_MEMORY     Allocation failed.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_context_create(const vgl_context_create_info* info,
                                      vgl_context** out_context);

/**
 * Destroys a context, and every pack that remains in it (with a warning for each).
 *
 * Reads and tasks still outstanding are cancelled; when the host completes them later, the
 * completions are discarded. Tasks that are running are waited for.
 *
 * @param context  The context, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_destroy(vgl_context* context);

/**
 * Gets the allocator of the context: the host's, or one that uses the C heap. Libraries that
 * work on behalf of a context, such as the backends, allocate through it, so that all of the
 * context's memory reaches the host's allocator (NFR-03). The table stays valid while the
 * context exists.
 *
 * @param context        The context.
 * @param out_allocator  Receives the allocator interface.
 *
 * @retval VGL_RESULT_SUCCESS           The interface was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_context_allocator_get(const vgl_context* context,
                                             vgl_allocator_interface* out_allocator);

/**
 * Passes a message to the context's log callback, if one is set and the level is enabled.
 * Libraries that work on behalf of a context, such as the backends, log through it, so that
 * their messages reach the host's log (docs/c-api.md, section 13).
 *
 * @param context  The context.
 * @param level    Severity: VGL_LOG_LEVEL_ERROR to VGL_LOG_LEVEL_DEBUG.
 * @param message  UTF-8 text without a trailing newline, starting with the name of the API
 *                 function involved.
 *
 * @retval VGL_RESULT_SUCCESS           The message was passed on, or the level is disabled.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, or level is not a message level.
 *
 * @par Thread safety
 * Free-threaded. The log callback runs on the calling thread.
 */
VGL_API vgl_result vgl_context_log(const vgl_context* context,
                                   vgl_log_level level,
                                   const char* message);

/* Parameters of an update. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_update_info
{
    uint32_t struct_size;
    uint32_t reserved;
    /* Time the update may spend decoding, in seconds. 0 selects 2 ms. Completions are always
     * processed in full, and at least one decode runs if any is waiting. */
    double time_budget;
} vgl_update_info;

/* ---------------------------------------------------------------------------------------- */
/* View and update                                                                          */
/* ---------------------------------------------------------------------------------------- */

/*
 * A view of the globe, for tile selection (design document, section 7.3). Matrices are
 * column-major and multiply column vectors (vgl_dmat4). Hosts with row-major matrices that
 * multiply row vectors, as is common with Direct3D, pass theirs unchanged: the two differences
 * cancel out (docs/c-api.md, section 4.2). Any handedness and any depth mapping work: OpenGL
 * or Direct3D clip space, reversed or infinite projections.
 */
typedef struct vgl_view
{
    uint32_t struct_size;
    uint32_t reserved;
    /* From ECEF to camera space. Must be invertible. */
    vgl_dmat4 view_matrix;
    /* From camera space to clip space: perspective or orthographic. */
    vgl_dmat4 projection_matrix;
    /* Size of the render target in pixels. Both must be at least 1. */
    uint32_t viewport_width;
    uint32_t viewport_height;
    /* Largest screen-space error of a selected tile, in pixels. 0 selects 2. */
    double max_screen_space_error;
} vgl_view;

/**
 * Sets the view that vgl_context_update selects tiles for. The view is copied.
 *
 * @param context  The context.
 * @param view     The view, or null to stop selecting tiles.
 *
 * @retval VGL_RESULT_SUCCESS           The view was set.
 * @retval VGL_RESULT_INVALID_ARGUMENT  context is null, or view is invalid: a matrix element
 *                                      is not finite, the view matrix is not invertible, the
 *                                      viewport is empty, or the error is negative.
 * @retval VGL_RESULT_NOT_SUPPORTED     view has fields from a newer version set.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_view_set(vgl_context* context, const vgl_view* view);

/**
 * Gets the view set with vgl_context_view_set, for example for a backend that needs its
 * projection matrix.
 *
 * @param context   The context.
 * @param out_view  Receives the view. The caller sets struct_size, and only the fields that
 *                  fit within it are written (docs/c-api.md, section 8.4).
 *
 * @retval VGL_RESULT_SUCCESS           The view was written.
 * @retval VGL_RESULT_NOT_FOUND         No view is set; out_view is unchanged.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, or out_view->struct_size is smaller
 *                                      than the first version of vgl_view.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_view_get(const vgl_context* context, vgl_view* out_view);

/**
 * Advances loading: processes completed reads and tasks, decodes (when there is no task
 * interface) within the time budget, selects tiles for the view and requests the ones that
 * are missing, and evicts decoded tiles beyond the CPU budget.
 *
 * @param context  The context.
 * @param info     Optional. Null selects the defaults.
 *
 * @retval VGL_RESULT_SUCCESS           The update ran.
 * @retval VGL_RESULT_INVALID_ARGUMENT  context is null or info is invalid.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_update(vgl_context* context, const vgl_update_info* info);

/* ---------------------------------------------------------------------------------------- */
/* Packs                                                                                    */
/* ---------------------------------------------------------------------------------------- */

typedef struct vgl_pack vgl_pack;

/* Creation parameters of a pack. Zero-initialize, set struct_size, then fill in. */
typedef struct vgl_pack_create_info
{
    uint32_t struct_size;
    /* Packs with a higher priority win where they have data (FR-05). Among equal priorities,
     * the pack created later wins. Ignored for a geoid pack. */
    int32_t priority;
    /* Location of the pack's directory, passed to the I/O interface with the pack's file
     * names appended after a '/'. Required. Copied. */
    const char* uri;
} vgl_pack_create_info;

typedef enum vgl_pack_state
{
    VGL_PACK_STATE_LOADING = 1, /* The manifest is being read. */
    VGL_PACK_STATE_READY = 2,   /* The pack is in use. */
    VGL_PACK_STATE_FAILED = 3,  /* The pack could not be used; see the failure result. */
    VGL_PACK_STATE_MAX_ENUM = 0x7FFFFFFF
} vgl_pack_state;

/**
 * Adds a pack to a context, and starts reading its manifest (docs/pack-format.md). An
 * elevation pack takes part in height queries once it is ready. A geoid pack provides heights
 * above mean sea level; a context takes at most one. Imagery packs are not used yet.
 *
 * @param context   The context.
 * @param info      The pack's location and priority.
 * @param out_pack  Receives the pack, which belongs to the context.
 *
 * @retval VGL_RESULT_SUCCESS            The pack was added and is loading.
 * @retval VGL_RESULT_INVALID_ARGUMENT   A pointer is null, or info is invalid.
 * @retval VGL_RESULT_INVALID_OPERATION  The context has no I/O interface.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_pack_create(vgl_context* context,
                                   const vgl_pack_create_info* info,
                                   vgl_pack** out_pack);

/**
 * Removes a pack from its context. Its outstanding reads are cancelled, and its decoded tiles
 * are freed.
 *
 * @param pack  The pack, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_pack_destroy(vgl_pack* pack);

/**
 * Gets the state of a pack.
 *
 * @param pack         The pack.
 * @param out_state    Receives the state.
 * @param out_failure  Optional. Receives why the pack failed, or VGL_RESULT_SUCCESS if it has
 *                     not: for example VGL_RESULT_NOT_FOUND, VGL_RESULT_CORRUPT_DATA or
 *                     VGL_RESULT_VERSION_MISMATCH. The log has the details.
 *
 * @retval VGL_RESULT_SUCCESS           The state was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  pack or out_state is null.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_pack_state_get(const vgl_pack* pack,
                                      vgl_pack_state* out_state,
                                      vgl_result* out_failure);

/* ---------------------------------------------------------------------------------------- */
/* Height queries                                                                           */
/* ---------------------------------------------------------------------------------------- */

typedef enum vgl_height_reference
{
    VGL_HEIGHT_REFERENCE_ELLIPSOID = 0,      /* Height above the reference ellipsoid. */
    VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL = 1, /* Height above the geoid (needs a geoid pack). */
    VGL_HEIGHT_REFERENCE_MAX_ENUM = 0x7FFFFFFF
} vgl_height_reference;

/**
 * Queries the terrain height at a geodetic position (FR-06).
 *
 * The height comes from the highest-priority elevation pack with data at the position, at the
 * deepest level loaded so far, interpolated as docs/pack-format.md, section 6.4 describes.
 * A query starts reading whatever finer data it needs; call vgl_context_update to receive it.
 *
 * @param context     The context.
 * @param latitude    Geodetic latitude in radians, in [-pi/2, pi/2].
 * @param longitude   Longitude in radians.
 * @param reference   What the height is relative to.
 * @param out_height  Receives the height in metres. NaN while no data has loaded at all.
 *
 * @retval VGL_RESULT_SUCCESS            The height is final: it comes from the finest data the
 *                                       packs have at the position.
 * @retval VGL_RESULT_NOT_READY          Finer data, a pack's manifest, or the geoid is still
 *                                       loading; out_height holds the best current value.
 * @retval VGL_RESULT_NOT_FOUND          No ready elevation pack has data at the position.
 * @retval VGL_RESULT_INVALID_OPERATION  reference is VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL, and
 *                                       the context has no geoid pack.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Starting a read of finer data failed; out_height is
 *                                       NaN. The query may be repeated.
 * @retval VGL_RESULT_INVALID_ARGUMENT   context or out_height is null, an angle is not finite or
 *                                       out of range, or reference is unknown.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_height_query(vgl_context* context,
                                            double latitude,
                                            double longitude,
                                            vgl_height_reference reference,
                                            double* out_height);

/* ---------------------------------------------------------------------------------------- */
/* Attribution                                                                              */
/* ---------------------------------------------------------------------------------------- */

/**
 * Gets the credits of the data in view (FR-08): the attribution lines of every pack whose tiles
 * the view selects or the last frame drew, in pack priority order, each distinct line once.
 * Hosts display them while the data is visible, as the data's licenses require. Uses the
 * two-call pattern (docs/c-api.md, section 6.3).
 *
 * @param context      The context.
 * @param inout_count  In: the capacity of out_strings. Out: the number of lines written, or
 *                     available if out_strings is null.
 * @param out_strings  Optional. Receives UTF-8 lines, valid until the pack they come from is
 *                     destroyed.
 *
 * @retval VGL_RESULT_SUCCESS           All lines were written, or counted.
 * @retval VGL_RESULT_INCOMPLETE        out_strings was too small; *inout_count lines were
 *                                      written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  context or inout_count is null.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_attribution_get(const vgl_context* context,
                                               uint32_t* inout_count,
                                               const char** out_strings);

/* ---------------------------------------------------------------------------------------- */
/* Statistics                                                                               */
/* ---------------------------------------------------------------------------------------- */

/*
 * What a context holds and does (design document, section 7.4). Set struct_size; the library
 * writes the fields that fit within it.
 */
typedef struct vgl_context_statistics
{
    uint32_t struct_size;
    uint32_t reserved;

    /* Memory, in bytes. */
    uint64_t cpu_used;
    uint64_t cpu_budget;
    uint64_t gpu_used; /* Resources offered to or created by the consumer. */
    uint64_t gpu_budget;

    /* Packs. */
    uint32_t packs_loading;
    uint32_t packs_failed;

    /* Work under way and cached. */
    uint32_t reads_outstanding;
    uint32_t tiles_decoding; /* Read, and waiting for or in decoding. */
    uint32_t tiles_decoded;  /* In the CPU cache. */
    uint32_t meshes_building;
    uint32_t meshes_waiting; /* Built for selected tiles, and waiting to be created. */
    uint32_t resources_resident;

    /* The view. */
    uint32_t tiles_selected;

    /* The last frame. */
    uint32_t tiles_drawn;
    uint32_t resources_created;
    uint32_t resources_destroyed;
    uint64_t bytes_created;

    /* Totals since the context was created. */
    uint64_t reads_total;
    uint64_t bytes_read_total;
    uint64_t tiles_loaded_total;

    /* Times, in seconds. */
    double tile_load_time_mean; /* From the start of a tile's read to its decoding. */
    double update_time;         /* Of the last vgl_context_update. */
    double frame_time;          /* Of the last vgl_context_frame_begin. */

    /* Imagery textures. */
    uint32_t textures_decoding; /* Imagery tiles being decoded into textures. */
    uint32_t textures_waiting;  /* Wanted by selected tiles, and not created by the consumer. */

    /* The last frame's meshes: vertices and triangles drawn, skirts included. */
    uint64_t vertices_drawn;
    uint64_t triangles_drawn;
} vgl_context_statistics;

/**
 * Gets the statistics of a context.
 *
 * @param context         The context.
 * @param out_statistics  Receives the statistics. Set its struct_size first; at least 8.
 *
 * @retval VGL_RESULT_SUCCESS           The statistics were written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, or struct_size is below 8.
 *
 * @par Thread safety
 * Context-synchronized.
 */
VGL_API vgl_result vgl_context_statistics_get(const vgl_context* context,
                                              vgl_context_statistics* out_statistics);

#ifdef __cplusplus
}
#endif

#endif /* VGL_CONTEXT_H */
