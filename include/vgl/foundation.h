/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl foundation: allocator and logging interfaces (docs/c-api.md, sections 10 and 13).
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
 * Must be thread-safe if a task callback is installed.
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

#ifdef __cplusplus
}
#endif

#endif /* VGL_FOUNDATION_H */
