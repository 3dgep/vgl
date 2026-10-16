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
    };

    std::map<std::uint64_t, resource> resident;
    std::set<std::uint64_t> ever; // Every id ever created, to check that none is reused.
    std::set<std::uint64_t> fail; // Ids to report as failed, once.
    bool set_handles = false;

    // Per frame.
    std::vector<vgl_draw_item> items;
    std::size_t meshes_created = 0;
    std::size_t index_lists_created = 0;
    std::size_t destroyed = 0;

    // The handle the consumer sets for a resource.
    static std::uint64_t handle_of(std::uint64_t id) { return id * 7 + 1; }

    // Begins, applies and ends a frame, checking the interface's rules.
    void frame(vgl_context* ctx, const vgl_frame_begin_info* info = nullptr)
    {
        vgl_frame* f = nullptr;
        REQUIRE(vgl_context_frame_begin(ctx, info, &f) == VGL_RESULT_SUCCESS);
        apply(f, info);
        CHECK_EQ(vgl_frame_end(f), VGL_RESULT_SUCCESS);
    }

    void apply(vgl_frame* f, const vgl_frame_begin_info* info)
    {
        items.clear();
        meshes_created = 0;
        index_lists_created = 0;
        destroyed = 0;

        const std::uint64_t* destroys = nullptr;
        std::uint32_t count = 0;
        REQUIRE(vgl_frame_destroys_get(f, &destroys, &count) == VGL_RESULT_SUCCESS);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            CHECK(resident.erase(destroys[k]) == 1);
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
            resident[d.resource] = {
                false, std::uint64_t{d.vertex_count} * d.vertex_size, 0, 0, d.vertex_count};
            ++meshes_created;
            if (set_handles)
            {
                CHECK_EQ(vgl_frame_resource_handle_set(f, d.resource, handle_of(d.resource)),
                         VGL_RESULT_SUCCESS);
            }
        }

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
            items.push_back(item);
            // Drawn only once created, in this frame or an earlier one.
            const auto mesh = resident.find(item.mesh);
            const auto list = resident.find(item.index_list);
            REQUIRE(mesh != resident.end());
            REQUIRE(list != resident.end());
            CHECK_EQ(item.index_count, list->second.index_count);
            CHECK_EQ(item.index_size, list->second.index_size);
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
        const bool idle = io.uris.size() == reads && gpu.meshes_created == 0 && gpu.destroyed == 0;
        quiet = idle ? quiet + 1 : 0;
    }
}

struct fixture
{
    test_pack pack = make_pack();
    memory_io io;
    vgl_context* ctx = nullptr;
    vgl_pack* p = nullptr;

    explicit fixture(std::uint64_t gpu_budget = 0, std::uint64_t cpu_budget = 0)
    {
        io.packs["mem://a"] = &pack;
        vgl_context_create_info info{};
        info.struct_size = sizeof(info);
        info.io = &io.table;
        info.gpu_budget = gpu_budget;
        info.cpu_budget = cpu_budget;
        if (vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS)
        {
            vgl_pack_create_info create{};
            create.struct_size = sizeof(create);
            create.uri = "mem://a";
            static_cast<void>(vgl_pack_create(ctx, &create, &p));
        }
    }

    fixture(const fixture&) = delete;
    fixture& operator=(const fixture&) = delete;

    ~fixture()
    {
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
        p->tiles.for_each([&](std::uint64_t, const tile_state& t) {
            tiles += t.q.size() * sizeof(std::int16_t);
        });
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

TEST_CASE(uploads_respect_the_upload_budget)
{
    fixture f;
    REQUIRE(f.ctx != nullptr);
    fake_gpu gpu;
    f.look(0.0, -30.0, 2e7);
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.upload_budget = 1; // One mesh per frame.
    std::size_t total = 0;
    for (int i = 0; i < 200; ++i)
    {
        REQUIRE(vgl_context_update(f.ctx, nullptr) == VGL_RESULT_SUCCESS);
        gpu.frame(f.ctx, &info);
        CHECK(gpu.meshes_created <= 1);
        total += gpu.meshes_created;
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
