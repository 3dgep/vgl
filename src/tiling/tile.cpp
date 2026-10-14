// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "tiling/tile.hpp"

#include <algorithm>
#include <cmath>

namespace vgl::internal
{

tile_id tile_at(double latitude, double longitude, std::uint32_t level) noexcept
{
    const double s = tile_span(level);

    // Wrap the longitude into [-180, 180), measured from the antimeridian.
    double lon = std::fmod(longitude + 180.0, 360.0);
    if (lon < 0.0)
    {
        lon += 360.0;
    }
    const auto columns = static_cast<double>(tile_columns(level));
    const double column = std::min(std::floor(lon / s), columns - 1.0);

    const double lat = std::clamp(latitude, -90.0, 90.0);
    const auto rows = static_cast<double>(tile_rows(level));
    const double row = std::min(std::floor((90.0 - lat) / s), rows - 1.0);

    return {level, static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(row)};
}

geographic_region tile_region(const tile_id& t) noexcept
{
    const degree_region b = tile_bounds(t);
    return {radians(b.west), radians(b.south), radians(b.east), radians(b.north)};
}

oriented_box tile_bounding_box(const ellipsoid& e,
                               const tile_id& t,
                               double min_height,
                               double max_height) noexcept
{
    return bound_region(e, tile_region(t), min_height, max_height);
}

} // namespace vgl::internal
