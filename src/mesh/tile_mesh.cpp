// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "mesh/tile_mesh.hpp"

#include <algorithm>
#include <cmath>

namespace vgl::internal
{

namespace
{

constexpr double snorm16_max = 32767.0;
constexpr double unorm16_max = 65535.0;

std::int16_t to_snorm16(double v) noexcept
{
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * snorm16_max));
}

std::uint16_t to_unorm16(double v) noexcept
{
    return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * unorm16_max));
}

dvec3 normalized(const dvec3& v) noexcept
{
    const double l = length(v);
    return l > 0.0 ? v * (1.0 / l) : v;
}

// Latitude of row j and longitude of column i of a tile's grid, in degrees. Exact at the edges,
// so that neighbouring tiles share their boundary positions.
double grid_latitude(const degree_region& b, double step, std::uint32_t grid, std::uint32_t j)
{
    return j == grid - 1 ? b.south : b.north - j * step;
}

double grid_longitude(const degree_region& b, double step, std::uint32_t grid, std::uint32_t i)
{
    return i == grid - 1 ? b.east : b.west + i * step;
}

// The unit normal at sample (i, j) of a grid of positions.
dvec3 grid_normal(const tile_id& t,
                  std::uint32_t grid,
                  std::span<const dvec3> positions,
                  std::uint32_t i,
                  std::uint32_t j) noexcept
{
    // Central differences inside, one-sided at the edges: east cross north is up.
    const std::size_t west = static_cast<std::size_t>(j) * grid + (i > 0 ? i - 1 : i);
    const std::size_t east = static_cast<std::size_t>(j) * grid + (i < grid - 1 ? i + 1 : i);
    const std::size_t north = static_cast<std::size_t>(j > 0 ? j - 1 : j) * grid + i;
    const std::size_t south = static_cast<std::size_t>(j < grid - 1 ? j + 1 : j) * grid + i;
    const dvec3 along_row = positions[east] - positions[west];
    const dvec3 n = cross(along_row, positions[north] - positions[south]);
    if (!(length(along_row) > 1e-3) || !(length(n) > 0.0))
    {
        // At a pole, a row collapses to a point (up to rounding): use the ellipsoid.
        const degree_region b = tile_bounds(t);
        const double step = tile_span(t.level) / (grid - 1);
        return ellipsoid::surface_normal(radians(grid_latitude(b, step, grid, j)),
                                         radians(grid_longitude(b, step, grid, i)));
    }
    return normalized(n);
}

} // namespace

std::uint32_t tile_mesh_boundary_vertex(std::uint32_t grid, std::uint32_t k) noexcept
{
    const std::uint32_t last = grid - 1;
    if (k < last)
    {
        return last - k; // North edge, westwards from the north-east corner.
    }
    k -= last;
    if (k < last)
    {
        return k * grid; // West edge, southwards from the north-west corner.
    }
    k -= last;
    if (k < last)
    {
        return last * grid + k; // South edge, eastwards from the south-west corner.
    }
    k -= last;
    return (last - k) * grid + last; // East edge, northwards from the south-east corner.
}

std::array<std::int16_t, 2> encode_octahedral(const dvec3& n) noexcept
{
    const double sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    double x = n.x / sum;
    double y = n.y / sum;
    if (n.z < 0.0)
    {
        const double fx = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        const double fy = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = fx;
        y = fy;
    }
    return {to_snorm16(x), to_snorm16(y)};
}

dvec3 decode_octahedral(const std::array<std::int16_t, 2>& e) noexcept
{
    const double x = std::max(e[0] / snorm16_max, -1.0);
    const double y = std::max(e[1] / snorm16_max, -1.0);
    dvec3 n{x, y, 1.0 - std::fabs(x) - std::fabs(y)};
    if (n.z < 0.0)
    {
        n = {(1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0),
             (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0),
             n.z};
    }
    return normalized(n);
}

void build_tile_positions(const ellipsoid& e,
                          const tile_id& t,
                          std::uint32_t grid,
                          std::span<const float> heights,
                          std::span<dvec3> out) noexcept
{
    const degree_region b = tile_bounds(t);
    const double step = tile_span(t.level) / (grid - 1);
    for (std::uint32_t j = 0; j < grid; ++j)
    {
        const double lat = radians(grid_latitude(b, step, grid, j));
        for (std::uint32_t i = 0; i < grid; ++i)
        {
            const double lon = radians(grid_longitude(b, step, grid, i));
            const std::size_t k = static_cast<std::size_t>(j) * grid + i;
            out[k] = e.to_ecef({lat, lon, heights[k]});
        }
    }
}

void build_tile_samples(const tile_id& t,
                        std::uint32_t grid,
                        std::span<const float> heights,
                        std::span<const dvec3> positions,
                        std::span<tile_sample> out) noexcept
{
    for (std::uint32_t j = 0; j < grid; ++j)
    {
        for (std::uint32_t i = 0; i < grid; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * grid + i;
            out[k] = {heights[k], encode_octahedral(grid_normal(t, grid, positions, i, j))};
        }
    }
}

std::uint32_t
tile_mesh_stride(std::uint32_t grid, std::span<const dvec3> positions, double tolerance) noexcept
{
    const std::uint32_t cells = grid - 1;
    // Within a cell, as in build_tile_indices: (a, c, b) north-west of the diagonal from b to
    // c, and (b, c, d) south-east of it.
    const auto within = [&](std::uint32_t stride) {
        for (std::uint32_t j0 = 0; j0 < cells; j0 += stride)
        {
            for (std::uint32_t i0 = 0; i0 < cells; i0 += stride)
            {
                const auto at = [&](std::uint32_t i, std::uint32_t j) -> const dvec3& {
                    return positions[static_cast<std::size_t>(j) * grid + i];
                };
                const dvec3& a = at(i0, j0);
                const dvec3& b = at(i0 + stride, j0);
                const dvec3& c = at(i0, j0 + stride);
                const dvec3& d = at(i0 + stride, j0 + stride);
                for (std::uint32_t j = 0; j <= stride; ++j)
                {
                    for (std::uint32_t i = 0; i <= stride; ++i)
                    {
                        const double u = static_cast<double>(i) / stride;
                        const double v = static_cast<double>(j) / stride;
                        const dvec3 on = u + v <= 1.0
                                             ? a + (b - a) * u + (c - a) * v
                                             : d + (c - d) * (1.0 - u) + (b - d) * (1.0 - v);
                        if (!(length(at(i0 + i, j0 + j) - on) <= tolerance))
                        {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    };
    std::uint32_t stride = 1;
    while (cells % (stride * 2) == 0 && cells / (stride * 2) >= min_mesh_grid - 1 &&
           within(stride * 2))
    {
        stride *= 2;
    }
    return stride;
}

std::uint32_t
tile_heights_decimate(std::uint32_t grid, std::uint32_t stride, std::span<float> heights) noexcept
{
    const std::uint32_t kept = (grid - 1) / stride + 1;
    // Every kept sample moves to an index no larger than its own, so in place is safe.
    for (std::uint32_t j = 0; j < kept; ++j)
    {
        for (std::uint32_t i = 0; i < kept; ++i)
        {
            heights[static_cast<std::size_t>(j) * kept + i] =
                heights[static_cast<std::size_t>(j) * stride * grid +
                        static_cast<std::size_t>(i) * stride];
        }
    }
    return kept;
}

dvec3 build_tile_vertices(const ellipsoid& e,
                          const tile_id& t,
                          std::uint32_t grid,
                          std::span<const float> heights,
                          double skirt_height,
                          std::span<dvec3> scratch,
                          std::span<tile_vertex> out) noexcept
{
    const degree_region b = tile_bounds(t);
    if (grid < 2)
    {
        return {}; // No grid to build.
    }
    const double step = tile_span(t.level) / (grid - 1);
    const auto at = [&](std::uint32_t i, std::uint32_t j) {
        return geodetic{radians(grid_latitude(b, step, grid, j)),
                        radians(grid_longitude(b, step, grid, i)),
                        0.0};
    };

    float lo = heights[0];
    float hi = heights[0];
    for (const float h : heights)
    {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    const dvec3 centre = e.to_ecef({radians((b.north + b.south) / 2.0),
                                    radians((b.west + b.east) / 2.0),
                                    (static_cast<double>(lo) + hi) / 2.0});

    build_tile_positions(e, t, grid, heights, scratch);

    const double uv_step = 1.0 / (grid - 1);
    for (std::uint32_t j = 0; j < grid; ++j)
    {
        for (std::uint32_t i = 0; i < grid; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * grid + i;
            const dvec3 p = scratch[k] - centre;
            tile_vertex& v = out[k];
            v.position = {
                static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
            v.height = heights[k];
            v.uv = {to_unorm16(i * uv_step), to_unorm16(j * uv_step)};
            v.normal = encode_octahedral(grid_normal(t, grid, scratch, i, j));
        }
    }

    const std::size_t ring = 4 * static_cast<std::size_t>(grid - 1);
    const std::size_t first = static_cast<std::size_t>(grid) * grid;
    for (std::size_t k = 0; k < ring; ++k)
    {
        const std::uint32_t above = tile_mesh_boundary_vertex(grid, static_cast<std::uint32_t>(k));
        geodetic g = at(above % grid, above / grid);
        g.height = skirt_height;
        const dvec3 p = e.to_ecef(g) - centre;
        tile_vertex& v = out[first + k];
        v = out[above];
        v.position = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
        v.height = static_cast<float>(skirt_height);
    }
    return centre;
}

void build_tile_indices(std::uint32_t grid, std::span<std::uint32_t> out) noexcept
{
    std::size_t n = 0;
    // Seen from above with north up, (i, j) is the top-left corner of a cell.
    for (std::uint32_t j = 0; j + 1 < grid; ++j)
    {
        for (std::uint32_t i = 0; i + 1 < grid; ++i)
        {
            const std::uint32_t a = j * grid + i;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + grid;
            const std::uint32_t d = c + 1;
            out[n++] = a;
            out[n++] = c;
            out[n++] = b;
            out[n++] = b;
            out[n++] = c;
            out[n++] = d;
        }
    }
    // Along the counter-clockwise boundary walk p -> q, the skirt quad below faces outwards
    // with triangles (p, skirt p, q) and (q, skirt p, skirt q).
    const std::uint32_t ring = 4 * (grid - 1);
    const std::uint32_t first = grid * grid;
    for (std::uint32_t k = 0; k < ring; ++k)
    {
        const std::uint32_t next = (k + 1) % ring;
        const std::uint32_t p = tile_mesh_boundary_vertex(grid, k);
        const std::uint32_t q = tile_mesh_boundary_vertex(grid, next);
        out[n++] = p;
        out[n++] = first + k;
        out[n++] = q;
        out[n++] = q;
        out[n++] = first + k;
        out[n++] = first + next;
    }
}

} // namespace vgl::internal
