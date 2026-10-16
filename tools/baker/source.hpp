// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Source rasters read through GDAL, resampled onto tile sample grids.
//
// Every tile is warped onto its own grid, with pixel centres exactly on the sample positions of
// docs/pack-format.md, section 2.3, and an exact coordinate transform. Each sample's value is
// then a function of its position only, so neighbouring tiles compute identical values for
// their shared border samples without knowing about each other. This is what lets shards bake
// independently.

#ifndef VGL_BAKER_SOURCE_HPP
#define VGL_BAKER_SOURCE_HPP

#include "bake_config.hpp"
#include "tiling/tile.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vgl::bake
{

using vgl::internal::degree_region;

// Footprint and resolution of one source file.
struct source_file
{
    std::filesystem::path path;
    degree_region bounds;
    double pixel_width = 0.0;  // In degrees.
    double pixel_height = 0.0; // In degrees.
    double pixel_size = 0.0;   // The finer of the two.
    // Pixel sizes of the overviews, finest first, in degrees.
    std::vector<double> overview_pixel_width;
    std::vector<double> overview_pixel_height;
};

// One source of a bake: its files, read on demand.
class source
{
public:
    // Opens every file once to read its footprint, and checks that it is in geographic
    // WGS84 coordinates, and that every file has the same number of bands. Throws
    // std::runtime_error otherwise.
    explicit source(source_config config);

    [[nodiscard]] const source_config& config() const noexcept { return config_; }
    [[nodiscard]] double finest_pixel_size() const noexcept { return finest_pixel_size_; }

    // Bands per file.
    [[nodiscard]] int bands() const noexcept { return bands_; }

    // True if every band of every file holds 8-bit unsigned values, as imagery must.
    [[nodiscard]] bool byte_data() const noexcept { return byte_data_; }

    // The largest pixel dimension of any file, in degrees. A footprint at least this large
    // contains a pixel centre of every file it covers.
    [[nodiscard]] double coarsest_pixel_size() const noexcept { return coarsest_pixel_size_; }

    // True if any file's footprint intersects a region.
    [[nodiscard]] bool covers(const degree_region& r) const noexcept;

    [[nodiscard]] const std::vector<source_file>& files() const noexcept { return files_; }

private:
    source_config config_;
    std::vector<source_file> files_;
    double finest_pixel_size_ = std::numeric_limits<double>::infinity();
    double coarsest_pixel_size_ = 0.0;
    int bands_ = 0;
    bool byte_data_ = true;
};

// Resamples sources onto tile grids. Holds per-thread GDAL handles, so use one per thread.
class sampler
{
public:
    sampler();
    sampler(const sampler&) = delete;
    sampler& operator=(const sampler&) = delete;
    sampler(sampler&&) = delete;
    sampler& operator=(sampler&&) = delete;
    ~sampler();

    // Samples a source on the size x size grid of tile t into values (row-major). Samples
    // without data are NaN. resampling is a GDAL resampling method name; band counts from 1.
    void sample(const source& s,
                const vgl::internal::tile_id& t,
                std::uint32_t size,
                const std::string& resampling,
                std::vector<double>& values,
                int band = 1);

    // Samples a grid of nx x ny positions, the first at (first_longitude, first_latitude) and
    // the others spaced by step eastwards and southwards, on the same terms as sample(): each
    // value depends only on its position and the spacing. Used to make samples that represent
    // the same point (on the antimeridian and at the poles) identical.
    void sample_grid(const source& s,
                     double first_longitude,
                     double first_latitude,
                     double step,
                     std::uint32_t nx,
                     std::uint32_t ny,
                     const std::string& resampling,
                     std::vector<double>& values,
                     int band = 1);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Opens GDAL with the baker's settings. Call once before any other function here.
void initialize_gdal(std::size_t cache_megabytes);

} // namespace vgl::bake

#endif // VGL_BAKER_SOURCE_HPP
