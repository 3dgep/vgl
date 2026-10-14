// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "inspect.hpp"

#include "decoders/elevation_codec.hpp"
#include "pack_file.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vgl::bake
{

namespace
{

const char* layer_name(pack::layer_type t)
{
    switch (t)
    {
    case pack::layer_type::elevation:
        return "elevation";
    case pack::layer_type::imagery:
        return "imagery";
    case pack::layer_type::geoid:
        return "geoid";
    }
    return "unknown";
}

struct level_stats
{
    std::uint64_t tiles = 0;
    std::uint64_t bytes = 0;
    std::uint64_t partial = 0;
};

// The border samples of a tile, kept to check seams between neighbours.
struct tile_edges
{
    std::vector<double> north;
    std::vector<double> south;
    std::vector<double> west;
    std::vector<double> east;
    std::uint8_t child_mask = 0;
    std::array<float, 4> child_min = {};
    std::array<float, 4> child_max = {};
    double min = 0.0;
    double max = 0.0;
};

// Two border samples agree if both are nodata or both decode to the same height.
bool same_sample(double a, double b)
{
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

} // namespace

void inspect(const std::filesystem::path& pack_directory, std::ostream& out)
{
    const pack_file p(pack_directory);
    const pack::manifest& m = p.manifest();
    const pack::layer_description& l = m.layer();
    out << "pack            " << pack_directory.string() << '\n'
        << "format          " << pack::major_version << '.' << m.minor_version() << '\n'
        << "layer           " << layer_name(l.type) << '\n'
        << "tile size       " << l.tile_size << '\n'
        << "levels          " << int{l.min_level} << " to " << int{l.max_level} << '\n'
        << "shard level     " << int{l.shard_level} << '\n'
        << "bounds          " << l.west << ", " << l.south << ", " << l.east << ", " << l.north
        << '\n';
    for (std::uint32_t i = 0; i < m.metadata_count(); ++i)
    {
        const pack::metadata_entry e = m.metadata(i);
        out << std::left << std::setw(16) << e.key << e.value << '\n';
    }

    std::map<std::uint32_t, level_stats> levels;
    std::vector<std::pair<tile_id, std::uint64_t>> tile_sizes;
    std::uint64_t total_bytes = 0;
    std::uint64_t shard_count = 0;
    for (const pack_file::shard_info& shard : p.shards())
    {
        ++shard_count;
        total_bytes += shard.file_size;
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte> bytes) {
            level_stats& s = levels[t.id.level];
            ++s.tiles;
            s.bytes += bytes.size();
            if ((t.header.flags & pack::elevation_flag_has_nodata) != 0)
            {
                ++s.partial;
            }
            tile_sizes.emplace_back(t.id, bytes.size());
        });
    }

    out << std::right << "shards          " << shard_count
        << (m.has_base() ? " (including the base shard)" : "") << '\n'
        << "total size      " << std::fixed << std::setprecision(1)
        << static_cast<double>(total_bytes) / 1e6 << " MB\n\n"
        << "level      tiles   partial      size MB   bytes/tile  bits/sample";
    if (pack::has_heights(l.type))
    {
        out << "   step m";
    }
    out << '\n';
    const double samples = static_cast<double>(l.tile_size) * l.tile_size;
    for (const auto& [level, s] : levels)
    {
        out << std::setw(5) << level << std::setw(11) << s.tiles << std::setw(10) << s.partial
            << std::setw(13) << std::setprecision(1) << static_cast<double>(s.bytes) / 1e6
            << std::setw(13) << std::setprecision(0)
            << static_cast<double>(s.bytes) / static_cast<double>(s.tiles) << std::setw(13)
            << std::setprecision(2)
            << 8.0 * static_cast<double>(s.bytes) / (static_cast<double>(s.tiles) * samples);
        if (pack::has_heights(l.type))
        {
            out << std::setw(9) << std::setprecision(4) << m.height_step(level);
        }
        out << '\n';
    }

    // What other shard levels would give for the same tiles: file counts and sizes. Shard
    // sizes are the tile bytes plus the header and an estimated compressed directory.
    out << "\nshard level   shards   median MB      max MB   base MB\n";
    for (std::uint32_t s = l.min_level; s <= l.max_level; ++s)
    {
        std::map<std::uint64_t, std::uint64_t> shard_bytes;
        std::uint64_t base = 0;
        constexpr std::uint64_t per_tile_overhead = pack::directory_entry_size / 2;
        for (const auto& [t, size] : tile_sizes)
        {
            if (t.level < s)
            {
                base += size + per_tile_overhead;
            }
            else
            {
                shard_bytes[vgl::internal::tile_key(vgl::internal::tile_ancestor(t, s))] +=
                    size + per_tile_overhead;
            }
        }
        std::vector<std::uint64_t> sizes;
        sizes.reserve(shard_bytes.size());
        for (const auto& [key, bytes] : shard_bytes)
        {
            sizes.push_back(bytes + pack::shard_header_size);
        }
        std::sort(sizes.begin(), sizes.end());
        const double median =
            sizes.empty() ? 0.0 : static_cast<double>(sizes[sizes.size() / 2]) / 1e6;
        const double largest = sizes.empty() ? 0.0 : static_cast<double>(sizes.back()) / 1e6;
        out << std::setw(11) << s << std::setw(9) << sizes.size() << std::setw(12)
            << std::setprecision(2) << median << std::setw(12) << largest << std::setw(10)
            << static_cast<double>(base) / 1e6 << (s == l.shard_level ? "   (this pack)" : "")
            << '\n';
    }
    out.unsetf(std::ios::fixed);
}

validation_report validate(const std::filesystem::path& pack_directory, std::ostream& out)
{
    const pack_file p(pack_directory);
    const pack::manifest& m = p.manifest();
    const pack::layer_description& l = m.layer();
    if (!pack::has_heights(l.type))
    {
        throw std::runtime_error("validate supports elevation and geoid packs");
    }
    const std::uint32_t n = l.tile_size;

    // Reading every tile checks every CRC, structure and encoding.
    std::map<std::uint64_t, tile_edges> tiles;
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte>) {
            tile_edges e;
            e.north.resize(n);
            e.south.resize(n);
            e.west.resize(n);
            e.east.resize(n);
            e.min = std::numeric_limits<double>::infinity();
            e.max = -std::numeric_limits<double>::infinity();
            const auto value = [&](std::uint32_t i, std::uint32_t j) {
                const std::size_t k = static_cast<std::size_t>(j) * n + i;
                return t.q[k] == vgl::internal::elevation_nodata
                           ? std::numeric_limits<double>::quiet_NaN()
                           : t.height(k);
            };
            for (std::uint32_t k = 0; k < n; ++k)
            {
                e.north[k] = value(k, 0);
                e.south[k] = value(k, n - 1);
                e.west[k] = value(0, k);
                e.east[k] = value(n - 1, k);
            }
            for (std::size_t k = 0; k < t.q.size(); ++k)
            {
                if (t.q[k] != vgl::internal::elevation_nodata)
                {
                    e.min = std::min(e.min, t.height(k));
                    e.max = std::max(e.max, t.height(k));
                }
            }
            e.child_mask = t.header.child_mask;
            e.child_min = t.header.child_min;
            e.child_max = t.header.child_max;
            tiles.emplace(vgl::internal::tile_key(t.id), std::move(e));
        });
    }

    validation_report r;
    std::map<std::uint32_t, std::uint64_t> mismatches_by_level;
    r.tiles = tiles.size();
    std::uint64_t reported = 0;
    const auto report = [&](const tile_id& t, const std::string& what) {
        if (++reported <= 20)
        {
            out << "  tile " << t.level << '/' << t.x << '/' << t.y << ": " << what << '\n';
        }
    };
    // Describes the differences along a shared border.
    const auto border_difference = [&](const std::vector<double>& a, const std::vector<double>& b) {
        double worst = 0.0;
        std::uint32_t at = 0;
        std::uint32_t count = 0;
        for (std::uint32_t k = 0; k < n; ++k)
        {
            if (!same_sample(a[k], b[k]))
            {
                ++count;
                const double d = std::isnan(a[k]) || std::isnan(b[k])
                                     ? std::numeric_limits<double>::infinity()
                                     : std::fabs(a[k] - b[k]);
                if (d > worst)
                {
                    worst = d;
                    at = k;
                }
            }
        }
        std::ostringstream os;
        os.precision(10);
        os << count << " of " << n << " samples differ, by up to " << worst << " m (at sample "
           << at << ": " << a[at] << " and " << b[at] << ")";
        return os.str();
    };
    for (const auto& [key, e] : tiles)
    {
        const tile_id t = vgl::internal::tile_from_key(key);

        // Shared borders with the eastern and southern neighbours, across the antimeridian too.
        const auto east = tiles.find(vgl::internal::tile_key(vgl::internal::tile_east(t)));
        if (east != tiles.end() && vgl::internal::tile_columns(t.level) > 1)
        {
            for (std::uint32_t k = 0; k < n; ++k)
            {
                if (!same_sample(e.east[k], east->second.west[k]))
                {
                    ++r.border_mismatches;
                    ++mismatches_by_level[t.level];
                    report(t, "eastern border: " + border_difference(e.east, east->second.west));
                    break;
                }
            }
        }
        if (t.y + 1 < vgl::internal::tile_rows(t.level))
        {
            const auto south = tiles.find(vgl::internal::tile_key({t.level, t.x, t.y + 1}));
            if (south != tiles.end())
            {
                for (std::uint32_t k = 0; k < n; ++k)
                {
                    if (!same_sample(e.south[k], south->second.north[k]))
                    {
                        ++r.border_mismatches;
                        ++mismatches_by_level[t.level];
                        report(t,
                               "southern border: " +
                                   border_difference(e.south, south->second.north));
                        break;
                    }
                }
            }
        }

        // Pole rows: every sample is the same point.
        const auto uniform = [](const std::vector<double>& row) {
            return std::all_of(
                row.begin(), row.end(), [&](double v) { return same_sample(v, row[0]); });
        };
        if ((t.y == 0 && !uniform(e.north)) ||
            (t.y == vgl::internal::tile_rows(t.level) - 1 && !uniform(e.south)))
        {
            ++r.pole_mismatches;
            report(t, "pole row is not uniform");
        }

        // Child mask and child bounds.
        if (t.level < l.max_level)
        {
            for (std::uint32_t c = 0; c < 4; ++c)
            {
                const auto child =
                    tiles.find(vgl::internal::tile_key(vgl::internal::tile_child(t, c)));
                const bool exists = child != tiles.end();
                if (exists != (((e.child_mask >> c) & 1u) != 0))
                {
                    ++r.child_mask_mismatches;
                    report(t, "child mask disagrees with the tiles in the pack");
                }
                else if (exists &&
                         (child->second.min < e.child_min[c] || child->second.max > e.child_max[c]))
                {
                    ++r.child_bound_violations;
                    report(t, "a child lies outside its recorded height bounds");
                }
            }
        }
    }

    if (!mismatches_by_level.empty())
    {
        out << "border mismatches by level:";
        for (const auto& [level, count] : mismatches_by_level)
        {
            out << ' ' << level << ": " << count;
        }
        out << '\n';
    }
    out << r.tiles << " tiles read and decoded; " << r.border_mismatches << " border mismatches, "
        << r.pole_mismatches << " pole mismatches, " << r.child_mask_mismatches
        << " child mask mismatches, " << r.child_bound_violations << " child bound violations\n";
    return r;
}

} // namespace vgl::bake
