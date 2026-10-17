// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "sample_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace sample
{

namespace
{

int failure_count = 0;

} // namespace

float contour_interval_for(double altitude)
{
    const double target = std::clamp(altitude / 250.0, 1.0, 2000.0);
    const double power = std::pow(10.0, std::floor(std::log10(target)));
    double step = power;
    for (const double m : {2.0, 5.0})
    {
        if (m * power <= target)
        {
            step = m * power;
        }
    }
    return static_cast<float>(step);
}

const char* depth_name(vgl_depth_convention depth)
{
    switch (depth)
    {
    case VGL_DEPTH_CONVENTION_REVERSED_Z:
        return "reversed-Z";
    case VGL_DEPTH_CONVENTION_CONVENTIONAL:
        return "conventional";
    default:
        return "logarithmic";
    }
}

bool idle(const vgl_context_statistics& s)
{
    return s.packs_loading == 0 && s.reads_outstanding == 0 && s.tiles_decoding == 0 &&
           s.meshes_building == 0 && s.meshes_waiting == 0 && s.textures_waiting == 0 &&
           s.resources_created == 0 && s.tiles_drawn == s.tiles_selected;
}

void save_image(const image& i, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << i.width << ' ' << i.height << "\n255\n";
    for (std::size_t p = 0; p < i.pixels.size(); p += 4)
    {
        f.write(reinterpret_cast<const char*>(&i.pixels[p]), 3);
    }
    if (!f)
    {
        std::fprintf(stderr, "could not write %s\n", path.c_str());
    }
}

std::string image_path(const std::string& directory, vgl_depth_convention depth, const char* name)
{
    std::string file = std::string(depth_name(depth)) + "-" + name;
    std::replace(file.begin(), file.end(), ' ', '-');
    std::replace(file.begin(), file.end(), ',', '_');
    return directory + "/" + file + ".ppm";
}

bool covered_pixel(const image& i, int x, int y)
{
    const std::size_t p = (static_cast<std::size_t>(y) * static_cast<std::size_t>(i.width) +
                           static_cast<std::size_t>(x)) *
                          4;
    for (std::size_t c = 0; c < 3; ++c)
    {
        const int background = static_cast<int>(std::lround(clear_color[c] * 255.0f));
        if (std::abs(i.pixels[p + c] - background) > 2)
        {
            return true;
        }
    }
    return false;
}

double coverage(const image& i)
{
    std::size_t n = 0;
    for (int y = 0; y < i.height; ++y)
    {
        for (int x = 0; x < i.width; ++x)
        {
            n += covered_pixel(i, x, y) ? 1u : 0u;
        }
    }
    return static_cast<double>(n) / (static_cast<double>(i.width) * i.height);
}

double difference(const image& a, const image& b)
{
    std::size_t differing = 0;
    for (std::size_t p = 0; p < a.pixels.size(); p += 4)
    {
        const int d = std::max({std::abs(a.pixels[p] - b.pixels[p]),
                                std::abs(a.pixels[p + 1] - b.pixels[p + 1]),
                                std::abs(a.pixels[p + 2] - b.pixels[p + 2])});
        if (d > 24)
        {
            ++differing;
        }
    }
    return static_cast<double>(differing) / static_cast<double>(a.pixels.size() / 4);
}

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "check failed: %s\n", what.c_str());
        ++failure_count;
    }
}

int failures()
{
    return failure_count;
}

void check_place(vgl_depth_convention depth,
                 const place& p,
                 int frames,
                 bool settled,
                 const vgl_context_statistics& s,
                 const image& pixels)
{
    const double covered = coverage(pixels);
    std::printf("%-13s %-20s %5d frames %5u tiles %8llu triangles %6.1f MiB  %5.1f%% globe\n",
                depth_name(depth),
                p.name,
                frames,
                s.tiles_drawn,
                static_cast<unsigned long long>(s.triangles_drawn),
                static_cast<double>(s.gpu_used) / (1024.0 * 1024.0),
                covered * 100.0);
    const std::string where = std::string(depth_name(depth)) + ", " + p.name;
    expect(settled, "loading settles: " + where);
    expect(s.tiles_drawn > 0 && s.tiles_drawn == s.tiles_selected,
           "every selected tile is drawn: " + where);
    // From orbit, tiles a pixel or two per sample apart draw coarser grids.
    expect(std::strcmp(p.name, "orbit, Atlantic") != 0 || s.triangles_drawn < 20000,
           "the globe from orbit draws coarse meshes: " + where);
    expect(covered >= p.least_coverage && covered <= p.most_coverage,
           "the globe covers its share of the view: " + where);
    // Every view looks at the globe in the middle, and past it in the top corners, except
    // straight down from close by.
    const int w = pixels.width;
    const int h = pixels.height;
    const bool sky = p.most_coverage < 1.0;
    expect(covered_pixel(pixels, w / 2, h / 2) &&
               (!sky || (!covered_pixel(pixels, 0, 0) && !covered_pixel(pixels, w - 1, 0))),
           "the globe is below and the sky above: " + where);
    // Imagery is drawn: the desert is tan, where the colours by height would be green.
    if (std::strcmp(p.name, "Sahara") == 0)
    {
        const std::uint8_t* middle =
            &pixels.pixels[(static_cast<std::size_t>(h / 2) * static_cast<std::size_t>(w) +
                            static_cast<std::size_t>(w / 2)) *
                           4];
        expect(middle[0] > middle[1] && middle[1] > middle[2],
               "the Sahara is sand-coloured: " + where);
    }
}

void compare_conventions(const std::vector<vgl_depth_convention>& conventions,
                         const std::vector<std::vector<image>>& images)
{
    for (std::size_t c = 1; c < conventions.size(); ++c)
    {
        if (images[0].size() != places.size() || images[c].size() != places.size())
        {
            continue;
        }
        for (std::size_t i = 0; i < places.size(); ++i)
        {
            const double d = difference(images[0][i], images[c][i]);
            const double bound = conventions[c] == VGL_DEPTH_CONVENTION_CONVENTIONAL ? 0.05 : 0.01;
            std::printf("%s and %s differ in %.2f%% of %s\n",
                        depth_name(conventions[0]),
                        depth_name(conventions[c]),
                        d * 100.0,
                        places[i].name);
            expect(d <= bound,
                   std::string(depth_name(conventions[c])) + " agrees with " +
                       depth_name(conventions[0]) + " depth: " + places[i].name);
        }
    }
}

} // namespace sample
