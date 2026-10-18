// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tile selection for the context's view (design document, section 7.3). Each update walks the
// quadtree from the two root tiles, culls against the frustum and the horizon, refines by
// screen-space error where data has loaded, and requests the tiles that are missing.

#include "context/context_impl.hpp"
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

// Reads selection may have outstanding at once. Height queries are not limited.
constexpr std::size_t max_outstanding_reads = 64;
// Samples per side assumed for the error of a tile that no pack has.
constexpr std::uint32_t default_tile_size = 129;
// Boxes of large tiles reach far below the surface, where they may cross the view even though
// the surface does not. A leaf tile coarser than this level is drawn only if the box of one of
// its descendants at this level is in view.
constexpr std::uint32_t probe_level = 6;
// Probe boxes are cached apart from tile boxes, under keys with this bit set; tile keys never
// use it.
constexpr std::uint64_t probe_key_bit = 1ull << 63;

bool overlaps(double lo, double hi, double other_lo, double other_hi) noexcept
{
    return lo <= other_hi && other_lo <= hi;
}

// True if a tile may lie in a pack's coverage, which is informative and may cross the
// antimeridian.
bool covers(const pk::layer_description& l, const degree_region& r) noexcept
{
    if (!overlaps(r.south, r.north, l.south, l.north))
    {
        return false;
    }
    return overlaps(r.west, r.east, l.west, l.east) ||
           overlaps(r.west + 360.0, r.east + 360.0, l.west, l.east) ||
           overlaps(r.west - 360.0, r.east - 360.0, l.west, l.east);
}

// What the packs hold for one tile.
struct tile_data
{
    vgl_pack* pack = nullptr; // Highest-priority elevation pack whose tile is ready.
    tile_state* tile = nullptr;
    bool loading = false;      // A pack has the tile, but it is not ready yet.
    std::uint8_t children = 0; // Children that some elevation or imagery pack may have.
};

// What a tile is drawn from: its own data, or an ancestor's.
struct source
{
    tile_id id;
    vgl_pack* pack = nullptr; // Null for the bare ellipsoid.
    tile_state* tile = nullptr;
    double min_height = lowest_surface_height;
    double max_height = highest_surface_height;
};

class traversal
{
public:
    explicit traversal(vgl_context& ctx) noexcept
        : ctx_(ctx),
          occluder_(make_horizon_occluder(ctx.shape, lowest_surface_height, ctx.view.eye))
    {
    }

    void run() noexcept
    {
        ctx_.selected.clear();
        ctx_.prefetched.clear();
        ctx_.wanted.clear();
        // The tiles that the last frame drew finer terrain inside.
        drawn_below_.clear();
        for (const drawn_tile& d : ctx_.drawn)
        {
            for (std::uint32_t level = d.id.level; level-- > 0;)
            {
                const std::uint64_t key = tile_key(tile_ancestor(d.id, level));
                if (drawn_below_.find(key) != nullptr || drawn_below_.insert(key, 1) == nullptr)
                {
                    break; // Its ancestors are in already, or out of memory.
                }
            }
        }
        for (std::uint32_t x = 0; x < tile_columns(0); ++x)
        {
            visit({0, x, 0}, source{});
        }
    }

private:
    tile_data find(const tile_id& t) noexcept
    {
        tile_data d;
        const degree_region region = tile_bounds(t);
        // Imagery takes part as well: where it is finer than the terrain, tiles refine to its
        // level, drawn with the terrain of an ancestor, and its tiles load with the terrain's.
        // Its own tile only matters while no terrain is ready: a tile with terrain refines
        // without waiting for its imagery, which an ancestor's texture stands in for.
        for (vgl_pack* p : ctx_.packs)
        {
            const pk::layer_type type = p->manifest.layer().type;
            const bool imagery = type == pk::layer_type::imagery;
            // A hidden imagery layer takes no part, as if it were not there.
            if (p->state != VGL_PACK_STATE_READY || type == pk::layer_type::geoid ||
                (imagery && p->opacity <= 0.0f))
            {
                continue;
            }
            const pk::layer_description& l = p->manifest.layer();
            if (t.level > l.max_level || !covers(l, region))
            {
                continue;
            }
            if (t.level < l.min_level)
            {
                // The pack's tiles lie deeper; any child may lead to them.
                d.children = 0xF;
                continue;
            }
            tile_state* tile = nullptr;
            vgl_result error = VGL_RESULT_SUCCESS;
            switch (find_tile(ctx_, *p, t, tile, error, false))
            {
            case availability::absent:
                break;
            case availability::wanted:
                d.loading = d.loading || imagery || d.tile == nullptr;
                static_cast<void>(local_.push_back({p->id, t, 0.0}));
                break;
            case availability::pending:
                d.loading = d.loading || imagery || d.tile == nullptr;
                break;
            case availability::ready:
                if (!imagery && d.tile == nullptr)
                {
                    d.pack = p;
                    d.tile = tile;
                }
                if (t.level < l.max_level)
                {
                    d.children |= tile->header.child_mask;
                }
                break;
            }
        }
        return d;
    }

    const cached_box&
    bounds(const tile_id& t, double min_height, double max_height, bool is_probe = false) noexcept
    {
        const std::uint64_t key = tile_key(t) | (is_probe ? probe_key_bit : 0);
        cached_box* c = ctx_.boxes.find(key);
        if (c == nullptr || c->min_height != min_height || c->max_height != max_height)
        {
            cached_box fresh;
            fresh.box = tile_bounding_box(ctx_.shape, t, min_height, max_height);
            fresh.has_horizon_point = horizon_point(
                ctx_.shape, lowest_surface_height, tile_region(t), max_height, fresh.horizon_point);
            fresh.min_height = min_height;
            fresh.max_height = max_height;
            fresh.last_used = ctx_.frame;
            cached_box* stored = c != nullptr ? c : ctx_.boxes.insert(key, fresh);
            if (stored == nullptr)
            {
                scratch_ = fresh; // Out of memory: compute without caching.
                return scratch_;
            }
            *stored = fresh;
            c = stored;
        }
        c->last_used = ctx_.frame;
        return *c;
    }

    bool visible(const tile_id& t, double min_height, double max_height, bool is_probe) noexcept
    {
        const cached_box& c = bounds(t, min_height, max_height, is_probe);
        return box_in_frustum(ctx_.view, c.box) &&
               !(c.has_horizon_point && scaled_point_below_horizon(occluder_, c.horizon_point)) &&
               !box_below_horizon(occluder_, c.box);
    }

    // True if part of a large tile is in view, by its descendants down to probe_level. The
    // recursion is at most probe_level deep.
    // NOLINTNEXTLINE(misc-no-recursion)
    bool probe(const tile_id& t, double min_height, double max_height) noexcept
    {
        if (t.level >= probe_level)
        {
            return true;
        }
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            const tile_id child = tile_child(t, c);
            if (visible(child, min_height, max_height, true) &&
                probe(child, min_height, max_height))
            {
                return true;
            }
        }
        return false;
    }

    void emit(const tile_id& t, const source& s) noexcept
    {
        static_cast<void>(ctx_.selected.push_back(
            {t, s.id, s.pack != nullptr ? s.pack->id : 0, s.min_height, s.max_height}));
    }

    // True if a tile has a mesh of its own data on the GPU, not a placeholder.
    bool mesh_ready(const tile_id& t) const noexcept
    {
        const mesh_state* m = ctx_.meshes.find(tile_key(t));
        const gpu_resource* r =
            m != nullptr && m->current != 0 ? ctx_.resources.find(m->current) : nullptr;
        return r != nullptr && r->current == gpu_resource::state::resident && !r->garbage &&
               !r->placeholder;
    }

    // The recursion is at most max_tile_level deep.
    // NOLINTNEXTLINE(misc-no-recursion)
    void visit(const tile_id& t, const source& parent) noexcept
    {
        local_.clear();
        const tile_data d = find(t);

        // Bounds: the tile's own heights, its parent's bounds for it, or those of the data
        // that draws it.
        source own = parent;
        double min_height = parent.min_height;
        double max_height = parent.max_height;
        if (d.tile != nullptr)
        {
            own = {t, d.pack, d.tile, d.tile->min_height, d.tile->max_height};
            if (d.tile->min_height <= d.tile->max_height)
            {
                min_height = d.tile->min_height;
                max_height = d.tile->max_height;
            }
        }
        else if (parent.tile != nullptr && parent.id.level + 1 == t.level &&
                 parent.tile->header.has_child(tile_child_index(t)))
        {
            const std::uint32_t c = tile_child_index(t);
            min_height = parent.tile->header.child_min[c];
            max_height = parent.tile->header.child_max[c];
        }

        if (!visible(t, min_height, max_height, false))
        {
            return;
        }
        const oriented_box& b = bounds(t, min_height, max_height).box;
        const std::uint32_t size =
            own.pack != nullptr ? own.pack->manifest.layer().tile_size : default_tile_size;
        const double error = screen_space_error(ctx_.view,
                                                tile_geometric_error(ctx_.shape, t.level, size),
                                                distance_to_box(ctx_.view.eye, b));
        // Refine only from loaded data, so that detail arrives level by level.
        const bool settled = d.tile != nullptr || !d.loading;
        const bool can_refine = settled && d.children != 0 && t.level != max_tile_level;
        // A coarser tile replaces the finer terrain that the last frame drew inside it only
        // once its own mesh is ready. Until then its children are selected instead, which keep
        // that finer terrain and fill the rest with placeholders, while its mesh builds: the
        // finer tiles alone would leave holes where they do not cover it.
        bool coarse_waits = false;
        if (error <= ctx_.view.max_error && can_refine && !mesh_ready(t) &&
            drawn_below_.find(tile_key(t)) != nullptr)
        {
            coarse_waits = true;
            static_cast<void>(ctx_.prefetched.push_back({t,
                                                         own.id,
                                                         own.pack != nullptr ? own.pack->id : 0,
                                                         own.min_height,
                                                         own.max_height}));
        }
        const bool leaf = (error <= ctx_.view.max_error && !coarse_waits) || !can_refine;
        if (leaf && !probe(t, min_height, max_height))
        {
            return;
        }
        for (load_request r : local_)
        {
            r.screen_space_error = error;
            static_cast<void>(ctx_.wanted.push_back(r));
        }
        if (leaf)
        {
            emit(t, own);
            return;
        }
        for (std::uint32_t child = 0; child < 4; ++child)
        {
            visit(tile_child(t, child), own);
        }
    }

    // The traversal lives for one update, inside it.
    vgl_context& ctx_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    horizon_occluder occluder_;
    vector<load_request> local_{ctx_.memory}; // Requests of the tile being visited.
    u64_map<std::uint8_t> drawn_below_{ctx_.memory};
    cached_box scratch_;
};

void start_requests(vgl_context& ctx) noexcept
{
    vector<load_request>& w = ctx.wanted;
    std::sort(w.begin(), w.end(), [](const load_request& a, const load_request& b) {
        if (a.id.level != b.id.level)
        {
            return a.id.level < b.id.level;
        }
        return a.screen_space_error > b.screen_space_error;
    });
    for (const load_request& r : w)
    {
        if (ctx.outstanding.size() >= max_outstanding_reads)
        {
            break;
        }
        for (vgl_pack* p : ctx.packs)
        {
            if (p->id == r.pack_id)
            {
                tile_state* t = nullptr;
                vgl_result error = VGL_RESULT_SUCCESS;
                static_cast<void>(find_tile(ctx, *p, r.id, t, error, true));
                break;
            }
        }
    }
}

// Forgets boxes of tiles no longer visited.
void sweep_boxes(vgl_context& ctx) noexcept
{
    vector<std::uint64_t> stale(ctx.memory);
    ctx.boxes.for_each([&](std::uint64_t key, const cached_box& c) {
        if (c.last_used + 1 < ctx.frame)
        {
            static_cast<void>(stale.push_back(key));
        }
    });
    for (const std::uint64_t key : stale)
    {
        static_cast<void>(ctx.boxes.erase(key));
    }
}

} // namespace

void select_tiles(vgl_context& ctx) noexcept
{
    traversal(ctx).run();
    start_requests(ctx);
    sweep_boxes(ctx);
}

} // namespace vgl::internal

using namespace vgl::internal;

extern "C" VGL_API vgl_result vgl_context_view_set(vgl_context* context, const vgl_view* view)
{
    if (context == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (view == nullptr)
    {
        context->has_view = false;
        context->selected.clear();
        return VGL_RESULT_SUCCESS;
    }
    if (view->struct_size < sizeof(vgl_view))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (view->struct_size > sizeof(vgl_view))
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(view);
        for (std::size_t i = sizeof(vgl_view); i < view->struct_size; ++i)
        {
            if (bytes[i] != 0)
            {
                return VGL_RESULT_NOT_SUPPORTED;
            }
        }
    }
    view_state state;
    if (!make_view_state(*view, state))
    {
        context->log.log(VGL_LOG_LEVEL_ERROR,
                         "vgl_context_view_set: the view is invalid; check that the matrices are "
                         "finite, that the view matrix is an invertible affine transform, and "
                         "that the viewport is not empty");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    context->view = state;
    context->host_view = *view;
    context->host_view.struct_size = sizeof(vgl_view);
    context->has_view = true;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_view_project(const vgl_view* view,
                                               const vgl_dvec3* position,
                                               vgl_dvec3* out_window)
{
    if (view == nullptr || position == nullptr || out_window == nullptr ||
        view->struct_size < sizeof(vgl_view) || !std::isfinite(position->x) ||
        !std::isfinite(position->y) || !std::isfinite(position->z))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (view->struct_size > sizeof(vgl_view))
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(view);
        for (std::size_t i = sizeof(vgl_view); i < view->struct_size; ++i)
        {
            if (bytes[i] != 0)
            {
                return VGL_RESULT_NOT_SUPPORTED;
            }
        }
    }
    view_state state;
    if (!make_view_state(*view, state))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    // Camera space first, then clip space: composing the two matrices would round the large
    // translation of the view into the projection's terms.
    const dmat4 v = dmat4::from_c(view->view_matrix);
    const dmat4 p = dmat4::from_c(view->projection_matrix);
    const dvec3 camera = transform_point(v, dvec3::from_c(*position));
    const auto clip = [&](int row) {
        return p(row, 0) * camera.x + p(row, 1) * camera.y + p(row, 2) * camera.z + p(row, 3);
    };
    const double w = clip(3);
    if (!(w > 0.0))
    {
        return VGL_RESULT_NOT_FOUND;
    }
    const double width = view->viewport_width;
    const double height = view->viewport_height;
    *out_window = {(clip(0) / w * 0.5 + 0.5) * width, (0.5 - clip(1) / w * 0.5) * height, w};
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_view_geodetic_project(const vgl_view* view,
                                                        const vgl_ellipsoid* ellipsoid,
                                                        const vgl_geodetic* geodetic,
                                                        vgl_dvec3* out_window)
{
    if (view == nullptr || out_window == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_ellipsoid wgs84{};
    if (ellipsoid == nullptr)
    {
        static_cast<void>(vgl_ellipsoid_wgs84_get(&wgs84));
        ellipsoid = &wgs84;
    }
    vgl_dvec3 position{};
    const vgl_result r = vgl_ellipsoid_geodetic_to_ecef(ellipsoid, geodetic, &position);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    return vgl_view_project(view, &position, out_window);
}

extern "C" VGL_API vgl_result vgl_context_view_get(const vgl_context* context, vgl_view* out_view)
{
    if (context == nullptr || out_view == nullptr || out_view->struct_size < 8)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (!context->has_view)
    {
        return VGL_RESULT_NOT_FOUND;
    }
    // The fields that fit, and the size written (docs/c-api.md, section 8.4).
    vgl_view v = context->host_view;
    const auto size =
        static_cast<std::uint32_t>(std::min<std::size_t>(out_view->struct_size, sizeof(vgl_view)));
    v.struct_size = size;
    std::memcpy(out_view, &v, size);
    return VGL_RESULT_SUCCESS;
}
