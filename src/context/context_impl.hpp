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

    // Decodes bytes into q, then frees the bytes and scratch.
    void run() noexcept;
};

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
        : memory(a), decompressor(a), outstanding(a), packs(a), pending(a)
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
};

#endif // VGL_CONTEXT_CONTEXT_IMPL_HPP
