// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Ellipsoid of revolution and conversions between geodetic, ECEF and ENU coordinates.
// Angles are in radians and lengths in metres.

#ifndef VGL_GEODESY_ELLIPSOID_HPP
#define VGL_GEODESY_ELLIPSOID_HPP

#include "foundation/math.hpp"

#include <vgl/geodesy.h>

namespace vgl::internal
{

struct geodetic
{
    double latitude = 0.0;
    double longitude = 0.0;
    double height = 0.0;

    [[nodiscard]] static constexpr geodetic from_c(const vgl_geodetic& g) noexcept
    {
        return {g.latitude, g.longitude, g.height};
    }

    [[nodiscard]] constexpr vgl_geodetic to_c() const noexcept
    {
        return {latitude, longitude, height};
    }
};

class ellipsoid
{
public:
    // WGS84.
    ellipsoid() noexcept;

    // The parameters must satisfy is_valid.
    ellipsoid(double semi_major_axis, double flattening) noexcept;

    [[nodiscard]] static ellipsoid from_c(const vgl_ellipsoid& e) noexcept
    {
        return {e.semi_major_axis, e.flattening};
    }

    [[nodiscard]] static bool is_valid(double semi_major_axis, double flattening) noexcept;

    [[nodiscard]] double semi_major_axis() const noexcept { return a_; }
    [[nodiscard]] double semi_minor_axis() const noexcept { return a_ * (1.0 - f_); }
    [[nodiscard]] double flattening() const noexcept { return f_; }
    [[nodiscard]] double eccentricity_squared() const noexcept { return e2_; }

    // Prime vertical radius of curvature at a geodetic latitude.
    [[nodiscard]] double prime_vertical_radius(double latitude) const noexcept;

    // Converts a geodetic position to ECEF. The latitude must be in [-pi/2, pi/2].
    [[nodiscard]] dvec3 to_ecef(const geodetic& g) const noexcept;

    // Converts an ECEF position to geodetic, with the longitude in [-pi, pi].
    [[nodiscard]] geodetic to_geodetic(const dvec3& p) const noexcept;

    // Unit normal of the ellipsoid at a geodetic position, which is the ENU up axis.
    [[nodiscard]] static dvec3 surface_normal(double latitude, double longitude) noexcept;

    // Transform from ENU coordinates at origin to ECEF.
    [[nodiscard]] dmat4 enu_to_ecef(const geodetic& origin) const noexcept;

private:
    double a_;   // Semi-major axis.
    double f_;   // Flattening.
    double e2_;  // First eccentricity squared, f (2 - f).
    double e2m_; // 1 - e2.
    double e4_;  // e2 squared.
};

} // namespace vgl::internal

#endif // VGL_GEODESY_ELLIPSOID_HPP
