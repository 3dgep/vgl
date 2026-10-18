// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// vgl_context_statistics_get. Counts that the context does not keep are gathered on demand, by
// walking its tables: statistics are read once a frame at most, the tables are walked often.

#include "context/context_impl.hpp"
#include "mesh/tile_mesh.hpp"

#include <algorithm>
#include <cstring>

using namespace vgl::internal;

extern "C" VGL_API vgl_result vgl_context_statistics_get(const vgl_context* context,
                                                         vgl_context_statistics* out_statistics)
{
    if (context == nullptr || out_statistics == nullptr || out_statistics->struct_size < 8)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *context;
    vgl_context_statistics s{};

    s.cpu_used = ctx.cpu_used;
    s.cpu_budget = ctx.cpu_budget;
    s.gpu_used = ctx.gpu_used;
    s.gpu_budget = ctx.gpu_budget;

    for (const vgl_pack* p : ctx.packs)
    {
        s.packs_loading += p->state == VGL_PACK_STATE_LOADING ? 1 : 0;
        s.packs_failed += p->state == VGL_PACK_STATE_FAILED ? 1 : 0;
        p->tiles.for_each([&](std::uint64_t, const tile_state& t) {
            s.tiles_decoding += t.current == tile_state::state::decoding ? 1 : 0;
            s.tiles_decoded += t.current == tile_state::state::ready ? 1 : 0;
        });
    }
    s.reads_outstanding = static_cast<std::uint32_t>(ctx.outstanding.size());

    ctx.meshes.for_each(
        [&](std::uint64_t, const mesh_state& m) { s.meshes_building += m.building ? 1 : 0; });
    ctx.resources.for_each([&](std::uint64_t, const gpu_resource& r) {
        if (r.garbage)
        {
            return;
        }
        if (r.current == gpu_resource::state::resident)
        {
            ++s.resources_resident;
        }
    });

    s.tiles_selected = ctx.has_view ? static_cast<std::uint32_t>(ctx.selected.size()) : 0;
    // Built meshes of selected tiles that the consumer has yet to create. Meshes built for tiles
    // that are no longer selected stay as a cache, and do not count.
    for (std::uint32_t i = 0; i < s.tiles_selected; ++i)
    {
        const mesh_state* m = ctx.meshes.find(tile_key(ctx.selected[i].id));
        const gpu_resource* r =
            m != nullptr && m->next != 0 ? ctx.resources.find(m->next) : nullptr;
        if (r != nullptr && !r->garbage && r->current == gpu_resource::state::pending)
        {
            ++s.meshes_waiting;
        }
    }
    // Textures that the last update wanted, decoding or decoded, until the consumer has them.
    for (const vgl_pack* p : ctx.packs)
    {
        p->textures.for_each([&](std::uint64_t, const texture_state& t) {
            s.textures_decoding += t.decoding ? 1 : 0;
            const gpu_resource* r = t.current != 0 ? ctx.resources.find(t.current) : nullptr;
            const bool resident = r != nullptr && r->current == gpu_resource::state::resident;
            if (ctx.has_view && t.last_wanted + 1 == ctx.frame && !resident)
            {
                ++s.textures_waiting;
            }
        });
    }
    s.tiles_drawn = static_cast<std::uint32_t>(ctx.drawn.size());
    for (const drawn_tile& d : ctx.drawn)
    {
        const gpu_resource* r = ctx.resources.find(d.mesh);
        if (r != nullptr)
        {
            s.vertices_drawn += tile_mesh_vertex_count(r->grid);
            s.triangles_drawn += tile_mesh_index_count(r->grid) / 3;
        }
    }
    s.resources_created = ctx.last_created;
    s.resources_destroyed = ctx.last_destroyed;
    s.bytes_created = ctx.last_bytes_created;

    s.reads_total = ctx.reads_total;
    s.bytes_read_total = ctx.bytes_read_total;
    s.tiles_loaded_total = ctx.tiles_loaded_total;
    s.tile_load_time_mean =
        ctx.tiles_loaded_total != 0
            ? ctx.load_seconds_total / static_cast<double>(ctx.tiles_loaded_total)
            : 0.0;
    s.update_time = ctx.update_seconds;
    s.frame_time = ctx.frame_seconds;

    // The fields that fit in the caller's structure, and the size written (docs/c-api.md,
    // section 8.4).
    const auto size =
        static_cast<std::uint32_t>(std::min<std::size_t>(out_statistics->struct_size, sizeof(s)));
    s.struct_size = size;
    std::memcpy(out_statistics, &s, size);
    return VGL_RESULT_SUCCESS;
}
