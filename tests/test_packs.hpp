// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Test packs built in memory with the baker's writers, the heights they should give, an
// allocator that counts and fails allocations, and hosts that serve packs from memory and run
// tasks on demand. Shared by the context, render and defaults tests.

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
#include <utility>
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
// many steps, to tell packs apart. The pack credits "<credit> A" and "<credit> B".
inline test_pack make_pack(layer_type type = layer_type::elevation,
                           int offset_shift = 0,
                           const std::string& credit = "Test data")
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
    m.metadata = {
        {"name", "Test pack"}, {"attribution", credit + " A"}, {"attribution", credit + " B"}};
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

// A PNG file of an 8-bit RGBA image (rows from the top), written with stored (uncompressed)
// deflate blocks, so that tests need no compression library.
inline std::vector<std::byte>
encode_test_png(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& rgba)
{
    std::vector<std::byte> out;
    const auto put8 = [&](std::uint32_t v) { out.push_back(static_cast<std::byte>(v & 0xFFu)); };
    const auto put32 = [&](std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            put8(v >> shift);
        }
    };
    const auto crc32 = [](const std::byte* data, std::size_t size) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < size; ++i)
        {
            c ^= static_cast<std::uint32_t>(data[i]);
            for (int k = 0; k < 8; ++k)
            {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
        }
        return c ^ 0xFFFFFFFFu;
    };
    const auto chunk = [&](const char* type, const std::vector<std::byte>& body) {
        put32(static_cast<std::uint32_t>(body.size()));
        const std::size_t start = out.size();
        for (int i = 0; i < 4; ++i)
        {
            put8(static_cast<std::uint8_t>(type[i]));
        }
        out.insert(out.end(), body.begin(), body.end());
        put32(crc32(out.data() + start, out.size() - start));
    };
    constexpr std::array<std::uint8_t, 8> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (const std::uint8_t b : signature)
    {
        put8(b);
    }
    std::vector<std::byte> header;
    for (const std::uint32_t v : {width, height})
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            header.push_back(static_cast<std::byte>((v >> shift) & 0xFFu));
        }
    }
    // 8 bits, RGBA, deflate, no filter, no interlace.
    constexpr std::array<std::uint8_t, 5> format = {8, 6, 0, 0, 0};
    for (const std::uint8_t b : format)
    {
        header.push_back(static_cast<std::byte>(b));
    }
    chunk("IHDR", header);

    // Each row: filter type 0, then the row.
    std::vector<std::byte> raw;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(std::byte{0});
        const std::size_t row = static_cast<std::size_t>(y) * width * 4;
        for (std::size_t k = 0; k < std::size_t{width} * 4; ++k)
        {
            raw.push_back(static_cast<std::byte>(rgba[row + k]));
        }
    }
    std::vector<std::byte> z = {std::byte{0x78}, std::byte{0x01}};
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;)
    {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(static_cast<std::byte>(last ? 1 : 0));
        for (const std::size_t v : {n, n ^ 0xFFFFu})
        {
            z.push_back(static_cast<std::byte>(v & 0xFFu));
            z.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
        }
        z.insert(z.end(),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last)
        {
            break;
        }
    }
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::byte v : raw)
    {
        a = (a + static_cast<std::uint32_t>(v)) % 65521u;
        b = (b + a) % 65521u;
    }
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        z.push_back(static_cast<std::byte>((((b << 16) | a) >> shift) & 0xFFu));
    }
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

// Imagery tiles of the test packs: 64 pixels, the smallest the format allows, with a border
// of 1, so 66 x 66 images.
inline constexpr std::uint32_t imagery_tile_size = 64;
inline constexpr std::uint32_t imagery_border = 1;
inline constexpr std::uint32_t imagery_side = imagery_tile_size + 2 * imagery_border;

// The colour of an imagery pixel: red and green give its column and row in the image, the
// border included, and blue tells the tile from others.
inline std::array<std::uint8_t, 4> imagery_pixel(const tile_id& t, std::uint32_t i, std::uint32_t j)
{
    return {static_cast<std::uint8_t>(i * 3),
            static_cast<std::uint8_t>(j * 3),
            static_cast<std::uint8_t>((t.level * 64 + t.x * 8 + t.y) & 0xFFu),
            255};
}

// An imagery pack with the tiles of make_pack, as PNG, crediting "<credit>".
inline test_pack make_imagery_pack(const std::string& credit = "Test imagery")
{
    test_pack p;
    bake::manifest_contents m;
    m.layer.type = layer_type::imagery;
    m.layer.tile_size = imagery_tile_size;
    m.layer.border = imagery_border;
    m.layer.height_reference = height_reference_none;
    m.layer.min_level = 0;
    m.layer.max_level = 3;
    m.layer.shard_level = 2;
    m.layer.semi_major_axis = 6378137.0;
    m.layer.inverse_flattening = 298.257223563;
    m.layer.west = -180.0;
    m.layer.south = -90.0;
    m.layer.east = 180.0;
    m.layer.north = 90.0;
    m.metadata = {{"name", "Test imagery"}, {"attribution", credit}};

    const auto encode = [&](const tile_id& t, std::uint8_t child_mask) {
        std::vector<std::uint8_t> rgba(std::size_t{imagery_side} * imagery_side * 4);
        for (std::uint32_t j = 0; j < imagery_side; ++j)
        {
            for (std::uint32_t i = 0; i < imagery_side; ++i)
            {
                const auto c = imagery_pixel(t, i, j);
                const auto at =
                    static_cast<std::ptrdiff_t>((std::size_t{j} * imagery_side + i) * 4);
                std::copy(c.begin(), c.end(), rgba.begin() + at);
            }
        }
        const std::vector<std::byte> png = encode_test_png(imagery_side, imagery_side, rgba);
        return bake::encode_imagery_tile(imagery_encoding::png, child_mask, png);
    };

    bake::shard_writer base(layer_type::imagery, 0, 0, 0, true);
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
        bake::shard_writer shard(layer_type::imagery, 2, root_x, 1, false);
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

// Serves packs from memory. Completes reads inside read(), or queues them for complete_all().
struct memory_io
{
    std::map<std::string, const test_pack*> packs; // By URI.
    bool deferred = false;
    std::vector<std::pair<vgl_io_request*, vgl_io_read>> queued;
    std::vector<std::string> uris; // Of every read started.
    int destroyed = 0;
    vgl_io_interface table{VGL_IO_INTERFACE_VERSION, 0, this, &read, nullptr, &destroy};

    void serve(vgl_io_request* request, const vgl_io_read& r) const
    {
        const std::string uri = r.uri;
        for (const auto& [base, p] : packs)
        {
            if (uri.size() > base.size() && uri.compare(0, base.size(), base) == 0 &&
                uri[base.size()] == '/')
            {
                const auto f = p->files.find(uri.substr(base.size() + 1));
                if (f == p->files.end())
                {
                    break;
                }
                const std::vector<std::byte>& bytes = f->second;
                const std::uint64_t offset = std::min<std::uint64_t>(r.offset, bytes.size());
                const std::uint64_t size = std::min<std::uint64_t>(r.size, bytes.size() - offset);
                static_cast<void>(vgl_io_request_complete(request,
                                                          VGL_RESULT_SUCCESS,
                                                          bytes.data() + offset,
                                                          static_cast<std::size_t>(size)));
                return;
            }
        }
        static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_NOT_FOUND, nullptr, 0));
    }

    void complete_all()
    {
        auto work = std::move(queued);
        queued.clear();
        for (const auto& [request, r] : work)
        {
            serve(request, r);
        }
    }

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* r)
    {
        auto* self = static_cast<memory_io*>(user_data);
        self->uris.emplace_back(r->uri);
        if (self->deferred)
        {
            // The read description lives in the request; copy its URI for later.
            self->queued.emplace_back(request, *r);
            return;
        }
        self->serve(request, *r);
    }

    static void destroy(void* user_data) { ++static_cast<memory_io*>(user_data)->destroyed; }
};

// Runs tasks on demand, on the test's thread.
struct manual_tasks
{
    std::vector<std::pair<vgl_task_fn, void*>> queued;
    vgl_task_interface table{VGL_TASK_INTERFACE_VERSION, 0, this, &submit, nullptr};

    void run_all()
    {
        auto work = std::move(queued);
        queued.clear();
        for (const auto& [fn, data] : work)
        {
            fn(data);
        }
    }

    static vgl_result submit(void* user_data, vgl_task_fn fn, void* data)
    {
        static_cast<manual_tasks*>(user_data)->queued.emplace_back(fn, data);
        return VGL_RESULT_SUCCESS;
    }
};

} // namespace vgl::test

#endif // VGL_TESTS_TEST_PACKS_HPP
