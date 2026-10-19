/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 */

#include <vgl/vgl.h>
#ifdef VGL_TEST_DEFAULTS
#    include <vgl/defaults.h>
#endif

#include <stdio.h>

int main(void)
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    vgl_context* context = NULL;
    if (vgl_version_get(&major, &minor, &patch) != VGL_RESULT_SUCCESS ||
        vgl_context_create(NULL, &context) != VGL_RESULT_SUCCESS)
    {
        return 1;
    }
    printf("vgl %u.%u.%u\n", (unsigned)major, (unsigned)minor, (unsigned)patch);
    (void)vgl_context_destroy(context);
#ifdef VGL_TEST_DEFAULTS
    {
        vgl_thread_pool* pool = NULL;
        if (vgl_thread_pool_create(NULL, &pool) != VGL_RESULT_SUCCESS)
        {
            return 1;
        }
        (void)vgl_thread_pool_destroy(pool);
    }
#endif
    return 0;
}
