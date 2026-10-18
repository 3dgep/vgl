// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// How the C++ samples read packs: files on the thread pool and, when vgl is built with
// VGL_DEFAULTS_HTTP, packs on a web server too, so that --pack takes http:// and https:// URIs.

#ifndef VGL_SAMPLES_PACK_READER_HPP
#define VGL_SAMPLES_PACK_READER_HPP

#include "sample_data.hpp"

#include <vgl/defaults.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sample
{

class pack_reader
{
public:
    // Fails only if the file reader cannot be created; without the HTTP reader, HTTP packs fail
    // to load.
    [[nodiscard]] static vgl::expected<pack_reader> create(vgl::thread_pool& pool) noexcept
    {
        pack_reader reader;
        vgl::expected<vgl::file_reader> files = vgl::file_reader::create(pool);
        if (!files)
        {
            return vgl::unexpected(files.error());
        }
        reader.files_ = std::move(*files);
        vgl::expected<vgl::http_reader> http = vgl::http_reader::create(reader.files_);
        if (http)
        {
            reader.http_ = std::move(*http);
        }
        return reader;
    }

    [[nodiscard]] vgl_io_interface io_interface_get() const noexcept
    {
        return http_ ? http_.io_interface_get() : files_.io_interface_get();
    }

    // Destroys the readers; the contexts that read through them must be gone.
    void reset() noexcept
    {
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
    // Declared first, so that the HTTP reader, which hands it file URIs, goes first.
    vgl::file_reader files_;
    vgl::http_reader http_;
};

} // namespace sample

#endif // VGL_SAMPLES_PACK_READER_HPP
