// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the core side of the render interface (docs/render-interface.md), with a consumer
// that keeps resources in memory instead of on a GPU and checks the interface's guarantees in
// every frame.

#include "context/context_impl.hpp"
#include "test_packs.hpp"
#include "test_views.hpp"
#include "vgl_test.hpp"

#include <vgl/render.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

using namespace vgl::internal;
using namespace vgl::test;

// A consumer that "creates" resources by remembering them.
struct fake_gpu
{
    struct resource
    {
        bool index_list = false;
        std::uint64_t bytes = 0;
        std::uint32_t index_count = 0;
        std::uint32_t index_size = 0;
        std::uint32_t vertex_count = 0;
        bool texture = false;
    };

    // A texture's largest level, kept to check what layers sample.
    struct image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mip_count = 0;
        vgl_texture_format format = VGL_TEXTURE_FORMAT_RGBA8;
        std::vector<std::uint8_t> pixels;
    };

    std::map<std::uint64_t, resource> resident;
    std::map<std::uint64_t, image> images; // Of the resident textures.
    std::set<std::uint64_t> ever;          // Every id ever created, to check that none is reused.
    std::set<std::uint64_t> fail;          // Ids to report as failed, once.
    std::set<std::uint64_t> failed_now;    // Ids reported as failed in this frame.
    bool set_handles = false;

    // Per frame.
    std::vector<vgl_draw_item> items;
    std::vector<vgl_draw_layer> layers;
    std::size_t meshes_created = 0;
    std::size_t placeholders_created = 0; // Of the meshes, as the core marks them.
    vgl_context* context = nullptr;       // Of the frame being applied, if frame() began it.
    std::size_t textures_created = 0;
    std::size_t index_lists_created = 0;
    std::size_t destroyed = 0;

    // The handle the consumer sets for a resource.
    static std::uint64_t handle_of(std::uint64_t id) { return id * 7 + 1; }

    // Begins, applies and ends a frame, checking the interface's rules.
    void frame(vgl_context* ctx, const vgl_frame_begin_info* info = nullptr)
    {
        vgl_frame* f = nullptr;
        context = ctx;
        REQUIRE(vgl_context_frame_begin(ctx, info, &f) == VGL_RESULT_SUCCESS);
        apply(f, info);
        CHECK_EQ(vgl_frame_end(f), VGL_RESULT_SUCCESS);
    }

    void apply(vgl_frame* f, const vgl_frame_begin_info* info)
    {
        items.clear();
        layers.clear();
        meshes_created = 0;
        placeholders_created = 0;
        failed_now.clear();
        textures_created = 0;
        index_lists_created = 0;
        destroyed = 0;

        const std::uint64_t* destroys = nullptr;
        std::uint32_t count = 0;
        REQUIRE(vgl_frame_destroys_get(f, &destroys, &count) == VGL_RESULT_SUCCESS);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            CHECK(resident.erase(destroys[k]) == 1);
            images.erase(destroys[k]);
            ++destroyed;
        }

        const vgl_index_list_data* lists = nullptr;
        REQUIRE(vgl_frame_index_lists_get(f, &lists, &count) == VGL_RESULT_SUCCESS);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            const vgl_index_list_data& d = lists[k];
            CHECK(ever.insert(d.resource).second);
            CHECK(d.index_size == 2 || d.index_size == 4);
            CHECK(d.indices != nullptr);
            if (fail.erase(d.resource) != 0)
            {
                CHECK_EQ(vgl_frame_resource_failed(f, d.resource, VGL_RESULT_OUT_OF_MEMORY),
                         VGL_RESULT_SUCCESS);
                ever.erase(d.resource);
                failed_now.insert(d.resource);
                continue;
            }
            resident[d.resource] = {
                true, std::uint64_t{d.index_count} * d.index_size, d.index_count, d.index_size, 0};
            ++index_lists_created;
            if (set_handles)
            {
                CHECK_EQ(vgl_frame_resource_handle_set(f, d.resource, handle_of(d.resource)),
                         VGL_RESULT_SUCCESS);
            }
        }

        const vgl_mesh_data* meshes = nullptr;
        REQUIRE(vgl_frame_meshes_get(f, &meshes, &count) == VGL_RESULT_SUCCESS);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            const vgl_mesh_data& d = meshes[k];
            CHECK(ever.insert(d.resource).second);
            CHECK_EQ(d.vertex_size, static_cast<std::uint32_t>(VGL_TERRAIN_VERTEX_SIZE));
            CHECK(d.vertices != nullptr);
            CHECK_EQ(d.sample_size, static_cast<std::uint32_t>(VGL_TERRAIN_SAMPLE_SIZE));
            CHECK(d.samples != nullptr && d.sample_grid >= 9);
            // Its index list exists by now, unless the consumer failed to create it, in which
            // case it cannot create the mesh either.
            const auto list = resident.find(d.index_list);
            const bool no_list = list == resident.end();
            CHECK(no_list || list->second.index_list);
            if (fail.erase(d.resource) != 0 || no_list)
            {
                CHECK_EQ(vgl_frame_resource_failed(f, d.resource, VGL_RESULT_OUT_OF_MEMORY),
                         VGL_RESULT_SUCCESS);
                ever.erase(d.resource);
                failed_now.insert(d.resource);
                continue;
            }
            // Touch every byte, so that a sanitizer sees a bad pointer.
            std::uint64_t sum = 0;
            const auto* bytes = static_cast<const unsigned char*>(d.vertices);
            for (std::size_t b = 0; b < std::size_t{d.vertex_count} * d.vertex_size; ++b)
            {
                sum += bytes[b];
            }
            CHECK(sum > 0);
            const std::size_t sample_bytes =
                std::size_t{d.sample_grid} * d.sample_grid * d.sample_size;
            const auto* samples = static_cast<const unsigned char*>(d.samples);
            sum = 0;
            for (std::size_t b = 0; b < sample_bytes; ++b)
            {
                sum += samples[b];
            }
            CHECK(sum > 0);
            resident[d.resource] = {false,
                                    std::uint64_t{d.vertex_count} * d.vertex_size + sample_bytes,
                                    0,
                                    0,
                                    d.vertex_count};
            ++meshes_created;
            const gpu_resource* r =
                context != nullptr ? context->resources.find(d.resource) : nullptr;
            placeholders_created += r != nullptr && r->placeholder ? 1 : 0;
            if (set_handles)
            {
                CHECK_EQ(vgl_frame_resource_handle_set(f, d.resource, handle_of(d.resource)),
                         VGL_RESULT_SUCCESS);
            }
        }

        const vgl_texture_data* textures = nullptr;
        REQUIRE(vgl_frame_textures_get(f, &textures, &count) == VGL_RESULT_SUCCESS);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            const vgl_texture_data& d = textures[k];
            CHECK(ever.insert(d.resource).second);
            if (fail.erase(d.resource) != 0)
            {
                CHECK_EQ(vgl_frame_resource_failed(f, d.resource, VGL_RESULT_OUT_OF_MEMORY),
                         VGL_RESULT_SUCCESS);
                ever.erase(d.resource);
                failed_now.insert(d.resource);
                continue;
            }
            // A full chain of mip levels, each half the size of the one before.
            REQUIRE(d.mip_count == mip_level_count(d.width, d.height));
            std::uint64_t bytes = 0;
            std::uint32_t w = d.width;
            std::uint32_t h = d.height;
            for (std::uint32_t level = 0; level < d.mip_count; ++level)
            {
                REQUIRE(d.mip_data[level] != nullptr);
                CHECK_EQ(d.mip_sizes[level], std::size_t{w} * h * 4);
                bytes += d.mip_sizes[level];
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
            const auto* level0 = static_cast<const std::uint8_t*>(d.mip_data[0]);
            images[d.resource] = {d.width,
                                  d.height,
                                  d.mip_count,
                                  d.format,
                                  std::vector<std::uint8_t>(level0, level0 + d.mip_sizes[0])};
            resident[d.resource] = {false, bytes, 0, 0, 0, true};
            ++textures_created;
            if (set_handles)
            {
                CHECK_EQ(vgl_frame_resource_handle_set(f, d.resource, handle_of(d.resource)),
                         VGL_RESULT_SUCCESS);
            }
        }

        const vgl_draw_layer* layer_list = nullptr;
        REQUIRE(vgl_frame_draw_layers_get(f, &layer_list, &count) == VGL_RESULT_SUCCESS);
        layers.assign(layer_list, layer_list + count);

        const vgl_draw_item* draw = nullptr;
        REQUIRE(vgl_frame_draw_list_get(f, &draw, &count) == VGL_RESULT_SUCCESS);
        const std::size_t stride = info != nullptr && info->draw_item_size != 0
                                       ? info->draw_item_size
                                       : sizeof(vgl_draw_item);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            vgl_draw_item item{};
            std::memcpy(&item,
                        reinterpret_cast<const std::byte*>(draw) + k * stride,
                        std::min(stride, sizeof(vgl_draw_item)));
            // Drawn only once created, in this frame or an earlier one; an item of a resource
            // the consumer could not create in this frame is skipped.
            if (failed_now.contains(item.mesh) || failed_now.contains(item.index_list))
            {
                continue;
            }
            items.push_back(item);
            const auto mesh = resident.find(item.mesh);
            const auto list = resident.find(item.index_list);
            REQUIRE(mesh != resident.end());
            REQUIRE(list != resident.end());
            CHECK_EQ(item.index_count, list->second.index_count);
            CHECK_EQ(item.index_size, list->second.index_size);
            // Layers name resident textures.
            REQUIRE(std::size_t{item.first_layer} + item.layer_count <= layers.size());
            for (std::uint32_t l = 0; l < item.layer_count; ++l)
            {
                const vgl_draw_layer& layer = layers[item.first_layer + l];
                const auto texture = resident.find(layer.texture);
                REQUIRE(texture != resident.end());
                CHECK(texture->second.texture);
                CHECK_EQ(layer.texture_handle, set_handles ? handle_of(layer.texture) : 0u);
                CHECK_EQ(layer.opacity, 1.0f);
            }
        }
    }

    [[nodiscard]] std::uint64_t bytes() const
    {
        std::uint64_t total = 0;
        for (const auto& [id, r] : resident)
        {
            total += r.bytes;
        }
        return total;
    }
};

tile_id id_of(const vgl_tile_id& t)
{
    return {t.level, t.x, t.y};
}

// True if no drawn tile overlaps another.
bool disjoint(const std::vector<vgl_draw_item>& items)
{
    for (std::size_t a = 0; a < items.size(); ++a)
    {
        for (std::size_t b = 0; b < items.size(); ++b)
        {
            const tile_id ta = id_of(items[a].tile);
            const tile_id tb = id_of(items[b].tile);
            if (a != b && (ta == tb || (ta.level < tb.level && tile_ancestor(tb, ta.level) == ta)))
            {
                return false;
            }
        }
    }
    return true;
}

// Fraction of a tile covered by drawn tiles: 1 when drawn itself or by an ancestor.
double coverage(const tile_id& t, const std::vector<vgl_draw_item>& items)
{
    double covered = 0.0;
    for (const vgl_draw_item& item : items)
    {
        const tile_id d = id_of(item.tile);
        if (d.level <= t.level && tile_ancestor(t, d.level) == d)
        {
            return 1.0;
        }
        if (d.level > t.level && tile_ancestor(d, t.level) == t)
        {
            covered += std::ldexp(1.0, -2 * static_cast<int>(d.level - t.level));
        }
    }
    return covered;
}

// True if a drawn tile contains a position in degrees.
bool drawn_at(const std::vector<vgl_draw_item>& items, const position& at)
{
    return std::any_of(items.begin(), items.end(), [&](const vgl_draw_item& item) {
        const degree_region b = tile_bounds(id_of(item.tile));
        return at.lat >= b.south && at.lat <= b.north && at.lon >= b.west && at.lon <= b.east;
    });
}

// Updates and draws until nothing changes: nothing loads, builds or uploads.
void settle(vgl_context* ctx,
            memory_io& io,
            fake_gpu& gpu,
            const vgl_frame_begin_info* info = nullptr)
{
    std::size_t quiet = 0;
    for (int i = 0; i < 400 && quiet < 3; ++i)
    {
        const std::size_t reads = io.uris.size();
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        gpu.frame(ctx, info);
        CHECK(disjoint(gpu.items));
        const bool idle = io.uris.size() == reads && gpu.meshes_created == 0 &&
                          gpu.textures_created == 0 && gpu.destroyed == 0;
        quiet = idle ? quiet + 1 : 0;
    }
}

struct fixture
{
    test_pack pack = make_pack();
    test_pack imagery_pack = make_imagery_pack();
    memory_io io;
    vgl_context* ctx = nullptr;
    vgl_pack* p = nullptr;
    vgl_pack* imagery = nullptr;

    explicit fixture(std::uint64_t gpu_budget = 0,
                     std::uint64_t cpu_budget = 0,
                     bool with_imagery = false,
                     manual_tasks* tasks = nullptr)
    {
        io.packs["mem://a"] = &pack;
        io.packs["mem://imagery"] = &imagery_pack;
        vgl_context_create_info info{};
        info.struct_size = sizeof(info);
        info.io = &io.table;
        info.gpu_budget = gpu_budget;
        info.cpu_budget = cpu_budget;
        info.tasks = tasks != nullptr ? &tasks->table : nullptr;
        if (vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS)
        {
            vgl_pack_create_info create{};
            create.struct_size = sizeof(create);
            create.uri = "mem://a";
            static_cast<void>(vgl_pack_create(ctx, &create, &p));
            if (with_imagery)
            {
                create.uri = "mem://imagery";
                static_cast<void>(vgl_pack_create(ctx, &create, &imagery));
            }
        }
    }

    fixture(const fixture&) = delete;
    fixture& operator=(const fixture&) = delete;

    ~fixture()
    {
        static_cast<void>(vgl_pack_destroy(imagery));
        static_cast<void>(vgl_pack_destroy(p));
        static_cast<void>(vgl_context_destroy(ctx));
    }

    void look(double lat, double lon, double altitude) const
    {
        const vgl_view v = camera_over(lat, lon, altitude);
        REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    }
};

} // namespace

TEST_CASE(frames_follow_the_calling_rules)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    vgl_frame* frame = nullptr;
    CHECK_EQ(vgl_context_frame_begin(nullptr, nullptr, &frame), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_frame_begin(f.ctx, nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    vgl_frame_begin_info bad{};
    bad.struct_size = 4;
    CHECK_EQ(vgl_context_frame_begin(f.ctx, &bad, &frame), VGL_RESULT_INVALID_ARGUMENT);

    // Without a view, nothing is drawn.
    REQUIRE(vgl_context_frame_begin(f.ctx, nullptr, &frame) == VGL_RESULT_SUCCESS);
    const vgl_draw_item* items = nullptr;
    std::uint32_t count = 1;
    CHECK_EQ(vgl_frame_draw_list_get(frame, &items, &count), VGL_RESULT_SUCCESS);
    CHECK_EQ(count, 0u);
    CHECK_EQ(vgl_frame_draw_list_get(frame, nullptr, &count), VGL_RESULT_INVALID_ARGUMENT);

    // One frame at a time, and no update inside one.
    vgl_frame* second = nullptr;
    CHECK_EQ(vgl_context_frame_begin(f.ctx, nullptr, &second), VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_context_update(f.ctx, nullptr), VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_context_resources_reset(f.ctx), VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_frame_resource_handle_set(frame, 12345, 1), VGL_RESULT_NOT_FOUND);
    CHECK_EQ(vgl_frame_resource_failed(frame, 12345, VGL_RESULT_OUT_OF_MEMORY),
             VGL_RESULT_NOT_FOUND);
    CHECK_EQ(vgl_frame_resource_failed(frame, 12345, VGL_RESULT_SUCCESS),
             VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_frame_end(frame), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_frame_end(nullptr), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_update(f.ctx, nullptr), VGL_RESULT_SUCCESS);

    const vgl_texture_format formats[] = {VGL_TEXTURE_FORMAT_BC7, VGL_TEXTURE_FORMAT_RGBA8};
    CHECK_EQ(vgl_context_texture_formats_set(f.ctx, formats, 2), VGL_RESULT_SUCCESS);
    const auto unknown = static_cast<vgl_texture_format>(99);
    CHECK_EQ(vgl_context_texture_formats_set(f.ctx, &unknown, 1), VGL_RESULT_INVALID_ARGUMENT);
}

TEST_CASE(selected_tiles_are_created_and_drawn)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    gpu.set_handles = true;
    const position centre = inside({3, 6, 2}, 0.5, 0.5);
    f.look(centre.lat, centre.lon, 50000.0);
    settle(f.ctx, f.io, gpu);

    // The draw list is the selection.
    REQUIRE(gpu.items.size() == f.ctx->selected.size());
    for (const vgl_draw_item& item : gpu.items)
    {
        const bool selected =
            std::any_of(f.ctx->selected.begin(),
                        f.ctx->selected.end(),
                        [&](const selected_tile& s) { return s.id == id_of(item.tile); });
        CHECK(selected);
        CHECK_EQ(item.mesh_handle, fake_gpu::handle_of(item.mesh));
        CHECK_EQ(item.index_list_handle, fake_gpu::handle_of(item.index_list));
        CHECK_EQ(item.layer_count, 0u);
    }

    // Uploaded meshes no longer hold CPU memory: what remains is decoded tiles.
    std::uint64_t tiles = 0;
    for (vgl_pack* p : f.ctx->packs)
    {
        p->tiles.for_each([&](std::uint64_t, const tile_state& t) { tiles += t.cpu_bytes(); });
    }
    CHECK_EQ(f.ctx->cpu_used, tiles);
    CHECK_EQ(f.ctx->gpu_used, gpu.bytes());

    // The model-view matrix is view x translate(centre), composed in double precision.
    const dmat4 view = f.ctx->view.view_matrix;
    for (const vgl_draw_item& item : gpu.items)
    {
        const dvec3 c = dvec3::from_c(item.centre);
        const dvec3 t = transform_point(view, c);
        CHECK_NEAR(item.model_view[12], t.x, std::fabs(t.x) * 1e-6 + 1e-3);
        CHECK_NEAR(item.model_view[13], t.y, std::fabs(t.y) * 1e-6 + 1e-3);
        CHECK_NEAR(item.model_view[14], t.z, std::fabs(t.z) * 1e-6 + 1e-3);
        CHECK_NEAR(item.model_view[0], view.m[0], 1e-6);
        CHECK_EQ(item.model_view[15], 1.0f);
    }
}

TEST_CASE(transitions_never_leave_holes_or_overlaps)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    // Far away, then close, then far again. Tiles never overlap, the ground under the camera,
    // once drawn, is drawn in every frame, and once loading is done, every selected tile is
    // drawn as itself.
    const position near_point = inside({3, 7, 3}, 0.5, 0.5);
    f.look(0.0, -30.0, 2e7);
    settle(f.ctx, f.io, gpu);
    REQUIRE(drawn_at(gpu.items, near_point));
    for (const double altitude : {2e6, 2e5, 50000.0, 2e6, 2e7})
    {
        f.look(near_point.lat, near_point.lon, altitude);
        for (int i = 0; i < 60; ++i)
        {
            REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
            gpu.frame(f.ctx);
            CHECK(disjoint(gpu.items));
            CHECK(drawn_at(gpu.items, near_point));
        }
        settle(f.ctx, f.io, gpu);
        CHECK_EQ(gpu.items.size(), f.ctx->selected.size());
        for (const selected_tile& s : f.ctx->selected)
        {
            CHECK_NEAR(coverage(s.id, gpu.items), 1.0, 1e-12);
        }
    }
}

TEST_CASE(new_regions_are_covered_from_their_first_frame)
{
    manual_tasks tasks;
    fixture f(0, 0, false, &tasks);
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    // To a region never drawn, with mesh tasks that run only every tenth frame and one real
    // mesh uploaded per frame: from the frame of the jump on, placeholders, drawn in the frame
    // that creates them, cover every selected tile, without overlaps, until the tiles' own
    // meshes arrive.
    f.look(0.0, -30.0, 2e7);
    for (int i = 0; i < 100; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        tasks.run_all();
        gpu.frame(f.ctx);
    }
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.upload_budget = 1;
    // The far side of the globe, hidden from the first view.
    f.look(0.0, 150.0, 2e5);
    bool placeholders = false;
    for (int i = 0; i < 120; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        if (i % 10 == 9)
        {
            tasks.run_all();
        }
        gpu.frame(f.ctx, &info);
        CHECK(disjoint(gpu.items));
        for (const selected_tile& s : f.ctx->selected)
        {
            CHECK_NEAR(coverage(s.id, gpu.items), 1.0, 1e-12);
        }
        for (const vgl_draw_item& item : gpu.items)
        {
            placeholders = placeholders || f.ctx->resources.find(item.mesh)->placeholder;
        }
    }
    CHECK(placeholders);
}

TEST_CASE(uploads_respect_the_upload_budget)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.upload_budget = 1; // One mesh per frame, besides placeholders, which are outside it.
    std::size_t total = 0;
    for (int i = 0; i < 200; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        gpu.frame(f.ctx, &info);
        CHECK(gpu.meshes_created - gpu.placeholders_created <= 1);
        total += gpu.meshes_created - gpu.placeholders_created;
    }
    CHECK(total >= f.ctx->selected.size());
    CHECK_EQ(gpu.items.size(), f.ctx->selected.size());
}

TEST_CASE(the_gpu_budget_destroys_what_is_not_drawn)
{
    fixture f(1); // Nothing stays beyond what is drawn.
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    settle(f.ctx, f.io, gpu);
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    // Only what the frame draws, and the index lists, stays on the "GPU".
    std::uint64_t drawn = 0;
    std::set<std::uint64_t> lists;
    for (const vgl_draw_item& item : gpu.items)
    {
        drawn += gpu.resident.at(item.mesh).bytes;
        lists.insert(item.index_list);
    }
    std::uint64_t list_bytes = 0;
    for (const auto& [id, r] : gpu.resident)
    {
        if (r.index_list)
        {
            list_bytes += r.bytes;
        }
    }
    CHECK_EQ(gpu.bytes(), drawn + list_bytes);
    CHECK_EQ(f.ctx->gpu_used, gpu.bytes());
}

TEST_CASE(failed_resources_are_offered_again)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    // Fail every resource the first time it is offered.
    for (std::uint64_t id = 1; id < 200; ++id)
    {
        gpu.fail.insert(id);
    }
    settle(f.ctx, f.io, gpu);
    REQUIRE(gpu.items.size() == f.ctx->selected.size());
    CHECK(!gpu.items.empty());
}

TEST_CASE(a_reset_creates_everything_again)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    const std::size_t drawn = gpu.items.size();
    REQUIRE(drawn > 0);

    // The device is lost.
    gpu.resident.clear();
    CHECK_EQ(vgl_context_resources_reset(f.ctx), VGL_RESULT_SUCCESS);
    CHECK_EQ(f.ctx->gpu_used, 0u);
    settle(f.ctx, f.io, gpu);
    CHECK_EQ(gpu.items.size(), drawn);
}

TEST_CASE(older_consumers_get_shorter_draw_items)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.items.empty());
    const vgl_draw_item full = gpu.items[0];

    // A consumer whose draw item ends before min_height.
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.draw_item_size = static_cast<std::uint32_t>(offsetof(vgl_draw_item, min_height));
    gpu.frame(f.ctx, &info);
    REQUIRE(!gpu.items.empty());
    CHECK_EQ(gpu.items[0].mesh, full.mesh);
    CHECK_EQ(gpu.items[0].min_height, 0.0); // Not written: beyond the consumer's size.

    info.draw_item_size = 8; // Too small for any version.
    vgl_frame* frame = nullptr;
    CHECK_EQ(vgl_context_frame_begin(f.ctx, &info, &frame), VGL_RESULT_INVALID_ARGUMENT);
}

TEST_CASE(a_frame_can_draw_from_another_camera)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.items.empty());

    // The host draws more often than it updates, from a slightly moved camera.
    const vgl_view moved = camera_over(p.lat + 0.01, p.lon, 50000.0);
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.view_matrix = &moved.view_matrix;
    gpu.frame(f.ctx, &info);
    const dmat4 view = dmat4::from_c(moved.view_matrix);
    const dvec3 t = transform_point(view, dvec3::from_c(gpu.items[0].centre));
    CHECK_NEAR(gpu.items[0].model_view[12], t.x, std::fabs(t.x) * 1e-6 + 1e-3);
    CHECK_NEAR(gpu.items[0].model_view[14], t.z, std::fabs(t.z) * 1e-6 + 1e-3);
}

TEST_CASE(destroying_a_pack_inside_a_frame_is_safe)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        gpu.frame(f.ctx);
    }
    // Leave meshes waiting, then destroy the pack while the frame offers them.
    f.look(30.0, -30.0, 3e6);
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
    }
    vgl_frame* frame = nullptr;
    REQUIRE(vgl_context_frame_begin(f.ctx, nullptr, &frame) == VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(f.p), VGL_RESULT_SUCCESS);
    f.p = nullptr;
    gpu.apply(frame, nullptr); // Reads every offered byte.
    CHECK_EQ(vgl_frame_end(frame), VGL_RESULT_SUCCESS);
    // The pack's resources are destroyed in the next frames; the ellipsoid takes over.
    settle(f.ctx, f.io, gpu);
    for (const vgl_draw_item& item : gpu.items)
    {
        CHECK_EQ(item.min_height, 0.0);
    }
}

TEST_CASE(frames_survive_allocation_failures)
{
    const test_pack pack = make_pack();
    for (long fail_at = 0;; ++fail_at)
    {
        test_allocator memory;
        memory.fail_at = fail_at;
        memory_io io;
        io.packs["mem://a"] = &pack;
        bool drawn = false;
        {
            vgl_context_create_info info{};
            info.struct_size = sizeof(info);
            info.allocator = &memory.table;
            info.io = &io.table;
            vgl_context* ctx = nullptr;
            if (vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS)
            {
                vgl_pack_create_info create{};
                create.struct_size = sizeof(create);
                create.uri = "mem://a";
                vgl_pack* p = nullptr;
                static_cast<void>(vgl_pack_create(ctx, &create, &p));
                const vgl_view v = camera_over(30.0, -30.0, 3e6);
                static_cast<void>(vgl_context_view_set(ctx, &v));
                fake_gpu gpu;
                for (int i = 0; i < 60; ++i)
                {
                    static_cast<void>(vgl_context_update(ctx, nullptr));
                    vgl_frame* frame = nullptr;
                    if (vgl_context_frame_begin(ctx, nullptr, &frame) == VGL_RESULT_SUCCESS)
                    {
                        gpu.apply(frame, nullptr);
                        static_cast<void>(vgl_frame_end(frame));
                        CHECK(disjoint(gpu.items));
                    }
                }
                drawn = !gpu.items.empty() && gpu.items.size() == ctx->selected.size();
                static_cast<void>(vgl_pack_destroy(p));
                static_cast<void>(vgl_context_destroy(ctx));
            }
        }
        CHECK_EQ(memory.live.load(), 0);
        if (!memory.failed)
        {
            CHECK(drawn);
            CHECK(fail_at > 50);
            break;
        }
    }
}

TEST_CASE(statistics_describe_the_context)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);

    vgl_context_statistics s{};
    s.struct_size = sizeof(s);
    REQUIRE(vgl_context_statistics_get(f.ctx, &s) == VGL_RESULT_SUCCESS);
    CHECK_EQ(s.struct_size, static_cast<std::uint32_t>(sizeof(s)));
    CHECK_EQ(s.cpu_used, f.ctx->cpu_used);
    CHECK_EQ(s.gpu_used, gpu.bytes());
    CHECK_EQ(s.gpu_budget, 512ull * 1024 * 1024);
    CHECK_EQ(s.packs_loading, 0u);
    CHECK_EQ(s.packs_failed, 0u);
    CHECK_EQ(s.reads_outstanding, 0u);
    CHECK_EQ(s.tiles_decoding, 0u);
    CHECK(s.tiles_decoded > 0);
    CHECK_EQ(s.meshes_building, 0u);
    CHECK_EQ(s.meshes_waiting, 0u);
    CHECK_EQ(s.resources_resident, static_cast<std::uint32_t>(gpu.resident.size()));
    CHECK_EQ(s.tiles_selected, static_cast<std::uint32_t>(f.ctx->selected.size()));
    CHECK_EQ(s.tiles_drawn, static_cast<std::uint32_t>(gpu.items.size()));
    CHECK_EQ(s.resources_created, 0u); // Settled: the last frame created nothing.
    CHECK_EQ(s.reads_total, static_cast<std::uint64_t>(f.io.uris.size()));
    CHECK(s.bytes_read_total > 0);
    CHECK(s.tiles_loaded_total >= s.tiles_decoded);
    CHECK(s.tile_load_time_mean >= 0.0);
    CHECK(s.update_time > 0.0);
    CHECK(s.frame_time > 0.0);

    // An older caller's structure gets only the fields that fit.
    vgl_context_statistics old{};
    old.struct_size = 16;
    old.cpu_budget = 12345;
    REQUIRE(vgl_context_statistics_get(f.ctx, &old) == VGL_RESULT_SUCCESS);
    CHECK_EQ(old.cpu_used, s.cpu_used);
    CHECK_EQ(old.cpu_budget, 12345u);

    old.struct_size = 4;
    CHECK_EQ(vgl_context_statistics_get(f.ctx, &old), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_statistics_get(nullptr, &s), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_statistics_get(f.ctx, nullptr), VGL_RESULT_INVALID_ARGUMENT);
}

namespace
{

std::vector<std::string> credits(const vgl_context* ctx)
{
    std::uint32_t count = 0;
    CHECK_EQ(vgl_context_attribution_get(ctx, &count, nullptr), VGL_RESULT_SUCCESS);
    std::vector<const char*> lines(count);
    std::vector<std::string> out;
    if (count == 0)
    {
        return out;
    }
    CHECK_EQ(vgl_context_attribution_get(ctx, &count, lines.data()), VGL_RESULT_SUCCESS);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        out.emplace_back(lines[i]);
    }
    return out;
}

} // namespace

TEST_CASE(attribution_names_the_packs_in_view)
{
    const test_pack low = make_pack(layer_type::elevation, 0, "Low");
    const test_pack high = make_pack(layer_type::elevation, 40, "High");
    const test_pack twin = make_pack(layer_type::elevation, 20, "Low"); // Same lines as low.
    memory_io io;
    io.packs["mem://low"] = &low;
    io.packs["mem://high"] = &high;
    io.packs["mem://twin"] = &twin;
    vgl_context_create_info info{};
    info.struct_size = sizeof(info);
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack_create_info create{};
    create.struct_size = sizeof(create);
    create.uri = "mem://low";
    vgl_pack* p_low = nullptr;
    REQUIRE(vgl_pack_create(ctx, &create, &p_low) == VGL_RESULT_SUCCESS);

    // Nothing in view, nothing to credit.
    CHECK(credits(ctx).empty());
    std::uint32_t count = 7;
    CHECK_EQ(vgl_context_attribution_get(nullptr, &count, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_attribution_get(ctx, nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);

    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    const vgl_view v = camera_over(p.lat, p.lon, 50000.0);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io, gpu);
    std::vector<std::string> lines = credits(ctx);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Low A");
    CHECK(lines[1] == "Low B");

    // Two-call pattern: too small an array is filled and reported incomplete.
    const char* one[1] = {};
    count = 1;
    CHECK_EQ(vgl_context_attribution_get(ctx, &count, one), VGL_RESULT_INCOMPLETE);
    CHECK_EQ(count, 1u);
    CHECK(std::string(one[0]) == "Low A");

    // A higher-priority pack covers the view: only its lines remain.
    create.uri = "mem://high";
    create.priority = 1;
    vgl_pack* p_high = nullptr;
    REQUIRE(vgl_pack_create(ctx, &create, &p_high) == VGL_RESULT_SUCCESS);
    settle(ctx, io, gpu);
    lines = credits(ctx);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "High A");

    // Below it, another pack with the same lines as the first: each line is listed once.
    CHECK_EQ(vgl_pack_destroy(p_high), VGL_RESULT_SUCCESS);
    create.uri = "mem://twin";
    create.priority = 0;
    vgl_pack* p_twin = nullptr;
    REQUIRE(vgl_pack_create(ctx, &create, &p_twin) == VGL_RESULT_SUCCESS);
    // While the view switches from one to the other, both are in view: the last frame drew
    // one, the view selects the other. Their shared lines appear once throughout.
    for (int i = 0; i < 40; ++i)
    {
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        gpu.frame(ctx);
        CHECK_EQ(credits(ctx).size(), 2u);
    }

    // Without a view or frames, nothing is credited.
    REQUIRE(vgl_context_view_set(ctx, nullptr) == VGL_RESULT_SUCCESS);
    gpu.frame(ctx);
    CHECK(credits(ctx).empty());

    CHECK_EQ(vgl_pack_destroy(p_twin), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(p_low), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

// --- Imagery ------------------------------------------------------------------------------

namespace
{

// The imagery tile a test pixel came from, among the ancestors-or-self of t, by its colour.
bool imagery_tile_of(const tile_id& t, std::uint8_t code, tile_id& out)
{
    for (std::uint32_t level = t.level + 1; level-- > 0;)
    {
        const tile_id a = tile_ancestor(t, level);
        if (imagery_pixel(a, 0, 0)[2] == code)
        {
            out = a;
            return true;
        }
    }
    return false;
}

// Checks that a draw item's layer samples the imagery at the right place: at mesh texture
// coordinates (u, v), the texel is the one of the imagery tile over the same position.
void check_layer(const fake_gpu& gpu, const vgl_draw_item& item, const vgl_draw_layer& layer)
{
    const fake_gpu::image& img = gpu.images.at(layer.texture);
    REQUIRE(img.width == imagery_side && img.height == imagery_side);
    const tile_id t = id_of(item.tile);
    for (const auto& [u, v] : {std::pair{0.3, 0.6}, std::pair{0.85, 0.1}, std::pair{0.05, 0.95}})
    {
        const double tu = u * layer.uv_scale_offset[0] + layer.uv_scale_offset[2];
        const double tv = v * layer.uv_scale_offset[1] + layer.uv_scale_offset[3];
        const auto i = static_cast<std::uint32_t>(tu * img.width);
        const auto j = static_cast<std::uint32_t>(tv * img.height);
        REQUIRE(i < img.width && j < img.height);
        const std::uint8_t* px = &img.pixels[(std::size_t{j} * img.width + i) * 4];
        tile_id source;
        REQUIRE(imagery_tile_of(t, px[2], source));
        // The texel is the imagery tile's pixel over the position, inside its border.
        const degree_region b = tile_bounds(t);
        const degree_region s = tile_bounds(source);
        const double lon = b.west + u * (b.east - b.west);
        const double lat = b.north - v * (b.north - b.south);
        const auto ei =
            static_cast<std::uint32_t>((lon - s.west) / (s.east - s.west) * imagery_tile_size) +
            imagery_border;
        const auto ej =
            static_cast<std::uint32_t>((s.north - lat) / (s.north - s.south) * imagery_tile_size) +
            imagery_border;
        CHECK_EQ(px[0], static_cast<std::uint8_t>(ei * 3));
        CHECK_EQ(px[1], static_cast<std::uint8_t>(ej * 3));
    }
}

} // namespace

TEST_CASE(tiles_are_drawn_with_the_imagery_over_them)
{
    fixture f(0, 0, true);
    REQUIRE(f.ctx != nullptr && f.imagery != nullptr);
    fake_gpu gpu;
    gpu.set_handles = true;
    const position centre = inside({3, 6, 2}, 0.5, 0.5);
    f.look(centre.lat, centre.lon, 50000.0);
    // Updates without frames: textures are decoded for the selected tiles, and wait for the
    // consumer to create them.
    vgl_context_statistics s{};
    s.struct_size = sizeof(s);
    std::uint32_t waiting = 0;
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        REQUIRE(vgl_context_statistics_get(f.ctx, &s) == VGL_RESULT_SUCCESS);
        CHECK_EQ(s.textures_decoding, 0u); // Without tasks, decoding happens in the update.
        waiting = std::max(waiting, s.textures_waiting);
    }
    CHECK(waiting > 0);
    settle(f.ctx, f.io, gpu);
    REQUIRE(vgl_context_statistics_get(f.ctx, &s) == VGL_RESULT_SUCCESS);
    CHECK_EQ(s.textures_waiting, 0u);

    REQUIRE(!gpu.items.empty());
    for (const vgl_draw_item& item : gpu.items)
    {
        // One imagery pack, so one layer, from the tile's own imagery once settled.
        REQUIRE(item.layer_count == 1u);
        const vgl_draw_layer& layer = gpu.layers[item.first_layer];
        check_layer(gpu, item, layer);
        const fake_gpu::image& img = gpu.images.at(layer.texture);
        tile_id source;
        REQUIRE(imagery_tile_of(id_of(item.tile), img.pixels[2], source));
        CHECK(source == id_of(item.tile));
        CHECK_EQ(img.mip_count, 7u); // 66, 33, 16, 8, 4, 2, 1.
        CHECK(img.format == VGL_TEXTURE_FORMAT_RGBA8);
    }

    // Uploaded resources hold no CPU memory. What remains is the tiles, imagery encoded, and
    // what the updates without frames prepared for tiles that finer ones replaced before it was
    // drawn: a cache that the CPU budget evicts.
    std::uint64_t tiles = 0;
    for (vgl_pack* p : f.ctx->packs)
    {
        p->tiles.for_each([&](std::uint64_t, const tile_state& t) { tiles += t.cpu_bytes(); });
    }
    std::uint64_t pending = 0;
    f.ctx->resources.for_each([&](std::uint64_t, const gpu_resource& r) {
        if (r.current == gpu_resource::state::resident)
        {
            CHECK_EQ(r.cpu_bytes(), 0u);
        }
        else
        {
            pending += r.cpu_bytes();
        }
    });
    CHECK_EQ(f.ctx->cpu_used, tiles + pending);
    CHECK_EQ(f.ctx->gpu_used, gpu.bytes());
}

TEST_CASE(tiles_without_their_own_imagery_use_an_ancestors)
{
    // Both packs have level-2 tiles only at (2, 3, 1) and (2, 4, 1). Their neighbour (2, 2, 1)
    // is drawn from the level-1 terrain, and with the level-1 texture.
    fixture f(0, 0, true);
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position at = inside({2, 2, 1}, 0.9, 0.5);
    f.look(at.lat, at.lon, 200000.0);
    settle(f.ctx, f.io, gpu);
    bool ancestor = false;
    for (const vgl_draw_item& item : gpu.items)
    {
        REQUIRE(item.layer_count == 1u);
        const vgl_draw_layer& layer = gpu.layers[item.first_layer];
        check_layer(gpu, item, layer);
        tile_id source;
        REQUIRE(imagery_tile_of(id_of(item.tile), gpu.images.at(layer.texture).pixels[2], source));
        ancestor = ancestor || source.level < item.tile.level;
    }
    CHECK(ancestor);
}

TEST_CASE(texture_formats_follow_the_consumers_preference)
{
    fixture f(0, 0, true);
    REQUIRE(f.ctx != nullptr);
    const vgl_texture_format formats[] = {
        VGL_TEXTURE_FORMAT_BC7, VGL_TEXTURE_FORMAT_RGBA8_SRGB, VGL_TEXTURE_FORMAT_RGBA8};
    REQUIRE(vgl_context_texture_formats_set(f.ctx, formats, 3) == VGL_RESULT_SUCCESS);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.images.empty());
    for (const auto& [id, img] : gpu.images)
    {
        CHECK(img.format == VGL_TEXTURE_FORMAT_RGBA8_SRGB);
    }
}

TEST_CASE(textures_are_created_again_after_a_reset_or_eviction)
{
    fixture f(1, 0, true); // Nothing stays beyond what is drawn and wanted.
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    settle(f.ctx, f.io, gpu);
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    // Only the textures of the frame stay.
    std::set<std::uint64_t> used;
    for (const vgl_draw_item& item : gpu.items)
    {
        REQUIRE(item.layer_count == 1u);
        used.insert(gpu.layers[item.first_layer].texture);
    }
    CHECK_EQ(gpu.images.size(), used.size());

    // Back to where the evicted textures were: they are decoded and created again.
    f.look(0.0, -30.0, 2e7);
    settle(f.ctx, f.io, gpu);
    for (const vgl_draw_item& item : gpu.items)
    {
        REQUIRE(item.layer_count == 1u);
        check_layer(gpu, item, gpu.layers[item.first_layer]);
    }

    // The device is lost.
    gpu.resident.clear();
    gpu.images.clear();
    REQUIRE(vgl_context_resources_reset(f.ctx) == VGL_RESULT_SUCCESS);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.items.empty());
    for (const vgl_draw_item& item : gpu.items)
    {
        REQUIRE(item.layer_count == 1u);
        check_layer(gpu, item, gpu.layers[item.first_layer]);
    }
}

TEST_CASE(imagery_is_credited_and_can_be_removed)
{
    fixture f(0, 0, true);
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    std::vector<std::string> lines = credits(f.ctx);
    CHECK(std::find(lines.begin(), lines.end(), "Test imagery") != lines.end());

    // Without its pack, tiles are drawn without layers, and its textures are destroyed.
    REQUIRE(vgl_pack_destroy(f.imagery) == VGL_RESULT_SUCCESS);
    f.imagery = nullptr;
    settle(f.ctx, f.io, gpu);
    for (const vgl_draw_item& item : gpu.items)
    {
        CHECK_EQ(item.layer_count, 0u);
    }
    CHECK(gpu.images.empty());
    lines = credits(f.ctx);
    CHECK(std::find(lines.begin(), lines.end(), "Test imagery") == lines.end());
}

TEST_CASE(draw_items_carry_the_geoid_at_their_corners)
{
    // Without a geoid pack, the draw items say so.
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    const position p = inside({3, 6, 2}, 0.5, 0.5);
    f.look(p.lat, p.lon, 50000.0);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.items.empty());
    for (const vgl_draw_item& item : gpu.items)
    {
        CHECK_EQ(item.has_geoid, VGL_FALSE);
        CHECK_EQ(item.geoid_heights[0], 0.0f);
    }

    // With one, every drawn mesh gets its corners once the geoid has loaded there, also the
    // meshes built before it did. The test geoid is a constant height.
    const test_pack geoid = make_pack(layer_type::geoid);
    f.io.packs["mem://geoid"] = &geoid;
    vgl_pack_create_info create{};
    create.struct_size = sizeof(create);
    create.uri = "mem://geoid";
    vgl_pack* g = nullptr;
    REQUIRE(vgl_pack_create(f.ctx, &create, &g) == VGL_RESULT_SUCCESS);
    settle(f.ctx, f.io, gpu);
    REQUIRE(!gpu.items.empty());
    for (const vgl_draw_item& item : gpu.items)
    {
        CHECK_EQ(item.has_geoid, VGL_TRUE);
        for (const float h : item.geoid_heights)
        {
            CHECK_NEAR(h, geoid_height, 1e-3);
        }
    }
    CHECK_EQ(vgl_pack_destroy(g), VGL_RESULT_SUCCESS);
}
