// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "foundation/allocator.hpp"

#include <cstdlib>

#if defined(_WIN32)
#    include <malloc.h>
#endif

namespace vgl::internal
{

namespace
{

bool is_power_of_two(std::size_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

void* heap_allocate(void* /*user_data*/, std::size_t size, std::size_t alignment)
{
    alignment = alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment;
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* memory = nullptr;
    return posix_memalign(&memory, alignment, size) == 0 ? memory : nullptr;
#endif
}

void heap_deallocate(void* /*user_data*/,
                     void* memory,
                     std::size_t /*size*/,
                     std::size_t /*alignment*/)
{
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc): memory from posix_memalign
#endif
}

} // namespace

allocator::allocator() noexcept
    : interface_{VGL_ALLOCATOR_INTERFACE_VERSION, 0, nullptr, heap_allocate, heap_deallocate}
{
}

allocator::allocator(const vgl_allocator_interface& host) noexcept : interface_(host) {}

void* allocator::allocate(std::size_t size, std::size_t alignment) const noexcept
{
    if (size == 0 || !is_power_of_two(alignment))
    {
        return nullptr;
    }
    return interface_.allocate(interface_.user_data, size, alignment);
}

void allocator::deallocate(void* memory, std::size_t size, std::size_t alignment) const noexcept
{
    if (memory != nullptr)
    {
        interface_.deallocate(interface_.user_data, memory, size, alignment);
    }
}

vgl_result validate_allocator_interface(const vgl_allocator_interface* host) noexcept
{
    if (host == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (host->version == 0 || host->version > VGL_ALLOCATOR_INTERFACE_VERSION)
    {
        return VGL_RESULT_VERSION_MISMATCH;
    }
    if (host->allocate == nullptr || host->deallocate == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    return VGL_RESULT_SUCCESS;
}

} // namespace vgl::internal
