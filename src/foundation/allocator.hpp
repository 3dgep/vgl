// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Allocator used for every allocation the library makes (NFR-03). It wraps a host
// vgl_allocator_interface, or the C runtime heap when the host supplies none.

#ifndef VGL_FOUNDATION_ALLOCATOR_HPP
#define VGL_FOUNDATION_ALLOCATOR_HPP

#include <vgl/foundation.h>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace vgl::internal
{

class allocator
{
public:
    // Uses the C runtime heap.
    allocator() noexcept;

    // Uses a host allocator. The interface must have passed validate_allocator_interface.
    explicit allocator(const vgl_allocator_interface& host) noexcept;

    // Returns null on failure, or when size is 0. alignment must be a power of two.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) const noexcept;

    // memory may be null. size and alignment must match the allocation.
    void deallocate(void* memory, std::size_t size, std::size_t alignment) const noexcept;

    // The interface it allocates through: the host's, or the C heap's.
    [[nodiscard]] const vgl_allocator_interface& host_interface() const noexcept
    {
        return interface_;
    }

    // Allocates and constructs a T. Returns null if allocation fails.
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) const noexcept
    {
        static_assert(std::is_nothrow_constructible_v<T, Args...>,
                      "library objects must be constructible without exceptions");
        void* memory = allocate(sizeof(T), alignof(T));
        if (memory == nullptr)
        {
            return nullptr;
        }
        return ::new (memory) T(std::forward<Args>(args)...);
    }

    // Destroys and frees an object made by create. object may be null.
    template <typename T>
    void destroy(T* object) const noexcept
    {
        if (object == nullptr)
        {
            return;
        }
        object->~T();
        deallocate(object, sizeof(T), alignof(T));
    }

private:
    vgl_allocator_interface interface_;
};

// Checks a host allocator interface (docs/c-api.md, section 9.2). Returns
// VGL_RESULT_INVALID_ARGUMENT for a null table or missing entries, and
// VGL_RESULT_VERSION_MISMATCH for an unsupported version.
[[nodiscard]] vgl_result validate_allocator_interface(const vgl_allocator_interface* host) noexcept;

} // namespace vgl::internal

#endif // VGL_FOUNDATION_ALLOCATOR_HPP
