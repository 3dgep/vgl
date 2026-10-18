// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of vgl-bake check (tools/baker/host_check.hpp): test packs served by the small HTTP
// server of http_server.hpp, as they should be and as they should not.

#include "host_check.hpp"
#include "http_server.hpp"
#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <cstddef>
#include <sstream>
#include <string>

namespace
{

using namespace vgl::test;

struct checked
{
    vgl::bake::host_check_report report;
    std::string text;
};

checked check(const std::string& url, std::uint32_t sample = 0)
{
    std::ostringstream out;
    vgl::bake::host_check_options options;
    options.sample = sample;
    options.timeout = 10.0;
    checked c;
    c.report = vgl::bake::check_host(url, options, out);
    c.text = out.str();
    return c;
}

bool says(const checked& c, const std::string& what)
{
    return c.text.find(what) != std::string::npos;
}

// The pack's shard files, by path.
std::vector<std::string> shard_files(const test_pack& p)
{
    std::vector<std::string> files;
    for (const auto& [path, bytes] : p.files)
    {
        if (path.size() > 5 && path.compare(path.size() - 5, 5, ".vgls") == 0)
        {
            files.push_back(path);
        }
    }
    return files;
}

} // namespace

TEST_CASE(host_check_passes_a_pack_served_well)
{
    REQUIRE(vgl::bake::host_check_available());
    http_server server(server_mode::ranges);
    const test_pack pack = make_pack();
    const std::string uri = server.add(pack, "dem");
    const checked c = check(uri);
    CHECK_EQ(c.report.errors, 0u);
    CHECK(says(c, "byte ranges: 206"));
    CHECK(says(c,
               "shards: " + std::to_string(shard_files(pack).size()) + " of " +
                   std::to_string(shard_files(pack).size()) + " checked"));
    CHECK(says(c, "header, directory and a tile check out"));
    // The test server sends no CORS or caching headers, which browsers and caches want.
    CHECK(says(c, "no Access-Control-Allow-Origin"));
    CHECK(says(c, "not marked for long caching"));
    CHECK(c.report.warnings > 0);
    // The manifest's own URL works too, and a sample checks fewer shards.
    const checked sampled = check(uri + "/pack.vglm", 2);
    CHECK_EQ(sampled.report.errors, 0u);
    CHECK(says(sampled, "shards: 2 of 2 checked (of "));
}

TEST_CASE(host_check_finds_what_vgl_cannot_read)
{
    const test_pack pack = make_pack();
    const std::vector<std::string> shards = shard_files(pack);
    REQUIRE(shards.size() >= 2);
    {
        // A server that ignores ranges.
        http_server server(server_mode::whole_files);
        const checked c = check(server.add(pack, "dem"));
        CHECK(c.report.errors > 0);
        CHECK(says(c, "ignores range requests"));
    }
    {
        // No pack at the URL.
        http_server server(server_mode::ranges);
        const checked c = check(server.add(pack, "dem") + "-missing");
        CHECK_EQ(c.report.errors, 1u);
        CHECK(says(c, "answers 404"));
    }
    {
        // A shard missing, and another from a different bake: the same name, other bytes.
        test_pack broken = pack;
        broken.files.erase(shards[0]);
        std::vector<std::byte>& other = broken.files[shards[1]];
        other[20] = static_cast<std::byte>(static_cast<unsigned>(other[20]) ^ 0xFFu);
        http_server server(server_mode::ranges);
        const checked c = check(server.add(broken, "dem"));
        CHECK_EQ(c.report.errors, 2u);
        CHECK(says(c, shards[0] + ": missing (404)"));
        CHECK(says(c, shards[1] + ": its header does not match the manifest"));
        CHECK(says(c, "shards: " + std::to_string(shards.size() - 2) + " of"));
    }
    {
        // Not a manifest.
        test_pack broken = pack;
        broken.files["pack.vglm"][0] = std::byte{'X'};
        http_server server(server_mode::ranges);
        const checked c = check(server.add(broken, "dem"));
        CHECK_EQ(c.report.errors, 1u);
        CHECK(says(c, "is not a valid manifest"));
    }
}
