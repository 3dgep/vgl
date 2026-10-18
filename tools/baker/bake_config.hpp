// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Bake configuration: the JSON file that lists sources, priorities, datums and output settings
// (design document, section 8.2). See tools/baker/README.md for the format.

#ifndef VGL_BAKER_BAKE_CONFIG_HPP
#define VGL_BAKER_BAKE_CONFIG_HPP

#include "providers/pack_format.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace vgl::bake
{

namespace pack = vgl::internal::pack;

// Height reference of a source's values (design document, section 8.3).
enum class vertical_datum : std::uint8_t
{
    ellipsoid, // Heights above the WGS84 ellipsoid, or geoid heights for a geoid source.
    egm2008,   // Heights above the EGM2008 geoid, such as Copernicus DEM. GEBCO uses this too.
};

// How coarser levels are filtered (design document, section 8.4).
enum class level_filter : std::uint8_t
{
    average,
    maximum, // Terrain never appears lower than it is, for aviation.
};

// How imagery tiles are encoded (docs/pack-format.md, section 6.3).
enum class image_encoding : std::uint8_t
{
    automatic, // JPEG for tiles with data everywhere, PNG with alpha for the others.
    jpeg,      // JPEG everywhere: tiles without data everywhere are filled with black.
    png,       // PNG everywhere, with alpha only where a tile lacks data.
};

struct source_config
{
    std::string name;
    std::vector<std::filesystem::path> files;
    vertical_datum datum = vertical_datum::ellipsoid;
    std::uint32_t max_level = 28;        // Finest level this source contributes to.
    std::string resampling = "bilinear"; // GDAL resampling near the source resolution.
    // Heights only: within this distance in metres of where the pack's data from this source
    // ends, its sea, exactly 0 m in its datum, eases into the later sources' heights below sea
    // level, so that it meets them without a step at the edge. 0 leaves the sea as it is.
    double sea_blend = 0.0;
    // Fills only the gaps of tiles that earlier sources have files over, and adds no tiles of
    // its own, as GEBCO does under a regional GLO-30 pack.
    bool fill_only = false;
};

struct bake_config
{
    std::filesystem::path output;
    pack::layer_type layer = pack::layer_type::elevation;
    std::uint32_t tile_size = 129; // Imagery: 256.
    std::uint32_t border = 0;      // Imagery: 1 (docs/pack-format.md, section 2.4).
    image_encoding encoding = image_encoding::automatic;
    int jpeg_quality = 85;
    std::uint32_t min_level = 0;
    std::uint32_t max_level = 12;
    std::uint32_t shard_level = 7; // Default: max_level - 5.
    // Region in degrees: west, south, east, north.
    double west = -180.0;
    double south = -90.0;
    double east = 180.0;
    double north = 90.0;
    level_filter filter = level_filter::average;
    std::vector<double> height_steps; // One per level from min_level; empty for imagery.
    int zstd_level = 19;
    std::filesystem::path geoid; // Geoid pack, needed for egm2008 sources.
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<source_config> sources; // Highest priority first.
    // The configuration as read, without comments or spacing: a resumed bake must match it.
    std::string settings;
};

// Reads and checks a configuration file. Throws std::runtime_error with a message that names
// the problem.
[[nodiscard]] bake_config load_bake_config(const std::filesystem::path& file);

// Default quantization steps (docs/pack-format.md, section 6.2 and 6.4).
[[nodiscard]] double default_height_step(pack::layer_type layer, std::uint32_t level) noexcept;

} // namespace vgl::bake

#endif // VGL_BAKER_BAKE_CONFIG_HPP
