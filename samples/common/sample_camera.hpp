// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Camera math shared by the C++ samples: view matrices from a position over the globe, and
// projections for the depth conventions (vgl/render.h). Matrices are column-major and multiply
// column vectors, as vgl_dmat4 is.

#ifndef VGL_SAMPLES_SAMPLE_CAMERA_HPP
#define VGL_SAMPLES_SAMPLE_CAMERA_HPP

#include <vgl/vgl.hpp>

#include <cmath>

namespace sample
{

inline vgl_dvec3 sub(const vgl_dvec3& a, const vgl_dvec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline vgl_dvec3 cross(const vgl_dvec3& a, const vgl_dvec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline double dot(const vgl_dvec3& a, const vgl_dvec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline vgl_dvec3 normalize(const vgl_dvec3& v)
{
    const double l = std::sqrt(dot(v, v));
    return {v.x / l, v.y / l, v.z / l};
}

// A right-handed view matrix looking from eye at target.
inline vgl_dmat4 look_at(const vgl_dvec3& eye, const vgl_dvec3& target, const vgl_dvec3& up)
{
    const vgl_dvec3 f = normalize(sub(target, eye));
    const vgl_dvec3 s = normalize(cross(f, up));
    const vgl_dvec3 u = cross(s, f);
    vgl_dmat4 m{};
    m.m[0] = s.x;
    m.m[4] = s.y;
    m.m[8] = s.z;
    m.m[1] = u.x;
    m.m[5] = u.y;
    m.m[9] = u.z;
    m.m[2] = -f.x;
    m.m[6] = -f.y;
    m.m[10] = -f.z;
    m.m[12] = -dot(s, eye);
    m.m[13] = -dot(u, eye);
    m.m[14] = dot(f, eye);
    m.m[15] = 1.0;
    return m;
}

// An OpenGL-style perspective projection, mapping near and far to -1 and 1.
inline vgl_dmat4 perspective(double fovy, double aspect, double near_plane, double far_plane)
{
    const double f = 1.0 / std::tan(fovy / 2.0);
    vgl_dmat4 m{};
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    m.m[11] = -1.0;
    m.m[14] = 2.0 * far_plane * near_plane / (near_plane - far_plane);
    return m;
}

// A reversed perspective projection with an infinite far plane, for the reversed-Z
// convention: near maps to 1 and infinity to 0, in a depth range from 0 to 1.
inline vgl_dmat4 perspective_reversed(double fovy, double aspect, double near_plane)
{
    const double f = 1.0 / std::tan(fovy / 2.0);
    vgl_dmat4 m{};
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[11] = -1.0;
    m.m[14] = near_plane;
    return m;
}

// Where a camera is and where it looks: over a point of the globe, turned to a heading
// (clockwise from north) and tilted down by a pitch (90 looks straight down). Angles in
// degrees, the altitude in metres above the ellipsoid.
struct camera
{
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 2.0e7;
    double heading = 0.0;
    double pitch = 90.0;
};

inline vgl_dvec3 camera_eye(const camera& c)
{
    const vgl_ellipsoid e = vgl::ellipsoid_wgs84_get();
    const double lat = vgl::degrees_to_radians(c.latitude);
    const double lon = vgl::degrees_to_radians(c.longitude);
    return *vgl::ellipsoid_geodetic_to_ecef(e, {lat, lon, c.altitude});
}

inline vgl_dmat4 camera_view_matrix(const camera& c)
{
    const double lat = vgl::degrees_to_radians(c.latitude);
    const double lon = vgl::degrees_to_radians(c.longitude);
    const vgl_dvec3 eye = camera_eye(c);
    const vgl_dvec3 up{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
    const vgl_dvec3 north{
        -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat)};
    const vgl_dvec3 east{-std::sin(lon), std::cos(lon), 0.0};
    const double h = vgl::degrees_to_radians(c.heading);
    const vgl_dvec3 ahead{north.x * std::cos(h) + east.x * std::sin(h),
                          north.y * std::cos(h) + east.y * std::sin(h),
                          north.z * std::cos(h) + east.z * std::sin(h)};
    const double p = vgl::degrees_to_radians(c.pitch);
    const vgl_dvec3 forward{ahead.x * std::cos(p) - up.x * std::sin(p),
                            ahead.y * std::cos(p) - up.y * std::sin(p),
                            ahead.z * std::cos(p) - up.z * std::sin(p)};
    const vgl_dvec3 target{eye.x + forward.x, eye.y + forward.y, eye.z + forward.z};
    return look_at(eye, target, c.pitch > 89.0 ? ahead : up);
}

} // namespace sample

#endif // VGL_SAMPLES_SAMPLE_CAMERA_HPP
