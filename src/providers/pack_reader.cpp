// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "providers/pack_reader.hpp"

#include "decoders/elevation_codec.hpp"
#include "foundation/byte_reader.hpp"
#include "foundation/crc32c.hpp"
#include "geodesy/ellipsoid.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

namespace vgl::internal::pack
{

namespace
{

constexpr std::size_t max_sections = 64;

bool magic_matches(std::span<const std::byte> bytes, const std::array<std::byte, 4>& magic) noexcept
{
    return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool is_power_of_two(double v) noexcept
{
    if (!std::isfinite(v) || v <= 0.0)
    {
        return false;
    }
    int exponent = 0;
    return std::frexp(v, &exponent) == 0.5;
}

bool is_power_of_two(std::uint32_t v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}

struct section
{
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint32_t stored_size = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
};

// Copies or decompresses a section's data into out.
vgl_result load_section(std::span<const std::byte> file,
                        const section& s,
                        const allocator& a,
                        zstd_decompressor& decompressor,
                        buffer<std::byte>& out) noexcept
{
    const std::span<const std::byte> stored =
        file.subspan(static_cast<std::size_t>(s.offset), s.stored_size);
    if (crc32c(stored) != s.crc)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (!out.allocate(a, s.size))
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if ((s.flags & section_flag_compressed) != 0)
    {
        return decompressor.decompress(stored, out.span());
    }
    if (s.stored_size != s.size)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (s.size != 0)
    {
        std::memcpy(out.data(), stored.data(), s.size);
    }
    return VGL_RESULT_SUCCESS;
}

bool tile_size_is_valid(layer_type type, std::uint32_t size) noexcept
{
    if (has_heights(type))
    {
        return elevation_tile_size_is_valid(size);
    }
    return size >= 64 && size <= 2048 && is_power_of_two(size);
}

vgl_result parse_layer(std::span<const std::byte> data, layer_description& out) noexcept
{
    if (data.size() < layer_section_size)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    byte_reader r(data);
    const std::uint8_t type = r.u8();
    out.tiling_scheme = r.u8();
    out.tile_size = r.u16();
    out.min_level = r.u8();
    out.max_level = r.u8();
    out.shard_level = r.u8();
    out.border = r.u8();
    out.height_reference = r.u8();
    r.skip(7);
    out.semi_major_axis = r.f64();
    out.inverse_flattening = r.f64();
    out.west = r.f64();
    out.south = r.f64();
    out.east = r.f64();
    out.north = r.f64();
    if (!r.ok() || type < 1 || type > 3)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    out.type = static_cast<layer_type>(type);

    const bool heights = has_heights(out.type);
    const double flattening = out.inverse_flattening == 0.0 ? 0.0 : 1.0 / out.inverse_flattening;
    const bool valid =
        out.tiling_scheme == tiling_scheme_geographic &&
        tile_size_is_valid(out.type, out.tile_size) && out.min_level <= out.max_level &&
        out.max_level <= max_tile_level && out.shard_level <= max_tile_level + 1 &&
        (heights ? out.border == 0 : out.border <= 1) &&
        out.height_reference == (heights ? height_reference_ellipsoid : height_reference_none) &&
        ellipsoid::is_valid(out.semi_major_axis, flattening) && std::isfinite(out.west) &&
        std::isfinite(out.south) && std::isfinite(out.east) && std::isfinite(out.north);
    return valid ? VGL_RESULT_SUCCESS : VGL_RESULT_CORRUPT_DATA;
}

} // namespace

vgl_result manifest::parse(std::span<const std::byte> file,
                           const allocator& a,
                           zstd_decompressor& decompressor,
                           manifest& out) noexcept
{
    out = manifest();
    if (file.size() < manifest_header_size || !magic_matches(file, manifest_magic))
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    byte_reader r(file);
    r.skip(4);
    const std::uint16_t major = r.u16();
    out.minor_version_ = r.u16();
    const std::uint32_t section_count = r.u32();
    r.skip(4); // flags
    const std::uint64_t file_size = r.u64();
    const std::uint32_t table_crc = r.u32();
    const std::uint32_t header_crc = r.u32();
    if (crc32c(file.first(28)) != header_crc)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (major != major_version)
    {
        return VGL_RESULT_VERSION_MISMATCH;
    }
    if (file_size != file.size() || section_count > max_sections)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const std::size_t table_end = manifest_header_size + section_count * section_entry_size;
    if (table_end > file.size() ||
        crc32c(file.subspan(manifest_header_size, section_count * section_entry_size)) != table_crc)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }

    std::array<section, max_sections> sections{};
    const section* layer = nullptr;
    const section* steps = nullptr;
    const section* index = nullptr;
    const section* metadata = nullptr;
    for (std::uint32_t i = 0; i < section_count; ++i)
    {
        section& s = sections[i];
        s.type = r.u32();
        s.flags = r.u32();
        s.offset = r.u64();
        s.stored_size = r.u32();
        s.size = r.u32();
        s.crc = r.u32();
        r.skip(4);
        if (s.offset < table_end || s.offset > file.size() ||
            s.stored_size > file.size() - s.offset)
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        for (std::uint32_t k = 0; k < i; ++k)
        {
            const section& o = sections[k];
            const bool overlap =
                s.offset < o.offset + o.stored_size && o.offset < s.offset + s.stored_size;
            if (o.type == s.type || (overlap && s.stored_size != 0 && o.stored_size != 0))
            {
                return VGL_RESULT_CORRUPT_DATA;
            }
        }
        switch (s.type)
        {
        case section_layer:
            layer = &s;
            break;
        case section_height_steps:
            steps = &s;
            break;
        case section_shard_index:
            index = &s;
            break;
        case section_metadata:
            metadata = &s;
            break;
        default:
            if ((s.type & section_optional_bit) == 0)
            {
                return VGL_RESULT_VERSION_MISMATCH;
            }
            break;
        }
    }
    if (!r.ok() || layer == nullptr || index == nullptr || metadata == nullptr)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }

    buffer<std::byte> data;
    vgl_result result = load_section(file, *layer, a, decompressor, data);
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    result = parse_layer(data.span(), out.layer_);
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    const layer_description& l = out.layer_;

    if (has_heights(l.type))
    {
        if (steps == nullptr)
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        result = load_section(file, *steps, a, decompressor, data);
        if (result != VGL_RESULT_SUCCESS)
        {
            return result;
        }
        const std::uint32_t level_count = l.max_level - l.min_level + 1u;
        if (data.size() != level_count * sizeof(double))
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        byte_reader steps_reader(data.span());
        for (std::uint32_t i = 0; i < level_count; ++i)
        {
            out.height_steps_[i] = steps_reader.f64();
            if (!is_power_of_two(out.height_steps_[i]))
            {
                return VGL_RESULT_CORRUPT_DATA;
            }
        }
    }

    result = load_section(file, *index, a, decompressor, data);
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    byte_reader index_reader(data.span());
    out.shard_count_ = index_reader.u32();
    const std::uint32_t has_base = index_reader.u32();
    out.base_file_size_ = index_reader.u64();
    out.base_digest_ = index_reader.u32();
    index_reader.skip(4);
    const std::uint64_t entries_size =
        static_cast<std::uint64_t>(out.shard_count_) * shard_index_entry_size;
    if (!index_reader.ok() || has_base > 1 || entries_size != index_reader.remaining())
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    out.has_base_ = has_base == 1;
    // A base shard exists only above the shard level, and shards only at or below max_level.
    if ((out.has_base_ && l.shard_level <= l.min_level) ||
        (out.shard_count_ != 0 && l.shard_level > l.max_level))
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (!out.shard_entries_.allocate(a, static_cast<std::size_t>(entries_size)))
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (entries_size != 0)
    {
        std::memcpy(out.shard_entries_.data(),
                    data.data() + shard_index_header_size,
                    static_cast<std::size_t>(entries_size));
    }
    std::uint64_t previous = 0;
    for (std::uint32_t i = 0; i < out.shard_count_; ++i)
    {
        const shard_entry e = out.shard(i);
        const std::uint64_t m = morton(e.x, e.y);
        if (!tile_is_valid({l.shard_level, e.x, e.y}) || (i > 0 && m <= previous))
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        previous = m;
    }

    result = load_section(file, *metadata, a, decompressor, data);
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    byte_reader meta_reader(data.span());
    out.metadata_count_ = meta_reader.u32();
    for (std::uint32_t i = 0; i < 2 * out.metadata_count_ && meta_reader.ok(); ++i)
    {
        meta_reader.skip(meta_reader.u32());
    }
    if (!meta_reader.ok() || meta_reader.remaining() != 0)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (!out.metadata_.allocate(a, data.size() - 4))
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (!out.metadata_.empty())
    {
        std::memcpy(out.metadata_.data(), data.data() + 4, out.metadata_.size());
    }
    return VGL_RESULT_SUCCESS;
}

shard_entry manifest::shard(std::uint32_t index) const noexcept
{
    byte_reader r(shard_entries_.span());
    r.seek(static_cast<std::size_t>(index) * shard_index_entry_size);
    shard_entry e;
    e.x = r.u32();
    e.y = r.u32();
    e.file_size = r.u64();
    e.digest = r.u32();
    return e;
}

bool manifest::find_shard(std::uint32_t x, std::uint32_t y, shard_entry& out) const noexcept
{
    const std::uint64_t target = morton(x, y);
    std::uint32_t lo = 0;
    std::uint32_t hi = shard_count_;
    while (lo < hi)
    {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const shard_entry e = shard(mid);
        const std::uint64_t m = morton(e.x, e.y);
        if (m == target)
        {
            out = e;
            return true;
        }
        if (m < target)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return false;
}

metadata_entry manifest::metadata(std::uint32_t index) const noexcept
{
    byte_reader r(metadata_.span());
    for (std::uint32_t i = 0; i < index; ++i)
    {
        r.skip(r.u32());
        r.skip(r.u32());
    }
    const std::span<const std::byte> key = r.bytes(r.u32());
    const std::span<const std::byte> value = r.bytes(r.u32());
    return {{reinterpret_cast<const char*>(key.data()), key.size()},
            {reinterpret_cast<const char*>(value.data()), value.size()}};
}

std::string_view manifest::metadata_value(std::string_view key) const noexcept
{
    for (std::uint32_t i = 0; i < metadata_count_; ++i)
    {
        const metadata_entry e = metadata(i);
        if (e.key == key)
        {
            return e.value;
        }
    }
    return {};
}

vgl_result parse_shard_header(std::span<const std::byte> bytes,
                              const manifest& m,
                              const shard_place& place,
                              shard_header& out) noexcept
{
    if (bytes.size() < shard_header_size || !magic_matches(bytes, shard_magic))
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    byte_reader r(bytes);
    r.skip(4);
    const std::uint16_t major = r.u16();
    r.skip(2); // minor version
    const std::uint8_t type = r.u8();
    out.root_level = r.u8();
    out.flags = r.u8();
    r.skip(1);
    out.root_x = r.u32();
    out.root_y = r.u32();
    out.tile_count = r.u32();
    out.directory_offset = r.u64();
    out.directory_stored_size = r.u32();
    out.directory_crc = r.u32();
    out.data_offset = r.u64();
    out.file_size = r.u64();
    r.skip(4);
    out.header_crc = r.u32();
    if (!r.ok() || crc32c(bytes.first(60)) != out.header_crc)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (major != major_version)
    {
        return VGL_RESULT_VERSION_MISMATCH;
    }
    out.type = static_cast<layer_type>(type);

    const layer_description& l = m.layer();
    std::uint64_t expected_size = 0;
    std::uint32_t expected_digest = 0;
    if (place.base)
    {
        expected_size = m.base_file_size();
        expected_digest = m.base_digest();
    }
    else
    {
        shard_entry e;
        if (!m.find_shard(place.x, place.y, e))
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        expected_size = e.file_size;
        expected_digest = e.digest;
    }
    const std::uint8_t expected_level = place.base ? l.min_level : l.shard_level;
    const bool valid =
        type == static_cast<std::uint8_t>(l.type) && out.is_base() == place.base &&
        out.root_level == expected_level && out.root_x == (place.base ? 0 : place.x) &&
        out.root_y == (place.base ? 0 : place.y) && out.header_crc == expected_digest &&
        out.file_size == expected_size && out.directory_offset >= shard_header_size &&
        out.directory_offset <= out.file_size &&
        out.directory_stored_size <= out.file_size - out.directory_offset &&
        out.data_offset >= out.directory_offset + out.directory_stored_size &&
        out.data_offset <= out.file_size;
    return valid ? VGL_RESULT_SUCCESS : VGL_RESULT_CORRUPT_DATA;
}

vgl_result shard_directory::parse(std::span<const std::byte> stored,
                                  const shard_header& header,
                                  const manifest& m,
                                  const allocator& a,
                                  zstd_decompressor& decompressor,
                                  shard_directory& out) noexcept
{
    out = shard_directory();
    if (stored.size() != header.directory_stored_size || crc32c(stored) != header.directory_crc)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(header.tile_count) * directory_entry_size;
    if (size > static_cast<std::size_t>(-1))
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    if (!out.entries_.allocate(a, static_cast<std::size_t>(size)))
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    const vgl_result result = decompressor.decompress(stored, out.entries_.span());
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    out.count_ = header.tile_count;
    out.data_offset_ = header.data_offset;

    // Keys strictly ascending and inside the shard; tiles contiguous up to the end of the file.
    const layer_description& l = m.layer();
    const tile_id root{header.root_level, header.root_x, header.root_y};
    std::uint64_t expected_offset = 0;
    std::uint64_t previous_key = 0;
    for (std::uint32_t i = 0; i < out.count_; ++i)
    {
        const directory_entry e = out.entry(i);
        const tile_id t = tile_from_key(e.key);
        const bool inside = header.is_base()
                                ? t.level >= l.min_level && t.level < l.shard_level
                                : t.level <= l.max_level && tile_is_in_subtree(t, root);
        if (!tile_key_is_valid(e.key) || !inside || (i > 0 && e.key <= previous_key) ||
            e.offset != expected_offset || e.size < tile_common_header_size)
        {
            return VGL_RESULT_CORRUPT_DATA;
        }
        previous_key = e.key;
        expected_offset += e.size;
    }
    return expected_offset == header.file_size - header.data_offset ? VGL_RESULT_SUCCESS
                                                                    : VGL_RESULT_CORRUPT_DATA;
}

directory_entry shard_directory::entry(std::uint32_t index) const noexcept
{
    byte_reader r(entries_.span());
    r.seek(static_cast<std::size_t>(index) * directory_entry_size);
    directory_entry e;
    e.key = r.u64();
    e.offset = r.u64();
    e.size = r.u32();
    e.crc = r.u32();
    return e;
}

bool shard_directory::find(const tile_id& t, directory_entry& out) const noexcept
{
    const std::uint64_t key = tile_key(t);
    std::uint32_t lo = 0;
    std::uint32_t hi = count_;
    while (lo < hi)
    {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const directory_entry e = entry(mid);
        if (e.key == key)
        {
            out = e;
            return true;
        }
        if (e.key < key)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return false;
}

vgl_result check_tile(std::span<const std::byte> tile, const directory_entry& e) noexcept
{
    return tile.size() == e.size && crc32c(tile) == e.crc ? VGL_RESULT_SUCCESS
                                                          : VGL_RESULT_CORRUPT_DATA;
}

vgl_result
parse_tile_header(std::span<const std::byte> tile, const manifest& m, tile_header& out) noexcept
{
    byte_reader r(tile);
    const std::uint8_t type = r.u8();
    out.encoding = r.u8();
    out.child_mask = r.u8() & 0x0Fu;
    out.flags = r.u8();
    out.header_size = r.u16();
    r.skip(2);
    const layer_description& l = m.layer();
    const std::size_t min_header =
        has_heights(l.type) ? elevation_tile_header_size : tile_common_header_size;
    if (!r.ok() || type != static_cast<std::uint8_t>(l.type) || out.header_size < min_header ||
        out.header_size > tile.size())
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    out.type = l.type;
    if (has_heights(l.type))
    {
        out.offset = r.f64();
        out.scale = r.f64();
        for (float& v : out.child_min)
        {
            v = r.f32();
        }
        for (float& v : out.child_max)
        {
            v = r.f32();
        }
        if (out.encoding != elevation_encoding_med_zstd)
        {
            return VGL_RESULT_NOT_SUPPORTED;
        }
    }
    else if (out.encoding < 1 || out.encoding > 5)
    {
        return VGL_RESULT_NOT_SUPPORTED;
    }
    return r.ok() ? VGL_RESULT_SUCCESS : VGL_RESULT_CORRUPT_DATA;
}

vgl_result decode_height_tile(std::span<const std::byte> tile,
                              const tile_header& header,
                              const manifest& m,
                              std::uint32_t level,
                              zstd_decompressor& decompressor,
                              std::span<std::int16_t> q,
                              std::span<std::byte> scratch) noexcept
{
    const layer_description& l = m.layer();
    const std::uint32_t size = l.tile_size;
    const std::size_t count = static_cast<std::size_t>(size) * size;
    if (!has_heights(l.type) || level < l.min_level || level > l.max_level || q.size() != count ||
        scratch.size() != 2 * count)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    // The scale is the level's step, and the offset a whole number of steps (section 6.2).
    const double steps = header.offset / header.scale;
    if (header.scale != m.height_step(level) || !std::isfinite(steps) || std::trunc(steps) != steps)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const vgl_result result = decompressor.decompress(tile.subspan(header.header_size), scratch);
    if (result != VGL_RESULT_SUCCESS)
    {
        return result;
    }
    elevation_decode_planes(scratch, size, q);
    if (l.type == layer_type::geoid && std::find(q.begin(), q.end(), elevation_nodata) != q.end())
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    return VGL_RESULT_SUCCESS;
}

namespace
{

// Appends text to a fixed buffer, tracking the length. The buffer always has room, because
// max_path_size covers the longest path.
struct path_builder
{
    std::span<char, max_path_size> out;
    std::size_t length = 0;

    void text(std::string_view s) noexcept
    {
        for (const char c : s)
        {
            out[length++] = c;
        }
    }

    void decimal(std::uint32_t v) noexcept
    {
        const std::to_chars_result r =
            std::to_chars(out.data() + length, out.data() + out.size() - 1, v);
        length = static_cast<std::size_t>(r.ptr - out.data());
    }

    // Eight lowercase hexadecimal digits.
    void hex8(std::uint32_t v) noexcept
    {
        constexpr std::string_view digits = "0123456789abcdef";
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            out[length++] = digits[(v >> shift) & 0xFu];
        }
    }

    [[nodiscard]] std::size_t finish() const noexcept
    {
        out[length] = '\0';
        return length;
    }
};

} // namespace

std::size_t base_shard_path(std::uint32_t digest, std::span<char, max_path_size> out) noexcept
{
    path_builder p{out};
    p.text("base-");
    p.hex8(digest);
    p.text(".vgls");
    return p.finish();
}

std::size_t shard_path(std::uint32_t x,
                       std::uint32_t y,
                       std::uint32_t digest,
                       std::span<char, max_path_size> out) noexcept
{
    path_builder p{out};
    p.text("shards/");
    p.decimal(y);
    p.text("/");
    p.decimal(x);
    p.text("-");
    p.hex8(digest);
    p.text(".vgls");
    return p.finish();
}

} // namespace vgl::internal::pack
