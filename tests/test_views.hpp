// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Views for selection tests: look-at cameras with OpenGL and Direct3D style projections.

#ifndef VGL_TESTS_TEST_VIEWS_HPP
#define VGL_TESTS_TEST_VIEWS_HPP

#include "foundation/math.hpp"

#include <vgl/context.h>

#include <cmath>
#include <cstdint>

namespace vgl::test
{

using vgl::internal::cross;
using vgl::internal::dmat4;
using vgl::internal::dot;
using vgl::internal::dvec3;

inline dvec3 normalized(const dvec3& v)
{
    return v * (1.0 / vgl::internal::length(v));
}

// Right-handed view looking down -z (OpenGL), or left-handed looking down +z (Direct3D).
inline dmat4
look_at(const dvec3& eye, const dvec3& target, const dvec3& up, bool left_handed = false)
{
    const dvec3 f = normalized(target - eye);
    const dvec3 s = normalized(left_handed ? cross(up, f) : cross(f, up));
    const dvec3 u = left_handed ? cross(f, s) : cross(s, f);
    const dvec3 z = left_handed ? f : -f;
    dmat4 m;
    m(0, 0) = s.x;
    m(0, 1) = s.y;
    m(0, 2) = s.z;
    m(1, 0) = u.x;
    m(1, 1) = u.y;
    m(1, 2) = u.z;
    m(2, 0) = z.x;
    m(2, 1) = z.y;
    m(2, 2) = z.z;
    m(0, 3) = -dot(s, eye);
    m(1, 3) = -dot(u, eye);
    m(2, 3) = -dot(z, eye);
    return m;
}

// OpenGL perspective: right-handed, depth to [-1, 1].
inline dmat4 perspective_gl(double fovy, double aspect, double near_plane, double far_plane)
{
    const double f = 1.0 / std::tan(fovy / 2.0);
    dmat4 m;
    m.m = {};
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 2) = (far_plane + near_plane) / (near_plane - far_plane);
    m(2, 3) = 2.0 * far_plane * near_plane / (near_plane - far_plane);
    m(3, 2) = -1.0;
    return m;
}

// Direct3D perspective with reversed, infinite depth: left-handed, depth to [1, 0].
inline dmat4 perspective_d3d_reversed_infinite(double fovy, double aspect, double near_plane)
{
    const double f = 1.0 / std::tan(fovy / 2.0);
    dmat4 m;
    m.m = {};
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 3) = near_plane;
    m(3, 2) = 1.0;
    return m;
}

inline vgl_view make_view(const dmat4& view,
                          const dmat4& projection,
                          std::uint32_t width = 1920,
                          std::uint32_t height = 1080)
{
    vgl_view v{};
    v.struct_size = sizeof(v);
    v.view_matrix = view.to_c();
    v.projection_matrix = projection.to_c();
    v.viewport_width = width;
    v.viewport_height = height;
    return v;
}

} // namespace vgl::test

#endif // VGL_TESTS_TEST_VIEWS_HPP
