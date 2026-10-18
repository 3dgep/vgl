// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// A small HTTP/1.1 server on the loopback address, serving test packs from memory, for the
// tests of what reads packs over HTTP: the HTTP reader and the baker's check of hosted packs.
// Link ws2_32 on Windows.

#ifndef VGL_TESTS_HTTP_SERVER_HPP
#define VGL_TESTS_HTTP_SERVER_HPP

#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <mutex>
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
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>

#    include <unistd.h>
#endif

namespace vgl::test
{

#if defined(_WIN32)
using socket_type = SOCKET;
using send_size = int;
constexpr socket_type no_socket = INVALID_SOCKET;
inline void close_socket(socket_type s)
{
    closesocket(s);
}
#else
using socket_type = int;
using send_size = std::size_t;
constexpr socket_type no_socket = -1;
inline void close_socket(socket_type s)
{
    close(s);
}
#endif

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

} // namespace vgl::test

#endif // VGL_TESTS_HTTP_SERVER_HPP
