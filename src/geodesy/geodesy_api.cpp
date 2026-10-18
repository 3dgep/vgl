// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C API for the geodesy module (include/vgl/geodesy.h). These functions take no context, so
// they report failures through result codes only and cannot log (docs/c-api.md, section 7.3).

#include "geodesy/ellipsoid.hpp"

#include <cmath>

namespace
{

using vgl::internal::dmat4;
using vgl::internal::dvec3;
using vgl::internal::ellipsoid;
using vgl::internal::geodetic;
using vgl::internal::pi;

// Latitudes this far beyond a pole are treated as the pole, so that converted angles such as
// VGL_DEGREES_TO_RADIANS(90.0) are always accepted.
constexpr double pole_tolerance = 1e-12;

bool read_ellipsoid(const vgl_ellipsoid* in, ellipsoid& out) noexcept
{
    if (in == nullptr || !ellipsoid::is_valid(in->semi_major_axis, in->flattening))
    {
        return false;
    }
    out = ellipsoid::from_c(*in);
    return true;
}

bool read_geodetic(const vgl_geodetic* in, geodetic& out) noexcept
{
    if (in == nullptr || !std::isfinite(in->latitude) || !std::isfinite(in->longitude) ||
        !std::isfinite(in->height) || std::fabs(in->latitude) > pi / 2.0 + pole_tolerance)
    {
        return false;
    }
    out = geodetic::from_c(*in);
    out.latitude = std::fmax(-pi / 2.0, std::fmin(pi / 2.0, out.latitude));
    return true;
}

} // namespace

extern "C" VGL_API vgl_result vgl_ellipsoid_wgs84_get(vgl_ellipsoid* out_ellipsoid)
{
    if (out_ellipsoid == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_ellipsoid = {VGL_WGS84_SEMI_MAJOR_AXIS, VGL_WGS84_FLATTENING};
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_ellipsoid_geodetic_to_ecef(const vgl_ellipsoid* ellipsoid_in,
                                                             const vgl_geodetic* geodetic_in,
                                                             vgl_dvec3* out_ecef)
{
    ellipsoid e;
    geodetic g;
    if (out_ecef == nullptr || !read_ellipsoid(ellipsoid_in, e) || !read_geodetic(geodetic_in, g))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_ecef = e.to_ecef(g).to_c();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_ellipsoid_ecef_to_geodetic(const vgl_ellipsoid* ellipsoid_in,
                                                             const vgl_dvec3* ecef,
                                                             vgl_geodetic* out_geodetic)
{
    ellipsoid e;
    if (out_geodetic == nullptr || ecef == nullptr || !read_ellipsoid(ellipsoid_in, e) ||
        !std::isfinite(ecef->x) || !std::isfinite(ecef->y) || !std::isfinite(ecef->z))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_geodetic = e.to_geodetic(dvec3::from_c(*ecef)).to_c();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_ellipsoid_enu_frame_get(const vgl_ellipsoid* ellipsoid_in,
                                                          const vgl_geodetic* origin,
                                                          vgl_dmat4* out_enu_to_ecef,
                                                          vgl_dmat4* out_ecef_to_enu)
{
    ellipsoid e;
    geodetic g;
    if ((out_enu_to_ecef == nullptr && out_ecef_to_enu == nullptr) ||
        !read_ellipsoid(ellipsoid_in, e) || !read_geodetic(origin, g))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const dmat4 enu_to_ecef = e.enu_to_ecef(g);
    if (out_enu_to_ecef != nullptr)
    {
        *out_enu_to_ecef = enu_to_ecef.to_c();
    }
    if (out_ecef_to_enu != nullptr)
    {
        *out_ecef_to_enu = rigid_inverse(enu_to_ecef).to_c();
    }
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_earth_rotation_angle_get(double julian_date, double* out_angle)
{
    if (out_angle == nullptr || !std::isfinite(julian_date))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    // IERS Conventions (2010), equation 5.15, with the day's fraction apart from the whole
    // days, so that the turns keep their precision.
    const double days = julian_date - 2451545.0;
    const double turns = 0.7790572732640 + 0.00273781191135448 * days + std::fmod(days, 1.0);
    double angle = 2.0 * vgl::internal::pi * std::fmod(turns, 1.0);
    if (angle < 0.0)
    {
        angle += 2.0 * vgl::internal::pi;
    }
    *out_angle = angle;
    return VGL_RESULT_SUCCESS;
}
