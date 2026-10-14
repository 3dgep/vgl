// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "geodesy/ellipsoid.hpp"

#include <algorithm>
#include <cmath>

namespace vgl::internal
{

ellipsoid::ellipsoid() noexcept : ellipsoid(VGL_WGS84_SEMI_MAJOR_AXIS, VGL_WGS84_FLATTENING) {}

ellipsoid::ellipsoid(double semi_major_axis, double flattening) noexcept
    : a_(semi_major_axis), f_(flattening), e2_(flattening * (2.0 - flattening)),
      e2m_((1.0 - flattening) * (1.0 - flattening)), e4_(e2_ * e2_)
{
}

bool ellipsoid::is_valid(double semi_major_axis, double flattening) noexcept
{
    return std::isfinite(semi_major_axis) && semi_major_axis > 0.0 && std::isfinite(flattening) &&
           flattening >= 0.0 && flattening < 0.5;
}

double ellipsoid::prime_vertical_radius(double latitude) const noexcept
{
    const double s = std::sin(latitude);
    return a_ / std::sqrt(1.0 - e2_ * s * s);
}

dvec3 ellipsoid::to_ecef(const geodetic& g) const noexcept
{
    const double sin_lat = std::sin(g.latitude);
    const double cos_lat = std::cos(g.latitude);
    const double n = a_ / std::sqrt(1.0 - e2_ * sin_lat * sin_lat);
    const double r = (n + g.height) * cos_lat;
    return {r * std::cos(g.longitude), r * std::sin(g.longitude), (n * e2m_ + g.height) * sin_lat};
}

// Closed-form inverse by Vermeille (2011), "An analytical method to transform geocentric into
// geodetic coordinates", Journal of Geodesy 85(2), in the numerically stable formulation that
// GeographicLib (C. F. F. Karney) uses. It is exact apart from rounding for every position,
// including those inside the evolute near the centre of the ellipsoid.
geodetic ellipsoid::to_geodetic(const dvec3& p) const noexcept
{
    const double r = std::hypot(p.x, p.y);
    const double longitude = (p.x == 0.0 && p.y == 0.0) ? 0.0 : std::atan2(p.y, p.x);

    if (r == 0.0 && p.z == 0.0)
    {
        // The centre: the nearest surface points are the poles.
        return {pi / 2.0, longitude, -semi_minor_axis()};
    }

    const double pp = (r / a_) * (r / a_);
    const double qq = e2m_ * (p.z / a_) * (p.z / a_);
    const double rr = (pp + qq - e4_) / 6.0;

    double sin_lat = 0.0;
    double cos_lat = 0.0;
    double height = 0.0;

    if (e4_ * qq != 0.0 || rr > 0.0)
    {
        const double s = e4_ * pp * qq / 4.0;
        const double r2 = rr * rr;
        const double r3 = rr * r2;
        const double disc = s * (2.0 * r3 + s);
        double u = rr;
        if (disc >= 0.0)
        {
            // Pick the sign of the square root that avoids cancellation.
            double t3 = s + r3;
            t3 += t3 < 0.0 ? -std::sqrt(disc) : std::sqrt(disc);
            const double t = std::cbrt(t3);
            u += t + (t != 0.0 ? r2 / t : 0.0);
        }
        else
        {
            // Inside the evolute, where three real roots exist.
            const double angle = std::atan2(std::sqrt(-disc), -(s + r3));
            u += 2.0 * rr * std::cos(angle / 3.0);
        }
        const double v = std::sqrt(u * u + e4_ * qq);
        // u + v, computed without cancellation when u is negative.
        const double uv = u < 0.0 ? e4_ * qq / (v - u) : u + v;
        const double w = std::max(0.0, e2_ * (uv - qq) / (2.0 * v));
        const double k = uv / (std::sqrt(uv + w * w) + w);
        const double k2 = k + e2_;
        const double d = k * r / k2;
        const double h = std::hypot(p.z / k, r / k2);
        sin_lat = (p.z / k) / h;
        cos_lat = (r / k2) / h;
        height = (1.0 - e2m_ / k) * std::hypot(d, p.z);
    }
    else
    {
        // On the equatorial plane, inside the singular disc of radius a e^2: the nearest
        // surface points lie off the plane, symmetric about it.
        const double zz = std::sqrt((e4_ - pp) / e2m_);
        const double xx = std::sqrt(pp);
        const double h = std::hypot(zz, xx);
        sin_lat = zz / h;
        cos_lat = xx / h;
        if (p.z < 0.0)
        {
            sin_lat = -sin_lat;
        }
        height = -a_ * e2m_ * h / e2_;
    }

    return {std::atan2(sin_lat, cos_lat), longitude, height};
}

dvec3 ellipsoid::surface_normal(double latitude, double longitude) noexcept
{
    const double cos_lat = std::cos(latitude);
    return {cos_lat * std::cos(longitude), cos_lat * std::sin(longitude), std::sin(latitude)};
}

dmat4 ellipsoid::enu_to_ecef(const geodetic& origin) const noexcept
{
    const double sin_lat = std::sin(origin.latitude);
    const double cos_lat = std::cos(origin.latitude);
    const double sin_lon = std::sin(origin.longitude);
    const double cos_lon = std::cos(origin.longitude);

    const dvec3 east{-sin_lon, cos_lon, 0.0};
    const dvec3 north{-sin_lat * cos_lon, -sin_lat * sin_lon, cos_lat};
    const dvec3 up{cos_lat * cos_lon, cos_lat * sin_lon, sin_lat};
    return dmat4::from_axes(east, north, up, to_ecef(origin));
}

} // namespace vgl::internal
