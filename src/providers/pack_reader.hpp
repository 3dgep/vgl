// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Readers for the pack format (docs/pack-format.md). They parse bytes that the caller has
// read, and validate everything section 8 requires: no file contents can make them read
// outside the data they were given. Every failure returns VGL_RESULT_CORRUPT_DATA, unless
// stated otherwise.

#ifndef VGL_PROVIDERS_PACK_READER_HPP
#define VGL_PROVIDERS_PACK_READER_HPP

#include "decoders/zstd_decompressor.hpp"
#include "foundation/buffer.hpp"
#include "providers/pack_format.hpp"
#include "tiling/tile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vgl::internal::pack
{

struct metadata_entry
{
    std::string_view key;
    std::string_view value;
};

class manifest
{
public:
    // Parses and validates a complete pack.vglm file. Returns VGL_RESULT_VERSION_MISMATCH for
    // an unknown major version or an unknown required section, and VGL_RESULT_OUT_OF_MEMORY
    // if allocation fails.
    [[nodiscard]] static vgl_result parse(std::span<const std::byte> file,
                                          const allocator& a,
                                          zstd_decompressor& decompressor,
                                          manifest& out) noexcept;

    [[nodiscard]] std::uint16_t minor_version() const noexcept { return minor_version_; }
    [[nodiscard]] const layer_description& layer() const noexcept { return layer_; }

    // Quantization step of a level, for packs with heights. Requires min_level <= level <=
    // max_level.
    [[nodiscard]] double height_step(std::uint32_t level) const noexcept
    {
        return height_steps_[level - layer_.min_level];
    }

    [[nodiscard]] bool has_base() const noexcept { return has_base_; }
    [[nodiscard]] std::uint64_t base_file_size() const noexcept { return base_file_size_; }
    [[nodiscard]] std::uint32_t base_digest() const noexcept { return base_digest_; }

    [[nodiscard]] std::uint32_t shard_count() const noexcept { return shard_count_; }
    [[nodiscard]] shard_entry shard(std::uint32_t index) const noexcept;

    // Finds the shard rooted at (shard_level, x, y). Returns false if it does not exist.
    [[nodiscard]] bool
    find_shard(std::uint32_t x, std::uint32_t y, shard_entry& out) const noexcept;

    [[nodiscard]] std::uint32_t metadata_count() const noexcept { return metadata_count_; }
    [[nodiscard]] metadata_entry metadata(std::uint32_t index) const noexcept;

    // Value of the first metadata entry with this key, or an empty view.
    [[nodiscard]] std::string_view metadata_value(std::string_view key) const noexcept;

private:
    std::uint16_t minor_version_ = 0;
    layer_description layer_;
    std::array<double, max_tile_level + 1> height_steps_ = {};
    bool has_base_ = false;
    std::uint64_t base_file_size_ = 0;
    std::uint32_t base_digest_ = 0;
    std::uint32_t shard_count_ = 0;
    buffer<std::byte> shard_entries_; // shard_count_ entries of shard_index_entry_size bytes.
    std::uint32_t metadata_count_ = 0;
    buffer<std::byte> metadata_; // The METADATA section after its count.
};

// Where a shard sits in a pack: the base shard, or the shard rooted at (shard_level, x, y).
struct shard_place
{
    bool base = false;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// Parses and validates the first shard_header_size bytes of a shard, and checks them against
// the manifest: layer type, place, digest and file size.
[[nodiscard]] vgl_result parse_shard_header(std::span<const std::byte> bytes,
                                            const manifest& m,
                                            const shard_place& place,
                                            shard_header& out) noexcept;

// The byte range of a shard's directory, from its header.
struct byte_range
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

[[nodiscard]] constexpr byte_range directory_range(const shard_header& h) noexcept
{
    return {h.directory_offset, h.directory_stored_size};
}

class shard_directory
{
public:
    // Parses and validates a shard's directory from its stored (compressed) bytes.
    [[nodiscard]] static vgl_result parse(std::span<const std::byte> stored,
                                          const shard_header& header,
                                          const manifest& m,
                                          const allocator& a,
                                          zstd_decompressor& decompressor,
                                          shard_directory& out) noexcept;

    [[nodiscard]] std::uint32_t tile_count() const noexcept { return count_; }
    [[nodiscard]] directory_entry entry(std::uint32_t index) const noexcept;

    // Finds a tile. Returns false if the shard does not contain it.
    [[nodiscard]] bool find(const tile_id& t, directory_entry& out) const noexcept;

    // Byte range of a tile in the shard file.
    [[nodiscard]] byte_range tile_range(const directory_entry& e) const noexcept
    {
        return {data_offset_ + e.offset, e.size};
    }

private:
    std::uint32_t count_ = 0;
    std::uint64_t data_offset_ = 0;
    buffer<std::byte> entries_;
};

// Checks a tile's bytes against its directory entry.
[[nodiscard]] vgl_result check_tile(std::span<const std::byte> tile,
                                    const directory_entry& e) noexcept;

// Parses a tile header (section 6.1) and, for packs with heights, the fields of section 6.2.
// Returns VGL_RESULT_NOT_SUPPORTED for an unknown encoding.
[[nodiscard]] vgl_result
parse_tile_header(std::span<const std::byte> tile, const manifest& m, tile_header& out) noexcept;

// Decodes the quantized samples of an elevation or geoid tile at a level. q receives
// tile_size^2 samples in row-major order; scratch must hold 2 * tile_size^2 bytes.
[[nodiscard]] vgl_result decode_height_tile(std::span<const std::byte> tile,
                                            const tile_header& header,
                                            const manifest& m,
                                            std::uint32_t level,
                                            zstd_decompressor& decompressor,
                                            std::span<std::int16_t> q,
                                            std::span<std::byte> scratch) noexcept;

// Shard file names, relative to the manifest (section 5.2). Return the number of characters
// written, not counting the terminator, which is always written.
inline constexpr std::size_t max_path_size = 48;
std::size_t base_shard_path(std::uint32_t digest, std::span<char, max_path_size> out) noexcept;
std::size_t shard_path(std::uint32_t x,
                       std::uint32_t y,
                       std::uint32_t digest,
                       std::span<char, max_path_size> out) noexcept;

} // namespace vgl::internal::pack

#endif // VGL_PROVIDERS_PACK_READER_HPP
