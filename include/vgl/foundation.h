/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl foundation: allocator, logging, I/O and task interfaces (docs/c-api.md, sections 10,
 * 12 and 13; design document, section 7.5).
 */
#ifndef VGL_FOUNDATION_H
#define VGL_FOUNDATION_H

#include <vgl/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Severity of a log message, and the minimum level a context reports. */
typedef enum vgl_log_level
{
    /* As a minimum level, selects VGL_LOG_LEVEL_WARNING. Never the level of a message. */
    VGL_LOG_LEVEL_DEFAULT = 0,
    VGL_LOG_LEVEL_ERROR = 1,
    VGL_LOG_LEVEL_WARNING = 2,
    VGL_LOG_LEVEL_INFO = 3,
    VGL_LOG_LEVEL_DEBUG = 4,
    VGL_LOG_LEVEL_MAX_ENUM = 0x7FFFFFFF
} vgl_log_level;

/**
 * Receives a log message.
 *
 * @param user_data  The pointer registered with the callback.
 * @param level      Severity of the message.
 * @param message    UTF-8 text without a trailing newline, valid only during the call.
 *
 * @par Thread safety
 * Any thread. Must be thread-safe if a task callback is installed.
 */
typedef void (*vgl_log_fn)(void* user_data, vgl_log_level level, const char* message);

#define VGL_ALLOCATOR_INTERFACE_VERSION 1

/*
 * Host allocator. Every allocation made on behalf of a context goes through it (NFR-03).
 * Must be thread-safe if a task callback is installed, or if the host completes I/O requests
 * on threads other than the one that calls into the context.
 */
typedef struct vgl_allocator_interface
{
    uint32_t version; /* VGL_ALLOCATOR_INTERFACE_VERSION */
    uint32_t reserved;
    void* user_data;

    /*
     * Allocates size bytes aligned to alignment, a power of two. Returns null on failure.
     * size is never 0.
     */
    void* (*allocate)(void* user_data, size_t size, size_t alignment);

    /*
     * Frees memory returned by allocate, with the same size and alignment. memory is never
     * null.
     */
    void (*deallocate)(void* user_data, void* memory, size_t size, size_t alignment);
} vgl_allocator_interface;

/* ---------------------------------------------------------------------------------------- */
/* I/O                                                                                      */
/* ---------------------------------------------------------------------------------------- */

/* A read the library has asked the host to perform. */
typedef struct vgl_io_request vgl_io_request;

/* Size meaning "to the end of the file". */
#define VGL_IO_SIZE_TO_END UINT64_MAX

/* What to read. */
typedef struct vgl_io_read
{
    /* Location of the file, as the host gave it to the library plus a relative path, joined
     * with '/'. UTF-8 and null-terminated; valid until the request is completed. */
    const char* uri;
    uint64_t offset; /* First byte to read. */
    /* Number of bytes to read, or VGL_IO_SIZE_TO_END. The host delivers fewer bytes only
     * where the file ends. */
    uint64_t size;
} vgl_io_read;

#define VGL_IO_INTERFACE_VERSION 1

/*
 * Host I/O (design document, section 7.5). The library never opens files or connections
 * itself: it asks the host to read byte ranges, and the host completes each request with
 * vgl_io_request_complete, from any thread, immediately or later.
 */
typedef struct vgl_io_interface
{
    uint32_t version; /* VGL_IO_INTERFACE_VERSION */
    uint32_t reserved;
    void* user_data;

    /*
     * Starts a read. The host completes the request exactly once, also when it cannot start
     * the read. read describes the work and is valid until the request is completed.
     *
     * Thread safety: called on the thread that calls into the context.
     */
    void (*read)(void* user_data, vgl_io_request* request, const vgl_io_read* read);

    /*
     * Optional. Asks the host to give up a read the library no longer needs. The host must
     * still complete the request, normally or with VGL_RESULT_CANCELLED.
     *
     * Thread safety: called on the thread that calls into the context.
     */
    void (*cancel)(void* user_data, vgl_io_request* request);

    /*
     * Optional. Called once, while the context is destroyed. Requests may still be
     * outstanding; the host must complete them, now or later (docs/c-api.md, section 12).
     */
    void (*destroy)(void* user_data);
} vgl_io_interface;

/**
 * Completes an I/O request.
 *
 * The library copies data before the function returns, so the host may free it afterwards.
 * The request handle is invalid once the function returns.
 *
 * @param request  The request, from vgl_io_interface.read, or one made with
 *                 vgl_io_request_create, whose callback then receives the result.
 * @param result   VGL_RESULT_SUCCESS if data holds what was read; otherwise the reason it
 *                 could not be read, such as VGL_RESULT_NOT_FOUND, VGL_RESULT_IO_ERROR or
 *                 VGL_RESULT_CANCELLED.
 * @param data     The bytes read, or null if result is not VGL_RESULT_SUCCESS.
 * @param size     Number of bytes in data.
 *
 * @retval VGL_RESULT_SUCCESS           The completion was queued.
 * @retval VGL_RESULT_OUT_OF_MEMORY     The data could not be copied; the request fails.
 * @retval VGL_RESULT_INVALID_ARGUMENT  request is null, or data is null with a non-zero size.
 *
 * @par Thread safety
 * Free-threaded. May be called from inside vgl_io_interface.read.
 */
VGL_API vgl_result vgl_io_request_complete(vgl_io_request* request,
                                           vgl_result result,
                                           const void* data,
                                           size_t size);

/*
 * Receives the completion of a request made with vgl_io_request_create: the result, and on
 * success the bytes read, valid only during the call.
 *
 * Thread safety: called on the thread that completes the request, inside
 * vgl_io_request_complete.
 */
typedef void (*vgl_io_complete_fn)(void* user_data,
                                   vgl_result result,
                                   const void* data,
                                   size_t size);

typedef struct vgl_io_request_create_info
{
    uint32_t struct_size;
    uint32_t reserved;
    /* Receives the completion. Required. */
    vgl_io_complete_fn complete;
    VGL_POINTER_PADDING(complete_padding)
    void* user_data;
    VGL_POINTER_PADDING(user_data_padding)
    /* Optional. Allocator for the request; must be thread-safe if the request completes on
     * another thread. */
    const vgl_allocator_interface* allocator;
    VGL_POINTER_PADDING(allocator_padding)
} vgl_io_request_create_info;

/**
 * Creates a request whose completion goes to a callback rather than to a context, so that an
 * I/O interface can pass a read on to another one and handle what it delivers, as a cache or
 * any other wrapper does (design document, section 7.5). The request goes to another
 * interface's read with a vgl_io_read the creator keeps valid until completion, and is
 * completed exactly once with vgl_io_request_complete, which calls complete and frees the
 * request. A request that never reaches a read is freed by completing it too.
 *
 * @param info         The callback and its user data, and the allocator.
 * @param out_request  Receives the request.
 *
 * @retval VGL_RESULT_SUCCESS            The request was created.
 * @retval VGL_RESULT_INVALID_ARGUMENT   A pointer is null, or info has no complete callback or
 *                                       an invalid allocator.
 * @retval VGL_RESULT_NOT_SUPPORTED      info has fields from a newer version set.
 * @retval VGL_RESULT_VERSION_MISMATCH   The allocator interface has an unknown version.
 * @retval VGL_RESULT_OUT_OF_MEMORY      Allocation failed.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_io_request_create(const vgl_io_request_create_info* info,
                                         vgl_io_request** out_request);

/* ---------------------------------------------------------------------------------------- */
/* Tasks                                                                                    */
/* ---------------------------------------------------------------------------------------- */

/* A unit of work: run function(task_data) once. */
typedef void (*vgl_task_fn)(void* task_data);

#define VGL_TASK_INTERFACE_VERSION 1

/*
 * Host task scheduling (design document, section 7.5). The library submits decoding work,
 * which the host runs on any of its threads. Without a task interface, vgl_context_update
 * does that work itself, within its time budget.
 */
typedef struct vgl_task_interface
{
    uint32_t version; /* VGL_TASK_INTERFACE_VERSION */
    uint32_t reserved;
    void* user_data;

    /*
     * Runs function(task_data) exactly once, on any thread, now or later. Returns
     * VGL_RESULT_SUCCESS if the task was accepted; for any other result the library does the
     * work itself, and function is never called. Tasks never block on each other or on the
     * host.
     *
     * Thread safety: called on the thread that calls into the context.
     */
    vgl_result (*submit)(void* user_data, vgl_task_fn function, void* task_data);

    /*
     * Optional. Called once, while the context is destroyed. Tasks may still be queued; the
     * host must run them, now or later, and they return without doing work.
     */
    void (*destroy)(void* user_data);
} vgl_task_interface;

#ifdef __cplusplus
}
#endif

#endif /* VGL_FOUNDATION_H */
