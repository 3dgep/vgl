// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "geodesy/bounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vgl::internal
{

bool oriented_box::contains(const dvec3& p, double tolerance) const noexcept
{
    const dvec3 d = p - center;
    return std::fabs(dot(d, axes[0])) <= half_extents.x + tolerance &&
           std::fabs(dot(d, axes[1])) <= half_extents.y + tolerance &&
           std::fabs(dot(d, axes[2])) <= half_extents.z + tolerance;
}

bool bounding_sphere::contains(const dvec3& p, double tolerance) const noexcept
{
    return length(p - center) <= radius + tolerance;
}

// Method: a box axis coordinate f(lat, lon, h) = axis . P(lat, lon, h) is linear in h, so its
// extremes over the height range lie at min_height or max_height. Over (lat, lon) it is
// smooth, and it is sampled on a grid that includes the region's edges and corners. At the
// true maximum, the derivative along the region vanishes (in both directions inside the
// region, or along the edge on an edge), so the nearest sample is lower by at most
// K d^2 / 2, where d <= spacing / sqrt(2) is the distance to it and K bounds the second
// derivatives of f. The same holds for the minimum. Enlarging the sampled extremes by
// K spacing^2 / 4 therefore gives a box that contains the region.
//
// K: the second partial derivatives of P with respect to latitude and longitude are bounded
// by the largest radius of curvature plus the height, times a factor that grows with the
// eccentricity. K = 4 (a / (1 - f)^6 + |h|max) bounds them for every flattening in [0, 0.5),
// with a wide margin for the Earth.
oriented_box bound_region(const ellipsoid& e,
                          const geographic_region& region,
                          double min_height,
                          double max_height) noexcept
{
    // Sample spacing: at most 1 degree, and at least 8 intervals per side.
    constexpr double max_spacing = pi / 180.0;
    constexpr int min_intervals = 8;
    const double lat_span = region.north - region.south;
    const double lon_span = region.east - region.west;
    const int lat_intervals =
        std::max(min_intervals, static_cast<int>(std::ceil(lat_span / max_spacing)));
    const int lon_intervals =
        std::max(min_intervals, static_cast<int>(std::ceil(lon_span / max_spacing)));
    const double lat_step = lat_span / lat_intervals;
    const double lon_step = lon_span / lon_intervals;

    const geodetic center_geodetic{
        (region.south + region.north) / 2.0, (region.west + region.east) / 2.0, 0.0};
    const dmat4 frame = e.enu_to_ecef(center_geodetic);
    const std::array<dvec3, 3> axes = {dvec3{frame(0, 0), frame(1, 0), frame(2, 0)},
                                       dvec3{frame(0, 1), frame(1, 1), frame(2, 1)},
                                       dvec3{frame(0, 2), frame(1, 2), frame(2, 2)}};
    const dvec3 origin{frame(0, 3), frame(1, 3), frame(2, 3)};

    dvec3 lo{HUGE_VAL, HUGE_VAL, HUGE_VAL};
    dvec3 hi{-HUGE_VAL, -HUGE_VAL, -HUGE_VAL};
    const std::array<double, 2> heights = {min_height, max_height};
    for (int i = 0; i <= lat_intervals; ++i)
    {
        // Use the exact edge for the last sample, so rounding cannot shrink the region.
        const double lat = i == lat_intervals ? region.north : region.south + i * lat_step;
        for (int j = 0; j <= lon_intervals; ++j)
        {
            const double lon = j == lon_intervals ? region.east : region.west + j * lon_step;
            for (const double h : heights)
            {
                const dvec3 d = e.to_ecef({lat, lon, h}) - origin;
                const dvec3 local{dot(d, axes[0]), dot(d, axes[1]), dot(d, axes[2])};
                lo = {std::min(lo.x, local.x), std::min(lo.y, local.y), std::min(lo.z, local.z)};
                hi = {std::max(hi.x, local.x), std::max(hi.y, local.y), std::max(hi.z, local.z)};
            }
        }
    }

    const double spacing = std::max(lat_step, lon_step);
    const double max_abs_height = std::max(std::fabs(min_height), std::fabs(max_height));
    const double k =
        4.0 * (e.semi_major_axis() / std::pow(1.0 - e.flattening(), 6.0) + max_abs_height);
    const double margin = k * spacing * spacing / 4.0;

    const dvec3 mid = (lo + hi) * 0.5;
    oriented_box box;
    box.center = origin + axes[0] * mid.x + axes[1] * mid.y + axes[2] * mid.z;
    box.axes = axes;
    box.half_extents = (hi - lo) * 0.5 + dvec3{margin, margin, margin};
    return box;
}

bounding_sphere bound_box(const oriented_box& box) noexcept
{
    return {box.center, length(box.half_extents)};
}

} // namespace vgl::internal
