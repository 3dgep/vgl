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
#include <cstring>

namespace vgl::internal
{

namespace
{

namespace pk = pack;

// Largest spacing of the bare ellipsoid's vertices, in degrees, and its largest grid.
constexpr double ellipsoid_spacing = 1.5;
constexpr std::uint32_t max_ellipsoid_grid = 129;
// How far a tile's mesh may lie from its samples, as a share of its geometric error.
constexpr double mesh_tolerance = 0.5;

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
        !job->vertices.allocate(ctx.memory, tile_mesh_vertex_count(grid)) ||
        !job->samples.allocate(ctx.memory, count))
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
    const double error = tile_geometric_error(ctx.shape, s.id.level, n);
    job->skirt_height = lowest - error;
    // A tile that can refine is drawn only while its geometric error is small on the screen,
    // so a coarser grid within a part of that error looks the same. Tiles that cannot refine
    // may be seen from any distance, and keep every sample.
    if (exact && depth == 0 && data->header.child_mask != 0 && s.id.level < max_tile_level)
    {
        job->tolerance = mesh_tolerance * error;
    }
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
    // The samples keep the full grid for shading, however coarse the mesh.
    build_tile_positions(shape, id, grid, heights.span(), scratch.span());
    build_tile_samples(id, grid, heights.span(), scratch.span(), samples.span());
    sample_grid = grid;
    if (tolerance > 0.0)
    {
        const std::uint32_t stride = tile_mesh_stride(grid, scratch.span(), tolerance);
        buffer<tile_vertex> fewer;
        // Out of memory, the mesh keeps every sample.
        if (stride > 1 &&
            fewer.allocate(core->memory(), tile_mesh_vertex_count((grid - 1) / stride + 1)))
        {
            grid = tile_heights_decimate(grid, stride, heights.span());
            vertices = std::move(fewer);
        }
    }
    const std::size_t count = static_cast<std::size_t>(grid) * grid;
    const std::span<const float> kept = heights.span().first(count);
    centre = build_tile_vertices(
        shape, id, grid, kept, skirt_height, scratch.span().first(count), vertices.span());
    // The terrain's range; the skirts hang below it.
    min_height = kept[0];
    max_height = kept[0];
    for (const float h : kept)
    {
        min_height = std::min(min_height, static_cast<double>(h));
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

namespace
{

// Makes sure the index list of a grid size exists. Returns false if out of memory.
bool ensure_index_list(vgl_context& ctx, std::uint32_t grid) noexcept
{
    if (ctx.index_lists.find(grid) != nullptr)
    {
        return true;
    }
    const std::size_t count = tile_mesh_index_count(grid);
    const bool wide = tile_mesh_vertex_count(grid) > 65536;
    gpu_resource r;
    r.resource_kind = gpu_resource::kind::index_list;
    r.grid = grid;
    r.index_count = static_cast<std::uint32_t>(count);
    r.index_size = wide ? 4 : 2;
    buffer<std::uint32_t> indices;
    if (!indices.allocate(ctx.memory, count) ||
        !r.indices.allocate(ctx.memory, count * r.index_size))
    {
        return false;
    }
    build_tile_indices(grid, indices.span());
    if (wide)
    {
        std::memcpy(r.indices.data(), indices.data(), count * 4);
    }
    else
    {
        for (std::size_t k = 0; k < count; ++k)
        {
            const auto v = static_cast<std::uint16_t>(indices.data()[k]);
            std::memcpy(r.indices.data() + 2 * k, &v, 2);
        }
    }
    r.bytes = r.indices.size();
    const std::uint64_t id = ctx.next_resource_id;
    if (ctx.resources.insert(id, std::move(r)) == nullptr)
    {
        return false;
    }
    if (ctx.index_lists.insert(grid, id) == nullptr)
    {
        static_cast<void>(ctx.resources.erase(id));
        return false;
    }
    ++ctx.next_resource_id;
    ctx.cpu_used += ctx.resources.find(id)->cpu_bytes();
    return true;
}

// Fills a mesh's geoid heights at its corners, once the geoid pack has them there. Corners are
// sampled at their own positions, so neighbouring tiles share them.
void fill_geoid(vgl_context& ctx, gpu_resource& r) noexcept
{
    if (r.has_geoid || r.resource_kind != gpu_resource::kind::mesh)
    {
        return;
    }
    const degree_region b = tile_bounds(r.tile);
    const std::array<double, 4> lat = {b.north, b.north, b.south, b.south};
    const std::array<double, 4> lon = {b.west, b.east, b.west, b.east};
    std::array<float, 4> heights = {};
    for (std::size_t k = 0; k < 4; ++k)
    {
        double h = 0.0;
        if (!sample_geoid(ctx, lat[k], lon[k], h))
        {
            return;
        }
        heights[k] = static_cast<float>(h);
    }
    r.geoid = heights;
    r.has_geoid = true;
}

// Erases a mesh state that holds nothing.
void forget_if_empty(vgl_context& ctx, std::uint64_t key) noexcept
{
    const mesh_state* m = ctx.meshes.find(key);
    if (m != nullptr && m->current == 0 && m->next == 0 && !m->building)
    {
        static_cast<void>(ctx.meshes.erase(key));
    }
}

} // namespace

const gpu_resource* latest_mesh(const vgl_context& ctx, const tile_id& t) noexcept
{
    const mesh_state* m = ctx.meshes.find(tile_key(t));
    if (m == nullptr)
    {
        return nullptr;
    }
    const std::uint64_t id = m->next != 0 ? m->next : m->current;
    return id != 0 ? ctx.resources.find(id) : nullptr;
}

void retire_resource(vgl_context& ctx, std::uint64_t id) noexcept
{
    gpu_resource* r = ctx.resources.find(id);
    if (r == nullptr)
    {
        return;
    }
    r->garbage = true;
    if (r->current == gpu_resource::state::offered)
    {
        return; // The open frame's lists point at its data; vgl_frame_end frees it.
    }
    ctx.cpu_used -= r->cpu_bytes();
    r->free_cpu_copy();
    if (r->current == gpu_resource::state::pending)
    {
        static_cast<void>(ctx.resources.erase(id)); // The consumer never received it.
    }
}

void handle_mesh_built(vgl_context& ctx, mesh_job* job) noexcept
{
    mesh_state* m = ctx.meshes.find(tile_key(job->id));
    if (m != nullptr && m->building)
    {
        m->building = false;
        gpu_resource r;
        r.resource_kind = gpu_resource::kind::mesh;
        r.tile = job->id;
        r.source = job->source;
        r.pack_id = job->pack_id;
        r.grid = job->grid;
        r.centre = job->centre;
        r.min_height = job->min_height;
        r.max_height = job->max_height;
        r.bytes =
            job->vertices.size() * sizeof(tile_vertex) + job->samples.size() * sizeof(tile_sample);
        r.vertices = std::move(job->vertices);
        r.samples = std::move(job->samples);
        r.sample_grid = job->sample_grid;
        // The index list first: it may take the next resource id.
        const bool has_list = ensure_index_list(ctx, job->grid);
        const std::uint64_t id = ctx.next_resource_id;
        if (has_list && ctx.resources.insert(id, std::move(r)) != nullptr)
        {
            ++ctx.next_resource_id;
            gpu_resource* installed = ctx.resources.find(id);
            ctx.cpu_used += installed->cpu_bytes();
            fill_geoid(ctx, *installed);
            // The map may have moved m; find it again.
            m = ctx.meshes.find(tile_key(job->id));
            if (m->next != 0)
            {
                retire_resource(ctx, m->next); // Superseded before it was created.
            }
            m->next = id;
        }
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
        // Meshes built before the geoid pack had loaded there get their geoid heights now.
        for (const std::uint64_t id : {m->current, m->next})
        {
            if (gpu_resource* r = id != 0 ? ctx.resources.find(id) : nullptr; r != nullptr)
            {
                fill_geoid(ctx, *r);
            }
        }
        const gpu_resource* latest = latest_mesh(ctx, s.id);
        const bool current =
            latest != nullptr && latest->source == s.source && latest->pack_id == s.pack_id;
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
    ctx.meshes.for_each(
        [&](std::uint64_t key, const mesh_state&) { static_cast<void>(keys.push_back(key)); });
    for (const std::uint64_t key : keys)
    {
        mesh_state* m = ctx.meshes.find(key);
        for (std::uint64_t* id : {&m->current, &m->next})
        {
            const gpu_resource* r = *id != 0 ? ctx.resources.find(*id) : nullptr;
            if (r != nullptr && r->pack_id == pack_id)
            {
                retire_resource(ctx, *id);
                *id = 0;
            }
        }
        forget_if_empty(ctx, key);
    }
}

void evict_pending_meshes(vgl_context& ctx) noexcept
{
    if (ctx.cpu_used <= ctx.cpu_budget)
    {
        return;
    }
    struct candidate
    {
        std::uint64_t last_used;
        std::uint64_t key;
    };
    vector<candidate> candidates(ctx.memory);
    ctx.meshes.for_each([&](std::uint64_t key, const mesh_state& m) {
        const gpu_resource* r = m.next != 0 ? ctx.resources.find(m.next) : nullptr;
        if (r != nullptr && r->current == gpu_resource::state::pending && !m.building &&
            m.last_used < ctx.frame)
        {
            static_cast<void>(candidates.push_back({m.last_used, key}));
        }
    });
    std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b) {
        return a.last_used < b.last_used;
    });
    for (const candidate& c : candidates)
    {
        if (ctx.cpu_used <= ctx.cpu_budget)
        {
            break;
        }
        mesh_state* m = ctx.meshes.find(c.key);
        retire_resource(ctx, m->next);
        m->next = 0;
        forget_if_empty(ctx, c.key);
    }
    if (ctx.cpu_used <= ctx.cpu_budget)
    {
        return;
    }
    // Index lists that were never uploaded and that no mesh uses any more.
    vector<std::uint32_t> unused(ctx.memory);
    ctx.index_lists.for_each([&](std::uint64_t grid, std::uint64_t id) {
        const gpu_resource* list = ctx.resources.find(id);
        if (list == nullptr || list->current != gpu_resource::state::pending)
        {
            return;
        }
        bool used = false;
        ctx.resources.for_each([&](std::uint64_t, const gpu_resource& r) {
            used = used ||
                   (r.resource_kind == gpu_resource::kind::mesh && !r.garbage && r.grid == grid);
        });
        if (!used)
        {
            static_cast<void>(unused.push_back(static_cast<std::uint32_t>(grid)));
        }
    });
    for (const std::uint32_t grid : unused)
    {
        retire_resource(ctx, *ctx.index_lists.find(grid));
        static_cast<void>(ctx.index_lists.erase(grid));
    }
}

} // namespace vgl::internal
