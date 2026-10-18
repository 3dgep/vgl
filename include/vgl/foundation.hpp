// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/foundation.h. Allocators, I/O and task scheduling written as C++ classes
// are adapted onto the C interface tables (docs/c-api.md, sections 9.2 and 16).

#ifndef VGL_FOUNDATION_HPP
#define VGL_FOUNDATION_HPP

#include <vgl/base.hpp>
#include <vgl/foundation.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace vgl
{

enum class log_level : std::int32_t
{
    default_level = VGL_LOG_LEVEL_DEFAULT,
    error = VGL_LOG_LEVEL_ERROR,
    warning = VGL_LOG_LEVEL_WARNING,
    info = VGL_LOG_LEVEL_INFO,
    debug = VGL_LOG_LEVEL_DEBUG,
};

// Base class for host allocators. Derive from it and pass c_interface() wherever the C API
// takes a vgl_allocator_interface. The object must outlive every use of the interface.
class allocator
{
public:
    allocator() = default;
    allocator(const allocator&) = delete;
    allocator& operator=(const allocator&) = delete;
    allocator(allocator&&) = delete;
    allocator& operator=(allocator&&) = delete;
    virtual ~allocator() = default;

    // Allocates size bytes (never 0) aligned to alignment, a power of two. Returns null on
    // failure. Must not throw.
    virtual void* allocate(std::size_t size, std::size_t alignment) noexcept = 0;

    // Frees memory from allocate, with the same size and alignment. memory is never null.
    virtual void deallocate(void* memory, std::size_t size, std::size_t alignment) noexcept = 0;

    // C table whose entries forward to this object.
    [[nodiscard]] vgl_allocator_interface c_interface() noexcept
    {
        vgl_allocator_interface table{};
        table.version = VGL_ALLOCATOR_INTERFACE_VERSION;
        table.user_data = this;
        table.allocate = &allocate_thunk;
        table.deallocate = &deallocate_thunk;
        return table;
    }

private:
    static void* allocate_thunk(void* user_data, std::size_t size, std::size_t alignment)
    {
        return static_cast<allocator*>(user_data)->allocate(size, alignment);
    }

    static void
    deallocate_thunk(void* user_data, void* memory, std::size_t size, std::size_t alignment)
    {
        static_cast<allocator*>(user_data)->deallocate(memory, size, alignment);
    }
};

using io_request = vgl_io_request;
using io_read = vgl_io_read;

inline constexpr std::uint64_t io_size_to_end = VGL_IO_SIZE_TO_END;

// Completes an I/O request with the bytes read, which are copied. Free-threaded.
[[nodiscard]] inline result
io_request_complete(io_request* request, result r, std::span<const std::byte> data = {}) noexcept
{
    return static_cast<result>(
        vgl_io_request_complete(request, static_cast<vgl_result>(r), data.data(), data.size()));
}

using io_complete_fn = vgl_io_complete_fn;

// Creates a request whose completion calls complete(user_data, result, data, size), for passing
// a read on to another I/O interface. alloc may be null for the C heap.
[[nodiscard]] inline expected<io_request*>
io_request_create(io_complete_fn complete,
                  void* user_data,
                  const vgl_allocator_interface* alloc = nullptr) noexcept
{
    auto info = init<vgl_io_request_create_info>();
    info.complete = complete;
    info.user_data = user_data;
    info.allocator = alloc;
    io_request* request = nullptr;
    const auto r = static_cast<result>(vgl_io_request_create(&info, &request));
    if (failed(r))
    {
        return unexpected(r);
    }
    return request;
}

// Base class for host I/O. Derive from it and pass c_interface() as
// vgl_context_create_info::io. The object must outlive every context that uses it.
class io_interface
{
public:
    io_interface() = default;
    io_interface(const io_interface&) = delete;
    io_interface& operator=(const io_interface&) = delete;
    io_interface(io_interface&&) = delete;
    io_interface& operator=(io_interface&&) = delete;
    virtual ~io_interface() = default;

    // Starts a read, which must be completed exactly once with io_request_complete. Called on
    // the thread that calls into the context. Must not throw.
    virtual void read(io_request* request, const io_read& read) noexcept = 0;

    // Asks to give up a read; it must still be completed. Called on the context's thread.
    virtual void cancel(io_request* /*request*/) noexcept {}

    // Called once while a context that uses this object is destroyed, on the thread that
    // destroys it. Reads may still be outstanding; they must still be completed.
    virtual void on_destroy() noexcept {}

    // C table whose entries forward to this object.
    [[nodiscard]] vgl_io_interface c_interface() noexcept
    {
        vgl_io_interface table{};
        table.version = VGL_IO_INTERFACE_VERSION;
        table.user_data = this;
        table.read = &read_thunk;
        table.cancel = &cancel_thunk;
        table.destroy = &destroy_thunk;
        return table;
    }

private:
    static void read_thunk(void* user_data, vgl_io_request* request, const vgl_io_read* read)
    {
        static_cast<io_interface*>(user_data)->read(request, *read);
    }

    static void cancel_thunk(void* user_data, vgl_io_request* request)
    {
        static_cast<io_interface*>(user_data)->cancel(request);
    }

    static void destroy_thunk(void* user_data)
    {
        static_cast<io_interface*>(user_data)->on_destroy();
    }
};

using task_fn = vgl_task_fn;

// Base class for host task scheduling. Derive from it and pass c_interface() as
// vgl_context_create_info::tasks. The object must outlive every context that uses it.
class task_interface
{
public:
    task_interface() = default;
    task_interface(const task_interface&) = delete;
    task_interface& operator=(const task_interface&) = delete;
    task_interface(task_interface&&) = delete;
    task_interface& operator=(task_interface&&) = delete;
    virtual ~task_interface() = default;

    // Runs function(task_data) exactly once, on any thread, and returns result::success; or
    // returns another result and never calls it. Called on the context's thread. Must not
    // throw.
    virtual result submit(task_fn function, void* task_data) noexcept = 0;

    // Called once while a context that uses this object is destroyed, on the thread that
    // destroys it. Submitted tasks may still be queued; they must still run.
    virtual void on_destroy() noexcept {}

    // C table whose entries forward to this object.
    [[nodiscard]] vgl_task_interface c_interface() noexcept
    {
        vgl_task_interface table{};
        table.version = VGL_TASK_INTERFACE_VERSION;
        table.user_data = this;
        table.submit = &submit_thunk;
        table.destroy = &destroy_thunk;
        return table;
    }

private:
    static vgl_result submit_thunk(void* user_data, vgl_task_fn function, void* task_data)
    {
        return static_cast<vgl_result>(
            static_cast<task_interface*>(user_data)->submit(function, task_data));
    }

    static void destroy_thunk(void* user_data)
    {
        static_cast<task_interface*>(user_data)->on_destroy();
    }
};

} // namespace vgl

#endif // VGL_FOUNDATION_HPP
