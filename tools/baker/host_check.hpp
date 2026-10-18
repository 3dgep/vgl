// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The check command: whether a web server serves a pack as vgl and browsers read it
// (docs/hosting.md). Built with libcurl, when the defaults module's HTTP reader is.

#ifndef VGL_BAKER_HOST_CHECK_HPP
#define VGL_BAKER_HOST_CHECK_HPP

#include <cstdint>
#include <ostream>
#include <string>

namespace vgl::bake
{

struct host_check_options
{
    std::uint32_t sample = 0; // Shards to check, spread over the pack; 0 for every one.
    double timeout = 30.0;    // Seconds per request.
};

struct host_check_report
{
    std::uint32_t errors = 0;   // What stops vgl from reading the pack, or makes it slow.
    std::uint32_t warnings = 0; // What stops browsers, or caches, from doing their part.

    [[nodiscard]] bool ok() const noexcept { return errors == 0; }
};

// Whether the baker was built with libcurl, which the check needs.
[[nodiscard]] bool host_check_available() noexcept;

// Checks the pack at url, a directory URL (with or without a trailing "/pack.vglm"), and
// reports each check to out. Throws std::runtime_error if the check cannot run at all.
[[nodiscard]] host_check_report
check_host(const std::string& url, const host_check_options& options, std::ostream& out);

} // namespace vgl::bake

#endif // VGL_BAKER_HOST_CHECK_HPP
