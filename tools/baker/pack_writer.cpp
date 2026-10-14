// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "pack_writer.hpp"

#include "decoders/elevation_codec.hpp"
#include "foundation/crc32c.hpp"

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace vgl::bake
{

using vgl::internal::crc32c;

void byte_writer::f32(float v)
{
    put(std::bit_cast<std::uint32_t>(v));
}

void byte_writer::f64(double v)
{
    put(std::bit_cast<std::uint64_t>(v));
}

void byte_writer::string(std::string_view s)
{
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(std::as_bytes(std::span(s.data(), s.size())));
}

void byte_writer::u32_at(std::size_t position, std::uint32_t v)
{
    for (std::size_t i = 0; i < 4; ++i)
    {
        data_.at(position + i) = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
}

std::vector<std::byte> zstd_compress(std::span<const std::byte> data, int level)
{
    std::vector<std::byte> out(ZSTD_compressBound(data.size()));
    const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
    if (ZSTD_isError(size) != 0)
    {
        throw std::runtime_error(std::string("Zstandard compression failed: ") +
                                 ZSTD_getErrorName(size));
    }
    out.resize(size);
    return out;
}

std::vector<std::byte> encode_height_tile(const height_tile& tile, int zstd_level)
{
    if (!pack::has_heights(tile.type) ||
        tile.q.size() != static_cast<std::size_t>(tile.size) * tile.size)
    {
        throw std::logic_error("encode_height_tile: invalid tile");
    }
    const bool has_nodata =
        std::find(tile.q.begin(), tile.q.end(), vgl::internal::elevation_nodata) != tile.q.end();

    std::vector<std::byte> planes(2 * tile.q.size());
    vgl::internal::elevation_encode_planes(tile.q, tile.size, planes);

    byte_writer w;
    w.u8(static_cast<std::uint8_t>(tile.type));
    w.u8(pack::elevation_encoding_med_zstd);
    w.u8(tile.child_mask);
    w.u8(has_nodata ? pack::elevation_flag_has_nodata : 0);
    w.u16(static_cast<std::uint16_t>(pack::elevation_tile_header_size));
    w.u16(0);
    w.f64(tile.offset);
    w.f64(tile.scale);
    for (const float v : tile.child_min)
    {
        w.f32(v);
    }
    for (const float v : tile.child_max)
    {
        w.f32(v);
    }
    w.bytes(zstd_compress(planes, zstd_level));
    return w.take();
}

std::vector<std::byte> encode_imagery_tile(pack::imagery_encoding encoding,
                                           std::uint8_t child_mask,
                                           std::span<const std::byte> image)
{
    byte_writer w;
    w.u8(static_cast<std::uint8_t>(pack::layer_type::imagery));
    w.u8(static_cast<std::uint8_t>(encoding));
    w.u8(child_mask);
    w.u8(0);
    w.u16(static_cast<std::uint16_t>(pack::tile_common_header_size));
    w.u16(0);
    w.bytes(image);
    return w.take();
}

shard_writer::shard_writer(pack::layer_type type,
                           std::uint8_t root_level,
                           std::uint32_t root_x,
                           std::uint32_t root_y,
                           bool base)
    : type_(type), root_level_(root_level), root_x_(root_x), root_y_(root_y), base_(base)
{
}

void shard_writer::add(const tile_id& t, std::vector<std::byte> tile)
{
    const std::uint64_t key = vgl::internal::tile_key(t);
    if (!tiles_.empty() && key <= tiles_.back().first)
    {
        throw std::logic_error("shard_writer::add: tiles must be added in ascending key order");
    }
    tiles_.emplace_back(key, std::move(tile));
}

shard_writer::output shard_writer::finish(int zstd_level) const
{
    byte_writer directory;
    std::uint64_t offset = 0;
    for (const auto& [key, tile] : tiles_)
    {
        directory.u64(key);
        directory.u64(offset);
        directory.u32(static_cast<std::uint32_t>(tile.size()));
        directory.u32(crc32c(tile));
        offset += tile.size();
    }
    const std::vector<std::byte> stored = zstd_compress(directory.view(), zstd_level);

    const std::uint64_t data_offset = pack::shard_header_size + stored.size();
    byte_writer w;
    w.bytes(pack::shard_magic);
    w.u16(pack::major_version);
    w.u16(pack::minor_version);
    w.u8(static_cast<std::uint8_t>(type_));
    w.u8(root_level_);
    w.u8(base_ ? pack::shard_flag_base : 0);
    w.u8(0);
    w.u32(base_ ? 0 : root_x_);
    w.u32(base_ ? 0 : root_y_);
    w.u32(static_cast<std::uint32_t>(tiles_.size()));
    w.u64(pack::shard_header_size);
    w.u32(static_cast<std::uint32_t>(stored.size()));
    w.u32(crc32c(stored));
    w.u64(data_offset);
    w.u64(data_offset + offset);
    w.u32(0);
    const std::uint32_t digest = crc32c(w.view());
    w.u32(digest);
    w.bytes(stored);
    for (const auto& entry : tiles_)
    {
        w.bytes(entry.second);
    }
    return {w.take(), digest};
}

std::vector<std::byte> write_manifest(const manifest_contents& c, int zstd_level)
{
    struct section_data
    {
        std::uint32_t type;
        bool compressed;
        std::vector<std::byte> stored;
        std::uint32_t size;
    };
    std::vector<section_data> sections;
    const auto add = [&](std::uint32_t type, std::vector<std::byte> data, bool compress) {
        const auto size = static_cast<std::uint32_t>(data.size());
        if (compress)
        {
            sections.push_back({type, true, zstd_compress(data, zstd_level), size});
        }
        else
        {
            sections.push_back({type, false, std::move(data), size});
        }
    };

    const pack::layer_description& l = c.layer;
    byte_writer layer;
    layer.u8(static_cast<std::uint8_t>(l.type));
    layer.u8(l.tiling_scheme);
    layer.u16(l.tile_size);
    layer.u8(l.min_level);
    layer.u8(l.max_level);
    layer.u8(l.shard_level);
    layer.u8(l.border);
    layer.u8(l.height_reference);
    layer.zeros(7);
    layer.f64(l.semi_major_axis);
    layer.f64(l.inverse_flattening);
    layer.f64(l.west);
    layer.f64(l.south);
    layer.f64(l.east);
    layer.f64(l.north);
    add(pack::section_layer, layer.take(), false);

    if (pack::has_heights(l.type))
    {
        byte_writer steps;
        for (const double s : c.height_steps)
        {
            steps.f64(s);
        }
        add(pack::section_height_steps, steps.take(), false);
    }

    std::vector<pack::shard_entry> shards = c.shards;
    std::sort(
        shards.begin(), shards.end(), [](const pack::shard_entry& a, const pack::shard_entry& b) {
            return vgl::internal::morton(a.x, a.y) < vgl::internal::morton(b.x, b.y);
        });
    byte_writer index;
    index.u32(static_cast<std::uint32_t>(shards.size()));
    index.u32(c.has_base ? 1 : 0);
    index.u64(c.base_file_size);
    index.u32(c.base_digest);
    index.u32(0);
    for (const pack::shard_entry& e : shards)
    {
        index.u32(e.x);
        index.u32(e.y);
        index.u64(e.file_size);
        index.u32(e.digest);
        index.u32(0);
    }
    add(pack::section_shard_index, index.take(), true);

    byte_writer metadata;
    metadata.u32(static_cast<std::uint32_t>(c.metadata.size()));
    for (const auto& [key, value] : c.metadata)
    {
        metadata.string(key);
        metadata.string(value);
    }
    add(pack::section_metadata, metadata.take(), false);

    for (const auto& [type, data] : c.extra_sections)
    {
        add(type, data, false);
    }

    // Header, section table, then the sections in order.
    const std::size_t table_size = sections.size() * pack::section_entry_size;
    std::uint64_t offset = pack::manifest_header_size + table_size;
    byte_writer table;
    for (const section_data& s : sections)
    {
        table.u32(s.type);
        table.u32(s.compressed ? pack::section_flag_compressed : 0);
        table.u64(offset);
        table.u32(static_cast<std::uint32_t>(s.stored.size()));
        table.u32(s.size);
        table.u32(crc32c(s.stored));
        table.u32(0);
        offset += s.stored.size();
    }

    byte_writer w;
    w.bytes(pack::manifest_magic);
    w.u16(pack::major_version);
    w.u16(pack::minor_version);
    w.u32(static_cast<std::uint32_t>(sections.size()));
    w.u32(0);
    w.u64(offset);
    w.u32(crc32c(table.view()));
    w.u32(crc32c(w.view()));
    w.bytes(table.view());
    for (const section_data& s : sections)
    {
        w.bytes(s.stored);
    }
    return w.take();
}

} // namespace vgl::bake
