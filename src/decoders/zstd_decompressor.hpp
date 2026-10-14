// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Zstandard decompression with every allocation routed through a vgl allocator (NFR-03).

#ifndef VGL_DECODERS_ZSTD_DECOMPRESSOR_HPP
#define VGL_DECODERS_ZSTD_DECOMPRESSOR_HPP

#include "foundation/allocator.hpp"

#include <cstddef>
#include <span>

namespace vgl::internal
{

class zstd_decompressor
{
public:
    // The decompressor keeps a copy of the allocator, whose address Zstandard holds on to, so
    // it cannot be moved or copied.
    explicit zstd_decompressor(const allocator& a) noexcept;
    zstd_decompressor(const zstd_decompressor&) = delete;
    zstd_decompressor& operator=(const zstd_decompressor&) = delete;
    zstd_decompressor(zstd_decompressor&&) = delete;
    zstd_decompressor& operator=(zstd_decompressor&&) = delete;
    ~zstd_decompressor();

    // True if the decompression context could be created.
    [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

    // Decompresses frame, which must be exactly one Zstandard frame with the content size
    // present, and whose content is exactly out.size() bytes (docs/pack-format.md, section 3).
    // Returns VGL_RESULT_CORRUPT_DATA otherwise, and VGL_RESULT_OUT_OF_MEMORY if no context
    // could be created.
    [[nodiscard]] vgl_result decompress(std::span<const std::byte> frame,
                                        std::span<std::byte> out) noexcept;

private:
    allocator allocator_;
    void* context_ = nullptr; // ZSTD_DCtx
};

} // namespace vgl::internal

#endif // VGL_DECODERS_ZSTD_DECOMPRESSOR_HPP
