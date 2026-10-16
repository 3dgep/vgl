// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Allocation and input checks for the libraries built on the C API: the defaults module and
// the backends. They link vgl only through the C API, so they cannot use the core's internal
// helpers, which a shared vgl does not export.

#ifndef VGL_COMMON_HOST_MEMORY_HPP
#define VGL_COMMON_HOST_MEMORY_HPP

#include <vgl/foundation.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#if defined(_WIN32)
#    include <malloc.h>
#endif

namespace vgl::common
{

class memory
{
public:
    memory() noexcept = default;
    explicit memory(const vgl_allocator_interface& host) noexcept : host_(host), has_host_(true) {}

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) const noexcept
    {
        if (has_host_)
        {
            return host_.allocate(host_.user_data, size, alignment);
        }
        alignment = alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment;
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        void* p = nullptr;
        return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
    }

    void deallocate(void* p, std::size_t size, std::size_t alignment) const noexcept
    {
        if (p == nullptr)
        {
            return;
        }
        if (has_host_)
        {
            host_.deallocate(host_.user_data, p, size, alignment);
            return;
        }
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p); // NOLINT(cppcoreguidelines-no-malloc): memory from posix_memalign
#endif
    }

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) const noexcept
    {
        void* p = allocate(sizeof(T), alignof(T));
        return p == nullptr ? nullptr : ::new (p) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void destroy(T* p) const noexcept
    {
        if (p != nullptr)
        {
            p->~T();
            deallocate(p, sizeof(T), alignof(T));
        }
    }

private:
    vgl_allocator_interface host_{};
    bool has_host_ = false;
};

// Checks an optional allocator interface and makes the memory object for it.
[[nodiscard]] inline vgl_result make_memory(const vgl_allocator_interface* host,
                                            memory& out) noexcept
{
    if (host == nullptr)
    {
        out = memory();
        return VGL_RESULT_SUCCESS;
    }
    if (host->version == 0 || host->version > VGL_ALLOCATOR_INTERFACE_VERSION)
    {
        return VGL_RESULT_VERSION_MISMATCH;
    }
    if (host->allocate == nullptr || host->deallocate == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    out = memory(*host);
    return VGL_RESULT_SUCCESS;
}

// Copies a versioned input structure (docs/c-api.md, section 8.3). in may be null, which
// leaves out zero-initialized.
template <typename T>
[[nodiscard]] vgl_result read_versioned(const T* in, std::size_t first_size, T& out) noexcept
{
    out = T{};
    if (in == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    const std::size_t size = in->struct_size;
    if (size < first_size)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (size > sizeof(T))
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(in);
        for (std::size_t i = sizeof(T); i < size; ++i)
        {
            if (bytes[i] != 0)
            {
                return VGL_RESULT_NOT_SUPPORTED;
            }
        }
    }
    std::memcpy(&out, in, size < sizeof(T) ? size : sizeof(T));
    return VGL_RESULT_SUCCESS;
}

} // namespace vgl::common

#endif // VGL_COMMON_HOST_MEMORY_HPP
