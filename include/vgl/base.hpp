// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/base.h: result codes, expected, versioned structure initialization and
// version. See docs/c-api.md, section 16.

#ifndef VGL_BASE_HPP
#define VGL_BASE_HPP

#include <vgl/base.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#    include <expected>
#endif

namespace vgl
{

enum class result : std::int32_t
{
    success = VGL_RESULT_SUCCESS,
    incomplete = VGL_RESULT_INCOMPLETE,
    not_ready = VGL_RESULT_NOT_READY,
    invalid_argument = VGL_RESULT_INVALID_ARGUMENT,
    out_of_memory = VGL_RESULT_OUT_OF_MEMORY,
    not_supported = VGL_RESULT_NOT_SUPPORTED,
    invalid_operation = VGL_RESULT_INVALID_OPERATION,
    not_found = VGL_RESULT_NOT_FOUND,
    io_error = VGL_RESULT_IO_ERROR,
    corrupt_data = VGL_RESULT_CORRUPT_DATA,
    version_mismatch = VGL_RESULT_VERSION_MISMATCH,
    cancelled = VGL_RESULT_CANCELLED,
    backend_error = VGL_RESULT_BACKEND_ERROR,
    internal_error = VGL_RESULT_INTERNAL_ERROR,
};

[[nodiscard]] constexpr bool succeeded(result r) noexcept
{
    return static_cast<std::int32_t>(r) >= 0;
}

[[nodiscard]] constexpr bool failed(result r) noexcept
{
    return static_cast<std::int32_t>(r) < 0;
}

// Returns the name of a result code, such as "VGL_RESULT_OUT_OF_MEMORY", or
// "VGL_RESULT_UNKNOWN" for an unknown code.
[[nodiscard]] inline const char* result_name_get(result r) noexcept
{
    const char* name = nullptr;
    static_cast<void>(vgl_result_name_get(static_cast<vgl_result>(r), &name));
    return name;
}

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L

template <typename T>
using expected = std::expected<T, result>;

using unexpected = std::unexpected<result>;

#else

// Error wrapper for constructing a failed expected, mirroring std::unexpected.
class unexpected
{
public:
    constexpr explicit unexpected(result error) noexcept : error_(error) {}

    [[nodiscard]] constexpr result error() const noexcept { return error_; }

private:
    result error_;
};

// Minimal subset of std::expected<T, vgl::result>, for standard libraries without
// <expected>. T must be default constructible. It never throws: calling value() on a
// failed expected is undefined behaviour, as with operator* on std::expected.
template <typename T>
class expected
{
    static_assert(std::is_default_constructible_v<T>,
                  "vgl::expected requires a default-constructible type");

public:
    constexpr expected(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value))
    {
    }

    constexpr expected(unexpected error) noexcept : error_(error.error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return succeeded(error_); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr T& value() & noexcept { return value_; }
    [[nodiscard]] constexpr const T& value() const& noexcept { return value_; }
    [[nodiscard]] constexpr T&& value() && noexcept { return std::move(value_); }

    [[nodiscard]] constexpr T& operator*() & noexcept { return value_; }
    [[nodiscard]] constexpr const T& operator*() const& noexcept { return value_; }
    [[nodiscard]] constexpr T* operator->() noexcept { return &value_; }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &value_; }

    [[nodiscard]] constexpr result error() const noexcept { return error_; }

private:
    T value_{};
    result error_ = result::success;
};

#endif

// Returns a zero-initialized versioned structure with struct_size set
// (docs/c-api.md, section 8.1).
template <typename T>
[[nodiscard]] constexpr T init() noexcept
{
    T value{};
    value.struct_size = static_cast<std::uint32_t>(sizeof(T));
    return value;
}

struct version
{
    std::uint32_t major_version = 0;
    std::uint32_t minor_version = 0;
    std::uint32_t patch_version = 0;
};

// Returns the version of the vgl binary.
[[nodiscard]] inline version version_get() noexcept
{
    version v;
    static_cast<void>(vgl_version_get(&v.major_version, &v.minor_version, &v.patch_version));
    return v;
}

} // namespace vgl

#endif // VGL_BASE_HPP
