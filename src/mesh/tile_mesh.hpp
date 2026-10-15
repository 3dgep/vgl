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
