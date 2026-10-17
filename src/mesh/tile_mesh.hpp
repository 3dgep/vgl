// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Terrain meshes from height grids (design document, section 7.3, "Tile meshes"): vertices
// relative to the tile's centre with normals and texture coordinates, a ring of skirt
// vertices, and the index list shared by every tile with the same grid size.

#ifndef VGL_MESH_TILE_MESH_HPP
#define VGL_MESH_TILE_MESH_HPP

#include "foundation/math.hpp"
#include "geodesy/ellipsoid.hpp"
#include "tiling/tile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vgl::internal
{

// The smallest grid of a mesh, in vertices per side.
inline constexpr std::uint32_t min_mesh_grid = 9;

struct tile_vertex
{
    std::array<float, 3> position;      // Relative to the tile's centre, along the ECEF axes.
    float height;                       // Above the ellipsoid.
    std::array<std::uint16_t, 2> uv;    // Over the tile: u east, v south, as unsigned normalized.
    std::array<std::int16_t, 2> normal; // Octahedral encoding, as signed normalized.
};

static_assert(sizeof(tile_vertex) == 24, "tile_vertex is 24 bytes");

// The surface at one sample of a grid, for shading at full resolution.
struct tile_sample
{
    float height;                       // Above the ellipsoid.
    std::array<std::int16_t, 2> normal; // Octahedral encoding, as signed normalized.
};

static_assert(sizeof(tile_sample) == 8, "tile_sample is 8 bytes");

// grid * grid vertices, then 4 * (grid - 1) skirt vertices.
[[nodiscard]] constexpr std::size_t tile_mesh_vertex_count(std::uint32_t grid) noexcept
{
    return static_cast<std::size_t>(grid) * grid + 4 * (static_cast<std::size_t>(grid) - 1);
}

// Two triangles per grid cell and per skirt segment.
[[nodiscard]] constexpr std::size_t tile_mesh_index_count(std::uint32_t grid) noexcept
{
    const std::size_t cells = static_cast<std::size_t>(grid) - 1;
    return std::size_t{6} * cells * cells + std::size_t{24} * cells;
}

// The grid vertex of boundary position k, walking the boundary counter-clockwise seen from
// above: from the north-east corner westwards along the north edge, then south, east and
// north. Skirt vertex k hangs below it.
[[nodiscard]] std::uint32_t tile_mesh_boundary_vertex(std::uint32_t grid, std::uint32_t k) noexcept;

// The positions of a tile's samples in ECEF. heights holds grid * grid heights, row by row from
// the north, each row from the west, at the tile's grid-registered positions
// (docs/pack-format.md, section 2.3), and out receives as many positions. Requires grid >= 2.
void build_tile_positions(const ellipsoid& e,
                          const tile_id& t,
                          std::uint32_t grid,
                          std::span<const float> heights,
                          std::span<dvec3> out) noexcept;

// The samples of a grid: heights, and normals from positions as from build_tile_positions,
// the same as build_tile_vertices gives its grid vertices.
void build_tile_samples(const tile_id& t,
                        std::uint32_t grid,
                        std::span<const float> heights,
                        std::span<const dvec3> positions,
                        std::span<tile_sample> out) noexcept;

// The largest stride, a power of two, at which a mesh through every stride-th sample of a grid
// stays within tolerance of every sample, in metres, with at least min_mesh_grid vertices per
// side. positions holds the grid's sample positions, as from build_tile_positions. The
// triangles are those of build_tile_indices. Returns 1 if no stride qualifies.
[[nodiscard]] std::uint32_t
tile_mesh_stride(std::uint32_t grid, std::span<const dvec3> positions, double tolerance) noexcept;

// Keeps every stride-th row and column of a grid of heights, moving them to the front of
// heights. Requires stride to divide grid - 1. Returns the grid of what is kept.
std::uint32_t
tile_heights_decimate(std::uint32_t grid, std::uint32_t stride, std::span<float> heights) noexcept;

// Builds the vertices of a tile. heights holds grid * grid heights, row by row from the north,
// each row from the west, at the tile's grid-registered positions (docs/pack-format.md,
// section 2.3). Skirt vertices lie at skirt_height. scratch holds grid * grid positions.
// Requires grid >= 2.
// Returns the centre the positions are relative to.
dvec3 build_tile_vertices(const ellipsoid& e,
                          const tile_id& t,
                          std::uint32_t grid,
                          std::span<const float> heights,
                          double skirt_height,
                          std::span<dvec3> scratch,
                          std::span<tile_vertex> out) noexcept;

// Builds the index list of a grid size: tile_mesh_index_count(grid) indices, counter-clockwise
// seen from outside the ellipsoid in right-handed ECEF.
void build_tile_indices(std::uint32_t grid, std::span<std::uint32_t> out) noexcept;

// Octahedral encoding of a unit vector.
[[nodiscard]] std::array<std::int16_t, 2> encode_octahedral(const dvec3& n) noexcept;
[[nodiscard]] dvec3 decode_octahedral(const std::array<std::int16_t, 2>& e) noexcept;

} // namespace vgl::internal

#endif // VGL_MESH_TILE_MESH_HPP
