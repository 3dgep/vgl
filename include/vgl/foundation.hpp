// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/foundation.h. An allocator written as a C++ class is adapted onto the C
// vgl_allocator_interface table (docs/c-api.md, sections 9.2 and 16).

#ifndef VGL_FOUNDATION_HPP
#define VGL_FOUNDATION_HPP

#include <vgl/base.hpp>
#include <vgl/foundation.h>

#include <cstddef>
#include <cstdint>

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

} // namespace vgl

#endif // VGL_FOUNDATION_HPP
