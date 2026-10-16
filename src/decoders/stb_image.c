/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * The stb_image implementation, for JPEG and PNG imagery tiles (docs/pack-format.md, section
 * 6.3). It is compiled as C, apart from vgl's own code and its warning flags. Every allocation
 * goes to the allocator of the decode in progress (src/decoders/image_decoder.cpp), so none
 * reaches the C heap (NFR-03). Failure strings are off: they are a global that stb_image
 * writes, and vgl reports failures through result codes.
 */
#include <stddef.h>

void* vgl_stbi_allocate(size_t size);
void* vgl_stbi_reallocate(void* memory, size_t old_size, size_t new_size);
void vgl_stbi_deallocate(void* memory);

/* stb_image's configuration, in its names. */
/* NOLINTBEGIN(readability-identifier-naming) */
#define STBI_MALLOC(size) vgl_stbi_allocate(size)
#define STBI_REALLOC_SIZED(memory, old_size, new_size)                                             \
    vgl_stbi_reallocate(memory, old_size, new_size)
#define STBI_FREE(memory) vgl_stbi_deallocate(memory)

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
/* NOLINTEND(readability-identifier-naming) */
#include <stb_image.h>

/* The two entry points vgl uses, with external linkage (stb_image's own are static). */
int vgl_stbi_info(const unsigned char* data, int size, int* width, int* height, int* channels)
{
    return stbi_info_from_memory(data, size, width, height, channels);
}

unsigned char* vgl_stbi_load(const unsigned char* data, int size, int* width, int* height)
{
    int channels = 0;
    return stbi_load_from_memory(data, size, width, height, &channels, 4);
}

void vgl_stbi_free(void* pixels)
{
    stbi_image_free(pixels);
}
