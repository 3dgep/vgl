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
 * column-major and multiply column vectors (vgl_dmat4); hosts that use row vectors pass their
 * matrices transposed. Any handedness and any depth mapping work: OpenGL or Direct3D clip
 * space, reversed or infinite projections.
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

#ifdef __cplusplus
}
#endif

#endif /* VGL_CONTEXT_H */
