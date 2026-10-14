// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Writers for the pack format (docs/pack-format.md), the counterpart of the runtime readers in
// src/providers/pack_reader.hpp. Unlike the library, the baker may allocate freely and throw:
// errors are reported as std::runtime_error or std::logic_error.

#ifndef VGL_BAKER_PACK_WRITER_HPP
#define VGL_BAKER_PACK_WRITER_HPP

#include "providers/pack_format.hpp"
#include "tiling/tile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vgl::bake
{

namespace pack = vgl::internal::pack;
using vgl::internal::tile_id;

// Appends little-endian values to a byte vector.
class byte_writer
{
public:
    void u8(std::uint8_t v) { data_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { put(v); }
    void u32(std::uint32_t v) { put(v); }
    void u64(std::uint64_t v) { put(v); }
    void f32(float v);
    void f64(double v);
    void bytes(std::span<const std::byte> b) { data_.insert(data_.end(), b.begin(), b.end()); }
    void zeros(std::size_t count) { data_.insert(data_.end(), count, std::byte{0}); }
    void string(std::string_view s);

    // Overwrites a value written earlier.
    void u32_at(std::size_t position, std::uint32_t v);

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::span<const std::byte> view() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(data_); }

private:
    template <typename T>
    void put(T v)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            data_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
    }

    std::vector<std::byte> data_;
};

// Compresses data as one Zstandard frame with the content size present.
[[nodiscard]] std::vector<std::byte> zstd_compress(std::span<const std::byte> data, int level);

// An elevation or geoid tile before encoding (section 6.2).
struct height_tile
{
    pack::layer_type type = pack::layer_type::elevation;
    std::uint32_t size = 0;      // Samples per side.
    std::vector<std::int16_t> q; // size * size quantized samples, row-major.
    double offset = 0.0;
    double scale = 0.0;
    std::uint8_t child_mask = 0;
    std::array<float, 4> child_min = {};
    std::array<float, 4> child_max = {};
};

[[nodiscard]] std::vector<std::byte> encode_height_tile(const height_tile& tile, int zstd_level);

// An imagery tile: the common header followed by an encoded image (section 6.3).
[[nodiscard]] std::vector<std::byte> encode_imagery_tile(pack::imagery_encoding encoding,
                                                         std::uint8_t child_mask,
                                                         std::span<const std::byte> image);

// Collects the tiles of one shard, in ascending key order, and writes the shard file.
class shard_writer
{
public:
    // A level-S shard rooted at (root_level, root_x, root_y), or the base shard, whose
    // root_level is the pack's min_level.
    shard_writer(pack::layer_type type,
                 std::uint8_t root_level,
                 std::uint32_t root_x,
                 std::uint32_t root_y,
                 bool base);

    // Adds a tile. Keys must be strictly ascending.
    void add(const tile_id& t, std::vector<std::byte> tile);

    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }
    [[nodiscard]] std::size_t tile_count() const noexcept { return tiles_.size(); }

    struct output
    {
        std::vector<std::byte> bytes;
        std::uint32_t digest = 0;
    };

    [[nodiscard]] output finish(int zstd_level) const;

private:
    pack::layer_type type_;
    std::uint8_t root_level_;
    std::uint32_t root_x_;
    std::uint32_t root_y_;
    bool base_;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> tiles_;
};

// Everything a manifest holds (section 4).
struct manifest_contents
{
    pack::layer_description layer;
    std::vector<double> height_steps; // One per level from min_level to max_level.
    bool has_base = false;
    std::uint64_t base_file_size = 0;
    std::uint32_t base_digest = 0;
    std::vector<pack::shard_entry> shards; // Any order; sorted when written.
    std::vector<std::pair<std::string, std::string>> metadata;

    // Additional sections, written as given. Used by tests for unknown section types.
    std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> extra_sections;
};

[[nodiscard]] std::vector<std::byte> write_manifest(const manifest_contents& contents,
                                                    int zstd_level);

} // namespace vgl::bake

#endif // VGL_BAKER_PACK_WRITER_HPP
