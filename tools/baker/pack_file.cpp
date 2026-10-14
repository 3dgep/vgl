// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "pack_file.hpp"

#include "decoders/elevation_codec.hpp"

#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace vgl::bake
{

namespace
{

std::string result_name(vgl_result r)
{
    const char* name = nullptr;
    static_cast<void>(vgl_result_name_get(r, &name));
    return name;
}

void check(vgl_result r, const std::filesystem::path& path, const char* what)
{
    if (r != VGL_RESULT_SUCCESS)
    {
        throw std::runtime_error(path.string() + ": " + what + " failed with " + result_name(r));
    }
}

} // namespace

std::vector<std::byte> read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot open " + path.string());
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in)
    {
        throw std::runtime_error("cannot read " + path.string());
    }
    return bytes;
}

pack_file::pack_file(std::filesystem::path directory)
    : directory_(std::move(directory)),
      decompressor_(std::make_unique<vgl::internal::zstd_decompressor>(allocator_)),
      manifest_(std::make_unique<pack::manifest>())
{
    const std::filesystem::path path = directory_ / pack::manifest_file_name;
    const std::vector<std::byte> bytes = read_file(path);
    check(pack::manifest::parse(bytes, allocator_, *decompressor_, *manifest_),
          path,
          "reading the manifest");
}

pack_file::~pack_file() = default;

std::vector<pack_file::shard_info> pack_file::shards() const
{
    std::vector<shard_info> result;
    std::array<char, pack::max_path_size> name{};
    if (manifest_->has_base())
    {
        static_cast<void>(pack::base_shard_path(manifest_->base_digest(), name));
        result.push_back({{true, 0, 0}, directory_ / name.data(), manifest_->base_file_size()});
    }
    for (std::uint32_t i = 0; i < manifest_->shard_count(); ++i)
    {
        const pack::shard_entry e = manifest_->shard(i);
        static_cast<void>(pack::shard_path(e.x, e.y, e.digest, name));
        result.push_back({{false, e.x, e.y}, directory_ / name.data(), e.file_size});
    }
    return result;
}

std::optional<pack_file::shard_info> pack_file::shard_for(const tile_id& t) const
{
    const pack::layer_description& layer = manifest_->layer();
    if (t.level < layer.min_level || t.level > layer.max_level)
    {
        return std::nullopt;
    }
    std::array<char, pack::max_path_size> name{};
    const vgl::internal::shard_location loc = vgl::internal::tile_shard(t, layer.shard_level);
    if (loc.in_base)
    {
        if (!manifest_->has_base())
        {
            return std::nullopt;
        }
        static_cast<void>(pack::base_shard_path(manifest_->base_digest(), name));
        return shard_info{{true, 0, 0}, directory_ / name.data(), manifest_->base_file_size()};
    }
    pack::shard_entry e;
    if (!manifest_->find_shard(loc.root.x, loc.root.y, e))
    {
        return std::nullopt;
    }
    static_cast<void>(pack::shard_path(e.x, e.y, e.digest, name));
    return shard_info{{false, e.x, e.y}, directory_ / name.data(), e.file_size};
}

std::unique_ptr<pack_file::open_shard> pack_file::open(const shard_info& shard) const
{
    auto s = std::make_unique<open_shard>();
    s->file = read_file(shard.path);
    check(pack::parse_shard_header(s->file, *manifest_, shard.place, s->header),
          shard.path,
          "reading the shard header");
    if (s->file.size() != s->header.file_size)
    {
        throw std::runtime_error(shard.path.string() + ": the file is " +
                                 std::to_string(s->file.size()) + " bytes, but its header says " +
                                 std::to_string(s->header.file_size));
    }
    const pack::byte_range dr = pack::directory_range(s->header);
    check(pack::shard_directory::parse(std::span(s->file).subspan(dr.offset, dr.size),
                                       s->header,
                                       *manifest_,
                                       allocator_,
                                       *decompressor_,
                                       s->directory),
          shard.path,
          "reading the shard directory");
    return s;
}

std::span<const std::byte>
pack_file::decode(const open_shard& shard, const pack::directory_entry& e, decoded_tile& out) const
{
    const pack::layer_description& layer = manifest_->layer();
    const pack::byte_range tr = shard.directory.tile_range(e);
    const std::span<const std::byte> bytes = std::span(shard.file).subspan(tr.offset, tr.size);
    out.id = vgl::internal::tile_from_key(e.key);
    check(pack::check_tile(bytes, e), directory_, "checking a tile CRC");
    check(pack::parse_tile_header(bytes, *manifest_, out.header),
          directory_,
          "reading a tile header");
    out.q.clear();
    if (pack::has_heights(layer.type))
    {
        const std::size_t count = static_cast<std::size_t>(layer.tile_size) * layer.tile_size;
        out.q.resize(count);
        std::vector<std::byte> scratch(2 * count);
        check(pack::decode_height_tile(
                  bytes, out.header, *manifest_, out.id.level, *decompressor_, out.q, scratch),
              directory_,
              "decoding a tile");
    }
    return bytes;
}

void pack_file::for_each_tile(
    const shard_info& shard,
    const std::function<void(const decoded_tile&, std::span<const std::byte>)>& visit) const
{
    const std::unique_ptr<open_shard> s = open(shard);
    decoded_tile tile;
    for (std::uint32_t i = 0; i < s->directory.tile_count(); ++i)
    {
        const std::span<const std::byte> bytes = decode(*s, s->directory.entry(i), tile);
        visit(tile, bytes);
    }
}

struct height_query::shard_cache
{
    std::map<std::filesystem::path, std::unique_ptr<pack_file::open_shard>> shards;
    std::map<std::uint64_t, decoded_tile> tiles;
};

height_query::height_query(const pack_file& p) : pack_(p), cache_(std::make_unique<shard_cache>())
{
    if (!pack::has_heights(p.manifest().layer().type))
    {
        throw std::runtime_error(p.directory().string() + " has no heights to query");
    }
}

height_query::~height_query() = default;

height_query::result height_query::height(double latitude, double longitude)
{
    const pack::layer_description& layer = pack_.manifest().layer();
    const std::uint32_t n = layer.tile_size;
    for (std::uint32_t level = layer.max_level + 1; level-- > layer.min_level;)
    {
        const tile_id t = vgl::internal::tile_at(latitude, longitude, level);
        const std::uint64_t key = vgl::internal::tile_key(t);
        auto tile = cache_->tiles.find(key);
        if (tile == cache_->tiles.end())
        {
            const std::optional<pack_file::shard_info> shard = pack_.shard_for(t);
            if (!shard)
            {
                continue;
            }
            auto& open = cache_->shards[shard->path];
            if (!open)
            {
                open = pack_.open(*shard);
            }
            pack::directory_entry e;
            if (!open->directory.find(t, e))
            {
                continue;
            }
            decoded_tile d;
            static_cast<void>(pack_.decode(*open, e, d));
            tile = cache_->tiles.emplace(key, std::move(d)).first;
        }
        const decoded_tile& d = tile->second;
        bool has_nodata = false;
        const double h = vgl::internal::tile_interpolate(
            t, n, latitude, longitude, [&](std::uint32_t i, std::uint32_t j) {
                const std::size_t k = static_cast<std::size_t>(j) * n + i;
                has_nodata = has_nodata || d.q[k] == vgl::internal::elevation_nodata;
                return d.height(k);
            });
        if (!has_nodata)
        {
            return {h, level};
        }
    }
    return {std::numeric_limits<double>::quiet_NaN(), 0};
}

geoid_model::geoid_model(const std::filesystem::path& pack_directory)
{
    const pack_file p(pack_directory);
    const pack::layer_description& layer = p.manifest().layer();
    if (layer.type != pack::layer_type::geoid)
    {
        throw std::runtime_error(pack_directory.string() + " is not a geoid pack");
    }
    level_ = layer.max_level;
    size_ = layer.tile_size;
    tiles_.resize(static_cast<std::size_t>(vgl::internal::tile_columns(level_)) *
                  vgl::internal::tile_rows(level_));
    std::size_t found = 0;
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte>) {
            if (t.id.level == level_)
            {
                tiles_[static_cast<std::size_t>(t.id.y) * vgl::internal::tile_columns(level_) +
                       t.id.x] = t;
                ++found;
            }
        });
    }
    if (found != tiles_.size())
    {
        throw std::runtime_error(pack_directory.string() +
                                 ": a geoid pack must cover the globe at its deepest level");
    }
}

double geoid_model::height(double latitude, double longitude) const noexcept
{
    const tile_id t = vgl::internal::tile_at(latitude, longitude, level_);
    const decoded_tile& tile =
        tiles_[static_cast<std::size_t>(t.y) * vgl::internal::tile_columns(level_) + t.x];
    return vgl::internal::tile_interpolate(
        t, size_, latitude, longitude, [&](std::uint32_t i, std::uint32_t j) {
            return tile.height(static_cast<std::size_t>(j) * size_ + i);
        });
}

} // namespace vgl::bake
