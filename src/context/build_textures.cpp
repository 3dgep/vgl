// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Textures for the selected tiles (docs/render-interface.md, section 7). Imagery tiles stay
// encoded in the cache, a few kilobytes each. When a selected tile wants one as a texture, a
// task decodes it to RGBA8 and builds its mip levels; the frame then offers it to the consumer
// and frees the pixels once created. A texture evicted from the GPU, or lost with the device,
// is decoded again from the cached tile.

#include "context/context_impl.hpp"
#include "tiling/tile.hpp"

#include <algorithm>
#include <cstring>

namespace vgl::internal
{

namespace
{

namespace pk = pack;

bool overlaps(double lo, double hi, double other_lo, double other_hi) noexcept
{
    return lo <= other_hi && other_lo <= hi;
}

// True if a tile may lie in a pack's coverage, which may cross the antimeridian.
bool covers(const pk::layer_description& l, const tile_id& t) noexcept
{
    const degree_region r = tile_bounds(t);
    if (!overlaps(r.south, r.north, l.south, l.north))
    {
        return false;
    }
    return overlaps(r.west, r.east, l.west, l.east) ||
           overlaps(r.west + 360.0, r.east + 360.0, l.west, l.east) ||
           overlaps(r.west - 360.0, r.east - 360.0, l.west, l.east);
}

// A ready imagery pack whose layer is shown: a hidden one is neither loaded nor drawn.
bool is_imagery(const vgl_pack& p) noexcept
{
    return p.state == VGL_PACK_STATE_READY && p.manifest.layer().type == pk::layer_type::imagery &&
           p.opacity > 0.0f;
}

vgl_pack* pack_by_id(vgl_context& ctx, std::uint32_t id) noexcept
{
    for (vgl_pack* p : ctx.packs)
    {
        if (p->id == id)
        {
            return p;
        }
    }
    return nullptr;
}

void texture_task(void* data) noexcept
{
    auto* job = static_cast<texture_job*>(data);
    async_core* core = job->core;
    if (!core->begin_task())
    {
        free_texture_job(core->memory(), job);
        return;
    }
    job->run();
    if (!core->end_task(job))
    {
        free_texture_job(core->memory(), job);
    }
}

// The deepest imagery tile of a pack, at t's level or above, whose encoded tile is loaded.
tile_state* deepest_ready(vgl_pack& p, const tile_id& t, tile_id& out) noexcept
{
    const pk::layer_description& l = p.manifest.layer();
    if (t.level < l.min_level)
    {
        return nullptr;
    }
    for (std::uint32_t level = std::min(t.level, static_cast<std::uint32_t>(l.max_level)) + 1;
         level-- > l.min_level;)
    {
        const tile_id a = tile_ancestor(t, level);
        tile_state* s = p.tiles.find(tile_key(a));
        if (s != nullptr && s->current == tile_state::state::ready)
        {
            out = a;
            return s;
        }
    }
    return nullptr;
}

// Makes the job that decodes a tile's texture. Returns null if out of memory.
texture_job* prepare(vgl_context& ctx, const vgl_pack& p, const tile_id& t, const tile_state& s)
{
    auto* job = ctx.memory.create<texture_job>();
    const std::span<const std::byte> file = s.encoded.span().subspan(s.header.header_size);
    if (job == nullptr || !job->file.allocate(ctx.memory, file.size()))
    {
        if (job != nullptr)
        {
            ctx.memory.destroy(job);
        }
        return nullptr;
    }
    std::memcpy(job->file.data(), file.data(), file.size());
    const pk::layer_description& l = p.manifest.layer();
    job->core = ctx.core;
    ctx.core->retain();
    job->pack_id = p.id;
    job->tile_key = tile_key(t);
    job->side = l.tile_size + 2u * l.border;
    return job;
}

} // namespace

void texture_job::run() noexcept
{
    result = decode_imagery(core->memory(), file.span(), side, image);
    file = buffer<std::byte>();
}

void free_texture_job(const allocator& a, texture_job* job) noexcept
{
    async_core* core = job->core;
    a.destroy(job);
    if (core != nullptr)
    {
        core->release();
    }
}

void handle_texture_built(vgl_context& ctx, texture_job* job) noexcept
{
    vgl_pack* p = pack_by_id(ctx, job->pack_id);
    texture_state* s = p != nullptr ? p->textures.find(job->tile_key) : nullptr;
    if (s != nullptr && s->decoding)
    {
        s->decoding = false;
        const tile_id t = tile_from_key(job->tile_key);
        if (job->result != VGL_RESULT_SUCCESS)
        {
            // The tile is unusable: mark it failed, so that it is neither drawn nor read again.
            const char* name = nullptr;
            static_cast<void>(vgl_result_name_get(job->result, &name));
            ctx.log.log(VGL_LOG_LEVEL_ERROR,
                        "vgl_context_update: pack %s: imagery tile %u/%u/%u does not decode (%s)",
                        p->uri.data(),
                        t.level,
                        t.x,
                        t.y,
                        name);
            if (tile_state* tile = p->tiles.find(job->tile_key); tile != nullptr)
            {
                ctx.cpu_used -= tile->cpu_bytes();
                tile->encoded = buffer<std::byte>();
                tile->current = tile_state::state::failed;
                tile->failure = job->result;
            }
            static_cast<void>(p->textures.erase(job->tile_key));
        }
        else
        {
            gpu_resource r;
            r.resource_kind = gpu_resource::kind::texture;
            r.tile = t;
            r.pack_id = p->id;
            r.image = std::move(job->image);
            r.bytes = r.image.pixels.size();
            for (std::uint32_t level = 0; level < r.image.levels; ++level)
            {
                r.mip_data[level] = r.image.pixels.data() + r.image.offsets[level];
                r.mip_sizes[level] = r.image.sizes[level];
            }
            const std::uint64_t id = ctx.next_resource_id;
            if (ctx.resources.insert(id, std::move(r)) != nullptr)
            {
                ++ctx.next_resource_id;
                ctx.cpu_used += ctx.resources.find(id)->cpu_bytes();
                p->textures.find(job->tile_key)->current = id;
            }
        }
    }
    free_texture_job(ctx.memory, job);
}

void build_textures(vgl_context& ctx, std::chrono::steady_clock::time_point deadline) noexcept
{
    bool started = false;
    for (vgl_pack* p : ctx.packs)
    {
        if (!is_imagery(*p))
        {
            continue;
        }
        const pk::layer_description& l = p->manifest.layer();
        for (const selected_tile& sel : ctx.selected)
        {
            if (!covers(l, sel.id))
            {
                continue;
            }
            tile_id t;
            tile_state* tile = deepest_ready(*p, sel.id, t);
            if (tile == nullptr)
            {
                continue;
            }
            tile->last_used = ctx.frame;
            p->imagery_wanted = ctx.frame;
            const std::uint64_t key = tile_key(t);
            texture_state* s = p->textures.find(key);
            if (s == nullptr)
            {
                s = p->textures.insert(key, texture_state{});
                if (s == nullptr)
                {
                    continue;
                }
            }
            s->last_wanted = ctx.frame;
            if (s->current != 0 || s->decoding ||
                (started && std::chrono::steady_clock::now() >= deadline))
            {
                continue;
            }
            texture_job* job = prepare(ctx, *p, t, *tile);
            if (job == nullptr)
            {
                continue;
            }
            started = true;
            s->decoding = true;
            if (ctx.has_tasks &&
                ctx.tasks.submit(ctx.tasks.user_data, &texture_task, job) == VGL_RESULT_SUCCESS)
            {
                continue;
            }
            job->run();
            handle_texture_built(ctx, job); // May move s; it is not used again.
        }
    }
}

void drop_pack_textures(vgl_context& ctx, vgl_pack& p) noexcept
{
    p.textures.for_each([&](std::uint64_t, texture_state& s) {
        if (s.current != 0)
        {
            retire_resource(ctx, s.current);
            s.current = 0;
        }
    });
    p.textures.clear();
}

void evict_pending_textures(vgl_context& ctx) noexcept
{
    if (ctx.cpu_used <= ctx.cpu_budget)
    {
        return;
    }
    struct candidate
    {
        std::uint64_t last_wanted;
        vgl_pack* pack;
        std::uint64_t key;
    };
    vector<candidate> candidates(ctx.memory);
    for (vgl_pack* p : ctx.packs)
    {
        p->textures.for_each([&](std::uint64_t key, const texture_state& s) {
            const gpu_resource* r = s.current != 0 ? ctx.resources.find(s.current) : nullptr;
            if (r != nullptr && r->current == gpu_resource::state::pending &&
                s.last_wanted < ctx.frame)
            {
                static_cast<void>(candidates.push_back({s.last_wanted, p, key}));
            }
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b) {
        return a.last_wanted < b.last_wanted;
    });
    for (const candidate& c : candidates)
    {
        if (ctx.cpu_used <= ctx.cpu_budget)
        {
            break;
        }
        texture_state* s = c.pack->textures.find(c.key);
        retire_resource(ctx, s->current);
        static_cast<void>(c.pack->textures.erase(c.key));
    }
}

std::uint64_t texture_for(const vgl_context& ctx,
                          const vgl_pack& p,
                          const tile_id& t,
                          std::uint32_t& level) noexcept
{
    const pk::layer_description& l = p.manifest.layer();
    if (t.level < l.min_level)
    {
        return 0;
    }
    for (std::uint32_t lv = std::min(t.level, static_cast<std::uint32_t>(l.max_level)) + 1;
         lv-- > l.min_level;)
    {
        const texture_state* s = p.textures.find(tile_key(tile_ancestor(t, lv)));
        const gpu_resource* r =
            s != nullptr && s->current != 0 ? ctx.resources.find(s->current) : nullptr;
        if (r != nullptr && r->current == gpu_resource::state::resident && !r->garbage)
        {
            level = lv;
            return s->current;
        }
    }
    return 0;
}

} // namespace vgl::internal
