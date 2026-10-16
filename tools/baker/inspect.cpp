// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "inspect.hpp"

#include "decoders/elevation_codec.hpp"
#include "image_codec.hpp"
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

// A line of RGBA pixels: a row or column of an imagery tile.
using pixel_line = std::vector<std::array<std::uint8_t, 4>>;

// The pixel lines of an imagery tile that its neighbours repeat or are repeated by: the
// outermost lines (the border, when there is one) and the tile's own edge lines inside them.
struct image_edges
{
    std::array<pixel_line, 4> outer; // North, south, west, east.
    std::array<pixel_line, 4> inner;
    std::uint8_t child_mask = 0;
};

enum edge : std::uint8_t
{
    north_edge,
    south_edge,
    west_edge,
    east_edge,
};

// The mean difference per channel between two lines, over the pixels with data in both, or 0
// if none has.
double
line_difference(const pixel_line& a, const pixel_line& b, std::uint32_t begin, std::uint32_t end)
{
    double sum = 0.0;
    std::uint32_t pixels = 0;
    for (std::uint32_t k = begin; k < end; ++k)
    {
        if (a[k][3] == 0 || b[k][3] == 0)
        {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c)
        {
            sum += std::abs(static_cast<int>(a[k][c]) - static_cast<int>(b[k][c]));
        }
        ++pixels;
    }
    return pixels == 0 ? 0.0 : sum / (3.0 * pixels);
}

// Border pixels repeat their neighbours exactly before encoding; JPEG then changes both a
// little, differently on either side.
constexpr double max_mean_border_difference = 12.0;

validation_report validate_imagery(const pack_file& p, std::ostream& out)
{
    const pack::layer_description& l = p.manifest().layer();
    const std::uint32_t size = l.tile_size;
    const std::uint32_t b = l.border;
    const std::uint32_t n = size + 2 * b;

    validation_report r;
    std::uint64_t reported = 0;
    const auto report = [&](const tile_id& t, const std::string& what) {
        if (++reported <= 20)
        {
            out << "  tile " << t.level << '/' << t.x << '/' << t.y << ": " << what << '\n';
        }
    };

    std::map<std::uint64_t, image_edges> tiles;
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte> bytes) {
            image img;
            try
            {
                img = decode_image(bytes.subspan(t.header.header_size));
            }
            catch (const std::exception& e)
            {
                ++r.image_errors;
                report(t.id, e.what());
                return;
            }
            if (img.width != n || img.height != n)
            {
                ++r.image_errors;
                report(t.id,
                       "the image is " + std::to_string(img.width) + " x " +
                           std::to_string(img.height) + " pixels, not " + std::to_string(n) +
                           " x " + std::to_string(n));
                return;
            }
            const auto pixel = [&](std::uint32_t i, std::uint32_t j) {
                std::array<std::uint8_t, 4> v{0, 0, 0, 255};
                const std::size_t k = (static_cast<std::size_t>(j) * n + i) * img.channels;
                for (std::uint32_t c = 0; c < 3; ++c)
                {
                    v[c] = img.pixels[k + (img.channels < 3 ? 0 : c)];
                }
                if (img.channels == 4)
                {
                    v[3] = img.pixels[k + 3];
                }
                return v;
            };
            image_edges e;
            e.child_mask = t.header.child_mask;
            for (auto& line : e.outer)
            {
                line.resize(n);
            }
            for (auto& line : e.inner)
            {
                line.resize(n);
            }
            for (std::uint32_t k = 0; k < n; ++k)
            {
                e.outer[north_edge][k] = pixel(k, 0);
                e.outer[south_edge][k] = pixel(k, n - 1);
                e.outer[west_edge][k] = pixel(0, k);
                e.outer[east_edge][k] = pixel(n - 1, k);
                e.inner[north_edge][k] = pixel(k, b);
                e.inner[south_edge][k] = pixel(k, n - 1 - b);
                e.inner[west_edge][k] = pixel(b, k);
                e.inner[east_edge][k] = pixel(n - 1 - b, k);
            }
            tiles.emplace(vgl::internal::tile_key(t.id), std::move(e));
        });
    }
    r.tiles = tiles.size() + r.image_errors;

    double worst = 0.0;
    for (const auto& [key, e] : tiles)
    {
        const tile_id t = vgl::internal::tile_from_key(key);

        // With a border, each tile's border repeats its neighbour's edge, both ways.
        if (b != 0)
        {
            const auto check =
                [&](const image_edges& other, edge mine, edge theirs, const char* side) {
                    const double d =
                        std::max(line_difference(e.outer[mine], other.inner[theirs], b, b + size),
                                 line_difference(e.inner[mine], other.outer[theirs], b, b + size));
                    worst = std::max(worst, d);
                    if (d > max_mean_border_difference)
                    {
                        ++r.border_mismatches;
                        std::ostringstream os;
                        os.precision(3);
                        os << side << " border differs from the neighbour by " << d
                           << " per channel on average";
                        report(t, os.str());
                    }
                };
            const auto east = tiles.find(vgl::internal::tile_key(vgl::internal::tile_east(t)));
            if (east != tiles.end() && vgl::internal::tile_columns(t.level) > 1)
            {
                check(east->second, east_edge, west_edge, "eastern");
            }
            if (t.y + 1 < vgl::internal::tile_rows(t.level))
            {
                const auto south = tiles.find(vgl::internal::tile_key({t.level, t.x, t.y + 1}));
                if (south != tiles.end())
                {
                    check(south->second, south_edge, north_edge, "southern");
                }
            }
            // Beyond a pole, the border repeats the row inside.
            for (const bool north : {true, false})
            {
                if (t.y != (north ? 0 : vgl::internal::tile_rows(t.level) - 1))
                {
                    continue;
                }
                const edge side = north ? north_edge : south_edge;
                if (line_difference(e.outer[side], e.inner[side], 0, n) >
                    max_mean_border_difference)
                {
                    ++r.pole_mismatches;
                    report(t, "the border beyond the pole does not repeat the row inside it");
                }
            }
        }

        if (t.level < l.max_level)
        {
            for (std::uint32_t c = 0; c < 4; ++c)
            {
                const bool exists =
                    tiles.contains(vgl::internal::tile_key(vgl::internal::tile_child(t, c)));
                if (exists != (((e.child_mask >> c) & 1u) != 0))
                {
                    ++r.child_mask_mismatches;
                    report(t, "child mask disagrees with the tiles in the pack");
                }
            }
        }
    }

    out << r.tiles << " tiles read and decoded; " << r.image_errors << " image errors, "
        << r.border_mismatches << " border mismatches (largest mean difference " << worst << "), "
        << r.pole_mismatches << " pole mismatches, " << r.child_mask_mismatches
        << " child mask mismatches\n";
    return r;
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
        << "tile size       " << l.tile_size
        << (l.type == pack::layer_type::imagery ? ", border " + std::to_string(l.border) : "")
        << '\n'
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
            // Imagery: a PNG tile, which the baker writes where data is missing.
            const bool partial =
                pack::has_heights(l.type)
                    ? (t.header.flags & pack::elevation_flag_has_nodata) != 0
                    : t.header.encoding == static_cast<std::uint8_t>(pack::imagery_encoding::png);
            if (partial)
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
    if (l.type == pack::layer_type::imagery)
    {
        return validate_imagery(p, out);
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
