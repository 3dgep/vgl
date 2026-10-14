// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The bake pipeline (design document, section 8.2): sources are resampled tile by tile,
// converted to ellipsoidal heights, quantized, encoded and written as sharded packs.
//
// Work is partitioned by shard. Each level-S shard bakes its subtree independently, children
// before parents so that parents can record their children's existence and height bounds.
// The base shard follows, using the shard roots' records. Finished shards are recorded, so an
// interrupted bake can resume, and every file is written atomically.

#ifndef VGL_BAKER_BAKER_HPP
#define VGL_BAKER_BAKER_HPP

#include "bake_config.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vgl::bake
{

struct bake_options
{
    unsigned jobs = 0;    // Worker threads; 0 for the number of cores.
    bool resume = false;  // Continue a bake, keeping finished shards.
    bool verbose = false; // Report every shard.
    std::size_t gdal_cache_megabytes = 2048;
    std::optional<std::uint32_t> max_level; // Preview: stop at this level.
};

struct bake_summary
{
    std::uint64_t tiles = 0;
    std::uint64_t shards = 0;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
};

// Bakes a pack. Throws std::runtime_error on failure; finished shards are kept for --resume.
bake_summary bake(const bake_config& config, const bake_options& options);

// Computes one tile's ellipsoidal heights as a bake would, before quantization: tile_size^2
// values in row-major order, NaN without data. For diagnostics.
[[nodiscard]] std::vector<double> compute_tile_heights(const bake_config& config,
                                                       std::uint32_t level,
                                                       std::uint32_t x,
                                                       std::uint32_t y);

} // namespace vgl::bake

#endif // VGL_BAKER_BAKER_HPP
