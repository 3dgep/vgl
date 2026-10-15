/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl defaults: a thread pool and a filesystem reader that implement the task and I/O
 * interfaces of vgl/foundation.h, so that the common case needs no callbacks (design
 * document, section 7.5). This module is optional and linked separately, as vgl_defaults:
 * unlike the core library, it creates threads and opens files.
 *
 * A minimal host:
 *
 *     vgl_thread_pool* pool;
 *     vgl_thread_pool_create(NULL, &pool);
 *     vgl_file_reader_create_info reader_info = {.struct_size = sizeof(reader_info)};
 *     reader_info.pool = pool;
 *     vgl_file_reader* reader;
 *     vgl_file_reader_create(&reader_info, &reader);
 *
 *     vgl_task_interface tasks;
 *     vgl_io_interface io;
 *     vgl_thread_pool_task_interface_get(pool, &tasks);
 *     vgl_file_reader_io_interface_get(reader, &io);
 *     vgl_context_create_info info = {.struct_size = sizeof(info)};
 *     info.tasks = &tasks;
 *     info.io = &io;
 *     ...
 *     vgl_context_destroy(context);
 *     vgl_file_reader_destroy(reader);
 *     vgl_thread_pool_destroy(pool);
 *
 * Destroy the context first, then the reader, then the pool.
 */
#ifndef VGL_DEFAULTS_H
#define VGL_DEFAULTS_H

#include <vgl/base.h>
#include <vgl/foundation.h>

/* Symbol visibility of the defaults library, which is shared when vgl is. */
#if defined(VGL_SHARED)
#    if defined(_WIN32)
#        if defined(VGL_DEFAULTS_BUILDING)
#            define VGL_DEFAULTS_API __declspec(dllexport)
#        else
#            define VGL_DEFAULTS_API __declspec(dllimport)
#        endif
#    else
#        define VGL_DEFAULTS_API __attribute__((visibility("default")))
#    endif
#else
#    define VGL_DEFAULTS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------- */
/* Thread pool                                                                              */
/* ---------------------------------------------------------------------------------------- */

/* A fixed set of worker threads that run tasks in the order they were submitted. */
typedef struct vgl_thread_pool vgl_thread_pool;

typedef struct vgl_thread_pool_create_info
{
    uint32_t struct_size;
    /* Number of threads. 0 means one less than the hardware threads, and at least one. */
    uint32_t thread_count;
    /* Optional. Allocator for the pool and its queue; must be thread-safe. */
    const vgl_allocator_interface* allocator;
} vgl_thread_pool_create_info;

/**
 * Creates a thread pool and starts its threads.
 *
 * In a build without exceptions, failing to start a thread ends the process, because the C++
 * runtime reports that only by an exception. With exceptions, the failure is returned.
 *
 * @param info      Optional. Null means all defaults.
 * @param out_pool  Receives the pool.
 *
 * @retval VGL_RESULT_SUCCESS            The pool was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT   out_pool is null, or info is invalid.
 * @retval VGL_RESULT_VERSION_MISMATCH   The allocator interface has an unknown version.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed, or a thread could not be started.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_thread_pool_create(const vgl_thread_pool_create_info* info,
                                                   vgl_thread_pool** out_pool);

/**
 * Runs the tasks still queued, then stops the threads and frees the pool. Every task the
 * pool accepted runs exactly once.
 *
 * @param pool  The pool, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Externally synchronized. Not from one of the pool's own tasks.
 */
VGL_DEFAULTS_API vgl_result vgl_thread_pool_destroy(vgl_thread_pool* pool);

/**
 * Gets a task interface that submits to the pool, for vgl_context_create_info.tasks. The
 * interface has no destroy callback: the pool must outlive every context that uses it.
 *
 * @param pool           The pool.
 * @param out_interface  Receives the interface.
 *
 * @retval VGL_RESULT_SUCCESS           The interface was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_thread_pool_task_interface_get(vgl_thread_pool* pool,
                                                               vgl_task_interface* out_interface);

/* ---------------------------------------------------------------------------------------- */
/* File reader                                                                              */
/* ---------------------------------------------------------------------------------------- */

/*
 * Reads files for the I/O interface. URIs are UTF-8 paths, absolute or relative to the
 * working directory, with an optional "file://" prefix; they are not percent-decoded. A file
 * that does not exist completes with VGL_RESULT_NOT_FOUND, other failures with
 * VGL_RESULT_IO_ERROR.
 */
typedef struct vgl_file_reader vgl_file_reader;

typedef struct vgl_file_reader_create_info
{
    uint32_t struct_size;
    uint32_t reserved;
    /*
     * Optional. Reads on the pool's threads. Null reads inside vgl_io_interface.read, on the
     * thread that calls into the context.
     */
    vgl_thread_pool* pool;
    /* Optional. Allocator for the reader and its reads; must be thread-safe with a pool. */
    const vgl_allocator_interface* allocator;
} vgl_file_reader_create_info;

/**
 * Creates a file reader.
 *
 * @param info        Optional. Null means all defaults: reading without a pool.
 * @param out_reader  Receives the reader.
 *
 * @retval VGL_RESULT_SUCCESS            The reader was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT   out_reader is null, or info is invalid.
 * @retval VGL_RESULT_VERSION_MISMATCH   The allocator interface has an unknown version.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_file_reader_create(const vgl_file_reader_create_info* info,
                                                   vgl_file_reader** out_reader);

/**
 * Waits for the reads in progress, then frees the reader. Its pool must still exist.
 *
 * @param reader  The reader, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Externally synchronized. Not from one of the pool's tasks.
 */
VGL_DEFAULTS_API vgl_result vgl_file_reader_destroy(vgl_file_reader* reader);

/**
 * Gets an I/O interface that reads through the reader, for vgl_context_create_info.io. The
 * interface has no destroy callback: the reader must outlive every context that uses it.
 *
 * @param reader         The reader.
 * @param out_interface  Receives the interface.
 *
 * @retval VGL_RESULT_SUCCESS           The interface was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_file_reader_io_interface_get(vgl_file_reader* reader,
                                                             vgl_io_interface* out_interface);

#ifdef __cplusplus
}
#endif

#endif /* VGL_DEFAULTS_H */
