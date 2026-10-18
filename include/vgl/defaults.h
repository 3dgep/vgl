/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl defaults: a thread pool, a filesystem reader, an HTTP reader and a disk cache that
 * implement the task and I/O interfaces of vgl/foundation.h, so that the common case needs no
 * callbacks (design document, section 7.5). This module is optional and linked separately, as
 * vgl_defaults: unlike the core library, it creates threads, opens files and connects to servers.
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

/** A fixed set of worker threads that run tasks in the order they were submitted. */
typedef struct vgl_thread_pool vgl_thread_pool;

typedef struct vgl_thread_pool_create_info
{
    uint32_t struct_size;
    /** Number of threads. 0 means one less than the hardware threads, and at least one. */
    uint32_t thread_count;
    /** Optional. Allocator for the pool and its queue; must be thread-safe. */
    const vgl_allocator_interface* allocator;
    VGL_POINTER_PADDING(allocator_padding)
} vgl_thread_pool_create_info;

/**
 * Creates a thread pool and starts its threads.
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

/**
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
    /**
     * Optional. Reads on the pool's threads. Null reads inside vgl_io_interface.read, on the
     * thread that calls into the context.
     */
    vgl_thread_pool* pool;
    VGL_POINTER_PADDING(pool_padding)
    /** Optional. Allocator for the reader and its reads; must be thread-safe with a pool. */
    const vgl_allocator_interface* allocator;
    VGL_POINTER_PADDING(allocator_padding)
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

/* ---------------------------------------------------------------------------------------- */
/* HTTP reader                                                                              */
/* ---------------------------------------------------------------------------------------- */

/**
 * Reads packs from a web server for the I/O interface, with HTTP range requests (design
 * document, section 8.6), so that any server, CDN or object store that serves range requests
 * can hold packs. It is built on libcurl, when vgl is built with VGL_DEFAULTS_HTTP; without it,
 * vgl_http_reader_create reports VGL_RESULT_NOT_SUPPORTED. URIs starting with "http://" or
 * "https://" are read over HTTP, others through the fallback interface the create info names.
 *
 * Reads run on one thread of the reader's own, which overlaps them over a few connections per
 * server, and complete from it. A missing file (404 or 410) completes with
 * VGL_RESULT_NOT_FOUND, a range past the end of the file (416) with no bytes, as the file
 * reader does, and other failures, timeouts among them, with VGL_RESULT_IO_ERROR. A server that
 * ignores range requests sends the whole file, of which the reader keeps the range.
 *
 * libcurl's own allocations do not go through the allocator, which serves the reader and the
 * data it delivers.
 *
 * On Emscripten, the reader is built on the browser's fetch instead, with VGL_DEFAULTS_HTTP and no
 * library, and needs no thread: fetches start and finish on the main thread, between the page's
 * frames, and reads that arrive on other threads are passed to it. Reads that fail without an
 * answer are tried again, a few times. Without a fallback, URIs that are not HTTP are fetched
 * too, relative to the page's address. The browser chooses the connections and the User-Agent
 * header, and allocates the data delivered.
 */
typedef struct vgl_http_reader vgl_http_reader;

typedef struct vgl_http_reader_create_info
{
    uint32_t struct_size;
    /** Connections to each server at most. 0 selects 8. Ignored in a browser. */
    uint32_t max_connection_count;
    /** Optional. Allocator for the reader and its reads; must be thread-safe. */
    const vgl_allocator_interface* allocator;
    VGL_POINTER_PADDING(allocator_padding)
    /** Optional. Reads the URIs that are not HTTP, such as a file reader's interface
     * (vgl_file_reader_io_interface_get) or a host's own; without it, they complete with
     * VGL_RESULT_NOT_FOUND, or in a browser are fetched. The table is copied; the object
     * behind its user_data must outlive the reader. */
    const vgl_io_interface* fallback;
    VGL_POINTER_PADDING(fallback_padding)
    /** Seconds a read may take before it fails with VGL_RESULT_IO_ERROR. 0 selects 30. */
    double timeout;
    /** Optional. The User-Agent header, copied; null selects "vgl/<version>". Ignored in a
     * browser. */
    const char* user_agent;
    VGL_POINTER_PADDING(user_agent_padding)
} vgl_http_reader_create_info;

/**
 * Creates an HTTP reader and starts its thread.
 *
 * @param info        Optional. Null means all defaults.
 * @param out_reader  Receives the reader.
 *
 * @retval VGL_RESULT_SUCCESS            The reader was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT   out_reader is null, or info is invalid: the timeout is
 *                                       negative or not finite, or the fallback has no read
 *                                       callback or an unknown version.
 * @retval VGL_RESULT_NOT_SUPPORTED      vgl was built without VGL_DEFAULTS_HTTP, or info has
 *                                       fields from a newer version set.
 * @retval VGL_RESULT_VERSION_MISMATCH   The allocator interface has an unknown version.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed.
 * @retval VGL_RESULT_IO_ERROR           libcurl could not be set up.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_http_reader_create(const vgl_http_reader_create_info* info,
                                                   vgl_http_reader** out_reader);

/**
 * Cancels the reads in progress, which complete with VGL_RESULT_CANCELLED, stops the reader's
 * thread and frees the reader.
 *
 * @param reader  The reader, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Externally synchronized.
 */
VGL_DEFAULTS_API vgl_result vgl_http_reader_destroy(vgl_http_reader* reader);

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
VGL_DEFAULTS_API vgl_result vgl_http_reader_io_interface_get(vgl_http_reader* reader,
                                                             vgl_io_interface* out_interface);

/* ---------------------------------------------------------------------------------------- */
/* Disk cache                                                                               */
/* ---------------------------------------------------------------------------------------- */

/**
 * Keeps what another I/O interface reads from web servers in a directory, so that areas seen
 * once load again without the network, and offline (design document, section 8.6). It wraps
 * the interface: put it between a context and an HTTP reader, or a host's own network layer.
 * Only "http://" and "https://" URIs are cached; others go straight to the wrapped interface.
 *
 * Each read is an entry, by its URI and byte range. Shards are kept as they are, since their
 * names hold a digest of their content (pack format, section 5.2): a cached shard is never
 * stale, and is read from the disk. A manifest (pack.vglm) changes in place when a pack is
 * rebaked, so it is always read from the server, and from the disk only when the server
 * cannot be reached. When the entries exceed the budget, the least recently used go.
 *
 * Cancelling a read completes it with VGL_RESULT_CANCELLED at once; a download in progress
 * goes on and is stored. Entries are written to a temporary name and renamed, so a cache
 * left by a crash holds only whole entries, and one that does not match its key or checksum
 * is read again. One process at a time may use a directory.
 */
typedef struct vgl_disk_cache vgl_disk_cache;

typedef struct vgl_disk_cache_create_info
{
    uint32_t struct_size;
    uint32_t reserved;
    /** The directory, UTF-8; created if missing. Required. */
    const char* directory;
    VGL_POINTER_PADDING(directory_padding)
    /** The interface whose reads are cached. Required. The table is copied; the object behind
     * its user_data must stay valid until the cache is destroyed and its reads have
     * completed. With a pool, the cache may call its read from the pool's threads (when an
     * entry on the disk cannot be read), so it must accept reads from any thread. */
    const vgl_io_interface* source;
    VGL_POINTER_PADDING(source_padding)
    /** Bytes the entries may take on disk. 0 selects 1 GiB. */
    uint64_t budget;
    /** Optional. Reads and writes the disk on the pool's threads. Null does it in
     * vgl_io_interface.read and on the thread that completes the source's reads. */
    vgl_thread_pool* pool;
    VGL_POINTER_PADDING(pool_padding)
    /** Optional. Allocator for the cache and its reads; must be thread-safe. */
    const vgl_allocator_interface* allocator;
    VGL_POINTER_PADDING(allocator_padding)
} vgl_disk_cache_create_info;

/** What the cache has done since it was created, and what it holds. */
typedef struct vgl_disk_cache_statistics
{
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t hits;         /**< Reads served from the disk. */
    uint64_t misses;       /**< Reads of shards that went to the source. */
    uint64_t offline_hits; /**< Manifests served from the disk because the source failed. */
    uint64_t entries;      /**< Entries on disk. */
    uint64_t bytes;        /**< Bytes the entries take on disk. */
    uint64_t budget;       /**< Bytes they may take. */
    uint64_t evictions;    /**< Entries removed to keep to the budget. */
} vgl_disk_cache_statistics;

/**
 * Creates a disk cache, reading what the directory already holds.
 *
 * @param info       The directory, the source and the budget.
 * @param out_cache  Receives the cache.
 *
 * @retval VGL_RESULT_SUCCESS            The cache was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT   A pointer is null, or info has no directory or source,
 *                                       or the source has no read callback.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_VERSION_MISMATCH   The source or allocator interface has an unknown
 *                                       version.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed.
 * @retval VGL_RESULT_IO_ERROR           The directory could not be created or read.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_disk_cache_create(const vgl_disk_cache_create_info* info,
                                                  vgl_disk_cache** out_cache);

/**
 * Destroys the cache, after the disk work in progress, so that what was read is stored. Reads
 * of the source that are still in progress complete later, without storing what they read;
 * the source and the pool must outlive them.
 *
 * @param cache  The cache, or null.
 *
 * @retval VGL_RESULT_SUCCESS  Always.
 *
 * @par Thread safety
 * Externally synchronized.
 */
VGL_DEFAULTS_API vgl_result vgl_disk_cache_destroy(vgl_disk_cache* cache);

/**
 * Gets an I/O interface that reads through the cache, for vgl_context_create_info.io. The
 * interface has no destroy callback: the cache must outlive every context that uses it.
 *
 * @param cache          The cache.
 * @param out_interface  Receives the interface.
 *
 * @retval VGL_RESULT_SUCCESS           The interface was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_disk_cache_io_interface_get(vgl_disk_cache* cache,
                                                            vgl_io_interface* out_interface);

/**
 * Gets the cache's statistics.
 *
 * @param cache           The cache.
 * @param out_statistics  Receives them, an output structure (docs/c-api.md, section 8.4): set
 *                        its struct_size, at least 8.
 *
 * @retval VGL_RESULT_SUCCESS           The statistics were written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, or struct_size is below 8.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result
vgl_disk_cache_statistics_get(vgl_disk_cache* cache, vgl_disk_cache_statistics* out_statistics);

/**
 * Removes every entry. Reads in progress complete, and may store what they read.
 *
 * @param cache  The cache.
 *
 * @retval VGL_RESULT_SUCCESS           The entries were removed.
 * @retval VGL_RESULT_INVALID_ARGUMENT  cache is null.
 * @retval VGL_RESULT_OUT_OF_MEMORY     Allocation failed; nothing was removed.
 * @retval VGL_RESULT_IO_ERROR          Some entries could not be removed; the others were.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_DEFAULTS_API vgl_result vgl_disk_cache_clear(vgl_disk_cache* cache);

#ifdef __cplusplus
}
#endif

#endif /* VGL_DEFAULTS_H */
