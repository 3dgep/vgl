// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the defaults module (vgl/defaults.h): the thread pool, and the file reader with and
// without it, loading test packs written to a temporary directory.

#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <vgl/context.h>
#include <vgl/defaults.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

namespace
{

using namespace vgl::test;
namespace fs = std::filesystem;

// A directory that holds test packs, removed again at the end.
class temporary_directory
{
public:
    temporary_directory()
    {
        std::random_device seed;
        // A name outside ASCII checks that URIs are read as UTF-8.
        path_ = fs::temp_directory_path() /
                fs::path(u8"vgl-défaults-ü-" +
                         std::u8string(1, static_cast<char8_t>(u8'a' + seed() % 26))) /
                std::to_string(seed());
        fs::create_directories(path_);
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;

    ~temporary_directory()
    {
        std::error_code ignored;
        fs::remove_all(path_.parent_path(), ignored);
    }

    // Writes a pack to a subdirectory and returns its URI.
    std::string write(const test_pack& p, const std::string& name) const
    {
        const fs::path root = path_ / name;
        for (const auto& [relative, bytes] : p.files)
        {
            const fs::path file = root / fs::path(relative);
            fs::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        const std::u8string u8 = root.generic_u8string();
        return {reinterpret_cast<const char*>(u8.data()), u8.size()};
    }

private:
    fs::path path_;
};

vgl_pack* add_pack(vgl_context* ctx, const std::string& uri)
{
    vgl_pack_create_info info{};
    info.struct_size = sizeof(info);
    info.uri = uri.c_str();
    vgl_pack* p = nullptr;
    CHECK_EQ(vgl_pack_create(ctx, &info, &p), VGL_RESULT_SUCCESS);
    return p;
}

vgl_result query(vgl_context* ctx, const position& at, double& height)
{
    vgl_result r = VGL_RESULT_NOT_READY;
    for (int i = 0; i < 100000 && r == VGL_RESULT_NOT_READY; ++i)
    {
        r = vgl_context_height_query(
            ctx, to_radians(at.lat), to_radians(at.lon), VGL_HEIGHT_REFERENCE_ELLIPSOID, &height);
        if (r == VGL_RESULT_NOT_READY)
        {
            std::this_thread::yield();
            CHECK_EQ(vgl_context_update(ctx, nullptr), VGL_RESULT_SUCCESS);
        }
    }
    return r;
}

void count_task(void* data)
{
    ++*static_cast<std::atomic<int>*>(data);
}

} // namespace

TEST_CASE(thread_pool_runs_every_task)
{
    test_allocator memory;
    std::atomic<int> count{0};
    {
        vgl_thread_pool_create_info info{};
        info.struct_size = sizeof(info);
        info.thread_count = 3;
        info.allocator = &memory.table;
        vgl_thread_pool* pool = nullptr;
        REQUIRE(vgl_thread_pool_create(&info, &pool) == VGL_RESULT_SUCCESS);
        vgl_task_interface tasks{};
        REQUIRE(vgl_thread_pool_task_interface_get(pool, &tasks) == VGL_RESULT_SUCCESS);
        CHECK_EQ(tasks.version, static_cast<std::uint32_t>(VGL_TASK_INTERFACE_VERSION));
        for (int i = 0; i < 1000; ++i)
        {
            CHECK_EQ(tasks.submit(tasks.user_data, &count_task, &count), VGL_RESULT_SUCCESS);
        }
        // Destroying runs what is still queued.
        CHECK_EQ(vgl_thread_pool_destroy(pool), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(count.load(), 1000);
    CHECK_EQ(memory.live.load(), 0);

    vgl_thread_pool* pool = nullptr;
    CHECK_EQ(vgl_thread_pool_create(nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    REQUIRE(vgl_thread_pool_create(nullptr, &pool) == VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_thread_pool_destroy(pool), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_thread_pool_destroy(nullptr), VGL_RESULT_SUCCESS);
    vgl_thread_pool_create_info small{};
    small.struct_size = 4;
    CHECK_EQ(vgl_thread_pool_create(&small, &pool), VGL_RESULT_INVALID_ARGUMENT);
}

TEST_CASE(file_reader_without_a_pool)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string uri = dir.write(pack, "dem");

    vgl_file_reader* reader = nullptr;
    REQUIRE(vgl_file_reader_create(nullptr, &reader) == VGL_RESULT_SUCCESS);
    vgl_io_interface io{};
    REQUIRE(vgl_file_reader_io_interface_get(reader, &io) == VGL_RESULT_SUCCESS);
    vgl_context_create_info info{};
    info.struct_size = sizeof(info);
    info.io = &io;
    vgl_context* ctx = nullptr;
    REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);

    vgl_pack* p = add_pack(ctx, uri);
    vgl_pack* with_scheme = add_pack(ctx, "file://" + uri);
    vgl_pack* missing = add_pack(ctx, uri + "-missing");
    REQUIRE(vgl_context_update(ctx, nullptr) == VGL_RESULT_SUCCESS);
    vgl_pack_state state{};
    vgl_result why = VGL_RESULT_SUCCESS;
    CHECK_EQ(vgl_pack_state_get(p, &state, nullptr), VGL_RESULT_SUCCESS);
    CHECK_EQ(state, VGL_PACK_STATE_READY);
    CHECK_EQ(vgl_pack_state_get(with_scheme, &state, nullptr), VGL_RESULT_SUCCESS);
    CHECK_EQ(state, VGL_PACK_STATE_READY);
    CHECK_EQ(vgl_pack_state_get(missing, &state, &why), VGL_RESULT_SUCCESS);
    CHECK_EQ(state, VGL_PACK_STATE_FAILED);
    CHECK_EQ(why, VGL_RESULT_NOT_FOUND);

    const tile_id t{3, 7, 2};
    const position at = inside(t);
    double h = 0.0;
    CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
    CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);

    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(with_scheme), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_pack_destroy(missing), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_file_reader_destroy(reader), VGL_RESULT_SUCCESS);
}

TEST_CASE(file_reader_and_tasks_on_a_pool)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const test_pack geoid = make_pack(layer_type::geoid);
    const std::string dem_uri = dir.write(pack, "dem");
    const std::string geoid_uri = dir.write(geoid, "geoid");
    test_allocator memory;
    {
        vgl_thread_pool_create_info pool_info{};
        pool_info.struct_size = sizeof(pool_info);
        pool_info.thread_count = 2;
        pool_info.allocator = &memory.table;
        vgl_thread_pool* pool = nullptr;
        REQUIRE(vgl_thread_pool_create(&pool_info, &pool) == VGL_RESULT_SUCCESS);
        vgl_file_reader_create_info reader_info{};
        reader_info.struct_size = sizeof(reader_info);
        reader_info.pool = pool;
        reader_info.allocator = &memory.table;
        vgl_file_reader* reader = nullptr;
        REQUIRE(vgl_file_reader_create(&reader_info, &reader) == VGL_RESULT_SUCCESS);

        vgl_task_interface tasks{};
        vgl_io_interface io{};
        REQUIRE(vgl_thread_pool_task_interface_get(pool, &tasks) == VGL_RESULT_SUCCESS);
        REQUIRE(vgl_file_reader_io_interface_get(reader, &io) == VGL_RESULT_SUCCESS);
        vgl_context_create_info info{};
        info.struct_size = sizeof(info);
        info.allocator = &memory.table;
        info.tasks = &tasks;
        info.io = &io;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* dem = add_pack(ctx, dem_uri);
        vgl_pack* n = add_pack(ctx, geoid_uri);

        for (std::uint32_t x = 6; x < 10; ++x)
        {
            const tile_id t{3, x, 3};
            const position at = inside(t);
            double h = 0.0;
            CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
            CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);
        }

        // Tear down with reads and decodes still in flight.
        const position at = inside({3, 6, 2});
        double h = 0.0;
        static_cast<void>(vgl_context_height_query(
            ctx, to_radians(at.lat), to_radians(at.lon), VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL, &h));
        CHECK_EQ(vgl_pack_destroy(dem), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_pack_destroy(n), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_file_reader_destroy(reader), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_thread_pool_destroy(pool), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(memory.live.load(), 0);
}
