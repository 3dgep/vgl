// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "tiling/selection.hpp"

#include "tiling/tile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vgl::internal
{

namespace
{

bool all_finite(const vgl_dmat4& m) noexcept
{
    return std::all_of(std::begin(m.m), std::end(m.m), [](double v) { return std::isfinite(v); });
}

std::array<dvec3, 8> box_corners(const oriented_box& box) noexcept
{
    std::array<dvec3, 8> corners;
    for (std::size_t i = 0; i < corners.size(); ++i)
    {
        const double sx = (i & 1u) != 0 ? 1.0 : -1.0;
        const double sy = (i & 2u) != 0 ? 1.0 : -1.0;
        const double sz = (i & 4u) != 0 ? 1.0 : -1.0;
        corners[i] = box.center + box.axes[0] * (sx * box.half_extents.x) +
                     box.axes[1] * (sy * box.half_extents.y) +
                     box.axes[2] * (sz * box.half_extents.z);
    }
    return corners;
}

} // namespace

bool make_view_state(const vgl_view& view, view_state& out) noexcept
{
    if (!all_finite(view.view_matrix) || !all_finite(view.projection_matrix) ||
        view.viewport_width == 0 || view.viewport_height == 0 ||
        !std::isfinite(view.max_screen_space_error) || view.max_screen_space_error < 0.0)
    {
        return false;
    }
    const dmat4 v = dmat4::from_c(view.view_matrix);
    const dmat4 p = dmat4::from_c(view.projection_matrix);

    // The view matrix must be affine: its last row is (0, 0, 0, 1).
    if (v(3, 0) != 0.0 || v(3, 1) != 0.0 || v(3, 2) != 0.0 || v(3, 3) != 1.0)
    {
        return false;
    }
    // Eye: the point the view maps to the origin, -A^-1 t for the linear part A.
    const double a00 = v(0, 0);
    const double a01 = v(0, 1);
    const double a02 = v(0, 2);
    const double a10 = v(1, 0);
    const double a11 = v(1, 1);
    const double a12 = v(1, 2);
    const double a20 = v(2, 0);
    const double a21 = v(2, 1);
    const double a22 = v(2, 2);
    const double c00 = a11 * a22 - a12 * a21;
    const double c01 = a12 * a20 - a10 * a22;
    const double c02 = a10 * a21 - a11 * a20;
    const double det = a00 * c00 + a01 * c01 + a02 * c02;
    const double scale = std::fabs(a00) + std::fabs(a11) + std::fabs(a22) + 1.0;
    if (!(std::fabs(det) > 1e-12 * scale * scale * scale))
    {
        return false;
    }
    const double inv_det = 1.0 / det;
    // Rows of the inverse are the cofactor columns.
    const dvec3 r0{c00, a02 * a21 - a01 * a22, a01 * a12 - a02 * a11};
    const dvec3 r1{c01, a00 * a22 - a02 * a20, a02 * a10 - a00 * a12};
    const dvec3 r2{c02, a01 * a20 - a00 * a21, a00 * a11 - a01 * a10};
    const dvec3 t{v(0, 3), v(1, 3), v(2, 3)};
    out.eye = -dvec3{dot(r0, t), dot(r1, t), dot(r2, t)} * inv_det;
    out.view_matrix = v;

    // Clip-space planes from the rows of P * V: w + x, w - x, w + y, w - y and w, each >= 0.
    const dmat4 m = p * v;
    const auto row = [&](int r) { return plane{{m(r, 0), m(r, 1), m(r, 2)}, m(r, 3)}; };
    const plane x = row(0);
    const plane y = row(1);
    const plane w = row(3);
    out.planes = {plane{w.n + x.n, w.d + x.d},
                  plane{w.n - x.n, w.d - x.d},
                  plane{w.n + y.n, w.d + y.d},
                  plane{w.n - y.n, w.d - y.d},
                  w};

    out.orthographic = p(3, 0) == 0.0 && p(3, 1) == 0.0 && p(3, 2) == 0.0 && p(3, 3) != 0.0;
    out.pixel_scale = std::fabs(p(1, 1)) * view.viewport_height / 2.0;
    if (out.orthographic)
    {
        out.pixel_scale /= std::fabs(p(3, 3));
    }
    if (!(out.pixel_scale > 0.0) || !std::isfinite(out.pixel_scale))
    {
        return false;
    }
    out.max_error = view.max_screen_space_error == 0.0 ? default_max_screen_space_error
                                                       : view.max_screen_space_error;
    return true;
}

bool box_in_frustum(const view_state& view, const oriented_box& box) noexcept
{
    // The box is outside if it lies wholly on the outer side of any plane.
    return std::ranges::all_of(view.planes, [&](const plane& p) {
        const double distance = dot(p.n, box.center) + p.d;
        const double radius = box.half_extents.x * std::fabs(dot(p.n, box.axes[0])) +
                              box.half_extents.y * std::fabs(dot(p.n, box.axes[1])) +
                              box.half_extents.z * std::fabs(dot(p.n, box.axes[2]));
        return distance + radius >= 0.0;
    });
}

horizon_occluder
make_horizon_occluder(const ellipsoid& e, double lowest_height, const dvec3& eye) noexcept
{
    horizon_occluder o;
    const double a = e.semi_major_axis() + lowest_height;
    const double b = e.semi_major_axis() * (1.0 - e.flattening()) + lowest_height;
    if (!(a > 0.0) || !(b > 0.0))
    {
        return o;
    }
    o.inverse_radii = {1.0 / a, 1.0 / a, 1.0 / b};
    o.eye = {eye.x * o.inverse_radii.x, eye.y * o.inverse_radii.y, eye.z * o.inverse_radii.z};
    o.horizon_distance_squared = dot(o.eye, o.eye) - 1.0;
    o.active = o.horizon_distance_squared > 0.0;
    return o;
}

bool scaled_point_below_horizon(const horizon_occluder& o, const dvec3& p) noexcept
{
    if (!o.active)
    {
        return false;
    }
    const dvec3 to_point = p - o.eye;
    // How far the point lies along the direction from the eye to the centre.
    const double along = -dot(to_point, o.eye);
    return along > o.horizon_distance_squared &&
           along * along / dot(to_point, to_point) > o.horizon_distance_squared;
}

bool box_below_horizon(const horizon_occluder& o, const oriented_box& box) noexcept
{
    if (!o.active)
    {
        return false;
    }
    return std::ranges::all_of(box_corners(box), [&](const dvec3& corner) {
        const dvec3 p{corner.x * o.inverse_radii.x,
                      corner.y * o.inverse_radii.y,
                      corner.z * o.inverse_radii.z};
        return scaled_point_below_horizon(o, p);
    });
}

bool horizon_point(const ellipsoid& e,
                   double lowest_height,
                   const geographic_region& region,
                   double max_height,
                   dvec3& out) noexcept
{
    const double a = e.semi_major_axis() + lowest_height;
    const double b = e.semi_major_axis() * (1.0 - e.flattening()) + lowest_height;
    if (!(a > 0.0) || !(b > 0.0))
    {
        return false;
    }
    const dvec3 inverse_radii{1.0 / a, 1.0 / a, 1.0 / b};
    const auto scaled = [&](const dvec3& p) {
        return dvec3{p.x * inverse_radii.x, p.y * inverse_radii.y, p.z * inverse_radii.z};
    };

    // Samples as dense as bound_region's, raised by its margin so that the surface between
    // samples stays below them.
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
    const double spacing = std::max(lat_step, lon_step);
    const double k =
        4.0 * (e.semi_major_axis() / std::pow(1.0 - e.flattening(), 6.0) + std::fabs(max_height));
    const double height = max_height + k * spacing * spacing / 4.0;

    const dvec3 centre = scaled(
        e.to_ecef({(region.south + region.north) / 2.0, (region.west + region.east) / 2.0, 0.0}));
    const dvec3 direction = centre * (1.0 / length(centre));

    double magnitude = 0.0;
    for (int i = 0; i <= lat_intervals; ++i)
    {
        const double lat = i == lat_intervals ? region.north : region.south + i * lat_step;
        for (int j = 0; j <= lon_intervals; ++j)
        {
            const double lon = j == lon_intervals ? region.east : region.west + j * lon_step;
            const dvec3 p = scaled(e.to_ecef({lat, lon, height}));
            // The distance along direction at which a point hides p (Cesium's
            // EllipsoidalOccluder.computeHorizonCullingPoint).
            const double m = std::max(1.0, length(p));
            const dvec3 unit = p * (1.0 / length(p));
            const double cos_alpha = dot(unit, direction);
            const double sin_alpha = length(cross(unit, direction));
            const double cos_beta = 1.0 / m;
            const double sin_beta = std::sqrt(m * m - 1.0) * cos_beta;
            const double denominator = cos_alpha * cos_beta - sin_alpha * sin_beta;
            if (!(denominator > 0.0))
            {
                return false;
            }
            magnitude = std::max(magnitude, 1.0 / denominator);
        }
    }
    out = direction * magnitude;
    return true;
}

double distance_to_box(const dvec3& p, const oriented_box& box) noexcept
{
    const dvec3 d = p - box.center;
    const std::array<double, 3> extents = {
        box.half_extents.x, box.half_extents.y, box.half_extents.z};
    double sum = 0.0;
    for (std::size_t i = 0; i < 3; ++i)
    {
        const double along = std::fabs(dot(d, box.axes[i]));
        const double outside = along - extents[i];
        if (outside > 0.0)
        {
            sum += outside * outside;
        }
    }
    return std::sqrt(sum);
}

double
tile_geometric_error(const ellipsoid& e, std::uint32_t level, std::uint32_t tile_size) noexcept
{
    return e.semi_major_axis() * radians(tile_span(level)) / (tile_size - 1);
}

double screen_space_error(const view_state& view, double geometric_error, double distance) noexcept
{
    if (view.orthographic)
    {
        return geometric_error * view.pixel_scale;
    }
    if (!(distance > 0.0))
    {
        return std::numeric_limits<double>::infinity();
    }
    return geometric_error * view.pixel_scale / distance;
}

} // namespace vgl::internal
