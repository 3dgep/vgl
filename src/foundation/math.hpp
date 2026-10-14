// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Double-precision vector and matrix types used inside the library. They have the same
// layout as the public vgl_dvec3 and vgl_dmat4 (column-major, column vectors).

#ifndef VGL_FOUNDATION_MATH_HPP
#define VGL_FOUNDATION_MATH_HPP

#include <vgl/base.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace vgl::internal
{

inline constexpr double pi = std::numbers::pi;

[[nodiscard]] constexpr double radians(double degrees) noexcept
{
    return degrees * (pi / 180.0);
}

[[nodiscard]] constexpr double degrees(double radians) noexcept
{
    return radians * (180.0 / pi);
}

struct dvec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] static constexpr dvec3 from_c(const vgl_dvec3& v) noexcept
    {
        return {v.x, v.y, v.z};
    }
    [[nodiscard]] constexpr vgl_dvec3 to_c() const noexcept { return {x, y, z}; }

    constexpr dvec3& operator+=(const dvec3& o) noexcept
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    constexpr dvec3& operator-=(const dvec3& o) noexcept
    {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }

    constexpr dvec3& operator*=(double s) noexcept
    {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    friend constexpr bool operator==(const dvec3&, const dvec3&) noexcept = default;
};

[[nodiscard]] constexpr dvec3 operator+(dvec3 a, const dvec3& b) noexcept
{
    return a += b;
}

[[nodiscard]] constexpr dvec3 operator-(dvec3 a, const dvec3& b) noexcept
{
    return a -= b;
}

[[nodiscard]] constexpr dvec3 operator-(const dvec3& a) noexcept
{
    return {-a.x, -a.y, -a.z};
}

[[nodiscard]] constexpr dvec3 operator*(dvec3 a, double s) noexcept
{
    return a *= s;
}

[[nodiscard]] constexpr dvec3 operator*(double s, dvec3 a) noexcept
{
    return a *= s;
}

[[nodiscard]] constexpr double dot(const dvec3& a, const dvec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr dvec3 cross(const dvec3& a, const dvec3& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline double length(const dvec3& a) noexcept
{
    return std::hypot(a.x, a.y, a.z);
}

// 4x4 matrix in column-major order: element (row r, column c) is m[c * 4 + r].
struct dmat4
{
    std::array<double, 16> m = {
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};

    [[nodiscard]] static constexpr std::size_t index(int row, int column) noexcept
    {
        return static_cast<std::size_t>(column) * 4 + static_cast<std::size_t>(row);
    }

    [[nodiscard]] constexpr double operator()(int row, int column) const noexcept
    {
        return m[index(row, column)];
    }

    constexpr double& operator()(int row, int column) noexcept { return m[index(row, column)]; }

    [[nodiscard]] static constexpr dmat4 from_c(const vgl_dmat4& c) noexcept
    {
        dmat4 r;
        for (std::size_t i = 0; i < 16; ++i)
        {
            r.m[i] = c.m[i];
        }
        return r;
    }

    [[nodiscard]] constexpr vgl_dmat4 to_c() const noexcept
    {
        vgl_dmat4 c{};
        for (std::size_t i = 0; i < 16; ++i)
        {
            c.m[i] = m[i];
        }
        return c;
    }

    // Builds a rigid transform from three orthonormal axes (columns 0 to 2) and a translation.
    [[nodiscard]] static constexpr dmat4 from_axes(const dvec3& x_axis,
                                                   const dvec3& y_axis,
                                                   const dvec3& z_axis,
                                                   const dvec3& translation) noexcept
    {
        dmat4 r;
        r.m[0] = x_axis.x;
        r.m[1] = x_axis.y;
        r.m[2] = x_axis.z;
        r.m[4] = y_axis.x;
        r.m[5] = y_axis.y;
        r.m[6] = y_axis.z;
        r.m[8] = z_axis.x;
        r.m[9] = z_axis.y;
        r.m[10] = z_axis.z;
        r.m[12] = translation.x;
        r.m[13] = translation.y;
        r.m[14] = translation.z;
        return r;
    }

    friend constexpr bool operator==(const dmat4&, const dmat4&) noexcept = default;
};

[[nodiscard]] constexpr dmat4 operator*(const dmat4& a, const dmat4& b) noexcept
{
    dmat4 r;
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                sum += a(row, k) * b(k, column);
            }
            r(row, column) = sum;
        }
    }
    return r;
}

// Transforms a point (w = 1).
[[nodiscard]] constexpr dvec3 transform_point(const dmat4& a, const dvec3& p) noexcept
{
    return {a(0, 0) * p.x + a(0, 1) * p.y + a(0, 2) * p.z + a(0, 3),
            a(1, 0) * p.x + a(1, 1) * p.y + a(1, 2) * p.z + a(1, 3),
            a(2, 0) * p.x + a(2, 1) * p.y + a(2, 2) * p.z + a(2, 3)};
}

// Transforms a direction (w = 0).
[[nodiscard]] constexpr dvec3 transform_vector(const dmat4& a, const dvec3& v) noexcept
{
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z};
}

// Inverts a rigid transform [R t] (rotation and translation only), giving [R^T -R^T t].
[[nodiscard]] constexpr dmat4 rigid_inverse(const dmat4& a) noexcept
{
    // The columns of R^T are the rows of R.
    const dvec3 row0{a(0, 0), a(0, 1), a(0, 2)};
    const dvec3 row1{a(1, 0), a(1, 1), a(1, 2)};
    const dvec3 row2{a(2, 0), a(2, 1), a(2, 2)};
    // Component i of R^T t is column i of R dotted with t.
    const dvec3 col0{a(0, 0), a(1, 0), a(2, 0)};
    const dvec3 col1{a(0, 1), a(1, 1), a(2, 1)};
    const dvec3 col2{a(0, 2), a(1, 2), a(2, 2)};
    const dvec3 t{a(0, 3), a(1, 3), a(2, 3)};
    return dmat4::from_axes(row0, row1, row2, {-dot(col0, t), -dot(col1, t), -dot(col2, t)});
}

} // namespace vgl::internal

#endif // VGL_FOUNDATION_MATH_HPP
