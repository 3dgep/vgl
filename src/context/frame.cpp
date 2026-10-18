// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The core side of the render interface (docs/render-interface.md). A frame decides what the
// consumer destroys, creates and draws:
//
// 1. Tiles whose next mesh is resident switch to it, retiring the old one.
// 2. Creates: meshes of selected tiles waiting for upload, with the index lists they need,
//    and the textures selected tiles want, within the upload budget: first placeholders, which
//    are small and outside the budget, then the meshes of tiles that have nothing of their own
//    to draw but a placeholder, then coarsest first.
// 3. The draw set: the selected tiles whose meshes are resident or created in this frame,
//    except where a tile drawn in the last frame still covers a region whose replacements are
//    not all ready yet. A tile with only its placeholder is drawn with it where nothing else
//    covers it.
// 4. Destroys: retired resources, and resources neither drawn nor wanted now, least recently
//    drawn first, while the GPU budget is exceeded.
// 5. Draw items, nearest first, with model-view matrices composed in double precision, and
//    for each item one imagery layer per imagery pack: the deepest resident texture.

#include "context/context_impl.hpp"
#include "foundation/math.hpp"
#include "tiling/tile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace vgl::internal
{

namespace
{

constexpr std::uint64_t default_upload_budget = VGL_UPLOAD_BUDGET_DEFAULT;
constexpr std::uint32_t none = 0xFFFFFFFFu;

bool is_live(const gpu_resource* r, gpu_resource::state s) noexcept
{
    return r != nullptr && r->current == s && !r->garbage;
}

// The index list resource of a mesh, or null.
gpu_resource* index_list_of(vgl_context& ctx, const gpu_resource& mesh) noexcept
{
    const std::uint64_t* id = ctx.index_lists.find(mesh.grid);
    return id != nullptr ? ctx.resources.find(*id) : nullptr;
}

// True if a mesh can be drawn in this frame: it and its index list are resident, or offered
// for creation in this frame (render interface, section 4.2).
bool drawable_now(vgl_context& ctx, std::uint64_t id) noexcept
{
    const gpu_resource* r = id != 0 ? ctx.resources.find(id) : nullptr;
    const auto usable = [](const gpu_resource* x) {
        return is_live(x, gpu_resource::state::resident) ||
               is_live(x, gpu_resource::state::offered);
    };
    return usable(r) && usable(index_list_of(ctx, *r));
}

// The newest mesh a selected tile can draw in this frame, or 0.
std::uint64_t newest_mesh(vgl_context& ctx, const tile_id& t) noexcept
{
    const mesh_state* m = ctx.meshes.find(tile_key(t));
    if (m == nullptr)
    {
        return 0;
    }
    if (drawable_now(ctx, m->next))
    {
        return m->next;
    }
    return drawable_now(ctx, m->current) ? m->current : 0;
}

// True if a mesh can be drawn: it and its index list are resident.
bool drawable(vgl_context& ctx, std::uint64_t id) noexcept
{
    gpu_resource* r = id != 0 ? ctx.resources.find(id) : nullptr;
    return is_live(r, gpu_resource::state::resident) &&
           is_live(index_list_of(ctx, *r), gpu_resource::state::resident);
}

void promote(vgl_context& ctx) noexcept
{
    ctx.meshes.for_each([&](std::uint64_t, mesh_state& m) {
        if (m.next != 0 && drawable(ctx, m.next))
        {
            if (m.current != 0)
            {
                retire_resource(ctx, m.current);
            }
            m.current = m.next;
            m.next = 0;
        }
    });
}

// Decides what to draw (step 2) into ctx.frame_tiles. Returns false if out of memory.
bool decide_draw_set(vgl_context& ctx) noexcept
{
    ctx.frame_tiles.clear();
    ctx.frame_selected.clear();
    ctx.frame_drawn.clear();
    const std::size_t selected = ctx.has_view ? ctx.selected.size() : 0;
    const std::size_t previous = ctx.drawn.size();
    if (!ctx.frame_ready.resize(selected) || !ctx.frame_owner.resize(selected) ||
        !ctx.frame_counts.resize(3 * previous))
    {
        return false;
    }

    // Which selected tiles can be drawn with their own mesh (1), rather than a placeholder or
    // nothing (0). A tile that waits for the last frame's tiles inside it becomes 2.
    for (std::size_t i = 0; i < selected; ++i)
    {
        const tile_id& t = ctx.selected[i].id;
        if (ctx.frame_selected.insert(tile_key(t), static_cast<std::uint32_t>(i)) == nullptr)
        {
            return false;
        }
        const std::uint64_t mesh = newest_mesh(ctx, t);
        ctx.frame_ready[i] = mesh != 0 && !ctx.resources.find(mesh)->placeholder ? 1 : 0;
        ctx.frame_owner[i] = none;
    }
    for (std::size_t j = 0; j < previous; ++j)
    {
        if (ctx.frame_drawn.insert(tile_key(ctx.drawn[j].id), static_cast<std::uint32_t>(j)) ==
            nullptr)
        {
            return false;
        }
        ctx.frame_counts[3 * j] = 0;     // Selected tiles inside it.
        ctx.frame_counts[3 * j + 1] = 0; // Of which ready.
        ctx.frame_counts[3 * j + 2] = 0; // 1 if it stays.
    }

    // The last-frame tile, if any, that contains each selected tile.
    for (std::size_t i = 0; i < selected; ++i)
    {
        const tile_id& t = ctx.selected[i].id;
        for (std::uint32_t level = t.level + 1; level-- > 0;)
        {
            const std::uint32_t* j = ctx.frame_drawn.find(tile_key(tile_ancestor(t, level)));
            if (j != nullptr)
            {
                ctx.frame_owner[i] = *j;
                const std::size_t k = std::size_t{3} * *j;
                ++ctx.frame_counts[k];
                ctx.frame_counts[k + 1] += ctx.frame_ready[i];
                break;
            }
        }
    }

    // Last-frame tiles stay while what replaces them is not all ready.
    for (std::size_t j = 0; j < previous; ++j)
    {
        drawn_tile p = ctx.drawn[j];
        if (!drawable(ctx, p.mesh))
        {
            // A newer mesh of its own may have replaced it since (step 1): it stays with that.
            const mesh_state* m = ctx.meshes.find(tile_key(p.id));
            if (m == nullptr || !drawable(ctx, m->current))
            {
                continue;
            }
            p.mesh = m->current;
        }
        bool stays = false;
        const std::uint32_t* ancestor = nullptr;
        for (std::uint32_t level = p.id.level; level-- > 0 && ancestor == nullptr;)
        {
            ancestor = ctx.frame_selected.find(tile_key(tile_ancestor(p.id, level)));
        }
        if (ancestor != nullptr)
        {
            // Zooming out: it stays until the coarser tile is ready, which waits meanwhile.
            stays = ctx.frame_ready[*ancestor] != 1;
            if (stays)
            {
                ctx.frame_ready[*ancestor] = 2;
            }
        }
        else
        {
            const std::uint32_t inside = ctx.frame_counts[3 * j];
            stays = inside != 0 && ctx.frame_counts[3 * j + 1] != inside;
        }
        ctx.frame_counts[3 * j + 2] = stays ? 1 : 0;
        if (stays && !ctx.frame_tiles.push_back(p))
        {
            return false;
        }
    }

    // The rest: ready, or else drawn with its placeholder. Both may have been offered in this
    // very frame, so that a tile's first frame is not a hole (render interface, section 4.2).
    for (std::size_t i = 0; i < selected; ++i)
    {
        const std::uint32_t owner = ctx.frame_owner[i];
        const bool covered = owner != none && ctx.frame_counts[3 * owner + 2] != 0;
        const std::uint64_t mesh =
            ctx.frame_ready[i] != 2 && !covered ? newest_mesh(ctx, ctx.selected[i].id) : 0;
        if (mesh != 0 && !ctx.frame_tiles.push_back({ctx.selected[i].id, mesh}))
        {
            return false;
        }
    }
    return true;
}

// A ready imagery pack whose layer is shown.
bool is_imagery(const vgl_pack& p) noexcept
{
    return p.state == VGL_PACK_STATE_READY &&
           p.manifest.layer().type == pack::layer_type::imagery && p.opacity > 0.0f;
}

// Marks what this frame draws: meshes, their index lists, and the textures of their layers.
void mark_drawn(vgl_context& ctx) noexcept
{
    for (const drawn_tile& d : ctx.frame_tiles)
    {
        gpu_resource* r = ctx.resources.find(d.mesh);
        r->last_drawn = ctx.render_frame;
        index_list_of(ctx, *r)->last_drawn = ctx.render_frame;
        for (const vgl_pack* p : ctx.packs)
        {
            std::uint32_t level = 0;
            const std::uint64_t texture = is_imagery(*p) ? texture_for(ctx, *p, d.id, level) : 0;
            if (texture != 0)
            {
                ctx.resources.find(texture)->last_drawn = ctx.render_frame;
            }
        }
    }
}

// Marks the textures that selected tiles wanted in the last update: they are uploaded, and
// kept from eviction while they wait to be drawn.
void mark_wanted(vgl_context& ctx) noexcept
{
    for (vgl_pack* p : ctx.packs)
    {
        p->textures.for_each([&](std::uint64_t, const texture_state& s) {
            if (s.current != 0 && s.last_wanted + 1 == ctx.frame)
            {
                ctx.resources.find(s.current)->last_wanted = ctx.render_frame;
            }
        });
    }
}

// The texture format to offer: the consumer's most preferred of those the core produces.
vgl_texture_format texture_format(const vgl_context& ctx) noexcept
{
    for (std::uint32_t i = 0; i < ctx.texture_format_count; ++i)
    {
        const vgl_texture_format f = ctx.texture_formats[i];
        if (f == VGL_TEXTURE_FORMAT_RGBA8 || f == VGL_TEXTURE_FORMAT_RGBA8_SRGB)
        {
            return f;
        }
    }
    return VGL_TEXTURE_FORMAT_RGBA8;
}

bool offer(vgl_context& ctx, std::uint64_t id, gpu_resource& r) noexcept
{
    if (!ctx.frame_offered.push_back(id))
    {
        return false;
    }
    if (r.resource_kind == gpu_resource::kind::index_list)
    {
        vgl_index_list_data d{};
        d.resource = id;
        d.indices = r.indices.data();
        d.index_count = r.index_count;
        d.index_size = r.index_size;
        if (!ctx.frame_index_lists.push_back(d))
        {
            ctx.frame_offered.pop_back();
            return false;
        }
    }
    else if (r.resource_kind == gpu_resource::kind::texture)
    {
        vgl_texture_data d{};
        d.resource = id;
        d.format = texture_format(ctx);
        d.width = r.image.width;
        d.height = r.image.height;
        d.mip_count = r.image.levels;
        d.mip_data = r.mip_data.data();
        d.mip_sizes = r.mip_sizes.data();
        if (!ctx.frame_textures.push_back(d))
        {
            ctx.frame_offered.pop_back();
            return false;
        }
    }
    else
    {
        vgl_mesh_data d{};
        d.resource = id;
        d.index_list = *ctx.index_lists.find(r.grid);
        d.vertices = r.vertices.data();
        d.vertex_count = static_cast<std::uint32_t>(r.vertices.size());
        d.vertex_size = sizeof(tile_vertex);
        d.tile = {r.tile.level, r.tile.x, r.tile.y, 0};
        d.centre = r.centre.to_c();
        d.min_height = r.min_height;
        d.max_height = r.max_height;
        d.samples = r.samples.data();
        d.sample_grid = r.sample_grid;
        d.sample_size = sizeof(tile_sample);
        if (!ctx.frame_meshes.push_back(d))
        {
            ctx.frame_offered.pop_back();
            return false;
        }
    }
    r.current = gpu_resource::state::offered;
    ctx.gpu_used += r.bytes;
    return true;
}

// Step 3.
void choose_uploads(vgl_context& ctx, std::uint64_t budget) noexcept
{
    vector<std::uint64_t>& candidates = ctx.frame_candidates;
    candidates.clear();
    const std::size_t selected = ctx.has_view ? ctx.selected.size() : 0;
    const std::size_t prefetched = ctx.has_view ? ctx.prefetched.size() : 0;
    for (std::size_t i = 0; i < selected + prefetched; ++i)
    {
        const tile_id& t = i < selected ? ctx.selected[i].id : ctx.prefetched[i - selected].id;
        const mesh_state* m = ctx.meshes.find(tile_key(t));
        const gpu_resource* r =
            m != nullptr && m->next != 0 ? ctx.resources.find(m->next) : nullptr;
        if (is_live(r, gpu_resource::state::pending) && !candidates.push_back(m->next))
        {
            break;
        }
    }
    ctx.resources.for_each([&](std::uint64_t id, const gpu_resource& r) {
        if (r.resource_kind == gpu_resource::kind::texture &&
            is_live(&r, gpu_resource::state::pending) && r.last_wanted == ctx.render_frame)
        {
            static_cast<void>(candidates.push_back(id));
        }
    });
    // Placeholders first; then the meshes of tiles with nothing of their own to draw, so that a
    // stream of finer data elsewhere cannot keep them waiting; then coarsest first, then oldest.
    const auto urgent = [&](const gpu_resource& r) {
        if (r.resource_kind != gpu_resource::kind::mesh)
        {
            return false;
        }
        const mesh_state* m = ctx.meshes.find(tile_key(r.tile));
        return m == nullptr || !drawable(ctx, m->current) ||
               ctx.resources.find(m->current)->placeholder;
    };
    std::sort(candidates.begin(), candidates.end(), [&](std::uint64_t a, std::uint64_t b) {
        const gpu_resource& ra = *ctx.resources.find(a);
        const gpu_resource& rb = *ctx.resources.find(b);
        if (ra.placeholder != rb.placeholder)
        {
            return ra.placeholder;
        }
        const bool ua = urgent(ra);
        const bool ub = urgent(rb);
        if (ua != ub)
        {
            return ua;
        }
        return ra.tile.level != rb.tile.level ? ra.tile.level < rb.tile.level : a < b;
    });
    std::uint64_t used = 0;
    bool any = false;
    for (const std::uint64_t id : candidates)
    {
        gpu_resource* mesh = ctx.resources.find(id);
        if (mesh->resource_kind == gpu_resource::kind::texture)
        {
            if (any && used + mesh->bytes > budget)
            {
                break;
            }
            if (!offer(ctx, id, *mesh))
            {
                break;
            }
            used += mesh->bytes;
            any = true;
            continue;
        }
        const std::uint64_t* list_id = ctx.index_lists.find(mesh->grid);
        gpu_resource* list = list_id != nullptr ? ctx.resources.find(*list_id) : nullptr;
        if (list == nullptr)
        {
            continue;
        }
        const bool list_needed = list->current == gpu_resource::state::pending;
        const std::uint64_t cost = mesh->bytes + (list_needed ? list->bytes : 0);
        if (any && used + cost > budget && !mesh->placeholder)
        {
            break;
        }
        if (list_needed && !offer(ctx, *list_id, *list))
        {
            break;
        }
        if (!offer(ctx, id, *mesh))
        {
            break;
        }
        used += cost;
        any = true;
    }
}

// Step 4.
void choose_destroys(vgl_context& ctx) noexcept
{
    if (ctx.gpu_used > ctx.gpu_budget)
    {
        vector<std::uint64_t>& candidates = ctx.frame_candidates;
        candidates.clear();
        ctx.resources.for_each([&](std::uint64_t id, const gpu_resource& r) {
            // Meshes of selected tiles stay even when not drawn yet: they are waiting for the
            // rest of a region to arrive, and evicting them would keep it from ever arriving.
            // Textures that selected tiles want stay for the same reason.
            const bool idle =
                is_live(&r, gpu_resource::state::resident) && r.last_drawn < ctx.render_frame;
            // So do the meshes of coarser tiles waiting to replace finer ones, which the last
            // update used as it did the selected tiles'.
            const mesh_state* m = r.resource_kind == gpu_resource::kind::mesh
                                      ? ctx.meshes.find(tile_key(r.tile))
                                      : nullptr;
            const bool in_use = ctx.frame_selected.find(tile_key(r.tile)) != nullptr ||
                                (m != nullptr && m->last_used + 1 == ctx.frame);
            const bool evictable = (r.resource_kind == gpu_resource::kind::mesh && !in_use) ||
                                   (r.resource_kind == gpu_resource::kind::texture &&
                                    r.last_wanted < ctx.render_frame);
            if (idle && evictable)
            {
                static_cast<void>(candidates.push_back(id));
            }
        });
        // Least recently drawn first, then oldest.
        std::sort(candidates.begin(), candidates.end(), [&](std::uint64_t a, std::uint64_t b) {
            const std::uint64_t la = ctx.resources.find(a)->last_drawn;
            const std::uint64_t lb = ctx.resources.find(b)->last_drawn;
            return la != lb ? la < lb : a < b;
        });
        std::uint64_t used = ctx.gpu_used;
        for (const std::uint64_t id : candidates)
        {
            if (used <= ctx.gpu_budget)
            {
                break;
            }
            gpu_resource* r = ctx.resources.find(id);
            used -= r->bytes;
            const std::uint64_t key = tile_key(r->tile);
            if (r->resource_kind == gpu_resource::kind::texture)
            {
                // Decoded again from the cached tile when wanted.
                for (vgl_pack* p : ctx.packs)
                {
                    if (p->id == r->pack_id)
                    {
                        static_cast<void>(p->textures.erase(key));
                    }
                }
                retire_resource(ctx, id);
                continue;
            }
            retire_resource(ctx, id);
            if (mesh_state* m = ctx.meshes.find(key); m != nullptr)
            {
                m->current = m->current == id ? 0 : m->current;
                m->next = m->next == id ? 0 : m->next;
                if (m->current == 0 && m->next == 0 && !m->building)
                {
                    static_cast<void>(ctx.meshes.erase(key));
                }
            }
        }
    }

    ctx.resources.for_each([&](std::uint64_t id, const gpu_resource& r) {
        if (r.garbage && r.current == gpu_resource::state::resident)
        {
            static_cast<void>(ctx.frame_destroys.push_back(id));
        }
    });
    for (const std::uint64_t id : ctx.frame_destroys)
    {
        ctx.gpu_used -= ctx.resources.find(id)->bytes;
        static_cast<void>(ctx.resources.erase(id));
    }
}

// The imagery layers of a draw item, bottom first: one per imagery pack, from the lowest
// priority up, each with the deepest resident texture for the tile (render interface,
// section 5). A tile drawn with an ancestor's texture maps its texture coordinates into the
// part of the ancestor it covers, inside the ancestor's border.
bool write_layers(vgl_context& ctx, const tile_id& t, vgl_draw_item& item) noexcept
{
    item.first_layer = static_cast<std::uint32_t>(ctx.frame_layers.size());
    item.layer_count = 0;
    for (std::size_t i = ctx.packs.size(); i-- > 0;)
    {
        vgl_pack& p = *ctx.packs[i];
        std::uint32_t level = 0;
        const std::uint64_t texture = is_imagery(p) ? texture_for(ctx, p, t, level) : 0;
        if (texture == 0)
        {
            continue;
        }
        const pack::layer_description& l = p.manifest.layer();
        const double inner = l.tile_size;
        const double side = inner + 2.0 * l.border;
        const std::uint32_t d = t.level - level;
        const double scale = std::ldexp(1.0, -static_cast<int>(d));
        // Where the tile lies in the imagery tile, as a fraction of it.
        const std::uint32_t within = (1u << d) - 1u;
        const double u = (t.x & within) * scale;
        const double v = (t.y & within) * scale;
        vgl_draw_layer layer{};
        layer.texture = texture;
        layer.texture_handle = ctx.resources.find(texture)->handle;
        layer.uv_scale_offset[0] = static_cast<float>(scale * inner / side);
        layer.uv_scale_offset[1] = static_cast<float>(scale * inner / side);
        layer.uv_scale_offset[2] = static_cast<float>((l.border + u * inner) / side);
        layer.uv_scale_offset[3] = static_cast<float>((l.border + v * inner) / side);
        layer.opacity = p.opacity;
        if (!ctx.frame_layers.push_back(layer))
        {
            return false;
        }
        ++item.layer_count;
        p.imagery_drawn = ctx.render_frame;
    }
    return true;
}

// Copies a frame's list to the consumer's stride, where it differs from this header's size
// (docs/render-interface.md, section 5): each item's bytes, then zeros for the fields of a later
// minor version, which this one does not know.
template <typename T>
bool restride(const vector<T>& items, std::uint32_t stride, vector<std::byte>& out) noexcept
{
    out.clear();
    if (stride == sizeof(T))
    {
        return true;
    }
    if (!out.resize(std::size_t{stride} * items.size()))
    {
        return false;
    }
    for (std::size_t k = 0; k < items.size(); ++k)
    {
        std::memcpy(
            out.data() + k * stride, &items.data()[k], std::min<std::size_t>(stride, sizeof(T)));
    }
    return true;
}

// A list as the consumer reads it: at its own stride if it asked for one.
template <typename T>
const T* list_at_stride(const vector<T>& items,
                        std::uint32_t stride,
                        const vector<std::byte>& strided) noexcept
{
    return stride == sizeof(T) ? items.data() : reinterpret_cast<const T*>(strided.data());
}

// Step 5.
bool write_draw_items(vgl_context& ctx, const dmat4& view, const dvec3& eye) noexcept
{
    std::sort(ctx.frame_tiles.begin(),
              ctx.frame_tiles.end(),
              [&](const drawn_tile& a, const drawn_tile& b) {
                  const double da = length(ctx.resources.find(a.mesh)->centre - eye);
                  const double db = length(ctx.resources.find(b.mesh)->centre - eye);
                  return da != db ? da < db : tile_key(a.id) < tile_key(b.id);
              });
    const std::size_t stride = ctx.frame_draw_stride;
    if (!ctx.frame_draw.resize(stride * ctx.frame_tiles.size()))
    {
        return false;
    }
    std::byte* out = ctx.frame_draw.data();
    for (const drawn_tile& d : ctx.frame_tiles)
    {
        const gpu_resource* r = ctx.resources.find(d.mesh);
        const std::uint64_t list_id = *ctx.index_lists.find(r->grid);
        const gpu_resource* list = ctx.resources.find(list_id);

        vgl_draw_item item{};
        item.tile = {d.id.level, d.id.x, d.id.y, 0};
        item.mesh = d.mesh;
        item.mesh_handle = r->handle;
        item.index_list = list_id;
        item.index_list_handle = list->handle;
        item.index_count = list->index_count;
        item.index_size = list->index_size;
        // view * translate(centre): the translation is where the centre lands.
        const dvec3 t = transform_point(view, r->centre);
        for (std::size_t k = 0; k < 12; ++k)
        {
            item.model_view[k] = static_cast<float>(view.m[k]);
        }
        item.model_view[12] = static_cast<float>(t.x);
        item.model_view[13] = static_cast<float>(t.y);
        item.model_view[14] = static_cast<float>(t.z);
        item.model_view[15] = static_cast<float>(view.m[15]);
        item.centre = r->centre.to_c();
        item.min_height = r->min_height;
        item.max_height = r->max_height;
        for (std::size_t k = 0; k < 4; ++k)
        {
            item.geoid_heights[k] = r->geoid[k];
        }
        item.has_geoid = r->has_geoid ? VGL_TRUE : VGL_FALSE;
        if (!write_layers(ctx, d.id, item))
        {
            return false;
        }

        // A consumer compiled against an older header gets the fields it knows; one compiled
        // against a newer header gets zeros in the rest.
        const std::size_t known = std::min(stride, sizeof(vgl_draw_item));
        std::memcpy(out, &item, known);
        std::memset(out + known, 0, stride - known);
        out += stride;
    }
    ctx.frame_draw_count = static_cast<std::uint32_t>(ctx.frame_tiles.size());
    return true;
}

// The eye of a view matrix, or the given one if it cannot be inverted.
dvec3 eye_of(const dmat4& v, const dvec3& fallback) noexcept
{
    vgl_view probe{};
    probe.struct_size = sizeof(probe);
    probe.view_matrix = v.to_c();
    probe.projection_matrix = dmat4{}.to_c();
    probe.viewport_width = 1;
    probe.viewport_height = 1;
    view_state s;
    return make_view_state(probe, s) ? s.eye : fallback;
}

} // namespace

} // namespace vgl::internal

using namespace vgl::internal;

extern "C" VGL_API vgl_result vgl_context_frame_begin(vgl_context* context,
                                                      const vgl_frame_begin_info* info,
                                                      vgl_frame** out_frame)
{
    if (out_frame == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_frame = nullptr;
    vgl_context* const ctx = context;
    if (ctx == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (ctx->frame_open)
    {
        ctx->log.log(VGL_LOG_LEVEL_ERROR,
                     "vgl_context_frame_begin: a frame is already open; call vgl_frame_end first");
        return VGL_RESULT_INVALID_OPERATION;
    }

    vgl_frame_begin_info settings{};
    if (info != nullptr)
    {
        if (info->struct_size < sizeof(vgl_frame_begin_info))
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
        const auto* bytes = reinterpret_cast<const unsigned char*>(info);
        for (std::size_t i = sizeof(vgl_frame_begin_info); i < info->struct_size; ++i)
        {
            if (bytes[i] != 0)
            {
                return VGL_RESULT_NOT_SUPPORTED;
            }
        }
        std::memcpy(&settings, info, sizeof(settings));
    }
    // Each stride is at least the size in this header: earlier versions have no smaller ones.
    if ((settings.draw_item_size != 0 && settings.draw_item_size < sizeof(vgl_draw_item)) ||
        (settings.draw_layer_size != 0 && settings.draw_layer_size < sizeof(vgl_draw_layer)) ||
        (settings.index_list_data_size != 0 &&
         settings.index_list_data_size < sizeof(vgl_index_list_data)) ||
        (settings.mesh_data_size != 0 && settings.mesh_data_size < sizeof(vgl_mesh_data)) ||
        (settings.texture_data_size != 0 && settings.texture_data_size < sizeof(vgl_texture_data)))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    dmat4 view = ctx->view.view_matrix;
    dvec3 eye = ctx->view.eye;
    if (settings.view_matrix != nullptr)
    {
        view = dmat4::from_c(*settings.view_matrix);
        if (!std::all_of(view.m.begin(), view.m.end(), [](double v) { return std::isfinite(v); }))
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
        eye = eye_of(view, eye);
    }
    const std::uint64_t upload_budget =
        settings.upload_budget == 0 ? default_upload_budget : settings.upload_budget;
    const auto start = std::chrono::steady_clock::now();

    ++ctx->render_frame;
    ctx->frame_destroys.clear();
    ctx->frame_index_lists.clear();
    ctx->frame_meshes.clear();
    ctx->frame_textures.clear();
    ctx->frame_layers.clear();
    ctx->frame_offered.clear();
    ctx->frame_draw_count = 0;
    ctx->frame_draw_stride =
        settings.draw_item_size == 0 ? sizeof(vgl_draw_item) : settings.draw_item_size;
    ctx->frame_layer_stride =
        settings.draw_layer_size == 0 ? sizeof(vgl_draw_layer) : settings.draw_layer_size;
    ctx->frame_index_list_stride = settings.index_list_data_size == 0
                                       ? sizeof(vgl_index_list_data)
                                       : settings.index_list_data_size;
    ctx->frame_mesh_stride =
        settings.mesh_data_size == 0 ? sizeof(vgl_mesh_data) : settings.mesh_data_size;
    ctx->frame_texture_stride =
        settings.texture_data_size == 0 ? sizeof(vgl_texture_data) : settings.texture_data_size;

    promote(*ctx);
    mark_wanted(*ctx);
    choose_uploads(*ctx, upload_budget);
    if (!decide_draw_set(*ctx))
    {
        // Draw what was drawn before, as far as it still exists.
        ctx->frame_tiles.clear();
        for (const drawn_tile& d : ctx->drawn)
        {
            if (drawable(*ctx, d.mesh))
            {
                static_cast<void>(ctx->frame_tiles.push_back(d));
            }
        }
    }
    mark_drawn(*ctx);
    choose_destroys(*ctx);
    if (!write_draw_items(*ctx, view, eye) ||
        !restride(
            ctx->frame_index_lists, ctx->frame_index_list_stride, ctx->frame_index_lists_strided) ||
        !restride(ctx->frame_meshes, ctx->frame_mesh_stride, ctx->frame_meshes_strided) ||
        !restride(ctx->frame_textures, ctx->frame_texture_stride, ctx->frame_textures_strided) ||
        !restride(ctx->frame_layers, ctx->frame_layer_stride, ctx->frame_layers_strided))
    {
        // Undo the offers, so that nothing is half delivered.
        for (const std::uint64_t id : ctx->frame_offered)
        {
            gpu_resource* r = ctx->resources.find(id);
            r->current = gpu_resource::state::pending;
            ctx->gpu_used -= r->bytes;
        }
        ctx->frame_offered.clear();
        ctx->frame_index_lists.clear();
        ctx->frame_meshes.clear();
        ctx->frame_textures.clear();
        ctx->frame_layers.clear();
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    ctx->frame_open = true;
    *out_frame = &ctx->frame_handle;
    ctx->last_created = static_cast<std::uint32_t>(ctx->frame_offered.size());
    ctx->last_destroyed = static_cast<std::uint32_t>(ctx->frame_destroys.size());
    ctx->last_bytes_created = 0;
    for (const std::uint64_t id : ctx->frame_offered)
    {
        ctx->last_bytes_created += ctx->resources.find(id)->bytes;
    }
    ctx->frame_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_destroys_get(const vgl_frame* frame,
                                                     const uint64_t** out_resources,
                                                     uint32_t* out_count)
{
    if (frame == nullptr || out_resources == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *frame->context;
    *out_resources = ctx.frame_destroys.data();
    *out_count = static_cast<std::uint32_t>(ctx.frame_destroys.size());
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_index_lists_get(const vgl_frame* frame,
                                                        const vgl_index_list_data** out_index_lists,
                                                        uint32_t* out_count)
{
    if (frame == nullptr || out_index_lists == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *frame->context;
    *out_index_lists = list_at_stride(
        ctx.frame_index_lists, ctx.frame_index_list_stride, ctx.frame_index_lists_strided);
    *out_count = static_cast<std::uint32_t>(ctx.frame_index_lists.size());
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_meshes_get(const vgl_frame* frame,
                                                   const vgl_mesh_data** out_meshes,
                                                   uint32_t* out_count)
{
    if (frame == nullptr || out_meshes == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *frame->context;
    *out_meshes = list_at_stride(ctx.frame_meshes, ctx.frame_mesh_stride, ctx.frame_meshes_strided);
    *out_count = static_cast<std::uint32_t>(ctx.frame_meshes.size());
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_textures_get(const vgl_frame* frame,
                                                     const vgl_texture_data** out_textures,
                                                     uint32_t* out_count)
{
    if (frame == nullptr || out_textures == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *frame->context;
    *out_textures =
        list_at_stride(ctx.frame_textures, ctx.frame_texture_stride, ctx.frame_textures_strided);
    *out_count = static_cast<std::uint32_t>(ctx.frame_textures.size());
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_draw_list_get(const vgl_frame* frame,
                                                      const vgl_draw_item** out_items,
                                                      uint32_t* out_count)
{
    if (frame == nullptr || out_items == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_context& ctx = *frame->context;
    // Handles set since the frame began, for resources created in it.
    for (std::uint32_t k = 0; k < ctx.frame_draw_count; ++k)
    {
        std::byte* item = ctx.frame_draw.data() + std::size_t{k} * ctx.frame_draw_stride;
        for (const auto& [id_at, handle_at] :
             {std::pair{offsetof(vgl_draw_item, mesh), offsetof(vgl_draw_item, mesh_handle)},
              std::pair{offsetof(vgl_draw_item, index_list),
                        offsetof(vgl_draw_item, index_list_handle)}})
        {
            std::uint64_t id = 0;
            std::memcpy(&id, item + id_at, sizeof(id));
            const gpu_resource* r = ctx.resources.find(id);
            const std::uint64_t handle = r != nullptr ? r->handle : 0;
            std::memcpy(item + handle_at, &handle, sizeof(handle));
        }
    }
    *out_items = reinterpret_cast<const vgl_draw_item*>(ctx.frame_draw.data());
    *out_count = ctx.frame_draw_count;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_draw_layers_get(const vgl_frame* frame,
                                                        const vgl_draw_layer** out_layers,
                                                        uint32_t* out_count)
{
    if (frame == nullptr || out_layers == nullptr || out_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *frame->context;
    *out_layers =
        list_at_stride(ctx.frame_layers, ctx.frame_layer_stride, ctx.frame_layers_strided);
    *out_count = static_cast<std::uint32_t>(ctx.frame_layers.size());
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_resource_handle_set(vgl_frame* frame,
                                                            uint64_t resource,
                                                            uint64_t handle)
{
    if (frame == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    gpu_resource* r = frame->context->resources.find(resource);
    if (r == nullptr || r->current == gpu_resource::state::pending)
    {
        return VGL_RESULT_NOT_FOUND;
    }
    r->handle = handle;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_resource_failed(vgl_frame* frame,
                                                        uint64_t resource,
                                                        vgl_result reason)
{
    if (frame == nullptr || reason >= 0)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_context& ctx = *frame->context;
    gpu_resource* r = ctx.resources.find(resource);
    if (r == nullptr || r->current != gpu_resource::state::offered)
    {
        return VGL_RESULT_NOT_FOUND;
    }
    if (!r->failed)
    {
        r->failed = true;
        const char* name = nullptr;
        static_cast<void>(vgl_result_name_get(reason, &name));
        ctx.log.log(VGL_LOG_LEVEL_WARNING,
                    "vgl_frame_resource_failed: resource %llu could not be created (%s); it is "
                    "offered again in a later frame",
                    static_cast<unsigned long long>(resource),
                    name);
    }
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_frame_end(vgl_frame* frame)
{
    if (frame == nullptr || !frame->context->frame_open)
    {
        return VGL_RESULT_SUCCESS;
    }
    vgl_context& ctx = *frame->context;
    for (const std::uint64_t id : ctx.frame_offered)
    {
        gpu_resource* r = ctx.resources.find(id);
        if (r->failed)
        {
            r->failed = false;
            r->current = gpu_resource::state::pending;
            ctx.gpu_used -= r->bytes;
            if (r->garbage)
            {
                retire_resource(ctx, id); // Retired during the frame; never created.
            }
            continue;
        }
        r->current = gpu_resource::state::resident;
        ctx.cpu_used -= r->cpu_bytes();
        r->free_cpu_copy();
    }
    // What was drawn now is what stays drawn while its replacements arrive.
    ctx.drawn.clear();
    for (const drawn_tile& d : ctx.frame_tiles)
    {
        static_cast<void>(ctx.drawn.push_back(d));
    }
    ctx.frame_open = false;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_resources_reset(vgl_context* context)
{
    if (context == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_context& ctx = *context;
    if (ctx.frame_open)
    {
        return VGL_RESULT_INVALID_OPERATION;
    }
    ctx.resources.for_each(
        [&](std::uint64_t, const gpu_resource& r) { ctx.cpu_used -= r.cpu_bytes(); });
    ctx.resources.clear();
    ctx.index_lists.clear();
    ctx.gpu_used = 0;
    ctx.drawn.clear();
    // Tiles rebuild their meshes when next selected.
    vector<std::uint64_t> idle(ctx.memory);
    ctx.meshes.for_each([&](std::uint64_t key, mesh_state& m) {
        m.current = 0;
        m.next = 0;
        if (!m.building)
        {
            static_cast<void>(idle.push_back(key));
        }
    });
    for (const std::uint64_t key : idle)
    {
        static_cast<void>(ctx.meshes.erase(key));
    }
    // Textures are decoded again when next wanted; a decode under way installs its result.
    for (vgl_pack* p : ctx.packs)
    {
        idle.clear();
        p->textures.for_each([&](std::uint64_t key, texture_state& s) {
            s.current = 0;
            if (!s.decoding)
            {
                static_cast<void>(idle.push_back(key));
            }
        });
        for (const std::uint64_t key : idle)
        {
            static_cast<void>(p->textures.erase(key));
        }
    }
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_texture_formats_set(vgl_context* context,
                                                              const vgl_texture_format* formats,
                                                              uint32_t format_count)
{
    if (context == nullptr || (formats == nullptr && format_count != 0))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    for (std::uint32_t i = 0; i < format_count; ++i)
    {
        if (formats[i] < VGL_TEXTURE_FORMAT_RGBA8 || formats[i] > VGL_TEXTURE_FORMAT_ASTC_4X4)
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
    }
    // The preference order, without repeats; RGBA8 is always possible, so it ends the list.
    vgl_context& ctx = *context;
    const auto listed = [&](vgl_texture_format f) {
        for (std::uint32_t k = 0; k < ctx.texture_format_count; ++k)
        {
            if (ctx.texture_formats[k] == f)
            {
                return true;
            }
        }
        return false;
    };
    ctx.texture_format_count = 0;
    for (std::uint32_t i = 0; i < format_count; ++i)
    {
        if (!listed(formats[i]) && ctx.texture_format_count + 1 < ctx.texture_formats.size())
        {
            ctx.texture_formats[ctx.texture_format_count++] = formats[i];
        }
    }
    if (!listed(VGL_TEXTURE_FORMAT_RGBA8))
    {
        ctx.texture_formats[ctx.texture_format_count++] = VGL_TEXTURE_FORMAT_RGBA8;
    }
    return VGL_RESULT_SUCCESS;
}
