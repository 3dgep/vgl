// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Bounding volumes of geographic regions with a height range, used to cull tiles
// (design document, sections 6.2 and 7.3).

#ifndef VGL_GEODESY_BOUNDS_HPP
#define VGL_GEODESY_BOUNDS_HPP

#include "geodesy/ellipsoid.hpp"

#include <array>

namespace vgl::internal
{

// Region in radians with west <= east and south <= north. Regions do not wrap across the
// antimeridian; tiles never do.
struct geographic_region
{
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
};

struct oriented_box
{
    dvec3 center;
    std::array<dvec3, 3> axes; // Orthonormal.
    dvec3 half_extents;        // Along each axis.

    [[nodiscard]] bool contains(const dvec3& p, double tolerance = 0.0) const noexcept;
};

struct bounding_sphere
{
    dvec3 center;
    double radius = 0.0;

    [[nodiscard]] bool contains(const dvec3& p, double tolerance = 0.0) const noexcept;
};

// Smallest box, aligned with the ENU frame at the centre of the region, that contains every
// point of the region at every height in [min_height, max_height]. The box is conservative:
// it contains the region exactly, enlarged by at most a small sampling margin (section 7.3).
[[nodiscard]] oriented_box bound_region(const ellipsoid& e,
                                        const geographic_region& region,
                                        double min_height,
                                        double max_height) noexcept;

// Sphere that contains a box.
[[nodiscard]] bounding_sphere bound_box(const oriented_box& box) noexcept;

} // namespace vgl::internal

#endif // VGL_GEODESY_BOUNDS_HPP
