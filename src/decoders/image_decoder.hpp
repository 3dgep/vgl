// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Imagery tile decoding (docs/pack-format.md, section 6.3): JPEG and PNG files to RGBA8, with
// a chain of mip levels for the render interface (docs/render-interface.md, section 7).

#ifndef VGL_DECODERS_IMAGE_DECODER_HPP
#define VGL_DECODERS_IMAGE_DECODER_HPP

#include "foundation/allocator.hpp"
#include "foundation/buffer.hpp"

#include <vgl/base.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vgl::internal
{

// The most mip levels an image can have: a 65,536-pixel side.
inline constexpr std::uint32_t max_mip_levels = 17;

// An RGBA8 image with its mip levels, largest first, one after another in one buffer. Rows
// run from the top, 4 bytes per pixel, without padding.
struct mip_chain
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    buffer<std::uint8_t> pixels;
    std::array<std::size_t, max_mip_levels> offsets = {};
    std::array<std::size_t, max_mip_levels> sizes = {};
};

// The number of levels of a full mip chain: down to one pixel.
[[nodiscard]] std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept;

// Decodes a JPEG or PNG file of side x side pixels into a full mip chain. Returns
// VGL_RESULT_CORRUPT_DATA if the file does not decode or has another size,
// VGL_RESULT_NOT_SUPPORTED if it is neither JPEG nor PNG, and VGL_RESULT_OUT_OF_MEMORY.
// Every allocation, the decoder's too, goes through a.
[[nodiscard]] vgl_result decode_imagery(const allocator& a,
                                        std::span<const std::byte> file,
                                        std::uint32_t side,
                                        mip_chain& out) noexcept;

// Fills levels 1 and up of a chain whose level 0 is set: each pixel of a level is the box
// filter of up to 2 x 2 pixels of the level above. Colour is weighted by alpha, so that
// pixels without data (alpha 0) do not darken their neighbours.
void generate_mips(mip_chain& chain) noexcept;

} // namespace vgl::internal

#endif // VGL_DECODERS_IMAGE_DECODER_HPP
