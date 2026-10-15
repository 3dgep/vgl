// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Test packs built in memory with the baker's writers, the heights they should give, and an
// allocator that counts and fails allocations. Shared by the context and defaults tests.

#ifndef VGL_TESTS_TEST_PACKS_HPP
#define VGL_TESTS_TEST_PACKS_HPP

#include "decoders/elevation_codec.hpp"
#include "foundation/allocator.hpp"
#include "foundation/math.hpp"
#include "pack_writer.hpp"
#include "providers/pack_reader.hpp"
#include "tiling/tile.hpp"

#include <vgl/foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vgl::test
{

using namespace vgl::internal;
using namespace vgl::internal::pack;
namespace bake = vgl::bake;

inline constexpr int zstd_level = 3;
inline constexpr std::uint32_t pack_tile_size = 17;
inline constexpr double geoid_height = 40.0;

// A pack in memory: files by path relative to the pack, and the samples of every tile.
struct test_pack
{
    std::map<std::string, std::vector<std::byte>> files;
    std::map<std::uint64_t, std::vector<std::int16_t>> samples; // By tile key.
    std::map<std::uint64_t, double> offsets;
    std::vector<double> steps;
};

inline std::vector<std::int16_t> terrain(std::uint32_t size, std::uint64_t seed)
{
    std::vector<std::int16_t> q(static_cast<std::size_t>(size) * size);
    for (std::uint32_t j = 0; j < size; ++j)
    {
        for (std::uint32_t i = 0; i < size; ++i)
        {
            const double v =
                2000.0 * std::sin(i * 0.05 + static_cast<double>(seed % 1000)) * std::cos(j * 0.07);
            q[static_cast<std::size_t>(j) * size + i] = static_cast<std::int16_t>(std::lround(v));
        }
    }
    return q;
}

// Levels 0 to 3, shard level 2. Tiles exist over the region of level-2 tiles (2, 3, 1) and
// (2, 4, 1), and all of levels 0 and 1. The level-3 tiles in column 8 have nodata in their top
// row. A geoid pack is the constant geoid_height. offset_shift moves every elevation by that
// many steps, to tell packs apart.
inline test_pack make_pack(layer_type type = layer_type::elevation, int offset_shift = 0)
{
    test_pack p;
    bake::manifest_contents m;
    m.layer.type = type;
    m.layer.tile_size = pack_tile_size;
    m.layer.min_level = 0;
    m.layer.max_level = 3;
    m.layer.shard_level = 2;
    m.layer.semi_major_axis = 6378137.0;
    m.layer.inverse_flattening = 298.257223563;
    m.layer.west = -180.0;
    m.layer.south = -90.0;
    m.layer.east = 180.0;
    m.layer.north = 90.0;
    m.height_steps = {0.5, 0.5, 0.25, 0.125};
    p.steps = m.height_steps;

    const auto encode = [&](const tile_id& t, std::uint8_t child_mask) {
        bake::height_tile h;
        h.type = type;
        h.size = pack_tile_size;
        h.scale = m.height_steps[t.level];
        if (type == layer_type::geoid)
        {
            h.q.assign(static_cast<std::size_t>(pack_tile_size) * pack_tile_size, 0);
            h.offset = geoid_height;
        }
        else
        {
            h.q = terrain(pack_tile_size, tile_key(t));
            if (t.level == 3 && t.x == 8)
            {
                std::fill_n(h.q.begin(), pack_tile_size, elevation_nodata);
            }
            h.offset = (100.0 + offset_shift) * h.scale;
        }
        h.child_mask = child_mask;
        h.child_min = {-1.0f, -2.0f, -3.0f, -4.0f};
        h.child_max = {1.0f, 2.0f, 3.0f, 4.0f};
        p.samples[tile_key(t)] = h.q;
        p.offsets[tile_key(t)] = h.offset;
        return bake::encode_height_tile(h, zstd_level);
    };

    bake::shard_writer base(type, 0, 0, 0, true);
    base.add({0, 0, 0}, encode({0, 0, 0}, 0xF));
    base.add({0, 1, 0}, encode({0, 1, 0}, 0xF));
    std::vector<tile_id> level1;
    for (std::uint32_t y = 0; y < 2; ++y)
    {
        for (std::uint32_t x = 0; x < 4; ++x)
        {
            level1.push_back({1, x, y});
        }
    }
    std::sort(level1.begin(), level1.end(), [](const tile_id& a, const tile_id& b) {
        return tile_key(a) < tile_key(b);
    });
    for (const tile_id& t : level1)
    {
        // (1, 1, 0) has child 3, (2, 3, 1); (1, 2, 0) has child 2, (2, 4, 1).
        const std::uint8_t mask = t.y != 0 ? 0 : (t.x == 1 ? 0x8 : (t.x == 2 ? 0x4 : 0));
        base.add(t, encode(t, mask));
    }
    const bake::shard_writer::output base_out = base.finish(zstd_level);
    m.has_base = true;
    m.base_file_size = base_out.bytes.size();
    m.base_digest = base_out.digest;
    std::array<char, max_path_size> path{};
    static_cast<void>(base_shard_path(base_out.digest, path));
    p.files[path.data()] = base_out.bytes;

    for (const std::uint32_t root_x : {3u, 4u})
    {
        bake::shard_writer shard(type, 2, root_x, 1, false);
        const tile_id root{2, root_x, 1};
        shard.add(root, encode(root, 0xF));
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            const tile_id child = tile_child(root, c);
            shard.add(child, encode(child, 0));
        }
        const bake::shard_writer::output out = shard.finish(zstd_level);
        m.shards.push_back({root_x, 1, out.bytes.size(), out.digest});
        static_cast<void>(shard_path(root_x, 1, out.digest, path));
        p.files[path.data()] = out.bytes;
    }
    p.files[std::string(manifest_file_name)] = bake::write_manifest(m, zstd_level);
    return p;
}

// The height a pack gives at a position from one tile, in degrees.
inline double expected_height(const test_pack& p, const tile_id& t, double lat, double lon)
{
    const std::vector<std::int16_t>& q = p.samples.at(tile_key(t));
    const double offset = p.offsets.at(tile_key(t));
    const double scale = p.steps[t.level];
    return tile_interpolate(t, pack_tile_size, lat, lon, [&](std::uint32_t i, std::uint32_t j) {
        return offset + scale * q[static_cast<std::size_t>(j) * pack_tile_size + i];
    });
}

// A position inside a tile, in degrees, away from its centre so that interpolation matters.
struct position
{
    double lat;
    double lon;
};

inline position inside(const tile_id& t, double u = 0.6, double v = 0.3)
{
    const degree_region b = tile_bounds(t);
    return {b.south + v * (b.north - b.south), b.west + u * (b.east - b.west)};
}

constexpr double to_radians(double d)
{
    return d * pi / 180.0;
}

// An allocator that counts live allocations and can fail one of them.
struct test_allocator
{
    std::atomic<int> live{0};
    std::atomic<long> count{0};
    long fail_at = -1; // Index of the allocation to fail, or -1.
    std::atomic<bool> failed{false};
    vgl_allocator_interface table{VGL_ALLOCATOR_INTERFACE_VERSION, 0, this, &allocate, &deallocate};

    static void* allocate(void* user_data, std::size_t size, std::size_t alignment)
    {
        auto* self = static_cast<test_allocator*>(user_data);
        if (self->count++ == self->fail_at)
        {
            self->failed = true;
            return nullptr;
        }
        ++self->live;
        return allocator().allocate(size, alignment);
    }

    static void deallocate(void* user_data, void* memory, std::size_t size, std::size_t alignment)
    {
        --static_cast<test_allocator*>(user_data)->live;
        allocator().deallocate(memory, size, alignment);
    }
};

} // namespace vgl::test

#endif // VGL_TESTS_TEST_PACKS_HPP
