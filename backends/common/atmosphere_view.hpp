// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// What the backends' atmosphere shaders (atmosphere.glsl) need of a view, computed in double
// precision: camera space turned into the atmosphere's frame, in which the ellipsoid is a sphere
// and distances are in kilometres, the camera's place and height in it, and the sun's direction.
// The sky also needs the inverse of the projection, to find the view ray through each pixel.

#ifndef VGL_BACKENDS_ATMOSPHERE_VIEW_HPP
#define VGL_BACKENDS_ATMOSPHERE_VIEW_HPP

#include <vgl/base.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace vgl::backends
{

struct atmosphere_view
{
    std::array<float, 9> to_planet = {}; // Camera space to the frame, column-major.
    std::array<float, 3> camera_km = {};
    float camera_height = 0.0f; // Above the sphere, in kilometres.
    std::array<float, 3> sun_planet = {};
};

// The camera's position in ECEF, from a view matrix (ECEF to camera space): -R^T t.
inline vgl_dvec3 view_eye(const vgl_dmat4& view)
{
    const double* m = &view.m[0];
    return {-(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]),
            -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]),
            -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14])};
}

// The sun's direction for lighting along the ECEF axes: the host's, or straight above the camera
// when it gave none, so that the atmosphere always has a sun.
inline vgl_dvec3 atmosphere_sun(const vgl_dmat4& view, const vgl_dvec3& sun)
{
    return sun.x == 0.0 && sun.y == 0.0 && sun.z == 0.0 ? view_eye(view) : sun;
}

// The atmosphere's values for a view matrix and a sun direction along the ECEF axes, which
// need not be normalized and may be zero (atmosphere_sun). The atmosphere is the Earth's, over
// the WGS84 ellipsoid.
inline atmosphere_view atmosphere_for(const vgl_dmat4& view, const vgl_dvec3& sun_direction)
{
    // The ellipsoid becomes a sphere of its semi-major axis when z stretches by a / b.
    constexpr double semi_major = 6378137.0;
    constexpr double stretch = 1.0 / (1.0 - 1.0 / 298.257223563);
    const std::array<double, 3> scale = {1e-3, 1e-3, 1e-3 * stretch};
    const double* m = &view.m[0];
    atmosphere_view a;
    // The view's rotation R is its upper 3 x 3, column-major; R^T turns camera space to ECEF,
    // and R^T(i, j) = R(j, i) = m[i * 4 + j].
    for (std::size_t i = 0; i < 3; ++i)
    {
        for (std::size_t j = 0; j < 3; ++j)
        {
            a.to_planet[j * 3 + i] = static_cast<float>(scale[i] * m[i * 4 + j]);
        }
    }
    const vgl_dvec3 eye = view_eye(view);
    const std::array<double, 3> c = {eye.x * scale[0], eye.y * scale[1], eye.z * scale[2]};
    const double radius = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    for (std::size_t i = 0; i < 3; ++i)
    {
        a.camera_km[i] = static_cast<float>(c[i]);
    }
    a.camera_height = static_cast<float>(radius - semi_major * 1e-3);
    const vgl_dvec3 sun = atmosphere_sun(view, sun_direction);
    const std::array<double, 3> s = {sun.x, sun.y, sun.z * stretch};
    const double length = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (std::size_t i = 0; i < 3; ++i)
    {
        a.sun_planet[i] = static_cast<float>(length > 0.0 ? s[i] / length : 0.0);
    }
    return a;
}

// The inverse of a matrix, by cofactors; false if it has none.
inline bool invert(const vgl_dmat4& in, vgl_dmat4& out)
{
    const double* a = &in.m[0];
    std::array<double, 16> inv{};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0.0 || !std::isfinite(det))
    {
        return false;
    }
    for (std::size_t i = 0; i < inv.size(); ++i)
    {
        out.m[i] = inv[i] / det;
    }
    return true;
}

} // namespace vgl::backends

#endif // VGL_BACKENDS_ATMOSPHERE_VIEW_HPP
