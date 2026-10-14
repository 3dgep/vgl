// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "decoders/zstd_decompressor.hpp"

// ZSTD_createDCtx_advanced and ZSTD_customMem are in the advanced API, which Zstandard
// exports from both its static and shared libraries.
#define ZSTD_STATIC_LINKING_ONLY // NOLINT(readability-identifier-naming): Zstandard's name.
#include <zstd.h>
#include <zstd_errors.h>

#include <cstring>

namespace vgl::internal
{

namespace
{

// Zstandard frees without a size, but vgl allocators need it, so every block starts with a
// header that records its size.
constexpr std::size_t header_size = 16;
constexpr std::size_t block_alignment = 16;

void* zstd_allocate(void* opaque, std::size_t size)
{
    const auto* a = static_cast<const allocator*>(opaque);
    if (size > static_cast<std::size_t>(-1) - header_size)
    {
        return nullptr;
    }
    auto* block = static_cast<unsigned char*>(a->allocate(size + header_size, block_alignment));
    if (block == nullptr)
    {
        return nullptr;
    }
    std::memcpy(block, &size, sizeof(size));
    return block + header_size;
}

void zstd_free(void* opaque, void* address)
{
    if (address == nullptr)
    {
        return;
    }
    const auto* a = static_cast<const allocator*>(opaque);
    unsigned char* block = static_cast<unsigned char*>(address) - header_size;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof(size));
    a->deallocate(block, size + header_size, block_alignment);
}

} // namespace

zstd_decompressor::zstd_decompressor(const allocator& a) noexcept : allocator_(a)
{
    const ZSTD_customMem memory{&zstd_allocate, &zstd_free, &allocator_};
    context_ = ZSTD_createDCtx_advanced(memory);
}

zstd_decompressor::~zstd_decompressor()
{
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
}

vgl_result zstd_decompressor::decompress(std::span<const std::byte> frame,
                                         std::span<std::byte> out) noexcept
{
    if (context_ == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    // Exactly one frame, with no trailing bytes, whose recorded content size is the size the
    // format requires.
    const std::size_t frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(frame_size) != 0 || frame_size != frame.size())
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const unsigned long long content_size = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content_size != out.size())
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const std::size_t written = ZSTD_decompressDCtx(
        static_cast<ZSTD_DCtx*>(context_), out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(written) != 0)
    {
        return ZSTD_getErrorCode(written) == ZSTD_error_memory_allocation ? VGL_RESULT_OUT_OF_MEMORY
                                                                          : VGL_RESULT_CORRUPT_DATA;
    }
    return written == out.size() ? VGL_RESULT_SUCCESS : VGL_RESULT_CORRUPT_DATA;
}

} // namespace vgl::internal
