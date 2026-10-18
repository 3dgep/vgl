// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// How the C++ samples read packs: files on the thread pool and, when vgl is built with
// VGL_DEFAULTS_HTTP, packs on a web server too, so that --pack takes http:// and https:// URIs.
// With --cache, a disk cache keeps what comes from web servers, for next time and offline, within
// --cache-budget mebibytes (1 GiB by default).
// In a page (VGL_SAMPLE_PAGE, an Emscripten build for a browser), every pack is fetched.

#ifndef VGL_SAMPLES_PACK_READER_HPP
#define VGL_SAMPLES_PACK_READER_HPP

#include "overlay.hpp"
#include "sample_data.hpp"

#include <vgl/defaults.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sample
{

// Reads --cache-budget's value, in mebibytes, as bytes. False unless it is a whole number from 1.
[[nodiscard]] inline bool parse_cache_budget(const char* text, std::uint64_t& out_bytes) noexcept
{
    std::uint64_t mib = 0;
    if (text == nullptr || *text == '\0')
    {
        return false;
    }
    for (const char* p = text; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9' || mib > (UINT64_MAX >> 20) / 10)
        {
            return false;
        }
        mib = mib * 10 + static_cast<std::uint64_t>(*p - '0');
    }
    if (mib == 0 || mib > (UINT64_MAX >> 20))
    {
        return false;
    }
    out_bytes = mib << 20;
    return true;
}

class pack_reader
{
public:
    // Fails only if the file reader cannot be created; without the HTTP reader, HTTP packs fail
    // to load. A cache directory puts a disk cache in front of the HTTP reader, within
    // cache_budget bytes, or the cache's default for 0.
    [[nodiscard]] static vgl::expected<pack_reader> create(vgl::thread_pool& pool,
                                                           const std::string& cache = {},
                                                           std::uint64_t cache_budget = 0) noexcept
    {
        pack_reader reader;
        vgl::expected<vgl::file_reader> files = vgl::file_reader::create(pool);
        if (!files)
        {
            return vgl::unexpected(files.error());
        }
        reader.files_ = std::move(*files);
#if defined(VGL_SAMPLE_PAGE)
        // In a page every pack is on a web server, and a relative URI is fetched from the page's
        // own address. Programs for Node.js read files as native ones do.
        vgl::expected<vgl::http_reader> http = vgl::http_reader::create();
#else
        vgl::expected<vgl::http_reader> http = vgl::http_reader::create(reader.files_);
#endif
        if (http)
        {
            reader.http_ = std::move(*http);
            if (!cache.empty())
            {
                vgl::expected<vgl::disk_cache> c = vgl::disk_cache::create(
                    cache.c_str(), reader.http_.io_interface_get(), cache_budget, &pool);
                if (c)
                {
                    reader.cache_ = std::move(*c);
                }
                else
                {
                    std::fprintf(stderr,
                                 "--cache %s: could not use it (%s)\n",
                                 cache.c_str(),
                                 vgl::result_name_get(c.error()));
                }
            }
        }
        return reader;
    }

    [[nodiscard]] vgl_io_interface io_interface_get() const noexcept
    {
        if (cache_)
        {
            return cache_.io_interface_get();
        }
        return http_ ? http_.io_interface_get() : files_.io_interface_get();
    }

    // The disk cache, if there is one.
    [[nodiscard]] const vgl::disk_cache& cache() const noexcept { return cache_; }

    // The cache's statistics for the panel; not enabled without a cache.
    [[nodiscard]] disk_cache_info cache_info() const noexcept
    {
        disk_cache_info info;
        if (cache_)
        {
            const vgl_disk_cache_statistics s = cache_.statistics_get();
            info = {
                true, s.hits, s.misses, s.offline_hits, s.entries, s.bytes, s.budget, s.evictions};
        }
        return info;
    }

    // Destroys the readers; the contexts that read through them must be gone.
    void reset() noexcept
    {
        cache_.reset();
        http_.reset();
        files_.reset();
    }

    // Says which packs cannot be read because vgl has no HTTP reader.
    void warn_unreadable(const std::vector<pack_source>& sources) const
    {
        if (http_)
        {
            return;
        }
        for (const pack_source& s : sources)
        {
            if (s.uri.starts_with("http://") || s.uri.starts_with("https://"))
            {
                std::fprintf(stderr,
                             "%s: vgl was built without VGL_DEFAULTS_HTTP, so it cannot read "
                             "packs from a web server\n",
                             s.uri.c_str());
            }
        }
    }

private:
    // In the order they are made, so that each goes before the one it reads through.
    vgl::file_reader files_;
    vgl::http_reader http_;
    vgl::disk_cache cache_;
};

} // namespace sample

#endif // VGL_SAMPLES_PACK_READER_HPP
