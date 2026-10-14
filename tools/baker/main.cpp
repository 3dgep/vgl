// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// vgl-bake: command-line front end of the baking tool (design document, section 8.2).

#include "bake_config.hpp"
#include "baker.hpp"
#include "inspect.hpp"
#include "pack_file.hpp"

#include <vgl/version.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr const char* usage = R"(vgl-bake: builds and checks vgl tile packs.

usage:
  vgl-bake bake <config.json> [options]   Bake the pack a configuration describes.
  vgl-bake inspect <pack>                  Show a pack's manifest and per-level sizes.
  vgl-bake validate <pack>                 Read every tile and check seams, poles and
                                           child masks. Exits with 1 if a check fails.
  vgl-bake query <pack>                    Read "latitude longitude" lines (degrees) from
                                           standard input and print heights.
  vgl-bake --version

bake options:
  --jobs <n>        Worker threads (default: all cores).
  --resume          Continue an interrupted bake, keeping finished shards.
  --max-level <l>   Preview: bake only down to level l.
  --cache <MB>      GDAL raster cache size (default: 2048).
  --verbose         Report every shard.
)";

unsigned long parse_number(std::string_view option, const char* value)
{
    char* end = nullptr;
    const unsigned long v = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0')
    {
        throw std::runtime_error(std::string(option) + " needs a number, not \"" + value + "\"");
    }
    return v;
}

int run(const std::vector<std::string_view>& args)
{
    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        std::fputs(usage, args.empty() ? stderr : stdout);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version")
    {
        std::printf("vgl-bake %s\n", VGL_VERSION_STRING);
        return 0;
    }
    if (args.size() < 2)
    {
        std::fputs(usage, stderr);
        return 2;
    }
    const std::string_view command = args[0];
    const std::filesystem::path target(args[1]);

    if (command == "bake")
    {
        vgl::bake::bake_options options;
        for (std::size_t i = 2; i < args.size(); ++i)
        {
            const std::string_view a = args[i];
            const auto value = [&]() -> const char* {
                if (i + 1 >= args.size())
                {
                    throw std::runtime_error(std::string(a) + " needs a value");
                }
                return args[++i].data();
            };
            if (a == "--jobs")
            {
                options.jobs = static_cast<unsigned>(parse_number(a, value()));
            }
            else if (a == "--resume")
            {
                options.resume = true;
            }
            else if (a == "--max-level")
            {
                options.max_level = static_cast<std::uint32_t>(parse_number(a, value()));
            }
            else if (a == "--cache")
            {
                options.gdal_cache_megabytes = parse_number(a, value());
            }
            else if (a == "--verbose")
            {
                options.verbose = true;
            }
            else
            {
                throw std::runtime_error("unknown option " + std::string(a) +
                                         "; see vgl-bake --help");
            }
        }
        const vgl::bake::bake_config config = vgl::bake::load_bake_config(target);
        const vgl::bake::bake_summary s = vgl::bake::bake(config, options);
        std::printf("baked %llu tiles in %llu shards, %.1f MB, in %.1f s\n",
                    static_cast<unsigned long long>(s.tiles),
                    static_cast<unsigned long long>(s.shards),
                    static_cast<double>(s.bytes) / 1e6,
                    s.seconds);
        return 0;
    }
    if (command == "inspect")
    {
        vgl::bake::inspect(target, std::cout);
        return 0;
    }
    if (command == "validate")
    {
        return vgl::bake::validate(target, std::cout).ok() ? 0 : 1;
    }
    if (command == "sample-tile" && args.size() == 5)
    {
        // Diagnostics: prints a tile's heights before quantization, one row per line.
        const vgl::bake::bake_config config = vgl::bake::load_bake_config(target);
        const auto level = static_cast<std::uint32_t>(parse_number("level", args[2].data()));
        const auto x = static_cast<std::uint32_t>(parse_number("x", args[3].data()));
        const auto y = static_cast<std::uint32_t>(parse_number("y", args[4].data()));
        const std::vector<double> h = vgl::bake::compute_tile_heights(config, level, x, y);
        for (std::size_t k = 0; k < h.size(); ++k)
        {
            std::printf("%.17g%c", h[k], (k + 1) % config.tile_size == 0 ? '\n' : ' ');
        }
        return 0;
    }
    if (command == "query")
    {
        // Reads "latitude longitude" lines (degrees) from standard input and prints the
        // height and the level it came from.
        const vgl::bake::pack_file p(target);
        vgl::bake::height_query q(p);
        double latitude = 0.0;
        double longitude = 0.0;
        while (std::cin >> latitude >> longitude)
        {
            const vgl::bake::height_query::result r = q.height(latitude, longitude);
            std::printf("%.9f %.9f %.6f %u\n", latitude, longitude, r.height, r.level);
        }
        return 0;
    }
    std::fprintf(stderr,
                 "vgl-bake: unknown command \"%.*s\"; see vgl-bake --help\n",
                 static_cast<int>(command.size()),
                 command.data());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    try
    {
        return run(args);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "vgl-bake: %s\n", e.what());
        return 1;
    }
}
