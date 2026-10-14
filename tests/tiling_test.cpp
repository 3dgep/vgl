// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tiling scheme tests (docs/pack-format.md, section 2).

#include "tiling/tile.hpp"
#include "vgl_test.hpp"

#include <cmath>
#include <ostream>
#include <random>

namespace vgl::internal
{

std::ostream& operator<<(std::ostream& os, const tile_id& t)
{
    return os << '(' << t.level << ", " << t.x << ", " << t.y << ')';
}

} // namespace vgl::internal

namespace
{

using namespace vgl::internal;

tile_id random_tile(std::mt19937_64& rng,
                    std::uint32_t min_level = 0,
                    std::uint32_t max_level = max_tile_level)
{
    const auto level = std::uniform_int_distribution<std::uint32_t>(min_level, max_level)(rng);
    const auto x = std::uniform_int_distribution<std::uint32_t>(0, tile_columns(level) - 1)(rng);
    const auto y = std::uniform_int_distribution<std::uint32_t>(0, tile_rows(level) - 1)(rng);
    return {level, x, y};
}

} // namespace

TEST_CASE(level_dimensions)
{
    CHECK_EQ(tile_columns(0), 2u);
    CHECK_EQ(tile_rows(0), 1u);
    CHECK_EQ(tile_columns(12), 8192u);
    CHECK_EQ(tile_rows(12), 4096u);
    CHECK_EQ(tile_columns(max_tile_level), 1u << 29);
    CHECK_EQ(tile_span(0), 180.0);
    CHECK_EQ(tile_span(12), 0.0439453125);
}

TEST_CASE(root_tile_bounds)
{
    CHECK(tile_bounds({0, 0, 0}) == (degree_region{-180.0, -90.0, 0.0, 90.0}));
    CHECK(tile_bounds({0, 1, 0}) == (degree_region{0.0, -90.0, 180.0, 90.0}));
    // Rows count southwards from the north pole.
    CHECK(tile_bounds({1, 3, 0}) == (degree_region{90.0, 0.0, 180.0, 90.0}));
    CHECK(tile_bounds({1, 0, 1}) == (degree_region{-180.0, -90.0, -90.0, 0.0}));
}

TEST_CASE(validity)
{
    CHECK(tile_is_valid({0, 1, 0}));
    CHECK(!tile_is_valid({0, 2, 0}));
    CHECK(!tile_is_valid({0, 0, 1}));
    CHECK(tile_is_valid(
        {max_tile_level, tile_columns(max_tile_level) - 1, tile_rows(max_tile_level) - 1}));
    CHECK(!tile_is_valid({max_tile_level + 1, 0, 0}));
}

TEST_CASE(key_matches_specification_example)
{
    // docs/pack-format.md, section 2.2.
    CHECK_EQ(tile_key({7, 181, 37}), 0x1C00000000004D33ull);
    CHECK_EQ(tile_from_key(0x1C00000000004D33ull), (tile_id{7, 181, 37}));
}

TEST_CASE(key_round_trip)
{
    std::mt19937_64 rng(4);
    for (int i = 0; i < 100000; ++i)
    {
        const tile_id t = random_tile(rng);
        const std::uint64_t key = tile_key(t);
        CHECK(tile_key_is_valid(key));
        CHECK_EQ(tile_from_key(key), t);
    }
    // The extreme corner of the deepest level.
    const tile_id corner{
        max_tile_level, tile_columns(max_tile_level) - 1, tile_rows(max_tile_level) - 1};
    CHECK_EQ(tile_from_key(tile_key(corner)), corner);
}

TEST_CASE(invalid_keys_are_rejected)
{
    CHECK(!tile_key_is_valid(tile_key({0, 2, 0})));
    CHECK(!tile_key_is_valid(static_cast<std::uint64_t>(29) << tile_key_level_shift));
    // A level-1 key with a column bit beyond the level's range.
    CHECK(!tile_key_is_valid(tile_key({1, 0, 0}) | (1ull << 8)));
}

TEST_CASE(keys_order_by_level_then_z_curve)
{
    // Children follow each other in key order, and a parent's children are contiguous.
    std::mt19937_64 rng(5);
    for (int i = 0; i < 10000; ++i)
    {
        const tile_id t = random_tile(rng, 0, max_tile_level - 1);
        const std::uint64_t first = tile_key(tile_child(t, 0));
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            CHECK_EQ(tile_key(tile_child(t, c)), first + c);
        }
        CHECK(tile_key(t) < first);
    }
}

TEST_CASE(parent_child_relations)
{
    std::mt19937_64 rng(6);
    for (int i = 0; i < 10000; ++i)
    {
        const tile_id t = random_tile(rng, 0, max_tile_level - 1);
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            const tile_id child = tile_child(t, c);
            CHECK(tile_is_valid(child));
            CHECK_EQ(tile_parent(child), t);
            CHECK_EQ(tile_child_index(child), c);
            CHECK(tile_is_in_subtree(child, t));
        }
        CHECK_EQ(tile_ancestor(t, 0).level, 0u);
        CHECK(tile_is_in_subtree(t, tile_ancestor(t, 0)));
    }

    // Child numbering: 0 north-west, 1 north-east, 2 south-west, 3 south-east.
    const degree_region parent = tile_bounds({1, 2, 0});
    const degree_region nw = tile_bounds(tile_child({1, 2, 0}, 0));
    const degree_region se = tile_bounds(tile_child({1, 2, 0}, 3));
    CHECK_EQ(nw.west, parent.west);
    CHECK_EQ(nw.north, parent.north);
    CHECK_EQ(se.east, parent.east);
    CHECK_EQ(se.south, parent.south);
    CHECK(!tile_is_in_subtree({2, 0, 0}, {1, 1, 0}));
}

TEST_CASE(neighbours_wrap_across_antimeridian)
{
    CHECK_EQ(tile_east({3, 15, 2}), (tile_id{3, 0, 2}));
    CHECK_EQ(tile_west({3, 0, 2}), (tile_id{3, 15, 2}));
    CHECK_EQ(tile_east({0, 0, 0}), (tile_id{0, 1, 0}));
}

TEST_CASE(shared_borders_are_bit_identical)
{
    // The east column of a tile and the west column of its eastern neighbour lie on the same
    // meridian, and the rows shared with the southern neighbour on the same parallel
    // (docs/pack-format.md, section 2.3).
    constexpr std::uint32_t size = 129;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 2000; ++i)
    {
        const tile_id t = random_tile(rng, 0, 20);
        const tile_id east = tile_east(t);
        for (std::uint32_t j = 0; j < size; ++j)
        {
            const degree_position a = elevation_sample_position(t, size, size - 1, j);
            const degree_position b = elevation_sample_position(east, size, 0, j);
            CHECK_EQ(a.latitude, b.latitude);
            if (east.x == 0)
            {
                // Across the antimeridian, 180 and -180 are the same meridian.
                CHECK_EQ(a.longitude, 180.0);
                CHECK_EQ(b.longitude, -180.0);
            }
            else
            {
                CHECK_EQ(a.longitude, b.longitude);
            }
        }
        if (t.y + 1 < tile_rows(t.level))
        {
            const tile_id south{t.level, t.x, t.y + 1};
            for (std::uint32_t k = 0; k < size; ++k)
            {
                CHECK(elevation_sample_position(t, size, k, size - 1) ==
                      elevation_sample_position(south, size, k, 0));
            }
        }
    }
}

TEST_CASE(child_samples_coincide_with_parent_samples)
{
    constexpr std::uint32_t size = 129;
    const tile_id parent{5, 37, 11};
    for (std::uint32_t c = 0; c < 4; ++c)
    {
        const tile_id child = tile_child(parent, c);
        const std::uint32_t i0 = (c & 1u) * (size - 1) / 2;
        const std::uint32_t j0 = (c >> 1) * (size - 1) / 2;
        for (std::uint32_t j = 0; j < size; j += 2)
        {
            for (std::uint32_t i = 0; i < size; i += 2)
            {
                CHECK(elevation_sample_position(child, size, i, j) ==
                      elevation_sample_position(parent, size, i0 + i / 2, j0 + j / 2));
            }
        }
    }
}

TEST_CASE(pole_rows)
{
    constexpr std::uint32_t size = 65;
    CHECK_EQ(elevation_sample_position({4, 3, 0}, size, 10, 0).latitude, 90.0);
    CHECK_EQ(elevation_sample_position({4, 3, tile_rows(4) - 1}, size, 10, size - 1).latitude,
             -90.0);
}

TEST_CASE(elevation_tile_sizes)
{
    CHECK(elevation_tile_size_is_valid(5));
    CHECK(elevation_tile_size_is_valid(129));
    CHECK(elevation_tile_size_is_valid(1025));
    CHECK(!elevation_tile_size_is_valid(3));
    CHECK(!elevation_tile_size_is_valid(128));
    CHECK(!elevation_tile_size_is_valid(130));
    CHECK(!elevation_tile_size_is_valid(2049));
}

TEST_CASE(imagery_pixel_centres)
{
    const tile_id t{2, 3, 1};
    const degree_region b = tile_bounds(t);
    const double step = tile_span(2) / 256;
    CHECK(imagery_pixel_center(t, 256, 0, 0, 0) ==
          (degree_position{b.west + step / 2, b.north - step / 2}));
    // With a border, image pixel (1, 1) is the first inner pixel and pixel (0, 0) lies outside.
    CHECK(imagery_pixel_center(t, 256, 1, 1, 1) == imagery_pixel_center(t, 256, 0, 0, 0));
    CHECK(imagery_pixel_center(t, 256, 1, 0, 0).longitude < b.west);
    CHECK(imagery_pixel_center(t, 256, 0, 255, 255) ==
          (degree_position{b.east - step / 2, b.south + step / 2}));
}

TEST_CASE(tile_containing_a_position)
{
    CHECK_EQ(tile_at(90.0, -180.0, 3), (tile_id{3, 0, 0}));
    CHECK_EQ(tile_at(-90.0, 179.999, 3), (tile_id{3, 15, 7}));
    // Longitude 180 is the antimeridian and belongs to column 0.
    CHECK_EQ(tile_at(10.0, 180.0, 3), (tile_id{3, 0, 3}));
    CHECK_EQ(tile_at(10.0, 540.0, 3), (tile_id{3, 0, 3}));
    CHECK_EQ(tile_at(10.0, -200.0, 3), tile_at(10.0, 160.0, 3));
    // Each tile owns its west and north edges.
    CHECK_EQ(tile_at(0.0, 0.0, 1), (tile_id{1, 2, 1}));
    CHECK_EQ(tile_at(0.0, -0.000001, 1), (tile_id{1, 1, 1}));
    CHECK_EQ(tile_at(1e-12, 0.0, 1), (tile_id{1, 2, 0}));

    std::mt19937_64 rng(8);
    std::uniform_real_distribution<double> lat(-90.0, 90.0);
    std::uniform_real_distribution<double> lon(-180.0, 180.0);
    for (int i = 0; i < 10000; ++i)
    {
        const double la = lat(rng);
        const double lo = lon(rng);
        const tile_id t = tile_at(la, lo, static_cast<std::uint32_t>(i % 20));
        const degree_region b = tile_bounds(t);
        CHECK(tile_is_valid(t));
        CHECK(la >= b.south && la <= b.north && lo >= b.west && lo < b.east);
    }
}

TEST_CASE(shard_of_a_tile)
{
    CHECK(tile_shard({3, 5, 2}, 6) == (shard_location{true, {}}));
    CHECK(tile_shard({6, 40, 20}, 6) == (shard_location{false, {6, 40, 20}}));
    CHECK(tile_shard({12, 40 * 64 + 63, 20 * 64}, 6) == (shard_location{false, {6, 40, 20}}));
}

TEST_CASE(tile_bounding_box_contains_corners)
{
    const ellipsoid wgs84;
    const tile_id t{9, 600, 100};
    const oriented_box box = tile_bounding_box(wgs84, t, -100.0, 2500.0);
    const degree_region b = tile_bounds(t);
    for (const double lat : {b.south, b.north})
    {
        for (const double lon : {b.west, b.east})
        {
            for (const double h : {-100.0, 2500.0})
            {
                CHECK(box.contains(wgs84.to_ecef({radians(lat), radians(lon), h}), 1e-6));
            }
        }
    }
}

namespace
{

// A field that is bilinear in longitude and latitude, which bilinear interpolation must
// reproduce exactly, sampled on a tile's grid.
double bilinear_field(double latitude, double longitude)
{
    return 3.0 + 0.25 * longitude - 0.5 * latitude + 0.001 * longitude * latitude;
}

auto field_samples(const tile_id& t, std::uint32_t size)
{
    return [t, size](std::uint32_t i, std::uint32_t j) {
        const degree_position p = elevation_sample_position(t, size, i, j);
        return bilinear_field(p.latitude, p.longitude);
    };
}

} // namespace

TEST_CASE(interpolation_reproduces_samples_and_bilinear_fields)
{
    constexpr std::uint32_t size = 17;
    const tile_id t{4, 13, 5};
    const degree_region b = tile_bounds(t);
    const auto sample = field_samples(t, size);

    // At sample positions, including the corners, the samples themselves come back.
    for (const std::uint32_t i : {0u, 7u, size - 1})
    {
        for (const std::uint32_t j : {0u, 3u, size - 1})
        {
            const degree_position p = elevation_sample_position(t, size, i, j);
            CHECK_NEAR(
                tile_interpolate(t, size, p.latitude, p.longitude, sample), sample(i, j), 1e-12);
        }
    }

    // Between samples, a field that is bilinear within each cell is reproduced: the product
    // term is bilinear too, because each cell is axis-aligned.
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> lat(b.south, b.north);
    std::uniform_real_distribution<double> lon(b.west, b.east);
    for (int k = 0; k < 10000; ++k)
    {
        const double la = lat(rng);
        const double lo = lon(rng);
        CHECK_NEAR(tile_interpolate(t, size, la, lo, sample), bilinear_field(la, lo), 1e-11);
    }
}

TEST_CASE(interpolation_is_continuous_across_tile_edges)
{
    constexpr std::uint32_t size = 9;
    // Values that differ per tile in the interior but agree on shared borders, like baked
    // tiles: each tile samples the same smooth field at its own grid.
    const auto field = [](double latitude, double longitude) {
        return std::sin(radians(latitude) * 3.0) * std::cos(radians(longitude) * 2.0);
    };
    const auto samples = [&](const tile_id& t) {
        return [t, field](std::uint32_t i, std::uint32_t j) {
            const degree_position p = elevation_sample_position(t, size, i, j);
            return field(p.latitude, p.longitude);
        };
    };

    const tile_id a{3, 5, 2};
    const tile_id east = tile_east(a);
    const tile_id south{3, 5, 3};
    const degree_region b = tile_bounds(a);
    for (int k = 0; k <= 50; ++k)
    {
        const double lat = b.south + (b.north - b.south) * k / 50.0;
        CHECK_EQ(tile_interpolate(a, size, lat, b.east, samples(a)),
                 tile_interpolate(east, size, lat, b.east, samples(east)));
        const double lon = b.west + (b.east - b.west) * k / 50.0;
        CHECK_EQ(tile_interpolate(a, size, b.south, lon, samples(a)),
                 tile_interpolate(south, size, b.south, lon, samples(south)));
    }

    // Across the antimeridian, 180 in the last column equals -180 in the first.
    const tile_id last{3, tile_columns(3) - 1, 2};
    const tile_id first{3, 0, 2};
    for (int k = 0; k <= 50; ++k)
    {
        const double lat = b.south + (b.north - b.south) * k / 50.0;
        CHECK_NEAR(tile_interpolate(last, size, lat, 180.0, samples(last)),
                   tile_interpolate(first, size, lat, -180.0, samples(first)),
                   1e-15);
        // Either spelling of the antimeridian works for either tile.
        CHECK_EQ(tile_interpolate(last, size, lat, -180.0, samples(last)),
                 tile_interpolate(last, size, lat, 180.0, samples(last)));
        CHECK_EQ(tile_interpolate(first, size, lat, 180.0, samples(first)),
                 tile_interpolate(first, size, lat, -180.0, samples(first)));
    }
}

TEST_CASE(interpolation_clamps_to_the_tile)
{
    constexpr std::uint32_t size = 5;
    const tile_id t{2, 1, 1};
    const degree_region b = tile_bounds(t);
    const auto sample = field_samples(t, size);
    CHECK_EQ(tile_interpolate(t, size, b.north + 1.0, b.west - 1.0, sample), sample(0, 0));
    CHECK_EQ(tile_interpolate(t, size, b.south - 1.0, b.east + 1.0, sample),
             sample(size - 1, size - 1));

    // At a pole, every sample of the pole row is the same point; interpolation along the row
    // returns that value when the baker writes identical values, as the format requires.
    const tile_id polar{2, 3, 0};
    const auto pole_samples = [](std::uint32_t /*i*/, std::uint32_t j) {
        return j == 0 ? 42.0 : 0.0;
    };
    CHECK_EQ(tile_interpolate(polar, size, 90.0, -80.0, pole_samples), 42.0);
    CHECK_EQ(tile_interpolate(polar, size, 90.0, -46.0, pole_samples), 42.0);
}
