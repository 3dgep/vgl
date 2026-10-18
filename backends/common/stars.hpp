// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The star field that every backend draws behind the sky (design document, section 7.6), from
// the stars the host supplies (vgl_star): each star a small quad, two triangles of six
// vertices, which the vertex shader places at the star's direction and widens by a radius in
// pixels, rather than a point, whose size not every API can set.

#ifndef VGL_BACKENDS_STARS_HPP
#define VGL_BACKENDS_STARS_HPP

#include <vgl/base.h>
#include <vgl/render.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vgl::backends
{

// One corner of a star's quad.
struct star_vertex
{
    std::array<float, 3> direction; // Celestial frame, unit length.
    std::array<float, 2> corner;    // -1 or 1 on each axis.
    float intensity;                // Of the brightest channel at the star's centre.
    float radius;                   // Half the quad's side, in pixels.
    std::array<float, 3> color;     // Linear RGB.
};
static_assert(sizeof(star_vertex) == 40, "star_vertex must be tightly packed");

inline constexpr std::uint32_t star_vertex_count = 6; // Per star.

// Byte offsets of star_vertex's members, for the vertex layouts.
inline constexpr std::uint32_t star_direction_offset = 0;
inline constexpr std::uint32_t star_corner_offset = 12;
inline constexpr std::uint32_t star_light_offset = 20; // Intensity, then radius.
inline constexpr std::uint32_t star_color_offset = 28;

// The most stars a backend takes (vgl/render.h).
inline constexpr std::uint32_t max_stars = VGL_STAR_COUNT_MAX;

// True if a star can be drawn: finite, with a direction of about unit length and colours
// from 0 to 1.
inline bool star_is_valid(const vgl_star& s)
{
    const float length =
        std::sqrt(s.direction[0] * s.direction[0] + s.direction[1] * s.direction[1] +
                  s.direction[2] * s.direction[2]);
    const auto unit = [](float c) { return std::isfinite(c) && c >= 0.0f && c <= 1.0f; };
    return std::isfinite(length) && std::fabs(length - 1.0f) < 1e-3f &&
           std::isfinite(s.magnitude) && unit(s.color[0]) && unit(s.color[1]) && unit(s.color[2]) &&
           s.reserved == 0.0f;
}

// The six vertices of a star's quad. Brightness follows the flux, 10^(-0.4 m), through a
// square root, so that faint stars stay visible on a display: magnitude 0 is 1, magnitude 6
// about 0.09. Brighter stars are a little larger, from about 1.2 to 2.6 pixels in radius.
inline void star_quad(const vgl_star& s, star_vertex* out)
{
    const double flux =
        std::pow(10.0, -0.4 * std::clamp(static_cast<double>(s.magnitude), -2.0, 9.0));
    const double root = std::sqrt(flux);
    const auto intensity = static_cast<float>(std::min(root, 1.6));
    const auto radius = static_cast<float>(1.2 + 1.4 * std::min(root, 1.0));
    const std::array<std::array<float, 2>, star_vertex_count> corners = {
        {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
    for (std::uint32_t k = 0; k < star_vertex_count; ++k)
    {
        out[k] = {{s.direction[0], s.direction[1], s.direction[2]},
                  corners[k],
                  intensity,
                  radius,
                  {s.color[0], s.color[1], s.color[2]}};
    }
}

// From the celestial frame to camera space, column-major: the Earth's turn by its rotation
// angle (vgl_earth_rotation_angle_get), then the view's rotation. Composed in double.
inline std::array<float, 9> celestial_to_camera(const vgl_dmat4& view, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    // Celestial to ECEF: x' = c x + s y, y' = -s x + c y, z' = z.
    const std::array<std::array<double, 3>, 3> earth = {
        {{c, s, 0.0}, {-s, c, 0.0}, {0.0, 0.0, 1.0}}};
    const double* m = &view.m[0];
    std::array<float, 9> out{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k)
            {
                sum += m[k * 4 + row] * earth[k][col]; // view(row, k) * earth(k, col)
            }
            out[col * 3 + row] = static_cast<float>(sum);
        }
    }
    return out;
}

} // namespace vgl::backends

#endif // VGL_BACKENDS_STARS_HPP
