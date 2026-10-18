// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Checks of the versioned structures and interfaces that public functions take
// (docs/c-api.md, sections 8.3 and 9).

#ifndef VGL_FOUNDATION_VERSIONED_HPP
#define VGL_FOUNDATION_VERSIONED_HPP

#include <vgl/base.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vgl::internal
{

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
        // Fields of a newer version: accepted only if they are all zero.
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

template <typename Interface>
[[nodiscard]] vgl_result check_interface_version(const Interface& i, std::uint32_t current) noexcept
{
    return i.version == 0 || i.version > current ? VGL_RESULT_VERSION_MISMATCH : VGL_RESULT_SUCCESS;
}

} // namespace vgl::internal

#endif // VGL_FOUNDATION_VERSIONED_HPP
