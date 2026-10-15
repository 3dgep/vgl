// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The context and pack objects behind vgl/context.h.
//
// Everything here is touched only on the thread that calls into the context, except decode
// jobs while a task runs them; those carry copies of what they need, so that a pack can be
// destroyed while its tiles decode.

#ifndef VGL_CONTEXT_CONTEXT_IMPL_HPP
#define VGL_CONTEXT_CONTEXT_IMPL_HPP

#include "decoders/zstd_decompressor.hpp"
#include "foundation/allocator.hpp"
#include "foundation/async.hpp"
#include "foundation/buffer.hpp"
#include "foundation/containers.hpp"
#include "foundation/log.hpp"
#include "geodesy/ellipsoid.hpp"
#include "providers/pack_reader.hpp"
#include "tiling/selection.hpp"
#include "tiling/tile.hpp"

#include <vgl/context.h>

#include <cstdint>

namespace vgl::internal
{

// Key of the base shard in a pack's shard table; level-S shards use morton(x, y).
inline constexpr std::uint64_t base_shard_key = ~0ull;

struct shard_state
{
    enum class state : std::uint8_t
    {
        loading,           // Reading the header (and usually the directory with it).
        loading_directory, // Reading the rest of the directory.
        ready,
        failed,
    };

    state current = state::loading;
    vgl_result failure = VGL_RESULT_SUCCESS; // Why it failed.
    pack::shard_place place;
    pack::shard_header header;
    pack::shard_directory directory;
    buffer<char> path; // Relative to the pack, null-terminated.
};

struct tile_state
{
    enum class state : std::uint8_t
    {
        loading,
        decoding,
        ready,
        failed,
    };

    state current = state::loading;
    vgl_result failure = VGL_RESULT_SUCCESS; // Why it failed.
    pack::tile_header header;
    buffer<std::int16_t> q; // Quantized samples once ready.
    std::uint64_t last_used = 0;
    // Range of the decoded heights, without nodata. min_height > max_height if every sample
    // is nodata.
    double min_height = 0.0;
    double max_height = 0.0;

    [[nodiscard]] double height(std::size_t k) const noexcept
    {
        return header.offset + header.scale * q.data()[k];
    }
};

// Decoding of one elevation or geoid tile, on a task thread or in vgl_context_update.
struct decode_job : async_item
{
    decode_job() noexcept : async_item(kind::task) {}

    async_core* core = nullptr;
    std::uint32_t pack_id = 0;
    std::uint64_t tile_key = 0;
    pack::layer_type type = pack::layer_type::elevation;
    std::uint32_t tile_size = 0;
    double step = 0.0;
    buffer<std::byte> bytes;
    pack::tile_header header;
    buffer<std::int16_t> q;
    buffer<std::byte> scratch;
    vgl_result result = VGL_RESULT_SUCCESS;
    double min_height = 0.0;
    double max_height = 0.0;

    // Decodes bytes into q and finds the height range, then frees the bytes and scratch.
    void run() noexcept;
};

// A tile chosen for drawing (design document, section 7.3).
struct selected_tile
{
    tile_id id;
    tile_id source;            // The tile whose data draws it: itself, or an ancestor.
    std::uint32_t pack_id = 0; // Pack of the source; 0 for the bare ellipsoid.
    double min_height = 0.0;
    double max_height = 0.0;
};

// A tile that selection wants loaded.
struct load_request
{
    std::uint32_t pack_id = 0;
    tile_id id;
    double screen_space_error = 0.0;
};

// Bounding box of a tile for a height range, kept between frames.
struct cached_box
{
    oriented_box box;
    dvec3 horizon_point; // In the horizon occluder's scaled space, if has_horizon_point.
    bool has_horizon_point = false;
    double min_height = 0.0;
    double max_height = 0.0;
    std::uint64_t last_used = 0;
};

enum class availability : std::uint8_t
{
    absent,  // The pack has no such tile.
    wanted,  // It may exist, and no read for it has started; only when not starting reads.
    pending, // It is loading.
    ready,
};

// Finds a tile in a pack. With start, it starts the reads the tile needs; without, it reports
// availability::wanted instead. error receives VGL_RESULT_OUT_OF_MEMORY if a read could not
// start.
availability find_tile(vgl_context& ctx,
                       vgl_pack& p,
                       const tile_id& t,
                       tile_state*& out,
                       vgl_result& error,
                       bool start = true) noexcept;

// Selects tiles for the context's view and starts the reads they need.
void select_tiles(vgl_context& ctx) noexcept;

} // namespace vgl::internal

struct vgl_pack
{
    explicit vgl_pack(const vgl::internal::allocator& a) noexcept : shards(a), tiles(a) {}

    vgl_context* context = nullptr;
    std::uint32_t id = 0;
    std::int32_t priority = 0;
    std::uint64_t order = 0;
    vgl::internal::buffer<char> uri; // Null-terminated.
    vgl_pack_state state = VGL_PACK_STATE_LOADING;
    vgl_result failure = VGL_RESULT_SUCCESS;
    vgl::internal::pack::manifest manifest;
    vgl::internal::u64_map<vgl::internal::shard_state> shards;
    vgl::internal::u64_map<vgl::internal::tile_state> tiles;
};

struct vgl_context
{
    explicit vgl_context(const vgl::internal::allocator& a) noexcept
        : memory(a), decompressor(a), outstanding(a), packs(a), pending(a), selected(a), wanted(a),
          boxes(a)
    {
    }

    vgl::internal::allocator memory;
    vgl::internal::logger log;
    vgl::internal::ellipsoid shape;
    bool has_io = false;
    vgl_io_interface io{};
    bool has_tasks = false;
    vgl_task_interface tasks{};
    std::uint64_t cpu_budget = 0;
    std::uint64_t cpu_used = 0;
    vgl::internal::async_core* core = nullptr;
    vgl::internal::zstd_decompressor decompressor; // For manifests and directories.
    vgl::internal::u64_map<vgl_io_request*> outstanding;
    std::uint64_t next_request_id = 1;
    vgl::internal::vector<vgl_pack*> packs; // Highest priority first.
    vgl_pack* geoid = nullptr;
    std::uint32_t next_pack_id = 1;
    std::uint64_t next_order = 1;
    vgl::internal::vector<vgl::internal::decode_job*> pending; // Waiting for an update to decode.
    std::uint64_t frame = 1;

    bool has_view = false;
    vgl::internal::view_state view;
    vgl::internal::vector<vgl::internal::selected_tile> selected; // For the view, this frame.
    vgl::internal::vector<vgl::internal::load_request> wanted;
    vgl::internal::u64_map<vgl::internal::cached_box> boxes; // By tile key.
};

#endif // VGL_CONTEXT_CONTEXT_IMPL_HPP
