// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the HTTP reader (vgl/defaults.h), loading test packs from a small HTTP server that
// the test runs on the loopback address, so that it needs no network.

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

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
using socket_type = SOCKET;
using send_size = int;
constexpr socket_type no_socket = INVALID_SOCKET;
void close_socket(socket_type s)
{
    closesocket(s);
}
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>

#    include <unistd.h>
using socket_type = int;
using send_size = std::size_t;
constexpr socket_type no_socket = -1;
void close_socket(socket_type s)
{
    close(s);
}
#endif

namespace
{

using namespace vgl::test;

// How the server answers.
enum class server_mode
{
    ranges,      // Range requests get 206 and the range.
    whole_files, // Ranges are ignored: every request gets 200 and the whole file.
    silent,      // Connections are accepted and never answered.
};

// An HTTP/1.1 server for GET requests on 127.0.0.1, serving files from memory and closing each
// connection after one response. It handles one connection at a time.
class http_server
{
public:
    explicit http_server(server_mode mode) : mode_(mode)
    {
#if defined(_WIN32)
        WSADATA wsa{};
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != no_socket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(listen(listener_, 64) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;

    ~http_server()
    {
        stop_ = true;
        thread_.join();
        for (const socket_type s : held_)
        {
            close_socket(s);
        }
        close_socket(listener_);
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    // Serves a pack's files under /<name>/ and returns its URI.
    std::string add(const test_pack& p, const std::string& name)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [relative, bytes] : p.files)
        {
            files_["/" + name + "/" + relative] = bytes;
        }
        return "http://127.0.0.1:" + std::to_string(port_) + "/" + name;
    }

    [[nodiscard]] int requests() const { return requests_.load(); }
    [[nodiscard]] int ranged_requests() const { return ranged_.load(); }

private:
    void serve()
    {
        while (!stop_)
        {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener_, &ready);
            timeval wait{0, 20000};
            if (select(static_cast<int>(listener_ + 1), &ready, nullptr, nullptr, &wait) <= 0)
            {
                continue;
            }
            const socket_type client = accept(listener_, nullptr, nullptr);
            if (client == no_socket)
            {
                continue;
            }
            if (mode_ == server_mode::silent)
            {
                held_.push_back(client);
                continue;
            }
            answer(client);
            close_socket(client);
        }
    }

    static bool send_all(socket_type s, const char* data, std::size_t size)
    {
        while (size != 0)
        {
            const auto chunk = static_cast<send_size>(size < 65536 ? size : 65536);
            const auto sent = send(s, data, chunk, 0);
            if (sent <= 0)
            {
                return false;
            }
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    void answer(socket_type s)
    {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            const auto got = recv(s, buffer, sizeof(buffer), 0);
            if (got <= 0)
            {
                return;
            }
            request.append(buffer, static_cast<std::size_t>(got));
        }
        ++requests_;
        const std::size_t path_start = request.find(' ') + 1;
        const std::string path =
            request.substr(path_start, request.find(' ', path_start) - path_start);
        std::string lower = request;
        for (char& c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::size_t range_at = lower.find("\r\nrange: bytes=");
        const std::vector<std::byte>* file = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto found = files_.find(path);
            file = found == files_.end() ? nullptr : &found->second;
        }
        std::string head;
        std::size_t first = 0;
        std::size_t count = 0;
        if (file == nullptr)
        {
            head =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found";
        }
        else if (range_at == std::string::npos || mode_ == server_mode::whole_files)
        {
            count = file->size();
            head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(count) +
                   "\r\nConnection: close\r\n\r\n";
        }
        else
        {
            ++ranged_;
            // "first-last" or "first-".
            const char* spec = request.c_str() + range_at + 15;
            char* end = nullptr;
            const unsigned long long a = std::strtoull(spec, &end, 10);
            const bool valid = end != spec && *end == '-';
            const char* last_spec = valid ? end + 1 : end;
            const unsigned long long b = std::strtoull(last_spec, &end, 10);
            const int fields = !valid ? 0 : end != last_spec ? 2 : 1;
            const std::size_t size = file->size();
            if (fields < 1 || a >= size)
            {
                head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                       std::to_string(size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            else
            {
                const std::size_t last =
                    fields == 2 && b < size ? static_cast<std::size_t>(b) : size - 1;
                first = static_cast<std::size_t>(a);
                count = last - first + 1;
                head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " +
                       std::to_string(first) + "-" + std::to_string(last) + "/" +
                       std::to_string(size) + "\r\nContent-Length: " + std::to_string(count) +
                       "\r\nConnection: close\r\n\r\n";
            }
        }
        if (send_all(s, head.data(), head.size()) && count != 0)
        {
            static_cast<void>(
                send_all(s, reinterpret_cast<const char*>(file->data() + first), count));
        }
    }

    server_mode mode_;
    socket_type listener_ = no_socket;
    unsigned short port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::atomic<int> ranged_{0};
    std::mutex mutex_;
    std::map<std::string, std::vector<std::byte>> files_;
    std::vector<socket_type> held_;
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
        info.max_connections = 4;
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
        vgl_http_reader_create_info info{};
        info.struct_size = sizeof(info);
        info.files = files;
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
