// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The cloud image that every backend's atmosphere draws as a shell above the ground (design
// document, section 7.6): the host's coverage of the whole globe, one byte per pixel, in
// latitude and longitude, and its mip levels, built once on the CPU so that every backend
// uploads the same chain.

#ifndef VGL_BACKENDS_CLOUDS_HPP
#define VGL_BACKENDS_CLOUDS_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace vgl::backends
{

// Limits of the cloud image, which every backend's textures take.
inline constexpr std::uint32_t max_cloud_width = 16384;
inline constexpr std::uint32_t max_cloud_height = 8192;

// The cloud shell's height when the host gives none, in metres: low and middle clouds.
inline constexpr double default_cloud_height = 6000.0;

// The width of a cloud image's pixels at the equator, in kilometres, on the WGS84 ellipsoid.
[[nodiscard]] inline double cloud_texel_km(std::uint32_t width)
{
    return 2.0 * std::numbers::pi * 6378.137 / width;
}

// The mip chain of a cloud image: each level's size and its place in one buffer.
struct cloud_levels
{
    std::uint32_t count = 0;
    std::array<std::uint32_t, 16> width{};
    std::array<std::uint32_t, 16> height{};
    std::array<std::size_t, 16> offset{};
    std::size_t bytes = 0;
};

[[nodiscard]] inline bool cloud_size_is_valid(std::uint32_t width, std::uint32_t height)
{
    return width >= 2 && height >= 2 && width <= max_cloud_width && height <= max_cloud_height;
}

// The levels down to 1 x 1, each half the one before, rounded down but at least 1.
[[nodiscard]] inline cloud_levels cloud_levels_of(std::uint32_t width, std::uint32_t height)
{
    cloud_levels l;
    std::uint32_t w = width;
    std::uint32_t h = height;
    for (;;)
    {
        l.width[l.count] = w;
        l.height[l.count] = h;
        l.offset[l.count] = l.bytes;
        l.bytes += std::size_t{w} * h;
        ++l.count;
        if ((w == 1 && h == 1) || l.count == l.width.size())
        {
            break;
        }
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    return l;
}

// Fills out (cloud_levels_of's bytes long) with the image, rows from the north, and each level
// below it, as the average of up to four pixels of the level above.
inline void cloud_levels_build(const std::uint8_t* image, const cloud_levels& l, std::uint8_t* out)
{
    std::copy_n(image, std::size_t{l.width[0]} * l.height[0], out);
    for (std::uint32_t k = 1; k < l.count; ++k)
    {
        const std::uint8_t* above = out + l.offset[k - 1];
        const std::uint32_t aw = l.width[k - 1];
        const std::uint32_t ah = l.height[k - 1];
        std::uint8_t* level = out + l.offset[k];
        for (std::uint32_t j = 0; j < l.height[k]; ++j)
        {
            for (std::uint32_t i = 0; i < l.width[k]; ++i)
            {
                const std::uint32_t i0 = std::min(2 * i, aw - 1);
                const std::uint32_t i1 = std::min(2 * i + 1, aw - 1);
                const std::uint32_t j0 = std::min(2 * j, ah - 1);
                const std::uint32_t j1 = std::min(2 * j + 1, ah - 1);
                const unsigned sum = unsigned{above[std::size_t{j0} * aw + i0]} +
                                     unsigned{above[std::size_t{j0} * aw + i1]} +
                                     unsigned{above[std::size_t{j1} * aw + i0]} +
                                     unsigned{above[std::size_t{j1} * aw + i1]};
                level[std::size_t{j} * l.width[k] + i] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
}

} // namespace vgl::backends

#endif // VGL_BACKENDS_CLOUDS_HPP
