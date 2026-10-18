// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the defaults module (vgl/defaults.h): the thread pool, the file reader with and
// without it, loading test packs written to a temporary directory, and the disk cache in front
// of a source in memory.

#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <vgl/context.h>
#include <vgl/defaults.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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

    // A path in the directory, UTF-8.
    [[nodiscard]] std::string path(const std::string& name) const
    {
        const std::u8string u8 = (path_ / name).generic_u8string();
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

fs::path utf8_path(const std::string& s)
{
    return {std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())};
}

// A source that is offline: every read fails as an unreachable server's would.
struct offline_io
{
    int reads = 0;
    vgl_io_interface table{VGL_IO_INTERFACE_VERSION, 0, this, &read, nullptr, nullptr};

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* /*r*/)
    {
        ++static_cast<offline_io*>(user_data)->reads;
        static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_IO_ERROR, nullptr, 0));
    }
};

// A context reading through a disk cache in front of source.
struct cached_context
{
    cached_context(const std::string& directory,
                   const vgl_io_interface& source,
                   test_allocator& memory,
                   std::uint64_t budget = 0,
                   vgl_thread_pool* pool = nullptr)
    {
        vgl_disk_cache_create_info info{};
        info.struct_size = sizeof(info);
        info.directory = directory.c_str();
        info.source = &source;
        info.budget = budget;
        info.pool = pool;
        info.allocator = &memory.table;
        REQUIRE(vgl_disk_cache_create(&info, &cache) == VGL_RESULT_SUCCESS);
        REQUIRE(vgl_disk_cache_io_interface_get(cache, &io) == VGL_RESULT_SUCCESS);
        vgl_context_create_info context_info{};
        context_info.struct_size = sizeof(context_info);
        context_info.allocator = &memory.table;
        context_info.io = &io;
        REQUIRE(vgl_context_create(&context_info, &ctx) == VGL_RESULT_SUCCESS);
    }

    cached_context(const cached_context&) = delete;
    cached_context& operator=(const cached_context&) = delete;

    ~cached_context()
    {
        CHECK_EQ(vgl_context_destroy(ctx), VGL_RESULT_SUCCESS);
        CHECK_EQ(vgl_disk_cache_destroy(cache), VGL_RESULT_SUCCESS);
    }

    [[nodiscard]] vgl_disk_cache_statistics statistics() const
    {
        vgl_disk_cache_statistics st{};
        st.struct_size = sizeof(st);
        CHECK_EQ(vgl_disk_cache_statistics_get(cache, &st), VGL_RESULT_SUCCESS);
        return st;
    }

    vgl_disk_cache* cache = nullptr;
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

std::size_t count_of(const std::vector<std::string>& uris, const char* suffix)
{
    return static_cast<std::size_t>(
        std::count_if(uris.begin(), uris.end(), [&](const std::string& u) {
            return u.size() >= std::strlen(suffix) &&
                   u.compare(u.size() - std::strlen(suffix), std::string::npos, suffix) == 0;
        }));
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

TEST_CASE(disk_cache_serves_what_it_stored_and_works_offline)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string uri = "http://packs.test/dem";
    const std::string cache_dir = dir.path("cache-Ã©");
    test_allocator memory;
    std::size_t first_reads = 0;
    {
        // Online, from an empty cache: everything comes from the source.
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        first_reads = server.uris.size();
        CHECK(first_reads > 1);
        const vgl_disk_cache_statistics st = c.statistics();
        CHECK_EQ(st.hits, 0u);
        CHECK_EQ(st.misses, static_cast<std::uint64_t>(first_reads - 1)); // All but the manifest.
        CHECK_EQ(st.entries, static_cast<std::uint64_t>(first_reads));
        CHECK(st.bytes > 0);
        CHECK_EQ(st.budget, 1ull << 30); // The default.
    }
    {
        // Online again, with a new cache on the same directory: only the manifest goes to the
        // server, which is asked first.
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        CHECK_EQ(c.statistics().entries, static_cast<std::uint64_t>(first_reads));
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(server.uris.size(), 1u);
        CHECK_EQ(count_of(server.uris, "/pack.vglm"), 1u);
        const vgl_disk_cache_statistics st = c.statistics();
        CHECK_EQ(st.hits, static_cast<std::uint64_t>(first_reads - 1));
        CHECK_EQ(st.misses, 0u);
        CHECK_EQ(st.offline_hits, 0u);
    }
    {
        // Offline: the manifest too comes from the disk.
        offline_io server;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(server.reads, 1); // The manifest was tried.
        const vgl_disk_cache_statistics st = c.statistics();
        CHECK_EQ(st.offline_hits, 1u);
        CHECK_EQ(st.hits, static_cast<std::uint64_t>(first_reads - 1));
        // A pack the cache has never seen fails as the source does.
        vgl_pack* unseen = add_pack(c.ctx, "http://packs.test/unseen");
        vgl_pack_state state{};
        vgl_result why = VGL_RESULT_SUCCESS;
        for (int i = 0; i < 100; ++i)
        {
            CHECK_EQ(vgl_context_update(c.ctx, nullptr), VGL_RESULT_SUCCESS);
        }
        CHECK_EQ(vgl_pack_state_get(unseen, &state, &why), VGL_RESULT_SUCCESS);
        CHECK_EQ(state, VGL_PACK_STATE_FAILED);
        CHECK_EQ(why, VGL_RESULT_IO_ERROR);
        CHECK_EQ(vgl_pack_destroy(unseen), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(disk_cache_reads_damaged_entries_again)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string uri = "https://packs.test/dem";
    const std::string cache_dir = dir.path("cache");
    test_allocator memory;
    {
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
    }
    // Damage every entry but leave its size, and add a file left half-written.
    std::size_t damaged = 0;
    for (const auto& e : fs::recursive_directory_iterator(utf8_path(cache_dir)))
    {
        if (e.is_regular_file() && e.path().extension() == ".vglc")
        {
            std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(-1, std::ios::end);
            f.put('\x5A');
            ++damaged;
        }
    }
    CHECK(damaged > 0);
    {
        std::ofstream(utf8_path(cache_dir) / "00" / "0000000000000000.1.tmp") << "partial";
    }
    {
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        // Every damaged shard read went to the server again.
        CHECK_EQ(server.uris.size(), damaged);
        CHECK_EQ(c.statistics().hits, 0u);
    }
    CHECK(!fs::exists(utf8_path(cache_dir) / "00" / "0000000000000000.1.tmp"));
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(disk_cache_keeps_to_its_budget_and_clears)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string uri = "http://packs.test/dem";
    const std::string cache_dir = dir.path("cache");
    test_allocator memory;
    std::uint64_t total = 0;
    {
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        total = c.statistics().bytes;
        CHECK_EQ(vgl_disk_cache_clear(c.cache), VGL_RESULT_SUCCESS);
        CHECK_EQ(c.statistics().entries, 0u);
        CHECK_EQ(c.statistics().bytes, 0u);
    }
    {
        // Half of what the pack took: the oldest entries go, and what stays fits.
        memory_io server;
        server.packs[uri] = &pack;
        const cached_context c(cache_dir, server.table, memory, total / 2);
        CHECK_EQ(c.statistics().entries, 0u); // The clear removed the files.
        vgl_pack* p = add_pack(c.ctx, uri);
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        const vgl_disk_cache_statistics st = c.statistics();
        CHECK_EQ(st.budget, total / 2);
        CHECK(st.evictions > 0);
        CHECK(st.bytes <= total / 2);
        CHECK(st.entries > 0);
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(disk_cache_passes_other_uris_on_and_outlives_its_reads)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string cache_dir = dir.path("cache");
    test_allocator memory;
    {
        // Not HTTP: nothing is stored.
        memory_io server;
        server.packs["mem://dem"] = &pack;
        const cached_context c(cache_dir, server.table, memory);
        vgl_pack* p = add_pack(c.ctx, "mem://dem");
        check_heights(c.ctx, pack);
        CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        CHECK_EQ(c.statistics().entries, 0u);
        CHECK_EQ(c.statistics().misses, 0u);
    }
    {
        // Reads still at the source when the context and the cache go: they complete later,
        // and nothing leaks.
        memory_io server;
        server.packs["http://packs.test/dem"] = &pack;
        server.deferred = true;
        {
            const cached_context c(cache_dir, server.table, memory);
            vgl_pack* p = add_pack(c.ctx, "http://packs.test/dem");
            CHECK_EQ(vgl_context_update(c.ctx, nullptr), VGL_RESULT_SUCCESS);
            server.complete_all(); // The manifest, so that shard reads start.
            for (int i = 0; i < 3; ++i)
            {
                CHECK_EQ(vgl_context_update(c.ctx, nullptr), VGL_RESULT_SUCCESS);
            }
            const position at = inside({3, 6, 3});
            double h = 0.0;
            static_cast<void>(vgl_context_height_query(
                c.ctx, to_radians(at.lat), to_radians(at.lon), VGL_HEIGHT_REFERENCE_ELLIPSOID, &h));
            CHECK(!server.queued.empty());
            CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
        }
        server.complete_all();
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(disk_cache_on_a_pool)
{
    const temporary_directory dir;
    const test_pack pack = make_pack();
    const std::string uri = "http://packs.test/dem";
    const std::string cache_dir = dir.path("cache");
    test_allocator memory;
    {
        vgl_thread_pool_create_info pool_info{};
        pool_info.struct_size = sizeof(pool_info);
        pool_info.thread_count = 2;
        pool_info.allocator = &memory.table;
        vgl_thread_pool* pool = nullptr;
        REQUIRE(vgl_thread_pool_create(&pool_info, &pool) == VGL_RESULT_SUCCESS);
        for (int run = 0; run < 2; ++run)
        {
            memory_io server;
            server.packs[uri] = &pack;
            const cached_context c(cache_dir, server.table, memory, 0, pool);
            vgl_pack* p = add_pack(c.ctx, uri);
            check_heights(c.ctx, pack);
            CHECK_EQ(vgl_pack_destroy(p), VGL_RESULT_SUCCESS);
            if (run == 1)
            {
                CHECK_EQ(server.uris.size(), 1u); // The manifest only.
            }
        }
        // Destroying the pool runs its queued work, the last stores among it.
        CHECK_EQ(vgl_thread_pool_destroy(pool), VGL_RESULT_SUCCESS);
    }
    CHECK_EQ(memory.live.load(), 0);
}

TEST_CASE(disk_cache_arguments)
{
    const temporary_directory dir;
    const std::string cache_dir = dir.path("cache");
    offline_io server;
    vgl_disk_cache* cache = nullptr;
    vgl_disk_cache_create_info info{};
    info.struct_size = sizeof(info);
    CHECK_EQ(vgl_disk_cache_create(&info, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_disk_cache_create(nullptr, &cache), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_disk_cache_create(&info, &cache), VGL_RESULT_INVALID_ARGUMENT); // No directory.
    info.directory = cache_dir.c_str();
    CHECK_EQ(vgl_disk_cache_create(&info, &cache), VGL_RESULT_INVALID_ARGUMENT); // No source.
    vgl_io_interface future = server.table;
    future.version = VGL_IO_INTERFACE_VERSION + 1;
    info.source = &future;
    CHECK_EQ(vgl_disk_cache_create(&info, &cache), VGL_RESULT_VERSION_MISMATCH);
    info.source = &server.table;
    REQUIRE(vgl_disk_cache_create(&info, &cache) == VGL_RESULT_SUCCESS);
    vgl_disk_cache_statistics st{};
    st.struct_size = 4;
    CHECK_EQ(vgl_disk_cache_statistics_get(cache, &st), VGL_RESULT_INVALID_ARGUMENT);
    st.struct_size = 16; // An older, smaller structure gets what fits.
    CHECK_EQ(vgl_disk_cache_statistics_get(cache, &st), VGL_RESULT_SUCCESS);
    CHECK_EQ(st.struct_size, 16u);
    CHECK_EQ(vgl_disk_cache_io_interface_get(cache, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_disk_cache_clear(nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_disk_cache_destroy(cache), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_disk_cache_destroy(nullptr), VGL_RESULT_SUCCESS);
    CHECK(fs::is_directory(utf8_path(cache_dir)));
}

namespace
{

// Serves any range of any URI, with bytes that depend on the offset.
struct pattern_io
{
    vgl_io_interface table{VGL_IO_INTERFACE_VERSION, 0, this, &read, nullptr, nullptr};

    static void read(void* /*user_data*/, vgl_io_request* request, const vgl_io_read* r)
    {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(r->size));
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<unsigned char>(r->offset + i);
        }
        static_cast<void>(
            vgl_io_request_complete(request, VGL_RESULT_SUCCESS, bytes.data(), bytes.size()));
    }
};

// Reads a range through an interface, as a context would, and waits for it.
vgl_result
read_through(const vgl_io_interface& io, const char* uri, std::uint64_t offset, std::uint64_t size)
{
    struct result_box
    {
        std::atomic<int> done{0};
        vgl_result result = VGL_RESULT_SUCCESS;
    } box;
    vgl_io_request_create_info info{};
    info.struct_size = sizeof(info);
    info.user_data = &box;
    info.complete = [](void* user_data, vgl_result r, const void*, std::size_t) {
        auto* b = static_cast<result_box*>(user_data);
        b->result = r;
        b->done = 1;
    };
    vgl_io_request* request = nullptr;
    if (const vgl_result r = vgl_io_request_create(&info, &request); r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    vgl_io_read read{};
    read.uri = uri;
    read.offset = offset;
    read.size = size;
    io.read(io.user_data, request, &read);
    while (box.done.load() == 0)
    {
        std::this_thread::yield();
    }
    return box.result;
}

} // namespace

TEST_CASE(disk_cache_evicts_without_holding_up_its_callers)
{
    const temporary_directory dir;
    const std::string cache_dir = dir.path("cache");
    constexpr std::uint64_t entries = 3000;
    constexpr std::uint64_t entry_size = 4096;
    pattern_io source;
    {
        // Fill the cache, on this thread.
        vgl_disk_cache_create_info info{};
        info.struct_size = sizeof(info);
        info.directory = cache_dir.c_str();
        info.source = &source.table;
        vgl_disk_cache* cache = nullptr;
        REQUIRE(vgl_disk_cache_create(&info, &cache) == VGL_RESULT_SUCCESS);
        vgl_io_interface io{};
        REQUIRE(vgl_disk_cache_io_interface_get(cache, &io) == VGL_RESULT_SUCCESS);
        for (std::uint64_t i = 0; i < entries; ++i)
        {
            CHECK_EQ(read_through(io, "http://packs.test/a.vgls", i * entry_size, entry_size),
                     VGL_RESULT_SUCCESS);
        }
        CHECK_EQ(vgl_disk_cache_destroy(cache), VGL_RESULT_SUCCESS);
    }
    // Reopen it with a budget for a tenth of that: the next store, on the pool, evicts most of
    // the entries, while this thread keeps asking for the statistics, as a sample's panel does
    // every frame. None of the calls may wait for the eviction.
    vgl_thread_pool_create_info pool_info{};
    pool_info.struct_size = sizeof(pool_info);
    pool_info.thread_count = 1;
    vgl_thread_pool* pool = nullptr;
    REQUIRE(vgl_thread_pool_create(&pool_info, &pool) == VGL_RESULT_SUCCESS);
    vgl_disk_cache_create_info info{};
    info.struct_size = sizeof(info);
    info.directory = cache_dir.c_str();
    info.source = &source.table;
    info.budget = entries * entry_size / 10;
    info.pool = pool;
    vgl_disk_cache* cache = nullptr;
    REQUIRE(vgl_disk_cache_create(&info, &cache) == VGL_RESULT_SUCCESS);
    vgl_io_interface io{};
    REQUIRE(vgl_disk_cache_io_interface_get(cache, &io) == VGL_RESULT_SUCCESS);
    CHECK_EQ(read_through(io, "http://packs.test/b.vgls", 0, entry_size), VGL_RESULT_SUCCESS);
    double longest = 0.0;
    const auto start = std::chrono::steady_clock::now();
    vgl_disk_cache_statistics st{};
    st.struct_size = sizeof(st);
    do
    {
        const auto before = std::chrono::steady_clock::now();
        CHECK_EQ(vgl_disk_cache_statistics_get(cache, &st), VGL_RESULT_SUCCESS);
        const auto after = std::chrono::steady_clock::now();
        longest = std::max(longest, std::chrono::duration<double>(after - before).count());
    } while (st.bytes > info.budget &&
             std::chrono::steady_clock::now() - start < std::chrono::seconds(60));
    std::printf("evicted %llu entries; the longest statistics call took %.1f ms\n",
                static_cast<unsigned long long>(st.evictions),
                longest * 1000.0);
    CHECK(st.bytes <= info.budget);
    CHECK(st.evictions > entries / 2);
    // A frame is 16 ms; an eviction of thousands of files takes far longer.
    CHECK(longest < 0.1);
    CHECK_EQ(vgl_disk_cache_destroy(cache), VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_thread_pool_destroy(pool), VGL_RESULT_SUCCESS);
}
