// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Pack format tests: codec, round trips through the writers and readers, and corruption
// (design document, section 12.2).

#include "decoders/elevation_codec.hpp"
#include "foundation/crc32c.hpp"
#include "pack_writer.hpp"
#include "providers/pack_reader.hpp"
#include "vgl_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{

using namespace vgl::internal;
using namespace vgl::internal::pack;
namespace bake = vgl::bake;

constexpr int zstd_level = 3;

std::span<const std::byte> as_bytes(const char* text)
{
    return std::as_bytes(std::span(text, std::strlen(text)));
}

// A synthetic terrain: smooth with some noise, quantized at a step.
std::vector<std::int16_t> terrain(std::uint32_t size, std::uint64_t seed, bool nodata = false)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> noise(-3, 3);
    std::vector<std::int16_t> q(static_cast<std::size_t>(size) * size);
    for (std::uint32_t j = 0; j < size; ++j)
    {
        for (std::uint32_t i = 0; i < size; ++i)
        {
            const double v =
                2000.0 * std::sin(i * 0.05 + static_cast<double>(seed)) * std::cos(j * 0.07);
            q[static_cast<std::size_t>(j) * size + i] =
                static_cast<std::int16_t>(std::lround(v) + noise(rng));
        }
    }
    if (nodata)
    {
        for (std::uint32_t i = 0; i < size; ++i)
        {
            q[i] = elevation_nodata;
        }
    }
    return q;
}

// A small elevation pack in memory: 17-sample tiles, levels 0 to 3, shard level 2. Tiles exist
// over the region of level-2 tiles (2, 3, 1) and (2, 4, 1), and all of levels 0 and 1.
struct test_pack
{
    std::vector<std::byte> manifest;
    std::map<std::string, std::vector<std::byte>> files;        // By path relative to the manifest.
    std::map<std::uint64_t, std::vector<std::int16_t>> samples; // By tile key.
};

constexpr std::uint32_t pack_tile_size = 17;

test_pack make_pack(layer_type type = layer_type::elevation)
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
        {"name", "Test pack"}, {"attribution", "Test data A"}, {"attribution", "Test data B"}};

    const auto encode = [&](const tile_id& t, std::uint8_t child_mask) {
        bake::height_tile h;
        h.type = type;
        h.size = pack_tile_size;
        h.q = terrain(
            pack_tile_size, tile_key(t), type == layer_type::elevation && t.level == 3 && t.x == 8);
        h.scale = m.height_steps[t.level];
        h.offset = 100.0 * h.scale;
        h.child_mask = child_mask;
        h.child_min = {-1.0f, -2.0f, -3.0f, -4.0f};
        h.child_max = {1.0f, 2.0f, 3.0f, 4.0f};
        p.samples[tile_key(t)] = h.q;
        return bake::encode_height_tile(h, zstd_level);
    };

    bake::shard_writer base(type, 0, 0, 0, true);
    base.add({0, 0, 0}, encode({0, 0, 0}, 0xF));
    base.add({0, 1, 0}, encode({0, 1, 0}, 0xF));
    // Tiles go into a shard in key order, which is Morton order within a level.
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
        const std::uint32_t x = t.x;
        const std::uint32_t y = t.y;
        {
            // (1, 1, 0) has child 3, (2, 3, 1); (1, 2, 0) has child 2, (2, 4, 1).
            const std::uint8_t mask = y != 0 ? 0 : (x == 1 ? 0x8 : (x == 2 ? 0x4 : 0));
            base.add({1, x, y}, encode({1, x, y}, mask));
        }
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
    p.manifest = bake::write_manifest(m, zstd_level);
    return p;
}

struct counting_allocator
{
    int live = 0;
    vgl_allocator_interface table{VGL_ALLOCATOR_INTERFACE_VERSION, 0, this, &allocate, &deallocate};

    static void* allocate(void* user_data, std::size_t size, std::size_t alignment)
    {
        ++static_cast<counting_allocator*>(user_data)->live;
        return allocator().allocate(size, alignment);
    }

    static void deallocate(void* user_data, void* memory, std::size_t size, std::size_t alignment)
    {
        --static_cast<counting_allocator*>(user_data)->live;
        allocator().deallocate(memory, size, alignment);
    }
};

} // namespace

TEST_CASE(crc32c_check_value)
{
    CHECK_EQ(crc32c(as_bytes("123456789")), 0xE3069283u);
    CHECK_EQ(crc32c({}), 0u);
    // In pieces.
    CHECK_EQ(crc32c(as_bytes("6789"), crc32c(as_bytes("12345"))), 0xE3069283u);
}

TEST_CASE(zigzag_round_trip)
{
    CHECK_EQ(zigzag(0), 0u);
    CHECK_EQ(zigzag(-1), 1u);
    CHECK_EQ(zigzag(1), 2u);
    CHECK_EQ(zigzag(32767), 65534u);
    CHECK_EQ(zigzag(-32768), 65535u);
    for (int v = -32768; v <= 32767; ++v)
    {
        const auto r = static_cast<std::int16_t>(v);
        if (unzigzag(zigzag(r)) != r)
        {
            CHECK_EQ(unzigzag(zigzag(r)), r);
            break;
        }
    }
}

TEST_CASE(med_predictor)
{
    CHECK_EQ(med_predict(10, 20, 25), 10); // c >= max: min
    CHECK_EQ(med_predict(10, 20, 5), 20);  // c <= min: max
    CHECK_EQ(med_predict(10, 20, 15), 15); // a + b - c
}

TEST_CASE(planes_round_trip)
{
    for (const std::uint32_t size : {5u, 17u, 129u})
    {
        for (int variant = 0; variant < 4; ++variant)
        {
            std::vector<std::int16_t> q =
                terrain(size, static_cast<std::uint64_t>(variant), variant == 1);
            if (variant == 2)
            {
                // Extremes and wild jumps, whose residuals wrap around 16 bits.
                for (std::size_t k = 0; k < q.size(); ++k)
                {
                    q[k] = static_cast<std::int16_t>(k % 2 == 0 ? 32767 : -32767);
                }
            }
            if (variant == 3)
            {
                std::mt19937 rng(7);
                std::uniform_int_distribution<int> any(-32768, 32767);
                for (std::int16_t& v : q)
                {
                    v = static_cast<std::int16_t>(any(rng));
                }
            }
            std::vector<std::byte> planes(2 * q.size());
            elevation_encode_planes(q, size, planes);
            std::vector<std::int16_t> back(q.size());
            elevation_decode_planes(planes, size, back);
            CHECK(back == q);
        }
    }
}

TEST_CASE(pack_round_trip)
{
    const test_pack p = make_pack();
    counting_allocator counter;
    {
        const allocator a(counter.table);
        zstd_decompressor z(a);
        REQUIRE(z.valid());
        manifest m;
        REQUIRE(manifest::parse(p.manifest, a, z, m) == VGL_RESULT_SUCCESS);
        CHECK_EQ(m.layer().tile_size, static_cast<std::uint16_t>(pack_tile_size));
        CHECK_EQ(m.layer().shard_level, static_cast<std::uint8_t>(2));
        CHECK_EQ(m.height_step(3), 0.125);
        CHECK_EQ(m.shard_count(), 2u);
        CHECK(m.has_base());
        CHECK_EQ(m.metadata_count(), 3u);
        CHECK(m.metadata_value("name") == "Test pack");
        CHECK(m.metadata(2).value == "Test data B");
        CHECK(m.metadata_value("missing").empty());

        std::vector<std::int16_t> q(static_cast<std::size_t>(pack_tile_size) * pack_tile_size);
        std::vector<std::byte> scratch(2 * q.size());
        int decoded = 0;
        for (const auto& [key, expected] : p.samples)
        {
            const tile_id t = tile_from_key(key);
            const shard_location loc = tile_shard(t, m.layer().shard_level);
            shard_place place{loc.in_base, loc.root.x, loc.root.y};
            std::array<char, max_path_size> path{};
            if (place.base)
            {
                static_cast<void>(base_shard_path(m.base_digest(), path));
            }
            else
            {
                shard_entry e;
                REQUIRE(m.find_shard(place.x, place.y, e));
                static_cast<void>(shard_path(e.x, e.y, e.digest, path));
            }
            const std::vector<std::byte>& file = p.files.at(path.data());

            shard_header h;
            REQUIRE(parse_shard_header(file, m, place, h) == VGL_RESULT_SUCCESS);
            REQUIRE(file.size() == h.file_size);
            const byte_range dr = directory_range(h);
            shard_directory dir;
            REQUIRE(
                shard_directory::parse(std::span(file).subspan(static_cast<std::size_t>(dr.offset),
                                                               static_cast<std::size_t>(dr.size)),
                                       h,
                                       m,
                                       a,
                                       z,
                                       dir) == VGL_RESULT_SUCCESS);
            directory_entry e;
            REQUIRE(dir.find(t, e));
            const byte_range tr = dir.tile_range(e);
            const std::span<const std::byte> tile = std::span(file).subspan(
                static_cast<std::size_t>(tr.offset), static_cast<std::size_t>(tr.size));
            REQUIRE(check_tile(tile, e) == VGL_RESULT_SUCCESS);
            tile_header th;
            REQUIRE(parse_tile_header(tile, m, th) == VGL_RESULT_SUCCESS);
            CHECK_EQ(th.scale, m.height_step(t.level));
            CHECK_EQ(th.child_max[3], 4.0f);
            REQUIRE(decode_height_tile(tile, th, m, t.level, z, q, scratch) == VGL_RESULT_SUCCESS);
            CHECK(q == expected);
            CHECK_EQ((th.flags & elevation_flag_has_nodata) != 0,
                     std::find(expected.begin(), expected.end(), elevation_nodata) !=
                         expected.end());
            ++decoded;
        }
        CHECK_EQ(decoded, 20);

        // Tiles and shards that do not exist.
        shard_entry missing;
        CHECK(!m.find_shard(0, 0, missing));
        CHECK(counter.live > 0);
    }
    // Everything the readers and the decompressor allocated went through the host allocator
    // and was returned.
    CHECK_EQ(counter.live, 0);
}

TEST_CASE(writer_is_deterministic)
{
    const test_pack a = make_pack();
    const test_pack b = make_pack();
    CHECK(a.manifest == b.manifest);
    CHECK(a.files == b.files);
}

TEST_CASE(shard_file_names)
{
    std::array<char, max_path_size> path{};
    CHECK_EQ(shard_path(40, 20, 0xABCu, path), std::strlen("shards/20/40-00000abc.vgls"));
    CHECK_EQ(std::string(path.data()), std::string("shards/20/40-00000abc.vgls"));
    static_cast<void>(base_shard_path(0xDEADBEEFu, path));
    CHECK_EQ(std::string(path.data()), std::string("base-deadbeef.vgls"));
}

namespace
{

// Parses a manifest, and every shard header and directory, and decodes every tile. Returns the
// first failure.
vgl_result read_everything(const std::vector<std::byte>& manifest_bytes,
                           const std::map<std::string, std::vector<std::byte>>& files)
{
    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    vgl_result r = manifest::parse(manifest_bytes, a, z, m);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    std::vector<std::int16_t> q(static_cast<std::size_t>(m.layer().tile_size) *
                                m.layer().tile_size);
    std::vector<std::byte> scratch(2 * q.size());
    const auto read_shard = [&](const shard_place& place, const std::string& path) {
        const auto it = files.find(path);
        if (it == files.end())
        {
            return VGL_RESULT_NOT_FOUND;
        }
        const std::vector<std::byte>& file = it->second;
        shard_header h;
        vgl_result result = parse_shard_header(file, m, place, h);
        if (result != VGL_RESULT_SUCCESS)
        {
            return result;
        }
        // A provider checks that it received the whole file before reading ranges from it.
        if (file.size() != h.file_size)
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        shard_directory dir;
        const byte_range dr = directory_range(h);
        result = shard_directory::parse(std::span(file).subspan(static_cast<std::size_t>(dr.offset),
                                                                static_cast<std::size_t>(dr.size)),
                                        h,
                                        m,
                                        a,
                                        z,
                                        dir);
        if (result != VGL_RESULT_SUCCESS)
        {
            return result;
        }
        for (std::uint32_t i = 0; i < dir.tile_count(); ++i)
        {
            const directory_entry e = dir.entry(i);
            const byte_range tr = dir.tile_range(e);
            const std::span<const std::byte> tile = std::span(file).subspan(
                static_cast<std::size_t>(tr.offset), static_cast<std::size_t>(tr.size));
            result = check_tile(tile, e);
            tile_header th;
            if (result == VGL_RESULT_SUCCESS)
            {
                result = parse_tile_header(tile, m, th);
            }
            if (result == VGL_RESULT_SUCCESS)
            {
                result = decode_height_tile(tile, th, m, tile_from_key(e.key).level, z, q, scratch);
            }
            if (result != VGL_RESULT_SUCCESS)
            {
                return result;
            }
        }
        return VGL_RESULT_SUCCESS;
    };
    std::array<char, max_path_size> path{};
    if (m.has_base())
    {
        static_cast<void>(base_shard_path(m.base_digest(), path));
        r = read_shard({true, 0, 0}, path.data());
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
    }
    for (std::uint32_t i = 0; i < m.shard_count(); ++i)
    {
        const shard_entry e = m.shard(i);
        static_cast<void>(shard_path(e.x, e.y, e.digest, path));
        r = read_shard({false, e.x, e.y}, path.data());
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
    }
    return VGL_RESULT_SUCCESS;
}

} // namespace

TEST_CASE(every_corrupted_manifest_byte_is_detected)
{
    const test_pack p = make_pack();
    REQUIRE(read_everything(p.manifest, p.files) == VGL_RESULT_SUCCESS);
    int undetected = 0;
    for (std::size_t i = 0; i < p.manifest.size(); ++i)
    {
        std::vector<std::byte> corrupted = p.manifest;
        corrupted[i] ^= std::byte{0x5A};
        undetected += read_everything(corrupted, p.files) == VGL_RESULT_SUCCESS ? 1 : 0;
    }
    CHECK_EQ(undetected, 0);
    for (std::size_t n = 0; n < p.manifest.size(); ++n)
    {
        const std::vector<std::byte> truncated(p.manifest.begin(),
                                               p.manifest.begin() + static_cast<std::ptrdiff_t>(n));
        undetected += read_everything(truncated, p.files) == VGL_RESULT_SUCCESS ? 1 : 0;
    }
    CHECK_EQ(undetected, 0);
}

TEST_CASE(every_corrupted_shard_byte_is_detected)
{
    const test_pack p = make_pack();
    int undetected = 0;
    int checked = 0;
    for (const auto& [path, bytes] : p.files)
    {
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            auto files = p.files;
            files[path][i] ^= std::byte{0x5A};
            undetected += read_everything(p.manifest, files) == VGL_RESULT_SUCCESS ? 1 : 0;
            ++checked;
        }
        for (std::size_t n = 0; n < bytes.size(); n += 7)
        {
            auto files = p.files;
            files[path].resize(n);
            undetected += read_everything(p.manifest, files) == VGL_RESULT_SUCCESS ? 1 : 0;
        }
    }
    CHECK(checked > 1000);
    CHECK_EQ(undetected, 0);
}

TEST_CASE(decoding_corrupted_payloads_never_reads_out_of_bounds)
{
    // Corruption that the CRCs would catch is passed straight to the decoder, which must still
    // fail cleanly or decode something of the right size. Run under a sanitizer to check.
    const test_pack p = make_pack();
    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    REQUIRE(manifest::parse(p.manifest, a, z, m) == VGL_RESULT_SUCCESS);
    bake::height_tile h;
    h.size = pack_tile_size;
    h.q = terrain(pack_tile_size, 99);
    h.scale = 0.125;
    h.offset = 0.0;
    const std::vector<std::byte> tile = bake::encode_height_tile(h, zstd_level);
    std::vector<std::int16_t> q(h.q.size());
    std::vector<std::byte> scratch(2 * q.size());
    std::mt19937 rng(11);
    for (int k = 0; k < 2000; ++k)
    {
        std::vector<std::byte> corrupted = tile;
        const std::size_t at =
            elevation_tile_header_size + rng() % (tile.size() - elevation_tile_header_size);
        corrupted[at] ^= static_cast<std::byte>(1 + rng() % 255);
        tile_header th;
        if (parse_tile_header(corrupted, m, th) == VGL_RESULT_SUCCESS)
        {
            static_cast<void>(decode_height_tile(corrupted, th, m, 3, z, q, scratch));
        }
    }
    CHECK(true);
}

namespace
{

// Rewrites the manifest's major version, with valid CRCs.
std::vector<std::byte> with_major_version(std::vector<std::byte> bytes, std::uint16_t major)
{
    bytes[4] = static_cast<std::byte>(major & 0xFFu);
    bytes[5] = static_cast<std::byte>(major >> 8);
    const std::uint32_t crc = crc32c(std::span(bytes).first(28));
    for (std::size_t i = 0; i < 4; ++i)
    {
        bytes[28 + i] = static_cast<std::byte>((crc >> (8 * i)) & 0xFFu);
    }
    return bytes;
}

bake::manifest_contents minimal_contents()
{
    bake::manifest_contents m;
    m.layer.type = layer_type::elevation;
    m.layer.tile_size = 129;
    m.layer.min_level = 0;
    m.layer.max_level = 1;
    m.layer.shard_level = 2;
    m.layer.semi_major_axis = 6378137.0;
    m.layer.inverse_flattening = 298.257223563;
    m.height_steps = {0.5, 0.5};
    return m;
}

vgl_result parse_contents(const bake::manifest_contents& c)
{
    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    return manifest::parse(bake::write_manifest(c, zstd_level), a, z, m);
}

} // namespace

TEST_CASE(versioning_rules)
{
    const test_pack p = make_pack();
    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    CHECK_EQ(manifest::parse(with_major_version(p.manifest, 2), a, z, m),
             VGL_RESULT_VERSION_MISMATCH);

    bake::manifest_contents c = minimal_contents();
    c.extra_sections = {{0x80000042u, {std::byte{1}, std::byte{2}}}};
    CHECK_EQ(parse_contents(c), VGL_RESULT_SUCCESS);
    c.extra_sections = {{0x00000042u, {std::byte{1}, std::byte{2}}}};
    CHECK_EQ(parse_contents(c), VGL_RESULT_VERSION_MISMATCH);
    c.extra_sections = {{section_metadata, {}}};
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA); // Duplicate section.
}

TEST_CASE(manifest_validation_rules)
{
    CHECK_EQ(parse_contents(minimal_contents()), VGL_RESULT_SUCCESS);

    bake::manifest_contents c = minimal_contents();
    c.height_steps = {0.5, 0.3}; // Not a power of two.
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.height_steps = {0.5}; // One step short.
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.layer.tile_size = 128;
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.layer.min_level = 2; // Above max_level.
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.shards = {{0, 0, 100, 1}}; // Shard level above max_level, so no shards may exist.
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.layer.max_level = 3;
    c.height_steps = {0.5, 0.5, 0.5, 0.5};
    c.shards = {{1, 1, 100, 1}, {1, 1, 100, 2}}; // Duplicate.
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);

    c = minimal_contents();
    c.layer.type = layer_type::imagery;
    c.layer.tile_size = 256;
    c.layer.height_reference = height_reference_none;
    c.height_steps.clear();
    CHECK_EQ(parse_contents(c), VGL_RESULT_SUCCESS);
    c.layer.border = 2;
    CHECK_EQ(parse_contents(c), VGL_RESULT_CORRUPT_DATA);
}

TEST_CASE(geoid_tiles_must_not_have_nodata)
{
    const test_pack p = make_pack(layer_type::geoid);
    CHECK_EQ(read_everything(p.manifest, p.files), VGL_RESULT_SUCCESS);

    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    REQUIRE(manifest::parse(p.manifest, a, z, m) == VGL_RESULT_SUCCESS);
    bake::height_tile h;
    h.type = layer_type::geoid;
    h.size = pack_tile_size;
    h.q = terrain(pack_tile_size, 5, true);
    h.scale = 0.125;
    const std::vector<std::byte> tile = bake::encode_height_tile(h, zstd_level);
    std::vector<std::int16_t> q(h.q.size());
    std::vector<std::byte> scratch(2 * q.size());
    tile_header th;
    REQUIRE(parse_tile_header(tile, m, th) == VGL_RESULT_SUCCESS);
    CHECK_EQ(decode_height_tile(tile, th, m, 3, z, q, scratch), VGL_RESULT_CORRUPT_DATA);
}

TEST_CASE(tile_scale_must_match_level_step)
{
    const test_pack p = make_pack();
    const allocator a;
    zstd_decompressor z(a);
    manifest m;
    REQUIRE(manifest::parse(p.manifest, a, z, m) == VGL_RESULT_SUCCESS);
    bake::height_tile h;
    h.size = pack_tile_size;
    h.q = terrain(pack_tile_size, 5);
    std::vector<std::int16_t> q(h.q.size());
    std::vector<std::byte> scratch(2 * q.size());
    tile_header th;

    h.scale = 0.25; // Level 3 uses 0.125.
    std::vector<std::byte> tile = bake::encode_height_tile(h, zstd_level);
    REQUIRE(parse_tile_header(tile, m, th) == VGL_RESULT_SUCCESS);
    CHECK_EQ(decode_height_tile(tile, th, m, 3, z, q, scratch), VGL_RESULT_CORRUPT_DATA);

    h.scale = 0.125;
    h.offset = 0.0625; // Not a whole number of steps.
    tile = bake::encode_height_tile(h, zstd_level);
    REQUIRE(parse_tile_header(tile, m, th) == VGL_RESULT_SUCCESS);
    CHECK_EQ(decode_height_tile(tile, th, m, 3, z, q, scratch), VGL_RESULT_CORRUPT_DATA);
}
