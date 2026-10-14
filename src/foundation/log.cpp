// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "foundation/log.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>

namespace vgl::internal
{

logger::logger(vgl_log_fn callback, void* user_data, vgl_log_level min_level) noexcept
    : callback_(callback), user_data_(user_data),
      min_level_(min_level == VGL_LOG_LEVEL_DEFAULT ? VGL_LOG_LEVEL_WARNING : min_level)
{
}

bool logger::enabled(vgl_log_level level) const noexcept
{
    return callback_ != nullptr && level != VGL_LOG_LEVEL_DEFAULT && level <= min_level_;
}

// printf-style formatting is deliberate: std::format may allocate and throw, and the library
// does neither (NFR-03, NFR-05). On some platforms va_list is an array type, which also
// triggers the array decay check.
// NOLINTBEGIN(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
void logger::log(vgl_log_level level, const char* format, ...) const noexcept
{
    if (!enabled(level))
    {
        return;
    }

    std::array<char, max_message_size> message{};
    va_list args;
    va_start(args, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): args is initialized by va_start.
    const int written = std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);
    if (written < 0)
    {
        return;
    }

    callback_(user_data_, level, message.data());
}
// NOLINTEND(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

} // namespace vgl::internal
