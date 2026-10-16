// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// JPEG and PNG files for imagery tiles (docs/pack-format.md, section 6.3), written and read
// through GDAL's drivers in memory.

#ifndef VGL_BAKER_IMAGE_CODEC_HPP
#define VGL_BAKER_IMAGE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vgl::bake
{

// An 8-bit image: channels interleaved per pixel, rows from the top. 1 channel is grey, 3 RGB
// and 4 RGBA, with straight alpha.
struct image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::vector<std::uint8_t> pixels;
};

// Encodes as a baseline JPEG file. The image must have 1 or 3 channels.
[[nodiscard]] std::vector<std::byte> encode_jpeg(const image& img, int quality);

// Encodes as a PNG file.
[[nodiscard]] std::vector<std::byte> encode_png(const image& img);

// Decodes a JPEG or PNG file. Throws std::runtime_error if GDAL cannot read it.
[[nodiscard]] image decode_image(std::span<const std::byte> file);

} // namespace vgl::bake

#endif // VGL_BAKER_IMAGE_CODEC_HPP
