// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/defaults.h (docs/c-api.md, section 16). Link vgl::defaults.

#ifndef VGL_DEFAULTS_HPP
#define VGL_DEFAULTS_HPP

#include <vgl/base.hpp>
#include <vgl/defaults.h>

#include <cassert>
#include <cstdint>
#include <utility>

namespace vgl
{

// Owns a vgl_thread_pool.
class thread_pool
{
public:
    thread_pool() noexcept = default;
    explicit thread_pool(vgl_thread_pool* handle) noexcept : handle_(handle) {}
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&& other) noexcept : handle_(other.release()) {}
    thread_pool& operator=(thread_pool&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~thread_pool() { reset(); }

    // info may be null for all defaults.
    [[nodiscard]] static expected<thread_pool>
    create(const vgl_thread_pool_create_info* info = nullptr) noexcept
    {
        vgl_thread_pool* handle = nullptr;
        const auto r = static_cast<result>(vgl_thread_pool_create(info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return thread_pool(handle);
    }

    [[nodiscard]] vgl_thread_pool* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_thread_pool* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_thread_pool* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_thread_pool_destroy(std::exchange(handle_, handle)));
    }

    // The pool must be valid.
    [[nodiscard]] vgl_task_interface task_interface_get() const noexcept
    {
        vgl_task_interface table{};
        assert(handle_ != nullptr);
        static_cast<void>(vgl_thread_pool_task_interface_get(handle_, &table));
        return table;
    }

private:
    vgl_thread_pool* handle_ = nullptr;
};

// Owns a vgl_file_reader.
class file_reader
{
public:
    file_reader() noexcept = default;
    explicit file_reader(vgl_file_reader* handle) noexcept : handle_(handle) {}
    file_reader(const file_reader&) = delete;
    file_reader& operator=(const file_reader&) = delete;
    file_reader(file_reader&& other) noexcept : handle_(other.release()) {}
    file_reader& operator=(file_reader&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~file_reader() { reset(); }

    // info may be null for all defaults.
    [[nodiscard]] static expected<file_reader>
    create(const vgl_file_reader_create_info* info = nullptr) noexcept
    {
        vgl_file_reader* handle = nullptr;
        const auto r = static_cast<result>(vgl_file_reader_create(info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return file_reader(handle);
    }

    // Reads on a pool's threads.
    [[nodiscard]] static expected<file_reader> create(thread_pool& pool) noexcept
    {
        auto info = init<vgl_file_reader_create_info>();
        info.pool = pool.get();
        return create(&info);
    }

    [[nodiscard]] vgl_file_reader* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_file_reader* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_file_reader* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_file_reader_destroy(std::exchange(handle_, handle)));
    }

    // The reader must be valid.
    [[nodiscard]] vgl_io_interface io_interface_get() const noexcept
    {
        vgl_io_interface table{};
        assert(handle_ != nullptr);
        static_cast<void>(vgl_file_reader_io_interface_get(handle_, &table));
        return table;
    }

private:
    vgl_file_reader* handle_ = nullptr;
};

// Owns a vgl_http_reader.
class http_reader
{
public:
    http_reader() noexcept = default;
    explicit http_reader(vgl_http_reader* handle) noexcept : handle_(handle) {}
    http_reader(const http_reader&) = delete;
    http_reader& operator=(const http_reader&) = delete;
    http_reader(http_reader&& other) noexcept : handle_(other.release()) {}
    http_reader& operator=(http_reader&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~http_reader() { reset(); }

    // info may be null for all defaults.
    [[nodiscard]] static expected<http_reader>
    create(const vgl_http_reader_create_info* info = nullptr) noexcept
    {
        vgl_http_reader* handle = nullptr;
        const auto r = static_cast<result>(vgl_http_reader_create(info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return http_reader(handle);
    }

    // Reads the URIs that are not HTTP through another interface, such as a file reader's,
    // whose object must outlive this one.
    [[nodiscard]] static expected<http_reader> create(const vgl_io_interface& fallback) noexcept
    {
        auto info = init<vgl_http_reader_create_info>();
        info.fallback = &fallback;
        return create(&info);
    }

    // Reads the URIs that are not HTTP with a file reader, which must outlive this one.
    [[nodiscard]] static expected<http_reader> create(const file_reader& files) noexcept
    {
        const vgl_io_interface fallback = files.io_interface_get();
        return create(fallback);
    }

    [[nodiscard]] vgl_http_reader* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_http_reader* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_http_reader* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_http_reader_destroy(std::exchange(handle_, handle)));
    }

    // The reader must be valid.
    [[nodiscard]] vgl_io_interface io_interface_get() const noexcept
    {
        vgl_io_interface table{};
        assert(handle_ != nullptr);
        static_cast<void>(vgl_http_reader_io_interface_get(handle_, &table));
        return table;
    }

private:
    vgl_http_reader* handle_ = nullptr;
};

// Owns a vgl_disk_cache.
class disk_cache
{
public:
    disk_cache() noexcept = default;
    explicit disk_cache(vgl_disk_cache* handle) noexcept : handle_(handle) {}
    disk_cache(const disk_cache&) = delete;
    disk_cache& operator=(const disk_cache&) = delete;
    disk_cache(disk_cache&& other) noexcept : handle_(other.release()) {}
    disk_cache& operator=(disk_cache&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~disk_cache() { reset(); }

    [[nodiscard]] static expected<disk_cache>
    create(const vgl_disk_cache_create_info& info) noexcept
    {
        vgl_disk_cache* handle = nullptr;
        const auto r = static_cast<result>(vgl_disk_cache_create(&info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return disk_cache(handle);
    }

    // Caches what source reads in directory, within budget bytes (0 for 1 GiB), with disk work
    // on pool if it is not null. source's object must outlive the cache and its reads.
    [[nodiscard]] static expected<disk_cache> create(const char* directory,
                                                     const vgl_io_interface& source,
                                                     std::uint64_t budget = 0,
                                                     thread_pool* pool = nullptr) noexcept
    {
        auto info = init<vgl_disk_cache_create_info>();
        info.directory = directory;
        info.source = &source;
        info.budget = budget;
        info.pool = pool != nullptr ? pool->get() : nullptr;
        return create(info);
    }

    [[nodiscard]] vgl_disk_cache* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_disk_cache* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_disk_cache* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_disk_cache_destroy(std::exchange(handle_, handle)));
    }

    // The cache must be valid.
    [[nodiscard]] vgl_io_interface io_interface_get() const noexcept
    {
        vgl_io_interface table{};
        assert(handle_ != nullptr);
        static_cast<void>(vgl_disk_cache_io_interface_get(handle_, &table));
        return table;
    }

    // The cache must be valid.
    [[nodiscard]] vgl_disk_cache_statistics statistics_get() const noexcept
    {
        auto s = init<vgl_disk_cache_statistics>();
        assert(handle_ != nullptr);
        static_cast<void>(vgl_disk_cache_statistics_get(handle_, &s));
        return s;
    }

    [[nodiscard]] result clear() const noexcept
    {
        return static_cast<result>(vgl_disk_cache_clear(handle_));
    }

private:
    vgl_disk_cache* handle_ = nullptr;
};

} // namespace vgl

#endif // VGL_DEFAULTS_HPP
