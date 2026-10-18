// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/geodesy.h (docs/c-api.md, section 16).

#ifndef VGL_GEODESY_HPP
#define VGL_GEODESY_HPP

#include <vgl/base.hpp>
#include <vgl/geodesy.h>

namespace vgl
{

inline constexpr double pi = VGL_PI;

[[nodiscard]] constexpr double degrees_to_radians(double degrees) noexcept
{
    return VGL_DEGREES_TO_RADIANS(degrees);
}

[[nodiscard]] constexpr double radians_to_degrees(double radians) noexcept
{
    return VGL_RADIANS_TO_DEGREES(radians);
}

// Returns the WGS84 ellipsoid.
[[nodiscard]] inline vgl_ellipsoid ellipsoid_wgs84_get() noexcept
{
    vgl_ellipsoid e{};
    static_cast<void>(vgl_ellipsoid_wgs84_get(&e));
    return e;
}

[[nodiscard]] inline expected<vgl_dvec3>
ellipsoid_geodetic_to_ecef(const vgl_ellipsoid& ellipsoid, const vgl_geodetic& geodetic) noexcept
{
    vgl_dvec3 ecef{};
    const auto r =
        static_cast<result>(vgl_ellipsoid_geodetic_to_ecef(&ellipsoid, &geodetic, &ecef));
    if (failed(r))
    {
        return unexpected(r);
    }
    return ecef;
}

[[nodiscard]] inline expected<vgl_geodetic>
ellipsoid_ecef_to_geodetic(const vgl_ellipsoid& ellipsoid, const vgl_dvec3& ecef) noexcept
{
    vgl_geodetic geodetic{};
    const auto r =
        static_cast<result>(vgl_ellipsoid_ecef_to_geodetic(&ellipsoid, &ecef, &geodetic));
    if (failed(r))
    {
        return unexpected(r);
    }
    return geodetic;
}

struct enu_frame
{
    vgl_dmat4 enu_to_ecef{};
    vgl_dmat4 ecef_to_enu{};
};

[[nodiscard]] inline expected<enu_frame>
ellipsoid_enu_frame_get(const vgl_ellipsoid& ellipsoid, const vgl_geodetic& origin) noexcept
{
    enu_frame frame;
    const auto r = static_cast<result>(
        vgl_ellipsoid_enu_frame_get(&ellipsoid, &origin, &frame.enu_to_ecef, &frame.ecef_to_enu));
    if (failed(r))
    {
        return unexpected(r);
    }
    return frame;
}

// The Earth rotation angle in radians at a Julian date in UT1 (vgl_earth_rotation_angle_get).
[[nodiscard]] inline expected<double> earth_rotation_angle_get(double julian_date) noexcept
{
    double angle = 0.0;
    const auto r = static_cast<result>(vgl_earth_rotation_angle_get(julian_date, &angle));
    if (failed(r))
    {
        return unexpected(r);
    }
    return angle;
}

} // namespace vgl

#endif // VGL_GEODESY_HPP
