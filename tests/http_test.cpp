// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the HTTP reader (vgl/defaults.h), loading test packs from a small HTTP server that
// the test runs on the loopback address, so that it needs no network.

#include "http_server.hpp"
#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <vgl/context.h>
#include <vgl/defaults.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace vgl::test;

vgl_pack* add_pack(vgl_context* ctx, const std::string& uri)
{
    vgl_pack_create_info info{};
    info.struct_size = sizeof(info);
    info.uri = uri.c_str();
    vgl_pack* p = nullptr;
    CHECK_EQ(vgl_pack_create(ctx, &info, &p), VGL_RESULT_SUCCESS);
    return p;
}

// Updates the context until the pack has loaded or failed, for a few seconds at most.
vgl_pack_state wait_for(vgl_context* ctx, vgl_pack* p, vgl_result* why = nullptr)
{
    vgl_pack_state state = VGL_PACK_STATE_LOADING;
    for (int i = 0; i < 1000 && state == VGL_PACK_STATE_LOADING; ++i)
    {
        CHECK_EQ(vgl_context_update(ctx, nullptr), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_pack_state_get(p, &state, why), VGL_RESULT_SUCCESS);
        if (state == VGL_PACK_STATE_LOADING)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return state;
}

vgl_result query(vgl_context* ctx, const position& at, double& height)
{
    vgl_result r = VGL_RESULT_NOT_READY;
    for (int i = 0; i < 1000 && r == VGL_RESULT_NOT_READY; ++i)
    {
        r = vgl_context_height_query(
            ctx, to_radians(at.lat), to_radians(at.lon), VGL_HEIGHT_REFERENCE_ELLIPSOID, &height);
        if (r == VGL_RESULT_NOT_READY)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            CHECK_EQ(vgl_context_update(ctx, nullptr), VGL_RESULT_SUCCESS);
        }
    }
    return r;
}

// A context reading through an HTTP reader.
struct http_context
{
    explicit http_context(const vgl_http_reader_create_info* reader_info = nullptr,
                          const vgl_allocator_interface* allocator = nullptr)
    {
        REQUIRE(vgl_http_reader_create(reader_info, &reader) == VGL_RESULT_SUCCESS);
        REQUIRE(vgl_http_reader_io_interface_get(reader, &io) == VGL_RESULT_SUCCESS);
        CHECK_EQ(io.version, static_cast<std::uint32_t>(VGL_IO_INTERFACE_VERSION));
        vgl_context_create_info info{};
        info.struct_size = sizeof(info);
        info.allocator = allocator;
        info.io = &io;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
    }

    http_context(const http_context&) = delete;
    http_context& operator=(const http_context&) = delete;

    ~http_context()
    {
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_http_reader_destroy(reader), VGL_RESULT_SUCCESS);
    }

    vgl_http_reader* reader = nullptr;
    vgl_io_interface io{};
    vgl_context* ctx = nullptr;
};

void check_heights(vgl_context* ctx, const test_pack& pack)
{
    for (std::uint32_t x = 6; x < 10; ++x)
    {
        const tile_id t{3, x, 3};
        const position at = inside(t);
        double h = 0.0;
        CHECK_EQ(query(ctx, at, h), VGL_RESULT_SUCCESS);
        CHECK_NEAR(h, expected_height(pack, t, at.lat, at.lon), 1e-9);
    }
}

} // namespace

TEST_CASE(http_reader_arguments)
{
    vgl_http_reader* reader = nullptr;
    CHECK_EQ(vgl_http_reader_create(nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    vgl_http_reader_create_info info{};
    info.struct_size = sizeof(info);
    info.timeout = -1.0;
    CHECK_EQ(vgl_http_reader_create(&info, &reader), VGL_RESULT_INVALID_ARGUMENT);
    CHECK(reader == nullptr);
    info.struct_size = 4;
    info.timeout = 0.0;
    CHECK_EQ(vgl_http_reader_create(&info, &reader), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_http_reader_destroy(nullptr), VGL_RESULT_SUCCESS);
    vgl_io_interface io{};
    CHECK_EQ(vgl_http_reader_io_interface_get(nullptr, &io), VGL_RESULT_INVALID_ARGUMENT);
    REQUIRE(vgl_http_reader_create(nullptr, &reader) == VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_http_reader_io_interface_get(reader, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_http_reader_destroy(reader), VGL_RESULT_SUCCESS);
}

TEST_CASE(http_reader_reads_ranges)
{
    http_server server(server_mode::ranges);
    const test_pack pack = make_pack();
    const std::string uri = server.add(pack, "dem");
    test_allocator memory;
    {
        vgl_http_reader_create_info info{};
        info.struct_size = sizeof(info);
        info.allocator = &memory.table;
        info.max_connection_count = 4;
        const http_context c(&info, &memory.table);
        vgl_pack* p = add_pack(c.ctx, uri);
        vgl_pack* missing = add_pack(c.ctx, uri + "-missing");
        CHECK_EQ(wait_for(c.ctx, p), VGL_PACK_STATE_READY);
        vgl_result why = VGL_RESULT_SUCCESS;
        CHECK_EQ(wait_for(c.ctx, missing, &why), VGL_PACK_STATE_FAILED);
        CHECK_EQ(why, VGL_RESULT_NOT_FOUND);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_pack_destroy(missing), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(memory.live.load(), 0);
    CHECK(server.ranged_requests() > 0);
}

TEST_CASE(http_reader_keeps_the_range_of_whole_files)
{
    http_server server(server_mode::whole_files);
    const test_pack pack = make_pack();
    const std::string uri = server.add(pack, "dem");
    const http_context c;
    vgl_pack* p = add_pack(c.ctx, uri);
    CHECK_EQ(wait_for(c.ctx, p), VGL_PACK_STATE_READY);
    check_heights(c.ctx, pack);
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    CHECK_EQ(server.ranged_requests(), 0);
}

TEST_CASE(http_reader_hands_other_uris_to_the_file_reader)
{
    const test_pack pack = make_pack();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("vgl-http-" + std::to_string(std::random_device{}()));
    for (const auto& [relative, bytes] : pack.files)
    {
        const std::filesystem::path file = root / "dem" / std::filesystem::path(relative);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const std::string uri = (root / "dem").generic_string();
    {
        vgl_file_reader* files = nullptr;
        REQUIRE(vgl_file_reader_create(nullptr, &files) == VGL_RESULT_SUCCESS);
        vgl_io_interface fallback{};
        REQUIRE(vgl_file_reader_io_interface_get(files, &fallback) == VGL_RESULT_SUCCESS);
        vgl_http_reader_create_info info{};
        info.struct_size = sizeof(info);
        info.fallback = &fallback;
        {
            const http_context c(&info);
            vgl_pack* p = add_pack(c.ctx, uri);
            CHECK_EQ(wait_for(c.ctx, p), VGL_PACK_STATE_READY);
            check_heights(c.ctx, pack);
            CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        }
        CHECK_EQ(vgl_file_reader_destroy(files), VGL_RESULT_SUCCESS);
    }
    {
        // Without a file reader, such URIs are not found.
        const http_context c;
        vgl_pack* p = add_pack(c.ctx, uri);
        vgl_result why = VGL_RESULT_SUCCESS;
        CHECK_EQ(wait_for(c.ctx, p, &why), VGL_PACK_STATE_FAILED);
        CHECK_EQ(why, VGL_RESULT_NOT_FOUND);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE(http_reader_times_out)
{
    http_server server(server_mode::silent);
    const std::string uri = server.add(make_pack(), "dem");
    vgl_http_reader_create_info info{};
    info.struct_size = sizeof(info);
    info.timeout = 0.2;
    const http_context c(&info);
    vgl_pack* p = add_pack(c.ctx, uri);
    vgl_result why = VGL_RESULT_SUCCESS;
    CHECK_EQ(wait_for(c.ctx, p, &why), VGL_PACK_STATE_FAILED);
    CHECK_EQ(why, VGL_RESULT_IO_ERROR);
    CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
}

TEST_CASE(http_reader_cancels_reads_in_progress)
{
    http_server server(server_mode::silent);
    const std::string uri = server.add(make_pack(), "dem");
    test_allocator memory;
    {
        vgl_http_reader_create_info info{};
        info.struct_size = sizeof(info);
        info.allocator = &memory.table;
        const http_context c(&info, &memory.table);
        vgl_pack* p = add_pack(c.ctx, uri);
        for (int i = 0; i < 10; ++i)
        {
            CHECK_EQ(vgl_context_update(c.ctx, nullptr), VGL_RESULT_SUCCESS);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Destroying the pack and the context cancels the read the server never answers.
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(http_reader_behind_a_disk_cache_works_offline)
{
    const test_pack pack = make_pack();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("vgl-http-cache-" + std::to_string(std::random_device{}()));
    const std::string cache_dir = root.generic_string();
    std::string uri;
    for (int run = 0; run < 2; ++run)
    {
        // The first run reads through a server; the second has none, at the same address.
        std::unique_ptr<http_server> server;
        if (run == 0)
        {
            server = std::make_unique<http_server>(server_mode::ranges);
            uri = server->add(pack, "dem");
        }
        vgl_http_reader_create_info reader_info{};
        reader_info.struct_size = sizeof(reader_info);
        reader_info.timeout = 2.0;
        vgl_http_reader* reader = nullptr;
        REQUIRE(vgl_http_reader_create(&reader_info, &reader) == VGL_RESULT_SUCCESS);
        vgl_io_interface http{};
        REQUIRE(vgl_http_reader_io_interface_get(reader, &http) == VGL_RESULT_SUCCESS);
        vgl_disk_cache_create_info cache_info{};
        cache_info.struct_size = sizeof(cache_info);
        cache_info.directory = cache_dir.c_str();
        cache_info.source = &http;
        vgl_disk_cache* cache = nullptr;
        REQUIRE(vgl_disk_cache_create(&cache_info, &cache) == VGL_RESULT_SUCCESS);
        vgl_io_interface io{};
        REQUIRE(vgl_disk_cache_io_interface_get(cache, &io) == VGL_RESULT_SUCCESS);
        vgl_context_create_info info{};
        info.struct_size = sizeof(info);
        info.io = &io;
        vgl_context* ctx = nullptr;
        REQUIRE(vgl_context_create(&info, &ctx) == VGL_RESULT_SUCCESS);
        vgl_pack* p = add_pack(ctx, uri);
        CHECK_EQ(wait_for(ctx, p), VGL_PACK_STATE_READY);
        check_heights(ctx, pack);
        vgl_disk_cache_statistics st{};
        st.struct_size = sizeof(st);
        CHECK_EQ(vgl_disk_cache_statistics_get(cache, &st), VGL_RESULT_SUCCESS);
        CHECK_EQ(st.offline_hits, run == 0 ? 0u : 1u);
        CHECK(run == 0 ? st.hits == 0 : st.hits > 0);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_disk_cache_destroy(cache), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_http_reader_destroy(reader), VGL_RESULT_SUCCESS);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
