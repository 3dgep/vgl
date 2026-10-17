// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the tile mesh builder (src/mesh/tile_mesh.hpp): vertex placement, normals,
// texture coordinates, skirts, and the winding of the shared index list.

#include "geodesy/ellipsoid.hpp"
#include "mesh/tile_mesh.hpp"
#include "tiling/tile.hpp"
#include "vgl_test.hpp"

#include <cmath>
#include <random>
#include <set>
#include <vector>

namespace
{

using namespace vgl::internal;

dvec3 position_of(const tile_vertex& v, const dvec3& centre)
{
    return centre + dvec3{v.position[0], v.position[1], v.position[2]};
}

struct built
{
    std::vector<tile_vertex> vertices;
    std::vector<std::uint32_t> indices;
    dvec3 centre;
};

built build(const tile_id& t, std::uint32_t grid, const std::vector<float>& heights, double skirt)
{
    const ellipsoid e;
    built b;
    b.vertices.resize(tile_mesh_vertex_count(grid));
    b.indices.resize(tile_mesh_index_count(grid));
    std::vector<dvec3> scratch(static_cast<std::size_t>(grid) * grid);
    b.centre = build_tile_vertices(e, t, grid, heights, skirt, scratch, b.vertices);
    build_tile_indices(grid, b.indices);
    return b;
}

} // namespace

TEST_CASE(boundary_walk_visits_every_edge_vertex_once)
{
    for (const std::uint32_t grid : {2u, 9u, 17u})
    {
        std::set<std::uint32_t> seen;
        for (std::uint32_t k = 0; k < 4 * (grid - 1); ++k)
        {
            const std::uint32_t v = tile_mesh_boundary_vertex(grid, k);
            const std::uint32_t i = v % grid;
            const std::uint32_t j = v / grid;
            CHECK(i == 0 || j == 0 || i == grid - 1 || j == grid - 1);
            seen.insert(v);
        }
        CHECK_EQ(seen.size(), static_cast<std::size_t>(4 * (grid - 1)));
        CHECK_EQ(tile_mesh_boundary_vertex(grid, 0), grid - 1); // North-east corner.
    }
}

TEST_CASE(octahedral_normals_round_trip)
{
    std::mt19937 rng(7);
    std::normal_distribution<double> d;
    for (int i = 0; i < 10000; ++i)
    {
        dvec3 n{d(rng), d(rng), d(rng)};
        n = n * (1.0 / length(n));
        const dvec3 back = decode_octahedral(encode_octahedral(n));
        CHECK(dot(n, back) > 1.0 - 1e-8);
    }
    for (const dvec3& axis : {dvec3{0, 0, 1}, dvec3{0, 0, -1}, dvec3{1, 0, 0}, dvec3{0, -1, 0}})
    {
        CHECK(dot(axis, decode_octahedral(encode_octahedral(axis))) > 1.0 - 1e-9);
    }
}

TEST_CASE(vertices_lie_on_the_surface_with_upward_normals)
{
    const ellipsoid e;
    const tile_id t{3, 6, 2};
    const std::uint32_t grid = 17;
    std::vector<float> heights(static_cast<std::size_t>(grid) * grid);
    for (std::size_t k = 0; k < heights.size(); ++k)
    {
        heights[k] = 100.0f + static_cast<float>(k % 7);
    }
    const built b = build(t, grid, heights, -500.0);
    REQUIRE(b.vertices.size() == tile_mesh_vertex_count(grid));

    const degree_region r = tile_bounds(t);
    const double step = tile_span(t.level) / (grid - 1);
    for (std::uint32_t j = 0; j < grid; ++j)
    {
        for (std::uint32_t i = 0; i < grid; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * grid + i;
            const geodetic g{radians(r.north - j * step), radians(r.west + i * step), heights[k]};
            const tile_vertex& v = b.vertices[k];
            // A level-3 tile is 2500 km across; float offsets from its centre are good to
            // well under a metre.
            CHECK(length(position_of(v, b.centre) - e.to_ecef(g)) < 0.5);
            CHECK_EQ(v.height, heights[k]);
            const dvec3 up = ellipsoid::surface_normal(g.latitude, g.longitude);
            CHECK(dot(decode_octahedral(v.normal), up) > 0.999);
        }
    }
    // Texture coordinates span the tile: (0, 0) at the north-west, (1, 1) at the south-east.
    CHECK_EQ(b.vertices[0].uv[0], 0);
    CHECK_EQ(b.vertices[0].uv[1], 0);
    CHECK_EQ(b.vertices[grid * grid - 1].uv[0], 65535);
    CHECK_EQ(b.vertices[grid * grid - 1].uv[1], 65535);

    // Skirt vertices hang straight below their edge vertices, with the same attributes.
    for (std::uint32_t k = 0; k < 4 * (grid - 1); ++k)
    {
        const tile_vertex& above = b.vertices[tile_mesh_boundary_vertex(grid, k)];
        const tile_vertex& skirt = b.vertices[grid * grid + k];
        CHECK_EQ(skirt.height, -500.0f);
        CHECK(skirt.uv == above.uv);
        CHECK(skirt.normal == above.normal);
        const dvec3 drop = position_of(above, b.centre) - position_of(skirt, b.centre);
        CHECK_NEAR(length(drop), above.height + 500.0, 1.0);
    }
}

TEST_CASE(triangles_face_outwards)
{
    for (const tile_id& t : {tile_id{3, 6, 2}, tile_id{1, 0, 0}, tile_id{5, 40, 31}})
    {
        const std::uint32_t grid = 9;
        const std::vector<float> heights(static_cast<std::size_t>(grid) * grid, 0.0f);
        const built b = build(t, grid, heights, -1000.0);
        REQUIRE(b.indices.size() == tile_mesh_index_count(grid));
        for (const std::uint32_t index : b.indices)
        {
            CHECK(index < b.vertices.size());
        }
        const std::size_t grid_indices = 6 * (grid - 1) * (grid - 1);
        for (std::size_t n = 0; n < b.indices.size(); n += 3)
        {
            const dvec3 p0 = position_of(b.vertices[b.indices[n]], b.centre);
            const dvec3 p1 = position_of(b.vertices[b.indices[n + 1]], b.centre);
            const dvec3 p2 = position_of(b.vertices[b.indices[n + 2]], b.centre);
            const dvec3 face = cross(p1 - p0, p2 - p0);
            if (!(length(face) > 1e-3))
            {
                continue; // Collapsed at a pole.
            }
            const dvec3 middle = (p0 + p1 + p2) * (1.0 / 3.0);
            if (n < grid_indices)
            {
                CHECK(dot(face, middle) > 0.0); // Away from the centre of the Earth.
            }
            else
            {
                // Skirts face away from the tile's centre, along the surface.
                const dvec3 up = middle * (1.0 / length(middle));
                dvec3 out = middle - b.centre;
                out = out - up * dot(out, up);
                CHECK(dot(face, out) > 0.0);
            }
        }
    }
}

TEST_CASE(pole_tiles_have_finite_normals)
{
    const std::uint32_t grid = 9;
    const std::vector<float> heights(static_cast<std::size_t>(grid) * grid, 0.0f);
    const built b = build({1, 0, 0}, grid, heights, -100.0);
    for (const tile_vertex& v : b.vertices)
    {
        const dvec3 n = decode_octahedral(v.normal);
        CHECK(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
        CHECK_NEAR(length(n), 1.0, 1e-6);
    }
    // The north row is the pole: its normals point north.
    CHECK(decode_octahedral(b.vertices[grid / 2].normal).z > 0.999);
}

TEST_CASE(samples_match_the_grid_vertices)
{
    const ellipsoid e;
    const std::uint32_t grid = 17;
    const tile_id t{8, 270, 60};
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(0.0f, 900.0f);
    std::vector<float> heights(static_cast<std::size_t>(grid) * grid);
    for (float& h : heights)
    {
        h = d(rng);
    }
    const built b = build(t, grid, heights, -100.0);
    std::vector<dvec3> positions(heights.size());
    std::vector<tile_sample> samples(heights.size());
    build_tile_positions(e, t, grid, heights, positions);
    build_tile_samples(t, grid, heights, positions, samples);
    for (std::size_t k = 0; k < samples.size(); ++k)
    {
        CHECK_EQ(samples[k].height, heights[k]);
        CHECK_EQ(samples[k].normal[0], b.vertices[k].normal[0]);
        CHECK_EQ(samples[k].normal[1], b.vertices[k].normal[1]);
    }
}

TEST_CASE(smooth_tiles_coarsen_and_rough_ones_do_not)
{
    const ellipsoid e;
    const std::uint32_t grid = 129;
    const tile_id t{3, 5, 2};
    std::vector<float> heights(static_cast<std::size_t>(grid) * grid, 0.0f);
    std::vector<dvec3> positions(heights.size());
    build_tile_positions(e, t, grid, heights, positions);
    // On the ellipsoid, a cell of 16 samples (2.8 degrees) sags by about 3.8 km in the middle
    // of its diagonal, and one of 8 samples by a quarter of that. A stride of 16 leaves the 9 x 9
    // vertices of the smallest grid.
    CHECK_EQ(tile_mesh_stride(grid, positions, 6000.0), 16u);
    CHECK_EQ(tile_mesh_stride(grid, positions, 2000.0), 8u);
    CHECK_EQ(tile_mesh_stride(grid, positions, 1.0), 1u);

    // A peak between the vertices of every coarser grid keeps every sample.
    heights[static_cast<std::size_t>(61) * grid + 37] = 3000.0f;
    build_tile_positions(e, t, grid, heights, positions);
    CHECK_EQ(tile_mesh_stride(grid, positions, 2000.0), 1u);

    // A grid too small to coarsen.
    const std::uint32_t small = 9;
    std::vector<dvec3> few(static_cast<std::size_t>(small) * small);
    build_tile_positions(e, t, small, std::vector<float>(few.size(), 0.0f), few);
    CHECK_EQ(tile_mesh_stride(small, few, 1e6), 1u);
}

TEST_CASE(decimating_keeps_every_stride_th_row_and_column)
{
    const std::uint32_t grid = 17;
    std::vector<float> heights(static_cast<std::size_t>(grid) * grid);
    for (std::size_t k = 0; k < heights.size(); ++k)
    {
        heights[k] = static_cast<float>(k);
    }
    const std::vector<float> original = heights;
    CHECK_EQ(tile_heights_decimate(grid, 4, heights), 5u);
    for (std::uint32_t j = 0; j < 5; ++j)
    {
        for (std::uint32_t i = 0; i < 5; ++i)
        {
            CHECK_EQ(
                heights[static_cast<std::size_t>(j) * 5 + i],
                original[static_cast<std::size_t>(j) * 4 * grid + static_cast<std::size_t>(i) * 4]);
        }
    }
}
