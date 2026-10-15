// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Meshes for the selected tiles (design document, section 7.3, "Tile meshes"). The update
// gathers each mesh's heights from the loaded tiles, filling nodata from other tiles; a task
// then builds the vertices, which is where the time goes.

#include "context/context_impl.hpp"
#include "decoders/elevation_codec.hpp"
#include "mesh/tile_mesh.hpp"
#include "tiling/selection.hpp"
#include "tiling/tile.hpp"

#include <algorithm>
#include <cmath>

namespace vgl::internal
{

namespace
{

namespace pk = pack;

// Largest spacing of the bare ellipsoid's vertices, in degrees, and its largest grid.
constexpr double ellipsoid_spacing = 1.5;
constexpr std::uint32_t max_ellipsoid_grid = 129;
// Samples per side assumed for the geometric error of a tile without data.
constexpr std::uint32_t default_tile_size = 129;

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

tile_state* ready_tile(vgl_pack& p, const tile_id& t) noexcept
{
    tile_state* s = p.tiles.find(tile_key(t));
    return s != nullptr && s->current == tile_state::state::ready ? s : nullptr;
}

// A loaded tile to take heights from.
struct height_source
{
    tile_id id;
    tile_state* tile = nullptr;
    std::uint32_t size = 0;
};

// Height of a tile at a position in degrees, or false at nodata.
bool sample(const height_source& s, double lat, double lon, float& out) noexcept
{
    bool nodata = false;
    const std::uint32_t n = s.size;
    const double h = tile_interpolate(s.id, n, lat, lon, [&](std::uint32_t i, std::uint32_t j) {
        const std::size_t k = static_cast<std::size_t>(j) * n + i;
        nodata = nodata || s.tile->q.data()[k] == elevation_nodata;
        return s.tile->height(k);
    });
    out = static_cast<float>(h);
    return !nodata;
}

// Loaded tiles that contain t, to fill nodata from: the source's own pack first, deepest
// first, then the other packs in priority order.
void fallbacks(vgl_context& ctx,
               const tile_id& t,
               vgl_pack* first,
               const tile_id& skip,
               vector<height_source>& out) noexcept
{
    const auto add_pack = [&](vgl_pack& p) {
        const pk::layer_description& l = p.manifest.layer();
        if (t.level < l.min_level)
        {
            return;
        }
        for (std::uint32_t level = std::min(t.level, static_cast<std::uint32_t>(l.max_level)) + 1;
             level-- > l.min_level;)
        {
            const tile_id a = tile_ancestor(t, level);
            if (&p == first && a == skip)
            {
                continue; // The source itself.
            }
            if (tile_state* s = ready_tile(p, a); s != nullptr)
            {
                static_cast<void>(out.push_back({a, s, l.tile_size}));
            }
        }
    };
    if (first != nullptr)
    {
        add_pack(*first);
    }
    for (vgl_pack* p : ctx.packs)
    {
        if (p != first && p->state == VGL_PACK_STATE_READY &&
            p->manifest.layer().type == pk::layer_type::elevation)
        {
            add_pack(*p);
        }
    }
}

// Makes the job for a selected tile, with its heights. Returns null if out of memory.
mesh_job* prepare(vgl_context& ctx, const selected_tile& s) noexcept
{
    vgl_pack* p = s.pack_id != 0 ? pack_by_id(ctx, s.pack_id) : nullptr;
    tile_state* data = p != nullptr ? ready_tile(*p, s.source) : nullptr;
    if (data == nullptr)
    {
        p = nullptr; // Evicted or gone since selection: draw the ellipsoid for now.
    }
    const degree_region b = tile_bounds(s.id);

    // The grid: the source's samples over the tile where there are enough, else interpolated.
    std::uint32_t grid = 0;
    std::uint32_t cells = 0;
    std::uint32_t n = default_tile_size;
    if (p != nullptr)
    {
        n = p->manifest.layer().tile_size;
        const std::uint32_t depth = s.id.level - s.source.level;
        cells = depth < 32 ? (n - 1) >> depth : 0;
        grid = cells >= min_mesh_grid - 1 ? cells + 1 : min_mesh_grid;
    }
    else
    {
        const auto wanted =
            static_cast<std::uint32_t>(std::ceil(tile_span(s.id.level) / ellipsoid_spacing)) + 1;
        grid = std::clamp(wanted, min_mesh_grid, max_ellipsoid_grid);
    }

    auto* job = ctx.memory.create<mesh_job>();
    const std::size_t count = static_cast<std::size_t>(grid) * grid;
    vector<height_source> fill(ctx.memory);
    if (job == nullptr || !job->heights.allocate(ctx.memory, count) ||
        !job->scratch.allocate(ctx.memory, count) ||
        !job->vertices.allocate(ctx.memory, tile_mesh_vertex_count(grid)))
    {
        ctx.memory.destroy(job);
        return nullptr;
    }
    fallbacks(ctx, s.id, p, s.source, fill);

    const double step = tile_span(s.id.level) / (grid - 1);
    float* heights = job->heights.data();
    const bool exact = p != nullptr && cells >= min_mesh_grid - 1;
    const std::uint32_t depth = s.id.level - s.source.level;
    const std::uint32_t ox = exact ? (s.id.x - (s.source.x << depth)) * cells : 0;
    const std::uint32_t oy = exact ? (s.id.y - (s.source.y << depth)) * cells : 0;
    const height_source own{s.source, data, n};
    for (std::uint32_t j = 0; j < grid; ++j)
    {
        const double lat = j == grid - 1 ? b.south : b.north - j * step;
        for (std::uint32_t i = 0; i < grid; ++i)
        {
            const double lon = i == grid - 1 ? b.east : b.west + i * step;
            float h = 0.0f;
            bool found = false;
            if (exact)
            {
                const std::size_t k = static_cast<std::size_t>(oy + j) * n + (ox + i);
                found = data->q.data()[k] != elevation_nodata;
                h = static_cast<float>(data->height(k));
            }
            else if (p != nullptr)
            {
                found = sample(own, lat, lon, h);
            }
            for (std::size_t c = 0; !found && c < fill.size(); ++c)
            {
                found = sample(fill[c], lat, lon, h);
            }
            heights[static_cast<std::size_t>(j) * grid + i] = found ? h : 0.0f;
        }
    }

    float lowest = heights[0];
    for (std::size_t k = 0; k < count; ++k)
    {
        lowest = std::min(lowest, heights[k]);
    }
    job->core = ctx.core;
    ctx.core->retain();
    job->id = s.id;
    job->source = p != nullptr ? s.source : s.id;
    job->pack_id = p != nullptr ? p->id : 0;
    job->grid = grid;
    job->shape = ctx.shape;
    job->skirt_height = lowest - tile_geometric_error(ctx.shape, s.id.level, n);
    return job;
}

void mesh_task(void* data) noexcept
{
    auto* job = static_cast<mesh_job*>(data);
    async_core* core = job->core;
    if (!core->begin_task())
    {
        free_mesh_job(core->memory(), job);
        return;
    }
    job->run();
    if (!core->end_task(job))
    {
        free_mesh_job(core->memory(), job);
    }
}

} // namespace

void mesh_job::run() noexcept
{
    centre = build_tile_vertices(
        shape, id, grid, heights.span(), skirt_height, scratch.span(), vertices.span());
    min_height = skirt_height;
    max_height = skirt_height;
    for (const float h : heights.span())
    {
        max_height = std::max(max_height, static_cast<double>(h));
    }
    heights = buffer<float>();
    scratch = buffer<dvec3>();
}

void free_mesh_job(const allocator& a, mesh_job* job) noexcept
{
    async_core* core = job->core;
    a.destroy(job);
    if (core != nullptr)
    {
        core->release();
    }
}

void handle_mesh_built(vgl_context& ctx, mesh_job* job) noexcept
{
    mesh_state* m = ctx.meshes.find(tile_key(job->id));
    if (m != nullptr && m->building)
    {
        m->building = false;
        if (m->has_mesh)
        {
            ctx.cpu_used -= m->mesh.vertices.size() * sizeof(tile_vertex);
        }
        m->mesh.source = job->source;
        m->mesh.pack_id = job->pack_id;
        m->mesh.grid = job->grid;
        m->mesh.centre = job->centre;
        m->mesh.min_height = job->min_height;
        m->mesh.max_height = job->max_height;
        m->mesh.vertices = std::move(job->vertices);
        m->has_mesh = true;
        ctx.cpu_used += m->mesh.vertices.size() * sizeof(tile_vertex);
    }
    free_mesh_job(ctx.memory, job);
}

void build_meshes(vgl_context& ctx, std::chrono::steady_clock::time_point deadline) noexcept
{
    bool started = false;
    for (const selected_tile& s : ctx.selected)
    {
        const std::uint64_t key = tile_key(s.id);
        mesh_state* m = ctx.meshes.find(key);
        if (m == nullptr)
        {
            m = ctx.meshes.insert(key, mesh_state{});
            if (m == nullptr)
            {
                continue;
            }
        }
        m->last_used = ctx.frame;
        const bool current =
            m->has_mesh && m->mesh.source == s.source && m->mesh.pack_id == s.pack_id;
        if (current || m->building || (started && std::chrono::steady_clock::now() >= deadline))
        {
            continue;
        }
        mesh_job* job = prepare(ctx, s);
        if (job == nullptr)
        {
            continue;
        }
        started = true;
        m->building = true;
        if (ctx.has_tasks &&
            ctx.tasks.submit(ctx.tasks.user_data, &mesh_task, job) == VGL_RESULT_SUCCESS)
        {
            continue;
        }
        job->run();
        handle_mesh_built(ctx, job); // May move m; it is not used again.
    }
}

void drop_pack_meshes(vgl_context& ctx, std::uint32_t pack_id) noexcept
{
    vector<std::uint64_t> keys(ctx.memory);
    ctx.meshes.for_each([&](std::uint64_t key, const mesh_state& m) {
        if (m.has_mesh && m.mesh.pack_id == pack_id)
        {
            static_cast<void>(keys.push_back(key));
        }
    });
    for (const std::uint64_t key : keys)
    {
        mesh_state* m = ctx.meshes.find(key);
        ctx.cpu_used -= m->mesh.vertices.size() * sizeof(tile_vertex);
        if (m->building)
        {
            // A build is under way; keep the entry for it, without the stale mesh.
            m->has_mesh = false;
            m->mesh = tile_mesh_data{};
        }
        else
        {
            static_cast<void>(ctx.meshes.erase(key));
        }
    }
}

} // namespace vgl::internal
