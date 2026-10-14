// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Routes log messages to the host's log callback (docs/c-api.md, section 13). Without a
// callback, the library is silent.

#ifndef VGL_FOUNDATION_LOG_HPP
#define VGL_FOUNDATION_LOG_HPP

#include <vgl/foundation.h>

namespace vgl::internal
{

class logger
{
public:
    // Longest message passed to the callback, in bytes. Longer messages are truncated.
    static constexpr int max_message_size = 1024;

    // A silent logger.
    logger() noexcept = default;

    // min_level VGL_LOG_LEVEL_DEFAULT selects VGL_LOG_LEVEL_WARNING.
    logger(vgl_log_fn callback, void* user_data, vgl_log_level min_level) noexcept;

    // True if a message at this level reaches the callback.
    [[nodiscard]] bool enabled(vgl_log_level level) const noexcept;

    // Formats with printf rules and passes the message to the callback. See log.cpp for why
    // this is a C-style variadic function.
    void log(vgl_log_level level, const char* format, ...) const noexcept // NOLINT(cert-dcl50-cpp)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;

private:
    vgl_log_fn callback_ = nullptr;
    void* user_data_ = nullptr;
    vgl_log_level min_level_ = VGL_LOG_LEVEL_WARNING;
};

} // namespace vgl::internal

#endif // VGL_FOUNDATION_LOG_HPP
