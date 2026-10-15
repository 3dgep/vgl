// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The geometry of tile selection (design document, section 7.3): a view's frustum, eye and
// pixel scale, culling of tile boxes against the frustum and the horizon, and screen-space
// error. The traversal that uses these lives with the context, which knows what is loaded.

#ifndef VGL_TILING_SELECTION_HPP
#define VGL_TILING_SELECTION_HPP

#include "foundation/math.hpp"
#include "geodesy/bounds.hpp"
#include "geodesy/ellipsoid.hpp"

#include <vgl/context.h>

#include <array>
#include <cstdint>

namespace vgl::internal
{

// Heights between which every point of the Earth's surface lies, in metres above the WGS84
// ellipsoid: the Challenger Deep and Everest, with the geoid's range and a margin.
inline constexpr double lowest_surface_height = -11500.0;
inline constexpr double highest_surface_height = 9000.0;

inline constexpr double default_max_screen_space_error = 2.0;

// A plane n . p + d >= 0 on its inner side. n need not be normalized.
struct plane
{
    dvec3 n;
    double d = 0.0;
};

struct view_state
{
    dvec3 eye;                   // In ECEF.
    std::array<plane, 5> planes; // The four sides of the frustum, and the plane of the eye.
    double pixel_scale = 0.0;    // Pixels per unit of camera space at unit distance.
    bool orthographic = false;
    double max_error = default_max_screen_space_error; // In pixels.
};

// Prepares a view. Returns false if it is invalid: a non-finite element, a view matrix that
// is not an invertible affine transform, an empty viewport or a negative error.
[[nodiscard]] bool make_view_state(const vgl_view& view, view_state& out) noexcept;

// False if the box lies entirely outside the frustum.
[[nodiscard]] bool box_in_frustum(const view_state& view, const oriented_box& box) noexcept;

// The ellipsoid lowered to the lowest surface height, as seen from an eye. In its scaled space,
// where it is the unit sphere, points behind its horizon are hidden.
struct horizon_occluder
{
    dvec3 inverse_radii;
    dvec3 eye; // Scaled.
    double horizon_distance_squared = 0.0;
    bool active = false; // False with the eye inside the occluder, where it hides nothing.
};

[[nodiscard]] horizon_occluder
make_horizon_occluder(const ellipsoid& e, double lowest_height, const dvec3& eye) noexcept;

// True if the whole box is hidden behind the horizon. The hidden region is convex, so the box
// is hidden if all of its corners are.
[[nodiscard]] bool box_below_horizon(const horizon_occluder& o, const oriented_box& box) noexcept;

// A point in the occluder's scaled space that is hidden only if all of a region at heights up
// to max_height is, which is much tighter than a box for large tiles. Returns false if no such
// point exists, as for regions that wrap far around the ellipsoid.
[[nodiscard]] bool horizon_point(const ellipsoid& e,
                                 double lowest_height,
                                 const geographic_region& region,
                                 double max_height,
                                 dvec3& out) noexcept;

// True if a point in the occluder's scaled space is hidden behind the horizon.
[[nodiscard]] bool scaled_point_below_horizon(const horizon_occluder& o, const dvec3& p) noexcept;

// Distance from a point to a box; 0 inside it.
[[nodiscard]] double distance_to_box(const dvec3& p, const oriented_box& box) noexcept;

// Error of drawing a tile instead of its children, in metres: its sample spacing at the
// equator.
[[nodiscard]] double
tile_geometric_error(const ellipsoid& e, std::uint32_t level, std::uint32_t tile_size) noexcept;

// A geometric error at a distance, projected to pixels.
[[nodiscard]] double
screen_space_error(const view_state& view, double geometric_error, double distance) noexcept;

} // namespace vgl::internal

#endif // VGL_TILING_SELECTION_HPP
