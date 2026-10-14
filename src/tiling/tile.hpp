// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Geographic tiling scheme (docs/pack-format.md, section 2). The baker and the runtime share
// this code, so tile addressing, bounds and sample placement have a single source of truth.
//
// Bounds and sample positions are computed in degrees, where they are exact binary fractions,
// so that neighbouring tiles agree bit for bit on their shared edges.

#ifndef VGL_TILING_TILE_HPP
#define VGL_TILING_TILE_HPP

#include "geodesy/bounds.hpp"

#include <algorithm>
#include <cstdint>

namespace vgl::internal
{

inline constexpr std::uint32_t max_tile_level = 28;

struct tile_id
{
    std::uint32_t level = 0;
    std::uint32_t x = 0; // Column, counted eastwards from the antimeridian.
    std::uint32_t y = 0; // Row, counted southwards from the north pole.

    friend constexpr bool operator==(const tile_id&, const tile_id&) noexcept = default;
};

// Region in degrees, with west <= east and south <= north.
struct degree_region
{
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;

    friend constexpr bool operator==(const degree_region&, const degree_region&) noexcept = default;
};

struct degree_position
{
    double longitude = 0.0;
    double latitude = 0.0;

    friend constexpr bool operator==(const degree_position&,
                                     const degree_position&) noexcept = default;
};

[[nodiscard]] constexpr std::uint32_t tile_columns(std::uint32_t level) noexcept
{
    return 2u << level;
}

[[nodiscard]] constexpr std::uint32_t tile_rows(std::uint32_t level) noexcept
{
    return 1u << level;
}

[[nodiscard]] constexpr bool tile_is_valid(const tile_id& t) noexcept
{
    return t.level <= max_tile_level && t.x < tile_columns(t.level) && t.y < tile_rows(t.level);
}

// Width and height of a tile at a level: 180 / 2^level degrees.
[[nodiscard]] constexpr double tile_span(std::uint32_t level) noexcept
{
    return 180.0 / static_cast<double>(1u << level);
}

[[nodiscard]] constexpr degree_region tile_bounds(const tile_id& t) noexcept
{
    const double s = tile_span(t.level);
    return {-180.0 + t.x * s, 90.0 - (t.y + 1) * s, -180.0 + (t.x + 1) * s, 90.0 - t.y * s};
}

// Requires level > 0.
[[nodiscard]] constexpr tile_id tile_parent(const tile_id& t) noexcept
{
    return {t.level - 1, t.x >> 1, t.y >> 1};
}

// Requires level >= ancestor_level.
[[nodiscard]] constexpr tile_id tile_ancestor(const tile_id& t,
                                              std::uint32_t ancestor_level) noexcept
{
    const std::uint32_t shift = t.level - ancestor_level;
    return {ancestor_level, t.x >> shift, t.y >> shift};
}

// Child c = i + 2 j: 0 north-west, 1 north-east, 2 south-west, 3 south-east.
[[nodiscard]] constexpr tile_id tile_child(const tile_id& t, std::uint32_t c) noexcept
{
    return {t.level + 1, 2 * t.x + (c & 1u), 2 * t.y + (c >> 1)};
}

// Index of a tile among its parent's children. Requires level > 0.
[[nodiscard]] constexpr std::uint32_t tile_child_index(const tile_id& t) noexcept
{
    return (t.x & 1u) | ((t.y & 1u) << 1);
}

// Eastern and western neighbours wrap across the antimeridian.
[[nodiscard]] constexpr tile_id tile_east(const tile_id& t) noexcept
{
    return {t.level, (t.x + 1) % tile_columns(t.level), t.y};
}

[[nodiscard]] constexpr tile_id tile_west(const tile_id& t) noexcept
{
    return {t.level, (t.x + tile_columns(t.level) - 1) % tile_columns(t.level), t.y};
}

namespace detail
{

// Spreads the low 32 bits of v to the even bits of the result.
[[nodiscard]] constexpr std::uint64_t spread_bits(std::uint64_t v) noexcept
{
    v &= 0xFFFFFFFFull;
    v = (v | (v << 16)) & 0x0000FFFF0000FFFFull;
    v = (v | (v << 8)) & 0x00FF00FF00FF00FFull;
    v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0Full;
    v = (v | (v << 2)) & 0x3333333333333333ull;
    v = (v | (v << 1)) & 0x5555555555555555ull;
    return v;
}

// Inverse of spread_bits.
[[nodiscard]] constexpr std::uint32_t compact_bits(std::uint64_t v) noexcept
{
    v &= 0x5555555555555555ull;
    v = (v | (v >> 1)) & 0x3333333333333333ull;
    v = (v | (v >> 2)) & 0x0F0F0F0F0F0F0F0Full;
    v = (v | (v >> 4)) & 0x00FF00FF00FF00FFull;
    v = (v | (v >> 8)) & 0x0000FFFF0000FFFFull;
    v = (v | (v >> 16)) & 0x00000000FFFFFFFFull;
    return static_cast<std::uint32_t>(v);
}

} // namespace detail

// Bit i of x at bit 2i, bit i of y at bit 2i + 1.
[[nodiscard]] constexpr std::uint64_t morton(std::uint32_t x, std::uint32_t y) noexcept
{
    return detail::spread_bits(x) | (detail::spread_bits(y) << 1);
}

inline constexpr int tile_key_level_shift = 58;

// key = (level << 58) | morton(x, y) (docs/pack-format.md, section 2.2).
[[nodiscard]] constexpr std::uint64_t tile_key(const tile_id& t) noexcept
{
    return (static_cast<std::uint64_t>(t.level) << tile_key_level_shift) | morton(t.x, t.y);
}

// Decodes a key. The result is valid only if tile_is_valid returns true for it.
[[nodiscard]] constexpr tile_id tile_from_key(std::uint64_t key) noexcept
{
    const std::uint64_t m = key & ((1ull << tile_key_level_shift) - 1);
    return {static_cast<std::uint32_t>(key >> tile_key_level_shift),
            detail::compact_bits(m),
            detail::compact_bits(m >> 1)};
}

// True if a key decodes to a valid tile and is the canonical key of that tile.
[[nodiscard]] constexpr bool tile_key_is_valid(std::uint64_t key) noexcept
{
    const tile_id t = tile_from_key(key);
    return tile_is_valid(t) && tile_key(t) == key;
}

// True if size is 2^k + 1 with 2 <= k <= 10 (docs/pack-format.md, section 4.3).
[[nodiscard]] constexpr bool elevation_tile_size_is_valid(std::uint32_t size) noexcept
{
    const std::uint32_t n = size - 1;
    return size >= 5 && size <= 1025 && (n & (n - 1)) == 0;
}

// Position of elevation sample (column i, row j) in a tile of size samples per side
// (docs/pack-format.md, section 2.3). Requires a valid size and i, j < size.
[[nodiscard]] constexpr degree_position elevation_sample_position(const tile_id& t,
                                                                  std::uint32_t size,
                                                                  std::uint32_t i,
                                                                  std::uint32_t j) noexcept
{
    const degree_region b = tile_bounds(t);
    const double step = tile_span(t.level) / (size - 1);
    return {b.west + i * step, b.north - j * step};
}

// Centre of imagery pixel (column i, row j) of a tile with inner size pixels per side and a
// border of border pixels (docs/pack-format.md, section 2.4). i and j count the full image,
// including the border.
[[nodiscard]] constexpr degree_position imagery_pixel_center(const tile_id& t,
                                                             std::uint32_t size,
                                                             std::uint32_t border,
                                                             std::uint32_t i,
                                                             std::uint32_t j) noexcept
{
    const degree_region b = tile_bounds(t);
    const double step = tile_span(t.level) / size;
    return {b.west + (static_cast<double>(i) - border + 0.5) * step,
            b.north - (static_cast<double>(j) - border + 0.5) * step};
}

// Bilinear interpolation of a grid-registered tile of size samples per side at a position in
// degrees (docs/pack-format.md, section 6.4). sample(i, j) returns the value of column i and
// row j. The position should lie in the tile; it is clamped to the tile's edges, and a
// longitude on the other side of the antimeridian is wrapped. Because neighbouring tiles share
// their border samples, the result is continuous across tile edges.
template <typename SampleFunction>
[[nodiscard]] double tile_interpolate(const tile_id& t,
                                      std::uint32_t size,
                                      double latitude,
                                      double longitude,
                                      const SampleFunction& sample) noexcept
{
    const degree_region b = tile_bounds(t);
    const double scale = (size - 1) / tile_span(t.level);
    double d_lon = longitude - b.west;
    if (d_lon < -180.0)
    {
        d_lon += 360.0;
    }
    else if (d_lon > 180.0 + tile_span(t.level))
    {
        d_lon -= 360.0;
    }
    const double last = size - 1;
    const double u = std::clamp(d_lon * scale, 0.0, last);
    const double v = std::clamp((b.north - latitude) * scale, 0.0, last);

    // The cell's top-left sample; the last row and column use the cell before them.
    const auto i = static_cast<std::uint32_t>(u < last ? u : last - 1.0);
    const auto j = static_cast<std::uint32_t>(v < last ? v : last - 1.0);
    const double fu = u - i;
    const double fv = v - j;

    const double top = sample(i, j) * (1.0 - fu) + sample(i + 1, j) * fu;
    const double bottom = sample(i, j + 1) * (1.0 - fu) + sample(i + 1, j + 1) * fu;
    return top * (1.0 - fv) + bottom * fv;
}

// Tile at a level that contains a position in degrees. Longitudes wrap, so 180 belongs to
// column 0 like -180. Each tile owns its west and north edges, and the south pole belongs to
// the last row. The latitude is clamped to [-90, 90]. Requires level <= max_tile_level and
// finite inputs.
[[nodiscard]] tile_id tile_at(double latitude, double longitude, std::uint32_t level) noexcept;

// Tile bounds in radians, for the geodesy module.
[[nodiscard]] geographic_region tile_region(const tile_id& t) noexcept;

// Bounding box of a tile with a height range, in ECEF.
[[nodiscard]] oriented_box tile_bounding_box(const ellipsoid& e,
                                             const tile_id& t,
                                             double min_height,
                                             double max_height) noexcept;

// Shard that holds a tile (docs/pack-format.md, section 5.1): the ancestor at shard_level,
// or the base shard for a tile above the shard level.
struct shard_location
{
    bool in_base = false;
    tile_id root; // Valid when in_base is false.

    friend constexpr bool operator==(const shard_location&,
                                     const shard_location&) noexcept = default;
};

[[nodiscard]] constexpr shard_location tile_shard(const tile_id& t,
                                                  std::uint32_t shard_level) noexcept
{
    if (t.level < shard_level)
    {
        return {true, {}};
    }
    return {false, tile_ancestor(t, shard_level)};
}

// True if a tile lies in the subtree rooted at root, including root itself.
[[nodiscard]] constexpr bool tile_is_in_subtree(const tile_id& t, const tile_id& root) noexcept
{
    return t.level >= root.level && tile_ancestor(t, root.level) == root;
}

} // namespace vgl::internal

#endif // VGL_TILING_TILE_HPP
