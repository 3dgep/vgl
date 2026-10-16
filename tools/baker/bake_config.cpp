// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "bake_config.hpp"

#include "tiling/tile.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace vgl::bake
{

namespace
{

using nlohmann::json;

[[noreturn]] void fail(const std::filesystem::path& file, const std::string& message)
{
    throw std::runtime_error(file.string() + ": " + message);
}

// Matches a file name against a pattern with * and ? wildcards.
bool wildcard_match(std::string_view pattern, std::string_view name)
{
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (n < name.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]))
        {
            ++p;
            ++n;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            mark = n;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            n = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.size();
}

// Expands a path whose last component may contain wildcards. Relative paths are relative to
// the configuration file. Results are sorted, so bakes are deterministic.
std::vector<std::filesystem::path> expand(const std::filesystem::path& config_file,
                                          const std::string& pattern)
{
    std::filesystem::path path(pattern);
    if (path.is_relative())
    {
        path = config_file.parent_path() / path;
    }
    const std::string name = path.filename().string();
    std::vector<std::filesystem::path> result;
    if (name.find_first_of("*?") == std::string::npos)
    {
        result.push_back(path);
        return result;
    }
    const std::filesystem::path directory = path.parent_path();
    if (!std::filesystem::is_directory(directory))
    {
        fail(config_file, "source directory " + directory.string() + " does not exist");
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.is_regular_file() && wildcard_match(name, entry.path().filename().string()))
        {
            result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> string_list(const json& value)
{
    if (value.is_string())
    {
        return {value.get<std::string>()};
    }
    return value.get<std::vector<std::string>>();
}

} // namespace

double default_height_step(pack::layer_type layer, std::uint32_t level) noexcept
{
    if (layer == pack::layer_type::geoid)
    {
        return 1.0 / 256.0;
    }
    // Ranges of 32,767 m, 16,383 m and 8,191 m. Level-8 tiles (78 km) around Hawaii already
    // span more than 8.5 km from the ocean floor to the summits.
    if (level <= 5)
    {
        return 0.5;
    }
    return level <= 9 ? 0.25 : 0.125;
}

bake_config load_bake_config(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        fail(file, "cannot open the configuration file");
    }
    json j;
    try
    {
        j = json::parse(in, nullptr, true, true);
    }
    catch (const json::parse_error& e)
    {
        fail(file, std::string("invalid JSON: ") + e.what());
    }

    bake_config c;
    try
    {
        if (!j.contains("output"))
        {
            fail(file, "\"output\" is required: the directory the pack is written to");
        }
        c.output = j.at("output").get<std::string>();
        if (c.output.is_relative())
        {
            c.output = file.parent_path() / c.output;
        }
        const std::string layer = j.value("layer", "elevation");
        if (layer == "elevation")
        {
            c.layer = pack::layer_type::elevation;
        }
        else if (layer == "geoid")
        {
            c.layer = pack::layer_type::geoid;
        }
        else if (layer == "imagery")
        {
            c.layer = pack::layer_type::imagery;
            c.tile_size = 256;
            c.border = 1;
        }
        else
        {
            fail(file,
                 R"("layer" must be "elevation", "imagery" or "geoid", not ")" + layer + "\"");
        }
        const bool imagery = c.layer == pack::layer_type::imagery;
        c.tile_size = j.value("tile_size", c.tile_size);
        if (imagery)
        {
            c.border = j.value("border", c.border);
            const std::string encoding = j.value("encoding", "auto");
            if (encoding == "auto")
            {
                c.encoding = image_encoding::automatic;
            }
            else if (encoding == "jpeg")
            {
                c.encoding = image_encoding::jpeg;
            }
            else if (encoding == "png")
            {
                c.encoding = image_encoding::png;
            }
            else
            {
                fail(file,
                     R"("encoding" must be "auto", "jpeg" or "png", not ")" + encoding + "\"");
            }
            c.jpeg_quality = j.value("jpeg_quality", c.jpeg_quality);
        }
        else
        {
            for (const char* key : {"border", "encoding", "jpeg_quality"})
            {
                if (j.contains(key))
                {
                    fail(file, std::string("\"") + key + "\" applies only to imagery");
                }
            }
        }
        c.min_level = j.value("min_level", c.min_level);
        c.max_level = j.value("max_level", c.max_level);
        // By default, shards hold five levels below their root, which gave shards of 9 to
        // 25 MB in the test bake (docs/test-bake.md, section 5).
        c.shard_level = j.value("shard_level", c.max_level > 5 ? c.max_level - 5 : 0u);
        if (j.contains("region"))
        {
            const auto r = j.at("region").get<std::vector<double>>();
            if (r.size() != 4 || r[0] >= r[2] || r[1] >= r[3])
            {
                fail(file,
                     "\"region\" must be [west, south, east, north] in degrees, with west < east "
                     "and south < north");
            }
            c.west = r[0];
            c.south = r[1];
            c.east = r[2];
            c.north = r[3];
        }
        const std::string filter = j.value("filter", "average");
        if (filter == "average")
        {
            c.filter = level_filter::average;
        }
        else if (filter == "maximum")
        {
            c.filter = level_filter::maximum;
        }
        else
        {
            fail(file, R"("filter" must be "average" or "maximum", not ")" + filter + "\"");
        }
        c.zstd_level = j.value("zstd_level", c.zstd_level);
        if (j.contains("height_steps"))
        {
            c.height_steps = j.at("height_steps").get<std::vector<double>>();
        }
        if (j.contains("geoid"))
        {
            c.geoid = j.at("geoid").get<std::string>();
            if (c.geoid.is_relative())
            {
                c.geoid = file.parent_path() / c.geoid;
            }
        }
        if (j.contains("metadata"))
        {
            for (const auto& [key, value] : j.at("metadata").items())
            {
                for (const std::string& v : string_list(value))
                {
                    c.metadata.emplace_back(key, v);
                }
            }
        }
        for (const json& s : j.at("sources"))
        {
            source_config src;
            src.name =
                s.value("name", std::string("source ") + std::to_string(c.sources.size() + 1));
            for (const std::string& pattern : string_list(s.at("files")))
            {
                const auto files = expand(file, pattern);
                src.files.insert(src.files.end(), files.begin(), files.end());
            }
            if (src.files.empty())
            {
                fail(file, "source \"" + src.name + "\" matches no files");
            }
            const std::string datum = s.value("datum", "ellipsoid");
            if (datum == "ellipsoid")
            {
                src.datum = vertical_datum::ellipsoid;
            }
            else if (datum == "egm2008")
            {
                src.datum = vertical_datum::egm2008;
            }
            else
            {
                fail(file,
                     "source \"" + src.name +
                         R"(": "datum" must be "ellipsoid" or "egm2008", not ")" + datum + "\"");
            }
            src.max_level = s.value("max_level", c.max_level);
            src.resampling = s.value("resampling", src.resampling);
            c.sources.push_back(std::move(src));
        }
    }
    catch (const json::exception& e)
    {
        fail(file, std::string("invalid configuration: ") + e.what());
    }

    // Checks against the pack format (docs/pack-format.md, section 4.3).
    const bool imagery = c.layer == pack::layer_type::imagery;
    if (imagery)
    {
        const bool power_of_two = (c.tile_size & (c.tile_size - 1)) == 0;
        if (!power_of_two || c.tile_size < 64 || c.tile_size > 2048)
        {
            fail(file, "\"tile_size\" of imagery must be a power of two from 64 to 2048");
        }
        if (c.border > 1)
        {
            fail(file, "\"border\" must be 0 or 1");
        }
        if (c.jpeg_quality < 1 || c.jpeg_quality > 100)
        {
            fail(file, "\"jpeg_quality\" must be from 1 to 100");
        }
        if (c.filter != level_filter::average || !c.height_steps.empty() || !c.geoid.empty())
        {
            fail(file, R"("filter", "height_steps" and "geoid" apply only to heights)");
        }
        if (std::any_of(c.sources.begin(), c.sources.end(), [](const source_config& s) {
                return s.datum != vertical_datum::ellipsoid;
            }))
        {
            fail(file, "\"datum\" applies only to heights");
        }
    }
    else if (!vgl::internal::elevation_tile_size_is_valid(c.tile_size))
    {
        fail(file, "\"tile_size\" must be 2^k + 1 with 2 <= k <= 10, such as 65, 129 or 257");
    }
    if (c.min_level > c.max_level || c.max_level > vgl::internal::max_tile_level)
    {
        fail(file, "levels must satisfy min_level <= max_level <= 28");
    }
    if (c.shard_level > vgl::internal::max_tile_level + 1)
    {
        fail(file, "\"shard_level\" must be at most 29");
    }
    if (c.sources.empty())
    {
        fail(file, "\"sources\" must list at least one source");
    }
    if (imagery)
    {
        return c;
    }
    if (c.height_steps.empty())
    {
        for (std::uint32_t level = c.min_level; level <= c.max_level; ++level)
        {
            c.height_steps.push_back(default_height_step(c.layer, level));
        }
    }
    else if (c.height_steps.size() != c.max_level - c.min_level + 1)
    {
        fail(file, "\"height_steps\" must have one step per level from min_level to max_level");
    }
    const bool needs_geoid =
        std::any_of(c.sources.begin(), c.sources.end(), [](const source_config& s) {
            return s.datum == vertical_datum::egm2008;
        });
    if (needs_geoid && c.geoid.empty())
    {
        fail(file, "sources referenced to egm2008 need \"geoid\": the path of a geoid pack");
    }
    return c;
}

} // namespace vgl::bake
