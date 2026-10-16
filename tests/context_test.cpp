// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the context (vgl/context.h): pack loading through the I/O interface, decoding on
// the task interface or in the update, layering, the CPU budget, and height queries. Packs
// are built in memory with the baker's writers.

#include "context/context_impl.hpp"
#include "test_packs.hpp"
#include "test_views.hpp"
#include "vgl_test.hpp"

#include <vgl/context.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace vgl::internal;
using namespace vgl::internal::pack;
using namespace vgl::test;

// One worker thread for I/O completions and tasks, as a host might have.
class worker
{
public:
    worker() : thread_([this] { loop(); }) {}

    worker(const worker&) = delete;
    worker& operator=(const worker&) = delete;

    ~worker() { stop(); }

    void post(std::function<void()> f)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            work_.push_back(std::move(f));
        }
        wake_.notify_one();
    }

    // Finishes the work posted so far, then joins.
    void stop()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

private:
    void loop()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !work_.empty(); });
                if (work_.empty())
                {
                    return;
                }
                f = std::move(work_.front());
                work_.pop_front();
            }
            f();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> work_;
    bool stopping_ = false;
    std::thread thread_;
};

struct threaded_host
{
    std::map<std::string, const test_pack*> packs;
    memory_io server; // Used only for its serve().
    worker thread;
    vgl_io_interface io{VGL_IO_INTERFACE_VERSION, 0, this, &read, nullptr, nullptr};
    vgl_task_interface tasks{VGL_TASK_INTERFACE_VERSION, 0, this, &submit, nullptr};

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* r)
    {
        auto* self = static_cast<threaded_host*>(user_data);
        self->thread.post(
            [self, request, uri = std::string(r->uri), offset = r->offset, size = r->size] {
                const vgl_io_read copy{uri.c_str(), offset, size};
                self->server.serve(request, copy);
            });
    }

    static vgl_result submit(void* user_data, vgl_task_fn fn, void* data)
    {
        static_cast<threaded_host*>(user_data)->thread.post([fn, data] { fn(data); });
        return VGL_RESULT_SUCCESS;
    }
};

vgl_context_create_info context_info()
{
    vgl_context_create_info info{};
    info.struct_size = sizeof(info);
    return info;
}

vgl_pack* add_pack(vgl_context* ctx, const char* uri, std::int32_t priority = 0)
{
    vgl_pack_create_info info{};
    info.struct_size = sizeof(info);
    info.priority = priority;
    info.uri = uri;
    vgl_pack* p = nullptr;
    CHECK_EQ(vgl_pack_create(ctx, &info, &p), VGL_RESULT_SUCCESS);
    return p;
}

vgl_pack_state state_of(const vgl_pack* p, vgl_result* failure = nullptr)
{
    vgl_pack_state s{};
    CHECK_EQ(vgl_pack_state_get(p, &s, failure), VGL_RESULT_SUCCESS);
    return s;
}

// Queries until the result is final, updating in between. pump runs the host's work.
vgl_result query(vgl_context* ctx,
                 const position& at,
                 double& height,
                 vgl_height_reference reference = VGL_HEIGHT_REFERENCE_ELLIPSOID,
                 const std::function<void()>& pump = {})
{
    vgl_result r = VGL_RESULT_NOT_READY;
    for (int i = 0; i < 10000 && r == VGL_RESULT_NOT_READY; ++i)
    {
        r = vgl_context_height_query(
            ctx, to_radians(at.lat), to_radians(at.lon), reference, &height);
        if (r == VGL_RESULT_NOT_READY)
        {
            if (pump)
            {
                pump();
            }
            CHECK_EQ(vgl_context_update(ctx, nullptr), VGL_RESULT_SUCCESS);
        }
    }
    return r;
}

} // namespace

TEST_CASE(context_create_validates_its_input)
{
    vgl_context* ctx = nullptr;
    CHECK_EQ(vgl_context_create(nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_create(nullptr, &ctx), VGL_RESULT_SUCCESS);
    REQUIRE(ctx != nullptr);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(nullptr), VGL_RESULT_SUCCESS);

    vgl_context_create_info info = context_info();
    info.struct_size = 4;
    CHECK_EQ(vgl_context_create(&info, &ctx), VGL_RESULT_INVALID_ARGUMENT);
    CHECK(ctx == nullptr);

    // A newer structure is accepted while its new fields are zero.
    struct
    {
        vgl_context_create_info info;
        std::uint64_t extra;
    } newer{};
    newer.info.struct_size = sizeof(newer);
    CHECK_EQ(vgl_context_create(&newer.info, &ctx), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    newer.extra = 1;
    CHECK_EQ(vgl_context_create(&newer.info, &ctx), VGL_RESULT_NOT_SUPPORTED);

    vgl_io_interface io{};
    info = context_info();
    info.io = &io;
    CHECK_EQ(vgl_context_create(&info, &ctx), VGL_RESULT_VERSION_MISMATCH);
    io.version = VGL_IO_INTERFACE_VERSION;
    CHECK_EQ(vgl_context_create(&info, &ctx), VGL_RESULT_INVALID_ARGUMENT); // No read.

    vgl_task_interface tasks{};
    info = context_info();
    info.tasks = &tasks;
    CHECK_EQ(vgl_context_create(&info, &ctx), VGL_RESULT_VERSION_MISMATCH);

    const vgl_ellipsoid bad{-1.0, 0.0};
    info = context_info();
    info.ellipsoid = &bad;
    CHECK_EQ(vgl_context_create(&info, &ctx), VGL_RESULT_INVALID_ARGUMENT);
}

TEST_CASE(pack_create_needs_an_io_interface)
{
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(nullptr, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack_create_info info{};
    info.struct_size = sizeof(info);
    info.uri = "mem://a";
    vgl_pack* p = nullptr;
    CHECK_EQ(vgl_pack_create(ctx, &info, &p), VGL_RESULT_INVALID_OPERATION);
    CHECK(p == nullptr);
    info.uri = nullptr;
    CHECK_EQ(vgl_pack_create(ctx, &info, &p), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_pack_create(ctx, nullptr, &p), VGL_RESULT_INVALID_ARGUMENT);

    double h = 0.0;
    CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
             VGL_RESULT_NOT_FOUND);
    CHECK(std::isnan(h));
    CHECK_EQ(vgl_context_height_query(ctx, 2.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
             VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_height_query(ctx, 0.0, NAN, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
             VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL, &h),
             VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(libraries_on_a_context_share_its_allocator_log_and_view)
{
    test_allocator memory;
    struct messages
    {
        std::vector<std::pair<vgl_log_level, std::string>> lines;
    } log;
    vgl_context_create_info info = context_info();
    info.allocator = &memory.table;
    info.log_level = VGL_LOG_LEVEL_INFO;
    info.log = [](void* user_data, vgl_log_level level, const char* message) {
        static_cast<messages*>(user_data)->lines.emplace_back(level, message);
    };
    info.log_user_data = &log;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);

    // The host's allocator itself, not a wrapper.
    vgl_allocator_interface a{};
    CHECK_EQ(vgl_context_allocator_get(ctx, &a), VGL_RESULT_SUCCESS);
    CHECK(a.user_data == &memory && a.allocate == memory.table.allocate);
    CHECK_EQ(vgl_context_allocator_get(ctx, nullptr), VGL_RESULT_INVALID_ARGUMENT);

    // Messages pass the context's level filter.
    CHECK_EQ(vgl_context_log(ctx, VGL_LOG_LEVEL_INFO, "test: shown"), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_log(ctx, VGL_LOG_LEVEL_DEBUG, "test: hidden"), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_log(ctx, static_cast<vgl_log_level>(9), "test: bad"),
             VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_log(ctx, VGL_LOG_LEVEL_ERROR, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    REQUIRE(log.lines.size() == 1u);
    CHECK(log.lines[0].first == VGL_LOG_LEVEL_INFO && log.lines[0].second == "test: shown");

    // The view comes back as it was set; a newer caller's extra fields are left alone.
    struct
    {
        vgl_view view;
        std::uint64_t extra;
    } got{};
    got.view.struct_size = sizeof(got);
    got.extra = 7;
    CHECK_EQ(vgl_context_view_get(ctx, &got.view), VGL_RESULT_NOT_FOUND);
    const vgl_view view = make_view(
        look_at({2e7, 0, 0}, {0, 0, 0}, {0, 0, 1}), perspective_gl(1.0, 1.5, 1.0, 1e9), 300, 200);
    REQUIRE(vgl_context_view_set(ctx, &view) == VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_view_get(ctx, &got.view), VGL_RESULT_SUCCESS);
    CHECK(got.view.struct_size == sizeof(got) && got.extra == 7u);
    CHECK(got.view.viewport_width == 300u && got.view.viewport_height == 200u);
    CHECK(std::memcmp(&got.view.view_matrix, &view.view_matrix, sizeof(vgl_dmat4)) == 0);
    CHECK(std::memcmp(&got.view.projection_matrix, &view.projection_matrix, sizeof(vgl_dmat4)) ==
          0);
    got.view.struct_size = 8;
    CHECK_EQ(vgl_context_view_get(ctx, &got.view), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_view_set(ctx, nullptr), VGL_RESULT_SUCCESS);
    got.view.struct_size = sizeof(vgl_view);
    CHECK_EQ(vgl_context_view_get(ctx, &got.view), VGL_RESULT_NOT_FOUND);

    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(heights_come_from_the_deepest_level)
{
    const test_pack pack = make_pack();
    test_allocator memory;
    memory_io io;
    io.packs["mem://a"] = &pack;
    {
        vgl_context_create_info info = context_info();
        info.allocator = &memory.table;
        info.io = &io.table;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* p = add_pack(ctx, "mem://a");
        CHECK_EQ(state_of(p), VGL_PACK_STATE_LOADING);
        CHECK_EQ(io.uris.back(), std::string("mem://a/pack.vglm"));

        double h = 0.0;
        CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
                 VGL_RESULT_NOT_READY);
        CHECK(std::isnan(h));
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        CHECK_EQ(state_of(p), VGL_PACK_STATE_READY);

        // Level 3, inside the shards.
        const tile_id deep{3, 6, 2};
        const position a = inside(deep);
        CHECK_EQ(query(ctx, a, h), VGL_RESULT_SUCCESS);
        CHECK_NEAR(h, expected_height(pack, deep, a.lat, a.lon), 1e-9);

        // Level 1, where the pack has no finer tiles.
        const tile_id coarse{1, 0, 1};
        const position b = inside(coarse);
        CHECK_EQ(query(ctx, b, h), VGL_RESULT_SUCCESS);
        CHECK_NEAR(h, expected_height(pack, coarse, b.lat, b.lon), 1e-9);

        // Nodata at level 3 falls back to level 2.
        const tile_id holey{3, 8, 2};
        const position c = inside(holey, 0.5, 0.99);
        CHECK_EQ(query(ctx, c, h), VGL_RESULT_SUCCESS);
        CHECK_NEAR(h, expected_height(pack, tile_parent(holey), c.lat, c.lon), 1e-9);

        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
                 VGL_RESULT_NOT_FOUND);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(io.destroyed, 1);
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(higher_priority_packs_win)
{
    const test_pack low = make_pack(layer_type::elevation, 0);
    const test_pack high = make_pack(layer_type::elevation, 40);
    memory_io io;
    io.packs["mem://low"] = &low;
    io.packs["mem://high"] = &high;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* a = add_pack(ctx, "mem://high", 1);
    vgl_pack* b = add_pack(ctx, "mem://low", 0);
    const tile_id t{3, 7, 3};
    const position at = inside(t);
    double h = 0.0;
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(high, t, at.lat, at.lon), 1e-9);

    // Among equal priorities, the later pack wins.
    vgl_pack* c = add_pack(ctx, "mem://low", 1);
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(low, t, at.lat, at.lon), 1e-9);

    CHECK_EQ(vgl_pack_destroy(a), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(b), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(c), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(mean_sea_level_heights_subtract_the_geoid)
{
    const test_pack terrain_pack = make_pack();
    const test_pack geoid = make_pack(layer_type::geoid);
    memory_io io;
    io.packs["mem://dem"] = &terrain_pack;
    io.packs["mem://geoid"] = &geoid;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* dem = add_pack(ctx, "mem://dem");
    vgl_pack* n = add_pack(ctx, "mem://geoid");

    const tile_id t{3, 6, 3};
    const position at = inside(t);
    double h = 0.0;
    CHECK_EQ(query(ctx, at, h, VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(terrain_pack, t, at.lat, at.lon) - geoid_height, 1e-9);
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(terrain_pack, t, at.lat, at.lon), 1e-9);

    // A second geoid pack fails.
    vgl_pack* second = add_pack(ctx, "mem://geoid");
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    vgl_result why = VGL_RESULT_SUCCESS;
    CHECK_EQ(state_of(second, &why), VGL_PACK_STATE_FAILED);
    CHECK_EQ(why, VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_pack_destroy(second), VGL_RESULT_SUCCESS);

    CHECK_EQ(vgl_pack_destroy(n), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL, &h),
             VGL_RESULT_INVALID_OPERATION);
    CHECK_EQ(vgl_pack_destroy(dem), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(packs_that_cannot_load_fail)
{
    test_pack corrupt = make_pack();
    corrupt.files[std::string(manifest_file_name)][20] ^= std::byte{0x40};
    memory_io io;
    io.packs["mem://corrupt"] = &corrupt;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* missing = add_pack(ctx, "mem://missing");
    vgl_pack* bad = add_pack(ctx, "mem://corrupt");
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    vgl_result why = VGL_RESULT_SUCCESS;
    CHECK_EQ(state_of(missing, &why), VGL_PACK_STATE_FAILED);
    CHECK_EQ(why, VGL_RESULT_NOT_FOUND);
    CHECK_EQ(state_of(bad, &why), VGL_PACK_STATE_FAILED);
    CHECK_EQ(why, VGL_RESULT_CORRUPT_DATA);
    double h = 0.0;
    CHECK_EQ(vgl_context_height_query(ctx, 0.0, 0.0, VGL_HEIGHT_REFERENCE_ELLIPSOID, &h),
             VGL_RESULT_NOT_FOUND);
    CHECK_EQ(vgl_pack_destroy(missing), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(bad), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(corrupt_shards_fall_back_to_coarser_data)
{
    test_pack pack = make_pack();
    // Damage the tile data of both level-2 shards, past their headers and directories.
    for (auto& [path, bytes] : pack.files)
    {
        if (path.find("base") == std::string::npos && path != manifest_file_name)
        {
            bytes[bytes.size() / 2] ^= std::byte{0x01};
            bytes[bytes.size() / 3] ^= std::byte{0x01};
            bytes[bytes.size() / 4] ^= std::byte{0x01};
        }
    }
    memory_io io;
    io.packs["mem://a"] = &pack;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* p = add_pack(ctx, "mem://a");
    // Whatever was damaged, every position still gets a height from level 1 or deeper.
    for (std::uint32_t x = 6; x < 10; ++x)
    {
        for (std::uint32_t y = 2; y < 4; ++y)
        {
            double h = 0.0;
            const tile_id t{3, x, y};
            const position at = inside(t, 0.4, 0.4);
            CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
            CHECK(std::isfinite(h));
        }
    }
    CHECK_EQ(state_of(p), VGL_PACK_STATE_READY);
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(deferred_io_and_tasks)
{
    const test_pack pack = make_pack();
    test_allocator memory;
    memory_io io;
    io.deferred = true;
    io.packs["mem://a"] = &pack;
    manual_tasks tasks;
    {
        vgl_context_create_info info = context_info();
        info.allocator = &memory.table;
        info.io = &io.table;
        info.tasks = &tasks.table;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* p = add_pack(ctx, "mem://a");
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        CHECK_EQ(state_of(p), VGL_PACK_STATE_LOADING);
        io.complete_all();
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        CHECK_EQ(state_of(p), VGL_PACK_STATE_READY);

        const tile_id t{3, 7, 2};
        const position at = inside(t);
        double h = 0.0;
        CHECK_EQ(query(ctx,
                       at,
                       h,
                       VGL_HEIGHT_REFERENCE_ELLIPSOID,
                       [&] {
                           io.complete_all();
                           tasks.run_all();
                       }),
                 VGL_RESULT_SUCCESS);
        CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);

        // Leave reads and tasks with the host, then destroy everything.
        const position elsewhere = inside({3, 9, 3});
        for (int i = 0; i < 6; ++i)
        {
            static_cast<void>(vgl_context_height_query(ctx,
                                                       to_radians(elsewhere.lat),
                                                       to_radians(elsewhere.lon),
                                                       VGL_HEIGHT_REFERENCE_ELLIPSOID,
                                                       &h));
            io.complete_all();
            REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        }
        static_cast<void>(vgl_context_height_query(ctx,
                                                   to_radians(elsewhere.lat),
                                                   to_radians(elsewhere.lon),
                                                   VGL_HEIGHT_REFERENCE_ELLIPSOID,
                                                   &h));
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    }
    // The host finishes its work after the context is gone.
    io.complete_all();
    tasks.run_all();
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(io_and_tasks_on_another_thread)
{
    const test_pack pack = make_pack();
    test_allocator memory;
    {
        threaded_host host;
        host.server.packs["mem://a"] = &pack;
        vgl_context_create_info info = context_info();
        info.allocator = &memory.table;
        info.io = &host.io;
        info.tasks = &host.tasks;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* p = add_pack(ctx, "mem://a");
        for (std::uint32_t x = 6; x < 10; ++x)
        {
            const tile_id t{3, x, 3};
            const position at = inside(t);
            double h = 0.0;
            CHECK_EQ(
                query(
                    ctx, at, h, VGL_HEIGHT_REFERENCE_ELLIPSOID, [] { std::this_thread::yield(); }),
                VGL_RESULT_SUCCESS);
            CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);
        }
        // Destroy while work is outstanding.
        double h = 0.0;
        const position at = inside({3, 6, 2});
        static_cast<void>(vgl_context_height_query(
            ctx, to_radians(at.lat), to_radians(at.lon), VGL_HEIGHT_REFERENCE_ELLIPSOID, &h));
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
        host.thread.stop();
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(the_cpu_budget_evicts_unused_tiles)
{
    const test_pack pack = make_pack();
    memory_io io;
    io.packs["mem://a"] = &pack;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    info.cpu_budget = 1; // Nothing stays after the frame it was used in.
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* p = add_pack(ctx, "mem://a");
    const tile_id t{3, 6, 2};
    const position at = inside(t);
    double h = 0.0;
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);
    CHECK(ctx->cpu_used > 0);
    // The first update keeps the tiles the query used; the second frees them.
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    CHECK(ctx->cpu_used > 0);
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    CHECK_EQ(ctx->cpu_used, 0u);
    // The tiles load again.
    const std::size_t reads = io.uris.size();
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);
    CHECK(io.uris.size() > reads);
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(allocation_failures_never_leak_or_crash)
{
    const test_pack pack = make_pack();
    const test_pack geoid = make_pack(layer_type::geoid);
    for (long fail_at = 0;; ++fail_at)
    {
        test_allocator memory;
        memory.fail_at = fail_at;
        memory_io io;
        io.packs["mem://a"] = &pack;
        io.packs["mem://geoid"] = &geoid;
        manual_tasks tasks;
        vgl_result last = VGL_RESULT_NOT_READY;
        {
            vgl_context_create_info info = context_info();
            info.allocator = &memory.table;
            info.io = &io.table;
            info.tasks = fail_at % 2 == 0 ? &tasks.table : nullptr;
            vgl_context* ctx = nullptr;
            if (vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS)
            {
                vgl_pack_create_info create{};
                create.struct_size = sizeof(create);
                create.uri = "mem://a";
                vgl_pack* p = nullptr;
                vgl_pack* n = nullptr;
                static_cast<void>(vgl_pack_create(ctx, &create, &p));
                create.uri = "mem://geoid";
                static_cast<void>(vgl_pack_create(ctx, &create, &n));
                const position at = inside({3, 6, 2});
                for (int i = 0; i < 40; ++i)
                {
                    double h = 0.0;
                    const vgl_result r =
                        vgl_context_height_query(ctx,
                                                 to_radians(at.lat),
                                                 to_radians(at.lon),
                                                 VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL,
                                                 &h);
                    last = r;
                    CHECK(r == VGL_RESULT_SUCCESS || r == VGL_RESULT_NOT_READY ||
                          r == VGL_RESULT_NOT_FOUND || r == VGL_RESULT_OUT_OF_MEMORY ||
                          r == VGL_RESULT_INVALID_OPERATION);
                    tasks.run_all();
                    static_cast<void>(vgl_context_update(ctx, nullptr));
                }
                // Tiles and shards that ran out of memory are read again, so once both packs
                // loaded, the query reaches the final height.
                vgl_pack_state dem_state{};
                vgl_pack_state geoid_state{};
                if (p != nullptr && n != nullptr &&
                    vgl_pack_state_get(p, &dem_state, nullptr) == VGL_RESULT_SUCCESS &&
                    vgl_pack_state_get(n, &geoid_state, nullptr) == VGL_RESULT_SUCCESS &&
                    dem_state == VGL_PACK_STATE_READY && geoid_state == VGL_PACK_STATE_READY)
                {
                    CHECK_EQ(last, VGL_RESULT_SUCCESS);
                }
                static_cast<void>(vgl_pack_destroy(p));
                static_cast<void>(vgl_pack_destroy(n));
                static_cast<void>(vgl_context_destroy(ctx));
            }
        }
        tasks.run_all();
        CHECK_EQ(memory.live.load(), 0);
        if (!memory.failed)
        {
            // The run without a failure reaches the final height.
            CHECK_EQ(last, VGL_RESULT_SUCCESS);
            CHECK(fail_at > 50);
            break; // Every allocation of the scenario has been failed once.
        }
    }
}

namespace
{

// Updates until nothing is loading. Returns the number of updates.
int settle(vgl_context* ctx, const memory_io& io)
{
    int updates = 0;
    std::size_t reads = 0;
    do
    {
        reads = io.uris.size();
        for (int i = 0; i < 3; ++i)
        {
            CHECK_EQ(vgl_context_update(ctx, nullptr), VGL_RESULT_SUCCESS);
            ++updates;
        }
    } while (io.uris.size() != reads && updates < 300);
    return updates;
}

bool is_ancestor(const tile_id& a, const tile_id& b)
{
    return a.level < b.level && tile_ancestor(b, a.level) == a;
}

// No selected tile overlaps another.
bool disjoint(const vector<selected_tile>& tiles)
{
    for (const selected_tile& a : tiles)
    {
        for (const selected_tile& b : tiles)
        {
            if (&a != &b && (a.id == b.id || is_ancestor(a.id, b.id)))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE(view_set_validates_its_input)
{
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(nullptr, &ctx) == VGL_RESULT_SUCCESS);
    vgl_view v = camera_over(0.0, 0.0, 1e7);
    CHECK_EQ(vgl_context_view_set(nullptr, &v), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_view_set(ctx, &v), VGL_RESULT_SUCCESS);
    CHECK(ctx->has_view);
    vgl_view bad = v;
    bad.struct_size = 8;
    CHECK_EQ(vgl_context_view_set(ctx, &bad), VGL_RESULT_INVALID_ARGUMENT);
    bad = v;
    bad.viewport_width = 0;
    CHECK_EQ(vgl_context_view_set(ctx, &bad), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_context_view_set(ctx, nullptr), VGL_RESULT_SUCCESS);
    CHECK(!ctx->has_view);
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    CHECK(ctx->selected.empty());
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(without_packs_the_ellipsoid_is_selected)
{
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(nullptr, &ctx) == VGL_RESULT_SUCCESS);
    const vgl_view v = camera_over(0.0, 0.0, 2e7);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    CHECK_EQ(ctx->selected.size(), 2u);
    for (const selected_tile& t : ctx->selected)
    {
        CHECK_EQ(t.id.level, 0u);
        CHECK_EQ(t.pack_id, 0u);
    }
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(selection_refines_where_data_has_loaded)
{
    const test_pack pack = make_pack();
    memory_io io;
    io.packs["mem://a"] = &pack;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* p = add_pack(ctx, "mem://a");

    // Low over the middle of tile (3, 6, 2): only that tile is in view, at its finest level.
    const tile_id deep{3, 6, 2};
    const position centre = inside(deep, 0.5, 0.5);
    vgl_view v = camera_over(centre.lat, centre.lon, 50000.0);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    REQUIRE(ctx->selected.size() == 1);
    CHECK(ctx->selected[0].id == deep);
    CHECK(ctx->selected[0].source == deep);
    CHECK_EQ(ctx->selected[0].pack_id, p->id);

    // From far away, half the globe is in view. Tiles cover it without overlapping, and none
    // lies wholly behind the horizon, about 75 degrees from the centre of the view.
    v = camera_over(0.0, 0.0, 2e7);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    CHECK(ctx->selected.size() > 2);
    CHECK(disjoint(ctx->selected));
    bool drawn_from_ancestor = false;
    for (const selected_tile& t : ctx->selected)
    {
        const degree_region b = tile_bounds(t.id);
        CHECK(b.west < 80.0 && b.east > -80.0);
        CHECK_EQ(t.pack_id, p->id);
        drawn_from_ancestor = drawn_from_ancestor || !(t.source == t.id);
    }
    // Children the pack lacks are drawn from their parents.
    CHECK(drawn_from_ancestor);

    // The selection is the same in every frame.
    std::vector<selected_tile> before(ctx->selected.begin(), ctx->selected.end());
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    REQUIRE(ctx->selected.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        CHECK(ctx->selected[i].id == before[i].id);
    }

    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(selected_tiles_are_not_evicted)
{
    const test_pack pack = make_pack();
    memory_io io;
    io.packs["mem://a"] = &pack;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    info.cpu_budget = 1;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* p = add_pack(ctx, "mem://a");
    const position centre = inside({3, 7, 3}, 0.5, 0.5);
    const vgl_view v = camera_over(centre.lat, centre.lon, 50000.0);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    REQUIRE(ctx->selected.size() == 1);
    CHECK(ctx->selected[0].source == (tile_id{3, 7, 3}));

    // The view keeps its tiles and their ancestors; nothing is read again.
    const std::size_t reads = io.uris.size();
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(io.uris.size(), reads);
    CHECK(ctx->selected[0].source == (tile_id{3, 7, 3}));
    CHECK(ctx->cpu_used > 0);

    // Without a view, they go.
    REQUIRE(vgl_context_view_set(ctx, nullptr) == VGL_RESULT_SUCCESS);
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    CHECK_EQ(ctx->cpu_used, 0u);
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

namespace
{

// The mesh of a selected tile, or null.
// The newest mesh of a tile. Without frames, meshes keep their CPU copies.
const gpu_resource* mesh_of(vgl_context* ctx, const tile_id& t)
{
    return latest_mesh(*ctx, t);
}

// Height of mesh vertex (i, j).
float mesh_height(const gpu_resource& m, std::uint32_t i, std::uint32_t j)
{
    return m.vertices.data()[static_cast<std::size_t>(j) * m.grid + i].height;
}

// Position in degrees of mesh vertex (i, j) of tile t.
position mesh_position(const tile_id& t, std::uint32_t grid, std::uint32_t i, std::uint32_t j)
{
    const degree_region b = tile_bounds(t);
    const double step = tile_span(t.level) / (grid - 1);
    return {b.north - j * step, b.west + i * step};
}

} // namespace

TEST_CASE(selected_tiles_get_meshes_of_their_data)
{
    const test_pack pack = make_pack();
    memory_io io;
    io.packs["mem://a"] = &pack;
    vgl_context_create_info info = context_info();
    info.io = &io.table;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    vgl_pack* p = add_pack(ctx, "mem://a");

    // A tile with its own data: one vertex per sample.
    const tile_id deep{3, 6, 2};
    const position centre = inside(deep, 0.5, 0.5);
    vgl_view v = camera_over(centre.lat, centre.lon, 50000.0);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    const gpu_resource* m = mesh_of(ctx, deep);
    REQUIRE(m != nullptr);
    CHECK(m->source == deep);
    CHECK_EQ(m->grid, pack_tile_size);
    CHECK_EQ(m->vertices.size(), tile_mesh_vertex_count(pack_tile_size));
    const std::vector<std::int16_t>& q = pack.samples.at(tile_key(deep));
    const double offset = pack.offsets.at(tile_key(deep));
    for (std::uint32_t j = 0; j < pack_tile_size; j += 5)
    {
        for (std::uint32_t i = 0; i < pack_tile_size; i += 3)
        {
            const double expected =
                offset + pack.steps[3] * q[static_cast<std::size_t>(j) * pack_tile_size + i];
            CHECK_NEAR(mesh_height(*m, i, j), expected, 1e-3);
        }
    }

    // Nodata in the top row of (3, 8, 2) is filled from its parent.
    const tile_id holey{3, 8, 2};
    const position over = inside(holey, 0.5, 0.5);
    v = camera_over(over.lat, over.lon, 50000.0);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    m = mesh_of(ctx, holey);
    REQUIRE(m != nullptr);
    CHECK(m->source == holey);
    for (std::uint32_t i = 0; i < pack_tile_size; i += 4)
    {
        const position at = mesh_position(holey, pack_tile_size, i, 0);
        CHECK_NEAR(
            mesh_height(*m, i, 0), expected_height(pack, tile_parent(holey), at.lat, at.lon), 1e-3);
    }

    // From far away, children the pack lacks are drawn from a part of an ancestor.
    v = camera_over(0.0, 0.0, 2e7);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    settle(ctx, io);
    int from_ancestor = 0;
    for (const selected_tile& t : ctx->selected)
    {
        m = mesh_of(ctx, t.id);
        REQUIRE(m != nullptr);
        CHECK(m->source == t.source);
        CHECK(m->grid >= min_mesh_grid);
        if (!(t.source == t.id))
        {
            ++from_ancestor;
            const std::uint32_t g = m->grid;
            for (std::uint32_t k = 0; k < g; k += 2)
            {
                const position at = mesh_position(t.id, g, k, g - 1 - k);
                CHECK_NEAR(mesh_height(*m, k, g - 1 - k),
                           expected_height(pack, t.source, at.lat, at.lon),
                           1e-2);
            }
        }
    }
    CHECK(from_ancestor > 0);

    // The pack's meshes go with it.
    const std::uint32_t pack_id = p->id;
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    bool any = false;
    ctx->resources.for_each([&](std::uint64_t, const gpu_resource& r) {
        any = any || (r.resource_kind == gpu_resource::kind::mesh && r.pack_id == pack_id);
    });
    CHECK(!any);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(the_bare_ellipsoid_gets_a_mesh)
{
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(nullptr, &ctx) == VGL_RESULT_SUCCESS);
    const vgl_view v = camera_over(0.0, 0.0, 2e7);
    REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);
    // Without tasks, meshes build within the update's time budget, at least one per update.
    for (int i = 0; i < 2; ++i)
    {
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    }
    REQUIRE(ctx->selected.size() == 2);
    for (const selected_tile& t : ctx->selected)
    {
        const gpu_resource* m = mesh_of(ctx, t.id);
        REQUIRE(m != nullptr);
        CHECK_EQ(m->pack_id, 0u);
        CHECK_EQ(m->grid, 121u); // 180 degrees at 1.5 degrees.
        CHECK_EQ(mesh_height(*m, 60, 60), 0.0f);
    }
    CHECK(ctx->cpu_used > 0);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
}

TEST_CASE(meshes_build_on_tasks_and_keep_the_old_mesh_meanwhile)
{
    const test_pack pack = make_pack();
    test_allocator memory;
    memory_io io;
    io.packs["mem://a"] = &pack;
    manual_tasks tasks;
    {
        vgl_context_create_info info = context_info();
        info.allocator = &memory.table;
        info.io = &io.table;
        info.tasks = &tasks.table;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* p = add_pack(ctx, "mem://a");
        const tile_id deep{3, 7, 3};
        const position centre = inside(deep, 0.5, 0.5);
        const vgl_view v = camera_over(centre.lat, centre.lon, 50000.0);
        REQUIRE(vgl_context_view_set(ctx, &v) == VGL_RESULT_SUCCESS);

        // While data loads, the tile is drawn from coarser data. A mesh for new data appears
        // only once its task has run; until then the previous mesh stays.
        bool reached = false;
        bool kept_while_building = false;
        for (int i = 0; i < 200 && !reached; ++i)
        {
            REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
            if (ctx->selected.empty())
            {
                tasks.run_all();
                continue;
            }
            const selected_tile s = ctx->selected[0];
            const mesh_state* state = ctx->meshes.find(tile_key(s.id));
            const gpu_resource* m = mesh_of(ctx, s.id);
            if (state != nullptr && state->building && m != nullptr && !(m->source == s.source))
            {
                kept_while_building = true;
            }
            tasks.run_all();
            REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
            m = mesh_of(ctx, s.id);
            reached = m != nullptr && m->source == deep;
        }
        CHECK(reached);
        CHECK(kept_while_building);
        // Leave mesh tasks for a new view with the host.
        const vgl_view far = camera_over(0.0, 0.0, 2e7);
        REQUIRE(vgl_context_view_set(ctx, &far) == VGL_RESULT_SUCCESS);
        REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
        CHECK(!tasks.queued.empty());
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    }
    tasks.run_all();
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(selection_and_meshes_survive_allocation_failures)
{
    const test_pack pack = make_pack();
    for (long fail_at = 0;; ++fail_at)
    {
        test_allocator memory;
        memory.fail_at = fail_at;
        memory_io io;
        io.packs["mem://a"] = &pack;
        manual_tasks tasks;
        bool complete = false;
        {
            vgl_context_create_info info = context_info();
            info.allocator = &memory.table;
            info.io = &io.table;
            info.tasks = fail_at % 2 == 0 ? &tasks.table : nullptr;
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
                for (int i = 0; i < 60; ++i)
                {
                    tasks.run_all();
                    static_cast<void>(vgl_context_update(ctx, nullptr));
                }
                // Once the pack has loaded, everything recovers: every selected tile has a
                // mesh of its current data.
                vgl_pack_state state{};
                if (p != nullptr && vgl_pack_state_get(p, &state, nullptr) == VGL_RESULT_SUCCESS &&
                    state == VGL_PACK_STATE_READY)
                {
                    complete = !ctx->selected.empty() &&
                               std::all_of(ctx->selected.begin(),
                                           ctx->selected.end(),
                                           [&](const selected_tile& t) {
                                               const gpu_resource* m = mesh_of(ctx, t.id);
                                               return m != nullptr && m->source == t.source;
                                           });
                    CHECK(complete);
                }
                static_cast<void>(vgl_pack_destroy(p));
                static_cast<void>(vgl_context_destroy(ctx));
            }
        }
        tasks.run_all();
        CHECK_EQ(memory.live.load(), 0);
        if (!memory.failed)
        {
            CHECK(complete);
            CHECK(fail_at > 50);
            break;
        }
    }
}
