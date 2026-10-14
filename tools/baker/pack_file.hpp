// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Reading a pack directory from disk with the runtime's readers. Used by the baker's geoid
// model, and by the inspect and validate commands.

#ifndef VGL_BAKER_PACK_FILE_HPP
#define VGL_BAKER_PACK_FILE_HPP

#include "providers/pack_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace vgl::bake
{

namespace pack = vgl::internal::pack;
using vgl::internal::tile_id;

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path);

// A decoded tile with heights (elevation or geoid).
struct decoded_tile
{
    tile_id id;
    pack::tile_header header;
    std::vector<std::int16_t> q;

    [[nodiscard]] double height(std::size_t k) const noexcept
    {
        return header.offset + header.scale * q[k];
    }
};

// An open pack: the manifest, with access to every shard. Throws std::runtime_error on any
// failure, naming the file and the result code.
class pack_file
{
public:
    explicit pack_file(std::filesystem::path directory);
    ~pack_file();
    pack_file(const pack_file&) = delete;
    pack_file& operator=(const pack_file&) = delete;
    pack_file(pack_file&&) = delete;
    pack_file& operator=(pack_file&&) = delete;

    [[nodiscard]] const pack::manifest& manifest() const noexcept { return *manifest_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    struct shard_info
    {
        pack::shard_place place;
        std::filesystem::path path;
        std::uint64_t file_size = 0;
    };

    // Every shard of the pack: the base shard first, if any.
    [[nodiscard]] std::vector<shard_info> shards() const;

    // The shard that would hold a tile, if the pack has that shard.
    [[nodiscard]] std::optional<shard_info> shard_for(const tile_id& t) const;

    // A shard read into memory, with its header and directory checked.
    struct open_shard
    {
        std::vector<std::byte> file;
        pack::shard_header header;
        pack::shard_directory directory;
    };

    [[nodiscard]] std::unique_ptr<open_shard> open(const shard_info& shard) const;

    // Checks a tile's CRC and decodes it (the samples only for packs with heights). Returns
    // the tile's bytes.
    std::span<const std::byte>
    decode(const open_shard& shard, const pack::directory_entry& e, decoded_tile& out) const;

    // Calls visit for every tile of a shard, in key order, after checking and decoding it.
    void for_each_tile(
        const shard_info& shard,
        const std::function<void(const decoded_tile&, std::span<const std::byte>)>& visit) const;

private:
    std::filesystem::path directory_;
    vgl::internal::allocator allocator_;
    std::unique_ptr<vgl::internal::zstd_decompressor> decompressor_;
    std::unique_ptr<pack::manifest> manifest_;
};

// Heights from any pack with heights, at the deepest level that has a tile at the position,
// interpolated as docs/pack-format.md, section 6.4 describes. Shards are cached once read.
class height_query
{
public:
    explicit height_query(const pack_file& p);
    ~height_query();
    height_query(const height_query&) = delete;
    height_query& operator=(const height_query&) = delete;
    height_query(height_query&&) = delete;
    height_query& operator=(height_query&&) = delete;

    struct result
    {
        double height = 0.0; // NaN if no tile has data at the position.
        std::uint32_t level = 0;
    };

    [[nodiscard]] result height(double latitude, double longitude);

private:
    struct shard_cache;
    const pack_file& pack_;
    std::unique_ptr<shard_cache> cache_;
};

// The geoid height N, from a geoid pack held in memory (docs/pack-format.md, section 6.4).
class geoid_model
{
public:
    explicit geoid_model(const std::filesystem::path& pack_directory);

    // Geoid height in metres at a position in degrees.
    [[nodiscard]] double height(double latitude, double longitude) const noexcept;

private:
    std::uint32_t level_ = 0;
    std::uint32_t size_ = 0;
    std::vector<decoded_tile> tiles_; // Every tile of the deepest level, indexed by row and column.
};

} // namespace vgl::bake

#endif // VGL_BAKER_PACK_FILE_HPP
