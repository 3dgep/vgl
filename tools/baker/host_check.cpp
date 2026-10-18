// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The check command (host_check.hpp). It asks the server what vgl and browsers ask: the
// manifest, byte ranges of it and of every shard, a preflight as a browser on another site
// sends, and one shard's directory and tile, all checked with the runtime's readers. Requests
// share one libcurl handle, so they reuse the connection.

#include "host_check.hpp"

#include "foundation/allocator.hpp"
#include "providers/pack_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(VGL_BAKE_HAS_CURL)
#    if defined(_WIN32) && !defined(NOMINMAX)
#        define NOMINMAX // curl.h brings windows.h, whose min and max macros hide std's.
#    endif
#    include <curl/curl.h>
#endif

namespace vgl::bake
{

#if !defined(VGL_BAKE_HAS_CURL)

bool host_check_available() noexcept
{
    return false;
}

host_check_report
check_host(const std::string& /*url*/, const host_check_options& /*options*/, std::ostream& /*out*/)
{
    throw std::runtime_error("this vgl-bake was built without libcurl; build it with "
                             "VGL_DEFAULTS_HTTP on to check hosted packs");
}

#else

namespace
{

namespace pack = vgl::internal::pack;

// The origin of a page on another site, as a browser sends it.
constexpr const char* other_origin = "https://example.org";
// A manifest cached longer than this delays rebakes.
constexpr long manifest_max_age = 3600;
// Shard failures listed one by one before the rest are only counted.
constexpr std::uint32_t listed_failures = 10;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

struct response
{
    long status = 0;
    std::map<std::string, std::string> headers; // Names in lower case; the final response's.
    std::vector<std::byte> body;
    bool truncated = false; // The body went past the limit and was cut off.
    std::string error;      // libcurl's, if the request failed.

    [[nodiscard]] std::string header(const std::string& name) const
    {
        const auto i = headers.find(name);
        return i == headers.end() ? std::string() : i->second;
    }
};

class http_client
{
public:
    explicit http_client(double timeout) : timeout_ms_(static_cast<long>(timeout * 1000.0))
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error("libcurl could not be set up");
        }
        handle_ = curl_easy_init();
        if (handle_ == nullptr)
        {
            curl_global_cleanup();
            throw std::runtime_error("libcurl could not be set up");
        }
    }

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;
    http_client(http_client&&) = delete;
    http_client& operator=(http_client&&) = delete;

    ~http_client()
    {
        curl_easy_cleanup(handle_);
        curl_global_cleanup();
    }

    // A request: GET, HEAD or OPTIONS, with extra headers. The body is kept up to limit bytes.
    response request(const std::string& url,
                     const std::vector<std::string>& headers,
                     const char* method = "GET",
                     std::size_t limit = SIZE_MAX)
    {
        response r;
        state s{&r, limit};
        curl_easy_reset(handle_);
        curl_slist* list = nullptr;
        for (const std::string& h : headers)
        {
            list = curl_slist_append(list, h.c_str());
        }
        CURL* e = handle_;
        curl_easy_setopt(e, CURLOPT_URL, url.c_str());
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(e, CURLOPT_USERAGENT, "vgl-bake check");
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &write);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &header);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, &s);
        if (std::strcmp(method, "HEAD") == 0)
        {
            curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
        }
        else if (std::strcmp(method, "GET") != 0)
        {
            curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, method);
        }
        const CURLcode code = curl_easy_perform(e);
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &r.status);
        // A write error after enough of the body is the limit stopping it, not a failure.
        if (code != CURLE_OK && (code != CURLE_WRITE_ERROR || !r.truncated))
        {
            r.error = curl_easy_strerror(code);
        }
        curl_slist_free_all(list);
        return r;
    }

private:
    struct state
    {
        response* r;
        std::size_t limit;
    };

    // NOLINTNEXTLINE(readability-non-const-parameter): libcurl's callback type.
    static std::size_t write(char* data, std::size_t one, std::size_t count, void* user)
    {
        auto* s = static_cast<state*>(user);
        const std::size_t n = one * count;
        const std::size_t room = s->limit - std::min(s->limit, s->r->body.size());
        const std::size_t keep = std::min(n, room);
        const auto* bytes = reinterpret_cast<const std::byte*>(data);
        s->r->body.insert(s->r->body.end(), bytes, bytes + keep);
        if (keep < n)
        {
            s->r->truncated = true;
            return 0; // Enough: stop the transfer.
        }
        return n;
    }

    static std::size_t header(char* data, std::size_t one, std::size_t count, void* user)
    {
        auto* s = static_cast<state*>(user);
        const std::size_t n = one * count;
        std::string line(data, n);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }
        if (line.starts_with("HTTP/"))
        {
            s->r->headers.clear(); // A new response, after a redirect.
            return n;
        }
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string value = line.substr(colon + 1);
            const std::size_t first = value.find_first_not_of(" \t");
            value = first == std::string::npos ? std::string() : value.substr(first);
            s->r->headers[lower(line.substr(0, colon))] = value;
        }
        return n;
    }

    CURL* handle_ = nullptr;
    long timeout_ms_ = 30000;
};

// The report, as lines starting with what each check found.
class reporter
{
public:
    reporter(std::ostream& out, host_check_report& report) : out_(&out), report_(&report) {}

    void ok(const std::string& what) const { *out_ << "ok       " << what << '\n'; }

    void warning(const std::string& what) const
    {
        ++report_->warnings;
        *out_ << "warning  " << what << '\n';
    }

    void error(const std::string& what) const
    {
        ++report_->errors;
        *out_ << "error    " << what << '\n';
    }

    void info(const std::string& what) const { *out_ << "         " << what << '\n'; }

private:
    std::ostream* out_;
    host_check_report* report_;
};

std::string range_header(std::uint64_t first, std::uint64_t last)
{
    return "Range: bytes=" + std::to_string(first) + "-" + std::to_string(last);
}

// The total of a Content-Range header ("bytes a-b/total"), or -1.
long long content_range_total(const std::string& value)
{
    const std::size_t slash = value.rfind('/');
    if (slash == std::string::npos || slash + 1 >= value.size() || value[slash + 1] == '*')
    {
        return -1;
    }
    return std::strtoll(value.c_str() + slash + 1, nullptr, 10);
}

// A compressed response is a whole body, whose ranges are not ranges of the file.
bool compressed(const response& r)
{
    const std::string e = lower(r.header("content-encoding"));
    return !e.empty() && e != "identity";
}

// The max-age of a Cache-Control header, or -1 without one.
long max_age(const std::string& cache_control)
{
    const std::string c = lower(cache_control);
    const std::size_t at = c.find("max-age=");
    return at == std::string::npos ? -1 : std::strtol(c.c_str() + at + 8, nullptr, 10);
}

std::span<const std::byte> view(const std::vector<std::byte>& v)
{
    return {v.data(), v.size()};
}

} // namespace

bool host_check_available() noexcept
{
    return true;
}

host_check_report
check_host(const std::string& url_in, const host_check_options& options, std::ostream& out)
{
    host_check_report report;
    const reporter say(out, report);

    // The pack's directory URL.
    std::string base = url_in;
    if (base.size() > 10 && base.compare(base.size() - 10, 10, "/pack.vglm") == 0)
    {
        base.resize(base.size() - 10);
    }
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }
    const std::string manifest_url = base + "/" + std::string(pack::manifest_file_name);
    http_client http(options.timeout);
    const std::string origin = std::string("Origin: ") + other_origin;
    // Browsers ask for compression; a server that compresses pack files breaks their ranges.
    const std::string encodings = "Accept-Encoding: gzip, deflate, br, zstd";

    // The manifest, whole.
    const response m = http.request(manifest_url, {origin, encodings});
    if (!m.error.empty() || m.status != 200)
    {
        say.error(manifest_url + ": " +
                  (m.error.empty() ? "the server answers " + std::to_string(m.status)
                                   : "the request failed: " + m.error) +
                  ", not 200 with the manifest");
        return report;
    }
    if (compressed(m))
    {
        say.error(
            "the manifest is sent compressed (Content-Encoding: " + m.header("content-encoding") +
            "), which breaks byte ranges: turn compression off for pack files");
        return report;
    }
    const vgl::internal::allocator memory;
    vgl::internal::zstd_decompressor decompressor(memory);
    pack::manifest manifest;
    const vgl_result parsed = pack::manifest::parse(view(m.body), memory, decompressor, manifest);
    if (parsed != VGL_RESULT_SUCCESS)
    {
        const char* name = nullptr;
        static_cast<void>(vgl_result_name_get(parsed, &name));
        say.error(manifest_url + " is not a valid manifest (" + name + ")");
        return report;
    }
    const pack::layer_description& layer = manifest.layer();
    const char* kind = "geoid";
    if (layer.type == pack::layer_type::elevation)
    {
        kind = "elevation";
    }
    else if (layer.type == pack::layer_type::imagery)
    {
        kind = "imagery";
    }
    const std::uint32_t shard_total = manifest.shard_count() + (manifest.has_base() ? 1 : 0);
    say.ok("manifest: " + std::string(manifest.metadata_value("name")) + " (" + kind + ", levels " +
           std::to_string(layer.min_level) + " to " + std::to_string(layer.max_level) + ", " +
           std::to_string(shard_total) + " shards)");

    // Headers browsers and caches need.
    if (m.header("access-control-allow-origin").empty())
    {
        say.warning("no Access-Control-Allow-Origin for a page on another site: browsers there "
                    "cannot read the pack");
    }
    else
    {
        say.ok("CORS: Access-Control-Allow-Origin: " + m.header("access-control-allow-origin"));
    }
    const std::string manifest_cache = m.header("cache-control");
    if (lower(manifest_cache).find("immutable") != std::string::npos ||
        max_age(manifest_cache) > manifest_max_age)
    {
        say.warning("the manifest may be cached for long (Cache-Control: " + manifest_cache +
                    "): a rebake reaches readers only when it runs out");
    }

    // Ranges.
    const std::uint64_t size = m.body.size();
    const std::uint64_t probe = std::min<std::uint64_t>(32, size);
    const response r = http.request(manifest_url, {origin, encodings, range_header(0, probe - 1)});
    if (r.status == 206 && compressed(r))
    {
        say.error("ranges are sent compressed (Content-Encoding: " + r.header("content-encoding") +
                  "): turn compression off for pack files");
    }
    else if (r.status == 206 && r.body.size() == probe &&
             std::equal(r.body.begin(), r.body.end(), m.body.begin()) &&
             content_range_total(r.header("content-range")) == static_cast<long long>(size))
    {
        say.ok("byte ranges: 206 Partial Content, Content-Range: " + r.header("content-range"));
    }
    else if (r.status == 200)
    {
        say.error("the server ignores range requests and sends whole files: vgl then downloads a "
                  "whole shard for every tile");
    }
    else
    {
        say.error("a range of the manifest gets " + std::to_string(r.status) +
                  (r.header("content-range").empty()
                       ? ""
                       : ", Content-Range: " + r.header("content-range")) +
                  ", not 206 with those bytes");
    }
    const response past = http.request(manifest_url, {range_header(size, size + 15)});
    if (past.status != 416)
    {
        say.warning("a range past the end of a file gets " + std::to_string(past.status) +
                    ", not 416 Range Not Satisfiable");
    }

    // A browser on another site may ask before sending a Range header (a CORS preflight); a
    // server that does not answer stops those browsers.
    const response pre = http.request(
        manifest_url,
        {origin, "Access-Control-Request-Method: GET", "Access-Control-Request-Headers: range"},
        "OPTIONS");
    const std::string allowed = lower(pre.header("access-control-allow-headers"));
    if (pre.status >= 200 && pre.status < 300 &&
        (allowed.find("range") != std::string::npos || allowed.find('*') != std::string::npos))
    {
        say.ok("CORS preflight for Range: " + std::to_string(pre.status));
    }
    else
    {
        say.warning(
            "a CORS preflight for Range gets " + std::to_string(pre.status) +
            (allowed.empty() ? " without Access-Control-Allow-Headers" : ", allowing " + allowed) +
            ": browsers that ask before sending a Range from another site cannot read "
            "the pack");
    }

    // The shards: each exists, with the size and digest the manifest gives.
    struct shard
    {
        pack::shard_place place;
        std::uint64_t file_size = 0;
        std::uint32_t digest = 0;
    };
    std::vector<shard> shards;
    if (manifest.has_base())
    {
        shards.push_back({{true, 0, 0}, manifest.base_file_size(), manifest.base_digest()});
    }
    for (std::uint32_t i = 0; i < manifest.shard_count(); ++i)
    {
        const pack::shard_entry e = manifest.shard(i);
        shards.push_back({{false, e.x, e.y}, e.file_size, e.digest});
    }
    std::vector<std::size_t> chosen;
    const std::size_t want =
        options.sample == 0 ? shards.size() : std::min<std::size_t>(options.sample, shards.size());
    for (std::size_t k = 0; k < want; ++k)
    {
        chosen.push_back(want == shards.size() ? k : k * shards.size() / want);
    }
    const auto shard_url = [&](const shard& s) {
        std::array<char, pack::max_path_size> path{};
        if (s.place.base)
        {
            static_cast<void>(pack::base_shard_path(s.digest, path));
        }
        else
        {
            static_cast<void>(pack::shard_path(s.place.x, s.place.y, s.digest, path));
        }
        return base + "/" + path.data();
    };
    std::uint32_t failed = 0;
    std::uint32_t good = 0;
    std::string shard_cache;
    bool shard_compressed = false;
    const shard* first_good = nullptr;
    pack::shard_header first_header{};
    for (const std::size_t index : chosen)
    {
        const shard& s = shards[index];
        const std::string u = shard_url(s);
        const response h = http.request(u,
                                        {encodings, range_header(0, pack::shard_header_size - 1)},
                                        "GET",
                                        std::size_t{64} * 1024);
        std::string problem;
        pack::shard_header header{};
        if (!h.error.empty())
        {
            problem = "the request failed: " + h.error;
        }
        else if (h.status == 404 || h.status == 410)
        {
            problem = "missing (" + std::to_string(h.status) + ")";
        }
        else if (h.status != 206)
        {
            problem = "gets " + std::to_string(h.status) + ", not 206";
        }
        else if (compressed(h))
        {
            shard_compressed = true;
            problem = "sent compressed";
        }
        else if (content_range_total(h.header("content-range")) !=
                 static_cast<long long>(s.file_size))
        {
            problem = "is " + std::to_string(content_range_total(h.header("content-range"))) +
                      " bytes, but the manifest says " + std::to_string(s.file_size) +
                      ": a shard from another bake";
        }
        else if (pack::parse_shard_header(view(h.body), manifest, s.place, header) !=
                 VGL_RESULT_SUCCESS)
        {
            problem =
                "its header does not match the manifest: a shard from another bake, or damaged";
        }
        if (problem.empty())
        {
            ++good;
            if (first_good == nullptr)
            {
                first_good = &s;
                first_header = header;
                shard_cache = h.header("cache-control");
            }
            continue;
        }
        if (++failed <= listed_failures)
        {
            std::string line = u;
            line += ": ";
            line += problem;
            say.error(line);
        }
    }
    if (failed > listed_failures)
    {
        say.info("and " + std::to_string(failed - listed_failures) + " more shards");
    }
    if (shard_compressed)
    {
        say.info("turn compression off for pack files: it breaks byte ranges");
    }
    const std::string checked =
        "shards: " + std::to_string(good) + " of " + std::to_string(chosen.size()) + " checked" +
        (chosen.size() < shards.size() ? " (of " + std::to_string(shards.size()) + ")"
                                       : std::string()) +
        " exist with the size and digest the manifest gives";
    if (failed == 0)
    {
        say.ok(checked);
    }
    else
    {
        say.info(checked);
    }
    if (first_good != nullptr && max_age(shard_cache) <= 0 &&
        lower(shard_cache).find("immutable") == std::string::npos)
    {
        say.warning("shards are not marked for long caching (Cache-Control: " +
                    (shard_cache.empty() ? std::string("none") : shard_cache) +
                    "); their names change with their content, so caches may keep them forever");
    }

    // One shard read as vgl reads it: its directory, then a tile.
    if (first_good != nullptr)
    {
        const std::string u = shard_url(*first_good);
        const pack::byte_range dr = pack::directory_range(first_header);
        const response d = http.request(u, {range_header(dr.offset, dr.offset + dr.size - 1)});
        pack::shard_directory directory;
        std::string problem;
        if (d.status != 206 || d.body.size() != dr.size)
        {
            problem = "its directory gets " + std::to_string(d.status) + " with " +
                      std::to_string(d.body.size()) + " bytes";
        }
        else if (pack::shard_directory::parse(
                     view(d.body), first_header, manifest, memory, decompressor, directory) !=
                 VGL_RESULT_SUCCESS)
        {
            problem = "its directory is damaged";
        }
        else if (directory.tile_count() == 0)
        {
            problem = "its directory lists no tiles";
        }
        else
        {
            const pack::directory_entry e = directory.entry(directory.tile_count() / 2);
            const pack::byte_range tr = directory.tile_range(e);
            const response t = http.request(u, {range_header(tr.offset, tr.offset + tr.size - 1)});
            if (t.status != 206 || t.body.size() != tr.size)
            {
                problem = "a tile gets " + std::to_string(t.status) + " with " +
                          std::to_string(t.body.size()) + " bytes";
            }
            else if (pack::check_tile(view(t.body), e) != VGL_RESULT_SUCCESS)
            {
                problem = "a tile does not match its checksum";
            }
        }
        if (problem.empty())
        {
            say.ok("a shard read as vgl reads it: header, directory and a tile check out");
        }
        else
        {
            say.error(u + ": " + problem);
        }
    }
    return report;
}

#endif // VGL_BAKE_HAS_CURL

} // namespace vgl::bake
